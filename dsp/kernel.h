/*
 * kernel.h - the names the DSP sources share.
 *
 * The host and the DSP agree on two numbers: the filter bits and the flag
 * bits.  They are spelled the same way on both sides (src/engine.h has the
 * kEdge/kEmboss/... constants) so a change has to be made in two places and
 * the compiler will not catch a drift.  That is the price of a C kernel that
 * must not include C++ headers; the values are part of the IDL comment too.
 */
#ifndef PIPE_HVX_KERNEL_H
#define PIPE_HVX_KERNEL_H

#include <stddef.h>
#include <stdint.h>

#include "hvx.h"

/* Filter mask, identical to pl::kEdge and friends. */
#define FX_EDGE 0x100u
#define FX_EMBOSS 0x200u
#define FX_POSTERIZE 0x400u
#define FX_INVERT 0x800u
#define FX_PIXELATE 0x1000u
#define FX_SKETCH 0x2000u
#define FX_SEPIA 0x4000u
#define FX_SHARPEN 0x8000u

/* Flag bits of pipe_hvx_render. */
#define FLAG_RAW8 0x1u
#define FLAG_AGC 0x2u

typedef struct {
    uint32_t width;  /* pixels per row, at most PIPE_MAX_W */
    uint32_t height; /* rows */
    uint32_t stride; /* bytes per raw row */
    /* Window scale: 1 leaves the frame alone, N writes out_width(width, N) by
       height/N by averaging N x N blocks.  The output width is width/N rounded
       *down* to a whole vector, so the surface stride is exactly
       out_width * 4 and the last block of the box filter ends inside the
       frame.  The host derives the same number in src/engine.h. */
    uint32_t scale;
} geom;

/* demosaic.c - RAW -> RGBA (downscaled when g->scale > 1), and the green mean
   used by the AGC loop.  `out` rows are `out_width(width, scale)` pixels
   apart, i.e. the visible width rounded down to 32. */
uint32_t demosaic_frame(const uint8_t* raw, const geom* g, uint32_t raw8,
                        uint32_t shift, uint8_t* out);

/* Width of a lane row: the visible width rounded up to a whole vector. */
uint32_t lane_width(uint32_t width);

/* Visible output width for a window scale: width/scale rounded down to a whole
   vector.  Not width/scale: a row that ended in the middle of a vector would
   make the stride differ from width*4, and a sink that then takes the stride
   from its own caps instead of from the buffer shears the picture into
   diagonal stripes.  Rounding down also keeps the box filter inside the frame
   (out_width * scale <= width), so no pass reads past the raw row.  Nothing
   here assumes the scale divides the width: 2, 3, 6 and primes behave alike.
 */
uint32_t out_width(uint32_t width, uint32_t scale);

/* filters.c - every pass works in place on the RGBA frame. */
void filter_edge(uint8_t* frame, uint32_t w, uint32_t h, uint32_t strength);
void filter_emboss(uint8_t* frame, uint32_t w, uint32_t h, uint32_t strength);
void filter_posterize(uint8_t* frame, uint32_t w, uint32_t h,
                      uint32_t strength);
void filter_invert(uint8_t* frame, uint32_t w, uint32_t h);
void filter_pixelate(uint8_t* frame, uint32_t w, uint32_t h, uint32_t strength);
void filter_sketch(uint8_t* frame, uint32_t w, uint32_t h, uint32_t strength);
void filter_sepia(uint8_t* frame, uint32_t w, uint32_t h, uint32_t strength);
void filter_sharpen(uint8_t* frame, uint32_t w, uint32_t h, uint32_t strength);

/* agc.c - exposure loop, one frame of delay like every AE. */
void agc_reset(void);
uint32_t agc_shift(void);
uint32_t agc_luma(void);
void agc_update(uint32_t luma);

#endif
