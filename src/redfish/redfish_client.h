/*
 * redfish_client.h: minimal DMTF Redfish REST client
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "internal.h"
#include "virjson.h"

typedef struct _redfishClient redfishClient;

redfishClient *
redfishClientNew(const char *baseURL,
                 bool verifyPeer,
                 unsigned int taskTimeout);

void
redfishClientFree(redfishClient *client);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(redfishClient, redfishClientFree);

int
redfishClientLogin(redfishClient *client,
                   const char *sessionsPath,
                   const char *username,
                   const char *password);

void
redfishClientLogout(redfishClient *client);

virJSONValue *
redfishClientGet(redfishClient *client,
                 const char *path);

int
redfishClientCall(redfishClient *client,
                  const char *method,
                  const char *path,
                  virJSONValue *body,
                  virJSONValue **reply,
                  char **location);

bool
redfishClientIsEncrypted(redfishClient *client);
