/*
 * redfish_conv.h: conversion between Redfish resources and libvirt objects
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "internal.h"
#include "capabilities.h"
#include "domain_conf.h"
#include "node_device_conf.h"
#include "virjson.h"

#define REDFISH_DOMAIN_NAMESPACE_URI "http://libvirt.org/schemas/domain/redfish/1.0"

/* Driver specific domain XML, i.e. the <redfish:composition> element */
typedef struct _redfishDomainNamespaceDef redfishDomainNamespaceDef;
struct _redfishDomainNamespaceDef {
    char **blocks;  /* NULL terminated list of resource block names */
    /* Optional details for each of the blocks, filled in when formatting
     * XML for users. Either is NULL or as long as blocks. */
    char **kinds;
    char **descs;
};

void
redfishDomainNamespaceDefFree(redfishDomainNamespaceDef *nsdef);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(redfishDomainNamespaceDef,
                              redfishDomainNamespaceDefFree);

virDomainXMLOption *
redfishDomainXMLConfInit(void);

virCaps *
redfishCapsInit(void);

char *
redfishPathBaseName(const char *path);

char *
redfishSanitizeName(const char *name);

char *
redfishResourceBlockName(virJSONValue *block);

bool
redfishResourceBlockNeedsParts(virJSONValue *block);

const char *
redfishResourceBlockState(virJSONValue *block);

bool
redfishResourceBlockHasType(virJSONValue *block,
                            const char *type);

bool
redfishSystemIsComposed(virJSONValue *system);

int
redfishSystemGetUUID(virJSONValue *system,
                     unsigned char *uuid);

const char *
redfishSystemGetName(virJSONValue *system);

int
redfishSystemGetState(virJSONValue *system,
                      int *reason);

virDomainDef *
redfishSystemToDomainDef(virJSONValue *system,
                         virDomainXMLOption *xmlopt,
                         int id);

virJSONValue *
redfishDomainDefToComposeRequest(virDomainDef *def,
                                 const char *const *blockPaths);

void
redfishNodeDeviceDefDescribe(virNodeDeviceDef *def,
                             char **kind,
                             char **desc);

virNodeDeviceDef *
redfishResourceBlockToNodeDeviceDef(virJSONValue *block,
                                    virJSONValue **parts,
                                    size_t nparts);
