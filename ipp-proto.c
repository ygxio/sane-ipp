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

/******************** Constants ********************/
/* How many times to try connecting before giving up.
 *
 * More than one, because a device that has only just announced itself
 * over DNS-SD may refuse the first connection while it is still coming
 * up.
 */
#define IPP_CONNECT_ATTEMPTS    3

/* The user name a scan job is created under.
 *
 * A service may use it to tell jobs apart, so it has to be something,
 * but nothing here depends on what it is.
 */
#define IPP_REQUESTING_USER     "sane-ipp"

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

/* What we ask a service to report.
 *
 * "all" is not everything: an attribute the printer considers optional
 * is only returned when it is named. The scan attributes are such a
 * case on some services, so they are spelled out here rather than left
 * to the wildcard (PWG 5100.15, RFC 8011 section 4.2.5).
 */
static const char * const ipp_requested_attrs[] = {
    "all",
    "input-attributes-default",
    "input-attributes-supported",
    "input-color-mode-supported",
    "input-media-supported",
    "input-orientation-requested-supported",
    "input-quality-supported",
    "input-resolution-supported",
    "input-scan-regions-supported",
    "input-sides-supported",
    "input-source-supported",
    "printer-service-type"
};

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

/* Fetch a multi-valued resolution attribute. Returns NULL and sets
 * *count to zero if the attribute is absent or holds no resolutions.
 */
static ipp_resolution*
ipp_attr_res_list (ipp_t *resp, const char *name, size_t *count)
{
    ipp_attribute_t *attr = ippFindAttribute(resp, name, IPP_TAG_RESOLUTION);
    ipp_resolution  *list;
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
        ipp_res_t units;

        /* The cross-feed resolution is the return value, the feed
         * resolution comes back through the second argument
         */
        list[i].x = ippGetResolution(attr, i, &list[i].y, &units);
        list[i].units = (int) units;
    }

    *count = (size_t) n;
    return list;
}

/* Fetch a member of a collection that is either a range or a single
 * integer, which is a range of one. Returns false if it is neither.
 */
static bool
ipp_col_range (ipp_t *col, const char *name, ipp_range *range)
{
    ipp_attribute_t *attr = ippFindAttribute(col, name, IPP_TAG_ZERO);

    if (attr == NULL || ippGetCount(attr) < 1) {
        return false;
    }

    switch (ippGetValueTag(attr)) {
    case IPP_TAG_RANGE:
        range->lower = ippGetRange(attr, 0, &range->upper);
        return true;

    case IPP_TAG_INTEGER:
        range->lower = range->upper = ippGetInteger(attr, 0);
        return true;

    default:
        return false;
    }
}

/* Fetch the "input-scan-regions-supported" collections. Returns NULL and
 * sets *count to zero if the attribute is absent or no collection in it
 * carries all four members.
 */
static ipp_scan_region*
ipp_attr_region_list (ipp_t *resp, const char *name, size_t *count)
{
    ipp_attribute_t *attr = ippFindAttribute(resp, name, IPP_TAG_BEGIN_COLLECTION);
    ipp_scan_region *list;
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
        ipp_t           *col = ippGetCollection(attr, i);
        ipp_scan_region *reg = &list[cnt];

        /* A collection missing a member says nothing usable about the
         * area it would describe, so it is skipped
         */
        if (col != NULL &&
            ipp_col_range(col, "x-origin", &reg->x_origin) &&
            ipp_col_range(col, "y-origin", &reg->y_origin) &&
            ipp_col_range(col, "x-dimension", &reg->x_dimension) &&
            ipp_col_range(col, "y-dimension", &reg->y_dimension)) {
            cnt ++;
        }
    }

    if (cnt == 0) {
        free(list);
        return NULL;
    }

    *count = cnt;
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
/* Release scan capabilities
 */
static void
ipp_scanner_free (ipp_scanner *scanner)
{
    if (scanner == NULL) {
        return;
    }

    ipp_str_list_free(scanner->color_modes, scanner->n_color_modes);
    ipp_str_list_free(scanner->sources, scanner->n_sources);
    ipp_str_list_free(scanner->media, scanner->n_media);
    ipp_str_list_free(scanner->sides, scanner->n_sides);
    ipp_str_list_free(scanner->members, scanner->n_members);

    free(scanner->resolutions);
    free(scanner->qualities);
    free(scanner->orientations);
    free(scanner->regions);

    free(scanner);
}

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
    free(printer->service_type);

    ipp_str_list_free(printer->uris, printer->n_uris);
    ipp_str_list_free(printer->versions, printer->n_versions);
    ipp_str_list_free(printer->formats, printer->n_formats);

    free(printer->ops);

    ipp_scanner_free(printer->scanner);

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

/* Decode the scan capabilities out of a Get-Printer-Attributes response.
 *
 * IPP Scan has no operation of its own for this: a scan service answers
 * Get-Printer-Attributes and its capabilities ride in that response
 * alongside the printing ones (PWG 5100.15).
 *
 * Returns NULL when the response carries no scan attribute at all, which
 * is how a print-only service answers.
 */
static ipp_scanner*
ipp_scanner_decode (ipp_t *resp)
{
    ipp_scanner *scanner = ipp_proto_alloc(sizeof(*scanner));

    memset(scanner, 0, sizeof(*scanner));

    scanner->color_modes = ipp_attr_str_list(resp,
            "input-color-mode-supported", &scanner->n_color_modes);
    scanner->sources = ipp_attr_str_list(resp,
            "input-source-supported", &scanner->n_sources);
    scanner->media = ipp_attr_str_list(resp,
            "input-media-supported", &scanner->n_media);
    scanner->sides = ipp_attr_str_list(resp,
            "input-sides-supported", &scanner->n_sides);
    scanner->members = ipp_attr_str_list(resp,
            "input-attributes-supported", &scanner->n_members);

    scanner->resolutions = ipp_attr_res_list(resp,
            "input-resolution-supported", &scanner->n_resolutions);

    scanner->qualities = ipp_attr_int_list(resp,
            "input-quality-supported", &scanner->n_qualities);
    scanner->orientations = ipp_attr_int_list(resp,
            "input-orientation-requested-supported", &scanner->n_orientations);

    scanner->regions = ipp_attr_region_list(resp,
            "input-scan-regions-supported", &scanner->n_regions);

    if (scanner->n_color_modes == 0 && scanner->n_sources == 0 &&
        scanner->n_media == 0 && scanner->n_sides == 0 &&
        scanner->n_members == 0 && scanner->n_resolutions == 0 &&
        scanner->n_qualities == 0 && scanner->n_orientations == 0 &&
        scanner->n_regions == 0) {
        ipp_scanner_free(scanner);
        return NULL;
    }

    return scanner;
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
    printer->service_type = ipp_attr_str(resp, "printer-service-type");

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

    printer->scanner = ipp_scanner_decode(resp);

    return printer;
}

/******************** Connecting ********************/
/* Where an IPP URI points, in the terms the connect path needs
 */
typedef struct {
    char              host[256];    /* Without the brackets of an IPv6 literal */
    int               port;
    char              resource[1024];
    http_encryption_t encryption;
} ipp_dest;

/* Split an IPP URI into its parts.
 *
 * Returns false if the URI is not one we can use, having described the
 * problem in errbuf.
 */
static bool
ipp_dest_parse (const char *uri, ipp_dest *dest, char *errbuf, size_t errlen)
{
    char              scheme[32], userpass[256];
    http_uri_status_t uri_status;

    /* The host comes back without the brackets of an IPv6 literal,
     * which is what the connect path below expects.
     */
    uri_status = httpSeparateURI(HTTP_URI_CODING_ALL, uri,
            scheme, sizeof(scheme),
            userpass, sizeof(userpass),
            dest->host, sizeof(dest->host),
            &dest->port,
            dest->resource, sizeof(dest->resource));

    if (uri_status < HTTP_URI_STATUS_OK) {
        ipp_proto_err(errbuf, errlen, "%s: cannot parse URI", uri);
        return false;
    }

    if (strcmp(scheme, "ipp") != 0 && strcmp(scheme, "ipps") != 0) {
        ipp_proto_err(errbuf, errlen, "%s: not an IPP URI", uri);
        return false;
    }

    /* An ipps:// URI means TLS from the first byte. An ipp:// URI is
     * plain, but the printer may still ask to upgrade, so let the
     * library honour that request rather than refusing it.
     */
    dest->encryption = strcmp(scheme, "ipps") == 0
        ? HTTP_ENCRYPTION_ALWAYS
        : HTTP_ENCRYPTION_IF_REQUESTED;

    return true;
}

/* Open a connection to a parsed destination.
 *
 * Connect explicitly, rather than letting the library pick a
 * destination of its own: its default is the local print server, which
 * is not what we are talking to.
 *
 * A device that has only just announced itself may not be listening
 * yet, so the connection is attempted more than once before it is
 * called a failure.
 */
static http_t*
ipp_dest_connect (const ipp_dest *dest, int timeout_ms, const char *uri,
        char *errbuf, size_t errlen)
{
    http_t *http;

    ipp_proto_dbg("connecting to %s:%d", dest->host, dest->port);

    http = httpConnect2(dest->host, dest->port, NULL, AF_UNSPEC,
            dest->encryption, IPP_CONNECT_ATTEMPTS, timeout_ms, NULL);

    if (http == NULL) {
        ipp_proto_err(errbuf, errlen, "%s: %s", uri, cupsLastErrorString());
        return NULL;
    }

    httpSetTimeout(http, (double) timeout_ms / 1000.0, NULL, NULL);

    return http;
}

/******************** Get-Printer-Attributes ********************/
/* Send Get-Printer-Attributes and decode the response
 */
ipp_printer*
ipp_get_printer_attributes (const char *uri, int timeout_ms,
        char *errbuf, size_t errlen)
{
    ipp_dest          dest;
    http_t            *http;
    ipp_t             *request, *response;
    ipp_status_t      status;
    ipp_printer       *printer;
    const char        *resource;

    if (errbuf != NULL && errlen != 0) {
        errbuf[0] = '\0';
    }

    if (!ipp_dest_parse(uri, &dest, errbuf, errlen)) {
        return NULL;
    }

    http = ipp_dest_connect(&dest, timeout_ms, uri, errbuf, errlen);
    if (http == NULL) {
        return NULL;
    }

    resource = dest.resource;

    /* Build the request. ippNewRequest() supplies the mandatory
     * "attributes-charset" and "attributes-natural-language", so only
     * the operation's own attributes are added here.
     */
    request = ippNewRequest(IPP_OP_GET_PRINTER_ATTRIBUTES);

    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI,
            "printer-uri", NULL, uri);
    ippAddStrings(request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD,
            "requested-attributes", sizeof(ipp_requested_attrs) /
                    sizeof(ipp_requested_attrs[0]),
            NULL, ipp_requested_attrs);

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

/******************** Scan jobs ********************/
/* A scan job in progress.
 *
 * The connection is part of the job, not borrowed for each request: the
 * image data arrives as the body of a Get-Next-Document-Data response,
 * so the connection has to survive between requests, and only one thing
 * can be in flight on it at a time.
 */
struct ipp_job {
    char       *uri;        /* Printer URI, as given to ipp_job_create() */
    ipp_dest   dest;        /* Where that URI points */
    int        timeout_ms;  /* Per-exchange timeout */
    http_t     *http;       /* Connection, NULL once closed */
    int        job_id;      /* job-id, as assigned by the service */
    char       *format;     /* document-format of the image being read */
    bool       reading;     /* An image body is open on the connection */
    bool       finished;    /* The service said there are no more images */
};

/* Build the "input-scan-regions" collection of a scan request.
 *
 * The members are in hundredths of a millimetre, and name the top-left
 * corner and the size rather than two corners (PWG 5100.15 section 6.2).
 */
static void
ipp_job_add_region (ipp_t *col, const ipp_job_params *params)
{
    ipp_t *region = ippNew();

    ippAddInteger(region, IPP_TAG_ZERO, IPP_TAG_INTEGER,
            "x-origin", params->x_origin);
    ippAddInteger(region, IPP_TAG_ZERO, IPP_TAG_INTEGER,
            "y-origin", params->y_origin);
    ippAddInteger(region, IPP_TAG_ZERO, IPP_TAG_INTEGER,
            "x-dimension", params->x_dimension);
    ippAddInteger(region, IPP_TAG_ZERO, IPP_TAG_INTEGER,
            "y-dimension", params->y_dimension);

    ippAddCollection(col, IPP_TAG_ZERO, "input-scan-regions", region);

    ippDelete(region);
}

/* Build the Create-Job request of a scan job.
 *
 * What to scan goes in the "input-attributes" collection, which IPP
 * Scan adds to the operation attributes of an ordinary Create-Job
 * (PWG 5100.17 section 7.1). A setting the caller left empty is left
 * out, so that the service applies its own default rather than being
 * told a value it may not have.
 */
static ipp_t*
ipp_job_request (const char *uri, const ipp_job_params *params)
{
    ipp_t           *request = ippNewRequest(IPP_OP_CREATE_JOB);
    ipp_t           *col = ippNew();
    ipp_attribute_t *attr;

    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI,
            "printer-uri", NULL, uri);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME,
            "requesting-user-name", NULL, IPP_REQUESTING_USER);

    if (params->format != NULL) {
        ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_MIMETYPE,
                "document-format-accepted", NULL, params->format);
    }

    if (params->color_mode != NULL) {
        ippAddString(col, IPP_TAG_ZERO, IPP_TAG_KEYWORD,
                "input-color-mode", NULL, params->color_mode);
    }

    if (params->source != NULL) {
        ippAddString(col, IPP_TAG_ZERO, IPP_TAG_KEYWORD,
                "input-source", NULL, params->source);
    }

    if (params->sides != NULL) {
        ippAddString(col, IPP_TAG_ZERO, IPP_TAG_KEYWORD,
                "input-sides", NULL, params->sides);
    }

    if (params->resolution > 0) {
        ippAddResolution(col, IPP_TAG_ZERO, "input-resolution",
                IPP_RES_PER_INCH, params->resolution, params->resolution);
    }

    if (params->have_region) {
        ipp_job_add_region(col, params);
    }

    attr = ippAddCollection(request, IPP_TAG_OPERATION,
            "input-attributes", NULL);
    ippSetCollection(request, &attr, 0, col);

    ippDelete(col);

    return request;
}

/* Start a scan job
 */
ipp_job*
ipp_job_create (const char *uri, const ipp_job_params *params,
        int timeout_ms, char *errbuf, size_t errlen)
{
    ipp_dest        dest;
    http_t          *http;
    ipp_t           *request, *response;
    ipp_attribute_t *attr;
    ipp_status_t    status;
    ipp_job         *job;

    if (errbuf != NULL && errlen != 0) {
        errbuf[0] = '\0';
    }

    if (!ipp_dest_parse(uri, &dest, errbuf, errlen)) {
        return NULL;
    }

    http = ipp_dest_connect(&dest, timeout_ms, uri, errbuf, errlen);
    if (http == NULL) {
        return NULL;
    }

    /* cupsDoRequest() consumes the request, whatever the outcome
     */
    request = ipp_job_request(uri, params);
    response = cupsDoRequest(http, request, dest.resource);

    if (response == NULL) {
        ipp_proto_err(errbuf, errlen, "%s: Create-Job: %s", uri,
                cupsLastErrorString());
        httpClose(http);
        return NULL;
    }

    status = ippGetStatusCode(response);
    if (status >= IPP_STATUS_ERROR_BAD_REQUEST) {
        ipp_proto_err(errbuf, errlen, "%s: Create-Job: %s", uri,
                ippErrorString(status));
        ippDelete(response);
        httpClose(http);
        return NULL;
    }

    /* Without a job-id there is nothing to poll, so a response that
     * omits it is no better than a failure.
     */
    attr = ippFindAttribute(response, "job-id", IPP_TAG_INTEGER);
    if (attr == NULL) {
        ipp_proto_err(errbuf, errlen, "%s: Create-Job: no job-id in reply",
                uri);
        ippDelete(response);
        httpClose(http);
        return NULL;
    }

    job = ipp_proto_alloc(sizeof(*job));
    memset(job, 0, sizeof(*job));

    job->uri = ipp_proto_str_dup(uri);
    job->dest = dest;
    job->timeout_ms = timeout_ms;
    job->http = http;
    job->job_id = ippGetInteger(attr, 0);

    ipp_proto_dbg("Create-Job: job-id=%d", job->job_id);

    ippDelete(response);

    return job;
}

/* Read the rest of the image body and throw it away.
 *
 * The connection carries one exchange at a time, so a body left half
 * read has to be finished before anything else can be asked of the
 * service.
 */
static void
ipp_job_drain (ipp_job *job)
{
    char buf[4096];

    while (job->reading) {
        ssize_t n = cupsReadResponseData(job->http, buf, sizeof(buf));

        if (n <= 0) {
            job->reading = false;
        }
    }
}

/* Ask for the next image of the job
 */
ipp_doc_status
ipp_job_next_document (ipp_job *job, int *wait_sec,
        char *errbuf, size_t errlen)
{
    ipp_t           *request, *response;
    ipp_attribute_t *attr;
    ipp_status_t    status;
    http_status_t   http_status;
    const char      *format;

    if (errbuf != NULL && errlen != 0) {
        errbuf[0] = '\0';
    }

    if (wait_sec != NULL) {
        *wait_sec = 0;
    }

    if (job->finished || job->http == NULL) {
        return IPP_DOC_DONE;
    }

    ipp_job_drain(job);

    free(job->format);
    job->format = NULL;

    request = ippNewRequest(IPP_OP_GET_NEXT_DOCUMENT_DATA);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI,
            "printer-uri", NULL, job->uri);
    ippAddInteger(request, IPP_TAG_OPERATION, IPP_TAG_INTEGER,
            "job-id", job->job_id);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME,
            "requesting-user-name", NULL, IPP_REQUESTING_USER);

    /* The response body is the image, so the request and the response
     * are sent and read in steps rather than with cupsDoRequest(),
     * which would consume the body along with the message. Unlike
     * cupsDoRequest(), cupsSendRequest() does not take the request over.
     */
    http_status = cupsSendRequest(job->http, request, job->dest.resource, 0);
    ippDelete(request);

    if (http_status != HTTP_STATUS_CONTINUE &&
        http_status != HTTP_STATUS_OK) {
        ipp_proto_err(errbuf, errlen, "%s: Get-Next-Document-Data: %s",
                job->uri, httpStatus(http_status));
        return IPP_DOC_ERROR;
    }

    response = cupsGetResponse(job->http, job->dest.resource);
    if (response == NULL) {
        ipp_proto_err(errbuf, errlen, "%s: Get-Next-Document-Data: %s",
                job->uri, cupsLastErrorString());
        return IPP_DOC_ERROR;
    }

    status = ippGetStatusCode(response);

    /* A job the service no longer knows about is a job that has ended,
     * not an error: a service is free to forget a finished job before
     * the client has asked for the image after the last one.
     */
    if (status == IPP_STATUS_ERROR_NOT_FOUND) {
        ipp_proto_dbg("Get-Next-Document-Data: job %d is gone", job->job_id);
        ippDelete(response);
        job->finished = true;
        return IPP_DOC_DONE;
    }

    if (status >= IPP_STATUS_ERROR_BAD_REQUEST) {
        ipp_proto_err(errbuf, errlen, "%s: Get-Next-Document-Data: %s",
                job->uri, ippErrorString(status));
        ippDelete(response);
        return IPP_DOC_ERROR;
    }

    attr = ippFindAttribute(response, "last-document", IPP_TAG_BOOLEAN);
    if (attr != NULL && ippGetBoolean(attr, 0)) {
        ipp_proto_dbg("Get-Next-Document-Data: last document");
        ippDelete(response);
        job->finished = true;
        return IPP_DOC_DONE;
    }

    /* An image is announced by its format. A response without one is
     * the service saying it has nothing yet, and naming how long to
     * leave it alone (PWG 5100.17 section 7.2.2).
     */
    attr = ippFindAttribute(response, "document-format", IPP_TAG_MIMETYPE);
    format = attr != NULL ? ippGetString(attr, 0, NULL) : NULL;

    if (format == NULL) {
        int interval = 0;

        attr = ippFindAttribute(response, "document-data-get-interval",
                IPP_TAG_INTEGER);
        if (attr != NULL) {
            interval = ippGetInteger(attr, 0);
        }

        if (interval <= 0) {
            interval = 1;
        }

        ipp_proto_dbg("Get-Next-Document-Data: nothing yet, retry in %d s",
                interval);

        if (wait_sec != NULL) {
            *wait_sec = interval;
        }

        ippDelete(response);
        return IPP_DOC_WAIT;
    }

    job->format = ipp_proto_str_dup(format);
    job->reading = true;

    ipp_proto_dbg("Get-Next-Document-Data: %s", job->format);

    ippDelete(response);

    return IPP_DOC_READY;
}

/* MIME type of the image being read
 */
const char*
ipp_job_document_format (const ipp_job *job)
{
    return job->format;
}

/* Read image data
 */
ssize_t
ipp_job_read (ipp_job *job, void *data, size_t size,
        char *errbuf, size_t errlen)
{
    ssize_t n;

    if (errbuf != NULL && errlen != 0) {
        errbuf[0] = '\0';
    }

    if (!job->reading) {
        return 0;
    }

    n = cupsReadResponseData(job->http, data, size);

    if (n < 0) {
        ipp_proto_err(errbuf, errlen, "%s: read: %s", job->uri,
                cupsLastErrorString());
        job->reading = false;
        return -1;
    }

    if (n == 0) {
        job->reading = false;
    }

    return n;
}

/* Abort the job
 */
void
ipp_job_cancel (ipp_job *job)
{
    http_t   *http;
    ipp_t    *request, *response;

    if (job->finished || job->http == NULL) {
        return;
    }

    /* Cancel-Job needs a connection of its own when an image is still
     * open on the job's one. Reading a whole page only to discard it
     * would be slower than the cancelling it is meant to serve, so the
     * connection is dropped instead.
     */
    if (job->reading) {
        httpClose(job->http);
        job->http = NULL;
        job->reading = false;

        http = ipp_dest_connect(&job->dest, job->timeout_ms, job->uri,
                NULL, 0);
        if (http == NULL) {
            job->finished = true;
            return;
        }

        job->http = http;
    }

    request = ippNewRequest(IPP_OP_CANCEL_JOB);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI,
            "printer-uri", NULL, job->uri);
    ippAddInteger(request, IPP_TAG_OPERATION, IPP_TAG_INTEGER,
            "job-id", job->job_id);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME,
            "requesting-user-name", NULL, IPP_REQUESTING_USER);

    response = cupsDoRequest(job->http, request, job->dest.resource);

    ipp_proto_dbg("Cancel-Job: job-id=%d: %s", job->job_id,
            response != NULL
                ? ippErrorString(ippGetStatusCode(response))
                : cupsLastErrorString());

    ippDelete(response);

    job->finished = true;
}

/* Release the job
 */
void
ipp_job_free (ipp_job *job)
{
    if (job == NULL) {
        return;
    }

    ipp_job_cancel(job);

    if (job->http != NULL) {
        httpClose(job->http);
    }

    free(job->format);
    free(job->uri);
    free(job);
}
