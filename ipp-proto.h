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

#ifdef __cplusplus
extern "C" {
#endif

/* Printer state, the values of the IPP "printer-state" attribute
 */
#define IPP_PRINTER_STATE_IDLE          3
#define IPP_PRINTER_STATE_PROCESSING    4
#define IPP_PRINTER_STATE_STOPPED       5

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

/* Enable/disable protocol debug messages on stderr. Off by default.
 */
void
ipp_proto_debug_enable (bool enable);

#ifdef __cplusplus
}
#endif

#endif
