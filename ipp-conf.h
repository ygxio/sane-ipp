/* sane-ipp -- IPP backend for SANE
 *
 * Copyright (C) 2026 Alexander Pevzner (pzz@apevzner.com)
 * Copyright (C) 2026 Yogesh Singla (yogeshsingla481@gmail.com)
 * SPDX-License-Identifier: BSD-2-Clause
 * See LICENSE for license terms and conditions
 *
 * Configuration file
 *
 * The file is ipp.conf, looked for in each directory of SANE_CONFIG_DIR
 * and then in the directory the backend was installed with. Every file
 * found is read, in that order, so a later one overrides an earlier.
 *
 * Only the [debug] section is understood so far:
 *
 *   [debug]
 *   enable = true|false    ; debug messages on stderr
 *   trace  = ~/ipp/trace   ; directory for per-device log files
 */

#ifndef ipp_conf_h
#define ipp_conf_h

#include <stdbool.h>

/* The configuration
 */
typedef struct {
    bool dbg_enabled;       /* [debug] enable */
    char *dbg_trace;        /* [debug] trace, "~/" expanded, or NULL */
} ipp_conf;

/* Load the configuration. Missing files are not an error; a line that
 * cannot be understood is reported on stderr and skipped.
 */
void
ipp_conf_load (ipp_conf *conf);

/* Release what ipp_conf_load() allocated
 */
void
ipp_conf_free (ipp_conf *conf);

#endif
