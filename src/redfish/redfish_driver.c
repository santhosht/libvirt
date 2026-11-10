/*
 * redfish_driver.c: Redfish composable infrastructure driver
 *
 * Exposes a DMTF Redfish composition service through the libvirt API:
 *
 *   connection          -> Redfish service (session)
 *   domain              -> composed ComputerSystem
 *   virDomainDefineXML  -> specific composition (POST /Systems)
 *   virDomainCreate     -> ComputerSystem.Reset "On"
 *   virDomainUndefine   -> decomposition (DELETE /Systems/{id})
 *   node device         -> ResourceBlock
 *
 * URI: redfish://host[:port]/?transport=http|https&no_verify=1&timeout=SECS
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <config.h>

#include "redfish_driver.h"
#include "redfish_client.h"
#include "redfish_conv.h"

#include "datatypes.h"
#include "domain_event.h"
#include "driver.h"
#include "virauth.h"
#include "virerror.h"
#include "virevent.h"
#include "virlog.h"
#include "virdomainobjlist.h"
#include "virnodedeviceobj.h"
#include "virsecureerase.h"
#include "virstring.h"
#include "viruri.h"
#include "viruuid.h"
#include "virxml.h"

#define VIR_FROM_THIS VIR_FROM_REDFISH

VIR_LOG_INIT("redfish.redfish_driver");

#define REDFISH_ROOT "/redfish/v1"
#define REDFISH_DEFAULT_TIMEOUT 600
#define REDFISH_DEFAULT_POLL 5 /* seconds between event polls */

typedef struct _redfishPrivate redfishPrivate;
struct _redfishPrivate {
    redfishClient *client;
    virCaps *caps;
    virDomainXMLOption *xmlopt;
    char *version;
    char *systemsPath;
    char *blocksPath;
    bool encrypted;

    /* Where composed systems were last seen, to avoid listing the whole
     * collection to find one of them. Only a hint: entries are verified
     * against the service before use. */
    virMutex pathsLock;
    GHashTable *pathByUUID;
    GHashTable *pathByName;

    /* Domain events. Redfish has no way to be told about changes
     * through this driver's connection, so while somebody listens the
     * service is polled and the differences are turned into events. */
    virObjectEventState *eventState;
    virMutex eventLock;     /* protects known */
    virMutex startLock;     /* serializes starting and stopping polling */
    GHashTable *known;      /* uuid string -> redfishKnownSystem */
    int nCallbacks;
    int pollTimer;          /* -1 when not polling */
    int pollInterval;       /* milliseconds */
};

typedef struct {
    char *name;
    int id;
    int state;
} redfishKnownSystem;


static void
redfishKnownSystemFree(gpointer data)
{
    redfishKnownSystem *k = data;

    g_free(k->name);
    g_free(k);
}


G_DEFINE_AUTOPTR_CLEANUP_FUNC(virNodeDeviceObjList, virObjectUnref);

static void
redfishPrivateFree(redfishPrivate *priv)
{
    if (!priv)
        return;

    if (priv->client) {
        redfishClientLogout(priv->client);
        redfishClientFree(priv->client);
    }
    virObjectUnref(priv->caps);
    virObjectUnref(priv->xmlopt);
    if (priv->pollTimer >= 0)
        virEventRemoveTimeout(priv->pollTimer);
    virObjectUnref(priv->eventState);
    g_clear_pointer(&priv->known, g_hash_table_unref);
    virMutexDestroy(&priv->eventLock);
    virMutexDestroy(&priv->startLock);
    virMutexDestroy(&priv->pathsLock);
    g_clear_pointer(&priv->pathByUUID, g_hash_table_unref);
    g_clear_pointer(&priv->pathByName, g_hash_table_unref);
    g_free(priv->version);
    g_free(priv->systemsPath);
    g_free(priv->blocksPath);
    g_free(priv);
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC(redfishPrivate, redfishPrivateFree);


static const char *
redfishURIParam(virURI *uri,
                const char *name)
{
    size_t i;

    for (i = 0; i < uri->paramsCount; i++) {
        if (STRCASEEQ(uri->params[i].name, name))
            return uri->params[i].value;
    }

    return NULL;
}


static const char *
redfishLinkPath(virJSONValue *obj,
                const char *key)
{
    virJSONValue *link = virJSONValueObjectGetObject(obj, key);

    return link ? virJSONValueObjectGetString(link, "@odata.id") : NULL;
}


/* Collections normally keep their members in "Members", but older
 * implementations (e.g. Intel RSD era ones) nest them under "Links". */
static virJSONValue *
redfishCollectionMembers(virJSONValue *coll)
{
    virJSONValue *members = virJSONValueObjectGetArray(coll, "Members");
    virJSONValue *links;

    if (!members && (links = virJSONValueObjectGetObject(coll, "Links")))
        members = virJSONValueObjectGetArray(links, "Members");

    return members;
}


/* List the paths of a collection's members without reading them */
static GPtrArray *
redfishListMemberPaths(redfishPrivate *priv,
                       const char *collection)
{
    g_autoptr(virJSONValue) coll = NULL;
    GPtrArray *ret = g_ptr_array_new_with_free_func(g_free);
    virJSONValue *members;
    size_t i;

    if (!(coll = redfishClientGet(priv->client, collection))) {
        g_ptr_array_unref(ret);
        return NULL;
    }

    members = redfishCollectionMembers(coll);
    for (i = 0; members && i < virJSONValueArraySize(members); i++) {
        const char *path = virJSONValueObjectGetString(virJSONValueArrayGet(members, i),
                                                       "@odata.id");

        if (path)
            g_ptr_array_add(ret, g_strdup(path));
    }

    return ret;
}


/* Fetch all members of a collection as full resources */
static GPtrArray *
redfishFetchMembers(redfishPrivate *priv,
                    const char *collection)
{
    g_autoptr(virJSONValue) coll = NULL;
    GPtrArray *ret = g_ptr_array_new_with_free_func((GDestroyNotify)virJSONValueFree);
    virJSONValue *members;
    size_t i;

    if (!(coll = redfishClientGet(priv->client, collection)))
        goto error;

    members = redfishCollectionMembers(coll);
    for (i = 0; members && i < virJSONValueArraySize(members); i++) {
        const char *path = virJSONValueObjectGetString(virJSONValueArrayGet(members, i),
                                                       "@odata.id");
        virJSONValue *res;

        if (!path)
            continue;
        if (!(res = redfishClientGet(priv->client, path)))
            goto error;
        g_ptr_array_add(ret, res);
    }

    return ret;

 error:
    g_ptr_array_unref(ret);
    return NULL;
}


static void
redfishRememberSystem(redfishPrivate *priv,
                      virJSONValue *sys)
{
    const char *path = virJSONValueObjectGetString(sys, "@odata.id");
    const char *name = redfishSystemGetName(sys);
    unsigned char uuid[VIR_UUID_BUFLEN];
    char uuidstr[VIR_UUID_STRING_BUFLEN];

    if (!path || !name || redfishSystemGetUUID(sys, uuid) < 0)
        return;

    virUUIDFormat(uuid, uuidstr);
    VIR_WITH_MUTEX_LOCK_GUARD(&priv->pathsLock) {
        g_hash_table_replace(priv->pathByUUID, g_strdup(uuidstr), g_strdup(path));
        g_hash_table_replace(priv->pathByName, g_strdup(name), g_strdup(path));
    }
}


static GPtrArray *
redfishFetchComposedSystems(redfishPrivate *priv)
{
    g_autoptr(GPtrArray) all = redfishFetchMembers(priv, priv->systemsPath);
    GPtrArray *ret;
    size_t i;

    if (!all)
        return NULL;

    ret = g_ptr_array_new_with_free_func((GDestroyNotify)virJSONValueFree);
    VIR_WITH_MUTEX_LOCK_GUARD(&priv->pathsLock) {
        g_hash_table_remove_all(priv->pathByUUID);
        g_hash_table_remove_all(priv->pathByName);
    }

    for (i = 0; i < all->len; i++) {
        virJSONValue *sys = g_ptr_array_index(all, i);

        if (!redfishSystemIsComposed(sys))
            continue;

        redfishRememberSystem(priv, sys);
        g_ptr_array_add(ret, sys);
        g_ptr_array_index(all, i) = NULL;
    }

    return ret;
}


static int
redfishSystemId(virJSONValue *system)
{
    const char *path = virJSONValueObjectGetString(system, "@odata.id");
    int id = path ? (g_str_hash(path) & 0x7fffffff) : 0;

    return id ? id : 1;
}


static virDomainPtr
redfishSystemToDomain(virConnectPtr conn,
                      virJSONValue *system)
{
    unsigned char uuid[VIR_UUID_BUFLEN];
    int state = redfishSystemGetState(system, NULL);
    const char *name = redfishSystemGetName(system);

    if (!name || redfishSystemGetUUID(system, uuid) < 0)
        return NULL;

    return virGetDomain(conn, name, uuid,
                        state == VIR_DOMAIN_SHUTOFF ? -1 : redfishSystemId(system));
}


/*
 * Find the composed system backing @dom. Returns a new reference.
 */
static bool
redfishSystemMatches(virJSONValue *sys,
                     const char *name,
                     const unsigned char *uuid)
{
    unsigned char sysuuid[VIR_UUID_BUFLEN];

    if (!redfishSystemIsComposed(sys))
        return false;

    if (uuid)
        return redfishSystemGetUUID(sys, sysuuid) == 0 &&
               memcmp(uuid, sysuuid, VIR_UUID_BUFLEN) == 0;

    return STREQ_NULLABLE(redfishSystemGetName(sys), name);
}


/* Try the remembered location with a single request. */
static virJSONValue *
redfishLookupSystemCached(redfishPrivate *priv,
                          const char *name,
                          const unsigned char *uuid)
{
    g_autofree char *path = NULL;
    virJSONValue *sys;

    VIR_WITH_MUTEX_LOCK_GUARD(&priv->pathsLock) {
        if (uuid) {
            char uuidstr[VIR_UUID_STRING_BUFLEN];

            virUUIDFormat(uuid, uuidstr);
            path = g_strdup(g_hash_table_lookup(priv->pathByUUID, uuidstr));
        } else {
            path = g_strdup(g_hash_table_lookup(priv->pathByName, name));
        }
    }

    if (!path)
        return NULL;

    /* The system may be gone or have changed, in which case the
     * caller falls back to a full scan, so errors are not fatal. */
    {
        virErrorPtr orig_err;

        virErrorPreserveLast(&orig_err);
        sys = redfishClientGet(priv->client, path);
        virErrorRestore(&orig_err);
    }

    if (sys && !redfishSystemMatches(sys, name, uuid))
        g_clear_pointer(&sys, virJSONValueFree);

    return sys;
}


static virJSONValue *
redfishLookupSystem(redfishPrivate *priv,
                    const char *name,
                    const unsigned char *uuid)
{
    g_autoptr(GPtrArray) paths = NULL;
    char uuidstr[VIR_UUID_STRING_BUFLEN];
    virJSONValue *cached;
    size_t i;

    if ((cached = redfishLookupSystemCached(priv, name, uuid)))
        return cached;

    if (!(paths = redfishListMemberPaths(priv, priv->systemsPath)))
        return NULL;

    /* Read systems one by one and stop at the match */
    for (i = 0; i < paths->len; i++) {
        virJSONValue *sys = redfishClientGet(priv->client,
                                             g_ptr_array_index(paths, i));

        if (!sys)
            return NULL;

        if (redfishSystemIsComposed(sys))
            redfishRememberSystem(priv, sys);

        if (redfishSystemMatches(sys, name, uuid))
            return sys;

        virJSONValueFree(sys);
    }

    if (uuid) {
        virUUIDFormat(uuid, uuidstr);
        virReportError(VIR_ERR_NO_DOMAIN,
                       _("no domain with matching uuid '%1$s'"), uuidstr);
    } else {
        virReportError(VIR_ERR_NO_DOMAIN,
                       _("no domain with matching name '%1$s'"), name);
    }
    return NULL;
}


static virJSONValue *
redfishLookupDomain(virDomainPtr dom)
{
    redfishPrivate *priv = dom->conn->privateData;

    return redfishLookupSystem(priv, NULL, dom->uuid);
}


/* --- connection ---------------------------------------------------------- */

static virDrvOpenStatus
redfishConnectOpen(virConnectPtr conn,
                   virConnectAuthPtr auth,
                   virConf *conf G_GNUC_UNUSED,
                   unsigned int flags)
{
    g_autoptr(redfishPrivate) priv = NULL;
    g_autoptr(virJSONValue) root = NULL;
    g_autofree char *baseURL = NULL;
    g_autofree char *username = NULL;
    g_autofree char *password = NULL;
    const char *transport;
    const char *param;
    const char *sessions;
    const char *systems;
    const char *comp;
    unsigned int timeout = REDFISH_DEFAULT_TIMEOUT;
    unsigned int port;
    bool verify = true;
    virDrvOpenStatus ret = VIR_DRV_OPEN_ERROR;

    virCheckFlags(VIR_CONNECT_RO, VIR_DRV_OPEN_ERROR);

    if (!conn->uri || !conn->uri->scheme ||
        STRNEQ(conn->uri->scheme, "redfish"))
        return VIR_DRV_OPEN_DECLINED;

    if (!conn->uri->server) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("Redfish URI must specify a host name"));
        return VIR_DRV_OPEN_ERROR;
    }

    transport = redfishURIParam(conn->uri, "transport");
    if (!transport)
        transport = "https";
    if (STRNEQ(transport, "http") && STRNEQ(transport, "https")) {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("Unsupported Redfish transport '%1$s'"), transport);
        return VIR_DRV_OPEN_ERROR;
    }

    if ((param = redfishURIParam(conn->uri, "no_verify")) &&
        STRNEQ(param, "0"))
        verify = false;

    if ((param = redfishURIParam(conn->uri, "timeout")) &&
        (virStrToLong_ui(param, NULL, 10, &timeout) < 0 || timeout == 0)) {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("Invalid Redfish timeout '%1$s'"), param);
        return VIR_DRV_OPEN_ERROR;
    }

    port = conn->uri->port ? conn->uri->port : (STREQ(transport, "https") ? 443 : 80);
    if (strchr(conn->uri->server, ':'))
        baseURL = g_strdup_printf("%s://[%s]:%u", transport, conn->uri->server, port);
    else
        baseURL = g_strdup_printf("%s://%s:%u", transport, conn->uri->server, port);

    priv = g_new0(redfishPrivate, 1);
    priv->pathByUUID = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    priv->pathByName = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    priv->known = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                        redfishKnownSystemFree);
    priv->pollTimer = -1;
    priv->pollInterval = REDFISH_DEFAULT_POLL * 1000;
    if ((param = redfishURIParam(conn->uri, "poll"))) {
        unsigned int secs;

        if (virStrToLong_ui(param, NULL, 10, &secs) < 0 || secs == 0 ||
            secs > 3600) {
            virReportError(VIR_ERR_INVALID_ARG,
                           _("Invalid Redfish poll interval '%1$s'"), param);
            redfishPrivateFree(g_steal_pointer(&priv));
            return VIR_DRV_OPEN_ERROR;
        }
        priv->pollInterval = secs * 1000;
    }
    if (!(priv->eventState = virObjectEventStateNew())) {
        redfishPrivateFree(g_steal_pointer(&priv));
        return VIR_DRV_OPEN_ERROR;
    }
    if (virMutexInit(&priv->eventLock) < 0 ||
        virMutexInit(&priv->startLock) < 0 ||
        virMutexInit(&priv->pathsLock) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not initialize mutex"));
        g_free(priv);
        priv = NULL;
        return VIR_DRV_OPEN_ERROR;
    }

    if (!(priv->client = redfishClientNew(baseURL, verify, timeout)))
        return VIR_DRV_OPEN_ERROR;
    priv->encrypted = redfishClientIsEncrypted(priv->client);

    /* The service root is readable without authentication */
    if (!(root = redfishClientGet(priv->client, REDFISH_ROOT)))
        return VIR_DRV_OPEN_ERROR;

    if (conn->uri->user) {
        username = g_strdup(conn->uri->user);
    } else if (!(username = virAuthGetUsername(conn, auth, "redfish", "admin",
                                               conn->uri->server))) {
        return VIR_DRV_OPEN_ERROR;
    }
    if (!(password = virAuthGetPassword(conn, auth, "redfish", username,
                                        conn->uri->server)))
        return VIR_DRV_OPEN_ERROR;

    sessions = NULL;
    {
        virJSONValue *links = virJSONValueObjectGetObject(root, "Links");
        if (links)
            sessions = redfishLinkPath(links, "Sessions");
    }
    if (redfishClientLogin(priv->client, sessions, username, password) < 0)
        return VIR_DRV_OPEN_ERROR;
    virSecureEraseString(password);

    if (!(systems = redfishLinkPath(root, "Systems")) ||
        !(comp = redfishLinkPath(root, "CompositionService"))) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("Redfish service at %1$s does not offer a composition service"),
                       baseURL);
        return VIR_DRV_OPEN_ERROR;
    }
    priv->systemsPath = g_strdup(systems);

    {
        g_autoptr(virJSONValue) cs = redfishClientGet(priv->client, comp);
        const char *blocks;

        if (!cs)
            return VIR_DRV_OPEN_ERROR;
        if (!(blocks = redfishLinkPath(cs, "ResourceBlocks"))) {
            virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                           _("Redfish composition service has no ResourceBlocks"));
            return VIR_DRV_OPEN_ERROR;
        }
        priv->blocksPath = g_strdup(blocks);
    }

    priv->version = g_strdup(virJSONValueObjectGetString(root, "RedfishVersion"));

    if (!(priv->caps = redfishCapsInit()) ||
        !(priv->xmlopt = redfishDomainXMLConfInit()))
        return VIR_DRV_OPEN_ERROR;

    conn->privateData = g_steal_pointer(&priv);
    ret = VIR_DRV_OPEN_SUCCESS;
    return ret;
}


static int
redfishConnectClose(virConnectPtr conn)
{
    redfishPrivate *priv = conn->privateData;

    redfishPrivateFree(priv);
    conn->privateData = NULL;
    return 0;
}


static const char *
redfishConnectGetType(virConnectPtr conn G_GNUC_UNUSED)
{
    return "Redfish";
}


static int
redfishConnectGetVersion(virConnectPtr conn,
                         unsigned long *version)
{
    redfishPrivate *priv = conn->privateData;
    unsigned long long v = 0;

    if (priv->version &&
        virStringParseVersion(&v, priv->version, true) < 0)
        v = 0;

    *version = v;
    return 0;
}


static char *
redfishConnectGetHostname(virConnectPtr conn)
{
    return g_strdup(conn->uri->server);
}


static char *
redfishConnectGetCapabilities(virConnectPtr conn)
{
    redfishPrivate *priv = conn->privateData;

    return virCapabilitiesFormatXML(priv->caps);
}


static int
redfishConnectIsEncrypted(virConnectPtr conn)
{
    redfishPrivate *priv = conn->privateData;

    return priv->encrypted ? 1 : 0;
}


static int
redfishConnectIsSecure(virConnectPtr conn)
{
    return redfishConnectIsEncrypted(conn);
}


static int
redfishConnectIsAlive(virConnectPtr conn G_GNUC_UNUSED)
{
    return 1;
}


/* --- domains ------------------------------------------------------------- */

#define MATCH(FLAG) (flags & (FLAG))

static int
redfishConnectListAllDomains(virConnectPtr conn,
                             virDomainPtr **domains,
                             unsigned int flags)
{
    redfishPrivate *priv = conn->privateData;
    g_autoptr(GPtrArray) systems = NULL;
    virDomainPtr *doms = NULL;
    size_t ndoms = 0;
    size_t i;

    virCheckFlags(VIR_CONNECT_LIST_DOMAINS_FILTERS_ALL, -1);

    if (domains)
        *domains = NULL;

    /* Composed systems are always persistent, and none of autostart,
     * snapshots or managed save apply to them */
    if ((MATCH(VIR_CONNECT_LIST_DOMAINS_TRANSIENT) &&
         !MATCH(VIR_CONNECT_LIST_DOMAINS_PERSISTENT)) ||
        (MATCH(VIR_CONNECT_LIST_DOMAINS_AUTOSTART) &&
         !MATCH(VIR_CONNECT_LIST_DOMAINS_NO_AUTOSTART)) ||
        (MATCH(VIR_CONNECT_LIST_DOMAINS_HAS_SNAPSHOT) &&
         !MATCH(VIR_CONNECT_LIST_DOMAINS_NO_SNAPSHOT)) ||
        (MATCH(VIR_CONNECT_LIST_DOMAINS_MANAGEDSAVE) &&
         !MATCH(VIR_CONNECT_LIST_DOMAINS_NO_MANAGEDSAVE)) ||
        (MATCH(VIR_CONNECT_LIST_DOMAINS_HAS_CHECKPOINT) &&
         !MATCH(VIR_CONNECT_LIST_DOMAINS_NO_CHECKPOINT))) {
        if (domains)
            *domains = g_new0(virDomainPtr, 1);
        return 0;
    }

    if (!(systems = redfishFetchComposedSystems(priv)))
        return -1;

    if (domains)
        doms = g_new0(virDomainPtr, systems->len + 1);

    for (i = 0; i < systems->len; i++) {
        virJSONValue *sys = g_ptr_array_index(systems, i);
        int state = redfishSystemGetState(sys, NULL);
        bool active = state != VIR_DOMAIN_SHUTOFF;

        if (MATCH(VIR_CONNECT_LIST_DOMAINS_FILTERS_ACTIVE) &&
            !((MATCH(VIR_CONNECT_LIST_DOMAINS_ACTIVE) && active) ||
              (MATCH(VIR_CONNECT_LIST_DOMAINS_INACTIVE) && !active)))
            continue;

        if (MATCH(VIR_CONNECT_LIST_DOMAINS_FILTERS_STATE) &&
            !((MATCH(VIR_CONNECT_LIST_DOMAINS_RUNNING) && state == VIR_DOMAIN_RUNNING) ||
              (MATCH(VIR_CONNECT_LIST_DOMAINS_PAUSED) && state == VIR_DOMAIN_PAUSED) ||
              (MATCH(VIR_CONNECT_LIST_DOMAINS_SHUTOFF) && state == VIR_DOMAIN_SHUTOFF) ||
              (MATCH(VIR_CONNECT_LIST_DOMAINS_OTHER) &&
               state != VIR_DOMAIN_RUNNING && state != VIR_DOMAIN_PAUSED &&
               state != VIR_DOMAIN_SHUTOFF)))
            continue;

        if (doms) {
            if (!(doms[ndoms] = redfishSystemToDomain(conn, sys)))
                goto error;
        }
        ndoms++;
    }

    if (domains)
        *domains = doms;
    return ndoms;

 error:
    virObjectListFree(doms);
    return -1;
}


static virDomainPtr
redfishDomainLookupByUUID(virConnectPtr conn,
                          const unsigned char *uuid)
{
    redfishPrivate *priv = conn->privateData;
    g_autoptr(virJSONValue) sys = redfishLookupSystem(priv, NULL, uuid);

    return sys ? redfishSystemToDomain(conn, sys) : NULL;
}


static virDomainPtr
redfishDomainLookupByName(virConnectPtr conn,
                          const char *name)
{
    redfishPrivate *priv = conn->privateData;
    g_autoptr(virJSONValue) sys = redfishLookupSystem(priv, name, NULL);

    return sys ? redfishSystemToDomain(conn, sys) : NULL;
}


static int
redfishDomainGetState(virDomainPtr dom,
                      int *state,
                      int *reason,
                      unsigned int flags)
{
    g_autoptr(virJSONValue) sys = NULL;

    virCheckFlags(0, -1);

    if (!(sys = redfishLookupDomain(dom)))
        return -1;

    *state = redfishSystemGetState(sys, reason);
    return 0;
}


static int
redfishDomainGetInfo(virDomainPtr dom,
                     virDomainInfoPtr info)
{
    redfishPrivate *priv = dom->conn->privateData;
    g_autoptr(virJSONValue) sys = NULL;
    g_autoptr(virDomainDef) def = NULL;

    if (!(sys = redfishLookupDomain(dom)))
        return -1;
    if (!(def = redfishSystemToDomainDef(sys, priv->xmlopt, -1)))
        return -1;

    memset(info, 0, sizeof(*info));
    info->state = redfishSystemGetState(sys, NULL);
    info->maxMem = virDomainDefGetMemoryTotal(def);
    info->memory = def->mem.cur_balloon;
    info->nrVirtCpu = virDomainDefGetVcpus(def);
    return 0;
}


static int
redfishFetchParts(redfishPrivate *priv,
                  virJSONValue *block,
                  GPtrArray *parts)
{
    const char *keys[] = { "Processors", "Memory", "Drives" };
    size_t k;

    for (k = 0; k < G_N_ELEMENTS(keys); k++) {
        virJSONValue *refs = virJSONValueObjectGetArray(block, keys[k]);
        size_t i;

        for (i = 0; refs && i < virJSONValueArraySize(refs); i++) {
            const char *path = virJSONValueObjectGetString(virJSONValueArrayGet(refs, i),
                                                           "@odata.id");
            virJSONValue *part;

            if (!path)
                continue;
            if (!(part = redfishClientGet(priv->client, path)))
                return -1;
            g_ptr_array_add(parts, part);
        }
    }

    return 0;
}


/*
 * Describe the blocks of a composed system, so that users can tell what
 * hardware each of them is without a separate nodedev-dumpxml.
 */
static void
redfishDescribeBlocks(redfishPrivate *priv,
                      virJSONValue *sys,
                      redfishDomainNamespaceDef *nsdef)
{
    virJSONValue *links = virJSONValueObjectGetObject(sys, "Links");
    virJSONValue *refs = links ? virJSONValueObjectGetArray(links, "ResourceBlocks") : NULL;
    size_t n = refs ? virJSONValueArraySize(refs) : 0;
    size_t i;
    size_t j = 0;
    virErrorPtr orig_err;

    /* Purely informational, so a failure to read a block is not fatal */
    virErrorPreserveLast(&orig_err);

    nsdef->kinds = g_new0(char *, g_strv_length(nsdef->blocks) + 1);
    nsdef->descs = g_new0(char *, g_strv_length(nsdef->blocks) + 1);

    for (i = 0; i < n && nsdef->blocks[j]; i++) {
        const char *path = virJSONValueObjectGetString(virJSONValueArrayGet(refs, i),
                                                       "@odata.id");
        g_autoptr(virJSONValue) block = NULL;
        g_autoptr(GPtrArray) parts = g_ptr_array_new_with_free_func((GDestroyNotify)virJSONValueFree);
        g_autoptr(virNodeDeviceDef) def = NULL;

        if (!path)
            continue;

        if ((block = redfishClientGet(priv->client, path)) &&
            redfishFetchParts(priv, block, parts) == 0 &&
            (def = redfishResourceBlockToNodeDeviceDef(block,
                                                       (virJSONValue **)parts->pdata,
                                                       parts->len)))
            redfishNodeDeviceDefDescribe(def, &nsdef->kinds[j], &nsdef->descs[j]);
        j++;
    }

    virErrorRestore(&orig_err);
}


static char *
redfishDomainGetXMLDesc(virDomainPtr dom,
                        unsigned int flags)
{
    redfishPrivate *priv = dom->conn->privateData;
    g_autoptr(virJSONValue) sys = NULL;
    g_autoptr(virDomainDef) def = NULL;

    virCheckFlags(VIR_DOMAIN_XML_COMMON_FLAGS, NULL);

    if (!(sys = redfishLookupDomain(dom)))
        return NULL;
    if (!(def = redfishSystemToDomainDef(sys, priv->xmlopt,
                                         redfishSystemGetState(sys, NULL) == VIR_DOMAIN_SHUTOFF
                                         ? -1 : redfishSystemId(sys))))
        return NULL;

    if (def->namespaceData)
        redfishDescribeBlocks(priv, sys, def->namespaceData);

    return virDomainDefFormat(def, priv->xmlopt,
                              virDomainDefFormatConvertXMLFlags(flags));
}


static int
redfishDomainIsActive(virDomainPtr dom)
{
    g_autoptr(virJSONValue) sys = redfishLookupDomain(dom);

    if (!sys)
        return -1;

    return redfishSystemGetState(sys, NULL) != VIR_DOMAIN_SHUTOFF;
}


static int
redfishDomainIsPersistent(virDomainPtr dom G_GNUC_UNUSED)
{
    return 1;
}


static int
redfishDomainIsUpdated(virDomainPtr dom G_GNUC_UNUSED)
{
    return 0;
}


static int
redfishDomainReset(redfishPrivate *priv,
                   virJSONValue *sys,
                   const char *type)
{
    g_autoptr(virJSONValue) body = NULL;
    g_autofree char *fallback = NULL;
    const char *target = NULL;
    virJSONValue *actions = virJSONValueObjectGetObject(sys, "Actions");
    virJSONValue *reset;

    if (actions && (reset = virJSONValueObjectGetObject(actions, "#ComputerSystem.Reset")))
        target = virJSONValueObjectGetString(reset, "target");
    if (!target) {
        fallback = g_strdup_printf("%s/Actions/ComputerSystem.Reset",
                                   virJSONValueObjectGetString(sys, "@odata.id"));
        target = fallback;
    }

    if (virJSONValueObjectAdd(&body, "s:ResetType", type, NULL) < 0)
        return -1;

    return redfishClientCall(priv->client, "POST", target, body, NULL, NULL);
}


static int
redfishDomainPower(virDomainPtr dom,
                   const char *type,
                   bool wantActive,
                   const char *invalidMsg)
{
    redfishPrivate *priv = dom->conn->privateData;
    g_autoptr(virJSONValue) sys = NULL;

    if (!(sys = redfishLookupDomain(dom)))
        return -1;

    if ((redfishSystemGetState(sys, NULL) != VIR_DOMAIN_SHUTOFF) != wantActive) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s", invalidMsg);
        return -1;
    }

    return redfishDomainReset(priv, sys, type);
}


static int
redfishDomainCreateWithFlags(virDomainPtr dom,
                             unsigned int flags)
{
    virCheckFlags(0, -1);

    return redfishDomainPower(dom, "On", false,
                              _("domain is already running"));
}


static int
redfishDomainCreate(virDomainPtr dom)
{
    return redfishDomainCreateWithFlags(dom, 0);
}


static int
redfishDomainDestroyFlags(virDomainPtr dom,
                          unsigned int flags)
{
    virCheckFlags(0, -1);

    return redfishDomainPower(dom, "ForceOff", true,
                              _("domain is not running"));
}


static int
redfishDomainDestroy(virDomainPtr dom)
{
    return redfishDomainDestroyFlags(dom, 0);
}


static int
redfishDomainShutdownFlags(virDomainPtr dom,
                           unsigned int flags)
{
    virCheckFlags(0, -1);

    return redfishDomainPower(dom, "GracefulShutdown", true,
                              _("domain is not running"));
}


static int
redfishDomainShutdown(virDomainPtr dom)
{
    return redfishDomainShutdownFlags(dom, 0);
}


static int
redfishDomainReboot(virDomainPtr dom,
                    unsigned int flags)
{
    virCheckFlags(0, -1);

    return redfishDomainPower(dom, "GracefulRestart", true,
                              _("domain is not running"));
}


static int
redfishDomainResetFlags(virDomainPtr dom,
                        unsigned int flags)
{
    virCheckFlags(0, -1);

    return redfishDomainPower(dom, "ForceRestart", true,
                              _("domain is not running"));
}


/* Find the resource block with the given device name in the collection
 * member @members. Returns the block's path, owned by @members. */
static const char *
redfishFindBlockPath(virJSONValue *members,
                     const char *name)
{
    size_t i;

    for (i = 0; members && i < virJSONValueArraySize(members); i++) {
        const char *path = virJSONValueObjectGetString(virJSONValueArrayGet(members, i),
                                                       "@odata.id");
        g_autofree char *base = path ? redfishPathBaseName(path) : NULL;
        g_autofree char *devname = base ? redfishSanitizeName(base) : NULL;

        if (devname && STREQ(devname, name))
            return path;
    }

    return NULL;
}


/* Map the <redfish:block> names of a definition to resource block URIs */
static char **
redfishResolveBlocks(redfishPrivate *priv,
                     virDomainDef *def)
{
    redfishDomainNamespaceDef *nsdef = def->namespaceData;
    g_autoptr(virJSONValue) coll = NULL;
    virJSONValue *members;
    g_autoptr(GStrvBuilder) builder = g_strv_builder_new();
    char **name;

    if (!nsdef || !nsdef->blocks || !nsdef->blocks[0]) {
        virReportError(VIR_ERR_XML_ERROR, "%s",
                       _("domain must list the resource blocks to compose in <redfish:composition>"));
        return NULL;
    }

    if (!(coll = redfishClientGet(priv->client, priv->blocksPath)))
        return NULL;
    members = redfishCollectionMembers(coll);

    for (name = nsdef->blocks; *name; name++) {
        const char *found = redfishFindBlockPath(members, *name);

        if (!found) {
            virReportError(VIR_ERR_NO_NODE_DEVICE,
                           _("no resource block named '%1$s'"), *name);
            return NULL;
        }
        g_strv_builder_add(builder, found);
    }

    return g_strv_builder_end(builder);
}


static virDomainPtr
redfishDomainDefineXMLFlags(virConnectPtr conn,
                            const char *xml,
                            unsigned int flags)
{
    redfishPrivate *priv = conn->privateData;
    g_autoptr(virDomainDef) def = NULL;
    g_autoptr(virJSONValue) req = NULL;
    g_autoptr(virJSONValue) created = NULL;
    g_auto(GStrv) blocks = NULL;
    g_autofree char *location = NULL;
    g_autoptr(virJSONValue) sys = NULL;

    virCheckFlags(0, NULL);

    if (!(def = virDomainDefParseString(xml, priv->xmlopt, NULL, 0)))
        return NULL;

    if (def->virtType != VIR_DOMAIN_VIRT_REDFISH) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("domain type must be 'redfish'"));
        return NULL;
    }

    if (!(blocks = redfishResolveBlocks(priv, def)))
        return NULL;
    if (!(req = redfishDomainDefToComposeRequest(def, (const char *const *)blocks)))
        return NULL;

    if (redfishClientCall(priv->client, "POST", priv->systemsPath, req,
                          &created, &location) < 0)
        return NULL;

    if (location) {
        if (!(sys = redfishClientGet(priv->client, location)))
            return NULL;
    } else if (created && virJSONValueObjectHasKey(created, "SystemType")) {
        sys = g_steal_pointer(&created);
    } else {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Redfish service did not report the composed system"));
        return NULL;
    }

    return redfishSystemToDomain(conn, sys);
}


static virDomainPtr
redfishDomainDefineXML(virConnectPtr conn,
                       const char *xml)
{
    return redfishDomainDefineXMLFlags(conn, xml, 0);
}


static virDomainPtr
redfishDomainCreateXML(virConnectPtr conn,
                       const char *xml,
                       unsigned int flags)
{
    virDomainPtr dom;

    virCheckFlags(0, NULL);

    if (!(dom = redfishDomainDefineXMLFlags(conn, xml, 0)))
        return NULL;

    if (redfishDomainCreate(dom) < 0) {
        virErrorPtr err;

        virErrorPreserveLast(&err);
        /* don't leave half composed hardware behind */
        if (virDomainUndefine(dom) < 0)
            VIR_WARN("Failed to decompose %s after failed power on", dom->name);
        virErrorRestore(&err);
        virObjectUnref(dom);
        return NULL;
    }

    return dom;
}


static int
redfishDomainUndefineFlags(virDomainPtr dom,
                           unsigned int flags)
{
    redfishPrivate *priv = dom->conn->privateData;
    g_autoptr(virJSONValue) sys = NULL;
    const char *path;

    virCheckFlags(0, -1);

    if (!(sys = redfishLookupDomain(dom)))
        return -1;

    if (redfishSystemGetState(sys, NULL) != VIR_DOMAIN_SHUTOFF) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("cannot decompose a running domain, power it off first"));
        return -1;
    }

    path = virJSONValueObjectGetString(sys, "@odata.id");
    return redfishClientCall(priv->client, "DELETE", path, NULL, NULL, NULL);
}


static int
redfishDomainUndefine(virDomainPtr dom)
{
    return redfishDomainUndefineFlags(dom, 0);
}


/*
 * Attach and detach take ordinary device XML whose <alias name=''/> names
 * the resource block:
 *
 *   <hostdev mode='subsystem' type='pci'><alias name='GPU2'/></hostdev>
 *   <disk type='network' device='disk'><alias name='NVMe1'/></disk>
 *
 * Only the alias is read. The regular device parser can't be used since
 * a block has no host address (libvirt requires a PCI source address) and
 * since what the device is comes from the block, not from the XML.
 */
static char *
redfishParseDeviceBlockName(const char *xml)
{
    g_autoptr(xmlDoc) doc = NULL;
    g_autoptr(xmlXPathContext) ctxt = NULL;
    g_autofree char *name = NULL;
    const char *root;

    if (!(doc = virXMLParseStringCtxt(xml, _("(device_definition)"), &ctxt)))
        return NULL;

    root = (const char *)ctxt->node->name;
    if (STRNEQ(root, "hostdev") && STRNEQ(root, "disk")) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("devices of type '%1$s' can't be attached, use <hostdev> or <disk>"),
                       root);
        return NULL;
    }

    if (!(name = virXPathString("string(./alias/@name)", ctxt)) || !*name) {
        virReportError(VIR_ERR_XML_ERROR, "%s",
                       _("device needs an <alias name=''/> naming the resource block"));
        return NULL;
    }

    return g_steal_pointer(&name);
}


static int
redfishDomainChangeBlocks(virDomainPtr dom,
                          const char *xml,
                          bool attach,
                          unsigned int flags)
{
    redfishPrivate *priv = dom->conn->privateData;
    g_autoptr(virJSONValue) sys = NULL;
    g_autoptr(virJSONValue) coll = NULL;
    g_autoptr(virJSONValue) blocks = virJSONValueNewArray();
    g_autoptr(virJSONValue) links = NULL;
    g_autoptr(virJSONValue) req = NULL;
    g_autofree char *name = NULL;
    const char *newPath = NULL;
    virJSONValue *sysLinks;
    virJSONValue *current;
    bool present = false;
    size_t i;

    /* A composed system has a single state, so live and persistent
     * changes are the same thing; the service decides when it may happen */
    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE | VIR_DOMAIN_AFFECT_CONFIG, -1);

    if (!(name = redfishParseDeviceBlockName(xml)))
        return -1;
    if (!(sys = redfishLookupDomain(dom)))
        return -1;

    sysLinks = virJSONValueObjectGetObject(sys, "Links");
    current = sysLinks ? virJSONValueObjectGetArray(sysLinks, "ResourceBlocks") : NULL;

    for (i = 0; current && i < virJSONValueArraySize(current); i++) {
        const char *path = virJSONValueObjectGetString(virJSONValueArrayGet(current, i),
                                                       "@odata.id");
        g_autofree char *base = path ? redfishPathBaseName(path) : NULL;
        g_autofree char *devname = base ? redfishSanitizeName(base) : NULL;
        g_autoptr(virJSONValue) ref = NULL;

        if (!path)
            continue;

        if (devname && STREQ(devname, name)) {
            present = true;
            if (!attach)
                continue;   /* leave it out */
        }

        if (virJSONValueObjectAdd(&ref, "s:@odata.id", path, NULL) < 0 ||
            virJSONValueArrayAppend(blocks, &ref) < 0)
            return -1;
    }

    if (attach) {
        g_autoptr(virJSONValue) ref = NULL;

        if (present) {
            virReportError(VIR_ERR_OPERATION_INVALID,
                           _("resource block '%1$s' is already part of the domain"), name);
            return -1;
        }

        if (!(coll = redfishClientGet(priv->client, priv->blocksPath)))
            return -1;
        if (!(newPath = redfishFindBlockPath(redfishCollectionMembers(coll), name))) {
            virReportError(VIR_ERR_NO_NODE_DEVICE,
                           _("no resource block named '%1$s'"), name);
            return -1;
        }

        if (virJSONValueObjectAdd(&ref, "s:@odata.id", newPath, NULL) < 0 ||
            virJSONValueArrayAppend(blocks, &ref) < 0)
            return -1;
    } else if (!present) {
        virReportError(VIR_ERR_DEVICE_MISSING,
                       _("resource block '%1$s' is not part of the domain"), name);
        return -1;
    }

    if (virJSONValueObjectAdd(&links, "a:ResourceBlocks", &blocks, NULL) < 0 ||
        virJSONValueObjectAdd(&req, "a:Links", &links, NULL) < 0)
        return -1;

    return redfishClientCall(priv->client, "PATCH",
                             virJSONValueObjectGetString(sys, "@odata.id"),
                             req, NULL, NULL);
}


static int
redfishDomainAttachDeviceFlags(virDomainPtr dom,
                               const char *xml,
                               unsigned int flags)
{
    return redfishDomainChangeBlocks(dom, xml, true, flags);
}


static int
redfishDomainAttachDevice(virDomainPtr dom,
                          const char *xml)
{
    return redfishDomainChangeBlocks(dom, xml, true, 0);
}


static int
redfishDomainDetachDeviceFlags(virDomainPtr dom,
                               const char *xml,
                               unsigned int flags)
{
    return redfishDomainChangeBlocks(dom, xml, false, flags);
}


static int
redfishDomainDetachDevice(virDomainPtr dom,
                          const char *xml)
{
    return redfishDomainChangeBlocks(dom, xml, false, 0);
}


static int
redfishDomainGetAutostart(virDomainPtr dom G_GNUC_UNUSED,
                          int *autostart)
{
    *autostart = 0;
    return 0;
}


/* --- events -------------------------------------------------------------- */

static void
redfishEmit(redfishPrivate *priv,
            const char *name,
            int id,
            const unsigned char *uuid,
            int type,
            int detail)
{
    virObjectEvent *ev = virDomainEventLifecycleNew(id, name, uuid, type, detail);

    if (ev)
        virObjectEventStateQueue(priv->eventState, ev);
}


/* Queue the events for the difference between two states of a system */
static void
redfishEmitTransition(redfishPrivate *priv,
                      const redfishKnownSystem *old,
                      const redfishKnownSystem *cur,
                      const unsigned char *uuid)
{
    if (cur->state == old->state)
        return;

    if (cur->state == VIR_DOMAIN_RUNNING) {
        if (old->state == VIR_DOMAIN_PAUSED)
            redfishEmit(priv, cur->name, cur->id, uuid,
                        VIR_DOMAIN_EVENT_RESUMED,
                        VIR_DOMAIN_EVENT_RESUMED_UNPAUSED);
        else
            redfishEmit(priv, cur->name, cur->id, uuid,
                        VIR_DOMAIN_EVENT_STARTED,
                        VIR_DOMAIN_EVENT_STARTED_BOOTED);
    } else if (cur->state == VIR_DOMAIN_SHUTOFF) {
        redfishEmit(priv, cur->name, -1, uuid,
                    VIR_DOMAIN_EVENT_STOPPED,
                    VIR_DOMAIN_EVENT_STOPPED_SHUTDOWN);
    } else if (cur->state == VIR_DOMAIN_PAUSED) {
        redfishEmit(priv, cur->name, cur->id, uuid,
                    VIR_DOMAIN_EVENT_SUSPENDED,
                    VIR_DOMAIN_EVENT_SUSPENDED_PAUSED);
    }
    /* States such as "powering off" are not reported until they settle */
}


/*
 * Read the composed systems and remember them. With @emit, queue events
 * for what changed since the previous call. Failures are logged and
 * otherwise ignored, so that a service which is briefly unreachable does
 * not end event delivery.
 */
static int
redfishEventsPoll(redfishPrivate *priv,
                  bool emit)
{
    g_autoptr(GPtrArray) systems = NULL;
    g_autoptr(GHashTable) fresh = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                                        redfishKnownSystemFree);
    GHashTableIter iter;
    gpointer key;
    gpointer value;
    virErrorPtr orig_err;
    size_t i;
    int ret = 0;

    virErrorPreserveLast(&orig_err);

    if (!(systems = redfishFetchComposedSystems(priv))) {
        VIR_WARN("Failed to poll Redfish service: %s", virGetLastErrorMessage());
        ret = -1;
        goto cleanup;
    }

    for (i = 0; i < systems->len; i++) {
        virJSONValue *sys = g_ptr_array_index(systems, i);
        redfishKnownSystem *k;
        unsigned char uuid[VIR_UUID_BUFLEN];
        char uuidstr[VIR_UUID_STRING_BUFLEN];

        if (redfishSystemGetUUID(sys, uuid) < 0 || !redfishSystemGetName(sys)) {
            virResetLastError();
            continue;
        }

        virUUIDFormat(uuid, uuidstr);
        k = g_new0(redfishKnownSystem, 1);
        k->name = g_strdup(redfishSystemGetName(sys));
        k->state = redfishSystemGetState(sys, NULL);
        k->id = k->state == VIR_DOMAIN_SHUTOFF ? -1 : redfishSystemId(sys);
        g_hash_table_replace(fresh, g_strdup(uuidstr), k);
    }

    VIR_WITH_MUTEX_LOCK_GUARD(&priv->eventLock) {
        if (emit) {
            g_hash_table_iter_init(&iter, fresh);
            while (g_hash_table_iter_next(&iter, &key, &value)) {
                const redfishKnownSystem *cur = value;
                const redfishKnownSystem *old = g_hash_table_lookup(priv->known, key);
                unsigned char uuid[VIR_UUID_BUFLEN];

                ignore_value(virUUIDParse(key, uuid));
                if (!old) {
                    redfishEmit(priv, cur->name, cur->id, uuid,
                                VIR_DOMAIN_EVENT_DEFINED,
                                VIR_DOMAIN_EVENT_DEFINED_ADDED);
                    if (cur->state == VIR_DOMAIN_RUNNING)
                        redfishEmit(priv, cur->name, cur->id, uuid,
                                    VIR_DOMAIN_EVENT_STARTED,
                                    VIR_DOMAIN_EVENT_STARTED_BOOTED);
                } else {
                    redfishEmitTransition(priv, old, cur, uuid);
                }
            }

            g_hash_table_iter_init(&iter, priv->known);
            while (g_hash_table_iter_next(&iter, &key, &value)) {
                const redfishKnownSystem *old = value;
                unsigned char uuid[VIR_UUID_BUFLEN];

                if (g_hash_table_contains(fresh, key))
                    continue;

                ignore_value(virUUIDParse(key, uuid));
                if (old->state != VIR_DOMAIN_SHUTOFF)
                    redfishEmit(priv, old->name, -1, uuid,
                                VIR_DOMAIN_EVENT_STOPPED,
                                VIR_DOMAIN_EVENT_STOPPED_DESTROYED);
                redfishEmit(priv, old->name, -1, uuid,
                            VIR_DOMAIN_EVENT_UNDEFINED,
                            VIR_DOMAIN_EVENT_UNDEFINED_REMOVED);
            }
        }

        g_hash_table_unref(priv->known);
        priv->known = g_steal_pointer(&fresh);
    }

 cleanup:
    virErrorRestore(&orig_err);
    return ret;
}


static void
redfishEventsTimer(int timer G_GNUC_UNUSED,
                   void *opaque)
{
    redfishEventsPoll(opaque, true);
}


static int
redfishEventsStart(redfishPrivate *priv)
{
    VIR_LOCK_GUARD lock = virLockGuardLock(&priv->startLock);

    if (priv->nCallbacks == 0) {
        /* Without a baseline everything would look new */
        if (redfishEventsPoll(priv, false) < 0) {
            virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                           _("Could not read the Redfish service to start watching it"));
            return -1;
        }

        if ((priv->pollTimer = virEventAddTimeout(priv->pollInterval,
                                                  redfishEventsTimer,
                                                  priv, NULL)) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Could not start polling, is an event loop running?"));
            return -1;
        }
    }

    priv->nCallbacks++;
    return 0;
}


static void
redfishEventsStop(redfishPrivate *priv)
{
    VIR_LOCK_GUARD lock = virLockGuardLock(&priv->startLock);

    if (priv->nCallbacks > 0 && --priv->nCallbacks == 0 && priv->pollTimer >= 0) {
        virEventRemoveTimeout(priv->pollTimer);
        priv->pollTimer = -1;
    }
}


static int
redfishConnectDomainEventRegisterAny(virConnectPtr conn,
                                     virDomainPtr dom,
                                     int eventID,
                                     virConnectDomainEventGenericCallback callback,
                                     void *opaque,
                                     virFreeCallback freecb)
{
    redfishPrivate *priv = conn->privateData;
    int ret;

    if (virDomainEventStateRegisterID(conn, priv->eventState, dom, eventID,
                                      callback, opaque, freecb, &ret) < 0)
        return -1;

    if (redfishEventsStart(priv) < 0) {
        virObjectEventStateDeregisterID(conn, priv->eventState, ret, false);
        return -1;
    }

    return ret;
}


static int
redfishConnectDomainEventDeregisterAny(virConnectPtr conn,
                                       int callbackID)
{
    redfishPrivate *priv = conn->privateData;

    if (virObjectEventStateDeregisterID(conn, priv->eventState,
                                        callbackID, true) < 0)
        return -1;

    redfishEventsStop(priv);
    return 0;
}


/* --- node devices -------------------------------------------------------- */

/*
 * Build node devices from the service's resource blocks.
 *
 * With @only NULL every block is read, but its parts (processors,
 * memory, drives) are fetched only when the block's own type does not
 * say what it is. Listing therefore costs one request per block.
 * With @only set just that block is read, including all of its parts so
 * that the device XML has the full detail.
 */
static virNodeDeviceObjList *
redfishLoadDevices(virConnectPtr conn,
                   const char *only)
{
    redfishPrivate *priv = conn->privateData;
    g_autoptr(GPtrArray) paths = redfishListMemberPaths(priv, priv->blocksPath);
    g_autoptr(virNodeDeviceObjList) list = NULL;
    size_t i;

    if (!paths)
        return NULL;
    if (!(list = virNodeDeviceObjListNew()))
        return NULL;

    for (i = 0; i < paths->len; i++) {
        const char *path = g_ptr_array_index(paths, i);
        g_autoptr(virJSONValue) block = NULL;
        g_autoptr(GPtrArray) parts = g_ptr_array_new_with_free_func((GDestroyNotify)virJSONValueFree);
        g_autoptr(virNodeDeviceDef) def = NULL;
        virNodeDeviceObj *obj;

        if (only) {
            g_autofree char *base = redfishPathBaseName(path);
            g_autofree char *name = redfishSanitizeName(base);

            if (STRNEQ(name, only))
                continue;
        }

        if (!(block = redfishClientGet(priv->client, path)))
            return NULL;

        if ((only || redfishResourceBlockNeedsParts(block)) &&
            redfishFetchParts(priv, block, parts) < 0)
            return NULL;

        if (!(def = redfishResourceBlockToNodeDeviceDef(block,
                                                        (virJSONValue **)parts->pdata,
                                                        parts->len)))
            return NULL;

        if (only && STRNEQ(def->name, only))
            continue;

        if (!(obj = virNodeDeviceObjListAssignDef(list, def)))
            return NULL;
        def = NULL; /* owned by the object now */

        virNodeDeviceObjSetActive(obj, true);
        virNodeDeviceObjEndAPI(&obj);
    }

    return g_steal_pointer(&list);
}


static int
redfishNodeNumOfDevices(virConnectPtr conn,
                        const char *cap,
                        unsigned int flags)
{
    g_autoptr(virNodeDeviceObjList) devs = NULL;

    virCheckFlags(0, -1);

    if (!(devs = redfishLoadDevices(conn, NULL)))
        return -1;

    return virNodeDeviceObjListNumOfDevices(devs, conn, cap, NULL);
}


static int
redfishNodeListDevices(virConnectPtr conn,
                       const char *cap,
                       char **const names,
                       int maxnames,
                       unsigned int flags)
{
    g_autoptr(virNodeDeviceObjList) devs = NULL;

    virCheckFlags(0, -1);

    if (!(devs = redfishLoadDevices(conn, NULL)))
        return -1;

    return virNodeDeviceObjListGetNames(devs, conn, NULL, cap, names, maxnames);
}


static int
redfishConnectListAllNodeDevices(virConnectPtr conn,
                                 virNodeDevicePtr **devices,
                                 unsigned int flags)
{
    g_autoptr(virNodeDeviceObjList) devs = NULL;

    virCheckFlags(VIR_CONNECT_LIST_NODE_DEVICES_FILTERS_ALL, -1);

    if (!(devs = redfishLoadDevices(conn, NULL)))
        return -1;

    return virNodeDeviceObjListExport(conn, devs, devices, NULL, flags);
}


static virNodeDeviceObj *
redfishNodeDeviceFind(virConnectPtr conn,
                      const char *name,
                      virNodeDeviceObjList **listRet)
{
    g_autoptr(virNodeDeviceObjList) devs = NULL;
    virNodeDeviceObj *obj;

    if (!(devs = redfishLoadDevices(conn, name)))
        return NULL;

    if (!(obj = virNodeDeviceObjListFindByName(devs, name))) {
        virReportError(VIR_ERR_NO_NODE_DEVICE,
                       _("no node device with matching name '%1$s'"), name);
        return NULL;
    }

    *listRet = g_steal_pointer(&devs);
    return obj;
}


static virNodeDevicePtr
redfishNodeDeviceLookupByName(virConnectPtr conn,
                              const char *name)
{
    redfishPrivate *priv = conn->privateData;
    g_autoptr(GPtrArray) paths = redfishListMemberPaths(priv, priv->blocksPath);
    size_t i;

    if (!paths)
        return NULL;

    /* Existence is all that is needed here, so avoid reading the block
     * itself: device names are the last segment of the block's path. */
    for (i = 0; i < paths->len; i++) {
        g_autofree char *base = redfishPathBaseName(g_ptr_array_index(paths, i));
        g_autofree char *devname = redfishSanitizeName(base);

        if (STREQ(devname, name))
            return virGetNodeDevice(conn, name);
    }

    virReportError(VIR_ERR_NO_NODE_DEVICE,
                   _("no node device with matching name '%1$s'"), name);
    return NULL;
}


static char *
redfishNodeDeviceGetXMLDesc(virNodeDevicePtr dev,
                            unsigned int flags)
{
    g_autoptr(virNodeDeviceObjList) devs = NULL;
    virNodeDeviceObj *obj;
    char *ret;

    virCheckFlags(0, NULL);

    if (!(obj = redfishNodeDeviceFind(dev->conn, dev->name, &devs)))
        return NULL;

    ret = virNodeDeviceDefFormat(virNodeDeviceObjGetDef(obj), 0);
    virNodeDeviceObjEndAPI(&obj);
    return ret;
}


static int
redfishNodeDeviceNumOfCaps(virNodeDevicePtr dev)
{
    g_autoptr(virNodeDeviceObjList) devs = NULL;
    virNodeDeviceObj *obj;
    virNodeDevCapsDef *caps;
    int n = 0;

    if (!(obj = redfishNodeDeviceFind(dev->conn, dev->name, &devs)))
        return -1;

    for (caps = virNodeDeviceObjGetDef(obj)->caps; caps; caps = caps->next)
        n++;

    virNodeDeviceObjEndAPI(&obj);
    return n;
}


static int
redfishNodeDeviceListCaps(virNodeDevicePtr dev,
                          char **const names,
                          int maxnames)
{
    g_autoptr(virNodeDeviceObjList) devs = NULL;
    virNodeDeviceObj *obj;
    virNodeDevCapsDef *caps;
    int n = 0;

    if (!(obj = redfishNodeDeviceFind(dev->conn, dev->name, &devs)))
        return -1;

    for (caps = virNodeDeviceObjGetDef(obj)->caps; caps && n < maxnames; caps = caps->next)
        names[n++] = g_strdup(virNodeDevCapTypeToString(caps->data.type));

    virNodeDeviceObjEndAPI(&obj);
    return n;
}


static int
redfishNodeDeviceIsActive(virNodeDevicePtr dev)
{
    g_autoptr(virNodeDeviceObjList) devs = NULL;
    virNodeDeviceObj *obj;
    int ret;

    if (!(obj = redfishNodeDeviceFind(dev->conn, dev->name, &devs)))
        return -1;

    ret = virNodeDeviceObjIsActive(obj);
    virNodeDeviceObjEndAPI(&obj);
    return ret;
}


static int
redfishNodeDeviceIsPersistent(virNodeDevicePtr dev G_GNUC_UNUSED)
{
    return 1;
}


/* --- registration -------------------------------------------------------- */

static virHypervisorDriver redfishHypervisorDriver = {
    .name = "Redfish",
    .connectOpen = redfishConnectOpen, /* 12.10.0 */
    .connectClose = redfishConnectClose, /* 12.10.0 */
    .connectGetType = redfishConnectGetType, /* 12.10.0 */
    .connectGetVersion = redfishConnectGetVersion, /* 12.10.0 */
    .connectGetHostname = redfishConnectGetHostname, /* 12.10.0 */
    .connectGetCapabilities = redfishConnectGetCapabilities, /* 12.10.0 */
    .connectListAllDomains = redfishConnectListAllDomains, /* 12.10.0 */
    .connectIsEncrypted = redfishConnectIsEncrypted, /* 12.10.0 */
    .connectIsSecure = redfishConnectIsSecure, /* 12.10.0 */
    .connectIsAlive = redfishConnectIsAlive, /* 12.10.0 */
    .domainLookupByUUID = redfishDomainLookupByUUID, /* 12.10.0 */
    .domainLookupByName = redfishDomainLookupByName, /* 12.10.0 */
    .domainGetState = redfishDomainGetState, /* 12.10.0 */
    .domainGetInfo = redfishDomainGetInfo, /* 12.10.0 */
    .domainGetXMLDesc = redfishDomainGetXMLDesc, /* 12.10.0 */
    .domainIsActive = redfishDomainIsActive, /* 12.10.0 */
    .domainIsPersistent = redfishDomainIsPersistent, /* 12.10.0 */
    .domainIsUpdated = redfishDomainIsUpdated, /* 12.10.0 */
    .domainCreate = redfishDomainCreate, /* 12.10.0 */
    .domainCreateWithFlags = redfishDomainCreateWithFlags, /* 12.10.0 */
    .domainCreateXML = redfishDomainCreateXML, /* 12.10.0 */
    .domainDefineXML = redfishDomainDefineXML, /* 12.10.0 */
    .domainDefineXMLFlags = redfishDomainDefineXMLFlags, /* 12.10.0 */
    .domainUndefine = redfishDomainUndefine, /* 12.10.0 */
    .domainUndefineFlags = redfishDomainUndefineFlags, /* 12.10.0 */
    .domainDestroy = redfishDomainDestroy, /* 12.10.0 */
    .domainDestroyFlags = redfishDomainDestroyFlags, /* 12.10.0 */
    .domainShutdown = redfishDomainShutdown, /* 12.10.0 */
    .domainShutdownFlags = redfishDomainShutdownFlags, /* 12.10.0 */
    .domainReboot = redfishDomainReboot, /* 12.10.0 */
    .domainReset = redfishDomainResetFlags, /* 12.10.0 */
    .domainAttachDevice = redfishDomainAttachDevice, /* 12.10.0 */
    .domainAttachDeviceFlags = redfishDomainAttachDeviceFlags, /* 12.10.0 */
    .domainDetachDevice = redfishDomainDetachDevice, /* 12.10.0 */
    .domainDetachDeviceFlags = redfishDomainDetachDeviceFlags, /* 12.10.0 */
    .connectDomainEventRegisterAny = redfishConnectDomainEventRegisterAny, /* 12.10.0 */
    .connectDomainEventDeregisterAny = redfishConnectDomainEventDeregisterAny, /* 12.10.0 */
    .domainGetAutostart = redfishDomainGetAutostart, /* 12.10.0 */
};


static virNodeDeviceDriver redfishNodeDeviceDriver = {
    .connectListAllNodeDevices = redfishConnectListAllNodeDevices, /* 12.10.0 */
    .nodeNumOfDevices = redfishNodeNumOfDevices, /* 12.10.0 */
    .nodeListDevices = redfishNodeListDevices, /* 12.10.0 */
    .nodeDeviceLookupByName = redfishNodeDeviceLookupByName, /* 12.10.0 */
    .nodeDeviceGetXMLDesc = redfishNodeDeviceGetXMLDesc, /* 12.10.0 */
    .nodeDeviceNumOfCaps = redfishNodeDeviceNumOfCaps, /* 12.10.0 */
    .nodeDeviceListCaps = redfishNodeDeviceListCaps, /* 12.10.0 */
    .nodeDeviceIsActive = redfishNodeDeviceIsActive, /* 12.10.0 */
    .nodeDeviceIsPersistent = redfishNodeDeviceIsPersistent, /* 12.10.0 */
};


static virConnectDriver redfishConnectDriver = {
    .remoteOnly = true,
    .uriSchemes = (const char *[]){ "redfish", NULL },
    .hypervisorDriver = &redfishHypervisorDriver,
    .nodeDeviceDriver = &redfishNodeDeviceDriver,
};


int
redfishRegister(void)
{
    return virRegisterConnectDriver(&redfishConnectDriver, false);
}
