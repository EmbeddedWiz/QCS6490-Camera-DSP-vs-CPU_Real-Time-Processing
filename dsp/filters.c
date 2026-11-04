/*
 * filters.c - every look, in place, on the CDSP.
 *
 * The host reference in src/cpu_engine.cpp defines what each filter does. The
 * functions here produce the same picture, one 32-pixel block at a time. Four
 * of the looks (edge, sketch, emboss, sharpen) work on a neighbourhood, so
 * they first turn rows of RGBA into luma rows. Recomputing luma for every
 * neighbour would be nine times the work.
 *
 * Writing in place is safe with one row of lag: the pass writes row y only
 * after it has read the luma of row y+1, so what the next iteration reads is
 * still the input of this pass, not its output.
 *
 * s_work is shared scratch. The row passes need up to three luma rows, and
 * pixelate uses the same four rows for its accumulators and its two block
 * rows. No two passes run at once, so they can share the arena.
 */
#include "kernel.h"

static _Alignas(PIPE_VBYTES) uint32_t s_work[4][PIPE_MAX_W + 2u * PIPE_PAD];

/* One lane of scratch, aligned, for pulling a single value out of a vector. */
static _Alignas(PIPE_VBYTES) uint32_t s_lane[PIPE_LANES];

static inline uint32_t* row_at(uint8_t* frame, uint32_t y, uint32_t w)
{
    return (uint32_t*)(frame + (size_t)y * w * 4u);
}

/* (a + b + 1) / 2 on 32-bit lanes, the same rounding the host uses. */
static inline HVX_Vector hvx_avg_w(HVX_Vector a, HVX_Vector b)
{
    return hvx_shr_w(hvx_add_w(hvx_add_w(a, b), hvx_splat(1u)), 1u);
}

/* RGBA row -> luma lanes (0..255).  Same weights as the host:
 * (77R+150G+29B)/256. */
static void luma_row(const uint32_t* src, uint32_t w, uint32_t* dst)
{
    const HVX_Vector red = hvx_splat(0x000000FFu);
    const HVX_Vector green = hvx_splat(0x0000FF00u);
    const HVX_Vector blue = hvx_splat(0x00FF0000u);

    for (uint32_t x = 0; x < w; x += PIPE_LANES) {
        const HVX_Vector pixel = hvx_load(src + x);
        const HVX_Vector r = hvx_and(pixel, red);
        const HVX_Vector g = hvx_shr_w(hvx_and(pixel, green), 8);
        const HVX_Vector b = hvx_shr_w(hvx_and(pixel, blue), 16);

        HVX_Vector y = hvx_add_w(hvx_mul_w(r, 77u), hvx_mul_w(g, 150u));
        y = hvx_add_w(y, hvx_mul_w(b, 29u));
        hvx_store(dst + x, hvx_shr_w(y, 8));
    }

    hvx_store(dst - PIPE_PAD, hvx_splat(dst[0]));
    hvx_store(dst + w, hvx_splat(dst[w - 1u]));
}

/*
 * Sobel on luma, |gx|+|gy| scaled by the strength, written as grey.
 *
 * `invert` turns the same magnitude into dark lines on white, which is the
 * sketch look: the picture is the paper and the edges are the pencil.
 */
static void sobel_row(const uint32_t* up, const uint32_t* cur,
                      const uint32_t* dn, uint32_t w, uint32_t gain, int invert,
                      uint32_t* out)
{
    const HVX_Vector limit = hvx_splat(255u);

    for (uint32_t x = 0; x < w; x += PIPE_LANES) {
        const HVX_Vector c = hvx_load(cur + x);
        const HVX_Vector l = hvx_left(cur, x, c);
        const HVX_Vector r = hvx_right(cur, x, c);
        const HVX_Vector u = hvx_load(up + x);
        const HVX_Vector d = hvx_load(dn + x);
        const HVX_Vector lu = hvx_left(up, x, u);
        const HVX_Vector ru = hvx_right(up, x, u);
        const HVX_Vector ld = hvx_left(dn, x, d);
        const HVX_Vector rd = hvx_right(dn, x, d);

        const HVX_Vector gx =
            hvx_sub_w(hvx_add_w(hvx_add_w(ru, hvx_shl_w(r, 1)), rd),
                      hvx_add_w(hvx_add_w(lu, hvx_shl_w(l, 1)), ld));
        const HVX_Vector gy =
            hvx_sub_w(hvx_add_w(hvx_add_w(ld, hvx_shl_w(d, 1)), rd),
                      hvx_add_w(hvx_add_w(lu, hvx_shl_w(u, 1)), ru));

        HVX_Vector magnitude = hvx_add_w(hvx_abs_w(gx), hvx_abs_w(gy));
        magnitude = hvx_shr_w(hvx_mul_w(magnitude, gain), 2);
        magnitude = hvx_min_w(magnitude, limit);
        if (invert != 0) {
            magnitude = hvx_sub_w(limit, magnitude);
        }
        hvx_store(out + x, hvx_grey_pixel(magnitude));
    }
}

/* 128 + (luma here - luma one row up and one column left) * strength. */
static void emboss_row(const uint32_t* up, const uint32_t* cur, uint32_t w,
                       uint32_t gain, uint32_t* out)
{
    const HVX_Vector mid = hvx_splat(128u);

    for (uint32_t x = 0; x < w; x += PIPE_LANES) {
        const HVX_Vector c = hvx_load(cur + x);
        const HVX_Vector u = hvx_load(up + x);
        const HVX_Vector corner = hvx_left(up, x, u);
        HVX_Vector value = hvx_sub_w(c, corner);
        value = hvx_add_w(mid, hvx_mul_w(value, gain));
        hvx_store(out + x, hvx_grey_pixel(hvx_clamp_u8_w(value)));
    }
}

/*
 * Sharpen: a luma high-pass added back to the colour.
 *
 *   blur   = average of the four neighbours (two rounded averages of two)
 *   detail = luma - blur, biased by 255 so the DSP's multiply stays unsigned
 *   out    = clamp(c + ((detail + 255) * strength >> 1) - (255 * strength >>
 * 1))
 *
 * The host computes the same sum in the same order, so both round alike.
 */
static void sharpen_row(const uint32_t* up, const uint32_t* cur,
                        const uint32_t* dn, uint32_t w, uint32_t gain,
                        uint32_t* out)
{
    const HVX_Vector bias = hvx_splat(255u);
    const HVX_Vector back = hvx_splat((255u * gain) >> 1);
    const HVX_Vector red = hvx_splat(0x000000FFu);
    const HVX_Vector green = hvx_splat(0x0000FF00u);
    const HVX_Vector blue = hvx_splat(0x00FF0000u);

    for (uint32_t x = 0; x < w; x += PIPE_LANES) {
        const HVX_Vector c = hvx_load(cur + x);
        const HVX_Vector horizontal =
            hvx_avg_w(hvx_left(cur, x, c), hvx_right(cur, x, c));
        const HVX_Vector vertical =
            hvx_avg_w(hvx_load(up + x), hvx_load(dn + x));
        const HVX_Vector blur = hvx_avg_w(horizontal, vertical);

        HVX_Vector detail = hvx_add_w(hvx_sub_w(c, blur), bias);
        detail = hvx_shr_w(hvx_mul_w(detail, gain), 1u);
        detail = hvx_sub_w(detail, back);

        const HVX_Vector pixel = hvx_load(out + x);
        const HVX_Vector r =
            hvx_clamp_u8_w(hvx_add_w(hvx_and(pixel, red), detail));
        const HVX_Vector g = hvx_clamp_u8_w(
            hvx_add_w(hvx_shr_w(hvx_and(pixel, green), 8), detail));
        const HVX_Vector b = hvx_clamp_u8_w(
            hvx_add_w(hvx_shr_w(hvx_and(pixel, blue), 16), detail));

        hvx_store(out + x,
                  hvx_or(hvx_or(r, hvx_shl_w(g, 8)),
                         hvx_or(hvx_shl_w(b, 16), hvx_splat(0xFF000000u))));
    }
}

/*
 * Posterize: levels = min(strength+1, 7), step = 256/levels, and every channel
 * becomes (v/step)*step + step/2.
 *
 * HVX has no integer divide, so the divide is a multiply-high by
 * m = ceil(65536/step): v/step == (v*m) >> 16 for v in 0..255 and every step
 * this filter can produce (128, 85, 64, 51, 42, 36).
 */
static void posterize_row(uint32_t* row, uint32_t w, uint32_t step)
{
    const uint32_t magic = (65536u + step - 1u) / step;
    const HVX_Vector red = hvx_splat(0x000000FFu);
    const HVX_Vector green = hvx_splat(0x0000FF00u);
    const HVX_Vector blue = hvx_splat(0x00FF0000u);
    const HVX_Vector alpha = hvx_splat(0xFF000000u);
    const HVX_Vector half = hvx_splat(step / 2u);
    const HVX_Vector limit = hvx_splat(255u);

    for (uint32_t x = 0; x < w; x += PIPE_LANES) {
        const HVX_Vector pixel = hvx_load(row + x);
        HVX_Vector r = hvx_and(pixel, red);
        HVX_Vector g = hvx_shr_w(hvx_and(pixel, green), 8);
        HVX_Vector b = hvx_shr_w(hvx_and(pixel, blue), 16);

        r = hvx_add_w(hvx_mul_w(hvx_shr_w(hvx_mul_w(r, magic), 16), step),
                      half);
        g = hvx_add_w(hvx_mul_w(hvx_shr_w(hvx_mul_w(g, magic), 16), step),
                      half);
        b = hvx_add_w(hvx_mul_w(hvx_shr_w(hvx_mul_w(b, magic), 16), step),
                      half);

        r = hvx_min_w(r, limit);
        g = hvx_min_w(g, limit);
        b = hvx_min_w(b, limit);

        hvx_store(row + x, hvx_or(hvx_or(r, hvx_shl_w(g, 8)),
                                  hvx_or(hvx_shl_w(b, 16), alpha)));
    }
}

/*
 * Sepia: the luma carries the picture, and a warm tone replaces the colour.
 *
 *   tone = (1.20 l, 0.95 l, 0.70 l), clamped
 *   out  = (c * (6 - strength) + tone * strength) / 6
 *
 * The divide by six is a multiply-high by ceil(65536/6) = 10923, exact for
 * every numerator this pass can produce (at most 6 * 255).
 */
static inline HVX_Vector sepia_blend(HVX_Vector colour, HVX_Vector tone,
                                     uint32_t keep, uint32_t strength,
                                     HVX_Vector magic)
{
    const HVX_Vector sum =
        hvx_add_w(hvx_mul_w(colour, keep), hvx_mul_w(tone, strength));
    return hvx_shr_w(hvx_mul_w(sum, 10923u), 16u);
}

static void sepia_row(uint32_t* row, uint32_t w, uint32_t strength)
{
    const HVX_Vector red = hvx_splat(0x000000FFu);
    const HVX_Vector green = hvx_splat(0x0000FF00u);
    const HVX_Vector blue = hvx_splat(0x00FF0000u);
    const HVX_Vector limit = hvx_splat(255u);
    const HVX_Vector magic = hvx_splat(10923u);
    const uint32_t keep = 6u - strength;

    for (uint32_t x = 0; x < w; x += PIPE_LANES) {
        const HVX_Vector pixel = hvx_load(row + x);
        const HVX_Vector r = hvx_and(pixel, red);
        const HVX_Vector g = hvx_shr_w(hvx_and(pixel, green), 8);
        const HVX_Vector b = hvx_shr_w(hvx_and(pixel, blue), 16);

        HVX_Vector l = hvx_add_w(hvx_mul_w(r, 77u), hvx_mul_w(g, 150u));
        l = hvx_shr_w(hvx_add_w(l, hvx_mul_w(b, 29u)), 8u);

        const HVX_Vector tr =
            hvx_min_w(hvx_shr_w(hvx_mul_w(l, 307u), 8u), limit);
        const HVX_Vector tg =
            hvx_min_w(hvx_shr_w(hvx_mul_w(l, 243u), 8u), limit);
        const HVX_Vector tb =
            hvx_min_w(hvx_shr_w(hvx_mul_w(l, 179u), 8u), limit);

        hvx_store(
            row + x,
            hvx_or(
                hvx_or(sepia_blend(r, tr, keep, strength, magic),
                       hvx_shl_w(sepia_blend(g, tg, keep, strength, magic), 8)),
                hvx_or(hvx_shl_w(sepia_blend(b, tb, keep, strength, magic), 16),
                       hvx_splat(0xFF000000u))));
    }
}

static void invert_row(uint32_t* row, uint32_t w)
{
    const HVX_Vector rgb = hvx_splat(0x00FFFFFFu);
    for (uint32_t x = 0; x < w; x += PIPE_LANES) {
        hvx_store(row + x, hvx_xor(hvx_load(row + x), rgb));
    }
}

/*
 * Pixelate: every n x n block becomes its average, n = 2^strength (2..64).
 *
 * Two stages, because one will not fit: the horizontal sum of n lanes is
 * divided by n (at most 255, so it still fits a 16-bit field), the n rows are
 * summed (at most 255 * 64), and the total is divided by n again. The host
 * does the same two divisions, so the rounding matches. The picture is at most
 * one level darker than an exact average.
 *
 * The sums live in packed (R,B) and (G,A) pairs, two 16-bit fields per lane.
 * Lanes that do not start a block are masked to zero right away, so the
 * vertical accumulator only ever adds real block sums.
 */
static inline HVX_Vector sum_n(HVX_Vector v, uint32_t n)
{
    HVX_Vector s = v;
    for (uint32_t k = 1; k < n; k <<= 1) {
        s = hvx_add_w(s, hvx_rot(s, k));
    }
    return s;
}

/* /2^lg on each 16-bit field of a packed (low, high) pair. */
static inline HVX_Vector field_div(HVX_Vector pair, uint32_t lg)
{
    const HVX_Vector low = hvx_splat(0x0000FFFFu);
    const HVX_Vector a = hvx_shr_w(hvx_and(pair, low), lg);
    const HVX_Vector b = hvx_shl_w(hvx_shr_w(hvx_shr_w(pair, 16u), lg), 16u);
    return hvx_or(a, b);
}

/* 0x00FF00FF in the lanes that start a block, zero elsewhere. */
static HVX_Vector block_mask(uint32_t n)
{
    for (uint32_t i = 0; i < PIPE_LANES; ++i) {
        s_lane[i] = (i % n == 0u) ? 0x00FF00FFu : 0u;
    }
    return hvx_load(s_lane);
}

static inline uint32_t log2_of(uint32_t n)
{
    uint32_t lg = 0;
    while ((1u << lg) < n) {
        ++lg;
    }
    return lg;
}

/* n == 64: a block is two vectors wide, so one block at a time is cheap:
   there is only one per 64 lanes, and the sum of two halves is one add. */
static void pixelate_wide(uint8_t* frame, uint32_t w, uint32_t h)
{
    const uint32_t n = 64u;
    const uint32_t blocks = (w + n - 1u) / n;
    uint32_t sum[3][(PIPE_MAX_W + 63u) / 64u];

    for (uint32_t by = 0; by < h; by += n) {
        const uint32_t rows = (by + n <= h) ? n : (h - by);
        for (uint32_t b = 0; b < blocks; ++b) {
            sum[0][b] = 0u;
            sum[1][b] = 0u;
            sum[2][b] = 0u;
        }
        for (uint32_t k = 0; k < n; ++k) {
            const uint32_t y = (by + k < h) ? by + k : h - 1u;
            const uint32_t* row = row_at(frame, y, w);
            for (uint32_t b = 0; b < blocks; ++b) {
                const uint32_t x = b * n;
                uint32_t r = 0u;
                uint32_t g = 0u;
                uint32_t bl = 0u;
                for (uint32_t j = 0; j < n; ++j) {
                    const uint32_t px = row[(x + j < w) ? x + j : w - 1u];
                    r += px & 0xFFu;
                    g += (px >> 8) & 0xFFu;
                    bl += (px >> 16) & 0xFFu;
                }
                sum[0][b] += r / n;
                sum[1][b] += g / n;
                sum[2][b] += bl / n;
            }
        }
        for (uint32_t b = 0; b < blocks; ++b) {
            const HVX_Vector r = hvx_splat(sum[0][b] / n);
            const HVX_Vector g = hvx_splat(sum[1][b] / n);
            const HVX_Vector bl = hvx_splat(sum[2][b] / n);
            const HVX_Vector pixel =
                hvx_or(hvx_or(r, hvx_shl_w(g, 8)),
                       hvx_or(hvx_shl_w(bl, 16), hvx_splat(0xFF000000u)));
            for (uint32_t k = 0; k < rows; ++k) {
                uint32_t* out = row_at(frame, by + k, w);
                if (b * n < w) {
                    hvx_store(out + b * n, pixel);
                }
                if (b * n + PIPE_LANES < w) {
                    hvx_store(out + b * n + PIPE_LANES, pixel);
                }
            }
        }
    }
}

void filter_pixelate(uint8_t* frame, uint32_t w, uint32_t h, uint32_t strength)
{
    const uint32_t step = strength < 1u ? 1u : (strength > 6u ? 6u : strength);
    const uint32_t n = 1u << step;
    if (n > PIPE_LANES) {
        pixelate_wide(frame, w, h);
        return;
    }

    const uint32_t lg = log2_of(n);
    const HVX_Vector pairs = hvx_splat(0x00FF00FFu);
    const HVX_Vector mask = block_mask(n);
    uint32_t* acc_rb = s_work[0];
    uint32_t* acc_ga = s_work[1];
    uint32_t* trb = s_work[2] + PIPE_PAD;
    uint32_t* tga = s_work[3] + PIPE_PAD;

    for (uint32_t by = 0; by < h; by += n) {
        const uint32_t rows = (by + n <= h) ? n : (h - by);
        for (uint32_t x = 0; x < w; x += PIPE_LANES) {
            hvx_store(acc_rb + x, hvx_splat(0u));
            hvx_store(acc_ga + x, hvx_splat(0u));
        }
        for (uint32_t k = 0; k < n; ++k) {
            const uint32_t y = (by + k < h) ? by + k : h - 1u;
            const uint32_t* row = row_at(frame, y, w);
            for (uint32_t x = 0; x < w; x += PIPE_LANES) {
                const HVX_Vector pixel = hvx_load(row + x);
                const HVX_Vector rb = hvx_and(pixel, pairs);
                const HVX_Vector ga = hvx_and(hvx_shr_w(pixel, 8u), pairs);
                const HVX_Vector hrb =
                    hvx_and(field_div(sum_n(rb, n), lg), mask);
                const HVX_Vector hga =
                    hvx_and(field_div(sum_n(ga, n), lg), mask);
                hvx_store(acc_rb + x, hvx_add_w(hvx_load(acc_rb + x), hrb));
                hvx_store(acc_ga + x, hvx_add_w(hvx_load(acc_ga + x), hga));
            }
        }

        /* Divide the column sums, keep the block starts, then fill each
           block from the start it belongs to.  The left pad is zero so the
           first block of the row spreads from nothing but itself. */
        for (uint32_t x = 0; x < w; x += PIPE_LANES) {
            hvx_store(trb + x,
                      hvx_and(field_div(hvx_load(acc_rb + x), lg), mask));
            hvx_store(tga + x,
                      hvx_and(field_div(hvx_load(acc_ga + x), lg), mask));
        }
        for (uint32_t p = 1; p <= PIPE_PAD; ++p) {
            trb[-(int)p] = 0u;
            tga[-(int)p] = 0u;
        }

        for (uint32_t x = 0; x < w; x += PIPE_LANES) {
            const HVX_Vector mrb = hvx_load(trb + x);
            const HVX_Vector mga = hvx_load(tga + x);
            HVX_Vector orb = mrb;
            HVX_Vector oga = mga;
            for (uint32_t j = 1; j < n; ++j) {
                orb = hvx_or(orb, hvx_left_k(trb, x, mrb, j));
                oga = hvx_or(oga, hvx_left_k(tga, x, mga, j));
            }
            const HVX_Vector one = hvx_splat(0x000000FFu);
            const HVX_Vector pixel = hvx_or(
                hvx_or(hvx_and(orb, one), hvx_shl_w(hvx_and(oga, one), 8u)),
                hvx_or(hvx_shl_w(hvx_and(hvx_shr_w(orb, 16u), one), 16u),
                       hvx_splat(0xFF000000u)));
            for (uint32_t k = 0; k < rows; ++k) {
                hvx_store(row_at(frame, by + k, w) + x, pixel);
            }
        }
    }
}

void filter_edge(uint8_t* frame, uint32_t w, uint32_t h, uint32_t strength)
{
    const uint32_t gain = strength < 1u ? 1u : (strength > 6u ? 6u : strength);
    uint32_t* a = s_work[0] + PIPE_PAD;
    uint32_t* b = s_work[1] + PIPE_PAD;
    uint32_t* c = s_work[2] + PIPE_PAD;

    luma_row(row_at(frame, 0u, w), w, b);
    if (h > 1u) {
        luma_row(row_at(frame, 1u, w), w, c);
    }

    for (uint32_t y = 0; y < h; ++y) {
        const uint32_t* up = (y == 0u) ? b : a;
        const uint32_t* cur = b;
        const uint32_t* dn = (y + 1u < h) ? c : b;
        sobel_row(up, cur, dn, w, gain, 0, row_at(frame, y, w));

        uint32_t* spare = a;
        a = b;
        b = c;
        c = spare;
        if (y + 2u < h) {
            luma_row(row_at(frame, y + 2u, w), w, c);
        }
    }
}

void filter_sketch(uint8_t* frame, uint32_t w, uint32_t h, uint32_t strength)
{
    const uint32_t gain = strength < 1u ? 1u : (strength > 6u ? 6u : strength);
    uint32_t* a = s_work[0] + PIPE_PAD;
    uint32_t* b = s_work[1] + PIPE_PAD;
    uint32_t* c = s_work[2] + PIPE_PAD;

    luma_row(row_at(frame, 0u, w), w, b);
    if (h > 1u) {
        luma_row(row_at(frame, 1u, w), w, c);
    }

    for (uint32_t y = 0; y < h; ++y) {
        const uint32_t* up = (y == 0u) ? b : a;
        const uint32_t* cur = b;
        const uint32_t* dn = (y + 1u < h) ? c : b;
        sobel_row(up, cur, dn, w, gain, 1, row_at(frame, y, w));

        uint32_t* spare = a;
        a = b;
        b = c;
        c = spare;
        if (y + 2u < h) {
            luma_row(row_at(frame, y + 2u, w), w, c);
        }
    }
}

void filter_emboss(uint8_t* frame, uint32_t w, uint32_t h, uint32_t strength)
{
    const uint32_t gain = strength < 1u ? 1u : (strength > 6u ? 6u : strength);
    uint32_t* a = s_work[0];
    uint32_t* b = s_work[1];

    luma_row(row_at(frame, 0u, w), w, b);

    for (uint32_t y = 0; y < h; ++y) {
        const uint32_t* up = (y == 0u) ? b : a;
        const uint32_t* cur = b;
        emboss_row(up, cur, w, gain, row_at(frame, y, w));

        uint32_t* spare = a;
        a = b;
        b = spare;
        if (y + 1u < h) {
            luma_row(row_at(frame, y + 1u, w), w, b);
        }
    }
}

void filter_sharpen(uint8_t* frame, uint32_t w, uint32_t h, uint32_t strength)
{
    const uint32_t gain = strength < 1u ? 1u : (strength > 6u ? 6u : strength);
    uint32_t* a = s_work[0] + PIPE_PAD;
    uint32_t* b = s_work[1] + PIPE_PAD;
    uint32_t* c = s_work[2] + PIPE_PAD;

    luma_row(row_at(frame, 0u, w), w, b);
    if (h > 1u) {
        luma_row(row_at(frame, 1u, w), w, c);
    }

    for (uint32_t y = 0; y < h; ++y) {
        const uint32_t* up = (y == 0u) ? b : a;
        const uint32_t* cur = b;
        const uint32_t* dn = (y + 1u < h) ? c : b;
        sharpen_row(up, cur, dn, w, gain, row_at(frame, y, w));

        uint32_t* spare = a;
        a = b;
        b = c;
        c = spare;
        if (y + 2u < h) {
            luma_row(row_at(frame, y + 2u, w), w, c);
        }
    }
}

void filter_sepia(uint8_t* frame, uint32_t w, uint32_t h, uint32_t strength)
{
    const uint32_t level = strength < 1u ? 1u : (strength > 6u ? 6u : strength);
    for (uint32_t y = 0; y < h; ++y) {
        sepia_row(row_at(frame, y, w), w, level);
    }
}

void filter_posterize(uint8_t* frame, uint32_t w, uint32_t h, uint32_t strength)
{
    const uint32_t levels = (strength + 1u < 7u) ? strength + 1u : 7u;
    const uint32_t step = 256u / levels;
    for (uint32_t y = 0; y < h; ++y) {
        posterize_row(row_at(frame, y, w), w, step);
    }
}

void filter_invert(uint8_t* frame, uint32_t w, uint32_t h)
{
    for (uint32_t y = 0; y < h; ++y) {
        invert_row(row_at(frame, y, w), w);
    }
}
