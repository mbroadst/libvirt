/*
 * secret_conf.h: internal <secret> XML handling API
 *
 * Copyright (C) 2009 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
 *
 * Red Hat Author: Miloslav Trmač <mitr@redhat.com>
 */

#ifndef __VIR_SECRET_CONF_H__
#define __VIR_SECRET_CONF_H__

#include "internal.h"
#include "util.h"

#define virSecretReportError(conn, code, fmt...)                \
    virReportErrorHelper(conn, VIR_FROM_SECRET, code, __FILE__, \
                         __FUNCTION__, __LINE__, fmt)

enum virSecretUsageType {
    VIR_SECRET_USAGE_TYPE_NONE = 0, /* default when zero-initialized */
    VIR_SECRET_USAGE_TYPE_VOLUME,

    VIR_SECRET_USAGE_TYPE_LAST
};
VIR_ENUM_DECL(virSecretUsageType)

typedef struct _virSecretDef virSecretDef;
typedef virSecretDef *virSecretDefPtr;
struct _virSecretDef {
    unsigned ephemeral : 1;
    unsigned private : 1;
    char *id;                   /* May be NULL */
    char *description;          /* May be NULL */
    int usage_type;
    union {
        char *volume;               /* May be NULL */
    } usage;
};

void virSecretDefFree(virSecretDefPtr def);
virSecretDefPtr virSecretDefParseString(virConnectPtr conn, const char *xml);
virSecretDefPtr virSecretDefParseFile(virConnectPtr conn, const char *filename);
char *virSecretDefFormat(virConnectPtr conn, const virSecretDefPtr def);

#endif
