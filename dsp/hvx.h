/*
 * hvx.h - the only place in this tree that names an HVX intrinsic.
 *
 * Two rules, used everywhere below:
 *
 *   - a pixel is one 32-bit lane, so a 128-byte vector holds 32 pixels;
 *   - a row that another pass will read has 32 lanes of padding on each side,
 *     filled with the first and last pixel repeated. That is what keeps the
 *     neighbour helpers branch-free: at x == 0 the "left" vector is the pad,
 *     and in the last block the "right" vector is the right pad.
 */
#ifndef PIPE_HVX_HVX_H
#define PIPE_HVX_HVX_H

#include <stdint.h>

#include <hexagon_protos.h>
#include <hexagon_types.h>

#define PIPE_LANES 32u   /* pixels per 128-byte vector */
#define PIPE_VBYTES 128u /* bytes per vector */
#define PIPE_PAD 32u     /* lanes of edge padding on each side of a row */
#define PIPE_MAX_W                                                             \
    3328u /* widest frame this kernel accepts (3280 rounded up) */

/* The widest lane row any pass touches.  out_width() rounds the output down, so
   a scaled pass reads exactly out_w*scale <= width lanes: the bound is the
   frame width itself, and the slack below covers the right-hand window of the
   last block plus the two aligned vectors a vlalign over-reads. */
#define PIPE_MAX_LANES 3456u
/* One padded lane row, with room for the two aligned vectors a vlalign
   over-reads at the far end. */
#define PIPE_LANE_ROW (PIPE_MAX_LANES + 2u * PIPE_PAD + 64u)

static inline HVX_Vector hvx_load(const void* p)
{
    return *(const HVX_Vector*)p;
}

/* Vector loads round the address down to 128 bytes, so an unaligned row is
   assembled from two aligned loads.  Callers only use this where reading a
   little past the row is known to stay inside the buffer. */
static inline HVX_Vector hvx_load_any(const void* p)
{
    const uintptr_t address = (uintptr_t)p;
    const uint32_t offset = (uint32_t)(address & (PIPE_VBYTES - 1u));
    const HVX_Vector* base = (const HVX_Vector*)(address - offset);
    if (offset == 0u) {
        return base[0];
    }
    /* vlalign(Vu, Vv, Rt) = Vv[128-Rt .. 127] ++ Vu[0 .. Rt-1]. */
    return Q6_V_vlalign_VVR(base[1], base[0], PIPE_VBYTES - offset);
}

static inline void hvx_store(void* p, HVX_Vector value)
{
    *(HVX_Vector*)p = value;
}

/* One 32-bit value in every lane. */
static inline HVX_Vector hvx_splat(uint32_t value)
{
    return Q6_V_vsplat_R((int32_t)value);
}

/* Row of pixels x-1 .. x+30 (the left neighbour of every lane).  `row` must
   point at lane 0 of a padded row. */
static inline HVX_Vector hvx_left(const uint32_t* row, uint32_t x,
                                  HVX_Vector own)
{
    const HVX_Vector previous = hvx_load(row + x - PIPE_LANES);
    return Q6_V_vlalign_VVR(own, previous, 4);
}

/* Row of pixels x+k .. x+k+31 (the k-th neighbour to the right of every
   lane).  k is at most 31, so both loads stay in the padded row. */
static inline HVX_Vector hvx_right_k(const uint32_t* row, uint32_t x,
                                     HVX_Vector own, uint32_t k)
{
    const HVX_Vector next = hvx_load(row + x + PIPE_LANES);
    return Q6_V_vlalign_VVR(next, own, PIPE_VBYTES - 4u * k);
}

/* Row of pixels x+1 .. x+32 (the right neighbour of every lane). */
static inline HVX_Vector hvx_right(const uint32_t* row, uint32_t x,
                                   HVX_Vector own)
{
    return hvx_right_k(row, x, own, 1u);
}

/* The same shift the other way: pixel x-k in every lane, taking the first k
   lanes from the previous vector of `row`.  k is at most 31. */
static inline HVX_Vector hvx_left_k(const uint32_t* row, uint32_t x,
                                    HVX_Vector own, uint32_t k)
{
    const HVX_Vector previous = hvx_load(row + x - PIPE_LANES);
    return Q6_V_vlalign_VVR(own, previous, 4u * k);
}

/* Lane i takes lane (i + k) mod 32 of the same vector.  Lanes at the far end
   wrap around, which is harmless wherever the caller only reads lanes whose
   window stays inside the vector.  vlalign moves values towards lower lanes,
   so a forward turn is a shift by 32 - k. */
static inline HVX_Vector hvx_rot(HVX_Vector v, uint32_t k)
{
    return Q6_V_vlalign_VVR(v, v, 4u * (PIPE_LANES - k));
}

static inline HVX_Vector hvx_and(HVX_Vector a, HVX_Vector b)
{
    return Q6_V_vand_VV(a, b);
}

static inline HVX_Vector hvx_or(HVX_Vector a, HVX_Vector b)
{
    return Q6_V_vor_VV(a, b);
}

static inline HVX_Vector hvx_xor(HVX_Vector a, HVX_Vector b)
{
    return Q6_V_vxor_VV(a, b);
}

static inline HVX_Vector hvx_avg_u8(HVX_Vector a, HVX_Vector b)
{
    return Q6_Vub_vavg_VubVub(a, b);
}

static inline HVX_Vector hvx_add_sat_u8(HVX_Vector a, HVX_Vector b)
{
    return Q6_Vub_vadd_VubVub_sat(a, b);
}

/* Per byte, saturating at zero: used for the sensor's black pedestal, where a
   sample below the pedestal is black, not a borrow into the next byte. */
static inline HVX_Vector hvx_sub_sat_u8(HVX_Vector a, HVX_Vector b)
{
    return Q6_Vub_vsub_VubVub_sat(a, b);
}

static inline HVX_Vector hvx_shr_u8(HVX_Vector a, uint32_t n)
{
    return Q6_Vub_vlsr_VubR(a, n);
}

static inline HVX_Vector hvx_add_w(HVX_Vector a, HVX_Vector b)
{
    return Q6_Vw_vadd_VwVw(a, b);
}

static inline HVX_Vector hvx_sub_w(HVX_Vector a, HVX_Vector b)
{
    return Q6_Vw_vsub_VwVw(a, b);
}

static inline HVX_Vector hvx_shl_w(HVX_Vector a, uint32_t n)
{
    return Q6_Vw_vasl_VwR(a, n);
}

static inline HVX_Vector hvx_shr_w(HVX_Vector a, uint32_t n)
{
    return Q6_Vuw_vlsr_VuwR(a, n);
}

static inline HVX_Vector hvx_abs_w(HVX_Vector a)
{
    return Q6_Vw_vabs_Vw(a);
}

static inline HVX_Vector hvx_max_w(HVX_Vector a, HVX_Vector b)
{
    return Q6_Vw_vmax_VwVw(a, b);
}

static inline HVX_Vector hvx_min_w(HVX_Vector a, HVX_Vector b)
{
    return Q6_Vw_vmin_VwVw(a, b);
}

/* Per-lane multiply by a constant.  HVX has no 32x32 multiply: vmpyie
   multiplies the even lanes and vmpyio the odd ones, each leaving the other
   half zero, so the two results combine with an OR.  (vmpyi looks like the
   obvious choice and is not: it is the even-lane form, and the odd lanes come
   back zero.) */
static inline HVX_Vector hvx_mul_w(HVX_Vector a, uint32_t factor)
{
    const HVX_Vector k = hvx_splat(factor);
    return hvx_or(Q6_Vw_vmpyie_VwVuh(a, k), Q6_Vw_vmpyio_VwVh(a, k));
}

/* 0x000000VV in each lane -> 0xVVVVVVVV. */
static inline HVX_Vector hvx_lane_bytes(HVX_Vector word)
{
    const HVX_Vector low = hvx_or(word, hvx_shl_w(word, 8));
    return hvx_or(low, hvx_shl_w(low, 16));
}

/* A 0..255 value in each lane -> the same value in R, G and B of an opaque
   pixel (0xFFVVVVVV). */
static inline HVX_Vector hvx_grey_pixel(HVX_Vector value)
{
    const HVX_Vector low = hvx_or(value, hvx_shl_w(value, 8));
    return hvx_or(hvx_or(low, hvx_shl_w(low, 8)), hvx_splat(0xFF000000u));
}

/* Clamp a signed word vector into 0..255. */
static inline HVX_Vector hvx_clamp_u8_w(HVX_Vector value)
{
    const HVX_Vector low = hvx_max_w(value, hvx_splat(0u));
    return hvx_min_w(low, hvx_splat(255u));
}

#endif
