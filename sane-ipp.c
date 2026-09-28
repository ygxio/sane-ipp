/* sane-ipp -- IPP backend for SANE
 *
 * Copyright (C) 2026 Alexander Pevzner (pzz@apevzner.com)
 * Copyright (C) 2026 Yogesh Singla (yogeshsingla481@gmail.com)
 * SPDX-License-Identifier: BSD-2-Clause
 * See LICENSE for license terms and conditions
 *
 * SANE API entry points
 *
 * Discovery, opening a device, its options and scanning are implemented
 * here and in the layers below. I/O is blocking only: there is no file
 * descriptor to hand a frontend that wants to wait for data itself.
 */

#include "ipp-opt.h"
#include "ipp-proto.h"
#include "ipp-scan.h"
#include "ipp-zeroconf.h"

#include <sane/sane.h>

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/******************** Constants ********************/
/* How long to browse the network in sane_get_devices()
 */
#define IPP_SANE_DISCOVERY_TIMEOUT      2500

/* How long to wait for a printer to answer in sane_open()
 */
#define IPP_SANE_OPEN_TIMEOUT           5000

/* How long to wait for each exchange of a scan job. A scanner may be
 * slow to produce the next part of a page, so this is more generous
 * than the timeout for opening a device.
 */
#define IPP_SANE_JOB_TIMEOUT            30000

/* The only image format we can decode, and so the one we ask for
 */
#define IPP_SANE_FORMAT                 "image/png"

/******************** Local Types ********************/
/* An open device. The SANE_Handle of this backend is a pointer to it.
 *
 * sane_cancel() may be called from a signal handler, in the middle of
 * sane_start() or sane_read(), so all it does is raise the cancel flag.
 * Whichever of them runs next notices it, gives up, and does the
 * tearing down.
 *
 * Frontends also call sane_cancel() when a scan has gone all the way,
 * to say they are done, and some of them after every page. So the flag
 * is only raised while a page is being scanned, from sane_start() until
 * sane_read() reports the end of it; outside of that there is nothing
 * to cancel, and a job still open is ended the ordinary way.
 */
typedef struct {
    char        *uri;       /* Device URI, as given to sane_open() */
    ipp_printer *printer;   /* Attributes, fetched when opened */
    ipp_opt     *opt;       /* Options, built from those attributes */

    ipp_scan    *scan;      /* Scan job in progress, or NULL */
    bool        multi;      /* Its source feeds several pages */
    bool        page;       /* A page of it has been started */

    volatile sig_atomic_t scanning; /* Between sane_start() and the end
                                       of the page */
    volatile sig_atomic_t cancel;   /* sane_cancel() came while scanning */

    char        err[512];   /* Why the last operation failed */
} ipp_sane_handle;

/******************** Static variables ********************/
static bool               ipp_sane_initialized;
static bool               ipp_sane_debug;

/* Devices found by the most recent sane_get_devices(). The strings
 * handed out in ipp_sane_devs point into this list, so it must outlive
 * the list reported to the caller.
 */
static ipp_zc_device      *ipp_sane_found;
static SANE_Device        *ipp_sane_devs;
static const SANE_Device  **ipp_sane_devlist;

/* Reported by sane_get_devices() when the caller asked for local
 * devices only. It is static because the SANE API does not give the
 * caller a way to release it.
 */
static const SANE_Device  *ipp_sane_devlist_empty[1] = { NULL };

/******************** Debugging ********************/
/* Print a debug message, if SANE_DEBUG_IPP asked for them.
 *
 * A SANE status says little about what went wrong, and a frontend has
 * nowhere else to learn it, so this is where the reason goes.
 */
static void
ipp_sane_dbg (const char *fmt, ...)
    __attribute__ ((format (printf, 1, 2)));

static void
ipp_sane_dbg (const char *fmt, ...)
{
    va_list ap;

    if (!ipp_sane_debug) {
        return;
    }

    va_start(ap, fmt);
    fprintf(stderr, "ipp: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

/******************** Device list ********************/
/* Release the device list built by the previous sane_get_devices()
 */
static void
ipp_sane_devlist_free (void)
{
    free(ipp_sane_devlist);
    free(ipp_sane_devs);
    ipp_zc_device_list_free(ipp_sane_found);

    ipp_sane_devlist = NULL;
    ipp_sane_devs = NULL;
    ipp_sane_found = NULL;
}

/* Report whether a discovered device is worth showing to a frontend.
 *
 * A device has to be reachable, named, and say that it scans. The DNS-SD
 * "rs" key is only a hint -- the attributes settle it when the device
 * is opened -- but it costs nothing here and keeps print-only services
 * out of a list of scanners.
 */
static bool
ipp_sane_device_usable (const ipp_zc_device *device)
{
    return device->endpoints != NULL && device->name != NULL && device->scan;
}

/* Build the SANE device list out of the discovered devices.
 *
 * The SANE_Device strings are not copied. They point into the device
 * list, which is kept until the next call or until sane_exit().
 */
static SANE_Status
ipp_sane_devlist_build (ipp_zc_device *found)
{
    ipp_zc_device *device;
    size_t        count = 0, i;

    for (device = found; device != NULL; device = device->next) {
        if (ipp_sane_device_usable(device)) {
            count ++;
        }
    }

    ipp_sane_devs = calloc(count + 1, sizeof(*ipp_sane_devs));
    ipp_sane_devlist = calloc(count + 1, sizeof(*ipp_sane_devlist));

    if (ipp_sane_devs == NULL || ipp_sane_devlist == NULL) {
        free(ipp_sane_devs);
        free(ipp_sane_devlist);
        ipp_sane_devs = NULL;
        ipp_sane_devlist = NULL;
        return SANE_STATUS_NO_MEM;
    }

    i = 0;
    for (device = found; device != NULL; device = device->next) {
        if (!ipp_sane_device_usable(device)) {
            continue;
        }

        /* The device is named by its DNS-SD instance name rather than by
         * a URI, because it may be reachable at several of them and the
         * best one is chosen when the device is opened.
         */
        ipp_sane_devs[i].name = device->name;
        ipp_sane_devs[i].vendor = "IPP";
        ipp_sane_devs[i].model =
            device->model != NULL ? device->model : device->name;
        ipp_sane_devs[i].type = "multi-function peripheral";

        ipp_sane_devlist[i] = &ipp_sane_devs[i];
        i ++;
    }

    ipp_sane_devlist[i] = NULL;

    return SANE_STATUS_GOOD;
}

/******************** SANE API ********************/
/* Initialize the backend
 */
SANE_Status
sane_init (SANE_Int *version_code, SANE_Auth_Callback authorize)
{
    const char *debug = getenv("SANE_DEBUG_IPP");

    (void) authorize;

    ipp_sane_debug = debug != NULL && *debug != '\0' && strcmp(debug, "0") != 0;
    ipp_proto_debug_enable(ipp_sane_debug);

    if (version_code != NULL) {
        *version_code = SANE_VERSION_CODE(SANE_CURRENT_MAJOR,
                SANE_CURRENT_MINOR, 0);
    }

    ipp_sane_initialized = true;

    return SANE_STATUS_GOOD;
}

/* Release all resources held by the backend
 */
void
sane_exit (void)
{
    ipp_sane_devlist_free();
    ipp_sane_initialized = false;
}

/* Get the list of available devices
 */
SANE_Status
sane_get_devices (const SANE_Device ***device_list, SANE_Bool local_only)
{
    ipp_zc_device *found;
    const char    *err;
    SANE_Status   status;

    if (device_list == NULL) {
        return SANE_STATUS_INVAL;
    }

    if (!ipp_sane_initialized) {
        return SANE_STATUS_INVAL;
    }

    /* Every device we can find is reached over the network, so there is
     * nothing to report when the caller wants local devices only.
     */
    if (local_only) {
        *device_list = ipp_sane_devlist_empty;
        return SANE_STATUS_GOOD;
    }

    ipp_sane_devlist_free();

    found = ipp_zeroconf_discover(IPP_SANE_DISCOVERY_TIMEOUT, &err);
    if (err != NULL) {
        return SANE_STATUS_IO_ERROR;
    }

    status = ipp_sane_devlist_build(found);
    if (status != SANE_STATUS_GOOD) {
        ipp_zc_device_list_free(found);
        return status;
    }

    ipp_sane_found = found;
    *device_list = ipp_sane_devlist;

    return SANE_STATUS_GOOD;
}

/* Report whether a name is an IPP URI rather than a device name
 */
static bool
ipp_sane_name_is_uri (const char *name)
{
    return strncmp(name, "ipp://", 6) == 0 ||
           strncmp(name, "ipps://", 7) == 0;
}

/* Build a handle for a device reached at the given URI
 */
static SANE_Status
ipp_sane_handle_new (const char *uri, ipp_sane_handle **out)
{
    ipp_sane_handle *h;
    ipp_printer     *printer;
    char            err[512];

    printer = ipp_get_printer_attributes(uri, IPP_SANE_OPEN_TIMEOUT,
            err, sizeof(err));

    if (printer == NULL) {
        return SANE_STATUS_IO_ERROR;
    }

    if (printer->scanner == NULL) {
        ipp_printer_free(printer);
        return SANE_STATUS_UNSUPPORTED;
    }

    h = calloc(1, sizeof(*h));
    if (h == NULL) {
        ipp_printer_free(printer);
        return SANE_STATUS_NO_MEM;
    }

    h->uri = strdup(uri);
    if (h->uri == NULL) {
        ipp_printer_free(printer);
        free(h);
        return SANE_STATUS_NO_MEM;
    }

    h->opt = ipp_opt_new(printer->scanner);
    if (h->opt == NULL) {
        ipp_printer_free(printer);
        free(h->uri);
        free(h);
        return SANE_STATUS_NO_MEM;
    }

    h->printer = printer;
    *out = h;

    return SANE_STATUS_GOOD;
}

/* Open a device, trying its endpoints in turn.
 *
 * The endpoints are ordered best first, but the best one is not always
 * the one that answers, so each is tried until one does. An endpoint
 * that answers without offering a scan service is no better than one
 * that does not answer at all, and the next is tried just the same.
 *
 * The failure reported is the most informative one seen, not the last.
 * A device is normally advertised on more addresses than it listens on,
 * so an endpoint that answered and said it does not scan tells us more
 * about the device than the ones that never answered at all.
 */
static SANE_Status
ipp_sane_device_open (const ipp_zc_device *device, ipp_sane_handle **out)
{
    const ipp_endpoint *endpoint;
    SANE_Status        status = SANE_STATUS_IO_ERROR;

    for (endpoint = device->endpoints; endpoint != NULL;
            endpoint = endpoint->next) {
        SANE_Status s = ipp_sane_handle_new(endpoint->uri, out);

        if (s == SANE_STATUS_GOOD) {
            return s;
        }

        if (s == SANE_STATUS_UNSUPPORTED) {
            status = s;
        }
    }

    return status;
}

/* Open the device
 */
SANE_Status
sane_open (SANE_String_Const devicename, SANE_Handle *handle)
{
    ipp_sane_handle *h = NULL;
    ipp_zc_device   *found, *device;
    const char      *err;
    SANE_Status     status;

    if (handle == NULL) {
        return SANE_STATUS_INVAL;
    }

    if (!ipp_sane_initialized) {
        return SANE_STATUS_INVAL;
    }

    /* A URI addresses a device directly, which is useful for a device
     * that discovery cannot see, and for testing.
     */
    if (devicename != NULL && ipp_sane_name_is_uri(devicename)) {
        status = ipp_sane_handle_new(devicename, &h);
        if (status == SANE_STATUS_GOOD) {
            *handle = (SANE_Handle) h;
        }
        return status;
    }

    found = ipp_zeroconf_discover(IPP_SANE_DISCOVERY_TIMEOUT, &err);
    if (err != NULL) {
        return SANE_STATUS_IO_ERROR;
    }

    /* An empty name means the first device, which is what a frontend
     * invoked without an explicit device expects.
     */
    if (devicename == NULL || *devicename == '\0') {
        device = found;
        while (device != NULL && device->endpoints == NULL) {
            device = device->next;
        }
    } else {
        for (device = found; device != NULL; device = device->next) {
            if (device->name != NULL &&
                strcasecmp(device->name, devicename) == 0) {
                break;
            }
        }
    }

    if (device == NULL) {
        ipp_zc_device_list_free(found);
        return SANE_STATUS_INVAL;
    }

    status = ipp_sane_device_open(device, &h);

    ipp_zc_device_list_free(found);

    if (status == SANE_STATUS_GOOD) {
        *handle = (SANE_Handle) h;
    }

    return status;
}

/* Close the device
 */
void
sane_close (SANE_Handle handle)
{
    ipp_sane_handle *h = (ipp_sane_handle*) handle;

    if (h == NULL) {
        return;
    }

    ipp_scan_free(h->scan);
    ipp_opt_free(h->opt);
    ipp_printer_free(h->printer);
    free(h->uri);
    free(h);
}

/* Return a string describing the status
 */
SANE_String_Const
sane_strstatus (SANE_Status status)
{
    switch (status) {
    case SANE_STATUS_GOOD:          return "Success";
    case SANE_STATUS_UNSUPPORTED:   return "Operation not supported";
    case SANE_STATUS_CANCELLED:     return "Operation was cancelled";
    case SANE_STATUS_DEVICE_BUSY:   return "Device busy";
    case SANE_STATUS_INVAL:         return "Invalid argument";
    case SANE_STATUS_EOF:           return "End of file reached";
    case SANE_STATUS_JAMMED:        return "Document feeder jammed";
    case SANE_STATUS_NO_DOCS:       return "Document feeder out of documents";
    case SANE_STATUS_COVER_OPEN:    return "Scanner cover is open";
    case SANE_STATUS_IO_ERROR:      return "Error during device I/O";
    case SANE_STATUS_NO_MEM:        return "Out of memory";
    case SANE_STATUS_ACCESS_DENIED: return "Access to resource has been denied";
    }

    return "Unknown SANE status";
}

/******************** Options ********************/
/* Get the option descriptor
 */
const SANE_Option_Descriptor*
sane_get_option_descriptor (SANE_Handle handle, SANE_Int option)
{
    ipp_sane_handle *h = (ipp_sane_handle*) handle;

    if (h == NULL) {
        return NULL;
    }

    return ipp_opt_descriptor(h->opt, option);
}

/* Get or set an option
 */
SANE_Status
sane_control_option (SANE_Handle handle, SANE_Int option, SANE_Action action,
        void *value, SANE_Int *info)
{
    ipp_sane_handle *h = (ipp_sane_handle*) handle;

    if (h == NULL || value == NULL) {
        return SANE_STATUS_INVAL;
    }

    if (info != NULL) {
        *info = 0;
    }

    switch (action) {
    case SANE_ACTION_GET_VALUE:
        return ipp_opt_get(h->opt, option, value);

    case SANE_ACTION_SET_VALUE:
        return ipp_opt_set(h->opt, option, value, info);

    case SANE_ACTION_SET_AUTO:
        /* Nothing here has an automatic setting to fall back on: every
         * option is a choice among what the device reported, and the
         * value it starts out with is already the best guess we have.
         */
        return SANE_STATUS_UNSUPPORTED;
    }

    return SANE_STATUS_INVAL;
}

/******************** Scanning ********************/
/* End the scan job, if there is one
 */
static void
ipp_sane_scan_end (ipp_sane_handle *h)
{
    ipp_scan_free(h->scan);
    h->scan = NULL;
    h->page = false;
}

/* Report whether the device can send an image we can decode.
 *
 * A device that lists no formats at all is given the benefit of the
 * doubt: the format it sends is checked again when the image arrives.
 */
static bool
ipp_sane_format_ok (const ipp_printer *printer)
{
    size_t i;

    if (printer->n_formats == 0) {
        return true;
    }

    for (i = 0; i < printer->n_formats; i ++) {
        if (strcasecmp(printer->formats[i], IPP_SANE_FORMAT) == 0) {
            return true;
        }
    }

    return false;
}

/* Report whether an input-color-mode keyword gives one channel per
 * pixel. These are the grey and black-and-white modes, which are the
 * ones whose names say so; everything else is colour.
 */
static bool
ipp_sane_mode_is_gray (const char *keyword)
{
    return keyword != NULL &&
           (strncmp(keyword, "monochrome", 10) == 0 ||
            strcmp(keyword, "bi-level") == 0 ||
            strcmp(keyword, "auto-monochrome") == 0 ||
            strcmp(keyword, "process-monochrome") == 0 ||
            strcmp(keyword, "process-bi-level") == 0);
}

/* Estimate the parameters of a scan not yet started.
 *
 * A frontend asks before it starts, to size its window or its output
 * file, and the real answer only comes with the image. The estimate is
 * what the options amount to; the device may still send something else,
 * which the frontend learns when it asks again after sane_start().
 *
 * Decoded images are 8 or 16 bits per channel, and the device picks
 * which, so 8 is assumed.
 */
static void
ipp_sane_params_estimate (const ipp_sane_handle *h, SANE_Parameters *params)
{
    bool gray = ipp_sane_mode_is_gray(ipp_opt_color_mode(h->opt));
    int  dpi = ipp_opt_resolution(h->opt);
    int  x, y, width, height;

    params->format = gray ? SANE_FRAME_GRAY : SANE_FRAME_RGB;
    params->last_frame = SANE_TRUE;
    params->depth = 8;
    params->pixels_per_line = 0;
    params->lines = -1;

    if (dpi > 0 && ipp_opt_scan_region(h->opt, &x, &y, &width, &height)) {
        /* The region is in hundredths of a millimetre, 2540 to the inch
         */
        params->pixels_per_line = (SANE_Int) ((long long) width * dpi / 2540);
        params->lines = (SANE_Int) ((long long) height * dpi / 2540);
    }

    params->bytes_per_line = params->pixels_per_line * (gray ? 1 : 3);
}

/* Create the scan job, out of the current options
 */
static SANE_Status
ipp_sane_job_start (ipp_sane_handle *h)
{
    ipp_job_params params;
    const char     *source;

    if (!ipp_sane_format_ok(h->printer)) {
        snprintf(h->err, sizeof(h->err), "device does not offer %s",
                IPP_SANE_FORMAT);
        return SANE_STATUS_UNSUPPORTED;
    }

    memset(&params, 0, sizeof(params));

    params.format = IPP_SANE_FORMAT;
    params.color_mode = ipp_opt_color_mode(h->opt);
    params.source = ipp_opt_source(h->opt);
    params.sides = ipp_opt_sides(h->opt);
    params.resolution = ipp_opt_resolution(h->opt);
    params.have_region = ipp_opt_scan_region(h->opt,
            &params.x_origin, &params.y_origin,
            &params.x_dimension, &params.y_dimension);

    /* Anything but the platen is taken to be a feeder of some kind. A
     * device that names no source is scanning from its only one, which
     * is its platen.
     */
    source = params.source;
    h->multi = source != NULL && strcmp(source, "platen") != 0;

    h->scan = ipp_scan_new(h->uri, &params, h->multi, IPP_SANE_JOB_TIMEOUT,
            &h->cancel, h->err, sizeof(h->err));

    if (h->scan == NULL) {
        return h->cancel ? SANE_STATUS_CANCELLED : SANE_STATUS_IO_ERROR;
    }

    return SANE_STATUS_GOOD;
}

/* Start the next page, creating the job first if there is none.
 *
 * A job scanning from a feeder is kept from one page to the next, and
 * each page after the first is only the next image of it. A job on the
 * platen gives one image, so each page is a job of its own.
 */
static SANE_Status
ipp_sane_start (ipp_sane_handle *h)
{
    SANE_Status status;

    if (h->scan != NULL && !h->multi) {
        ipp_sane_scan_end(h);
    }

    h->page = false;

    if (h->scan == NULL) {
        status = ipp_sane_job_start(h);
        if (status != SANE_STATUS_GOOD) {
            return status;
        }
    }

    status = ipp_scan_next_page(h->scan, h->err, sizeof(h->err));
    if (status == SANE_STATUS_GOOD) {
        h->page = true;
    }

    return status;
}

/* Get the parameters of the next scan
 */
SANE_Status
sane_get_parameters (SANE_Handle handle, SANE_Parameters *params)
{
    ipp_sane_handle *h = (ipp_sane_handle*) handle;

    if (h == NULL || params == NULL) {
        return SANE_STATUS_INVAL;
    }

    if (h->page) {
        ipp_scan_parameters(h->scan, params);
    } else {
        ipp_sane_params_estimate(h, params);
    }

    return SANE_STATUS_GOOD;
}

/* Start scanning
 */
SANE_Status
sane_start (SANE_Handle handle)
{
    ipp_sane_handle *h = (ipp_sane_handle*) handle;
    SANE_Status     status;

    if (h == NULL) {
        return SANE_STATUS_INVAL;
    }

    /* A cancel that came after the last sane_read() had nobody to act
     * on it, so the job it was meant for is ended here
     */
    if (h->cancel) {
        ipp_sane_scan_end(h);
    }

    h->err[0] = '\0';
    h->cancel = 0;
    h->scanning = 1;

    status = ipp_sane_start(h);

    if (status != SANE_STATUS_GOOD) {
        h->scanning = 0;

        if (h->err[0] != '\0') {
            ipp_sane_dbg("sane_start: %s: %s", sane_strstatus(status), h->err);
        }

        ipp_sane_scan_end(h);
    }

    return status;
}

/* Read the next portion of the image
 */
SANE_Status
sane_read (SANE_Handle handle, SANE_Byte *data, SANE_Int max_length,
        SANE_Int *length)
{
    ipp_sane_handle *h = (ipp_sane_handle*) handle;
    SANE_Status     status;

    if (length != NULL) {
        *length = 0;
    }

    if (h == NULL || data == NULL || length == NULL || max_length <= 0) {
        return SANE_STATUS_INVAL;
    }

    if (!h->scanning) {
        return h->cancel ? SANE_STATUS_CANCELLED : SANE_STATUS_INVAL;
    }

    status = ipp_scan_read(h->scan, data, max_length, length,
            h->err, sizeof(h->err));

    if (status == SANE_STATUS_GOOD) {
        return status;
    }

    h->scanning = 0;

    /* The end of a page from a feeder leaves the job in place for the
     * page after it. A job on the platen has nothing more to give, and
     * is ended the ordinary way rather than left for sane_cancel(),
     * which no longer has anything to cancel.
     *
     * Anything else that is not success ends the job.
     */
    if (status == SANE_STATUS_EOF) {
        if (!h->multi) {
            ipp_scan_finish(h->scan);
            ipp_sane_scan_end(h);
        }

        return status;
    }

    if (h->err[0] != '\0') {
        ipp_sane_dbg("sane_read: %s: %s", sane_strstatus(status), h->err);
    }

    ipp_sane_scan_end(h);

    return status;
}

/* Cancel the scan in progress.
 *
 * This may be running in a signal handler, so it touches nothing but
 * the cancel flag. sane_start() and sane_read() look at it and end the
 * job; a frontend that calls neither of them again ends it with
 * sane_close().
 *
 * Outside of a page there is nothing to cancel, and the call is
 * ignored: a frontend calls this after every scan that went all the
 * way, and a job on a feeder must survive it to give its next page.
 */
void
sane_cancel (SANE_Handle handle)
{
    ipp_sane_handle *h = (ipp_sane_handle*) handle;

    if (h == NULL || !h->scanning) {
        return;
    }

    h->cancel = 1;
}

/* Switch between blocking and non-blocking I/O
 */
SANE_Status
sane_set_io_mode (SANE_Handle handle, SANE_Bool non_blocking)
{
    (void) handle;

    /* Blocking I/O is the only mode we have, and it is the default,
     * so asking for it is not an error.
     */
    return non_blocking ? SANE_STATUS_UNSUPPORTED : SANE_STATUS_GOOD;
}

/* Get the file descriptor to wait on for image data
 */
SANE_Status
sane_get_select_fd (SANE_Handle handle, SANE_Int *fd)
{
    (void) handle;
    (void) fd;

    return SANE_STATUS_UNSUPPORTED;
}

/******************** API aliases for libsane-dll ********************/
SANE_Status __attribute__ ((alias ("sane_init")))
sane_ipp_init (SANE_Int *version_code, SANE_Auth_Callback authorize);

void __attribute__ ((alias ("sane_exit")))
sane_ipp_exit (void);

SANE_Status __attribute__ ((alias ("sane_get_devices")))
sane_ipp_get_devices (const SANE_Device ***device_list, SANE_Bool local_only);

SANE_Status __attribute__ ((alias ("sane_open")))
sane_ipp_open (SANE_String_Const devicename, SANE_Handle *handle);

void __attribute__ ((alias ("sane_close")))
sane_ipp_close (SANE_Handle handle);

const SANE_Option_Descriptor* __attribute__ ((alias ("sane_get_option_descriptor")))
sane_ipp_get_option_descriptor (SANE_Handle handle, SANE_Int option);

SANE_Status __attribute__ ((alias ("sane_control_option")))
sane_ipp_control_option (SANE_Handle handle, SANE_Int option,
    SANE_Action action, void *value, SANE_Int *info);

SANE_Status __attribute__ ((alias ("sane_get_parameters")))
sane_ipp_get_parameters (SANE_Handle handle, SANE_Parameters *params);

SANE_Status __attribute__ ((alias ("sane_start")))
sane_ipp_start (SANE_Handle handle);

SANE_Status __attribute__ ((alias ("sane_read")))
sane_ipp_read (SANE_Handle handle, SANE_Byte *data,
    SANE_Int max_length, SANE_Int *length);

void __attribute__ ((alias ("sane_cancel")))
sane_ipp_cancel (SANE_Handle handle);

SANE_Status __attribute__ ((alias ("sane_set_io_mode")))
sane_ipp_set_io_mode (SANE_Handle handle, SANE_Bool non_blocking);

SANE_Status __attribute__ ((alias ("sane_get_select_fd")))
sane_ipp_get_select_fd (SANE_Handle handle, SANE_Int *fd);
