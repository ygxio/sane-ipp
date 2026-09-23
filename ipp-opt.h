/* sane-ipp -- IPP backend for SANE
 *
 * Copyright (C) 2026 Alexander Pevzner (pzz@apevzner.com)
 * Copyright (C) 2026 Yogesh Singla (yogeshsingla481@gmail.com)
 * SPDX-License-Identifier: BSD-2-Clause
 * See LICENSE for license terms and conditions
 *
 * SANE options, built from the scan capabilities
 *
 * A scan service describes itself in IPP terms, which are finer than
 * what SANE can express: sixteen colour modes where SANE has three, and
 * a duplex setting kept apart from the input source where SANE folds
 * the two together. This layer performs that translation in both
 * directions, so that the rest of the backend speaks IPP and a frontend
 * sees an ordinary SANE device.
 */

#ifndef ipp_opt_h
#define ipp_opt_h

#include "ipp-proto.h"

#include <sane/sane.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Option numbers.
 *
 * Option zero is required by the SANE standard to exist and to report
 * how many options the device has, so that a frontend can discover the
 * rest.
 *
 * Every option is always present. One the device has nothing to say
 * about is reported as inactive rather than left out, so that these
 * numbers mean the same thing on every device.
 */
typedef enum {
    IPP_OPT_NUM_OPTIONS = 0,    /* Option count, read-only */

    IPP_OPT_GROUP_STANDARD,     /* Group header */
    IPP_OPT_MODE,               /* Lineart, Gray or Color */
    IPP_OPT_RESOLUTION,         /* In DPI */
    IPP_OPT_SOURCE,             /* Flatbed, ADF or ADF Duplex */

    IPP_OPT_GROUP_GEOMETRY,     /* Group header */
    IPP_OPT_TL_X,               /* Scan area, in mm */
    IPP_OPT_TL_Y,
    IPP_OPT_BR_X,
    IPP_OPT_BR_Y,

    NUM_IPP_OPT
} IPP_OPT;

/* The options of one open device, with their current values
 */
typedef struct ipp_opt ipp_opt;

/* Build the options of a device from its scan capabilities.
 *
 * Returns NULL if out of memory. The scanner must outlive the returned
 * options, which keep pointers into it. Release with ipp_opt_free().
 */
ipp_opt*
ipp_opt_new (const ipp_scanner *scanner);

/* Release the options
 */
void
ipp_opt_free (ipp_opt *opt);

/* Return the descriptor of an option, or NULL if there is no such
 * option
 */
const SANE_Option_Descriptor*
ipp_opt_descriptor (const ipp_opt *opt, SANE_Int n);

/* Read the current value of an option into the caller's buffer, which
 * must be at least as large as the option's descriptor says
 */
SANE_Status
ipp_opt_get (const ipp_opt *opt, SANE_Int n, void *value);

/* Set an option.
 *
 * A value the device cannot take exactly is replaced with the nearest
 * one it can, and SANE_INFO_INEXACT is reported. info may be NULL.
 */
SANE_Status
ipp_opt_set (ipp_opt *opt, SANE_Int n, void *value, SANE_Int *info);

/* The IPP keywords and values the current settings amount to.
 *
 * These are what goes into a scan request. A NULL keyword means the
 * device reported no choice and the attribute is to be left out.
 */
const char*
ipp_opt_color_mode (const ipp_opt *opt);

const char*
ipp_opt_source (const ipp_opt *opt);

const char*
ipp_opt_sides (const ipp_opt *opt);

int
ipp_opt_resolution (const ipp_opt *opt);

/* The scan area the current settings amount to, as the members of an
 * "input-scan-regions" collection, in hundredths of a millimetre.
 *
 * The corners may have been set either way round, so the area is taken
 * between them whichever that is. Returns false if the device reported
 * nothing to build a scan area from, in which case the attribute is to
 * be left out.
 */
bool
ipp_opt_scan_region (const ipp_opt *opt, int *x_origin, int *y_origin,
        int *x_dimension, int *y_dimension);

#ifdef __cplusplus
}
#endif

#endif
