/* sane-ipp -- IPP backend for SANE
 *
 * Copyright (C) 2026 Alexander Pevzner (pzz@apevzner.com)
 * Copyright (C) 2026 Yogesh Singla (yogeshsingla481@gmail.com)
 * SPDX-License-Identifier: BSD-2-Clause
 * See LICENSE for license terms and conditions
 *
 * Logging
 */

#include "ipp-log.h"
#include "ipp-conf.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/******************** Constants ********************/
/* Environment variable of the stderr level
 */
#define IPP_LOG_LEVEL_ENV       "SANE_DEBUG_IPP"

/* Longest message written in one piece. A longer one is cut short.
 */
#define IPP_LOG_MSG_MAX         4096

/******************** Static variables ********************/
static bool             ipp_log_initialized;
static int              ipp_log_stderr_level;
static FILE             *ipp_log_file;   /* Trace file, or NULL */
static ipp_conf         ipp_log_conf;

/* The backend itself is single-threaded, but a frontend is free to call
 * it from threads of its own, and a log line must not be torn in two
 * by another.
 */
static pthread_mutex_t  ipp_log_mutex = PTHREAD_MUTEX_INITIALIZER;

/******************** Helpers ********************/
/* Name of a level, as it appears in a message
 */
static const char*
ipp_log_level_name (ipp_log_level level)
{
    switch (level) {
    case IPP_LOG_ERROR: return "error";
    case IPP_LOG_INFO:  return "info";
    case IPP_LOG_DEBUG: return "debug";
    case IPP_LOG_TRACE: return "trace";
    }

    return "?";
}

/* Parse the stderr level. "true" is the level of "enable = true" in
 * the configuration; anything else that is not a number, but is not
 * empty either, asks for everything: SANE_DEBUG_IPP=yes should not
 * leave its user wondering why nothing came out.
 */
static int
ipp_log_parse_level (const char *value)
{
    char *end;
    long level;

    if (value == NULL || *value == '\0' || !strcasecmp(value, "false")) {
        return 0;
    }

    if (!strcasecmp(value, "true")) {
        return IPP_LOG_DEBUG;
    }

    level = strtol(value, &end, 10);
    if (*end != '\0' || level < 0) {
        return IPP_LOG_TRACE;
    }

    return level > IPP_LOG_TRACE ? IPP_LOG_TRACE : (int) level;
}

/* Format the wall-clock time, to the millisecond
 */
static void
ipp_log_fmt_time (char *buf, size_t size, bool with_date)
{
    struct timeval tv;
    struct tm      tm;
    char           tmp[32];

    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, &tm);

    strftime(tmp, sizeof(tmp), with_date ? "%Y-%m-%d %H:%M:%S" : "%H:%M:%S",
            &tm);
    snprintf(buf, size, "%s.%03d", tmp, (int) (tv.tv_usec / 1000));
}

/* Create a directory along with any of its parents that are missing
 */
static int
ipp_log_mkdir (const char *dir)
{
    char *path = strdup(dir), *p;
    int  rc = 0;

    if (path == NULL) {
        return -1;
    }

    for (p = path + 1; rc == 0; p ++) {
        if (*p == '/' || *p == '\0') {
            char c = *p;

            *p = '\0';
            if (mkdir(path, 0755) != 0 && errno != EEXIST) {
                rc = -1;
            }
            *p = c;

            if (c == '\0') {
                break;
            }
        }
    }

    free(path);

    return rc;
}

/* Close the trace file. The mutex must be held.
 */
static void
ipp_log_trace_close_locked (void)
{
    char now[64];

    if (ipp_log_file == NULL) {
        return;
    }

    ipp_log_fmt_time(now, sizeof(now), true);
    fprintf(ipp_log_file, "===== trace closed %s =====\n", now);
    fclose(ipp_log_file);
    ipp_log_file = NULL;
}

/******************** Setup ********************/
/* Set up logging out of ipp.conf and the environment
 */
void
ipp_log_init (void)
{
    const char *env;

    pthread_mutex_lock(&ipp_log_mutex);

    if (ipp_log_initialized) {
        pthread_mutex_unlock(&ipp_log_mutex);
        return;
    }

    ipp_log_initialized = true;

    /* The environment has the last word, so that a user can turn
     * messages on for one run without editing the configuration
     */
    ipp_conf_load(&ipp_log_conf);
    ipp_log_stderr_level = ipp_log_conf.dbg_enabled ? IPP_LOG_DEBUG : 0;

    env = getenv(IPP_LOG_LEVEL_ENV);
    if (env != NULL) {
        ipp_log_stderr_level = ipp_log_parse_level(env);
    }

    pthread_mutex_unlock(&ipp_log_mutex);
}

/* Close the trace file and forget the configuration
 */
void
ipp_log_exit (void)
{
    pthread_mutex_lock(&ipp_log_mutex);

    ipp_log_trace_close_locked();
    ipp_conf_free(&ipp_log_conf);
    ipp_log_initialized = false;

    pthread_mutex_unlock(&ipp_log_mutex);
}

/* Start the trace file of a device
 */
void
ipp_log_trace_open (const char *device)
{
    const char *dir = ipp_log_conf.dbg_trace;
    char       *path, *p;
    char       now[64];
    size_t     prefix;

    if (dir == NULL) {
        return;
    }

    if (device == NULL || *device == '\0') {
        device = "default";
    }

    if (ipp_log_mkdir(dir) != 0) {
        fprintf(stderr, "ipp: %s: cannot create trace directory: %m\n", dir);
        return;
    }

    if (asprintf(&path, "%s/%s-", dir, program_invocation_short_name) < 0) {
        return;
    }

    prefix = strlen(path);

    if (asprintf(&p, "%s%s.log", path, device) < 0) {
        free(path);
        return;
    }

    free(path);
    path = p;

    /* A device name is free text, and a URI is full of slashes, and
     * neither is a good file name as it is
     */
    for (p = path + prefix; *p != '\0'; p ++) {
        if (*p == '/' || *p == ' ') {
            *p = '-';
        }
    }

    pthread_mutex_lock(&ipp_log_mutex);

    ipp_log_trace_close_locked();

    ipp_log_file = fopen(path, "w");
    if (ipp_log_file == NULL) {
        fprintf(stderr, "ipp: %s: cannot open trace file: %m\n", path);
    } else {
        setvbuf(ipp_log_file, NULL, _IOLBF, 0);
        ipp_log_fmt_time(now, sizeof(now), true);
        fprintf(ipp_log_file, "===== trace of \"%s\" opened %s, "
                "pid %d =====\n", device, now, (int) getpid());
    }

    pthread_mutex_unlock(&ipp_log_mutex);

    free(path);
}

/* End the trace file
 */
void
ipp_log_trace_close (void)
{
    pthread_mutex_lock(&ipp_log_mutex);
    ipp_log_trace_close_locked();
    pthread_mutex_unlock(&ipp_log_mutex);
}

/* Override the stderr level
 */
void
ipp_log_set_level (ipp_log_level level)
{
    ipp_log_stderr_level = (int) level;
}

/******************** Messages ********************/
/* Report whether a message of the given level would be written
 */
bool
ipp_log_enabled (ipp_log_level level)
{
    return ipp_log_file != NULL || (int) level <= ipp_log_stderr_level;
}

/* Write a message
 */
void
ipp_log (ipp_log_level level, const char *module, const char *fmt, ...)
{
    char    msg[IPP_LOG_MSG_MAX];
    char    now[32];
    va_list ap;
    size_t  len;
    bool    to_stderr = (int) level <= ipp_log_stderr_level;

    if (!to_stderr && ipp_log_file == NULL) {
        return;
    }

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    /* Callers pass messages from elsewhere through here, such as an
     * error string of a library, and those may end in a newline of
     * their own
     */
    len = strlen(msg);
    while (len > 0 && (msg[len - 1] == '\n' || msg[len - 1] == '\r')) {
        msg[--len] = '\0';
    }

    ipp_log_fmt_time(now, sizeof(now), false);

    pthread_mutex_lock(&ipp_log_mutex);

    if (to_stderr) {
        fprintf(stderr, "[%s] ipp-%s: %s%s\n", now, module,
                level == IPP_LOG_ERROR ? "ERROR: " : "", msg);
    }

    if (ipp_log_file != NULL) {
        fprintf(ipp_log_file, "%s %-5s %-6s %s\n", now,
                ipp_log_level_name(level), module, msg);
    }

    pthread_mutex_unlock(&ipp_log_mutex);
}
