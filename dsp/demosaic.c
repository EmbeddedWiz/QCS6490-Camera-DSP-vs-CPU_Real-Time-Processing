/*
 * demosaic.c - Bayer to RGBA, all on the CDSP.
 *
 * Three rows of the frame are expanded into "lanes", one 32-bit lane per
 * pixel with the 8-bit sample copied into all four bytes, and kept in a ring.
 * A row is expanded once, not once per use: demosaicing row y needs rows y-1,
 * y and y+1, and each of them is the middle of its own output row.
 *
 * Copying the sample into all four bytes is what makes the rest cheap. A u8
 * average of two lanes averages two whole pixels, and picking a channel at the
 * end is one AND: lane & 0x000000FF is R, lane & 0x0000FF00 is G.
 *
 * The window scale lives here too, because the cheapest moment to throw pixels
 * away is the row that was just demosaiced. Instead of writing every row to
 * the surface, `scale` rows are summed per channel, and every scale-th lane of
 * that sum becomes one output pixel. Nothing at full resolution is ever
 * stored, so a 1:6 window costs one pass over the raw frame, not a full RGBA
 * frame plus a second pass.
 *
 * RAW10 is unpacked here as well. Packed 10-bit keeps four pixels in five
 * bytes; this pipeline works in 8 bits and cuts the low two, so pixel k is
 * simply byte k*5/4. That row is unpacked with scalar code into aligned
 * scratch and then treated like RAW8. The sensor only sends RAW10 when asked,
 * and the live pipeline does not ask.
 *
 * The colour stages run in the same order the camera's own ISP uses them, and
 * each one fixes something visible in a reference capture:
 *
 *   black level   the sensor pedestal (16 in these eight bits) is not light.
 *                 Left in, it lifts every shadow and flattens contrast;
 *   gain          the exposure loop's digital stop, applied to light only;
 *   white balance per channel, so a neutral scene comes out neutral;
 *   window        the box average, still in linear light;
 *   gamma 2.2     the display encoding, applied once, at the end.
 *
 * Gamma is last for a reason: averaging gamma-encoded samples darkens a
 * window, and applying it before white balance would make the gains depend on
 * the scene.
 */
#include "kernel.h"
#include "gamma.h"

/* The sensor's black pedestal: libcamera reports SensorBlackLevels 4096 on its
   16-bit scale, which is 64 of the sensor's 10 bits, and this pipeline keeps
   the top eight of those, so 16 here.  Subtracting it before anything else is
   what puts black at zero: without it every shadow sits 16 levels up and the
   picture looks washed out however the gain is set. */
#define PIPE_BLACK PIPE_GAMMA_BLACK

static _Alignas(PIPE_VBYTES) uint32_t s_row[3][PIPE_LANE_ROW];
static _Alignas(PIPE_VBYTES) uint32_t s_acc[2][PIPE_LANE_ROW];
static _Alignas(PIPE_VBYTES) uint32_t s_hrow[PIPE_LANE_ROW];
static _Alignas(PIPE_VBYTES) uint8_t s_bytes[PIPE_MAX_W];
static _Alignas(PIPE_VBYTES) uint32_t s_even_lane[PIPE_LANES];
static _Alignas(PIPE_VBYTES) uint32_t s_odd_lane[PIPE_LANES];
static int s_masks;

uint32_t lane_width(uint32_t width)
{
    return (width + PIPE_LANES - 1u) & ~(PIPE_LANES - 1u);
}

uint32_t out_width(uint32_t width, uint32_t scale)
{
    return (width / scale) & ~(PIPE_LANES - 1u);
}

static void masks_init(void)
{
    if (s_masks != 0) {
        return;
    }
    for (uint32_t i = 0; i < PIPE_LANES; ++i) {
        s_even_lane[i] = ((i & 1u) == 0u) ? 0xFFFFFFFFu : 0u;
        s_odd_lane[i] = ((i & 1u) != 0u) ? 0xFFFFFFFFu : 0u;
    }
    s_masks = 1;
}

/* 128 source bytes -> 128 lanes, four vectors of 32.  `bytes` is the same
   expansion for a row whose start is not 128-byte aligned; it reads up to 255
   bytes from the address, so the caller only uses it where that stays inside
   the row. */
static inline void spread_128_bytes(HVX_Vector bytes, uint32_t* dst,
                                    uint32_t shift)
{
    /* Black first, then the gain: the pedestal is a property of the sensor and
       must not be amplified with the light. */
    bytes = hvx_sub_sat_u8(bytes, hvx_splat(0x10101010u));

    /* AGC is a power of two, applied while the row is still packed: one
       saturating add per stop for 128 pixels instead of 32. */
    for (uint32_t i = 0; i < shift; ++i) {
        bytes = hvx_add_sat_u8(bytes, bytes);
    }

    union {
        HVX_VectorPair pair;
        HVX_Vector half[2];
    } wide, low, high;

    wide.pair = Q6_Wuh_vunpack_Vub(bytes);
    low.pair = Q6_Wuw_vunpack_Vuh(wide.half[0]);
    high.pair = Q6_Wuw_vunpack_Vuh(wide.half[1]);

    hvx_store(dst + 0, hvx_lane_bytes(low.half[0]));
    hvx_store(dst + 32, hvx_lane_bytes(low.half[1]));
    hvx_store(dst + 64, hvx_lane_bytes(high.half[0]));
    hvx_store(dst + 96, hvx_lane_bytes(high.half[1]));
}

static inline void spread_128(const uint8_t* src, uint32_t* dst, uint32_t shift)
{
    spread_128_bytes(hvx_load(src), dst, shift);
}

static inline void spread_128_any(const uint8_t* src, uint32_t* dst,
                                  uint32_t shift)
{
    spread_128_bytes(hvx_load_any(src), dst, shift);
}

/* One raw row into `dst` lanes, with the edge padding filled.  `dst` points
   at lane 0; the PIPE_PAD lanes on either side belong to this function.
 *
 * Two vector paths: a row that is 128-byte aligned and a whole number of
 * vectors long is read with plain aligned loads, and any other row uses the
 * unaligned load, which touches up to 255 bytes from the address.  That is
 * why the second path stops two vectors short of the end of the row and
 * finishes with the scalar loop: it must never read past the dma-buf, and on
 * the last row there is nothing after it. */
static void spread_row(const uint8_t* src, uint32_t w, uint32_t* dst,
                       uint32_t shift)
{
    uint32_t x = 0;
    if (((uintptr_t)src & (PIPE_VBYTES - 1u)) == 0u &&
        ((w % PIPE_VBYTES) == 0u)) {
        for (; x < w; x += PIPE_VBYTES) {
            spread_128(src + x, dst + x, shift);
        }
    } else {
        for (; x + 2u * PIPE_VBYTES <= w; x += PIPE_VBYTES) {
            spread_128_any(src + x, dst + x, shift);
        }
    }
    for (; x < w; ++x) {
        const uint32_t sample = src[x];
        uint32_t value = (sample > PIPE_BLACK) ? sample - PIPE_BLACK : 0u;
        value <<= shift;
        if (value > 255u) {
            value = 255u;
        }
        dst[x] = value * 0x01010101u;
    }

    hvx_store(dst - PIPE_PAD, hvx_splat(dst[0]));
    /* The right pad has to go through the scalar unit: a vector store needs a
       128-byte address, and dst + w is only aligned when the width is a
       multiple of 32.  Aligning it down would overwrite the last samples of
       the row, which is exactly the eight-pixel stripe at the right edge of
       a 1640-wide frame. */
    const uint32_t last = dst[w - 1u];
    for (uint32_t pad = 0; pad < PIPE_PAD; ++pad) {
        dst[w + pad] = last;
    }
}

/* Packed 10-bit, four pixels in five bytes: the top eight bits of pixel k sit
   at byte k*5/4.  The low two bits are what truncation drops. */
static void unpack10_row(const uint8_t* src, uint32_t w, uint8_t* dst)
{
    for (uint32_t x = 0; x < w; ++x) {
        dst[x] = src[(x * 5u) >> 2];
    }
}

static void spread_source(const uint8_t* raw, const geom* g, uint32_t y,
                          uint32_t raw8, uint32_t shift, uint32_t* dst)
{
    const uint8_t* row = raw + (size_t)y * g->stride;
    if (raw8 != 0u) {
        spread_row(row, g->width, dst, shift);
        return;
    }
    unpack10_row(row, g->width, s_bytes);
    spread_row(s_bytes, g->width, dst, shift);
}

/* The three rows the demosaic of one row reads, as a rolling window. */
typedef struct {
    uint32_t* a; /* row y-1 */
    uint32_t* b; /* row y */
    uint32_t* c; /* row y+1 */
    uint32_t y;
} ring;

static void ring_start(ring* r, const uint8_t* raw, const geom* g,
                       uint32_t raw8, uint32_t shift)
{
    r->a = s_row[0] + PIPE_PAD;
    r->b = s_row[1] + PIPE_PAD;
    r->c = s_row[2] + PIPE_PAD;
    r->y = 0;
    spread_source(raw, g, 0u, raw8, shift, r->b);
    if (g->height > 1u) {
        spread_source(raw, g, 1u, raw8, shift, r->c);
    }
}

static void ring_rows(const ring* r, const geom* g, const uint32_t** up,
                      const uint32_t** cur, const uint32_t** dn)
{
    *up = (r->y == 0u) ? r->b : r->a;
    *cur = r->b;
    *dn = (r->y + 1u < g->height) ? r->c : r->b;
}

/* Rotate the ring and refill the buffer that just became free.  It held row
   y-1, so writing row y+2 into it cannot clobber anything the next iteration
   still reads. */
static void ring_next(ring* r, const uint8_t* raw, const geom* g, uint32_t raw8,
                      uint32_t shift)
{
    uint32_t* spare = r->a;
    r->a = r->b;
    r->b = r->c;
    r->c = spare;
    ++r->y;
    if (r->y + 1u < g->height) {
        spread_source(raw, g, r->y + 1u, raw8, shift, r->c);
    }
}

/*
 * White balance, as a fixed-point gain on a lane whose byte is replicated in
 * all four positions: (v * m) >> 8, saturating, with m = gain * 256.
 *
 * The numbers come from the sensor's own raw.  With the black pedestal
 * removed, the four planes of a live frame average R/G 0.804 and B/G 0.460,
 * so 1/0.804 = 1.246 and 1/0.460 = 2.172 make a neutral scene neutral.  The
 * 1.25 / 1.75 this replaces were measured with the pedestal still in the
 * samples: that is why the picture came out 7% red and 8% blue, and why the
 * cast was in both the AGC and the no-AGC frame alike.
 */
static inline HVX_Vector wb_gain(HVX_Vector v, uint32_t m)
{
    const HVX_Vector x = hvx_and(v, hvx_splat(0x000000FFu));
    HVX_Vector y = hvx_shr_w(hvx_mul_w(x, m), 8u);
    y = hvx_min_w(y, hvx_splat(255u));
    return hvx_lane_bytes(y);
}

/*
 * One block of 32 output pixels.  The CFA pattern is RGGB, so the row parity
 * picks which channel the centre sample is, and the column parity picks the
 * rest.  The masks are lane masks: a 32-pixel block starts on an even column,
 * so lane i is pixel x+i and its parity is i&1.
 */
static inline HVX_Vector demosaic_block(const uint32_t* up, const uint32_t* cur,
                                        const uint32_t* dn, uint32_t x,
                                        uint32_t y, HVX_Vector even,
                                        HVX_Vector odd)
{
    const HVX_Vector c = hvx_load(cur + x);
    const HVX_Vector l = hvx_left(cur, x, c);
    const HVX_Vector r = hvx_right(cur, x, c);
    const HVX_Vector u = hvx_load(up + x);
    const HVX_Vector d = hvx_load(dn + x);
    const HVX_Vector ul = hvx_left(up, x, u);
    const HVX_Vector ur = hvx_right(up, x, u);
    const HVX_Vector dl = hvx_left(dn, x, d);
    const HVX_Vector dr = hvx_right(dn, x, d);

    const HVX_Vector h = hvx_avg_u8(l, r); /* horizontal pair */
    const HVX_Vector v = hvx_avg_u8(u, d); /* vertical pair */
    const HVX_Vector x4 = hvx_avg_u8(hvx_avg_u8(ul, ur), hvx_avg_u8(dl, dr));
    const HVX_Vector cross = hvx_avg_u8(h, v); /* the four axial neighbours */

    HVX_Vector cr;
    HVX_Vector cg;
    HVX_Vector cb;
    if ((y & 1u) == 0u) {
        /* even row: R at even columns, Gr at odd */
        cr = hvx_or(hvx_and(c, even), hvx_and(h, odd));
        cg = hvx_or(hvx_and(cross, even), hvx_and(c, odd));
        cb = hvx_or(hvx_and(x4, even), hvx_and(v, odd));
    } else {
        /* odd row: Gb at even columns, B at odd */
        cr = hvx_or(hvx_and(v, even), hvx_and(x4, odd));
        cg = hvx_or(hvx_and(c, even), hvx_and(cross, odd));
        cb = hvx_or(hvx_and(h, even), hvx_and(c, odd));
    }

    /* Fixed white balance, the same numbers the host reference uses. */
    cr = wb_gain(cr, 319u);
    cb = wb_gain(cb, 556u);

    return hvx_or(
        hvx_or(hvx_and(cr, hvx_splat(0x000000FFu)),
               hvx_and(cg, hvx_splat(0x0000FF00u))),
        hvx_or(hvx_and(cb, hvx_splat(0x00FF0000u)), hvx_splat(0xFF000000u)));
}

/* sum / (scale*scale) for a channel sum of at most 36*255.  HVX has no
   integer divide, so a power-of-two divisor is a shift and the others are a
   multiply-high by ceil(2^18/d): exact for every sum this pass can produce,
   and the multiplier stays inside the signed halfword vmpyio takes. */
static inline HVX_Vector scale_div(HVX_Vector sum, uint32_t d, uint32_t magic)
{
    if (d == 4u) {
        return hvx_shr_w(sum, 2u);
    }
    if (d == 16u) {
        return hvx_shr_w(sum, 4u);
    }
    return hvx_shr_w(hvx_mul_w(sum, magic), 18u);
}

/* One output row: every scale-th lane of the reduced row.  A strided gather
   is the one thing HVX cannot do in a vector, so this is scalar, but four
   independent loads per iteration, which is what lets the load unit pipeline
   them instead of waiting on one at a time. */
static void compact_row(const uint32_t* hrow, uint32_t out_w,
                        uint32_t out_lanes, uint32_t scale, uint32_t* dst)
{
    const uint32_t* src = hrow;
    uint32_t ox = 0;
    for (; ox + 4u <= out_w; ox += 4u) {
        const uint32_t a = src[0];
        const uint32_t b = src[scale];
        const uint32_t c = src[2u * scale];
        const uint32_t d = src[3u * scale];
        dst[ox] = a;
        dst[ox + 1u] = b;
        dst[ox + 2u] = c;
        dst[ox + 3u] = d;
        src += 4u * scale;
    }
    for (; ox < out_w; ++ox) {
        dst[ox] = *src;
        src += scale;
    }
    const uint32_t last = dst[out_w - 1u];
    for (ox = out_w; ox < out_lanes; ++ox) {
        dst[ox] = last;
    }
}

/* One 32-bit lane (R,G,B,A bytes) through the display curve.
 *
 * The bytes have to be masked off before the lookup: gamma22() clamps its index
 * to the end of the table, so a whole word handed to it, alpha included,
 * reads past the top for every channel and the frame comes out pure white. That
 * is not a hypothetical: it is what a tone-curve change did to the skel while
 * the host reference kept working, and the two engines disagreed on every
 * pixel. */
static inline uint32_t gamma_word(uint32_t v)
{
    return (uint32_t)gamma22(v & 0xFFu) |
           ((uint32_t)gamma22((v >> 8) & 0xFFu) << 8) |
           ((uint32_t)gamma22((v >> 16) & 0xFFu) << 16) | 0xFF000000u;
}

/* Display encoding, one output row at a time, returning the sum of the green
   bytes so the exposure loop measures what the screen shows.  Gamma goes
   after the window average, never before: this pass sums linear samples and
   divides, and the host reference is built the same way round.
 *
 * Four pixels per turn: the three table loads of one pixel are dependent on
 * that pixel's bytes, and a straight loop waits on each in turn: a dozen
 * cycles per pixel on a 640x480 frame.  Interleaving four pixels lets the
 * loads overlap, which is the difference between a pass that costs more than
 * the demosaic and one that does not. */
static uint32_t gamma_row(uint32_t* row, uint32_t n)
{
    uint32_t sum = 0;
    uint32_t x = 0;
    for (; x + 4u <= n; x += 4u) {
        const uint32_t o0 = gamma_word(row[x + 0u]);
        const uint32_t o1 = gamma_word(row[x + 1u]);
        const uint32_t o2 = gamma_word(row[x + 2u]);
        const uint32_t o3 = gamma_word(row[x + 3u]);
        row[x + 0u] = o0;
        row[x + 1u] = o1;
        row[x + 2u] = o2;
        row[x + 3u] = o3;
        sum += ((o0 >> 8) & 0xFFu) + ((o1 >> 8) & 0xFFu) + ((o2 >> 8) & 0xFFu) +
               ((o3 >> 8) & 0xFFu);
    }
    for (; x < n; ++x) {
        const uint32_t o = gamma_word(row[x]);
        row[x] = o;
        sum += (o >> 8) & 0xFFu;
    }
    return sum;
}

/* scale == 1: every demosaiced row goes straight to the surface. */
static uint32_t demosaic_direct(const uint8_t* raw, const geom* g,
                                uint32_t raw8, uint32_t shift, uint32_t lanes,
                                uint8_t* out)
{
    const uint32_t h = g->height;
    const HVX_Vector even = hvx_load(s_even_lane);
    const HVX_Vector odd = hvx_load(s_odd_lane);
    uint32_t sum = 0;

    ring r;
    ring_start(&r, raw, g, raw8, shift);

    for (uint32_t y = 0; y < h; ++y) {
        const uint32_t* up;
        const uint32_t* cur;
        const uint32_t* dn;
        ring_rows(&r, g, &up, &cur, &dn);
        uint32_t* dst = (uint32_t*)(out + (size_t)y * lanes * 4u);

        for (uint32_t x = 0; x < lanes; x += PIPE_LANES) {
            const HVX_Vector pixel =
                demosaic_block(up, cur, dn, x, y, even, odd);
            hvx_store(dst + x, pixel);
        }
        /* The lanes past the visible width repeat the last visible pixel, the
           same rule compact_row and the host's pad_row use.  Without this the
           block would hold the demosaic of the repeated last *raw* sample,
           which is not the same value, and a filter that averages across the
           boundary (pixelate) would then disagree with the reference. */
        if (lanes > g->width) {
            const uint32_t last = dst[g->width - 1u];
            for (uint32_t x = g->width; x < lanes; ++x) {
                dst[x] = last;
            }
        }
        sum += gamma_row(dst, lanes);
        ring_next(&r, raw, g, raw8, shift);
    }
    return sum / (lanes * h);
}

/* scale > 1: sum scale rows per channel, reduce scale columns per pixel.
 *
 * The accumulators hold two 16-bit channels per 32-bit lane rather than one:
 * (R,B) in one row and (G,A) in the other.  Six rows of 8-bit samples sum to
 * at most 1530, and six lanes of that to at most 9180, so nothing carries
 * between the halves and the whole vertical and horizontal sum is two
 * vectors wide instead of three. */
static uint32_t demosaic_scaled(const uint8_t* raw, const geom* g,
                                uint32_t raw8, uint32_t shift, uint32_t scale,
                                uint32_t out_w, uint32_t out_lanes,
                                uint32_t src_lanes, uint8_t* out)
{
    const uint32_t h = g->height;
    const uint32_t out_h = h / scale;
    const uint32_t d = scale * scale;
    const uint32_t magic = (262144u + d - 1u) / d;
    const HVX_Vector even = hvx_load(s_even_lane);
    const HVX_Vector odd = hvx_load(s_odd_lane);
    const HVX_Vector pairs = hvx_splat(0x00FF00FFu);
    const HVX_Vector low = hvx_splat(0x0000FFFFu);
    uint32_t sum = 0;

    uint32_t* arb = s_acc[0] + PIPE_PAD; /* R in the low half, B in the high */
    uint32_t* aga = s_acc[1] + PIPE_PAD; /* G in the low half, A in the high */
    uint32_t* hrow = s_hrow + PIPE_PAD;

    ring r;
    ring_start(&r, raw, g, raw8, shift);

    for (uint32_t oy = 0; oy < out_h; ++oy) {
        for (uint32_t k = 0; k < scale; ++k) {
            const uint32_t y = oy * scale + k;
            const uint32_t* up;
            const uint32_t* cur;
            const uint32_t* dn;
            ring_rows(&r, g, &up, &cur, &dn);

            for (uint32_t x = 0; x < src_lanes; x += PIPE_LANES) {
                const HVX_Vector pixel =
                    demosaic_block(up, cur, dn, x, y, even, odd);
                const HVX_Vector rb = hvx_and(pixel, pairs);
                const HVX_Vector ga = hvx_and(hvx_shr_w(pixel, 8u), pairs);
                if (k == 0u) {
                    hvx_store(arb + x, rb);
                    hvx_store(aga + x, ga);
                } else {
                    hvx_store(arb + x, hvx_add_w(hvx_load(arb + x), rb));
                    hvx_store(aga + x, hvx_add_w(hvx_load(aga + x), ga));
                }
            }
            ring_next(&r, raw, g, raw8, shift);
        }

        /* The horizontal sum of the last block reads one vector past the
           row; repeating the last lane keeps that read inside the buffer. */
        hvx_store(arb + src_lanes, hvx_splat(arb[src_lanes - 1u]));
        hvx_store(aga + src_lanes, hvx_splat(aga[src_lanes - 1u]));

        for (uint32_t x = 0; x < src_lanes; x += PIPE_LANES) {
            const HVX_Vector own_rb = hvx_load(arb + x);
            const HVX_Vector own_ga = hvx_load(aga + x);
            HVX_Vector srb = own_rb;
            HVX_Vector sga = own_ga;
            for (uint32_t k = 1; k < scale; ++k) {
                srb = hvx_add_w(srb, hvx_right_k(arb, x, own_rb, k));
                sga = hvx_add_w(sga, hvx_right_k(aga, x, own_ga, k));
            }
            const HVX_Vector r = scale_div(hvx_and(srb, low), d, magic);
            const HVX_Vector b = scale_div(hvx_shr_w(srb, 16u), d, magic);
            const HVX_Vector gr = scale_div(hvx_and(sga, low), d, magic);
            hvx_store(hrow + x, hvx_or(hvx_or(r, hvx_shl_w(gr, 8u)),
                                       hvx_or(hvx_shl_w(b, 16u),
                                              hvx_splat(0xFF000000u))));
        }

        compact_row(hrow, out_w, out_lanes, scale,
                    (uint32_t*)(out + (size_t)oy * out_lanes * 4u));
        sum += gamma_row((uint32_t*)(out + (size_t)oy * out_lanes * 4u),
                         out_lanes);
    }
    return sum / (out_lanes * out_h);
}

uint32_t demosaic_frame(const uint8_t* raw, const geom* g, uint32_t raw8,
                        uint32_t shift, uint8_t* out)
{
    masks_init();

    const uint32_t scale = (g->scale == 0u) ? 1u : g->scale;
    const uint32_t out_w = out_width(g->width, scale);
    const uint32_t out_lanes = lane_width(out_w); /* == out_w by construction */

    if (scale == 1u) {
        return demosaic_direct(raw, g, raw8, shift, out_lanes, out);
    }

    /* The box filter reads `scale` consecutive lanes per output pixel, so the
       last window ends at out_w*scale, which is <= width by construction.
       Nothing is read past the raw row, and the surface has no padding lanes
       to fill, the same invariant that keeps the surface stride equal to
       width*4. */
    const uint32_t src_lanes = out_lanes * scale;
    return demosaic_scaled(raw, g, raw8, shift, scale, out_w, out_lanes,
                           src_lanes, out);
}
