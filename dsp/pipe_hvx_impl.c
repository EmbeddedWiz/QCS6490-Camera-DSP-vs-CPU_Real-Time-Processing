/*
 * pipe_hvx_impl.c - the DSP end of the IDL.
 *
 * This is the whole frame. The camera's Bayer data is read straight out of the
 * DMA-BUF the host registered, and the result is written straight into the
 * surface the host handed over. Nothing is copied, and no host code runs in
 * between.
 *
 * The order of the stages is the same as in the host reference: demosaic with
 * the window scale folded in, then edge, emboss, posterize, invert. Switching
 * engines gives the same picture, not just a similar one. The filters work on
 * the scaled frame, so a 1:6 window is also 36 times cheaper to filter.
 */
#include "pipe_hvx.h"

#include "kernel.h"

/* The IDL passes the frame as a sequence, so the DSP sees a length as well as
   a pointer.  A wrong length means the host and the kernel disagree about the
   geometry, and writing anyway would run off the surface. Refusing is better.
 */
static int geometry_ok(const geom* g, int raw_len, int rgba_len)
{
    if (g->width < PIPE_LANES || g->width > PIPE_MAX_W) {
        return 0;
    }
    if (g->height < 2u) {
        return 0;
    }
    if (g->scale < 1u || g->scale > 6u) {
        return 0;
    }
    /* The output width is width/scale rounded down to a whole vector; the
       arithmetic itself has no preference for a scale that divides the width,
       so raising the limit above is the only thing a 1:7 (or a prime) window
       would need. */
    if (out_width(g->width, g->scale) < PIPE_LANES) {
        return 0;
    }
    if (g->stride < g->width) {
        return 0;
    }
    if ((uint64_t)raw_len < (uint64_t)g->stride * g->height) {
        return 0;
    }
    const uint64_t out_lanes = out_width(g->width, g->scale);
    const uint64_t out_rows = g->height / g->scale;
    if ((uint64_t)rgba_len < out_lanes * out_rows * 4u) {
        return 0;
    }
    return 1;
}

int pipe_hvx_open(const char* uri, remote_handle64* handle)
{
    (void)uri;
    agc_reset();
    *handle = (remote_handle64)1;
    return 0;
}

int pipe_hvx_close(remote_handle64 handle)
{
    (void)handle;
    return 0;
}

int pipe_hvx_render(remote_handle64 handle, const uint8_t* raw, int raw_len,
                    uint32_t stride, uint32_t width, uint32_t height,
                    uint32_t scale, uint32_t filters, uint32_t strength,
                    uint32_t flags, uint8_t* rgba, int rgba_len)
{
    (void)handle;

    geom g = {width, height, stride, scale};
    if (!geometry_ok(&g, raw_len, rgba_len)) {
        return -1;
    }

    const uint32_t raw8 = flags & FLAG_RAW8;
    const uint32_t agc = flags & FLAG_AGC;
    /* The gain is part of the picture, not part of the adaptation: with the
       loop off the last value it settled on stays applied, so turning AGC off
       freezes the exposure instead of darkening the frame back to unity. */
    const uint32_t shift = agc_shift();

    const uint32_t luma = demosaic_frame(raw, &g, raw8, shift, rgba);
    /* The surface rows are a whole number of vectors wide and the visible
       width is that same number, so every filter runs over exactly the lanes
       the demosaic wrote. */
    const uint32_t out_w = out_width(width, scale);
    const uint32_t out_h = height / scale;

    if ((filters & FX_SHARPEN) != 0u) {
        filter_sharpen(rgba, out_w, out_h, strength);
    }
    if ((filters & FX_PIXELATE) != 0u) {
        filter_pixelate(rgba, out_w, out_h, strength);
    }
    if ((filters & FX_EDGE) != 0u) {
        filter_edge(rgba, out_w, out_h, strength);
    }
    if ((filters & FX_SKETCH) != 0u) {
        filter_sketch(rgba, out_w, out_h, strength);
    }
    if ((filters & FX_EMBOSS) != 0u) {
        filter_emboss(rgba, out_w, out_h, strength);
    }
    if ((filters & FX_SEPIA) != 0u) {
        filter_sepia(rgba, out_w, out_h, strength);
    }
    if ((filters & FX_POSTERIZE) != 0u) {
        filter_posterize(rgba, out_w, out_h, strength);
    }
    if ((filters & FX_INVERT) != 0u) {
        filter_invert(rgba, out_w, out_h);
    }

    if (agc != 0u) {
        agc_update(luma);
    }
    return 0;
}

/* The panel asks for this a few times a second, never per frame, so the extra
   round trip costs the frame path nothing. */
int pipe_hvx_state(remote_handle64 handle, uint32_t* shift, uint32_t* luma)
{
    (void)handle;
    *shift = agc_shift();
    *luma = agc_luma();
    return 0;
}
