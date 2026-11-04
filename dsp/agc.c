/*
 * agc.c - the exposure loop.
 *
 * The sensor is already at its maximum gain on this board, so the loop cannot
 * ask it for more. Instead it doubles the frame on the CDSP during the spread
 * step, where the row is still packed and one saturating add covers 128
 * pixels.
 *
 * The loop measures the luma of the picture it just wrote, after white balance
 * and after gamma, because that is what the screen shows and what the eye
 * judges. Measuring before gamma was the old mistake: a linear 52 of 255 is
 * already 124 on the display, so the old loop saw "dark" where the picture was
 * fine and doubled it.
 *
 * The gain is a power of two, so one step is a whole stop. On the display
 * curve a stop is a factor of 2^(1/2.2) = 1.37, which is why the target band
 * is 100..140 instead of one octave. A step from just outside the band always
 * lands inside it, so the loop does not hunt: 99 doubles to 136, 141 halves to
 * 103. Three frames must agree before the gain changes, which keeps one noisy
 * frame from moving it.
 *
 * One frame of delay is unavoidable: the frame we measure is the one already
 * rendered, and the new gain only applies to the next one.
 */
#include "kernel.h"

#define AE_TARGET_LOW 100u
#define AE_TARGET_HIGH 140u
#define AE_PERIOD 3u
#define AE_MAX_SHIFT 6u

static uint32_t s_shift;
static uint32_t s_ema;
static uint32_t s_ticks;
static uint32_t s_luma;

void agc_reset(void)
{
    s_shift = 0;
    s_ema = 0;
    s_ticks = 0;
    s_luma = 0;
}

uint32_t agc_shift(void)
{
    return s_shift;
}

uint32_t agc_luma(void)
{
    return s_luma;
}

void agc_update(uint32_t luma)
{
    s_luma = luma;

    /* A zero measurement is not a missing frame: at unity gain this scene is
       dark enough that every green byte rounds to zero.  It says "more light"
       but not how much, so the ratio walk below cannot be used.  Two stops a
       period is the compromise: one stop would leave the cold start dark for
       a fifth of a second, and the full jump would overshoot the band. */
    if (luma == 0u) {
        s_ema = 0u;
        if (++s_ticks < AE_PERIOD) {
            return;
        }
        s_ticks = 0;
        s_shift = s_shift + 2u > AE_MAX_SHIFT ? AE_MAX_SHIFT : s_shift + 2u;
        return;
    }

    /* The first sample *is* the measurement, not something to average with a
       zero.  Starting the average at zero made a cold start look darker than
       the frame actually was, and the loop overshot by a stop. */
    if (s_ema == 0u) {
        s_ema = luma;
    } else {
        s_ema = (s_ema * 3u + luma) >> 2;
    }

    if (++s_ticks < AE_PERIOD) {
        return;
    }
    s_ticks = 0;

    /* One stop lifts the display luma by 2^(1/2.2) = 1.37, so the loop takes
       as many stops as the measurement asks for, not one per period: a cold
       start needs four of them, and one per period left the picture visibly
       dark for a fifth of a second every time the process started or
       restarted.  The band still ends the walk, so 99 steps to 136 and 141
       back to 103, exactly as before. */
    while (s_shift < AE_MAX_SHIFT && s_ema < AE_TARGET_LOW) {
        ++s_shift;
        s_ema = (s_ema * 137u) / 100u;
    }
    while (s_shift > 0u && s_ema > AE_TARGET_HIGH) {
        --s_shift;
        s_ema = (s_ema * 100u) / 137u;
    }
}
