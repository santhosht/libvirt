/*
 * redfish_client.c: minimal DMTF Redfish REST client
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <config.h>

#include <curl/curl.h>

#include "redfish_client.h"
#include "virbuffer.h"
#include "virerror.h"
#include "virlog.h"
#include "virstring.h"
#include "virsecureerase.h"
#include "virthread.h"
#include "virtime.h"

#define VIR_FROM_THIS VIR_FROM_REDFISH

VIR_LOG_INIT("redfish.redfish_client");

/* Timeout for a single HTTP request, as opposed to a whole task */
#define REDFISH_REQUEST_TIMEOUT 60L
#define REDFISH_MAX_REDIRECTS 5
#define REDFISH_TASK_POLL_MIN 1
#define REDFISH_TASK_POLL_MAX 10

struct _redfishClient {
    virMutex lock;
    CURL *handle;
    char *baseURL;
    bool verifyPeer;
    unsigned int taskTimeout; /* seconds */

    /* Exactly one of token or username/password is used for auth */
    char *token;
    char *sessionPath;
    char *username;
    char *password;
};

typedef struct {
    virBuffer body;
    char *location;
    char *token;
    unsigned int retryAfter;
} redfishResponse;

static void
redfishResponseClear(redfishResponse *resp)
{
    virBufferFreeAndReset(&resp->body);
    g_clear_pointer(&resp->location, g_free);
    g_clear_pointer(&resp->token, g_free);
}


static size_t
redfishWriteCallback(char *data, size_t size, size_t nmemb, void *opaque)
{
    redfishResponse *resp = opaque;
    size_t len = size * nmemb;

    virBufferAdd(&resp->body, data, len);
    return len;
}


static char *
redfishHeaderValue(const char *line, size_t len, const char *name)
{
    size_t nlen = strlen(name);
    const char *val;
    const char *end = line + len;

    if (len <= nlen + 1 || g_ascii_strncasecmp(line, name, nlen) != 0 ||
        line[nlen] != ':')
        return NULL;

    val = line + nlen + 1;
    while (val < end && g_ascii_isspace(*val))
        val++;
    while (end > val && g_ascii_isspace(end[-1]))
        end--;

    return g_strndup(val, end - val);
}


static size_t
redfishHeaderCallback(char *data, size_t size, size_t nmemb, void *opaque)
{
    redfishResponse *resp = opaque;
    size_t len = size * nmemb;
    char *val;

    if ((val = redfishHeaderValue(data, len, "Location"))) {
        g_free(resp->location);
        resp->location = val;
    } else if ((val = redfishHeaderValue(data, len, "X-Auth-Token"))) {
        g_free(resp->token);
        resp->token = val;
    } else if ((val = redfishHeaderValue(data, len, "Retry-After"))) {
        ignore_value(virStrToLong_ui(val, NULL, 10, &resp->retryAfter));
        g_free(val);
    }

    return len;
}


redfishClient *
redfishClientNew(const char *baseURL,
                 bool verifyPeer,
                 unsigned int taskTimeout)
{
    redfishClient *client = g_new0(redfishClient, 1);

    if (virMutexInit(&client->lock) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not initialize Redfish client mutex"));
        g_free(client);
        return NULL;
    }

    client->baseURL = g_strdup(baseURL);
    client->verifyPeer = verifyPeer;
    client->taskTimeout = taskTimeout;

    if (!(client->handle = curl_easy_init())) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not initialize CURL"));
        redfishClientFree(client);
        return NULL;
    }

    return client;
}


void
redfishClientFree(redfishClient *client)
{
    if (!client)
        return;

    if (client->handle)
        curl_easy_cleanup(client->handle);
    g_free(client->baseURL);
    g_free(client->token);
    g_free(client->sessionPath);
    g_free(client->username);
    virSecureEraseString(client->password);
    g_free(client->password);
    virMutexDestroy(&client->lock);
    g_free(client);
}


bool
redfishClientIsEncrypted(redfishClient *client)
{
    return STRPREFIX(client->baseURL, "https://");
}


/*
 * Redfish returns resource locations either as absolute paths or as
 * full URLs. Normalize to a path relative to the service, rejecting
 * URLs pointing at a different host.
 */
static char *
redfishClientNormalizePath(redfishClient *client,
                           const char *location)
{
    if (!location || !*location)
        return NULL;

    if (*location == '/')
        return g_strdup(location);

    if (STRPREFIX(location, client->baseURL) &&
        location[strlen(client->baseURL)] == '/')
        return g_strdup(location + strlen(client->baseURL));

    virReportError(VIR_ERR_INTERNAL_ERROR,
                   _("Redfish service returned unexpected location '%1$s'"),
                   location);
    return NULL;
}


/*
 * Perform a single HTTP request. Transport failures are reported as
 * errors, HTTP level failures are left to the caller.
 */
static int
redfishClientPerform(redfishClient *client,
                     const char *method,
                     const char *path,
                     const char *body,
                     long *status,
                     redfishResponse *resp)
{
    g_autofree char *url = NULL;
    g_autofree char *tokenHeader = NULL;
    struct curl_slist *headers = NULL;
    char errbuf[CURL_ERROR_SIZE] = "";
    CURLcode rc;
    int ret = -1;

    if (!path || *path != '/') {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Invalid Redfish resource path '%1$s'"),
                       NULLSTR(path));
        return -1;
    }

    url = g_strdup_printf("%s%s", client->baseURL, path);
    VIR_DEBUG("%s %s", method, url);

    curl_easy_reset(client->handle);
    curl_easy_setopt(client->handle, CURLOPT_URL, url);
    curl_easy_setopt(client->handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(client->handle, CURLOPT_TIMEOUT, REDFISH_REQUEST_TIMEOUT);
    curl_easy_setopt(client->handle, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(client->handle, CURLOPT_SSL_VERIFYPEER,
                     client->verifyPeer ? 1L : 0L);
    curl_easy_setopt(client->handle, CURLOPT_SSL_VERIFYHOST,
                     client->verifyPeer ? 2L : 0L);
    curl_easy_setopt(client->handle, CURLOPT_WRITEFUNCTION,
                     redfishWriteCallback);
    curl_easy_setopt(client->handle, CURLOPT_WRITEDATA, resp);
    curl_easy_setopt(client->handle, CURLOPT_HEADERFUNCTION,
                     redfishHeaderCallback);
    curl_easy_setopt(client->handle, CURLOPT_HEADERDATA, resp);

    if (STREQ(method, "GET")) {
        curl_easy_setopt(client->handle, CURLOPT_HTTPGET, 1L);
    } else {
        curl_easy_setopt(client->handle, CURLOPT_CUSTOMREQUEST, method);
        curl_easy_setopt(client->handle, CURLOPT_POSTFIELDS, body ? body : "");
    }

    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, "OData-Version: 4.0");
    if (body)
        headers = curl_slist_append(headers,
                                    "Content-Type: application/json; charset=utf-8");

    if (client->token) {
        tokenHeader = g_strdup_printf("X-Auth-Token: %s", client->token);
        headers = curl_slist_append(headers, tokenHeader);
    } else if (client->username) {
        curl_easy_setopt(client->handle, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
        curl_easy_setopt(client->handle, CURLOPT_USERNAME, client->username);
        curl_easy_setopt(client->handle, CURLOPT_PASSWORD, client->password);
    }
    curl_easy_setopt(client->handle, CURLOPT_HTTPHEADER, headers);

    rc = curl_easy_perform(client->handle);
    if (rc != CURLE_OK) {
        virReportError(VIR_ERR_OPERATION_FAILED,
                       _("Redfish request %1$s %2$s failed: %3$s"),
                       method, url,
                       *errbuf ? errbuf : curl_easy_strerror(rc));
        goto cleanup;
    }

    curl_easy_getinfo(client->handle, CURLINFO_RESPONSE_CODE, status);
    VIR_DEBUG("%s %s -> %ld", method, url, *status);
    ret = 0;

 cleanup:
    /* Don't leave pointers to stack data inside the handle */
    curl_easy_setopt(client->handle, CURLOPT_ERRORBUFFER, NULL);
    curl_easy_setopt(client->handle, CURLOPT_HTTPHEADER, NULL);
    curl_slist_free_all(headers);
    return ret;
}


static virJSONValue *
redfishResponseParse(redfishResponse *resp,
                     const char *method,
                     const char *path,
                     bool required)
{
    const char *content = virBufferCurrentContent(&resp->body);
    virJSONValue *json;

    if (!content || !*content)
        return NULL;

    if (!(json = virJSONValueFromString(content))) {
        /* The body of a successful reply to something other than a read
         * is optional, and some services put plain text in it. The
         * status says what happened, so don't turn it into a failure. */
        if (required)
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Malformed JSON in reply to Redfish request %1$s %2$s"),
                           method, path);
        else
            VIR_DEBUG("Ignoring body which is not JSON in reply to %s %s",
                      method, path);
        return NULL;
    }

    return json;
}


/*
 * Turn a Redfish error object (DSP0266 section 9.6) into a message.
 */
static const char *
redfishErrorMessage(virJSONValue *reply)
{
    virJSONValue *err;
    virJSONValue *info;
    virJSONValue *msg;
    const char *str;

    if (!reply)
        return NULL;

    /* Not every service follows the Redfish error format; some answer
     * {"error": "text"} */
    if ((str = virJSONValueObjectGetString(reply, "error")))
        return str;

    if (!(err = virJSONValueObjectGetObject(reply, "error")))
        return NULL;

    if ((info = virJSONValueObjectGetArray(err, "@Message.ExtendedInfo")) &&
        virJSONValueArraySize(info) > 0 &&
        (msg = virJSONValueArrayGet(info, 0)) &&
        (str = virJSONValueObjectGetString(msg, "Message")))
        return str;

    return virJSONValueObjectGetString(err, "message");
}


static void
redfishReportHTTPError(const char *method,
                       const char *path,
                       long status,
                       virJSONValue *reply)
{
    const char *msg = redfishErrorMessage(reply);
    int code = VIR_ERR_OPERATION_FAILED;

    if (status == 401 || status == 403)
        code = VIR_ERR_AUTH_FAILED;

    virReportError(code,
                   _("Redfish request %1$s %2$s failed with HTTP status %3$ld: %4$s"),
                   method, path, status, msg ? msg : _("no details"));
}


static int
redfishClientRequestLocked(redfishClient *client,
                           const char *method,
                           const char *path,
                           const char *body,
                           long *status,
                           virJSONValue **reply,
                           redfishResponse *resp)
{
    g_autoptr(virJSONValue) json = NULL;
    g_autofree char *redirected = NULL;
    size_t hops;

    if (redfishClientPerform(client, method, path, body, status, resp) < 0)
        return -1;

    /* Services commonly redirect between "/redfish/v1" and
     * "/redfish/v1/". Follow redirects by hand, staying on the same
     * service, so that credentials are never sent anywhere else. */
    for (hops = 0; hops < REDFISH_MAX_REDIRECTS &&
         *status >= 300 && *status < 400 && resp->location; hops++) {
        g_autofree char *next = redfishClientNormalizePath(client, resp->location);

        if (!next)
            return -1;

        VIR_DEBUG("%s %s redirected to %s", method, path, next);
        g_free(redirected);
        redirected = g_steal_pointer(&next);
        path = redirected;
        redfishResponseClear(resp);
        if (redfishClientPerform(client, method, path, body, status, resp) < 0)
            return -1;
    }

    /* Error replies get their text from the body when it can be read.
     * Reads need a body, anything else may do without. */
    json = redfishResponseParse(resp, method, path,
                                *status < 400 && STREQ(method, "GET"));
    if (!json && virBufferUse(&resp->body) > 0 && *status < 400 &&
        STREQ(method, "GET"))
        return -1;

    if (*status >= 400) {
        redfishReportHTTPError(method, path, *status, json);
        return -1;
    }

    if (reply)
        *reply = g_steal_pointer(&json);
    return 0;
}


/*
 * Poll a task monitor (DSP0266 section 13.2) until the asynchronous
 * operation is done. On completion @reply holds the final response
 * body and @location the URI of a created resource, if any.
 */
static int
redfishClientWaitTaskLocked(redfishClient *client,
                            const char *method,
                            const char *path,
                            const char *monitor,
                            unsigned int retryAfter,
                            virJSONValue **reply,
                            char **location)
{
    unsigned long long start;
    unsigned long long now;

    if (virTimeMillisNow(&start) < 0)
        return -1;

    while (true) {
        redfishResponse resp = { 0 };
        g_autoptr(virJSONValue) json = NULL;
        unsigned int delay = CLAMP(retryAfter, REDFISH_TASK_POLL_MIN,
                                   REDFISH_TASK_POLL_MAX);
        long status = 0;
        const char *state;

        if (virTimeMillisNow(&now) < 0)
            return -1;
        if (now - start >= client->taskTimeout * 1000ULL) {
            virReportError(VIR_ERR_OPERATION_TIMEOUT,
                           _("Timed out after %1$u seconds waiting for Redfish task %2$s (%3$s %4$s)"),
                           client->taskTimeout, monitor, method, path);
            return -1;
        }

        g_usleep(delay * G_USEC_PER_SEC);

        if (redfishClientRequestLocked(client, "GET", monitor, NULL,
                                       &status, &json, &resp) < 0) {
            redfishResponseClear(&resp);
            return -1;
        }

        if (status == 202) {
            retryAfter = resp.retryAfter;
            redfishResponseClear(&resp);
            continue;
        }

        /* Some services answer with the final Task resource rather
         * than with the result of the operation. */
        if (json && (state = virJSONValueObjectGetString(json, "TaskState")) &&
            virJSONValueObjectHasKey(json, "TaskMonitor")) {
            if (STRNEQ(state, "Completed")) {
                const char *msg = NULL;
                virJSONValue *msgs = virJSONValueObjectGetArray(json, "Messages");
                if (msgs && virJSONValueArraySize(msgs) > 0)
                    msg = virJSONValueObjectGetString(virJSONValueArrayGet(msgs, 0),
                                                      "Message");
                virReportError(VIR_ERR_OPERATION_FAILED,
                               _("Redfish task for %1$s %2$s ended in state %3$s: %4$s"),
                               method, path, state, msg ? msg : _("no details"));
                redfishResponseClear(&resp);
                return -1;
            }
        }

        if (location && resp.location && STRNEQ(resp.location, monitor)) {
            if (!(*location = redfishClientNormalizePath(client, resp.location))) {
                redfishResponseClear(&resp);
                return -1;
            }
        }
        redfishResponseClear(&resp);

        if (reply)
            *reply = g_steal_pointer(&json);
        return 0;
    }
}


/**
 * redfishClientCall:
 * @client: the client
 * @method: HTTP method
 * @path: absolute resource path, e.g. "/redfish/v1/Systems"
 * @body: optional request body
 * @reply: optional, filled with the parsed response body (or NULL)
 * @location: optional, filled with the normalized Location header
 *
 * Perform a request, transparently waiting for asynchronous operations
 * reported with "202 Accepted" to complete.
 *
 * Returns 0 on success, -1 with an error reported otherwise.
 */
int
redfishClientCall(redfishClient *client,
                  const char *method,
                  const char *path,
                  virJSONValue *body,
                  virJSONValue **reply,
                  char **location)
{
    VIR_LOCK_GUARD lock = virLockGuardLock(&client->lock);
    redfishResponse resp = { 0 };
    g_autofree char *bodystr = NULL;
    g_autofree char *monitor = NULL;
    g_autoptr(virJSONValue) json = NULL;
    long status = 0;
    int ret = -1;

    if (reply)
        *reply = NULL;
    if (location)
        *location = NULL;

    if (body && !(bodystr = virJSONValueToString(body, false)))
        return -1;

    if (redfishClientRequestLocked(client, method, path, bodystr,
                                   &status, &json, &resp) < 0)
        goto cleanup;

    if (status == 202) {
        if (!resp.location) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Redfish service accepted %1$s %2$s without providing a task monitor"),
                           method, path);
            goto cleanup;
        }
        if (!(monitor = redfishClientNormalizePath(client, resp.location)))
            goto cleanup;

        VIR_DEBUG("%s %s is asynchronous, polling task monitor %s",
                  method, path, monitor);
        ret = redfishClientWaitTaskLocked(client, method, path, monitor,
                                          resp.retryAfter, reply, location);
        goto cleanup;
    }

    if (location && resp.location &&
        !(*location = redfishClientNormalizePath(client, resp.location)))
        goto cleanup;

    if (reply)
        *reply = g_steal_pointer(&json);
    ret = 0;

 cleanup:
    redfishResponseClear(&resp);
    return ret;
}


virJSONValue *
redfishClientGet(redfishClient *client,
                 const char *path)
{
    virJSONValue *reply = NULL;

    if (redfishClientCall(client, "GET", path, NULL, &reply, NULL) < 0)
        return NULL;

    if (!reply || !virJSONValueIsObject(reply)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Redfish resource %1$s is not a JSON object"), path);
        virJSONValueFree(reply);
        return NULL;
    }

    return reply;
}


/**
 * redfishClientLogin:
 *
 * Create a Redfish session (DSP0266 section 13.3.4). Services that do
 * not implement the session service are accessed with HTTP Basic
 * authentication instead.
 */
int
redfishClientLogin(redfishClient *client,
                   const char *sessionsPath,
                   const char *username,
                   const char *password)
{
    VIR_LOCK_GUARD lock = virLockGuardLock(&client->lock);
    g_autoptr(virJSONValue) body = NULL;
    g_autofree char *bodystr = NULL;
    redfishResponse resp = { 0 };
    long status = 0;
    int ret = -1;

    if (sessionsPath) {
        if (virJSONValueObjectAdd(&body,
                                  "s:UserName", username,
                                  "s:Password", password,
                                  NULL) < 0)
            return -1;

        if (!(bodystr = virJSONValueToString(body, false)))
            return -1;

        if (redfishClientPerform(client, "POST", sessionsPath, bodystr,
                                 &status, &resp) < 0)
            goto cleanup;

        if ((status == 200 || status == 201) && resp.token) {
            client->token = g_steal_pointer(&resp.token);
            if (resp.location)
                client->sessionPath = redfishClientNormalizePath(client,
                                                                 resp.location);
            VIR_DEBUG("Created Redfish session %s", NULLSTR(client->sessionPath));
            ret = 0;
            goto cleanup;
        }

        if (status == 401 || status == 403 || status == 400) {
            g_autoptr(virJSONValue) json = redfishResponseParse(&resp, "POST",
                                                                sessionsPath, false);
            const char *msg = redfishErrorMessage(json);
            virReportError(VIR_ERR_AUTH_FAILED,
                           _("Redfish login as '%1$s' failed: %2$s"),
                           username, msg ? msg : _("invalid credentials"));
            goto cleanup;
        }

        VIR_DEBUG("Session creation returned HTTP %ld, using Basic auth",
                  status);
    }

    client->username = g_strdup(username);
    client->password = g_strdup(password);
    ret = 0;

 cleanup:
    virSecureEraseString(bodystr);
    redfishResponseClear(&resp);
    return ret;
}


void
redfishClientLogout(redfishClient *client)
{
    VIR_LOCK_GUARD lock = virLockGuardLock(&client->lock);
    redfishResponse resp = { 0 };
    virErrorPtr orig_err;
    long status;

    if (!client->sessionPath)
        return;

    virErrorPreserveLast(&orig_err);
    if (redfishClientPerform(client, "DELETE", client->sessionPath, NULL,
                             &status, &resp) == 0 && status >= 400)
        VIR_WARN("Failed to delete Redfish session %s: HTTP %ld",
                 client->sessionPath, status);
    virErrorRestore(&orig_err);
    redfishResponseClear(&resp);

    g_clear_pointer(&client->sessionPath, g_free);
    g_clear_pointer(&client->token, g_free);
}
