/* sane-ipp -- IPP backend for SANE
 *
 * Copyright (C) 2026 Alexander Pevzner (pzz@apevzner.com)
 * Copyright (C) 2026 Yogesh Singla (yogeshsingla481@gmail.com)
 * SPDX-License-Identifier: BSD-2-Clause
 * See LICENSE for license terms and conditions
 *
 * Command-line IPP device discovery tool
 */

#include "ipp-mdns.h"
#include "ipp-zeroconf.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Print usage and exit
 */
static void
usage (const char *argv0, int exit_code)
{
    printf("usage: %s [-d] [-z] [-t TIMEOUT_MS]\n", argv0);
    printf("  -d            enable debug output\n");
    printf("  -z            report assembled devices, not raw findings\n");
    printf("  -t TIMEOUT_MS discovery timeout, default 2500\n");
    exit(exit_code);
}

/* Print a single device
 */
static void
print_device (const ipp_device *dev)
{
    int i;

    printf("%s\n", dev->name);
    printf("  service:  %s (if=%d)\n", dev->type, dev->ifindex);

    if (!dev->resolved) {
        printf("  NOT RESOLVED\n\n");
        return;
    }

    printf("  uri:      %s\n", dev->uri);

    if (dev->host != NULL) {
        printf("  host:     %s:%d\n", dev->host, dev->port);
    }

    for (i = 0; i < dev->naddrs; i ++) {
        printf("  address:  %s\n", dev->addrs[i]);
    }

    if (dev->model != NULL) {
        printf("  model:    %s\n", dev->model);
    }

    if (dev->uuid != NULL) {
        printf("  uuid:     %s\n", dev->uuid);
    }

    if (dev->rp != NULL) {
        printf("  rp:       %s\n", dev->rp);
    }

    if (dev->pdl != NULL) {
        printf("  pdl:      %s\n", dev->pdl);
    }

    if (dev->adminurl != NULL) {
        printf("  admin:    %s\n", dev->adminurl);
    }

    printf("  scanner:  %s\n", dev->scan ? "yes" : "no");
    printf("\n");
}

/* Print a device assembled by the zeroconf layer
 */
static void
print_zc_device (const ipp_zc_device *device)
{
    const ipp_endpoint *endpoint;

    printf("%s\n", device->name);

    if (device->model != NULL) {
        printf("  model:    %s\n", device->model);
    }

    if (device->uuid != NULL) {
        printf("  uuid:     %s\n", device->uuid);
    }

    printf("  scanner:  %s\n", device->scan ? "yes" : "no");
    printf("  endpoints (%d), best first:\n", device->nendpoints);

    for (endpoint = device->endpoints; endpoint != NULL;
            endpoint = endpoint->next) {
        printf("    %-46s %s%s if=%d\n", endpoint->uri,
                ipp_netif_distance_name(endpoint->distance),
                endpoint->linklocal ? ", link-local" : "",
                endpoint->ifindex);
    }

    printf("\n");
}

/* Report the devices as assembled by the zeroconf layer
 */
static int
discover_assembled (int timeout)
{
    ipp_zc_device *list, *device;
    const char    *err;
    int           count = 0;

    list = ipp_zeroconf_discover(timeout, &err);

    if (err != NULL) {
        fprintf(stderr, "discovery failed: %s\n", err);
        return 1;
    }

    for (device = list; device != NULL; device = device->next) {
        print_zc_device(device);
        count ++;
    }

    printf("%d device(s) found\n", count);

    ipp_zc_device_list_free(list);

    return 0;
}

/* The main function
 */
int
main (int argc, char **argv)
{
    int        timeout = 2500;
    bool       assembled = false;
    ipp_device *list, *dev;
    const char *err;
    int        i, count = 0;

    for (i = 1; i < argc; i ++) {
        if (!strcmp(argv[i], "-d")) {
            ipp_mdns_debug_enable(true);
        } else if (!strcmp(argv[i], "-z")) {
            assembled = true;
        } else if (!strcmp(argv[i], "-t") && i + 1 < argc) {
            timeout = atoi(argv[++ i]);
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0], 0);
        } else {
            fprintf(stderr, "%s: unknown option %s\n", argv[0], argv[i]);
            usage(argv[0], 1);
        }
    }

    if (assembled) {
        return discover_assembled(timeout);
    }

    list = ipp_mdns_discover(timeout, &err);

    if (err != NULL) {
        fprintf(stderr, "discovery failed: %s\n", err);
        return 1;
    }

    for (dev = list; dev != NULL; dev = dev->next) {
        print_device(dev);
        count ++;
    }

    printf("%d device(s) found\n", count);

    ipp_device_list_free(list);

    return 0;
}
