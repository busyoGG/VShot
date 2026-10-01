// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

// The half-float picture surface the pin helper draws on: EGL and OpenGL on the
// render node, a dma-buf per buffer, and the few draws a pin is made of.
//
// Why half-float at all: a pinned HDR image is PQ-encoded light, and the rim
// and the shadow that belong with it need the *same* buffer -- one surface, one
// commit, so the picture can never trail its own edge.  A ten-bit `wl_shm`
// buffer cannot carry that: its alpha is two bits.  `ABGR16161616F` carries ten
// bits of colour and sixteen of alpha, and the compositor offers it on every
// NVIDIA modifier its feedback lists.
//
// Everything here is best-effort: `vshot_fp16_open` answers NULL when the
// machine has no EGL, no GBM or no render node, and the caller keeps its
// software path.  The API is deliberately coarse -- upload a picture, upload a
// mask, draw, present -- because the bookkeeping of which pin is where belongs
// to the caller, and the look of a shadow belongs here, next to the shaders
// that draw it.

#ifndef VSHOT_PIN_HDR_FP16_H
#define VSHOT_PIN_HDR_FP16_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vshot_fp16 vshot_fp16;

/// One allocated picture buffer: the dma-buf the compositor takes, and the slot
/// this side renders and presents it through.
typedef struct {
    int slot;
    /// The dma-buf.  Owned by the buffer, not by the caller: it is closed when
    /// the buffer is freed or its context is.
    int fd;
    uint32_t stride;
    uint32_t offset;
    uint64_t modifier;
    uint32_t format;
} vshot_fp16_buffer;

/// Opens `render_node` and makes a context current on it.  On failure answers
/// NULL and writes the reason into `why`.
vshot_fp16 *vshot_fp16_open(const char *render_node, char *why, size_t why_len);
void vshot_fp16_close(vshot_fp16 *ctx);

/// Allocates one picture buffer of `width` x `height` with `modifier` (one the
/// compositor offered) and `format` (`0x48344241` for ABGR16161616F).  Answers
/// 0, or -1 with the reason in `why`.
int vshot_fp16_alloc(vshot_fp16 *ctx, uint32_t width, uint32_t height, uint32_t format,
                     uint64_t modifier, vshot_fp16_buffer *out, char *why, size_t why_len);
/// Drops one buffer; its slot is free for another allocation afterwards.
void vshot_fp16_free_buffer(vshot_fp16 *ctx, vshot_fp16_buffer *buffer);

/// Uploads one pin's pixels: `words` are the packed ten-bit PQ codes the capture
/// wrote, `width` x `height` of them.  Kept under `id` until it is dropped, so a
/// drag re-uploads nothing.  Answers 0 or -1.
int vshot_fp16_image(vshot_fp16 *ctx, uint64_t id, const uint32_t *words, uint32_t width,
                     uint32_t height);
/// Builds (and keeps under `id`) the soft mask a shadow is drawn from: the shape
/// `width` x `height` with corner `radius`, grown by `spread` on every side,
/// dropped by `offset`, at an alpha of `opacity` out of 255, then blurred the way
/// the SDR shadow is blurred.  Answers 0, or -1 when the shadow is off or the
/// shape is too small to carry one.
int vshot_fp16_mask(vshot_fp16 *ctx, uint64_t id, int width, int height, int radius, int spread,
                    int offset, int opacity);
/// Forgets a picture or a mask.
void vshot_fp16_drop(vshot_fp16 *ctx, uint64_t id);

/// Starts one picture: the compose texture is this size, and its damaged region
/// is cleared by the caller.
int vshot_fp16_begin(vshot_fp16 *ctx, uint32_t width, uint32_t height);
/// Clears one device-pixel region to transparent.
void vshot_fp16_clear(vshot_fp16 *ctx, int x, int y, int width, int height);
/// Draws the pins of the stack, in the order they are called.
int vshot_fp16_draw_shadow(vshot_fp16 *ctx, uint64_t mask_id, int x, int y, int width, int height);
int vshot_fp16_draw_image(vshot_fp16 *ctx, uint64_t image_id, int x, int y, int width, int height,
                          int radius);
/// Draws the stroke around a pin.  `colour` is the encoded colour the buffer
/// wants: a PQ code, three floats, since only the caller knows what one stands
/// for here.  `alpha` is the stroke's own opacity, 0..1 -- a rim the user set
/// to a transparent colour paints nothing, exactly as the Qt side's pen does.
int vshot_fp16_draw_rim(vshot_fp16 *ctx, int x, int y, int width, int height, int radius,
                        int thickness, float red, float green, float blue, float alpha);

/// Copies a rectangle of the compose texture into `slot` and leaves it ready to
/// attach.  Answers 0, or -1 when that slot has nothing in it.
int vshot_fp16_present(vshot_fp16 *ctx, int slot, int x, int y, int width, int height);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // VSHOT_PIN_HDR_FP16_H
