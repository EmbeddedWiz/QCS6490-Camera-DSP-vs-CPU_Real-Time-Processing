/*
 * gamma.h - the display curve, shared by the DSP kernel and the host
 * reference so both turn a sample into the same byte.
 *
 * This is libcamera's tone curve, not a plain gamma. The simple IPA builds its
 * table (lut.cpp, updateGammaTable) as a contrast S-curve followed by a gamma
 * of 1/2.2, over the range left above the sensor's black level:
 *
 *     divisor = 256 - black - 1
 *     n       = (v - black) / divisor
 *     n      < 0.5 ? 0.5 * (n / 0.5)^contrastExp
 *                  : 1 - 0.5 * ((1 - n) / 0.5)^contrastExp
 *     out     = 255 * n^(1/2.2)
 *
 * with contrastExp = tan(contrast * pi/4). The tuning file asks for gamma 2.2
 * and contrast 1.15, so contrastExp is 1.268494.
 *
 * `v` is the sample after the black level has been removed and after white
 * balance. The table itself is indexed by the sample *with* the pedestal still
 * on it, because that is how libcamera builds it: the first black+1 entries
 * are zero and it divides by 256 - black - 1. So the lookup puts the pedestal
 * back. A pedestal-free sample would lose black twice, and everything within a
 * few counts of black would come out as pure zero.
 *
 * This was measured, not guessed. libcamera's own 640x480 capture of this dark
 * room has a median luma of 27, and the raw behind it has a median of 21
 * counts with the pedestal on. The table maps 21 to 28, and maps 5, which is
 * 21 with the pedestal taken off, to 0. That difference is the whole "our
 * picture is muddy and half black" report: with the double subtraction the
 * median sample of this room rendered as zero.
 *
 * It is a table and not a chain of pow() calls because the two libms (Hexagon
 * and the host's) may disagree in the last bit, and the parity test compares
 * bytes. One sample that rounds differently fails the test.
 */
#ifndef PIPE_GAMMA_H
#define PIPE_GAMMA_H

#include <stdint.h>

/* The sensor's black pedestal, in the 8-bit domain: 4096 on a 16-bit scale is
   64 in 10 bits is 16 here.  demosaic.c keeps PIPE_BLACK equal to this. */
#define PIPE_GAMMA_BLACK 16u

static const uint8_t kGamma22[256] = {
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   12,  18,  22,  26,  30,  33,  36,  39,  42,  45,  47,  49,  52,
    54,  56,  58,  60,  62,  64,  66,  68,  70,  72,  74,  76,  77,  79,  81,
    82,  84,  85,  87,  89,  90,  92,  93,  95,  96,  98,  99,  100, 102, 103,
    105, 106, 107, 109, 110, 111, 113, 114, 115, 116, 118, 119, 120, 121, 123,
    124, 125, 126, 127, 129, 130, 131, 132, 133, 134, 136, 137, 138, 139, 140,
    141, 142, 143, 144, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156,
    157, 158, 159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171,
    172, 173, 174, 175, 176, 176, 177, 178, 179, 180, 181, 182, 183, 184, 185,
    186, 187, 187, 188, 189, 190, 191, 192, 193, 193, 194, 195, 196, 197, 198,
    198, 199, 200, 201, 202, 202, 203, 204, 205, 205, 206, 207, 208, 208, 209,
    210, 211, 211, 212, 213, 213, 214, 215, 215, 216, 217, 217, 218, 219, 219,
    220, 221, 221, 222, 223, 223, 224, 224, 225, 226, 226, 227, 228, 228, 229,
    229, 230, 230, 231, 232, 232, 233, 233, 234, 234, 235, 235, 236, 236, 237,
    237, 238, 239, 239, 240, 240, 241, 241, 241, 242, 242, 243, 243, 244, 244,
    245, 245, 246, 246, 246, 247, 247, 248, 248, 249, 249, 249, 250, 250, 250,
    251, 251, 251, 252, 252, 252, 253, 253, 253, 254, 254, 254, 254, 255, 255,
    255,
};

/* One sample of the tone curve.  `v` is the value after the black level and
   after white balance; the lookup puts the pedestal back, because the table's
   domain is the sample as the sensor delivered it.  Anything past the table
   saturates. */
static inline uint8_t gamma22(uint32_t v)
{
    const uint32_t index = v + PIPE_GAMMA_BLACK;
    return kGamma22[index > 255u ? 255u : index];
}

#endif
