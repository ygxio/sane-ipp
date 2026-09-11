/* sane-ipp -- IPP backend for SANE
 *
 * Copyright (C) 2026 Alexander Pevzner (pzz@apevzner.com)
 * Copyright (C) 2026 Yogesh Singla (yogeshsingla481@gmail.com)
 * SPDX-License-Identifier: BSD-2-Clause
 * See LICENSE for license terms and conditions
 *
 * Command-line IPP printer attributes tool
 */

#include "ipp-proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Print usage and exit
 */
static void
usage (const char *argv0, int exit_code)
{
    printf("usage: %s [-d] [-t TIMEOUT_MS] URI...\n", argv0);
    printf("  -d            enable debug output\n");
    printf("  -t TIMEOUT_MS request timeout, default 5000\n");
    printf("\n");
    printf("URI is an ipp:// or ipps:// printer URI, as reported\n");
    printf("by ipp-discover.\n");
    exit(exit_code);
}

/* Print one labelled value, or nothing if the printer reported it as
 * absent or empty. Empty attributes are common and carry no information,
 * so they are not worth a line of output.
 */
static void
print_str (const char *label, const char *value)
{
    if (value == NULL || value[0] == '\0') {
        return;
    }

    printf("  %-16s%s\n", label, value);
}

/* Print a list of strings on one line, or nothing if the list is empty
 */
static void
print_str_list (const char *label, char **list, size_t count)
{
    size_t i;

    if (count == 0) {
        return;
    }

    printf("  %-16s", label);
    for (i = 0; i < count; i ++) {
        printf("%s%s", i != 0 ? " " : "", list[i]);
    }
    printf("\n");
}

/* Print the operations the printer supports, one per line
 */
static void
print_ops (const ipp_printer *printer)
{
    size_t i;

    if (printer->n_ops == 0) {
        return;
    }

    printf("  %-16s%zu\n", "operations:", printer->n_ops);
    for (i = 0; i < printer->n_ops; i ++) {
        printf("      0x%04x  %s\n", printer->ops[i],
                ipp_op_name(printer->ops[i]));
    }
}

/* Print everything known about one printer
 */
static void
print_printer (const char *uri, const ipp_printer *printer)
{
    printf("%s\n", uri);

    print_str("model:", printer->make_and_model);
    print_str("dns-sd-name:", printer->dns_sd_name);
    print_str("info:", printer->info);
    print_str("location:", printer->location);
    print_str("uuid:", printer->uuid);

    printf("  %-16s%s (%d), accepting jobs: %s\n", "state:",
            ipp_state_name(printer->state), printer->state,
            printer->accepting_jobs ? "yes" : "no");

    print_str("state-message:", printer->state_message);

    print_str_list("versions:", printer->versions, printer->n_versions);
    print_str_list("uris:", printer->uris, printer->n_uris);
    print_str_list("formats:", printer->formats, printer->n_formats);
    print_ops(printer);

    printf("\n");
}

/* The main function
 */
int
main (int argc, char **argv)
{
    int  timeout = 5000;
    int  i, probed = 0, failed = 0;
    char err[512];

    for (i = 1; i < argc; i ++) {
        if (!strcmp(argv[i], "-d")) {
            ipp_proto_debug_enable(true);
        } else if (!strcmp(argv[i], "-t") && i + 1 < argc) {
            timeout = atoi(argv[++ i]);
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0], 0);
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "%s: unknown option %s\n", argv[0], argv[i]);
            usage(argv[0], 1);
        } else {
            ipp_printer *printer;

            printer = ipp_get_printer_attributes(argv[i], timeout,
                    err, sizeof(err));

            if (printer == NULL) {
                fprintf(stderr, "%s\n", err);
                failed ++;
                continue;
            }

            print_printer(argv[i], printer);
            ipp_printer_free(printer);
            probed ++;
        }
    }

    if (probed == 0 && failed == 0) {
        fprintf(stderr, "%s: no URI given\n", argv[0]);
        usage(argv[0], 1);
    }

    return failed != 0 ? 1 : 0;
}
