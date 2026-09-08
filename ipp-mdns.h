/* sane-ipp -- IPP backend for SANE
 *
 * Copyright (C) 2026 Yogesh Singla
 * SPDX-License-Identifier: BSD-2-Clause
 * See LICENSE for license terms and conditions
 *
 * MDNS device discovery (synchronous)
 */

#ifndef ipp_mdns_h
#define ipp_mdns_h

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Max addresses remembered per discovered device
 */
#define IPP_MDNS_MAX_ADDRS      8

/* ipp_device represents a single device discovered over DNS-SD.
 *
 * A device is identified by the tuple (ifindex, instance name, tls),
 * so the same printer seen over _ipp._tcp and _ipps._tcp yields two
 * entries, and the same printer seen on two interfaces yields two
 * entries as well.
 */
typedef struct ipp_device {
    struct ipp_device *next;                       /* Next in the list */

    char     *name;                                /* DNS-SD instance name */
    char     *type;                                /* "_ipp._tcp" or "_ipps._tcp" */
    char     *host;                                /* Resolved host name */

    char     *model;                               /* TXT "ty" */
    char     *uuid;                                /* TXT "uuid" */
    char     *rp;                                  /* TXT "rp", resource path */
    char     *pdl;                                 /* TXT "pdl" */
    char     *adminurl;                            /* TXT "adminurl" */

    char     *addrs[IPP_MDNS_MAX_ADDRS];           /* Address literals */
    int      naddrs;                               /* Count of addrs */

    char     *uri;                                 /* ipp[s]://addr:port/rp */

    int      ifindex;                              /* Network interface index */
    uint16_t port;                                 /* TCP port */
    bool     tls;                                  /* Service is _ipps._tcp */
    bool     scan;                                 /* TXT "scan" is T/t */
    bool     resolved;                             /* Resolver succeeded */
    bool     uri_is_ip6;                           /* uri holds an IPv6 literal */
} ipp_device;

/* Browse the network for IPP devices.
 *
 * Blocks until browsing settles (all browsers reported ALL_FOR_NOW and
 * every resolver has completed) or until timeout_ms milliseconds elapse,
 * whichever comes first.
 *
 * Returns a singly-linked list of discovered devices, to be released with
 * ipp_device_list_free(). On error returns NULL and, if err is not NULL,
 * stores there a static error string; err is set to NULL on success.
 */
ipp_device*
ipp_mdns_discover (int timeout_ms, const char **err);

/* Release the list returned by ipp_mdns_discover()
 */
void
ipp_device_list_free (ipp_device *list);

/* Enable/disable discovery debug messages on stderr. Off by default.
 */
void
ipp_mdns_debug_enable (bool enable);

#ifdef __cplusplus
}
#endif

#endif
