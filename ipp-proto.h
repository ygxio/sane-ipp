/* sane-ipp -- IPP backend for SANE
 *
 * Copyright (C) 2026 Alexander Pevzner (pzz@apevzner.com)
 * Copyright (C) 2026 Yogesh Singla (yogeshsingla481@gmail.com)
 * SPDX-License-Identifier: BSD-2-Clause
 * See LICENSE for license terms and conditions
 *
 * IPP protocol operations (synchronous)
 */

#ifndef ipp_proto_h
#define ipp_proto_h

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Printer state, the values of the IPP "printer-state" attribute
 */
#define IPP_PRINTER_STATE_IDLE          3
#define IPP_PRINTER_STATE_PROCESSING    4
#define IPP_PRINTER_STATE_STOPPED       5

/* Units a resolution is expressed in, as per RFC 8011.
 *
 * The names avoid IPP_RES_*, which CUPS uses for the same two values in
 * an enum of its own, and which would collide here.
 */
#define IPP_RESOLUTION_PER_INCH         3
#define IPP_RESOLUTION_PER_CM           4

/* A single scan resolution
 */
typedef struct {
    int x;                      /* Cross-feed direction */
    int y;                      /* Feed direction */
    int units;                  /* IPP_RESOLUTION_PER_INCH or _PER_CM */
} ipp_resolution;

/* A range of integers, both ends included
 */
typedef struct {
    int lower;
    int upper;
} ipp_range;

/* One collection of "input-scan-regions-supported", in hundredths of a
 * millimetre (PWG 5100.15)
 */
typedef struct {
    ipp_range x_origin;         /* x-origin */
    ipp_range y_origin;         /* y-origin */
    ipp_range x_dimension;      /* x-dimension */
    ipp_range y_dimension;      /* y-dimension */
} ipp_scan_region;

/* What a scan service can be asked for.
 *
 * These attributes come back in the ordinary Get-Printer-Attributes
 * response: IPP Scan defines no operation of its own for fetching them
 * (PWG 5100.15). Unlike eSCL, the capabilities are not nested per input
 * source; they are flat lists covering the service as a whole.
 *
 * Every member is optional. A service that reports none of them leaves
 * ipp_printer::scanner NULL.
 */
typedef struct ipp_scanner {
    char   **color_modes;       /* input-color-mode-supported */
    size_t n_color_modes;

    char   **sources;           /* input-source-supported */
    size_t n_sources;

    char   **media;             /* input-media-supported */
    size_t n_media;

    char   **sides;             /* input-sides-supported */
    size_t n_sides;

    char   **members;           /* input-attributes-supported */
    size_t n_members;

    ipp_resolution *resolutions;/* input-resolution-supported */
    size_t n_resolutions;

    int    *qualities;          /* input-quality-supported */
    size_t n_qualities;

    int    *orientations;       /* input-orientation-requested-supported */
    size_t n_orientations;

    ipp_scan_region *regions;   /* input-scan-regions-supported */
    size_t n_regions;
} ipp_scanner;

/* ipp_printer holds the subset of Get-Printer-Attributes that interests
 * us. String members are NULL when the printer did not report them, and
 * the arrays are NULL with a zero count in the same case.
 */
typedef struct {
    char   *make_and_model;     /* printer-make-and-model */
    char   *info;               /* printer-info */
    char   *location;           /* printer-location */
    char   *dns_sd_name;        /* printer-dns-sd-name */
    char   *uuid;               /* printer-uuid */
    char   *state_message;      /* printer-state-message */
    char   *service_type;       /* printer-service-type */

    char   **uris;              /* printer-uri-supported */
    size_t n_uris;

    char   **versions;          /* ipp-versions-supported */
    size_t n_versions;

    char   **formats;           /* document-format-supported */
    size_t n_formats;

    int    *ops;                /* operations-supported */
    size_t n_ops;

    int    state;               /* printer-state, 0 if not reported */
    bool   accepting_jobs;      /* printer-is-accepting-jobs */

    ipp_scanner *scanner;
} ipp_printer;

/* Send Get-Printer-Attributes to the printer at the given URI and decode
 * the response.
 *
 * The URI is an "ipp://" or "ipps://" URI, such as the one found by
 * discovery. timeout_ms bounds both connecting and waiting for the reply.
 *
 * Returns the decoded attributes, to be released with ipp_printer_free().
 * On error returns NULL and, when errbuf is not NULL, writes there a
 * NUL-terminated description of what went wrong.
 */
ipp_printer*
ipp_get_printer_attributes (const char *uri, int timeout_ms,
        char *errbuf, size_t errlen);

/* Release the structure returned by ipp_get_printer_attributes()
 */
void
ipp_printer_free (ipp_printer *printer);

/* Report whether the printer listed the given operation code in its
 * "operations-supported" attribute
 */
bool
ipp_printer_has_op (const ipp_printer *printer, int op);

/* Name of an IPP operation code, for display. Never returns NULL.
 */
const char*
ipp_op_name (int op);

/* Name of a printer state, for display. Never returns NULL.
 */
const char*
ipp_state_name (int state);

/******************** Scan jobs ********************/
/* What a scan job is asked to produce.
 *
 * A NULL string or a zero number leaves the attribute out of the
 * request, and the service scans with a default of its own. The
 * keywords are IPP ones, as reported by ipp_scanner.
 */
typedef struct {
    const char *format;         /* document-format-accepted */
    const char *color_mode;     /* input-color-mode */
    const char *source;         /* input-source */
    const char *sides;          /* input-sides */
    int        resolution;      /* input-resolution, in DPI */

    /* input-scan-regions, in hundredths of a millimetre. Sent only if
     * have_region is set, as a service that scans a fixed area rejects
     * a region it did not offer.
     */
    bool       have_region;
    int        x_origin;
    int        y_origin;
    int        x_dimension;
    int        y_dimension;
} ipp_job_params;

/* A scan job in progress
 */
typedef struct ipp_job ipp_job;

/* What ipp_job_next_document() found.
 *
 * A scan service does not hold the connection open until the scanner
 * has something to give: it answers at once, and says whether the
 * answer carries an image, or is only an invitation to ask again.
 */
typedef enum {
    IPP_DOC_READY,              /* Image started, read it */
    IPP_DOC_WAIT,               /* Not yet, ask again later */
    IPP_DOC_DONE,               /* No more images, the job is over */
    IPP_DOC_ERROR               /* The job failed */
} ipp_doc_status;

/* Start a scan job on the service at the given URI.
 *
 * The connection is kept open for the life of the job, because the
 * image data arrives on it. timeout_ms bounds each exchange, not the
 * job as a whole: scanning a page takes as long as it takes, and the
 * waiting is done between requests rather than inside one.
 *
 * Returns the job, to be released with ipp_job_free(). On error returns
 * NULL and, when errbuf is not NULL, writes there a description of what
 * went wrong.
 */
ipp_job*
ipp_job_create (const char *uri, const ipp_job_params *params,
        int timeout_ms, char *errbuf, size_t errlen);

/* Ask for the next image of the job.
 *
 * On IPP_DOC_WAIT the caller is to wait *wait_sec seconds and call
 * again. On IPP_DOC_READY the image is read with ipp_job_read() until
 * that reports the end, and then this is called again for the image
 * after it. wait_sec may be NULL.
 */
ipp_doc_status
ipp_job_next_document (ipp_job *job, int *wait_sec,
        char *errbuf, size_t errlen);

/* MIME type of the image ipp_job_next_document() last started, such as
 * "image/jpeg". It stays valid until the next image is asked for, so
 * that a caller can decode what it has read. NULL before the first
 * image.
 */
const char*
ipp_job_document_format (const ipp_job *job);

/* Read image data.
 *
 * Returns the number of bytes read, 0 at the end of the image, or -1 on
 * error, writing a description to errbuf.
 */
ssize_t
ipp_job_read (ipp_job *job, void *data, size_t size,
        char *errbuf, size_t errlen);

/* Abort the job.
 *
 * Safe to call at any point, including in the middle of an image and on
 * a job that has already finished. The job is still to be released with
 * ipp_job_free().
 */
void
ipp_job_cancel (ipp_job *job);

/* Release the job, cancelling it first if it has not finished
 */
void
ipp_job_free (ipp_job *job);

/* Enable/disable protocol debug messages on stderr. Off by default.
 */
void
ipp_proto_debug_enable (bool enable);

#ifdef __cplusplus
}
#endif

#endif
