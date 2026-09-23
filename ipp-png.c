/* sane-ipp -- IPP backend for SANE
 *
 * Copyright (C) 2026 Alexander Pevzner (pzz@apevzner.com)
 * Copyright (C) 2026 Yogesh Singla (yogeshsingla481@gmail.com)
 * SPDX-License-Identifier: BSD-2-Clause
 * See LICENSE for license terms and conditions
 *
 * PNG decoding
 *
 * PNG is decoded through libpng's progressive reader.
 */

#include "ipp-png.h"

#include <png.h>

#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/******************** Constants ********************/
/* The MIME type of the only format decoded so far
 */
#define IPP_PNG_MIME_TYPE    "image/png"

/******************** Local Types ********************/
/* An image being decoded.
 *
 * The decoded rows are queued here rather than handed to the caller as
 * they appear, because libpng emits them from inside a callback, at a
 * moment of its choosing, while the caller takes them at a moment of
 * its own.
 */
struct ipp_png {
    png_struct *png;            /* Underlying libpng reader */
    png_info   *info;           /* Its info structure */

    char       error[512];      /* Message from the error callback */
    bool       failed;          /* Decoding has failed, error is set */

    bool       have_params;     /* The header has been parsed */
    bool       ended;           /* libpng reported the end of the image */

    int        width;           /* Image size, in pixels */
    int        height;
    int        depth;           /* Bits per channel, 8 or 16 */
    int        channels;        /* 1 for grey, 3 for colour */
    size_t     bytes_per_line;  /* Size of a decoded row */
    int        rows;            /* Rows decoded so far */

    uint8_t    *queue;          /* Decoded rows not yet taken */
    size_t     queue_cap;
    size_t     queue_len;       /* Bytes held */
    size_t     queue_pos;       /* Bytes already taken */
};

/******************** Small helpers ********************/
/* Format an error message into the caller's buffer. Tolerates a NULL
 * buffer, so callers that do not want the message need no special case.
 */
static void
ipp_png_err (char *errbuf, size_t errlen, const char *fmt, ...)
    __attribute__ ((format (printf, 3, 4)));

static void
ipp_png_err (char *errbuf, size_t errlen, const char *fmt, ...)
{
    va_list ap;

    if (errbuf == NULL || errlen == 0) {
        return;
    }

    va_start(ap, fmt);
    vsnprintf(errbuf, errlen, fmt, ap);
    va_end(ap);
}

static int
ipp_png_fail (ipp_png *image, char *errbuf, size_t errlen)
{
    ipp_png_err(errbuf, errlen, "%s", image->error[0] != '\0'
            ? image->error
            : "PNG: decoding failed");

    return -1;
}

/******************** Row queue ********************/
/* Append a decoded row to the queue.
 */
static void
ipp_png_queue_add (ipp_png *image, const uint8_t *row, size_t size)
{
    if (image->queue_pos == image->queue_len) {
        image->queue_pos = image->queue_len = 0;
    }

    if (image->queue_len + size > image->queue_cap) {
        size_t cap = image->queue_cap != 0 ? image->queue_cap : size;

        while (cap < image->queue_len + size) {
            cap *= 2;
        }

        image->queue = realloc(image->queue, cap);
        if (image->queue == NULL) {
            perror("ipp-png: realloc");
            abort();
        }

        image->queue_cap = cap;
    }

    memcpy(image->queue + image->queue_len, row, size);
    image->queue_len += size;
}

/******************** libpng callbacks ********************/
/* Keep the message of a libpng error and jump out.
 *
 * The jump is made here rather than left to libpng, whose own handler
 * prints the message to stderr before jumping. A backend shares its
 * error output with the frontend that loaded it, and a scanned page
 * that libpng dislikes is no reason to write on it. This function does
 * not return.
 */
static void
ipp_png_error_callback (png_struct *png, const char *message)
{
    ipp_png *image = png_get_error_ptr(png);

    snprintf(image->error, sizeof(image->error), "PNG: %s", message);

    png_longjmp(png, 1);
}

/* Ignore a libpng warning.
 *
 * A warning means the image is unusual, not that it cannot be decoded,
 * and a scan is no place to report it.
 */
static void
ipp_png_warning_callback (png_struct *png, const char *message)
{
    (void) png;
    (void) message;
}

/* The header has been parsed: settle what the decoded rows will look
 * like.
 *
 * The transformations asked for here are what turns the variety PNG
 * allows into the two layouts SANE has: 8 or 16 bits per channel, one
 * channel or three, and nothing else.
 */
static void
ipp_png_info_callback (png_struct *png, png_info *info)
{
    ipp_png     *image = png_get_progressive_ptr(png);
    png_uint_32 width, height;
    int         depth, color_type, interlace;

    png_get_IHDR(png, info, &width, &height, &depth, &color_type,
            &interlace, NULL, NULL);

    if (interlace != PNG_INTERLACE_NONE) {
        png_error(png, "interlaced images are not supported");
    }

    if (color_type == PNG_COLOR_TYPE_PALETTE) {
        png_set_palette_to_rgb(png);
    }

    if (color_type == PNG_COLOR_TYPE_GRAY && depth < 8) {
        png_set_expand_gray_1_2_4_to_8(png);
    }

    if ((color_type & PNG_COLOR_MASK_ALPHA) != 0 ||
        png_get_valid(png, info, PNG_INFO_tRNS)) {
        png_set_strip_alpha(png);
    }

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    if (depth == 16) {
        png_set_swap(png);
    }
#endif

    png_read_update_info(png, info);

    image->width = (int) width;
    image->height = (int) height;
    image->depth = png_get_bit_depth(png, info);
    image->channels = png_get_channels(png, info);
    image->bytes_per_line = png_get_rowbytes(png, info);
    image->have_params = true;
}

/* A row has been decoded: put it in the queue.
 *
 * row_num and pass are what an interlaced image would need to place the
 * row, and those are refused above, so the rows arrive here in order
 * and each one only once.
 */
static void
ipp_png_row_callback (png_struct *png, png_byte *row, png_uint_32 row_num,
        int pass)
{
    ipp_png *image = png_get_progressive_ptr(png);

    (void) row_num;
    (void) pass;

    if (row != NULL) {
        ipp_png_queue_add(image, row, image->bytes_per_line);
        image->rows ++;
    }
}

/* The image has ended
 */
static void
ipp_png_end_callback (png_struct *png, png_info *info)
{
    ipp_png *image = png_get_progressive_ptr(png);

    (void) info;

    image->ended = true;
}

/******************** Decoding ********************/
/* Report whether a MIME type can be decoded
 */
bool
ipp_png_format_supported (const char *format)
{
    return format != NULL && strcmp(format, IPP_PNG_MIME_TYPE) == 0;
}

/* This function creates a new decoder for png format.
 * returns NULL if the format is not supported.
 */
ipp_png*
ipp_png_new (const char *format, char *errbuf, size_t errlen)
{
    ipp_png *image;

    if (errbuf != NULL && errlen != 0) {
        errbuf[0] = '\0';
    }

    if (!ipp_png_format_supported(format)) {
        ipp_png_err(errbuf, errlen, "%s: unsupported image format",
                format != NULL ? format : "(none)");
        return NULL;
    }

    image = calloc(1, sizeof(*image));
    if (image == NULL) {
        ipp_png_err(errbuf, errlen, "PNG: out of memory");
        return NULL;
    }

    image->png = png_create_read_struct(PNG_LIBPNG_VER_STRING, image,
            ipp_png_error_callback, ipp_png_warning_callback);

    if (image->png == NULL) {
        ipp_png_err(errbuf, errlen, "PNG: cannot create reader");
        free(image);
        return NULL;
    }

    image->info = png_create_info_struct(image->png);
    if (image->info == NULL) {
        ipp_png_err(errbuf, errlen, "PNG: cannot create reader");
        png_destroy_read_struct(&image->png, NULL, NULL);
        free(image);
        return NULL;
    }

    png_set_progressive_read_fn(image->png, image, ipp_png_info_callback,
            ipp_png_row_callback, ipp_png_end_callback);

    return image;
}

/* Push encoded bytes into the decoder
 */
int
ipp_png_write (ipp_png *image, const void *data, size_t size,
        char *errbuf, size_t errlen)
{
    if (errbuf != NULL && errlen != 0) {
        errbuf[0] = '\0';
    }

    if (image->failed) {
        return ipp_png_fail(image, errbuf, errlen);
    }

    if (image->ended || size == 0) {
        return 0;
    }

    if (setjmp(png_jmpbuf(image->png))) {
        image->failed = true;
        return ipp_png_fail(image, errbuf, errlen);
    }

    png_process_data(image->png, image->info, (png_byte*) data, size);

    return 0;
}

/* Report the end of the encoded data
 */
int
ipp_png_finish (ipp_png *image, char *errbuf, size_t errlen)
{
    if (errbuf != NULL && errlen != 0) {
        errbuf[0] = '\0';
    }

    if (image->failed) {
        return ipp_png_fail(image, errbuf, errlen);
    }

    /* libpng reports the end of an image from its own end callback, so
     * data that stops before that is data that was cut short, however
     * much of it arrived.
     */
    if (!image->ended) {
        if (image->have_params) {
            ipp_png_err(errbuf, errlen,
                    "PNG: image ended after %d of %d rows",
                    image->rows, image->height);
        } else {
            ipp_png_err(errbuf, errlen,
                    "PNG: image ended before its header");
        }

        image->failed = true;
        return -1;
    }

    return 0;
}

/* Report whether the header has arrived
 */
bool
ipp_png_have_params (const ipp_png *image)
{
    return image->have_params;
}

/* Report what the decoded image looks like
 */
void
ipp_png_params (const ipp_png *image, int *width, int *height,
        int *depth, int *channels)
{
    if (width != NULL) {
        *width = image->width;
    }

    if (height != NULL) {
        *height = image->height;
    }

    if (depth != NULL) {
        *depth = image->depth;
    }

    if (channels != NULL) {
        *channels = image->channels;
    }
}

/* Report the size of a decoded row
 */
size_t
ipp_png_bytes_per_line (const ipp_png *image)
{
    return image->bytes_per_line;
}

/* Take decoded pixels out of the decoder
 */
size_t
ipp_png_read (ipp_png *image, void *data, size_t size)
{
    size_t have = image->queue_len - image->queue_pos;

    if (have == 0) {
        return 0;
    }

    if (size > have) {
        size = have;
    }

    memcpy(data, image->queue + image->queue_pos, size);
    image->queue_pos += size;

    return size;
}

/* Report whether everything has been decoded and taken
 */
bool
ipp_png_eof (const ipp_png *image)
{
    return image->ended && image->queue_pos == image->queue_len;
}

/* Release the decoder
 */
void
ipp_png_free (ipp_png *image)
{
    if (image == NULL) {
        return;
    }

    png_destroy_read_struct(&image->png, &image->info, NULL);

    free(image->queue);
    free(image);
}
