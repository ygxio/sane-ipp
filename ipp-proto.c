/* sane-ipp -- IPP backend for SANE
 *
 * Copyright (C) 2026 Alexander Pevzner (pzz@apevzner.com)
 * Copyright (C) 2026 Yogesh Singla (yogeshsingla481@gmail.com)
 * SPDX-License-Identifier: BSD-2-Clause
 * See LICENSE for license terms and conditions
 *
 * IPP protocol operations (synchronous)
 *
 * This is the only file that speaks to the CUPS library. Everything the
 * rest of the backend needs from IPP is expressed here in terms of our
 * own types, so that the library underneath can be replaced without
 * touching anything else.
 */

#include "ipp-proto.h"

#include <cups/cups.h>
#include <cups/http.h>
#include <cups/ipp.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/******************** Static variables ********************/
static bool ipp_proto_debug = false;

/******************** Debugging ********************/
/* Print debug message, if enabled
 */
static void
ipp_proto_dbg (const char *fmt, ...)
{
    va_list ap;

    if (!ipp_proto_debug) {
        return;
    }

    va_start(ap, fmt);
    fputs("ipp-proto: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* Enable/disable protocol debug messages
 */
void
ipp_proto_debug_enable (bool enable)
{
    ipp_proto_debug = enable;
}

/******************** Small helpers ********************/
/* strdup() that aborts on out of memory
 */
static char*
ipp_proto_str_dup (const char *s)
{
    char *p = strdup(s);

    if (p == NULL) {
        perror("ipp-proto: strdup");
        abort();
    }

    return p;
}

/* malloc() that aborts on out of memory
 */
static void*
ipp_proto_alloc (size_t size)
{
    void *p = malloc(size);

    if (p == NULL) {
        perror("ipp-proto: malloc");
        abort();
    }

    return p;
}

/* Format an error message into the caller's buffer. Tolerates a NULL
 * buffer, so callers that do not want the message need no special case.
 */
static void
ipp_proto_err (char *errbuf, size_t errlen, const char *fmt, ...)
{
    va_list ap;

    if (errbuf == NULL || errlen == 0) {
        return;
    }

    va_start(ap, fmt);
    vsnprintf(errbuf, errlen, fmt, ap);
    va_end(ap);

    ipp_proto_dbg("%s", errbuf);
}

/******************** Attribute decoding ********************/
/* Fetch a single-valued string attribute, regardless of its value tag.
 * Returns NULL if the attribute is absent.
 */
static char*
ipp_attr_str (ipp_t *resp, const char *name)
{
    ipp_attribute_t *attr = ippFindAttribute(resp, name, IPP_TAG_ZERO);
    const char      *val;

    if (attr == NULL || ippGetCount(attr) == 0) {
        return NULL;
    }

    val = ippGetString(attr, 0, NULL);
    if (val == NULL) {
        return NULL;
    }

    return ipp_proto_str_dup(val);
}

/* Fetch a multi-valued string attribute. Returns NULL and sets *count to
 * zero if the attribute is absent or holds no string values.
 */
static char**
ipp_attr_str_list (ipp_t *resp, const char *name, size_t *count)
{
    ipp_attribute_t *attr = ippFindAttribute(resp, name, IPP_TAG_ZERO);
    char            **list;
    int             i, n;
    size_t          cnt = 0;

    *count = 0;

    if (attr == NULL) {
        return NULL;
    }

    n = ippGetCount(attr);
    if (n <= 0) {
        return NULL;
    }

    list = ipp_proto_alloc(sizeof(*list) * (size_t) n);

    for (i = 0; i < n; i ++) {
        const char *val = ippGetString(attr, i, NULL);

        /* Skip values that are not strings: a printer may report a mixed
         * attribute, and ippGetString() returns NULL for those.
         */
        if (val != NULL) {
            list[cnt ++] = ipp_proto_str_dup(val);
        }
    }

    if (cnt == 0) {
        free(list);
        return NULL;
    }

    *count = cnt;
    return list;
}

/* Fetch a multi-valued integer or enum attribute. Returns NULL and sets
 * *count to zero if the attribute is absent.
 */
static int*
ipp_attr_int_list (ipp_t *resp, const char *name, size_t *count)
{
    ipp_attribute_t *attr = ippFindAttribute(resp, name, IPP_TAG_ZERO);
    int             *list;
    int             i, n;

    *count = 0;

    if (attr == NULL) {
        return NULL;
    }

    n = ippGetCount(attr);
    if (n <= 0) {
        return NULL;
    }

    list = ipp_proto_alloc(sizeof(*list) * (size_t) n);
    for (i = 0; i < n; i ++) {
        list[i] = ippGetInteger(attr, i);
    }

    *count = (size_t) n;
    return list;
}

/* Release an array of strings
 */
static void
ipp_str_list_free (char **list, size_t count)
{
    size_t i;

    for (i = 0; i < count; i ++) {
        free(list[i]);
    }

    free(list);
}

/******************** Printer attributes ********************/
/* Release the structure returned by ipp_get_printer_attributes()
 */
void
ipp_printer_free (ipp_printer *printer)
{
    if (printer == NULL) {
        return;
    }

    free(printer->make_and_model);
    free(printer->info);
    free(printer->location);
    free(printer->dns_sd_name);
    free(printer->uuid);
    free(printer->state_message);

    ipp_str_list_free(printer->uris, printer->n_uris);
    ipp_str_list_free(printer->versions, printer->n_versions);
    ipp_str_list_free(printer->formats, printer->n_formats);

    free(printer->ops);
    free(printer);
}

/* Report whether the printer listed the given operation code
 */
bool
ipp_printer_has_op (const ipp_printer *printer, int op)
{
    size_t i;

    if (printer == NULL) {
        return false;
    }

    for (i = 0; i < printer->n_ops; i ++) {
        if (printer->ops[i] == op) {
            return true;
        }
    }

    return false;
}

/* Name of an IPP operation code, for display
 */
const char*
ipp_op_name (int op)
{
    const char *name = ippOpString((ipp_op_t) op);

    return name != NULL ? name : "unknown";
}

/* Name of a printer state, for display
 */
const char*
ipp_state_name (int state)
{
    switch (state) {
    case IPP_PRINTER_STATE_IDLE:       return "idle";
    case IPP_PRINTER_STATE_PROCESSING: return "processing";
    case IPP_PRINTER_STATE_STOPPED:    return "stopped";
    }

    return "unknown";
}

/* Decode a Get-Printer-Attributes response
 */
static ipp_printer*
ipp_printer_decode (ipp_t *resp)
{
    ipp_printer     *printer = ipp_proto_alloc(sizeof(*printer));
    ipp_attribute_t *attr;

    memset(printer, 0, sizeof(*printer));

    printer->make_and_model = ipp_attr_str(resp, "printer-make-and-model");
    printer->info = ipp_attr_str(resp, "printer-info");
    printer->location = ipp_attr_str(resp, "printer-location");
    printer->dns_sd_name = ipp_attr_str(resp, "printer-dns-sd-name");
    printer->uuid = ipp_attr_str(resp, "printer-uuid");
    printer->state_message = ipp_attr_str(resp, "printer-state-message");

    printer->uris = ipp_attr_str_list(resp, "printer-uri-supported",
            &printer->n_uris);
    printer->versions = ipp_attr_str_list(resp, "ipp-versions-supported",
            &printer->n_versions);
    printer->formats = ipp_attr_str_list(resp, "document-format-supported",
            &printer->n_formats);

    printer->ops = ipp_attr_int_list(resp, "operations-supported",
            &printer->n_ops);

    attr = ippFindAttribute(resp, "printer-state", IPP_TAG_ENUM);
    if (attr != NULL) {
        printer->state = ippGetInteger(attr, 0);
    }

    attr = ippFindAttribute(resp, "printer-is-accepting-jobs",
            IPP_TAG_BOOLEAN);
    if (attr != NULL) {
        printer->accepting_jobs = ippGetBoolean(attr, 0) != 0;
    }

    return printer;
}

/* Send Get-Printer-Attributes and decode the response
 */
ipp_printer*
ipp_get_printer_attributes (const char *uri, int timeout_ms,
        char *errbuf, size_t errlen)
{
    char              scheme[32], userpass[256], host[256], resource[1024];
    int               port;
    http_uri_status_t uri_status;
    http_encryption_t encryption;
    http_t            *http;
    ipp_t             *request, *response;
    ipp_status_t      status;
    ipp_printer       *printer;

    if (errbuf != NULL && errlen != 0) {
        errbuf[0] = '\0';
    }

    /* Split the URI. The host comes back without the brackets of an IPv6
     * literal, which is what the connect path below expects.
     */
    uri_status = httpSeparateURI(HTTP_URI_CODING_ALL, uri,
            scheme, sizeof(scheme),
            userpass, sizeof(userpass),
            host, sizeof(host),
            &port,
            resource, sizeof(resource));

    if (uri_status < HTTP_URI_STATUS_OK) {
        ipp_proto_err(errbuf, errlen, "%s: cannot parse URI", uri);
        return NULL;
    }

    if (strcmp(scheme, "ipp") != 0 && strcmp(scheme, "ipps") != 0) {
        ipp_proto_err(errbuf, errlen, "%s: not an IPP URI", uri);
        return NULL;
    }

    /* An ipps:// URI means TLS from the first byte. An ipp:// URI is
     * plain, but the printer may still ask to upgrade, so let the
     * library honour that request rather than refusing it.
     */
    encryption = strcmp(scheme, "ipps") == 0
        ? HTTP_ENCRYPTION_ALWAYS
        : HTTP_ENCRYPTION_IF_REQUESTED;

    ipp_proto_dbg("connecting to %s:%d (%s)", host, port, scheme);

    /* Connect explicitly, rather than letting the library pick a
     * destination of its own: its default is the local print server,
     * which is not what we are probing.
     */
    http = httpConnect2(host, port, NULL, AF_UNSPEC, encryption, 1,
            timeout_ms, NULL);

    if (http == NULL) {
        ipp_proto_err(errbuf, errlen, "%s: %s", uri, cupsLastErrorString());
        return NULL;
    }

    httpSetTimeout(http, (double) timeout_ms / 1000.0, NULL, NULL);

    /* Build the request. ippNewRequest() supplies the mandatory
     * "attributes-charset" and "attributes-natural-language", so only
     * the operation's own attributes are added here.
     */
    request = ippNewRequest(IPP_OP_GET_PRINTER_ATTRIBUTES);

    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI,
            "printer-uri", NULL, uri);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD,
            "requested-attributes", NULL, "all");

    /* cupsDoRequest() consumes the request, whatever the outcome
     */
    response = cupsDoRequest(http, request, resource);

    if (response == NULL) {
        ipp_proto_err(errbuf, errlen, "%s: %s", uri, cupsLastErrorString());
        httpClose(http);
        return NULL;
    }

    /* Status codes below the first error code are successes, possibly
     * with attributes ignored or substituted, which is still usable.
     */
    status = ippGetStatusCode(response);
    if (status >= IPP_STATUS_ERROR_BAD_REQUEST) {
        ipp_proto_err(errbuf, errlen, "%s: %s", uri, ippErrorString(status));
        ippDelete(response);
        httpClose(http);
        return NULL;
    }

    ipp_proto_dbg("Get-Printer-Attributes: %s", ippErrorString(status));

    printer = ipp_printer_decode(response);

    ippDelete(response);
    httpClose(http);

    return printer;
}
