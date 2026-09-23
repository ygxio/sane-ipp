/* sane-ipp -- IPP backend for SANE
 *
 * Copyright (C) 2026 Alexander Pevzner (pzz@apevzner.com)
 * Copyright (C) 2026 Yogesh Singla (yogeshsingla481@gmail.com)
 * SPDX-License-Identifier: BSD-2-Clause
 * See LICENSE for license terms and conditions
 *
 * SANE options, built from the scan capabilities
 */

#include "ipp-opt.h"

#include <cups/cups.h>
#include <sane/saneopts.h>

#include <stdlib.h>
#include <string.h>
#include <strings.h>

/******************** Constants ********************/
/* The most SANE sources an IPP service can amount to: a platen, an ADF
 * in each of its two ways of being used, and a film reader
 */
#define IPP_OPT_MAX_SOURCES     4

/* The most SANE colour modes there are
 */
#define IPP_OPT_MAX_MODES       3

/* The resolution a scan is set to when the device offers a choice and
 * the frontend has not made one. It is what most documents are scanned
 * at, and every device that offers anything offers something near it.
 */
#define IPP_OPT_DEFAULT_DPI     300

/******************** Local Types ********************/
/* A colour mode as the frontend sees it, and the IPP keyword it stands
 * for
 */
typedef struct {
    const char *sane;           /* Lineart, Gray or Color */
    const char *keyword;        /* input-color-mode keyword */
} ipp_opt_mode;

/* A source as the frontend sees it, and the IPP it stands for.
 *
 * SANE has no duplex setting: an ADF used on both sides is a source of
 * its own. So one SANE source carries two IPP keywords.
 */
typedef struct {
    const char *sane;           /* Flatbed, ADF, ADF Duplex or Film */
    const char *source;         /* input-source keyword */
    const char *sides;          /* input-sides keyword, NULL if none */
} ipp_opt_src;

/* The options of one open device, with their current values
 */
struct ipp_opt {
    SANE_Option_Descriptor desc[NUM_IPP_OPT];

    /* Colour modes. mode_names is what the descriptor constrains
     * against, so it is NULL-terminated, as SANE requires.
     */
    ipp_opt_mode      modes[IPP_OPT_MAX_MODES];
    SANE_String_Const mode_names[IPP_OPT_MAX_MODES + 1];
    size_t            n_modes;
    size_t            mode;             /* Index of the current one */

    /* Sources, the same way
     */
    ipp_opt_src       sources[IPP_OPT_MAX_SOURCES];
    SANE_String_Const source_names[IPP_OPT_MAX_SOURCES + 1];
    size_t            n_sources;
    size_t            source;

    /* Resolutions, in the SANE word list form: the count first, then
     * that many values, ascending
     */
    SANE_Word         *resolutions;
    size_t            resolution;       /* Index into the values, from 1 */

    /* Scan area. The limits are what the descriptors constrain against,
     * in millimetres. The corners are kept in hundredths of a
     * millimetre, the unit IPP uses, so that no rounding happens
     * between what the frontend set and what goes into a request.
     */
    bool              has_area;
    SANE_Range        x_origin_range;
    SANE_Range        y_origin_range;
    SANE_Range        x_range;
    SANE_Range        y_range;
    int               tl_x, tl_y;
    int               br_x, br_y;
};

/******************** Capability lookup ********************/
/* Report whether a keyword appears in one of the scanner's lists
 */
static bool
ipp_opt_supported (char * const *list, size_t count, const char *keyword)
{
    size_t i;

    for (i = 0; i < count; i ++) {
        if (list[i] != NULL && strcasecmp(list[i], keyword) == 0) {
            return true;
        }
    }

    return false;
}

/* Return the first of the given keywords the scanner supports, or NULL
 */
static const char*
ipp_opt_first_supported (char * const *list, size_t count,
        const char * const *keywords, size_t n_keywords)
{
    size_t i;

    for (i = 0; i < n_keywords; i ++) {
        if (ipp_opt_supported(list, count, keywords[i])) {
            return keywords[i];
        }
    }

    return NULL;
}

/******************** Colour modes ********************/
/* How IPP colour modes collapse onto the three SANE has.
 *
 * IPP names a mode by what it produces, down to the bit depth, where
 * SANE names it by kind alone, so this is many to one. The order is
 * the order the frontend sees, and within one SANE mode the first
 * entry the device supports is the one used.
 *
 * "auto" is deliberately absent. It asks the device to decide, which
 * SANE has no way to express and a frontend has no way to anticipate,
 * and a device offering it always offers something definite as well.
 */
static const ipp_opt_mode ipp_opt_color_modes[] = {
    { SANE_VALUE_SCAN_MODE_LINEART, "bi-level"      },
    { SANE_VALUE_SCAN_MODE_GRAY,    "monochrome"    },
    { SANE_VALUE_SCAN_MODE_GRAY,    "monochrome_8"  },
    { SANE_VALUE_SCAN_MODE_GRAY,    "monochrome_16" },
    { SANE_VALUE_SCAN_MODE_GRAY,    "monochrome_4"  },
    { SANE_VALUE_SCAN_MODE_COLOR,   "color"         },
    { SANE_VALUE_SCAN_MODE_COLOR,   "color_8"       },
    { SANE_VALUE_SCAN_MODE_COLOR,   "rgb_16"        },
    { SANE_VALUE_SCAN_MODE_COLOR,   "rgba_8"        },
    { SANE_VALUE_SCAN_MODE_COLOR,   "rgba_16"       },
    { SANE_VALUE_SCAN_MODE_COLOR,   "cmyk_8"        },
    { SANE_VALUE_SCAN_MODE_COLOR,   "cmyk_16"       }
};

/* Work out the colour modes the device offers
 */
static void
ipp_opt_modes_build (ipp_opt *opt, const ipp_scanner *scanner)
{
    size_t i, j;

    for (i = 0; i < sizeof(ipp_opt_color_modes) /
            sizeof(ipp_opt_color_modes[0]); i ++) {
        const ipp_opt_mode *mode = &ipp_opt_color_modes[i];
        bool               taken = false;

        if (!ipp_opt_supported(scanner->color_modes, scanner->n_color_modes,
                mode->keyword)) {
            continue;
        }

        /* A SANE mode several IPP keywords collapse onto is offered
         * once, and stands for the first of them
         */
        for (j = 0; j < opt->n_modes; j ++) {
            if (strcmp(opt->modes[j].sane, mode->sane) == 0) {
                taken = true;
                break;
            }
        }

        if (!taken) {
            opt->modes[opt->n_modes ++] = *mode;
        }
    }

    for (i = 0; i < opt->n_modes; i ++) {
        opt->mode_names[i] = opt->modes[i].sane;
    }

    /* Colour unless the device cannot, which is what a scan is
     * normally wanted in
     */
    for (i = 0; i < opt->n_modes; i ++) {
        if (strcmp(opt->modes[i].sane, SANE_VALUE_SCAN_MODE_COLOR) == 0) {
            opt->mode = i;
            break;
        }
    }
}

/******************** Sources ********************/
/* The IPP keywords for scanning both sides of a sheet, best first.
 *
 * Long edge is the way a document is normally bound, so it is the one
 * assumed when a device supports both.
 */
static const char * const ipp_opt_two_sided[] = {
    "two-sided-long-edge",
    "two-sided-short-edge"
};

/* Work out the sources the device offers.
 *
 * SANE folds duplex into the source, so an ADF that can do both sides
 * appears twice, once each way.
 */
static void
ipp_opt_sources_build (ipp_opt *opt, const ipp_scanner *scanner)
{
    const char *simplex, *duplex;
    size_t     i;

    /* A service that names no sides at all is asked for none, rather
     * than told a keyword it never claimed to understand
     */
    simplex = ipp_opt_supported(scanner->sides, scanner->n_sides,
            "one-sided") ? "one-sided" : NULL;

    duplex = ipp_opt_first_supported(scanner->sides, scanner->n_sides,
            ipp_opt_two_sided, sizeof(ipp_opt_two_sided) /
                    sizeof(ipp_opt_two_sided[0]));

    if (ipp_opt_supported(scanner->sources, scanner->n_sources, "platen")) {
        ipp_opt_src *src = &opt->sources[opt->n_sources ++];

        src->sane = "Flatbed";
        src->source = "platen";
        src->sides = simplex;
    }

    if (ipp_opt_supported(scanner->sources, scanner->n_sources, "adf")) {
        ipp_opt_src *src = &opt->sources[opt->n_sources ++];

        src->sane = "ADF";
        src->source = "adf";
        src->sides = simplex;

        if (duplex != NULL) {
            src = &opt->sources[opt->n_sources ++];

            src->sane = "ADF Duplex";
            src->source = "adf";
            src->sides = duplex;
        }
    }

    if (ipp_opt_supported(scanner->sources, scanner->n_sources,
            "film-reader")) {
        ipp_opt_src *src = &opt->sources[opt->n_sources ++];

        src->sane = "Film";
        src->source = "film-reader";
        src->sides = simplex;
    }

    for (i = 0; i < opt->n_sources; i ++) {
        opt->source_names[i] = opt->sources[i].sane;
    }

    /* The first is the platen when there is one, which is where a
     * single sheet is scanned from
     */
    opt->source = 0;
}

/******************** Resolutions ********************/
/* Order two resolutions, ascending
 */
static int
ipp_opt_dpi_cmp (const void *p1, const void *p2)
{
    SANE_Word w1 = *(const SANE_Word*) p1;
    SANE_Word w2 = *(const SANE_Word*) p2;

    return (w1 > w2) - (w1 < w2);
}

/* Work out the resolutions the device offers.
 *
 * Only square resolutions expressed in dots per inch are taken. SANE
 * has a single resolution here, so a device offering, say, 300x600 has
 * nothing this option could be set to that would mean it, and a
 * resolution per centimetre is not a whole number of dots per inch.
 * Either is rare, and a device reporting one reports ordinary
 * resolutions as well.
 *
 * Returns false if out of memory.
 */
static bool
ipp_opt_resolutions_build (ipp_opt *opt, const ipp_scanner *scanner)
{
    SANE_Word *list;
    size_t    i, count = 0;
    SANE_Word best = 0;

    if (scanner->n_resolutions == 0) {
        return true;
    }

    list = calloc(scanner->n_resolutions + 1, sizeof(*list));
    if (list == NULL) {
        return false;
    }

    for (i = 0; i < scanner->n_resolutions; i ++) {
        const ipp_resolution *res = &scanner->resolutions[i];
        size_t               j;
        bool                 seen = false;

        if (res->units != IPP_RESOLUTION_PER_INCH || res->x != res->y ||
            res->x <= 0) {
            continue;
        }

        for (j = 1; j <= count; j ++) {
            if (list[j] == (SANE_Word) res->x) {
                seen = true;
                break;
            }
        }

        if (!seen) {
            list[++ count] = (SANE_Word) res->x;
        }
    }

    if (count == 0) {
        free(list);
        return true;
    }

    list[0] = (SANE_Word) count;
    qsort(list + 1, count, sizeof(*list), ipp_opt_dpi_cmp);

    opt->resolutions = list;

    /* The offered resolution closest to the usual one. Ties go to the
     * lower, which is the cheaper scan of the two.
     */
    for (i = 1; i <= count; i ++) {
        if (best == 0 || labs((long) list[i] - IPP_OPT_DEFAULT_DPI) <
                         labs((long) best - IPP_OPT_DEFAULT_DPI)) {
            best = list[i];
            opt->resolution = i;
        }
    }

    return true;
}

/******************** Scan area ********************/
/* Hundredths of a millimetre, as a SANE fixed-point millimetre value
 */
static SANE_Word
ipp_opt_hmm_to_fixed (int hmm)
{
    return SANE_FIX((double) hmm / 100.0);
}

/* A SANE fixed-point millimetre value, as hundredths of a millimetre
 */
static int
ipp_opt_fixed_to_hmm (SANE_Word w)
{
    double hmm = SANE_UNFIX(w) * 100.0;

    return (int) (hmm < 0 ? hmm - 0.5 : hmm + 0.5);
}

/* Work out the largest area the device can scan, and how far into it
 * a scan may start.
 *
 * The scan regions are what the standard has for this, so they are used
 * when the device reports them. A device that does not still names the
 * media it takes, and a PWG media name spells out its size, so the
 * largest of those is the next best thing. The origin can then be
 * anywhere on it.
 */
static void
ipp_opt_area_build (ipp_opt *opt, const ipp_scanner *scanner)
{
    int    width = 0, height = 0, x_origin = 0, y_origin = 0;
    size_t i;

    for (i = 0; i < scanner->n_regions; i ++) {
        const ipp_scan_region *reg = &scanner->regions[i];

        if (reg->x_dimension.upper > width) {
            width = reg->x_dimension.upper;
        }
        if (reg->y_dimension.upper > height) {
            height = reg->y_dimension.upper;
        }
        if (reg->x_origin.upper > x_origin) {
            x_origin = reg->x_origin.upper;
        }
        if (reg->y_origin.upper > y_origin) {
            y_origin = reg->y_origin.upper;
        }
    }

    if (width <= 0 || height <= 0) {
        width = height = 0;

        for (i = 0; i < scanner->n_media; i ++) {
            pwg_media_t *media = pwgMediaForPWG(scanner->media[i]);

            /* libcups sizes are in 2540ths of an inch, which is
             * hundredths of a millimetre
             */
            if (media != NULL) {
                if (media->width > width) {
                    width = media->width;
                }
                if (media->length > height) {
                    height = media->length;
                }
            }
        }

        x_origin = width;
        y_origin = height;
    }

    if (width <= 0 || height <= 0) {
        return;
    }

    /* A scan cannot start beyond the far edge of the area
     */
    if (x_origin > width) {
        x_origin = width;
    }
    if (y_origin > height) {
        y_origin = height;
    }

    opt->has_area = true;

    opt->x_origin_range.min = 0;
    opt->x_origin_range.max = ipp_opt_hmm_to_fixed(x_origin);
    opt->y_origin_range.min = 0;
    opt->y_origin_range.max = ipp_opt_hmm_to_fixed(y_origin);
    opt->x_range.min = 0;
    opt->x_range.max = ipp_opt_hmm_to_fixed(width);
    opt->y_range.min = 0;
    opt->y_range.max = ipp_opt_hmm_to_fixed(height);

    /* The whole area, which is what a frontend that sets nothing
     * expects to get
     */
    opt->tl_x = 0;
    opt->tl_y = 0;
    opt->br_x = width;
    opt->br_y = height;
}

/* The corner an option sets, in hundredths of a millimetre
 */
static int*
ipp_opt_area_value (ipp_opt *opt, SANE_Int n)
{
    switch (n) {
    case IPP_OPT_TL_X: return &opt->tl_x;
    case IPP_OPT_TL_Y: return &opt->tl_y;
    case IPP_OPT_BR_X: return &opt->br_x;
    case IPP_OPT_BR_Y: return &opt->br_y;
    }

    return NULL;
}

/******************** Descriptors ********************/
/* Length of the longest string in a NULL-terminated list, including
 * the terminator, which is the buffer size SANE asks a frontend for
 */
static SANE_Int
ipp_opt_str_list_size (const SANE_String_Const *list)
{
    size_t max = 0;
    size_t i;

    for (i = 0; list[i] != NULL; i ++) {
        size_t len = strlen(list[i]);

        if (len > max) {
            max = len;
        }
    }

    return (SANE_Int) (max + 1);
}

/* Fill in the descriptor of a choice of strings, or mark it inactive if
 * the device offered no choice
 */
static void
ipp_opt_desc_str_list (SANE_Option_Descriptor *desc,
        const SANE_String_Const *list, size_t count)
{
    desc->type = SANE_TYPE_STRING;
    desc->unit = SANE_UNIT_NONE;

    if (count == 0) {
        desc->size = 1;
        desc->cap = SANE_CAP_SOFT_DETECT | SANE_CAP_INACTIVE;
        desc->constraint_type = SANE_CONSTRAINT_NONE;
        return;
    }

    desc->size = ipp_opt_str_list_size(list);
    desc->cap = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;
    desc->constraint_type = SANE_CONSTRAINT_STRING_LIST;
    desc->constraint.string_list = list;
}

/* Fill in the descriptor of a scan area corner
 */
static void
ipp_opt_desc_area (const ipp_opt *opt, SANE_Option_Descriptor *desc,
        const SANE_Range *range)
{
    desc->type = SANE_TYPE_FIXED;
    desc->unit = SANE_UNIT_MM;
    desc->size = sizeof(SANE_Word);

    if (opt->has_area) {
        desc->cap = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;
        desc->constraint_type = SANE_CONSTRAINT_RANGE;
        desc->constraint.range = range;
    } else {
        desc->cap = SANE_CAP_SOFT_DETECT | SANE_CAP_INACTIVE;
        desc->constraint_type = SANE_CONSTRAINT_NONE;
    }
}

/* Fill in all the descriptors
 */
static void
ipp_opt_desc_build (ipp_opt *opt)
{
    SANE_Option_Descriptor *desc;

    desc = &opt->desc[IPP_OPT_NUM_OPTIONS];
    desc->name = SANE_NAME_NUM_OPTIONS;
    desc->title = SANE_TITLE_NUM_OPTIONS;
    desc->desc = SANE_DESC_NUM_OPTIONS;
    desc->type = SANE_TYPE_INT;
    desc->unit = SANE_UNIT_NONE;
    desc->size = sizeof(SANE_Word);
    desc->cap = SANE_CAP_SOFT_DETECT;
    desc->constraint_type = SANE_CONSTRAINT_NONE;

    desc = &opt->desc[IPP_OPT_GROUP_STANDARD];
    desc->title = SANE_I18N("Scan Mode");
    desc->type = SANE_TYPE_GROUP;
    desc->cap = 0;
    desc->constraint_type = SANE_CONSTRAINT_NONE;

    desc = &opt->desc[IPP_OPT_MODE];
    desc->name = SANE_NAME_SCAN_MODE;
    desc->title = SANE_TITLE_SCAN_MODE;
    desc->desc = SANE_DESC_SCAN_MODE;
    ipp_opt_desc_str_list(desc, opt->mode_names, opt->n_modes);

    desc = &opt->desc[IPP_OPT_RESOLUTION];
    desc->name = SANE_NAME_SCAN_RESOLUTION;
    desc->title = SANE_TITLE_SCAN_RESOLUTION;
    desc->desc = SANE_DESC_SCAN_RESOLUTION;
    desc->type = SANE_TYPE_INT;
    desc->unit = SANE_UNIT_DPI;
    desc->size = sizeof(SANE_Word);

    if (opt->resolutions != NULL) {
        desc->cap = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;
        desc->constraint_type = SANE_CONSTRAINT_WORD_LIST;
        desc->constraint.word_list = opt->resolutions;
    } else {
        desc->cap = SANE_CAP_SOFT_DETECT | SANE_CAP_INACTIVE;
        desc->constraint_type = SANE_CONSTRAINT_NONE;
    }

    desc = &opt->desc[IPP_OPT_SOURCE];
    desc->name = SANE_NAME_SCAN_SOURCE;
    desc->title = SANE_TITLE_SCAN_SOURCE;
    desc->desc = SANE_DESC_SCAN_SOURCE;
    ipp_opt_desc_str_list(desc, opt->source_names, opt->n_sources);

    desc = &opt->desc[IPP_OPT_GROUP_GEOMETRY];
    desc->title = SANE_I18N("Geometry");
    desc->type = SANE_TYPE_GROUP;
    desc->cap = 0;
    desc->constraint_type = SANE_CONSTRAINT_NONE;

    desc = &opt->desc[IPP_OPT_TL_X];
    desc->name = SANE_NAME_SCAN_TL_X;
    desc->title = SANE_TITLE_SCAN_TL_X;
    desc->desc = SANE_DESC_SCAN_TL_X;
    ipp_opt_desc_area(opt, desc, &opt->x_origin_range);

    desc = &opt->desc[IPP_OPT_TL_Y];
    desc->name = SANE_NAME_SCAN_TL_Y;
    desc->title = SANE_TITLE_SCAN_TL_Y;
    desc->desc = SANE_DESC_SCAN_TL_Y;
    ipp_opt_desc_area(opt, desc, &opt->y_origin_range);

    desc = &opt->desc[IPP_OPT_BR_X];
    desc->name = SANE_NAME_SCAN_BR_X;
    desc->title = SANE_TITLE_SCAN_BR_X;
    desc->desc = SANE_DESC_SCAN_BR_X;
    ipp_opt_desc_area(opt, desc, &opt->x_range);

    desc = &opt->desc[IPP_OPT_BR_Y];
    desc->name = SANE_NAME_SCAN_BR_Y;
    desc->title = SANE_TITLE_SCAN_BR_Y;
    desc->desc = SANE_DESC_SCAN_BR_Y;
    ipp_opt_desc_area(opt, desc, &opt->y_range);
}

/******************** API ********************/
/* Build the options of a device from its scan capabilities
 */
ipp_opt*
ipp_opt_new (const ipp_scanner *scanner)
{
    ipp_opt *opt;

    if (scanner == NULL) {
        return NULL;
    }

    opt = calloc(1, sizeof(*opt));
    if (opt == NULL) {
        return NULL;
    }

    ipp_opt_modes_build(opt, scanner);
    ipp_opt_sources_build(opt, scanner);

    if (!ipp_opt_resolutions_build(opt, scanner)) {
        free(opt);
        return NULL;
    }

    ipp_opt_area_build(opt, scanner);
    ipp_opt_desc_build(opt);

    return opt;
}

/* Release the options
 */
void
ipp_opt_free (ipp_opt *opt)
{
    if (opt != NULL) {
        free(opt->resolutions);
        free(opt);
    }
}

/* Return the descriptor of an option
 */
const SANE_Option_Descriptor*
ipp_opt_descriptor (const ipp_opt *opt, SANE_Int n)
{
    if (opt == NULL || n < 0 || n >= NUM_IPP_OPT) {
        return NULL;
    }

    return &opt->desc[n];
}

/* Read the current value of an option
 */
SANE_Status
ipp_opt_get (const ipp_opt *opt, SANE_Int n, void *value)
{
    const SANE_Option_Descriptor *desc = ipp_opt_descriptor(opt, n);

    if (desc == NULL || value == NULL) {
        return SANE_STATUS_INVAL;
    }

    if ((desc->cap & SANE_CAP_INACTIVE) != 0) {
        return SANE_STATUS_INVAL;
    }

    switch (n) {
    case IPP_OPT_NUM_OPTIONS:
        *(SANE_Word*) value = NUM_IPP_OPT;
        return SANE_STATUS_GOOD;

    case IPP_OPT_MODE:
        strcpy(value, opt->modes[opt->mode].sane);
        return SANE_STATUS_GOOD;

    case IPP_OPT_SOURCE:
        strcpy(value, opt->sources[opt->source].sane);
        return SANE_STATUS_GOOD;

    case IPP_OPT_RESOLUTION:
        *(SANE_Word*) value = opt->resolutions[opt->resolution];
        return SANE_STATUS_GOOD;

    case IPP_OPT_TL_X:
    case IPP_OPT_TL_Y:
    case IPP_OPT_BR_X:
    case IPP_OPT_BR_Y:
        *(SANE_Word*) value =
            ipp_opt_hmm_to_fixed(*ipp_opt_area_value((ipp_opt*) opt, n));
        return SANE_STATUS_GOOD;
    }

    /* A group has no value
     */
    return SANE_STATUS_INVAL;
}

/* Find a string in a list of choices. Returns the count if absent.
 */
static size_t
ipp_opt_str_index (const SANE_String_Const *list, size_t count,
        const char *s)
{
    size_t i;

    for (i = 0; i < count; i ++) {
        if (strcasecmp(list[i], s) == 0) {
            break;
        }
    }

    return i;
}

/* Set an option
 */
SANE_Status
ipp_opt_set (ipp_opt *opt, SANE_Int n, void *value, SANE_Int *info)
{
    const SANE_Option_Descriptor *desc = ipp_opt_descriptor(opt, n);
    size_t                       i;

    if (desc == NULL || value == NULL) {
        return SANE_STATUS_INVAL;
    }

    if ((desc->cap & SANE_CAP_SOFT_SELECT) == 0) {
        return SANE_STATUS_INVAL;
    }

    switch (n) {
    case IPP_OPT_MODE:
        i = ipp_opt_str_index(opt->mode_names, opt->n_modes, value);
        if (i == opt->n_modes) {
            return SANE_STATUS_INVAL;
        }

        opt->mode = i;
        break;

    case IPP_OPT_SOURCE:
        i = ipp_opt_str_index(opt->source_names, opt->n_sources, value);
        if (i == opt->n_sources) {
            return SANE_STATUS_INVAL;
        }

        opt->source = i;
        break;

    case IPP_OPT_RESOLUTION: {
        SANE_Word want = *(SANE_Word*) value;
        size_t    best = 1;
        size_t    count = (size_t) opt->resolutions[0];

        for (i = 1; i <= count; i ++) {
            if (labs((long) opt->resolutions[i] - want) <
                labs((long) opt->resolutions[best] - want)) {
                best = i;
            }
        }

        opt->resolution = best;

        if (opt->resolutions[best] != want) {
            /* The frontend is told what it actually got, as the SANE
             * standard requires of an inexact value
             */
            *(SANE_Word*) value = opt->resolutions[best];

            if (info != NULL) {
                *info |= SANE_INFO_INEXACT;
            }
        }
        break;
    }

    case IPP_OPT_TL_X:
    case IPP_OPT_TL_Y:
    case IPP_OPT_BR_X:
    case IPP_OPT_BR_Y: {
        SANE_Word want = *(SANE_Word*) value;
        SANE_Word got = want;

        if (got < desc->constraint.range->min) {
            got = desc->constraint.range->min;
        } else if (got > desc->constraint.range->max) {
            got = desc->constraint.range->max;
        }

        *ipp_opt_area_value(opt, n) = ipp_opt_fixed_to_hmm(got);

        if (got != want) {
            *(SANE_Word*) value = got;

            if (info != NULL) {
                *info |= SANE_INFO_INEXACT;
            }
        }
        break;
    }

    default:
        return SANE_STATUS_INVAL;
    }

    if (info != NULL) {
        /* Any of these changes the size or the depth of the image the
         * next scan will produce
         */
        *info |= SANE_INFO_RELOAD_PARAMS;
    }

    return SANE_STATUS_GOOD;
}

/******************** Current settings, in IPP terms ********************/
/* The input-color-mode keyword the current mode amounts to
 */
const char*
ipp_opt_color_mode (const ipp_opt *opt)
{
    if (opt == NULL || opt->n_modes == 0) {
        return NULL;
    }

    return opt->modes[opt->mode].keyword;
}

/* The input-source keyword the current source amounts to
 */
const char*
ipp_opt_source (const ipp_opt *opt)
{
    if (opt == NULL || opt->n_sources == 0) {
        return NULL;
    }

    return opt->sources[opt->source].source;
}

/* The input-sides keyword the current source amounts to
 */
const char*
ipp_opt_sides (const ipp_opt *opt)
{
    if (opt == NULL || opt->n_sources == 0) {
        return NULL;
    }

    return opt->sources[opt->source].sides;
}

/* The current resolution, in dots per inch, or 0 if the device offered
 * no choice
 */
int
ipp_opt_resolution (const ipp_opt *opt)
{
    if (opt == NULL || opt->resolutions == NULL) {
        return 0;
    }

    return (int) opt->resolutions[opt->resolution];
}

/* The scan area the current settings amount to
 */
bool
ipp_opt_scan_region (const ipp_opt *opt, int *x_origin, int *y_origin,
        int *x_dimension, int *y_dimension)
{
    if (opt == NULL || !opt->has_area) {
        return false;
    }

    *x_origin = opt->tl_x < opt->br_x ? opt->tl_x : opt->br_x;
    *y_origin = opt->tl_y < opt->br_y ? opt->tl_y : opt->br_y;
    *x_dimension = abs(opt->br_x - opt->tl_x);
    *y_dimension = abs(opt->br_y - opt->tl_y);

    return true;
}
