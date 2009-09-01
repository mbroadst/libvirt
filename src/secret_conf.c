/*
 * secret_conf.c: internal <secret> XML handling
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

#include <config.h>

#include "internal.h"
#include "buf.h"
#include "datatypes.h"
#include "logging.h"
#include "memory.h"
#include "secret_conf.h"
#include "virterror_internal.h"
#include "util.h"
#include "xml.h"

#define VIR_FROM_THIS VIR_FROM_SECRET

VIR_ENUM_IMPL(virSecretUsageType, VIR_SECRET_USAGE_TYPE_LAST, "none", "volume")

void
virSecretDefFree(virSecretDefPtr def)
{
    if (def == NULL)
        return;

    VIR_FREE(def->id);
    VIR_FREE(def->description);
    switch (def->usage_type) {
    case VIR_SECRET_USAGE_TYPE_NONE:
        break;

    case VIR_SECRET_USAGE_TYPE_VOLUME:
        VIR_FREE(def->usage.volume);
        break;

    default:
        VIR_ERROR(_("unexpected secret usage type %d"), def->usage_type);
        break;
    }
    VIR_FREE(def);
}

static int
virSecretDefParseUsage(virConnectPtr conn, xmlXPathContextPtr ctxt,
                       virSecretDefPtr def)
{
    char *type_str;
    int type;

    type_str = virXPathString(conn, "string(./usage/@type)", ctxt);
    if (type_str == NULL) {
        virSecretReportError(conn, VIR_ERR_XML_ERROR, "%s",
                             _("unknown secret usage type"));
        return -1;
    }
    type = virSecretUsageTypeTypeFromString(type_str);
    if (type < 0) {
        virSecretReportError(conn, VIR_ERR_XML_ERROR,
                             _("unknown secret usage type %s"), type_str);
        VIR_FREE(type_str);
        return -1;
    }
    VIR_FREE(type_str);
    def->usage_type = type;
    switch (def->usage_type) {
    case VIR_SECRET_USAGE_TYPE_NONE:
        break;

    case VIR_SECRET_USAGE_TYPE_VOLUME:
        def->usage.volume = virXPathString(conn, "string(./usage/volume)",
                                           ctxt);
        break;

    default:
        virSecretReportError(conn, VIR_ERR_INTERNAL_ERROR,
                             _("unexpected secret usage type %d"),
                             def->usage_type);
        return -1;
    }
    return 0;
}

static virSecretDefPtr
secretXMLParseNode(virConnectPtr conn, xmlDocPtr xml, xmlNodePtr root)
{
    xmlXPathContextPtr ctxt = NULL;
    virSecretDefPtr def = NULL, ret = NULL;
    char *prop = NULL;

    if (!xmlStrEqual(root->name, BAD_CAST "secret")) {
        virSecretReportError(conn, VIR_ERR_XML_ERROR, "%s",
                             _("incorrect root element"));
        goto cleanup;
    }

    ctxt = xmlXPathNewContext(xml);
    if (ctxt == NULL) {
        virReportOOMError(conn);
        goto cleanup;
    }
    ctxt->node = root;

    if (VIR_ALLOC(def) < 0) {
        virReportOOMError(conn);
        goto cleanup;
    }

    prop = virXPathString(conn, "string(./@ephemeral)", ctxt);
    if (prop != NULL) {
        if (STREQ(prop, "yes"))
            def->ephemeral = 1;
        else if (STREQ(prop, "no"))
            def->ephemeral = 0;
        else {
            virSecretReportError(conn, VIR_ERR_XML_ERROR, "%s",
                                 _("invalid value of 'ephemeral'"));
            goto cleanup;
        }
        VIR_FREE(prop);
    }

    prop = virXPathString(conn, "string(./@private)", ctxt);
    if (prop != NULL) {
        if (STREQ(prop, "yes"))
            def->private = 1;
        else if (STREQ(prop, "no"))
            def->private = 0;
        else {
            virSecretReportError(conn, VIR_ERR_XML_ERROR, "%s",
                                 _("invalid value of 'private'"));
            goto cleanup;
        }
        VIR_FREE(prop);
    }

    def->id = virXPathString(conn, "string(./uuid)", ctxt);
    def->description = virXPathString(conn, "string(./description)", ctxt);
    if (virXPathNode(conn, "./usage", ctxt) != NULL
        && virSecretDefParseUsage(conn, ctxt, def) < 0)
        goto cleanup;
    ret = def;
    def = NULL;

 cleanup:
    VIR_FREE(prop);
    virSecretDefFree(def);
    xmlXPathFreeContext(ctxt);
    return ret;
}

/* Called from SAX on parsing errors in the XML. */
static void
catchXMLError(void *ctx, const char *msg ATTRIBUTE_UNUSED, ...)
{
    xmlParserCtxtPtr ctxt = (xmlParserCtxtPtr) ctx;

    if (ctxt) {
        virConnectPtr conn = ctxt->_private;

        if (virGetLastError() == NULL &&
            ctxt->lastError.level == XML_ERR_FATAL &&
            ctxt->lastError.message != NULL) {
            virSecretReportError(conn,  VIR_ERR_XML_DETAIL, _("at line %d: %s"),
                                 ctxt->lastError.line, ctxt->lastError.message);
        }
    }
}

static virSecretDefPtr
virSecretDefParse(virConnectPtr conn, const char *xmlStr, const char *filename)
{
    xmlParserCtxtPtr pctxt;
    xmlDocPtr xml = NULL;
    xmlNodePtr root;
    virSecretDefPtr ret = NULL;

    pctxt = xmlNewParserCtxt();
    if (pctxt == NULL || pctxt->sax == NULL)
        goto cleanup;
    pctxt->sax->error = catchXMLError;
    pctxt->_private = conn;

    if (filename != NULL)
        xml = xmlCtxtReadFile(pctxt, filename, NULL,
                              XML_PARSE_NOENT | XML_PARSE_NONET |
                              XML_PARSE_NOWARNING);
    else
        xml = xmlCtxtReadDoc(pctxt, BAD_CAST xmlStr, "secret.xml", NULL,
                             XML_PARSE_NOENT | XML_PARSE_NONET |
                             XML_PARSE_NOWARNING);
    if (xml == NULL) {
        if (conn->err.code == VIR_ERR_NONE)
            virSecretReportError(conn, VIR_ERR_XML_ERROR, "%s",
                                 _("failed to parse xml document"));
        goto cleanup;
    }

    root = xmlDocGetRootElement(xml);
    if (root == NULL) {
        virSecretReportError(conn, VIR_ERR_INTERNAL_ERROR, "%s",
                             _("missing root element"));
        goto cleanup;
    }

    ret = secretXMLParseNode(conn, xml, root);

 cleanup:
    xmlFreeDoc(xml);
    xmlFreeParserCtxt(pctxt);
    return ret;
}

virSecretDefPtr
virSecretDefParseString(virConnectPtr conn, const char *xmlStr)
{
    return virSecretDefParse(conn, xmlStr, NULL);
}

virSecretDefPtr
virSecretDefParseFile(virConnectPtr conn, const char *filename)
{
    return virSecretDefParse(conn, NULL, filename);
}

static int
virSecretDefFormatUsage(virConnectPtr conn, virBufferPtr buf,
                        const virSecretDefPtr def)
{
    const char *type;

    type = virSecretUsageTypeTypeToString(def->usage_type);
    if (type == NULL) {
        virSecretReportError(conn, VIR_ERR_INTERNAL_ERROR,
                             _("unexpected secret usage type %d"),
                             def->usage_type);
        return -1;
    }
    virBufferVSprintf(buf, "  <usage type='%s'>\n", type);
    switch (def->usage_type) {
    case VIR_SECRET_USAGE_TYPE_NONE:
        break;

    case VIR_SECRET_USAGE_TYPE_VOLUME:
        if (def->usage.volume != NULL)
            virBufferEscapeString(buf, "    <volume>%s</volume>\n",
                                  def->usage.volume);
        break;

    default:
        virSecretReportError(conn, VIR_ERR_INTERNAL_ERROR,
                             _("unexpected secret usage type %d"),
                             def->usage_type);
        return -1;
    }
    virBufferAddLit(buf, "  </usage>\n");

    return 0;
}

char *
virSecretDefFormat(virConnectPtr conn, const virSecretDefPtr def)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    char *tmp;

    virBufferVSprintf(&buf, "<secret ephemeral='%s' private='%s'>\n",
                      def->ephemeral ? "yes" : "no",
                      def->private ? "yes" : "no");
    if (def->id != NULL)
        virBufferEscapeString(&buf, "  <uuid>%s</uuid>\n", def->id);
    if (def->description != NULL)
        virBufferEscapeString(&buf, "  <description>%s</description>\n",
                              def->description);
    if (def->usage_type != VIR_SECRET_USAGE_TYPE_NONE &&
        virSecretDefFormatUsage(conn, &buf, def) < 0)
        goto error;
    virBufferAddLit(&buf, "</secret>\n");

    if (virBufferError(&buf))
        goto no_memory;

    return virBufferContentAndReset(&buf);

 no_memory:
    virReportOOMError(conn);
 error:
    tmp = virBufferContentAndReset(&buf);
    VIR_FREE(tmp);
    return NULL;
}
