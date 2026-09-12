/* sane-ipp -- IPP backend for SANE
 *
 * Copyright (C) 2026 Alexander Pevzner (pzz@apevzner.com)
 * Copyright (C) 2026 Yogesh Singla (yogeshsingla481@gmail.com)
 * SPDX-License-Identifier: BSD-2-Clause
 * See LICENSE for license terms and conditions
 *
 * SANE API entry points
 *
 * Scanning is not implemented yet. The entry points that depend on
 * scanner capabilities or on a running scan job are present, because
 * the SANE loader resolves all of them, but report that they are
 * unsupported.
 */

#include "ipp-proto.h"
#include "ipp-zeroconf.h"

#include <sane/sane.h>
#include <sane/saneopts.h>

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

/******************** Options ********************/
/* Option numbers.
 *
 * Option zero is required by the SANE standard to exist and to report
 * the number of options the device has, so a frontend can discover the
 * rest. The scan options will be added after it, once the scanner
 * capabilities are decoded.
 */
enum {
    IPP_OPT_NUM_OPTIONS = 0,

    NUM_IPP_OPT
};

/******************** Local Types ********************/
/* An open device. The SANE_Handle of this backend is a pointer to it.
 */
typedef struct {
    char        *uri;       /* Device URI, as given to sane_open() */
    ipp_printer *printer;   /* Attributes, fetched when opened */
} ipp_sane_handle;

/******************** Static variables ********************/
static bool               ipp_sane_initialized;

/* Option descriptors. They do not depend on the device yet, so one set
 * is shared by every handle.
 */
static SANE_Option_Descriptor ipp_sane_opt_desc[NUM_IPP_OPT];

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

/******************** Options ********************/
/* Fill in the option descriptors
 */
static void
ipp_sane_opt_init (void)
{
    SANE_Option_Descriptor *desc = &ipp_sane_opt_desc[IPP_OPT_NUM_OPTIONS];

    memset(desc, 0, sizeof(*desc));

    desc->name = SANE_NAME_NUM_OPTIONS;
    desc->title = SANE_TITLE_NUM_OPTIONS;
    desc->desc = SANE_DESC_NUM_OPTIONS;
    desc->type = SANE_TYPE_INT;
    desc->unit = SANE_UNIT_NONE;
    desc->size = sizeof(SANE_Word);
    desc->cap = SANE_CAP_SOFT_DETECT;
    desc->constraint_type = SANE_CONSTRAINT_NONE;
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
        if (device->endpoints != NULL && device->name != NULL) {
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
        if (device->endpoints == NULL || device->name == NULL) {
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
    (void) authorize;

    if (version_code != NULL) {
        *version_code = SANE_VERSION_CODE(SANE_CURRENT_MAJOR,
                SANE_CURRENT_MINOR, 0);
    }

    ipp_sane_opt_init();
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

    h->printer = printer;
    *out = h;

    return SANE_STATUS_GOOD;
}

/* Open a device, trying its endpoints in turn.
 *
 * The endpoints are ordered best first, but the best one is not always
 * the one that answers, so each is tried until one does.
 */
static SANE_Status
ipp_sane_device_open (const ipp_zc_device *device, ipp_sane_handle **out)
{
    const ipp_endpoint *endpoint;
    SANE_Status        status = SANE_STATUS_IO_ERROR;

    for (endpoint = device->endpoints; endpoint != NULL;
            endpoint = endpoint->next) {
        status = ipp_sane_handle_new(endpoint->uri, out);
        if (status == SANE_STATUS_GOOD) {
            return status;
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

/******************** Not implemented yet ********************/
/* Scanning needs the scanner capabilities to be decoded and the scan
 * operations to be implemented. Until then these report that they have
 * nothing to offer, rather than pretending to work.
 */

/* Get the option descriptor
 */
const SANE_Option_Descriptor*
sane_get_option_descriptor (SANE_Handle handle, SANE_Int option)
{
    if (handle == NULL) {
        return NULL;
    }

    if (option < 0 || option >= NUM_IPP_OPT) {
        return NULL;
    }

    return &ipp_sane_opt_desc[option];
}

/* Get or set an option
 */
SANE_Status
sane_control_option (SANE_Handle handle, SANE_Int option, SANE_Action action,
        void *value, SANE_Int *info)
{
    if (handle == NULL || value == NULL) {
        return SANE_STATUS_INVAL;
    }

    if (option < 0 || option >= NUM_IPP_OPT) {
        return SANE_STATUS_INVAL;
    }

    if (info != NULL) {
        *info = 0;
    }

    switch (option) {
    case IPP_OPT_NUM_OPTIONS:
        /* Read-only, as the standard requires
         */
        if (action != SANE_ACTION_GET_VALUE) {
            return SANE_STATUS_INVAL;
        }

        *(SANE_Word*) value = NUM_IPP_OPT;
        return SANE_STATUS_GOOD;
    }

    return SANE_STATUS_UNSUPPORTED;
}

/* Get the parameters of the next scan
 */
SANE_Status
sane_get_parameters (SANE_Handle handle, SANE_Parameters *params)
{
    (void) handle;

    if (params != NULL) {
        memset(params, 0, sizeof(*params));
    }

    return SANE_STATUS_UNSUPPORTED;
}

/* Start scanning
 */
SANE_Status
sane_start (SANE_Handle handle)
{
    (void) handle;

    return SANE_STATUS_UNSUPPORTED;
}

/* Read the next portion of the image
 */
SANE_Status
sane_read (SANE_Handle handle, SANE_Byte *data, SANE_Int max_length,
        SANE_Int *length)
{
    (void) handle;
    (void) data;
    (void) max_length;

    if (length != NULL) {
        *length = 0;
    }

    return SANE_STATUS_UNSUPPORTED;
}

/* Cancel the scan in progress
 */
void
sane_cancel (SANE_Handle handle)
{
    (void) handle;
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
