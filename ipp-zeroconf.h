/* sane-ipp -- IPP backend for SANE
 *
 * Copyright (C) 2026 Alexander Pevzner (pzz@apevzner.com)
 * Copyright (C) 2026 Yogesh Singla (yogeshsingla481@gmail.com)
 * SPDX-License-Identifier: BSD-2-Clause
 * See LICENSE for license terms and conditions
 *
 * Zeroconf device model
 *
 * Discovery reports one finding per service instance, per interface and
 * per service type, so a single printer is normally seen many times.
 * This layer folds those findings into one device per physical printer,
 * carrying every way of reaching it, best way first.
 */

#ifndef ipp_zeroconf_h
#define ipp_zeroconf_h

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* How close a target address is to us. Smaller is better.
 */
typedef enum {
    IPP_NETIF_LOOPBACK, /* The address is one of this host's own */
    IPP_NETIF_DIRECT,   /* The address is on a directly attached network */
    IPP_NETIF_ROUTED    /* The address is reachable through a router */
} IPP_NETIF_DISTANCE;

/* One way of reaching a device
 */
typedef struct ipp_endpoint {
    struct ipp_endpoint *next;      /* Next in the list */

    char     *uri;                  /* ipp[s]://addr:port/rs or rp */
    char     *addr;                 /* Address literal, as it appears in uri */

    int      ifindex;               /* Network interface index */
    int      family;                /* AF_INET or AF_INET6 */
    uint16_t port;                  /* TCP port */
    bool     tls;                   /* Reached over TLS, i.e. "ipps" */
    bool     linklocal;             /* Address is link-local */

    IPP_NETIF_DISTANCE distance;    /* Distance to the address */
} ipp_endpoint;

/* A device, as assembled from all the findings that refer to it.
 *
 * Findings are considered to describe the same device when they agree
 * on the UUID. Findings without a UUID are kept apart.
 */
typedef struct ipp_zc_device {
    struct ipp_zc_device *next;     /* Next in the list */

    char *name;                     /* DNS-SD instance name */
    char *uuid;                     /* TXT "uuid", NULL if not reported */
    char *model;                    /* TXT "ty", NULL if not reported */

    ipp_endpoint *endpoints;        /* Ordered, the best one first */
    int          nendpoints;        /* Count of endpoints */

    bool scan;                      /* Some finding reported TXT "rs" */
} ipp_zc_device;

/* Browse the network and return the assembled device list.
 *
 * Blocking, with the same timeout semantics as the underlying discovery.
 * Returns a list to be released with ipp_zc_device_list_free(). On error
 * returns NULL and, if err is not NULL, stores there a static error
 * string; err is set to NULL on success.
 */
ipp_zc_device*
ipp_zeroconf_discover (int timeout_ms, const char **err);

/* Release the list returned by ipp_zeroconf_discover()
 */
void
ipp_zc_device_list_free (ipp_zc_device *list);

/* Name of a distance value, for display. Never returns NULL.
 */
const char*
ipp_netif_distance_name (IPP_NETIF_DISTANCE distance);

#ifdef __cplusplus
}
#endif

#endif
