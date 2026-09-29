/* sane-ipp -- IPP backend for SANE
 *
 * Copyright (C) 2026 Alexander Pevzner (pzz@apevzner.com)
 * Copyright (C) 2026 Yogesh Singla (yogeshsingla481@gmail.com)
 * SPDX-License-Identifier: BSD-2-Clause
 * See LICENSE for license terms and conditions
 *
 * Configuration file
 */

#include "ipp-conf.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/******************** Constants ********************/
/* Where SANE keeps backend configuration. The Makefile supplies the
 * real one; this is only for a build outside of it.
 */
#ifndef IPP_CONFIG_DIR
#   define IPP_CONFIG_DIR       "/etc/sane.d"
#endif

/* Name of the file, and the variable that adds directories to look in
 */
#define IPP_CONF_FILE           "ipp.conf"
#define IPP_CONF_PATH_ENV       "SANE_CONFIG_DIR"

/******************** Helpers ********************/
/* Strip white space from both ends of a string, in place
 */
static char*
ipp_conf_trim (char *s)
{
    char *end;

    while (isspace((unsigned char) *s)) {
        s ++;
    }

    end = s + strlen(s);
    while (end > s && isspace((unsigned char) end[-1])) {
        *--end = '\0';
    }

    return s;
}

/* Expand a leading "~/" to the home directory. Returns a new string.
 */
static char*
ipp_conf_expand_path (const char *path)
{
    const char *home = getenv("HOME");
    char       *out;

    if (strncmp(path, "~/", 2) != 0 || home == NULL) {
        return strdup(path);
    }

    if (asprintf(&out, "%s/%s", home, path + 2) < 0) {
        return NULL;
    }

    return out;
}

/* Parse a boolean value
 */
static bool
ipp_conf_bool (const char *value, bool *out)
{
    if (!strcasecmp(value, "true") || !strcasecmp(value, "yes") ||
        !strcmp(value, "1")) {
        *out = true;
        return true;
    }

    if (!strcasecmp(value, "false") || !strcasecmp(value, "no") ||
        !strcmp(value, "0")) {
        *out = false;
        return true;
    }

    return false;
}

/******************** Loading ********************/
/* Apply one "name = value" of a section
 */
static void
ipp_conf_apply (ipp_conf *conf, const char *path, int lineno,
        const char *section, const char *name, const char *value)
{
    if (!strcasecmp(section, "debug")) {
        if (!strcasecmp(name, "enable")) {
            if (ipp_conf_bool(value, &conf->dbg_enabled)) {
                return;
            }
        } else if (!strcasecmp(name, "trace")) {
            free(conf->dbg_trace);
            conf->dbg_trace = *value ? ipp_conf_expand_path(value) : NULL;
            return;
        }
    }

    fprintf(stderr, "ipp: %s:%d: [%s] %s = %s: not understood\n",
            path, lineno, section, name, value);
}

/* Load one file, if it is there
 */
static void
ipp_conf_load_file (ipp_conf *conf, const char *path)
{
    FILE *fp = fopen(path, "r");
    char line[1024];
    char section[64] = "";
    int  lineno = 0;

    if (fp == NULL) {
        return;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *s = ipp_conf_trim(line), *eq, *name, *value;
        size_t len;

        lineno ++;

        if (*s == '\0' || *s == ';' || *s == '#') {
            continue;
        }

        if (*s == '[') {
            char *end = strchr(s, ']');

            if (end != NULL) {
                *end = '\0';
                snprintf(section, sizeof(section), "%s",
                        ipp_conf_trim(s + 1));
                continue;
            }
        }

        eq = strchr(s, '=');
        if (eq == NULL) {
            fprintf(stderr, "ipp: %s:%d: syntax error\n", path, lineno);
            continue;
        }

        *eq = '\0';
        name = ipp_conf_trim(s);
        value = ipp_conf_trim(eq + 1);

        /* A value may be quoted, to keep white space at its ends
         */
        len = strlen(value);
        if (len >= 2 && value[0] == '"' && value[len - 1] == '"') {
            value[len - 1] = '\0';
            value ++;
        }

        ipp_conf_apply(conf, path, lineno, section, name, value);
    }

    fclose(fp);
}

/* Load the configuration
 */
void
ipp_conf_load (ipp_conf *conf)
{
    const char *env = getenv(IPP_CONF_PATH_ENV);
    char       *dirs, *dir, *save = NULL;
    char       path[4096];

    memset(conf, 0, sizeof(*conf));

    if (asprintf(&dirs, "%s:%s", env != NULL ? env : "",
            IPP_CONFIG_DIR) < 0) {
        return;
    }

    for (dir = strtok_r(dirs, ":", &save); dir != NULL;
            dir = strtok_r(NULL, ":", &save)) {
        snprintf(path, sizeof(path), "%s/%s", dir, IPP_CONF_FILE);
        ipp_conf_load_file(conf, path);
    }

    free(dirs);
}

/* Release the configuration
 */
void
ipp_conf_free (ipp_conf *conf)
{
    free(conf->dbg_trace);
    conf->dbg_trace = NULL;
}
