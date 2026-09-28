/* sane-ipp -- IPP backend for SANE
 *
 * Copyright (C) 2026 Alexander Pevzner (pzz@apevzner.com)
 * Copyright (C) 2026 Yogesh Singla (yogeshsingla481@gmail.com)
 * SPDX-License-Identifier: BSD-2-Clause
 * See LICENSE for license terms and conditions
 *
 * Scan sessions
 */

#include "ipp-scan.h"
#include "ipp-png.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/******************** Constants ********************/
/* How much encoded data is taken from the network at a time.
 *
 * This bounds how far the decoder can run ahead of the frontend: one
 * buffer is decoded in full before the frontend is given its rows, so
 * a smaller buffer holds fewer decoded rows at once, at the cost of more
 * trips to the network.
 */
#define IPP_SCAN_BUFSIZE        65536

/* How often a wait for the next page looks for a cancellation, in
 * milliseconds
 */
#define IPP_SCAN_WAIT_STEP_MS   100

/******************** Local Types ********************/
/* A scan session
 */
struct ipp_scan {
    ipp_job                     *job;       /* The job */
    bool                        multi;      /* Source feeds several pages */
    int                         pages;      /* Pages started so far */
    const volatile sig_atomic_t *cancel;    /* Raised to abandon the scan */

    ipp_png                     *png;       /* Page being read, or NULL */
    bool                        net_eof;    /* Its data has all arrived */

    uint8_t                     buf[IPP_SCAN_BUFSIZE];
};

/******************** Small helpers ********************/
/* Format an error message into the caller's buffer. Tolerates a NULL
 * buffer, so callers that do not want the message need no special case.
 */
static void
ipp_scan_err (char *errbuf, size_t errlen, const char *fmt, ...)
    __attribute__ ((format (printf, 3, 4)));

static void
ipp_scan_err (char *errbuf, size_t errlen, const char *fmt, ...)
{
    va_list ap;

    if (errbuf == NULL || errlen == 0) {
        return;
    }

    va_start(ap, fmt);
    vsnprintf(errbuf, errlen, fmt, ap);
    va_end(ap);
}

/* Report whether the caller has asked to abandon the scan
 */
static bool
ipp_scan_cancelled (const ipp_scan *scan)
{
    return *scan->cancel != 0;
}

/* Wait for the given number of seconds, as a service that has no page
 * yet asks us to.
 *
 * The wait is taken in small steps, so that a cancellation does not
 * have to sit it out. Returns false if the scan was cancelled.
 */
static bool
ipp_scan_sleep (const ipp_scan *scan, int seconds)
{
    struct timespec step = {
        .tv_sec  = 0,
        .tv_nsec = IPP_SCAN_WAIT_STEP_MS * 1000000L
    };
    int             steps = seconds * (1000 / IPP_SCAN_WAIT_STEP_MS);
    int             i;

    for (i = 0; i < steps; i ++) {
        if (ipp_scan_cancelled(scan)) {
            return false;
        }

        nanosleep(&step, NULL);
    }

    return !ipp_scan_cancelled(scan);
}

/******************** Page data ********************/
/* Move one buffer of encoded data from the network into the decoder.
 *
 * At the end of the data the decoder is told so, which is where a page
 * that was cut short comes to light.
 */
static SANE_Status
ipp_scan_feed (ipp_scan *scan, char *errbuf, size_t errlen)
{
    ssize_t n;

    if (ipp_scan_cancelled(scan)) {
        return SANE_STATUS_CANCELLED;
    }

    /* The decoder accepted the end of the data, so it has everything
     * it will ever get, and still wants more. It cannot come to this,
     * but a loop waiting on it would never end if it did.
     */
    if (scan->net_eof) {
        ipp_scan_err(errbuf, errlen, "image data ended early");
        return SANE_STATUS_IO_ERROR;
    }

    n = ipp_job_read(scan->job, scan->buf, sizeof(scan->buf),
            errbuf, errlen);

    if (n < 0) {
        return SANE_STATUS_IO_ERROR;
    }

    if (ipp_scan_cancelled(scan)) {
        return SANE_STATUS_CANCELLED;
    }

    if (n == 0) {
        scan->net_eof = true;

        if (ipp_png_finish(scan->png, errbuf, errlen) < 0) {
            return SANE_STATUS_IO_ERROR;
        }

        return SANE_STATUS_GOOD;
    }

    if (ipp_png_write(scan->png, scan->buf, (size_t) n, errbuf, errlen) < 0) {
        return SANE_STATUS_IO_ERROR;
    }

    return SANE_STATUS_GOOD;
}

/* The image has been decoded in full: read the response to its end.
 *
 * The image can end before the response carrying it does, if only by
 * the few bytes that close an HTTP chunked body. Until those are read
 * the connection is still busy with the image, and cancelling the job
 * would take a connection of its own.
 *
 * The page is whole whatever happens here, so a failure to read what
 * follows it is no reason to report the page as failed. It will show
 * up again, if it matters, when the next page is asked for.
 */
static SANE_Status
ipp_scan_page_end (ipp_scan *scan)
{
    while (!scan->net_eof) {
        if (ipp_job_read(scan->job, scan->buf, sizeof(scan->buf),
                NULL, 0) <= 0) {
            scan->net_eof = true;
        }
    }

    return SANE_STATUS_EOF;
}

/******************** Session ********************/
/* Start a scan job
 */
ipp_scan*
ipp_scan_new (const char *uri, const ipp_job_params *params, bool multi,
        int timeout_ms, const volatile sig_atomic_t *cancel,
        char *errbuf, size_t errlen)
{
    ipp_scan *scan;

    if (errbuf != NULL && errlen != 0) {
        errbuf[0] = '\0';
    }

    /* The session is in a single piece of memory with its buffer, so
     * it is allocated before the job, which would otherwise have to be
     * cancelled over nothing more than a failed allocation.
     */
    scan = calloc(1, sizeof(*scan));
    if (scan == NULL) {
        ipp_scan_err(errbuf, errlen, "out of memory");
        return NULL;
    }

    scan->job = ipp_job_create(uri, params, timeout_ms, errbuf, errlen);
    if (scan->job == NULL) {
        free(scan);
        return NULL;
    }

    scan->multi = multi;
    scan->cancel = cancel;

    return scan;
}

/* Wait for the next page and read its header
 */
SANE_Status
ipp_scan_next_page (ipp_scan *scan, char *errbuf, size_t errlen)
{
    const char *format;

    if (errbuf != NULL && errlen != 0) {
        errbuf[0] = '\0';
    }

    ipp_png_free(scan->png);
    scan->png = NULL;
    scan->net_eof = false;

    for (;;) {
        int wait_sec;

        if (ipp_scan_cancelled(scan)) {
            return SANE_STATUS_CANCELLED;
        }

        switch (ipp_job_next_document(scan->job, &wait_sec, errbuf, errlen)) {
        case IPP_DOC_READY:
            goto READY;

        case IPP_DOC_WAIT:
            if (!ipp_scan_sleep(scan, wait_sec)) {
                return SANE_STATUS_CANCELLED;
            }
            break;

        case IPP_DOC_DONE:
            /* A feeder that runs out has done its job. A platen that
             * gives nothing at all has not: it always has a page on it,
             * blank or not, so a job that ends without one has failed.
             */
            if (scan->multi || scan->pages != 0) {
                return SANE_STATUS_NO_DOCS;
            }

            ipp_scan_err(errbuf, errlen, "the job ended without an image");
            return SANE_STATUS_IO_ERROR;

        case IPP_DOC_ERROR:
            return SANE_STATUS_IO_ERROR;
        }
    }

READY:
    /* The format was asked for when the job was created, but the service
     * is free to send another, and one we cannot decode is not a page we
     * can hand over.
     */
    format = ipp_job_document_format(scan->job);
    if (!ipp_png_format_supported(format)) {
        ipp_scan_err(errbuf, errlen, "%s: unsupported image format",
                format != NULL ? format : "(none)");
        return SANE_STATUS_UNSUPPORTED;
    }

    scan->png = ipp_png_new(format, errbuf, errlen);
    if (scan->png == NULL) {
        return SANE_STATUS_NO_MEM;
    }

    scan->pages ++;

    /* The frontend asks for the size of the page before it reads any of
     * it, and the size arrives with the header, so the header is read
     * here. The rows that come with it wait in the decoder.
     */
    while (!ipp_png_have_params(scan->png)) {
        SANE_Status status = ipp_scan_feed(scan, errbuf, errlen);

        if (status != SANE_STATUS_GOOD) {
            return status;
        }
    }

    return SANE_STATUS_GOOD;
}

/* Report the parameters of the page being read
 */
void
ipp_scan_parameters (const ipp_scan *scan, SANE_Parameters *params)
{
    int width, height, depth, channels;

    ipp_png_params(scan->png, &width, &height, &depth, &channels);

    params->format = channels == 1 ? SANE_FRAME_GRAY : SANE_FRAME_RGB;
    params->last_frame = SANE_TRUE;
    params->bytes_per_line = (SANE_Int) ipp_png_bytes_per_line(scan->png);
    params->pixels_per_line = width;
    params->lines = height;
    params->depth = depth;
}

/* Read the pixels of the page.
 *
 * The decoder is drained before the network is touched again, so the
 * pace of the frontend sets the pace of the transfer, and the rows held
 * here at any time are those of a single buffer of encoded data.
 */
SANE_Status
ipp_scan_read (ipp_scan *scan, SANE_Byte *data, SANE_Int max_length,
        SANE_Int *length, char *errbuf, size_t errlen)
{
    if (errbuf != NULL && errlen != 0) {
        errbuf[0] = '\0';
    }

    *length = 0;

    if (scan->png == NULL) {
        return SANE_STATUS_EOF;
    }

    for (;;) {
        SANE_Status status;
        size_t      n;

        if (ipp_scan_cancelled(scan)) {
            return SANE_STATUS_CANCELLED;
        }

        n = ipp_png_read(scan->png, data, (size_t) max_length);
        if (n != 0) {
            *length = (SANE_Int) n;
            return SANE_STATUS_GOOD;
        }

        if (ipp_png_eof(scan->png)) {
            return ipp_scan_page_end(scan);
        }

        status = ipp_scan_feed(scan, errbuf, errlen);
        if (status != SANE_STATUS_GOOD) {
            return status;
        }
    }
}

/* Let the job end by itself.
 *
 * A job that has handed over every page it had is not a job to cancel:
 * a service may count a cancelled job as a failed one, and keep the
 * scanner busy until it has been told the job is over. The service is
 * asked for the next page instead, and its answer that there is none
 * is what ends the job.
 *
 * Only an answer given at once is taken. A service that asks to be
 * polled later is not waited for, as nobody is waiting for its page:
 * the job is left to ipp_scan_free(), which cancels it.
 */
void
ipp_scan_finish (ipp_scan *scan)
{
    if (scan == NULL) {
        return;
    }

    ipp_png_free(scan->png);
    scan->png = NULL;

    (void) ipp_job_next_document(scan->job, NULL, NULL, 0);
}

/* Release the session
 */
void
ipp_scan_free (ipp_scan *scan)
{
    if (scan == NULL) {
        return;
    }

    ipp_png_free(scan->png);
    ipp_job_free(scan->job);
    free(scan);
}
