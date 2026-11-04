/*
 * engine.h - what the pipeline asks an engine to do.
 *
 * An engine takes one frame and writes the result into a surface it already
 * owns. It does not allocate and does not copy. There are two of them, the
 * DSP and the CPU, and they must produce the same bytes.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "frame.h"

namespace pl {

/* Filter bits. Same values as the panel and the command line. */
constexpr std::uint32_t kInvert = 0x800U;
constexpr std::uint32_t kPosterize = 0x400U;
constexpr std::uint32_t kEmboss = 0x200U;
constexpr std::uint32_t kEdge = 0x100U;
constexpr std::uint32_t kPixelate = 0x1000U;
constexpr std::uint32_t kSketch = 0x2000U;
constexpr std::uint32_t kSepia = 0x4000U;
constexpr std::uint32_t kSharpen = 0x8000U;

/* Every filter, in the order the engines apply them. */
constexpr std::uint32_t kAllFilters = kSharpen | kPixelate | kEdge | kSketch |
                                      kEmboss | kSepia | kPosterize | kInvert;

/* Window scale: 1 keeps the full picture, 6 shows one sixth of it. Both
   engines and the DSP kernel work out the surface from this number. */
constexpr std::uint32_t kMaxScale = 6U;

/* The widest frame the kernel accepts. The real limit is PIPE_MAX_W in
   dsp/hvx.h; this copy is only here to produce a readable error. */
constexpr std::uint32_t kMaxWidth = 3328U;

/*
 * The shape of the surface an engine writes.
 *
 * Width is rounded down to a whole number of 32-pixel vectors, because that is
 * the unit the DSP works in. Rounding down keeps stride == width * 4 exactly.
 * With a wider stride, a sink that takes the stride from its own caps would
 * start each row a little further left than the one above, and the picture
 * would come out as diagonal stripes.
 *
 * Rounding down also keeps the box filter inside the frame (out_w * scale <=
 * width), so no pass reads past the raw row. The price is up to 31 columns cut
 * from the right, at every scale, 1:1 included.
 */
struct OutputGeometry {
    std::uint32_t width = 0;  /* visible pixels per row, a multiple of 32 */
    std::uint32_t height = 0; /* rows */
    std::uint32_t lanes = 0;  /* pixels per row on the surface == width */
    std::size_t stride = 0;   /* bytes per row == width * 4 */
    std::size_t size = 0;     /* whole surface, plus a vector of slack */
};

inline OutputGeometry output_geometry(std::uint32_t width, std::uint32_t height,
                                      std::uint32_t scale)
{
    OutputGeometry geometry;
    /* The same expression as out_width() in dsp/kernel.h: host and kernel must
       agree on the surface without guessing. */
    geometry.width = (width / scale) & ~31U;
    geometry.height = height / scale;
    geometry.lanes = geometry.width;
    geometry.stride = static_cast<std::size_t>(geometry.width) * 4U;
    /* The last row's neighbourhood pass reads one vector past the row. */
    geometry.size = geometry.stride * geometry.height + 128U;
    return geometry;
}

class Engine {
public:
    virtual ~Engine() = default;

    /* A free surface, owned by the engine until release(). */
    virtual Surface* acquire() = 0;
    virtual void release(Surface* surface) = 0;

    /* Returns 0 on success. The frame stays owned by the caller. */
    virtual int process(const Frame& frame, Surface& out) = 0;

    virtual void configure(std::uint32_t filters, std::uint32_t strength) = 0;
    virtual std::size_t output_size() const = 0;

    /*
     * Turn the exposure loop on or off. Nothing else changes: the camera, the
     * surfaces and the DSP session stay as they are, so the panel can flip it
     * without a restart. Turning it off freezes the gain; it does not reset it.
     */
    virtual void set_exposure(bool on) { (void)on; }

    /* True while the loop is adapting. The gain is applied either way. */
    virtual bool agc_running() const { return false; }

    /*
     * What the loop settled on, for the panel. False means the engine has no
     * loop. Both engines here do: the DSP runs dsp/agc.c, and the CPU version
     * repeats it step for step, so a reference run applies the same gain and
     * the parity check still means something. With the loop off these are the
     * values that are still being applied.
     */
    virtual bool exposure(std::uint32_t& shift, std::uint32_t& luma)
    {
        (void)shift;
        (void)luma;
        return false;
    }
};

} // namespace pl
