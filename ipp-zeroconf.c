/* sane-ipp -- IPP backend for SANE
 *
 * Copyright (C) 2026 Alexander Pevzner (pzz@apevzner.com)
 * Copyright (C) 2026 Yogesh Singla (yogeshsingla481@gmail.com)
 * SPDX-License-Identifier: BSD-2-Clause
 * See LICENSE for license terms and conditions
 *
 * Zeroconf device model
 */

#include "ipp-zeroconf.h"
#include "ipp-mdns.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>

/******************** Small helpers ********************/
/* strdup() that aborts on out of memory, and tolerates NULL
 */
static char*
ipp_zc_str_dup (const char *s)
{
    char *p;

    if (s == NULL) {
        return NULL;
    }

    p = strdup(s);
    if (p == NULL) {
        perror("ipp-zeroconf: strdup");
        abort();
    }

    return p;
}

/* calloc() that aborts on out of memory
 */
static void*
ipp_zc_alloc (size_t count, size_t size)
{
    void *p = calloc(count, size);

    if (p == NULL) {
        perror("ipp-zeroconf: calloc");
        abort();
    }

    return p;
}

/* strcasecmp() that treats NULL as smaller than everything else
 */
static int
ipp_zc_str_cmp (const char *s1, const char *s2)
{
    if (s1 == NULL || s2 == NULL) {
        return (s1 != NULL) - (s2 != NULL);
    }

    return strcasecmp(s1, s2);
}

/* Reduce a UUID to a canonical form, so that the same identity written
 * in two different ways still compares equal.
 *
 * Devices are inconsistent about this. The same UUID turns up bare or
 * hyphenated, in either case, and with or without a "urn:uuid:" prefix,
 * and the two service types of one device need not agree. The prefix is
 * stripped first, because "uuid:" is itself part hex digit, and what is
 * left must hold exactly 32 of them once punctuation is ignored.
 *
 * Returns NULL if the input is not a UUID at all, in which case the
 * caller has nothing better to do than keep it as it stands. The result
 * is to be released with free().
 */
static char*
ipp_zc_uuid_normalize (const char *uuid)
{
    static const char hex[] = "0123456789abcdef";
    char              digits[32];
    size_t            count = 0;
    char              *out;
    size_t            i, o;

    if (uuid == NULL) {
        return NULL;
    }

    /* Strip the URN prefix. It has to go before the scan below, rather
     * than be ignored by it, because the "d" of "uuid" is a hex digit
     * and would otherwise be counted as part of the value.
     */
    if (strncasecmp(uuid, "urn:", 4) == 0) {
        uuid += 4;
    }

    if (strncasecmp(uuid, "uuid:", 5) == 0) {
        uuid += 5;
    }

    while (*uuid != '\0') {
        const char *p = strchr(hex, tolower((unsigned char) *uuid));

        if (p != NULL) {
            if (count == sizeof(digits)) {
                return NULL;    /* Too long to be a UUID */
            }
            digits[count ++] = (char) *p;
        }

        uuid ++;
    }

    if (count != sizeof(digits)) {
        return NULL;
    }

    /* 8-4-4-4-12, the form everything else writes it in */
    out = ipp_zc_alloc(37, 1);

    for (i = o = 0; i < sizeof(digits); i ++) {
        if (i == 8 || i == 12 || i == 16 || i == 20) {
            out[o ++] = '-';
        }
        out[o ++] = digits[i];
    }

    return out;
}

/******************** Address handling ********************/
/* Parse an address literal, as it appears in a URI, into a socket
 * address. IPv6 literals are bracketed and may carry a "%25"-escaped
 * zone, both of which are stripped here.
 *
 * Returns false if the literal cannot be parsed.
 */
static bool
ipp_zc_addr_parse (const char *straddr, struct sockaddr_storage *ss)
{
    char   buf[128];
    size_t len;
    char   *zone;

    memset(ss, 0, sizeof(*ss));

    if (straddr == NULL) {
        return false;
    }

    if (straddr[0] == '[') {
        /* IPv6 literal: strip the brackets */
        len = strlen(straddr);
        if (len < 3 || straddr[len - 1] != ']' || len - 2 >= sizeof(buf)) {
            return false;
        }
        memcpy(buf, straddr + 1, len - 2);
        buf[len - 2] = '\0';

        /* Strip the zone, which identifies the interface rather than
         * the address itself
         */
        zone = strstr(buf, "%25");
        if (zone == NULL) {
            zone = strchr(buf, '%');
        }
        if (zone != NULL) {
            *zone = '\0';
        }

        ss->ss_family = AF_INET6;
        return inet_pton(AF_INET6, buf,
                &((struct sockaddr_in6*) ss)->sin6_addr) == 1;
    }

    ss->ss_family = AF_INET;
    return inet_pton(AF_INET, straddr,
            &((struct sockaddr_in*) ss)->sin_addr) == 1;
}

/* Report whether an address is link-local
 */
static bool
ipp_zc_addr_is_linklocal (const struct sockaddr_storage *ss)
{
    if (ss->ss_family == AF_INET6) {
        const uint8_t *a =
            ((const struct sockaddr_in6*) ss)->sin6_addr.s6_addr;

        return a[0] == 0xfe && (a[1] & 0xc0) == 0x80;
    }

    if (ss->ss_family == AF_INET) {
        uint32_t a = ntohl(((const struct sockaddr_in*) ss)->sin_addr.s_addr);

        /* 169.254.0.0/16 */
        return (a >> 16) == 0xa9fe;
    }

    return false;
}

/* Report whether two IPv6 addresses share a prefix, as given by a mask
 */
static bool
ipp_zc_in6_same_prefix (const struct in6_addr *a, const struct in6_addr *b,
        const struct in6_addr *mask)
{
    size_t i;

    for (i = 0; i < sizeof(a->s6_addr); i ++) {
        if (((a->s6_addr[i] ^ b->s6_addr[i]) & mask->s6_addr[i]) != 0) {
            return false;
        }
    }

    return true;
}

/* Work out how close an address is to us.
 *
 * An address that belongs to one of our own interfaces is the closest
 * thing there is: it means the device runs on this very host. An address
 * that shares a prefix with one of our interfaces is directly attached.
 * Anything else has to go through a router.
 */
static IPP_NETIF_DISTANCE
ipp_zc_distance (const struct sockaddr_storage *ss, const struct ifaddrs *ifa)
{
    IPP_NETIF_DISTANCE distance = IPP_NETIF_ROUTED;

    for (; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL || ifa->ifa_netmask == NULL) {
            continue;
        }

        if (ifa->ifa_addr->sa_family != (int) ss->ss_family) {
            continue;
        }

        if (ss->ss_family == AF_INET) {
            struct in_addr addr = ((const struct sockaddr_in*) ss)->sin_addr;
            struct in_addr ifaddr =
                ((const struct sockaddr_in*) (void*) ifa->ifa_addr)->sin_addr;
            struct in_addr ifmask =
                ((const struct sockaddr_in*) (void*) ifa->ifa_netmask)->sin_addr;

            if (addr.s_addr == ifaddr.s_addr) {
                return IPP_NETIF_LOOPBACK;
            }

            if (((addr.s_addr ^ ifaddr.s_addr) & ifmask.s_addr) == 0) {
                distance = IPP_NETIF_DIRECT;
            }
        } else if (ss->ss_family == AF_INET6) {
            const struct in6_addr *addr =
                &((const struct sockaddr_in6*) ss)->sin6_addr;
            const struct in6_addr *ifaddr =
                &((const struct sockaddr_in6*) (void*) ifa->ifa_addr)->sin6_addr;
            const struct in6_addr *ifmask =
                &((const struct sockaddr_in6*) (void*) ifa->ifa_netmask)->sin6_addr;

            if (memcmp(addr, ifaddr, sizeof(*addr)) == 0) {
                return IPP_NETIF_LOOPBACK;
            }

            if (ipp_zc_in6_same_prefix(addr, ifaddr, ifmask)) {
                distance = IPP_NETIF_DIRECT;
            }
        }
    }

    return distance;
}

/* Name of a distance value, for display
 */
const char*
ipp_netif_distance_name (IPP_NETIF_DISTANCE distance)
{
    switch (distance) {
    case IPP_NETIF_LOOPBACK: return "loopback";
    case IPP_NETIF_DIRECT:   return "direct";
    case IPP_NETIF_ROUTED:   return "routed";
    }

    return "unknown";
}

/******************** Endpoints ********************/
/* Release a single endpoint
 */
static void
ipp_endpoint_free (ipp_endpoint *endpoint)
{
    if (endpoint == NULL) {
        return;
    }

    free(endpoint->uri);
    free(endpoint->addr);
    free(endpoint);
}

/* Release a list of endpoints
 */
static void
ipp_endpoint_list_free (ipp_endpoint *list)
{
    while (list != NULL) {
        ipp_endpoint *next = list->next;

        ipp_endpoint_free(list);
        list = next;
    }
}

/* Create an endpoint out of an address of a discovered device.
 *
 * Returns NULL if the address cannot be understood, in which case there
 * is nothing useful we could do with it anyway.
 */
static ipp_endpoint*
ipp_endpoint_new (const ipp_device *dev, const char *straddr,
        const struct ifaddrs *ifaces)
{
    struct sockaddr_storage ss;
    ipp_endpoint            *endpoint;

    if (!ipp_zc_addr_parse(straddr, &ss)) {
        return NULL;
    }

    endpoint = ipp_zc_alloc(1, sizeof(*endpoint));

    /* A scanner is addressed at its scan resource, which is not the one
     * its print service answers at (PWG 5100.17, section 10)
     */
    endpoint->uri = ipp_uri_make(dev->tls, straddr, dev->port,
            dev->scan ? dev->rs : dev->rp);
    endpoint->addr = ipp_zc_str_dup(straddr);
    endpoint->ifindex = dev->ifindex;
    endpoint->family = ss.ss_family;
    endpoint->port = dev->port;
    endpoint->tls = dev->tls;
    endpoint->linklocal = ipp_zc_addr_is_linklocal(&ss);
    endpoint->distance = ipp_zc_distance(&ss, ifaces);

    return endpoint;
}

/* Order two endpoints, best first.
 *
 * Reachability comes first, because an endpoint we cannot route to is
 * worthless however pretty it looks. Link-local addresses are demoted
 * because they only work from the interface that saw them. Of two
 * endpoints equally reachable, the encrypted one wins, and then IPv6.
 */
static int
ipp_endpoint_cmp (const void *p1, const void *p2)
{
    const ipp_endpoint *e1 = *(const ipp_endpoint* const*) p1;
    const ipp_endpoint *e2 = *(const ipp_endpoint* const*) p2;

    if (e1->distance != e2->distance) {
        return (int) e1->distance - (int) e2->distance;
    }

    if (e1->linklocal != e2->linklocal) {
        return e1->linklocal ? 1 : -1;
    }

    /* A device normally advertises both _ipp._tcp and _ipps._tcp, which
     * reach the same service over the same address. There is no reason
     * to talk in clear to something willing to talk encrypted.
     */
    if (e1->tls != e2->tls) {
        return e1->tls ? -1 : 1;
    }

    if (e1->family != e2->family) {
        return e1->family == AF_INET6 ? -1 : 1;
    }

    return strcmp(e1->uri, e2->uri);
}

/* Sort a device's endpoints best first and drop the duplicates.
 *
 * The same address can be reported by several findings, so duplicates
 * are the normal case rather than an oddity.
 */
static void
ipp_endpoint_list_sort_dedup (ipp_zc_device *device)
{
    ipp_endpoint **array, *endpoint;
    int          count = 0, i, kept;

    for (endpoint = device->endpoints; endpoint != NULL;
            endpoint = endpoint->next) {
        count ++;
    }

    if (count < 2) {
        device->nendpoints = count;
        return;
    }

    array = ipp_zc_alloc((size_t) count, sizeof(*array));

    i = 0;
    for (endpoint = device->endpoints; endpoint != NULL;
            endpoint = endpoint->next) {
        array[i ++] = endpoint;
    }

    qsort(array, (size_t) count, sizeof(*array), ipp_endpoint_cmp);

    /* Duplicates are adjacent after sorting, because the URI is the
     * last thing the ordering looks at
     */
    kept = 1;
    for (i = 1; i < count; i ++) {
        if (strcmp(array[kept - 1]->uri, array[i]->uri) == 0) {
            ipp_endpoint_free(array[i]);
        } else {
            array[kept ++] = array[i];
        }
    }

    for (i = 0; i < kept - 1; i ++) {
        array[i]->next = array[i + 1];
    }
    array[kept - 1]->next = NULL;

    device->endpoints = array[0];
    device->nendpoints = kept;

    free(array);
}

/******************** Devices ********************/
/* Release a device list
 */
void
ipp_zc_device_list_free (ipp_zc_device *list)
{
    while (list != NULL) {
        ipp_zc_device *next = list->next;

        ipp_endpoint_list_free(list->endpoints);
        free(list->name);
        free(list->uuid);
        free(list->model);
        free(list);

        list = next;
    }
}

/* Report whether a finding describes a device we have already seen.
 *
 * Only the UUID identifies a device: the instance name is not unique and
 * may differ between its service types. The UUID is compared in its
 * canonical form, so the two service types of one device still match
 * when they spell it differently. A finding that reports no UUID cannot
 * be told apart from any other, so it is never merged.
 */
static bool
ipp_zc_device_match (const ipp_zc_device *device, const char *uuid)
{
    if (device->uuid == NULL || uuid == NULL) {
        return false;
    }

    return strcasecmp(device->uuid, uuid) == 0;
}

/* Find the device a finding belongs to, or NULL if it is a new one
 */
static ipp_zc_device*
ipp_zc_device_find (ipp_zc_device *list, const char *uuid)
{
    for (; list != NULL; list = list->next) {
        if (ipp_zc_device_match(list, uuid)) {
            return list;
        }
    }

    return NULL;
}

/* Fold one finding into the device list
 */
static ipp_zc_device*
ipp_zc_device_merge (ipp_zc_device *list, const ipp_device *dev,
        const struct ifaddrs *ifaces)
{
    char          *uuid = ipp_zc_uuid_normalize(dev->uuid);
    ipp_zc_device *device;
    int           i;

    /* A UUID we cannot make sense of is kept as it stands, so that two
     * findings carrying the same nonsense still match each other
     */
    if (uuid == NULL) {
        uuid = ipp_zc_str_dup(dev->uuid);
    }

    device = ipp_zc_device_find(list, uuid);

    if (device == NULL) {
        device = ipp_zc_alloc(1, sizeof(*device));

        device->name = ipp_zc_str_dup(dev->name);
        device->uuid = uuid;
        device->model = ipp_zc_str_dup(dev->model);

        device->next = list;
        list = device;
    } else {
        free(uuid);
    }

    if (device->model == NULL) {
        /* A later finding may carry a model where an earlier one did not
         */
        device->model = ipp_zc_str_dup(dev->model);
    }

    device->scan = device->scan || dev->scan;

    for (i = 0; i < dev->naddrs; i ++) {
        ipp_endpoint *endpoint = ipp_endpoint_new(dev, dev->addrs[i], ifaces);

        if (endpoint != NULL) {
            endpoint->next = device->endpoints;
            device->endpoints = endpoint;
        }
    }

    return list;
}

/* Order two devices, for display
 */
static int
ipp_zc_device_cmp (const void *p1, const void *p2)
{
    const ipp_zc_device *d1 = *(const ipp_zc_device* const*) p1;
    const ipp_zc_device *d2 = *(const ipp_zc_device* const*) p2;
    int                 cmp = ipp_zc_str_cmp(d1->name, d2->name);

    if (cmp != 0) {
        return cmp;
    }

    /* Two devices are allowed to share an instance name, so the UUID
     * is what separates them here
     */
    return ipp_zc_str_cmp(d1->uuid, d2->uuid);
}

/* Sort a device list by name.
 *
 * Findings arrive in whatever order the network answers in, which varies
 * from one run to the next. Sorting gives a frontend a list that stays
 * put, so that a device does not change its place between two calls.
 */
static ipp_zc_device*
ipp_zc_device_list_sort (ipp_zc_device *list)
{
    ipp_zc_device **array, *device;
    int           n = 0, i;

    for (device = list; device != NULL; device = device->next) {
        n ++;
    }

    if (n < 2) {
        return list;
    }

    array = ipp_zc_alloc((size_t) n, sizeof(*array));

    i = 0;
    for (device = list; device != NULL; device = device->next) {
        array[i ++] = device;
    }

    qsort(array, (size_t) n, sizeof(*array), ipp_zc_device_cmp);

    for (i = 0; i < n - 1; i ++) {
        array[i]->next = array[i + 1];
    }
    array[n - 1]->next = NULL;

    list = array[0];
    free(array);

    return list;
}

/******************** Discovery ********************/
/* The environment variable that replaces discovery with one device
 */
#define IPP_ZC_DEVICE_ENV       "SANE_IPP_DEVICE"

/* Build the device list out of the SANE_IPP_DEVICE variable.
 *
 * The value is "name:uri", where uri is an ipp:// or ipps:// URI. The
 * name ends where the URI begins, so it may itself contain colons.
 *
 * Returns NULL and sets err if the value cannot be understood.
 */
static ipp_zc_device*
ipp_zc_device_from_env (const char *value, const char **err)
{
    const char    *uri;
    ipp_zc_device *device;
    ipp_endpoint  *endpoint;
    bool          tls = false;

    uri = strstr(value, ":ipp://");
    if (uri == NULL) {
        uri = strstr(value, ":ipps://");
        tls = uri != NULL;
    }

    if (uri == NULL || uri == value) {
        if (err != NULL) {
            *err = "Invalid " IPP_ZC_DEVICE_ENV;
        }
        return NULL;
    }

    endpoint = ipp_zc_alloc(1, sizeof(*endpoint));
    endpoint->uri = ipp_zc_str_dup(uri + 1);
    endpoint->tls = tls;

    device = ipp_zc_alloc(1, sizeof(*device));
    device->name = strndup(value, (size_t) (uri - value));
    if (device->name == NULL) {
        perror("ipp-zeroconf: strndup");
        abort();
    }

    device->endpoints = endpoint;
    device->nendpoints = 1;
    device->scan = true;

    return device;
}

/* Browse the network and assemble the device list
 */
ipp_zc_device*
ipp_zeroconf_discover (int timeout_ms, const char **err)
{
    ipp_device     *found, *dev;
    ipp_zc_device  *list = NULL, *device;
    struct ifaddrs *ifaces = NULL;

    if (err != NULL) {
        *err = NULL;
    }

    /* A configured device replaces discovery altogether, so that testing
     * against a simulator is not disturbed by whatever else is around
     */
    if (getenv(IPP_ZC_DEVICE_ENV) != NULL) {
        return ipp_zc_device_from_env(getenv(IPP_ZC_DEVICE_ENV), err);
    }

    found = ipp_mdns_discover(timeout_ms, err);
    if (err != NULL && *err != NULL) {
        return NULL;
    }

    /* Interfaces are read once and shared by every distance
     * computation, rather than re-read per address
     */
    if (getifaddrs(&ifaces) != 0) {
        ifaces = NULL;
    }

    for (dev = found; dev != NULL; dev = dev->next) {
        if (dev->resolved && dev->naddrs != 0) {
            list = ipp_zc_device_merge(list, dev, ifaces);
        }
    }

    if (ifaces != NULL) {
        freeifaddrs(ifaces);
    }

    ipp_device_list_free(found);

    list = ipp_zc_device_list_sort(list);

    for (device = list; device != NULL; device = device->next) {
        ipp_endpoint_list_sort_dedup(device);
    }

    return list;
}
