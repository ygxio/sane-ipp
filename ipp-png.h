/* sane-ipp -- IPP backend for SANE
 *
 * Copyright (C) 2026 Alexander Pevzner (pzz@apevzner.com)
 * Copyright (C) 2026 Yogesh Singla (yogeshsingla481@gmail.com)
 * SPDX-License-Identifier: BSD-2-Clause
 * See LICENSE for license terms and conditions
 *
 * PNG decoding
 *
 * A scan service sends an encoded image, and a SANE frontend expects
 * rows of pixels. This layer sits between the two.
 *
 * Decoding is progressive: the caller pushes in whatever encoded bytes
 * have arrived and takes out whatever pixels that produced, and neither
 * step waits for the other. A page is therefore never held in memory
 * whole, the first rows reach the frontend while the rest is still on
 * the wire, and nothing blocks inside the decoder, which is what a
 * frontend that drives the backend from its own event loop needs.
 */

#ifndef ipp_png_h
#define ipp_png_h

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* An image being decoded
 */
typedef struct ipp_png ipp_png;

/* Report whether images of this MIME type can be decoded.
 *
 * A service offers a choice of formats and the choice is made before a
 * job is created, so this has to be answerable without an image in
 * hand. The type is an IPP "document-format" value, such as
 * "image/png".
 */
bool
ipp_png_format_supported (const char *format);

/* Start decoding an image of the given MIME type.
 *
 * Nothing is known about the image yet: its size arrives with its
 * header, which is part of the data still to be pushed in.
 *
 * Returns the decoder, to be released with ipp_png_free(). On error
 * returns NULL and, when errbuf is not NULL, writes there a description
 * of what went wrong.
 */
ipp_png*
ipp_png_new (const char *format, char *errbuf, size_t errlen);

/* Push encoded bytes into the decoder.
 *
 * Returns 0, or -1 on error, writing a description to errbuf. What the
 * bytes decoded to, if anything, is then taken out with
 * ipp_png_read(); a push that completes no row is normal and is not
 * an error.
 */
int
ipp_png_write (ipp_png *image, const void *data, size_t size,
        char *errbuf, size_t errlen);

/* Tell the decoder that the encoded data has ended.
 *
 * Returns 0, or -1 if the image was cut short, which is an error the
 * caller only learns here: an image is not complete merely because the
 * connection stopped giving bytes.
 */
int
ipp_png_finish (ipp_png *image, char *errbuf, size_t errlen);

/* Report whether the header has arrived, and with it the parameters
 * below. Until it has, the size of the image is not yet known.
 */
bool
ipp_png_have_params (const ipp_png *image);

/* What the decoded image looks like.
 *
 * channels is 1 for grey and 3 for colour, depth is bits per channel.
 * Any of the pointers may be NULL. Meaningless before
 * ipp_png_have_params() is true.
 */
void
ipp_png_params (const ipp_png *image, int *width, int *height,
        int *depth, int *channels);

/* Size of one decoded row, in bytes
 */
size_t
ipp_png_bytes_per_line (const ipp_png *image);

/* Take decoded pixels out of the decoder.
 *
 * Returns how many bytes were copied, which is 0 when nothing has been
 * decoded yet. The bytes are rows of pixels, one after another, with no
 * padding between them.
 */
size_t
ipp_png_read (ipp_png *image, void *data, size_t size);

/* Report whether the whole image has been decoded and taken out
 */
bool
ipp_png_eof (const ipp_png *image);

/* Release the decoder
 */
void
ipp_png_free (ipp_png *image);

#ifdef __cplusplus
}
#endif

#endif
