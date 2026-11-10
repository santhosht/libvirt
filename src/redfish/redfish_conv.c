/*
 * redfish_conv.c: conversion between Redfish resources and libvirt objects
 *
 * The mapping implemented here is:
 *
 *   ComputerSystem (SystemType=Composed)  <->  domain
 *   ResourceBlock                          ->  node device
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <config.h>

#include "redfish_conv.h"
#include "vircrypto.h"
#include "virerror.h"
#include "virlog.h"
#include "virstring.h"
#include "viruuid.h"
#include "virxml.h"

#define VIR_FROM_THIS VIR_FROM_REDFISH

VIR_LOG_INIT("redfish.redfish_conv");


/* --- driver specific domain XML namespace -------------------------------- */

void
redfishDomainNamespaceDefFree(redfishDomainNamespaceDef *nsdef)
{
    if (!nsdef)
        return;

    g_strfreev(nsdef->blocks);
    g_strfreev(nsdef->kinds);
    g_strfreev(nsdef->descs);
    g_free(nsdef);
}


static void
redfishDomainDefNamespaceFree(void *nsdata)
{
    redfishDomainNamespaceDefFree(nsdata);
}


static int
redfishDomainDefNamespaceParse(xmlXPathContextPtr ctxt,
                               void **data)
{
    g_autoptr(redfishDomainNamespaceDef) nsdef = NULL;
    g_autofree xmlNodePtr *nodes = NULL;
    size_t i;
    int n;

    if ((n = virXPathNodeSet("./redfish:composition/redfish:block",
                             ctxt, &nodes)) < 0)
        return -1;

    if (n == 0)
        return 0;

    nsdef = g_new0(redfishDomainNamespaceDef, 1);
    nsdef->blocks = g_new0(char *, n + 1);

    for (i = 0; i < n; i++) {
        g_autofree char *name = virXMLPropString(nodes[i], "name");

        if (!name || !*name) {
            virReportError(VIR_ERR_XML_ERROR, "%s",
                           _("missing 'name' attribute on <redfish:block>"));
            return -1;
        }

        if (g_strv_contains((const char *const *)nsdef->blocks, name)) {
            virReportError(VIR_ERR_XML_ERROR,
                           _("resource block '%1$s' listed more than once"),
                           name);
            return -1;
        }

        nsdef->blocks[i] = g_steal_pointer(&name);
    }

    *data = g_steal_pointer(&nsdef);
    return 0;
}


static int
redfishDomainDefNamespaceFormatXML(virBuffer *buf,
                                   void *nsdata)
{
    redfishDomainNamespaceDef *nsdef = nsdata;
    char **block;

    if (!nsdef->blocks || !nsdef->blocks[0])
        return 0;

    virBufferAddLit(buf, "<redfish:composition>\n");
    virBufferAdjustIndent(buf, 2);
    for (block = nsdef->blocks; *block; block++) {
        size_t i = block - nsdef->blocks;

        virBufferEscapeString(buf, "<redfish:block name='%s'", *block);
        if (nsdef->kinds && nsdef->kinds[i])
            virBufferEscapeString(buf, " type='%s'", nsdef->kinds[i]);
        if (nsdef->descs && nsdef->descs[i] && *nsdef->descs[i])
            virBufferEscapeString(buf, " description='%s'", nsdef->descs[i]);
        virBufferAddLit(buf, "/>\n");
    }
    virBufferAdjustIndent(buf, -2);
    virBufferAddLit(buf, "</redfish:composition>\n");

    return 0;
}


static virXMLNamespace redfishDomainXMLNamespace = {
    .parse = redfishDomainDefNamespaceParse,
    .free = redfishDomainDefNamespaceFree,
    .format = redfishDomainDefNamespaceFormatXML,
    .prefix = "redfish",
    .uri = REDFISH_DOMAIN_NAMESPACE_URI,
};


static int
redfishDomainDefPostParse(virDomainDef *def,
                          unsigned int parseFlags G_GNUC_UNUSED,
                          void *opaque G_GNUC_UNUSED,
                          void *parseOpaque G_GNUC_UNUSED)
{
    def->os.type = VIR_DOMAIN_OSTYPE_HVM;
    if (def->os.arch == VIR_ARCH_NONE)
        def->os.arch = VIR_ARCH_X86_64;

    /* The memory comes from the resource blocks, so the XML need not say
     * how much. libvirt insists on a size, and checks for it after this
     * callback, so give it a placeholder. */
    if (virDomainDefGetMemoryTotal(def) == 0)
        virDomainDefSetMemoryTotal(def, 1024 * 1024);

    return 0;
}


static virDomainDefParserConfig redfishDomainDefParserConfig = {
    .domainPostParseCallback = redfishDomainDefPostParse,
};


virDomainXMLOption *
redfishDomainXMLConfInit(void)
{
    return virDomainXMLOptionNew(&redfishDomainDefParserConfig, NULL,
                                 &redfishDomainXMLNamespace,
                                 NULL, NULL, NULL);
}


virCaps *
redfishCapsInit(void)
{
    g_autoptr(virCaps) caps = NULL;
    virArch arches[] = { VIR_ARCH_X86_64, VIR_ARCH_AARCH64 };
    size_t i;

    /* The "host" is a rack worth of disaggregated hardware rather
     * than a single machine, so there is no meaningful host CPU or
     * NUMA topology to report. */
    if (!(caps = virCapabilitiesNew(VIR_ARCH_X86_64, false, false)))
        return NULL;

    for (i = 0; i < G_N_ELEMENTS(arches); i++) {
        virCapsGuest *guest = virCapabilitiesAddGuest(caps,
                                                      VIR_DOMAIN_OSTYPE_HVM,
                                                      arches[i],
                                                      NULL, NULL, 0, NULL);
        virCapabilitiesAddGuestDomain(guest, VIR_DOMAIN_VIRT_REDFISH,
                                      NULL, NULL, 0, NULL);
    }

    return g_steal_pointer(&caps);
}


/* --- generic helpers ----------------------------------------------------- */

/**
 * redfishPathBaseName:
 *
 * Return the last segment of a Redfish resource path, e.g. "GPU1" for
 * "/redfish/v1/CompositionService/ResourceBlocks/GPU1/".
 */
char *
redfishPathBaseName(const char *path)
{
    g_autofree char *tmp = g_strdup(path);
    char *p;

    while (*tmp && tmp[strlen(tmp) - 1] == '/')
        tmp[strlen(tmp) - 1] = '\0';

    if ((p = strrchr(tmp, '/')))
        return g_strdup(p + 1);

    return g_steal_pointer(&tmp);
}


static const char *
redfishGetPath(virJSONValue *obj)
{
    return virJSONValueObjectGetString(obj, "@odata.id");
}


/* Node device names show up on the virsh command line and in XML, keep
 * them to a conservative character set. */
char *
redfishSanitizeName(const char *name)
{
    char *ret = g_strdup(name);
    char *p;

    for (p = ret; *p; p++) {
        if (!g_ascii_isalnum(*p) && *p != '-' && *p != '_' && *p != '.')
            *p = '_';
    }

    return ret;
}


static int
redfishHashUUID(const char *seed,
                unsigned char *uuid)
{
    unsigned char hash[VIR_CRYPTO_HASH_SIZE_SHA256];

    if (virCryptoHashBuf(VIR_CRYPTO_HASH_SHA256, seed, hash) < 0)
        return -1;

    memcpy(uuid, hash, VIR_UUID_BUFLEN);
    /* RFC 9562 version 8 (custom), variant 1 */
    uuid[6] = (uuid[6] & 0x0f) | 0x80;
    uuid[8] = (uuid[8] & 0x3f) | 0x80;
    return 0;
}


/* --- resource blocks ----------------------------------------------------- */

char *
redfishResourceBlockName(virJSONValue *block)
{
    const char *id = virJSONValueObjectGetString(block, "Id");
    const char *path;

    if (id && *id)
        return redfishSanitizeName(id);

    if ((path = redfishGetPath(block))) {
        g_autofree char *base = redfishPathBaseName(path);
        return redfishSanitizeName(base);
    }

    virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                   _("Redfish resource block has neither Id nor @odata.id"));
    return NULL;
}


const char *
redfishResourceBlockState(virJSONValue *block)
{
    virJSONValue *status = virJSONValueObjectGetObject(block,
                                                       "CompositionStatus");
    bool reserved = false;

    if (!status)
        return NULL;

    if (virJSONValueObjectGetBoolean(status, "Reserved", &reserved) == 0 &&
        reserved)
        return "Reserved";

    return virJSONValueObjectGetString(status, "CompositionState");
}


bool
redfishResourceBlockHasType(virJSONValue *block,
                            const char *type)
{
    virJSONValue *types = virJSONValueObjectGetArray(block,
                                                     "ResourceBlockType");
    size_t i;

    if (!types)
        return false;

    for (i = 0; i < virJSONValueArraySize(types); i++) {
        const char *t = virJSONValueGetString(virJSONValueArrayGet(types, i));
        if (STREQ_NULLABLE(t, type))
            return true;
    }

    return false;
}


typedef enum {
    REDFISH_BLOCK_KIND_UNKNOWN,
    REDFISH_BLOCK_KIND_SYSTEM,
    REDFISH_BLOCK_KIND_STORAGE,
    REDFISH_BLOCK_KIND_PCI,
} redfishBlockKind;


/* What a block is, if its own ResourceBlockType says so. */
static redfishBlockKind
redfishBlockKindFromType(virJSONValue *block)
{
    if (redfishResourceBlockHasType(block, "Compute"))
        return REDFISH_BLOCK_KIND_SYSTEM;
    if (redfishResourceBlockHasType(block, "Storage"))
        return REDFISH_BLOCK_KIND_STORAGE;
    if (redfishResourceBlockHasType(block, "Expansion"))
        return REDFISH_BLOCK_KIND_PCI;
    if (redfishResourceBlockHasType(block, "Memory"))
        return REDFISH_BLOCK_KIND_SYSTEM;

    return REDFISH_BLOCK_KIND_UNKNOWN;
}


/**
 * redfishResourceBlockNeedsParts:
 *
 * Whether the parts (processors, memory, drives) of @block have to be
 * read to tell what kind of device it is. When the block's own type is
 * decisive they only add detail, so listing can skip fetching them.
 */
bool
redfishResourceBlockNeedsParts(virJSONValue *block)
{
    return redfishBlockKindFromType(block) == REDFISH_BLOCK_KIND_UNKNOWN;
}


static bool
redfishPartIsType(virJSONValue *part,
                  const char *type)
{
    const char *otype = virJSONValueObjectGetString(part, "@odata.type");
    g_autofree char *prefix = g_strdup_printf("#%s.", type);

    return otype && STRPREFIX(otype, prefix);
}


static unsigned int
redfishParseHexID(const char *str)
{
    unsigned int val = 0;

    if (str && virStrToLong_ui(str, NULL, 16, &val) < 0)
        val = 0;

    return val & 0xffff;
}


/* Summary of the parts making up a resource block */
typedef struct {
    virJSONValue *firstCPU;
    virJSONValue *firstAccel;
    virJSONValue *firstDrive;
    virJSONValue *firstMemory;
    unsigned int sockets;
    unsigned long long memMiB;
    unsigned long long driveBytes;
} redfishBlockParts;


static void
redfishBlockPartsCollect(virJSONValue **parts,
                         size_t nparts,
                         redfishBlockParts *info)
{
    size_t i;

    for (i = 0; i < nparts; i++) {
        virJSONValue *part = parts[i];

        if (redfishPartIsType(part, "Processor")) {
            const char *ptype = virJSONValueObjectGetString(part,
                                                            "ProcessorType");
            if (!ptype || STREQ(ptype, "CPU")) {
                info->sockets++;
                if (!info->firstCPU)
                    info->firstCPU = part;
            } else if (!info->firstAccel) {
                info->firstAccel = part;
            }
        } else if (redfishPartIsType(part, "Memory")) {
            unsigned long long mib = 0;
            if (virJSONValueObjectGetNumberUlong(part, "CapacityMiB", &mib) == 0)
                info->memMiB += mib;
            if (!info->firstMemory)
                info->firstMemory = part;
        } else if (redfishPartIsType(part, "Drive")) {
            unsigned long long bytes = 0;
            if (virJSONValueObjectGetNumberUlong(part, "CapacityBytes", &bytes) == 0)
                info->driveBytes += bytes;
            if (!info->firstDrive)
                info->firstDrive = part;
        }
    }
}


/**
 * redfishResourceBlockToNodeDeviceDef:
 * @block: the ResourceBlock resource
 * @parts: the Processor, Memory and Drive resources the block links to
 * @nparts: number of items in @parts
 *
 * Map a resource block to a node device. Node device capabilities are
 * modeled after devices visible on a single host, so the mapping is
 * necessarily approximate:
 *
 *  - blocks with accelerators (GPU, FPGA, ...) become 'pci' devices,
 *    carrying vendor/model information but no bus address since they
 *    are not attached to any host until composed
 *  - storage blocks become 'storage' devices
 *  - compute and memory blocks become 'system' devices
 *
 * The Redfish URI of the block is reported as the device <path>.
 */
virNodeDeviceDef *
redfishResourceBlockToNodeDeviceDef(virJSONValue *block,
                                    virJSONValue **parts,
                                    size_t nparts)
{
    g_autoptr(virNodeDeviceDef) def = g_new0(virNodeDeviceDef, 1);
    virNodeDevCapsDef *caps = g_new0(virNodeDevCapsDef, 1);
    virNodeDevCapData *data = &caps->data;
    redfishBlockParts info = { 0 };
    redfishBlockKind kind;
    const char *path = redfishGetPath(block);

    def->caps = caps;

    if (!(def->name = redfishResourceBlockName(block)))
        return NULL;
    def->sysfs_path = g_strdup(path);
    def->driver = g_strdup("redfish");

    redfishBlockPartsCollect(parts, nparts, &info);

    kind = redfishBlockKindFromType(block);
    if (kind == REDFISH_BLOCK_KIND_UNKNOWN) {
        if (info.firstAccel)
            kind = REDFISH_BLOCK_KIND_PCI;
        else if (info.firstDrive)
            kind = REDFISH_BLOCK_KIND_STORAGE;
        else
            kind = REDFISH_BLOCK_KIND_SYSTEM;
    }

    if (kind == REDFISH_BLOCK_KIND_PCI) {
        virJSONValue *accel = info.firstAccel;
        virJSONValue *procid = NULL;

        data->type = VIR_NODE_DEV_CAP_PCI_DEV;
        data->pci_dev.klass = -1;
        data->pci_dev.numa_node = -1;
        if (accel) {
            data->pci_dev.vendor_name =
                g_strdup(virJSONValueObjectGetString(accel, "Manufacturer"));
            data->pci_dev.product_name =
                g_strdup(virJSONValueObjectGetString(accel, "Model"));
            if ((procid = virJSONValueObjectGetObject(accel, "ProcessorId"))) {
                data->pci_dev.vendor =
                    redfishParseHexID(virJSONValueObjectGetString(procid,
                                                                  "VendorId"));
                data->pci_dev.product =
                    redfishParseHexID(virJSONValueObjectGetString(procid,
                                                                  "IdentificationRegisters"));
            }
        }
    } else if (kind == REDFISH_BLOCK_KIND_STORAGE) {
        virJSONValue *drive = info.firstDrive;

        data->type = VIR_NODE_DEV_CAP_STORAGE;
        data->storage.drive_type = g_strdup("disk");
        data->storage.size = info.driveBytes;
        if (drive) {
            data->storage.bus =
                g_strdup(virJSONValueObjectGetString(drive, "Protocol"));
            data->storage.model =
                g_strdup(virJSONValueObjectGetString(drive, "Model"));
            data->storage.vendor =
                g_strdup(virJSONValueObjectGetString(drive, "Manufacturer"));
            data->storage.serial =
                g_strdup(virJSONValueObjectGetString(drive, "SerialNumber"));
        }
    } else {
        virNodeDevCapSystem *sys = &data->system;

        data->type = VIR_NODE_DEV_CAP_SYSTEM;
        if (info.firstCPU) {
            const char *model = virJSONValueObjectGetString(info.firstCPU,
                                                            "Model");
            sys->product_name = g_strdup_printf("%ux %s, %llu GiB",
                                                info.sockets,
                                                NULLSTR_EMPTY(model),
                                                info.memMiB / 1024);
            sys->hardware.vendor_name =
                g_strdup(virJSONValueObjectGetString(info.firstCPU,
                                                     "Manufacturer"));
        } else if (info.firstMemory) {
            const char *mtype = virJSONValueObjectGetString(info.firstMemory,
                                                            "MemoryDeviceType");
            if (!mtype)
                mtype = virJSONValueObjectGetString(info.firstMemory,
                                                    "MemoryType");
            sys->product_name = g_strdup_printf("%s memory, %llu GiB",
                                                NULLSTR_EMPTY(mtype),
                                                info.memMiB / 1024);
        } else {
            sys->product_name =
                g_strdup(virJSONValueObjectGetString(block, "Name"));
        }
        if (redfishHashUUID(NULLSTR_EMPTY(path), sys->hardware.uuid) < 0)
            return NULL;
    }

    return g_steal_pointer(&def);
}


/**
 * redfishNodeDeviceDefDescribe:
 *
 * Short, human readable summary of what a resource block is, for use in
 * the domain XML, e.g. type 'pci', "NVIDIA Corporation H100 80GB HBM3".
 */
void
redfishNodeDeviceDefDescribe(virNodeDeviceDef *def,
                             char **kind,
                             char **desc)
{
    virNodeDevCapData *data = &def->caps->data;

    *kind = g_strdup(virNodeDevCapTypeToString(data->type));

    *desc = NULL;
    if (data->type == VIR_NODE_DEV_CAP_PCI_DEV) {
        *desc = g_strdup_printf("%s %s",
                                NULLSTR_EMPTY(data->pci_dev.vendor_name),
                                NULLSTR_EMPTY(data->pci_dev.product_name));
    } else if (data->type == VIR_NODE_DEV_CAP_STORAGE) {
        *desc = g_strdup_printf("%s %s, %llu GiB",
                                NULLSTR_EMPTY(data->storage.vendor),
                                NULLSTR_EMPTY(data->storage.model),
                                data->storage.size >> 30);
    } else if (data->type == VIR_NODE_DEV_CAP_SYSTEM) {
        *desc = g_strdup(data->system.product_name);
    }

    if (*desc)
        g_strstrip(*desc);
}


/* --- composed systems ---------------------------------------------------- */

bool
redfishSystemIsComposed(virJSONValue *system)
{
    return STREQ_NULLABLE(virJSONValueObjectGetString(system, "SystemType"),
                          "Composed");
}


const char *
redfishSystemGetName(virJSONValue *system)
{
    const char *name = virJSONValueObjectGetString(system, "Name");

    if (!name || !*name)
        name = virJSONValueObjectGetString(system, "Id");

    return name;
}


/**
 * redfishSystemGetUUID:
 *
 * Use the system's UUID when it reports a valid one. Otherwise derive
 * a stable UUID from its resource path, since libvirt requires every
 * domain to have one.
 */
int
redfishSystemGetUUID(virJSONValue *system,
                     unsigned char *uuid)
{
    const char *str = virJSONValueObjectGetString(system, "UUID");
    const char *path;

    if (str && virUUIDParse(str, uuid) == 0 && virUUIDIsValid(uuid))
        return 0;

    if (!(path = redfishGetPath(system))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Redfish system has neither a UUID nor an @odata.id"));
        return -1;
    }

    return redfishHashUUID(path, uuid);
}


int
redfishSystemGetState(virJSONValue *system,
                      int *reason)
{
    const char *power = virJSONValueObjectGetString(system, "PowerState");
    int dummy;

    if (!reason)
        reason = &dummy;

    if (STREQ_NULLABLE(power, "On")) {
        *reason = VIR_DOMAIN_RUNNING_BOOTED;
        return VIR_DOMAIN_RUNNING;
    }
    if (STREQ_NULLABLE(power, "PoweringOn")) {
        *reason = VIR_DOMAIN_RUNNING_BOOTED;
        return VIR_DOMAIN_RUNNING;
    }
    if (STREQ_NULLABLE(power, "PoweringOff")) {
        *reason = VIR_DOMAIN_SHUTDOWN_USER;
        return VIR_DOMAIN_SHUTDOWN;
    }
    if (STREQ_NULLABLE(power, "Paused")) {
        *reason = VIR_DOMAIN_PAUSED_USER;
        return VIR_DOMAIN_PAUSED;
    }
    if (STREQ_NULLABLE(power, "Off")) {
        *reason = VIR_DOMAIN_SHUTOFF_UNKNOWN;
        return VIR_DOMAIN_SHUTOFF;
    }

    *reason = VIR_DOMAIN_NOSTATE_UNKNOWN;
    return VIR_DOMAIN_NOSTATE;
}


static const char *
redfishBootTargetFromDev(virDomainBootOrder dev)
{
    switch (dev) {
    case VIR_DOMAIN_BOOT_NET:
        return "Pxe";
    case VIR_DOMAIN_BOOT_CDROM:
        return "Cd";
    case VIR_DOMAIN_BOOT_DISK:
        /* Booting from disk is what every system does by default, and
         * libvirt fills it in even when the XML has no <boot>, so
         * don't turn it into an explicit override. */
    case VIR_DOMAIN_BOOT_FLOPPY:
    case VIR_DOMAIN_BOOT_LAST:
        break;
    }

    return NULL;
}


static int
redfishBootDevFromTarget(const char *target)
{
    if (STREQ_NULLABLE(target, "Pxe") || STREQ_NULLABLE(target, "UefiHttp"))
        return VIR_DOMAIN_BOOT_NET;
    if (STREQ_NULLABLE(target, "Cd"))
        return VIR_DOMAIN_BOOT_CDROM;
    if (STREQ_NULLABLE(target, "Hdd"))
        return VIR_DOMAIN_BOOT_DISK;

    return -1;
}


/**
 * redfishSystemToDomainDef:
 * @system: a ComputerSystem resource
 * @xmlopt: the driver's XML options
 * @id: domain ID to report, -1 for inactive domains
 *
 * Build a domain definition describing a composed system. CPU and
 * memory reflect what the composed hardware actually provides, and the
 * resource blocks it is made of are listed in <redfish:composition>.
 */
virDomainDef *
redfishSystemToDomainDef(virJSONValue *system,
                         virDomainXMLOption *xmlopt,
                         int id)
{
    g_autoptr(virDomainDef) def = NULL;
    virJSONValue *procs;
    virJSONValue *mem;
    virJSONValue *boot;
    virJSONValue *links;
    virJSONValue *blocks;
    const char *desc;
    unsigned int ncpus = 0;
    double memGiB = 0;
    size_t i;

    if (!(def = virDomainDefNew(xmlopt)))
        return NULL;

    def->virtType = VIR_DOMAIN_VIRT_REDFISH;
    def->id = id;
    def->name = g_strdup(redfishSystemGetName(system));
    if (!def->name) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Redfish system has neither a Name nor an Id"));
        return NULL;
    }
    if (redfishSystemGetUUID(system, def->uuid) < 0)
        return NULL;

    if ((desc = virJSONValueObjectGetString(system, "Description")) && *desc)
        def->description = g_strdup(desc);

    if ((procs = virJSONValueObjectGetObject(system, "ProcessorSummary"))) {
        if (virJSONValueObjectGetNumberUint(procs, "LogicalProcessorCount",
                                            &ncpus) < 0 || ncpus == 0) {
            if (virJSONValueObjectGetNumberUint(procs, "CoreCount",
                                                &ncpus) < 0 || ncpus == 0) {
                if (virJSONValueObjectGetNumberUint(procs, "Count",
                                                    &ncpus) < 0)
                    ncpus = 0;
            }
        }
    }
    if (ncpus == 0)
        ncpus = 1;
    if (virDomainDefSetVcpusMax(def, ncpus, xmlopt) < 0 ||
        virDomainDefSetVcpus(def, ncpus) < 0)
        return NULL;

    if ((mem = virJSONValueObjectGetObject(system, "MemorySummary")) &&
        virJSONValueObjectGetNumberDouble(mem, "TotalSystemMemoryGiB",
                                          &memGiB) == 0 && memGiB > 0) {
        unsigned long long kib = memGiB * 1024 * 1024;
        virDomainDefSetMemoryTotal(def, kib);
        def->mem.cur_balloon = kib;
    }

    def->os.type = VIR_DOMAIN_OSTYPE_HVM;
    def->os.arch = VIR_ARCH_X86_64;

    if ((boot = virJSONValueObjectGetObject(system, "Boot"))) {
        const char *enabled = virJSONValueObjectGetString(boot,
                                                          "BootSourceOverrideEnabled");
        int dev = redfishBootDevFromTarget(virJSONValueObjectGetString(boot,
                                                                       "BootSourceOverrideTarget"));

        if (dev >= 0 && enabled && STRNEQ(enabled, "Disabled")) {
            def->os.bootDevs[0] = dev;
            def->os.nBootDevs = 1;
        }
    }

    if ((links = virJSONValueObjectGetObject(system, "Links")) &&
        (blocks = virJSONValueObjectGetArray(links, "ResourceBlocks")) &&
        virJSONValueArraySize(blocks) > 0) {
        g_autoptr(redfishDomainNamespaceDef) nsdef = g_new0(redfishDomainNamespaceDef, 1);
        size_t n = virJSONValueArraySize(blocks);
        size_t j = 0;

        nsdef->blocks = g_new0(char *, n + 1);
        for (i = 0; i < n; i++) {
            const char *path = redfishGetPath(virJSONValueArrayGet(blocks, i));
            g_autofree char *base = NULL;

            if (!path)
                continue;
            base = redfishPathBaseName(path);
            nsdef->blocks[j++] = redfishSanitizeName(base);
        }

        def->ns = *virDomainXMLOptionGetNamespace(xmlopt);
        def->namespaceData = g_steal_pointer(&nsdef);
    }

    return g_steal_pointer(&def);
}


/**
 * redfishDomainDefToComposeRequest:
 * @def: domain definition
 * @blockPaths: NULL terminated list of resource block URIs
 *
 * Build the body of a "specific composition" request, i.e. a POST to
 * the ComputerSystem collection (DSP0268, CompositionService).
 */
virJSONValue *
redfishDomainDefToComposeRequest(virDomainDef *def,
                                 const char *const *blockPaths)
{
    g_autoptr(virJSONValue) req = NULL;
    g_autoptr(virJSONValue) links = NULL;
    g_autoptr(virJSONValue) blocks = virJSONValueNewArray();
    g_autoptr(virJSONValue) boot = NULL;
    const char *target = NULL;
    const char *const *p;

    for (p = blockPaths; p && *p; p++) {
        g_autoptr(virJSONValue) ref = NULL;

        if (virJSONValueObjectAdd(&ref, "s:@odata.id", *p, NULL) < 0 ||
            virJSONValueArrayAppend(blocks, &ref) < 0)
            return NULL;
    }

    if (virJSONValueObjectAdd(&links, "a:ResourceBlocks", &blocks, NULL) < 0)
        return NULL;

    if (def->os.nBootDevs > 0)
        target = redfishBootTargetFromDev(def->os.bootDevs[0]);

    if (target &&
        virJSONValueObjectAdd(&boot,
                              "s:BootSourceOverrideTarget", target,
                              "s:BootSourceOverrideEnabled", "Continuous",
                              NULL) < 0)
        return NULL;

    if (virJSONValueObjectAdd(&req,
                              "s:Name", def->name,
                              "S:Description", def->description,
                              "a:Links", &links,
                              "A:Boot", &boot,
                              NULL) < 0)
        return NULL;

    return g_steal_pointer(&req);
}
