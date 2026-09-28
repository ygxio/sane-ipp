/* sane-ipp -- IPP backend for SANE
 *
 * Copyright (C) 2026 Alexander Pevzner (pzz@apevzner.com)
 * Copyright (C) 2026 Yogesh Singla (yogeshsingla481@gmail.com)
 * SPDX-License-Identifier: BSD-2-Clause
 * See LICENSE for license terms and conditions
 *
 * Scan sessions
 *
 * A scan job hands out encoded images and a SANE frontend asks for rows
 * of pixels. This layer joins the job to the decoder, one page at a
 * time, and speaks SANE to the caller: its results are SANE statuses and
 * SANE parameters, so that the entry points above it have nothing left
 * to translate.
 *
 * Everything here blocks. The network is read only when the decoder has
 * no pixels left to give, so a frontend that reads slowly holds back the
 * device rather than filling our memory with a page it has yet to take.
 */

#ifndef ipp_scan_h
#define ipp_scan_h

#include "ipp-proto.h"

#include <sane/sane.h>

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A scan job, and the page of it being read
 */
typedef struct ipp_scan ipp_scan;

/* Start a scan job.
 *
 * multi tells whether the source can feed more than one page, which
 * decides whether a job that runs out of pages has merely emptied the
 * feeder or has failed to scan at all.
 *
 * cancel points to a flag the caller may raise at any time, from a
 * signal handler included, to abandon whatever is in progress. It must
 * outlive the session.
 *
 * Returns the session, to be released with ipp_scan_free(). On error
 * returns NULL and, when errbuf is not NULL, writes there a description
 * of what went wrong.
 */
ipp_scan*
ipp_scan_new (const char *uri, const ipp_job_params *params, bool multi,
        int timeout_ms, const volatile sig_atomic_t *cancel,
        char *errbuf, size_t errlen);

/* Wait for the next page and read up to the end of its header, so that
 * its size is known.
 *
 * Returns SANE_STATUS_GOOD when a page is ready to be read, and
 * SANE_STATUS_NO_DOCS when the job has no more pages to give. Anything
 * else is an error, described in errbuf.
 */
SANE_Status
ipp_scan_next_page (ipp_scan *scan, char *errbuf, size_t errlen);

/* Report the parameters of the page being read. Meaningless unless
 * ipp_scan_next_page() has returned SANE_STATUS_GOOD.
 */
void
ipp_scan_parameters (const ipp_scan *scan, SANE_Parameters *params);

/* Read the pixels of the page.
 *
 * Returns SANE_STATUS_GOOD with *length set to the number of bytes
 * stored, which is never 0, or SANE_STATUS_EOF when the page has been
 * read in full. Anything else is an error, described in errbuf.
 */
SANE_Status
ipp_scan_read (ipp_scan *scan, SANE_Byte *data, SANE_Int max_length,
        SANE_Int *length, char *errbuf, size_t errlen);

/* Let the job end by itself, now that the caller wants no more pages.
 *
 * The session is still to be released with ipp_scan_free(), which
 * cancels the job if this could not end it.
 */
void
ipp_scan_finish (ipp_scan *scan);

/* Release the session, cancelling the job if it has not finished
 */
void
ipp_scan_free (ipp_scan *scan);

#ifdef __cplusplus
}
#endif

#endif
