/* sane-ipp -- IPP backend for SANE
 *
 * Copyright (C) 2026 Yogesh Singla
 * SPDX-License-Identifier: BSD-2-Clause
 * See LICENSE for license terms and conditions
 *
 * MDNS device discovery (synchronous)
 *
 * This is the blocking counterpart of the event-loop driven discovery
 * used by sane-airscan: a private AvahiSimplePoll is created, browsers
 * for _ipp._tcp and _ipps._tcp are started, and the poll is iterated
 * until browsing settles or the caller's timeout expires.
 */

#include "ipp-mdns.h"

#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>
#include <avahi-common/simple-watch.h>

#include <net/if.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/******************** Constants ********************/
/* MDNS_SERVICE represents numerical identifiers for the DNS-SD service
 * types we are interested in
 */
typedef enum {
    MDNS_SERVICE_IPP_TCP,       /* _ipp._tcp */
    MDNS_SERVICE_IPPS_TCP,      /* _ipps._tcp */

    NUM_MDNS_SERVICE
} MDNS_SERVICE;

/* Service type strings, indexed by MDNS_SERVICE
 */
static const char *ipp_mdns_service_types[NUM_MDNS_SERVICE] = {
    "_ipp._tcp",
    "_ipps._tcp"
};

/******************** Local Types ********************/
/* Forward declaration
 */
typedef struct ipp_mdns_rctx ipp_mdns_rctx;

/* Discovery context, lives for the duration of a single
 * ipp_mdns_discover() call
 */
typedef struct {
    AvahiSimplePoll     *poll;                         /* Event poll */
    AvahiClient         *client;                       /* AVAHI client */
    AvahiServiceBrowser *browser[NUM_MDNS_SERVICE];    /* Service browsers */
    bool                all_for_now[NUM_MDNS_SERVICE]; /* Browser settled */
    int                 pending;                       /* Pending resolvers */
    ipp_mdns_rctx       *rctx_list;                    /* Pending resolvers */
    ipp_device          *devices;                      /* Discovered devices */
    const char          *err;                          /* Error, NULL if none */
} ipp_mdns_ctx;

/* Resolver context, one per pending AvahiServiceResolver
 */
struct ipp_mdns_rctx {
    ipp_mdns_ctx         *ctx;      /* Parent context */
    ipp_device           *device;   /* Device being resolved */
    AvahiServiceResolver *resolver; /* The resolver itself */
    ipp_mdns_rctx        *next;     /* In ctx->rctx_list */
};

/* Browser context, one per service type
 */
typedef struct {
    ipp_mdns_ctx *ctx;          /* Parent context */
    MDNS_SERVICE service;       /* Service being browsed */
} ipp_mdns_bctx;

/******************** Static variables ********************/
static bool ipp_mdns_debug = false;

/******************** Debugging ********************/
/* Print debug message, if enabled
 */
static void
ipp_mdns_dbg (const char *fmt, ...)
{
    va_list ap;

    if (!ipp_mdns_debug) {
        return;
    }

    va_start(ap, fmt);
    fputs("ipp-mdns: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/******************** Small helpers ********************/
/* strdup() that aborts on out of memory
 */
static char*
ipp_str_dup (const char *s)
{
    char *p = strdup(s);

    if (p == NULL) {
        perror("ipp-mdns: strdup");
        abort();
    }

    return p;
}

/* asprintf()-alike that aborts on out of memory
 */
static char*
ipp_str_printf (const char *fmt, ...)
{
    va_list ap;
    char    *p = NULL;
    int     rc;

    va_start(ap, fmt);
    rc = vasprintf(&p, fmt, ap);
    va_end(ap);

    if (rc < 0 || p == NULL) {
        perror("ipp-mdns: vasprintf");
        abort();
    }

    return p;
}

/* Fetch a value from the TXT record.
 *
 * Returns a newly allocated string, or NULL if the key is missing.
 * A key present without a value yields an empty string.
 */
static char*
ipp_txt_get (AvahiStringList *txt, const char *key)
{
    AvahiStringList *s = avahi_string_list_find(txt, key);
    size_t          klen = strlen(key);
    size_t          vlen;
    char            *p;

    if (s == NULL || s->size <= klen) {
        return NULL;
    }

    /* s->text is "key=value", not NUL-terminated */
    vlen = s->size - klen - 1;
    p = malloc(vlen + 1);
    if (p == NULL) {
        perror("ipp-mdns: malloc");
        abort();
    }

    memcpy(p, (char*) s->text + klen + 1, vlen);
    p[vlen] = '\0';

    return p;
}

/* Check if IPv6 address is link-local
 */
static bool
ipp_ip6_is_linklocal (const uint8_t *addr)
{
    return addr[0] == 0xfe && (addr[1] & 0xc0) == 0x80;
}

/* Format AvahiAddress as a URI host literal.
 *
 * IPv6 addresses come out bracketed, link-local ones carry the
 * escaped zone index (RFC 6874).
 */
static char*
ipp_addr_format (const AvahiAddress *addr, int ifindex)
{
    char buf[AVAHI_ADDRESS_STR_MAX];

    avahi_address_snprint(buf, sizeof(buf), addr);

    if (addr->proto != AVAHI_PROTO_INET6) {
        return ipp_str_dup(buf);
    }

    if (ipp_ip6_is_linklocal(addr->data.data)) {
        return ipp_str_printf("[%s%%25%d]", buf, ifindex);
    }

    return ipp_str_printf("[%s]", buf);
}

/* Build the device URI out of address, port and the "rp" TXT value
 */
static char*
ipp_uri_make (bool tls, const char *straddr, uint16_t port, const char *rp)
{
    const char *scheme = tls ? "ipps" : "ipp";
    size_t     len;

    if (rp == NULL) {
        /* Assume /ipp/print by default, as most printers do */
        return ipp_str_printf("%s://%s:%d/ipp/print", scheme, straddr, port);
    }

    /* Normalize rp: strip leading and trailing slashes */
    while (*rp == '/') {
        rp ++;
    }

    len = strlen(rp);
    while (len != 0 && rp[len - 1] == '/') {
        len --;
    }

    if (len == 0) {
        return ipp_str_printf("%s://%s:%d/", scheme, straddr, port);
    }

    return ipp_str_printf("%s://%s:%d/%.*s", scheme, straddr, port,
        (int) len, rp);
}

/******************** Device list ********************/
/* Create a new device
 */
static ipp_device*
ipp_device_new (const char *name, const char *type, int ifindex, bool tls)
{
    ipp_device *dev = calloc(1, sizeof(*dev));

    if (dev == NULL) {
        perror("ipp-mdns: calloc");
        abort();
    }

    dev->name = ipp_str_dup(name);
    dev->type = ipp_str_dup(type);
    dev->ifindex = ifindex;
    dev->tls = tls;

    return dev;
}

/* Free a single device
 */
static void
ipp_device_free (ipp_device *dev)
{
    int i;

    free(dev->name);
    free(dev->type);
    free(dev->host);
    free(dev->model);
    free(dev->uuid);
    free(dev->rp);
    free(dev->pdl);
    free(dev->adminurl);
    free(dev->uri);

    for (i = 0; i < dev->naddrs; i ++) {
        free(dev->addrs[i]);
    }

    free(dev);
}

/* Free the whole device list
 */
void
ipp_device_list_free (ipp_device *list)
{
    while (list != NULL) {
        ipp_device *next = list->next;
        ipp_device_free(list);
        list = next;
    }
}

/* Lookup a device by (ifindex, name, tls), creating it if not found
 */
static ipp_device*
ipp_device_get (ipp_mdns_ctx *ctx, const char *name, const char *type,
        int ifindex, bool tls)
{
    ipp_device *dev, *last = NULL;

    for (dev = ctx->devices; dev != NULL; dev = dev->next) {
        if (dev->ifindex == ifindex && dev->tls == tls &&
            !strcmp(dev->name, name)) {
            return dev;
        }
        last = dev;
    }

    dev = ipp_device_new(name, type, ifindex, tls);

    /* Append, so the list keeps the discovery order */
    if (last == NULL) {
        ctx->devices = dev;
    } else {
        last->next = dev;
    }

    return dev;
}

/* Remove a device from the list and free it
 */
static void
ipp_device_del (ipp_mdns_ctx *ctx, const char *name, int ifindex, bool tls)
{
    ipp_device **prev = &ctx->devices;
    ipp_device *dev;

    for (dev = ctx->devices; dev != NULL; dev = dev->next) {
        if (dev->ifindex == ifindex && dev->tls == tls &&
            !strcmp(dev->name, name)) {
            *prev = dev->next;
            ipp_device_free(dev);
            return;
        }
        prev = &dev->next;
    }
}

/* Add an address to the device, unless already known
 */
static void
ipp_device_add_addr (ipp_device *dev, char *straddr)
{
    int i;

    for (i = 0; i < dev->naddrs; i ++) {
        if (!strcmp(dev->addrs[i], straddr)) {
            free(straddr);
            return;
        }
    }

    if (dev->naddrs == IPP_MDNS_MAX_ADDRS) {
        free(straddr);
        return;
    }

    dev->addrs[dev->naddrs ++] = straddr;
}

/******************** Discovery ********************/
/* Check if discovery is complete: every browser has settled and
 * no resolver is still pending. If so, stop the poll.
 */
static void
ipp_mdns_check_done (ipp_mdns_ctx *ctx)
{
    int i;

    if (ctx->pending != 0) {
        return;
    }

    for (i = 0; i < NUM_MDNS_SERVICE; i ++) {
        if (!ctx->all_for_now[i]) {
            return;
        }
    }

    ipp_mdns_dbg("discovery complete");
    avahi_simple_poll_quit(ctx->poll);
}

/* AVAHI service resolver callback
 */
static void
ipp_mdns_resolver_callback (AvahiServiceResolver *r,
        AvahiIfIndex interface, AvahiProtocol protocol,
        AvahiResolverEvent event, const char *name, const char *type,
        const char *domain, const char *host_name, const AvahiAddress *addr,
        uint16_t port, AvahiStringList *txt, AvahiLookupResultFlags flags,
        void *userdata)
{
    ipp_mdns_rctx *rctx = userdata;
    ipp_mdns_ctx  *ctx = rctx->ctx;
    ipp_device    *dev = rctx->device;
    ipp_mdns_rctx **prev;
    char          *straddr;
    char          *scan;

    (void) protocol;
    (void) domain;
    (void) flags;

    switch (event) {
    case AVAHI_RESOLVER_FOUND:
        straddr = ipp_addr_format(addr, interface);
        ipp_mdns_dbg("resolve %s (%s): %s:%d", name, type, straddr, port);

        dev->resolved = true;
        dev->port = port;

        if (dev->host == NULL && host_name != NULL) {
            dev->host = ipp_str_dup(host_name);
        }

        if (dev->model == NULL) {
            dev->model = ipp_txt_get(txt, "ty");
        }

        if (dev->uuid == NULL) {
            dev->uuid = ipp_txt_get(txt, "uuid");
        }

        if (dev->rp == NULL) {
            dev->rp = ipp_txt_get(txt, "rp");
        }

        if (dev->pdl == NULL) {
            dev->pdl = ipp_txt_get(txt, "pdl");
        }

        if (dev->adminurl == NULL) {
            dev->adminurl = ipp_txt_get(txt, "adminurl");
        }

        scan = ipp_txt_get(txt, "scan");
        if (scan != NULL) {
            dev->scan = dev->scan || !strcasecmp(scan, "t");
            free(scan);
        }

        /* Build the URI, preferring IPv4: it is resolved first, but
         * both resolvers run in parallel and either may answer first.
         */
        if (dev->uri == NULL || (dev->uri_is_ip6 &&
                addr->proto == AVAHI_PROTO_INET)) {
            free(dev->uri);
            dev->uri = ipp_uri_make(dev->tls, straddr, port, dev->rp);
            dev->uri_is_ip6 = addr->proto == AVAHI_PROTO_INET6;
        }

        ipp_device_add_addr(dev, straddr);
        break;

    case AVAHI_RESOLVER_FAILURE:
        ipp_mdns_dbg("resolve %s (%s): %s", name, type,
            avahi_strerror(avahi_client_errno(ctx->client)));
        break;
    }

    /* Unlink the resolver context and drop the resolver */
    prev = &ctx->rctx_list;
    while (*prev != NULL && *prev != rctx) {
        prev = &(*prev)->next;
    }
    if (*prev == rctx) {
        *prev = rctx->next;
    }

    avahi_service_resolver_free(r);
    free(rctx);

    ctx->pending --;
    ipp_mdns_check_done(ctx);
}

/* Start a resolver for the given protocol. Returns true on success.
 */
static bool
ipp_mdns_resolve (ipp_mdns_ctx *ctx, ipp_device *dev, AvahiIfIndex interface,
        AvahiProtocol protocol, const char *name, const char *type,
        const char *domain, AvahiProtocol want)
{
    ipp_mdns_rctx        *rctx;
    AvahiServiceResolver *r;

    rctx = calloc(1, sizeof(*rctx));
    if (rctx == NULL) {
        perror("ipp-mdns: calloc");
        abort();
    }

    rctx->ctx = ctx;
    rctx->device = dev;

    r = avahi_service_resolver_new(ctx->client, interface, protocol,
            name, type, domain, want, 0, ipp_mdns_resolver_callback, rctx);

    if (r == NULL) {
        ipp_mdns_dbg("avahi_service_resolver_new(%s): %s", name,
            avahi_strerror(avahi_client_errno(ctx->client)));
        free(rctx);
        return false;
    }

    rctx->resolver = r;
    rctx->next = ctx->rctx_list;
    ctx->rctx_list = rctx;
    ctx->pending ++;

    return true;
}

/* AVAHI service browser callback
 */
static void
ipp_mdns_browser_callback (AvahiServiceBrowser *b, AvahiIfIndex interface,
        AvahiProtocol protocol, AvahiBrowserEvent event,
        const char *name, const char *type, const char *domain,
        AvahiLookupResultFlags flags, void *userdata)
{
    ipp_mdns_bctx *bctx = userdata;
    ipp_mdns_ctx  *ctx = bctx->ctx;
    MDNS_SERVICE  service = bctx->service;
    bool          tls = service == MDNS_SERVICE_IPPS_TCP;
    ipp_device    *dev;

    (void) b;
    (void) flags;

    switch (event) {
    case AVAHI_BROWSER_NEW:
        ipp_mdns_dbg("browse %s: new \"%s\" (if=%d)", type, name,
            (int) interface);

        dev = ipp_device_get(ctx, name, type, (int) interface, tls);

        /* Resolve IPv4 and IPv6 separately, exactly as sane-airscan does:
         * a single AVAHI_PROTO_UNSPEC resolver would only report one of
         * the address families.
         */
        ipp_mdns_resolve(ctx, dev, interface, protocol, name, type, domain,
            AVAHI_PROTO_INET);
        ipp_mdns_resolve(ctx, dev, interface, protocol, name, type, domain,
            AVAHI_PROTO_INET6);
        break;

    case AVAHI_BROWSER_REMOVE:
        ipp_mdns_dbg("browse %s: remove \"%s\" (if=%d)", type, name,
            (int) interface);
        ipp_device_del(ctx, name, (int) interface, tls);
        break;

    case AVAHI_BROWSER_FAILURE:
        ipp_mdns_dbg("browse %s: %s", type,
            avahi_strerror(avahi_client_errno(ctx->client)));
        ctx->err = "MDNS browser failed";
        avahi_simple_poll_quit(ctx->poll);
        break;

    case AVAHI_BROWSER_CACHE_EXHAUSTED:
        break;

    case AVAHI_BROWSER_ALL_FOR_NOW:
        ipp_mdns_dbg("browse %s: all for now", type);
        ctx->all_for_now[service] = true;
        ipp_mdns_check_done(ctx);
        break;
    }
}

/* AVAHI client callback
 */
static void
ipp_mdns_client_callback (AvahiClient *client, AvahiClientState state,
        void *userdata)
{
    ipp_mdns_ctx *ctx = userdata;

    (void) client;

    if (state == AVAHI_CLIENT_FAILURE) {
        ipp_mdns_dbg("client failure: %s",
            avahi_strerror(avahi_client_errno(ctx->client)));
        ctx->err = "AVAHI client failed";
        avahi_simple_poll_quit(ctx->poll);
    }
}

/* Get current time in milliseconds, from a monotonic clock
 */
static int64_t
ipp_mdns_now (void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Release everything the context holds, except the device list
 */
static void
ipp_mdns_ctx_cleanup (ipp_mdns_ctx *ctx)
{
    int i;

    for (i = 0; i < NUM_MDNS_SERVICE; i ++) {
        if (ctx->browser[i] != NULL) {
            avahi_service_browser_free(ctx->browser[i]);
            ctx->browser[i] = NULL;
        }
    }

    /* Resolvers still pending: their callbacks will never run, so
     * free them and their contexts here.
     */
    while (ctx->rctx_list != NULL) {
        ipp_mdns_rctx *rctx = ctx->rctx_list;

        ctx->rctx_list = rctx->next;
        avahi_service_resolver_free(rctx->resolver);
        free(rctx);
        ctx->pending --;
    }

    if (ctx->client != NULL) {
        avahi_client_free(ctx->client);
        ctx->client = NULL;
    }

    if (ctx->poll != NULL) {
        avahi_simple_poll_free(ctx->poll);
        ctx->poll = NULL;
    }
}

/* Enable/disable discovery debug messages
 */
void
ipp_mdns_debug_enable (bool enable)
{
    ipp_mdns_debug = enable;
}

/* Browse the network for IPP devices
 */
ipp_device*
ipp_mdns_discover (int timeout_ms, const char **err)
{
    ipp_mdns_ctx  ctx;
    ipp_mdns_bctx bctx[NUM_MDNS_SERVICE];
    ipp_device    *devices;
    int64_t       deadline;
    int           avahi_err = 0;
    int           i;

    memset(&ctx, 0, sizeof(ctx));
    memset(bctx, 0, sizeof(bctx));

    if (err != NULL) {
        *err = NULL;
    }

    /* Create poll and client */
    ctx.poll = avahi_simple_poll_new();
    if (ctx.poll == NULL) {
        ctx.err = "Not enough memory";
        goto DONE;
    }

    ctx.client = avahi_client_new(avahi_simple_poll_get(ctx.poll),
            0, ipp_mdns_client_callback, &ctx, &avahi_err);

    if (ctx.client == NULL) {
        ipp_mdns_dbg("avahi_client_new: %s", avahi_strerror(avahi_err));
        ctx.err = "Cannot connect to the AVAHI daemon";
        goto DONE;
    }

    /* Start browsers */
    for (i = 0; i < NUM_MDNS_SERVICE; i ++) {
        bctx[i].ctx = &ctx;
        bctx[i].service = (MDNS_SERVICE) i;

        ctx.browser[i] = avahi_service_browser_new(ctx.client,
                AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC,
                ipp_mdns_service_types[i], NULL, 0,
                ipp_mdns_browser_callback, &bctx[i]);

        if (ctx.browser[i] == NULL) {
            ipp_mdns_dbg("avahi_service_browser_new(%s): %s",
                ipp_mdns_service_types[i],
                avahi_strerror(avahi_client_errno(ctx.client)));
            ctx.err = "Cannot start MDNS browser";
            goto DONE;
        }
    }

    /* Run the poll until discovery settles or the timeout expires */
    deadline = ipp_mdns_now() + timeout_ms;

    for (;;) {
        int64_t remain = deadline - ipp_mdns_now();
        int     rc;

        if (remain <= 0) {
            ipp_mdns_dbg("discovery timed out");
            break;
        }

        rc = avahi_simple_poll_iterate(ctx.poll, (int) remain);
        if (rc != 0) {
            /* rc > 0 means avahi_simple_poll_quit() was called */
            if (rc < 0) {
                ipp_mdns_dbg("avahi_simple_poll_iterate: %d", rc);
                ctx.err = "AVAHI poll failed";
            }
            break;
        }
    }

DONE:
    ipp_mdns_ctx_cleanup(&ctx);

    devices = ctx.devices;
    ctx.devices = NULL;

    if (ctx.err != NULL) {
        if (err != NULL) {
            *err = ctx.err;
        }

        ipp_device_list_free(devices);

        return NULL;
    }

    return devices;
}
