/* sane-ipp -- IPP backend for SANE
 *
 * Copyright (C) 2026 Alexander Pevzner (pzz@apevzner.com)
 * Copyright (C) 2026 Yogesh Singla (yogeshsingla481@gmail.com)
 * SPDX-License-Identifier: BSD-2-Clause
 * See LICENSE for license terms and conditions
 *
 * Logging
 *
 * Messages go to two places, each with a verbosity of its own:
 *
 *   stderr      SANE_DEBUG_IPP=<level>|true|false, or "enable" in the
 *               [debug] section of ipp.conf. Off by default. This is
 *               where a user running a frontend from a terminal looks
 *               first.
 *
 *   trace file  "trace" in the [debug] section of ipp.conf names a
 *               directory. Opening a device starts a file there named
 *               <program>-<device>.log, replacing the one of the last
 *               time, and closing the device ends it. Always written at
 *               the most verbose level, so that one file from a user
 *               holds everything needed to follow what happened to
 *               their device, IPP messages included.
 *
 * The levels are those of SANE_DEBUG_IPP, each including the ones
 * before it. "true", or "enable = true", means IPP_LOG_DEBUG.
 */

#ifndef ipp_log_h
#define ipp_log_h

#include <stdbool.h>

/* Message levels
 */
typedef enum {
    IPP_LOG_ERROR = 1,  /* An operation failed, and why */
    IPP_LOG_INFO  = 2,  /* Milestones: devices found, job created, page done */
    IPP_LOG_DEBUG = 3,  /* The steps between the milestones */
    IPP_LOG_TRACE = 4   /* IPP messages in full, and every chunk of data */
} ipp_log_level;

/* Set up logging out of ipp.conf and the environment. May be called
 * more than once; only the first call after ipp_log_exit() does
 * anything.
 */
void
ipp_log_init (void);

/* Close the trace file, if one is open, and forget the configuration
 */
void
ipp_log_exit (void);

/* Start the trace file of a device, if a trace directory is
 * configured. A trace file already open is closed first: there is one
 * at a time, of the device most recently opened.
 */
void
ipp_log_trace_open (const char *device);

/* End the trace file, if one is open
 */
void
ipp_log_trace_close (void);

/* Override the stderr level given by the environment. Used by the
 * command-line tools for their -d switch.
 */
void
ipp_log_set_level (ipp_log_level level);

/* Report whether a message of the given level would be written
 * anywhere. Lets a caller skip the work of building a large message,
 * such as a dump of an IPP message, that nobody will see.
 */
bool
ipp_log_enabled (ipp_log_level level);

/* Write a message. The module names the part of the backend it comes
 * from, such as "proto" or "mdns". A trailing newline is not needed.
 */
void
ipp_log (ipp_log_level level, const char *module, const char *fmt, ...)
    __attribute__ ((format (printf, 3, 4)));

#endif
