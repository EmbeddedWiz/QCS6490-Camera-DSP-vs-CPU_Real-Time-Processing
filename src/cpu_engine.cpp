/*
 * cpu_engine.cpp - the same arithmetic as the DSP, one pixel at a time.
 *
 * Every vector step has a scalar twin here, and the twin has to produce the
 * same integer. That is what makes --parity worth running. The picture is
 * built in the surface the sink will import, so this path copies nothing
 * either.
 */
#include "cpu_engine.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <stdexcept>

#include <fastrpc/rpcmem.h>

#include "../dsp/gamma.h"

namespace {

/* The sensor's black pedestal in these eight bits; the DSP's PIPE_BLACK. */
constexpr unsigned kBlack = 16U;

/* White balance, the DSP's wb_gain(): (v * m) >> 8, saturating, m = gain*256.
 */
constexpr unsigned kRedGain = 319U;
constexpr unsigned kBlueGain = 556U;

inline int clamp_coord(int value, int limit)
{
    if (value < 0) {
        return 0;
    }
    if (value >= limit) {
        return limit - 1;
    }
    return value;
}

inline std::uint8_t sample(const std::uint8_t* lanes, int width, int height,
                           int x, int y)
{
    return lanes[clamp_coord(y, height) * width + clamp_coord(x, width)];
}

/* The DSP's neighbour estimates, byte for byte.
 *
 * Two details decide whether the two engines agree to the last level, and both
 * were measured on the board rather than assumed:
 *
 *   - the CDSP's hardware average (Q6_Vub_vavg_VubVub) *truncates*: avg(0, 1)
 *     comes back 0, not 1.  Rounding here made the first column of a ramp
 *     differ by up to 18 display levels while the whole interior matched.
 *   - a 2x2 estimate is the average of two averages, not one sum of four:
 *     the kernel computes avg(avg(a,b), avg(c,d)), which is not the same
 *     integer as (a+b+c+d+2)>>2 even when both truncate.
 *
 * The DSP is the shipped path and the CPU is the check, so the check copies
 * the hardware, not the other way round. */
inline std::uint8_t average2(unsigned a, unsigned b)
{
    return static_cast<std::uint8_t>((a + b) >> 1);
}

inline std::uint8_t average4(unsigned a, unsigned b, unsigned c, unsigned d)
{
    return average2(average2(a, b), average2(c, d));
}

inline std::uint8_t saturate(int value)
{
    return static_cast<std::uint8_t>(value < 0 ? 0
                                               : (value > 255 ? 255 : value));
}

/* The surface rows are a whole number of 32-pixel vectors wide; the lanes
   past the visible width repeat the last pixel so a neighbourhood pass sees
   the same edge the DSP's padded rows do. */
inline void pad_row(std::uint8_t* row, std::uint32_t width, std::uint32_t lanes)
{
    if (lanes <= width) {
        return;
    }
    auto* words = reinterpret_cast<std::uint32_t*>(row);
    const std::uint32_t last = words[width - 1U];
    for (std::uint32_t x = width; x < lanes; ++x) {
        words[x] = last;
    }
}

/* Packed RAW10 keeps the top eight bits in one byte per pixel, so truncating
   to 8 bits is the byte itself; the low two bits only matter for 10-bit
   output, which this pipeline does not produce. */
void unpack8(const std::uint8_t* src, std::uint32_t width, std::uint8_t* dst,
             std::uint32_t shift)
{
    for (std::uint32_t x = 0; x < width; ++x) {
        const unsigned value = src[x];
        unsigned sample = value > kBlack ? value - kBlack : 0U;
        /* AGC after black, before the demosaic: the pedestal is a property of
           the sensor and must not be amplified with the light.  A power of two,
           so the scalar path is a shift and a clamp, exactly the saturating
           doubling the DSP does on the packed row. */
        sample <<= shift;
        dst[x] = static_cast<std::uint8_t>(sample > 255U ? 255U : sample);
    }
}

/* Packed RAW10 stores four pixels in five bytes: the top eight bits of pixel
   k live at byte k*5/4, the fifth byte carries the two low bits.  The pipeline
   works in 8 bits, and truncating 10 -> 8 drops those low bits, so the byte
   itself is the sample. */
void unpack10(const std::uint8_t* src, std::uint32_t width, std::uint8_t* dst,
              std::uint32_t shift)
{
    for (std::uint32_t x = 0; x < width; ++x) {
        const unsigned value = src[(static_cast<std::size_t>(x) * 5U) / 4U];
        unsigned sample = value > kBlack ? value - kBlack : 0U;
        sample <<= shift;
        dst[x] = static_cast<std::uint8_t>(sample > 255U ? 255U : sample);
    }
}

/* Bilinear demosaic for an RGGB pattern, one pass over the frame. */
void demosaic_row(const std::uint8_t* lanes, int width, int height, int y,
                  std::uint8_t* out)
{
    const bool even_y = (y & 1) == 0;
    for (int x = 0; x < width; ++x) {
        const bool even_x = (x & 1) == 0;
        const unsigned centre = sample(lanes, width, height, x, y);
        unsigned r = 0;
        unsigned g = 0;
        unsigned b = 0;

        if (even_y && even_x) { /* R */
            r = centre;
            g = average4(sample(lanes, width, height, x - 1, y),
                         sample(lanes, width, height, x + 1, y),
                         sample(lanes, width, height, x, y - 1),
                         sample(lanes, width, height, x, y + 1));
            b = average4(sample(lanes, width, height, x - 1, y - 1),
                         sample(lanes, width, height, x + 1, y - 1),
                         sample(lanes, width, height, x - 1, y + 1),
                         sample(lanes, width, height, x + 1, y + 1));
        } else if (even_y) { /* Gr */
            r = average2(sample(lanes, width, height, x - 1, y),
                         sample(lanes, width, height, x + 1, y));
            g = centre;
            b = average2(sample(lanes, width, height, x, y - 1),
                         sample(lanes, width, height, x, y + 1));
        } else if (even_x) { /* Gb */
            r = average2(sample(lanes, width, height, x, y - 1),
                         sample(lanes, width, height, x, y + 1));
            g = centre;
            b = average2(sample(lanes, width, height, x - 1, y),
                         sample(lanes, width, height, x + 1, y));
        } else { /* B */
            b = centre;
            g = average4(sample(lanes, width, height, x - 1, y),
                         sample(lanes, width, height, x + 1, y),
                         sample(lanes, width, height, x, y - 1),
                         sample(lanes, width, height, x, y + 1));
            r = average4(sample(lanes, width, height, x - 1, y - 1),
                         sample(lanes, width, height, x + 1, y - 1),
                         sample(lanes, width, height, x - 1, y + 1),
                         sample(lanes, width, height, x + 1, y + 1));
        }

        /* Fixed white balance, the same fixed point the DSP uses. */
        r = (r * kRedGain) >> 8;
        b = (b * kBlueGain) >> 8;

        std::uint8_t* pixel = out + static_cast<std::size_t>(x) * 4U;
        pixel[0] = saturate(static_cast<int>(r));
        pixel[1] = saturate(static_cast<int>(g));
        pixel[2] = saturate(static_cast<int>(b));
        pixel[3] = 255;
    }
}

/* Display encoding, exactly the DSP's gamma_row: after the window average,
   never before it. */
void gamma_row(std::uint8_t* row, std::uint32_t width)
{
    for (std::uint32_t x = 0; x < width; ++x) {
        std::uint8_t* pixel = row + static_cast<std::size_t>(x) * 4U;
        pixel[0] = gamma22(pixel[0]);
        pixel[1] = gamma22(pixel[1]);
        pixel[2] = gamma22(pixel[2]);
        pixel[3] = 255U;
    }
}

inline int luma(const std::uint8_t* pixel)
{
    return (pixel[0] * 77 + pixel[1] * 150 + pixel[2] * 29) >> 8;
}

void sobel_pass(const std::uint8_t* src, std::uint8_t* dst, int width,
                int height, std::uint32_t strength, bool sketch)
{
    const int gain = static_cast<int>(strength);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int xm = clamp_coord(x - 1, width);
            const int xp = clamp_coord(x + 1, width);
            const int ym = clamp_coord(y - 1, height);
            const int yp = clamp_coord(y + 1, height);
            const auto at = [&](int px, int py) {
                return luma(src +
                            (static_cast<std::size_t>(py) * width + px) * 4U);
            };
            const int gx = -at(xm, ym) - 2 * at(xm, y) - at(xm, yp) +
                           at(xp, ym) + 2 * at(xp, y) + at(xp, yp);
            const int gy = -at(xm, ym) - 2 * at(x, ym) - at(xp, ym) +
                           at(xm, yp) + 2 * at(x, yp) + at(xp, yp);
            const int magnitude =
                ((gx < 0 ? -gx : gx) + (gy < 0 ? -gy : gy)) * gain / 4;
            /* The sketch is the same magnitude on white: the picture becomes
               the paper and the edges become the pencil. */
            const int value =
                sketch ? 255 - std::min(magnitude, 255) : magnitude;
            const std::uint8_t grey = saturate(value);
            std::uint8_t* pixel =
                dst + (static_cast<std::size_t>(y) * width + x) * 4U;
            pixel[0] = grey;
            pixel[1] = grey;
            pixel[2] = grey;
            pixel[3] = 255;
        }
    }
}

void edge_pass(const std::uint8_t* src, std::uint8_t* dst, int width,
               int height, std::uint32_t strength)
{
    sobel_pass(src, dst, width, height, strength, false);
}

void sketch_pass(const std::uint8_t* src, std::uint8_t* dst, int width,
                 int height, std::uint32_t strength)
{
    sobel_pass(src, dst, width, height, strength, true);
}

void emboss_pass(const std::uint8_t* src, std::uint8_t* dst, int width,
                 int height, std::uint32_t strength)
{
    const int gain = static_cast<int>(strength);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int xm = clamp_coord(x - 1, width);
            const int ym = clamp_coord(y - 1, height);
            const int here =
                luma(src + (static_cast<std::size_t>(y) * width + x) * 4U);
            const int corner =
                luma(src + (static_cast<std::size_t>(ym) * width + xm) * 4U);
            const int value = 128 + (here - corner) * gain;
            const std::uint8_t grey = saturate(value);
            std::uint8_t* pixel =
                dst + (static_cast<std::size_t>(y) * width + x) * 4U;
            pixel[0] = grey;
            pixel[1] = grey;
            pixel[2] = grey;
            pixel[3] = 255;
        }
    }
}

void sharpen_pass(const std::uint8_t* src, std::uint8_t* dst, int width,
                  int height, std::uint32_t strength)
{
    const int gain = static_cast<int>(strength);
    const int back = (255 * gain) >> 1;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int xm = clamp_coord(x - 1, width);
            const int xp = clamp_coord(x + 1, width);
            const int ym = clamp_coord(y - 1, height);
            const int yp = clamp_coord(y + 1, height);
            const auto at = [&](int px, int py) {
                return luma(src +
                            (static_cast<std::size_t>(py) * width + px) * 4U);
            };
            const int horizontal = (at(xm, y) + at(xp, y) + 1) >> 1;
            const int vertical = (at(x, ym) + at(x, yp) + 1) >> 1;
            const int blur = (horizontal + vertical + 1) >> 1;
            const int detail = at(x, y) - blur + 255;
            const int scaled = ((detail * gain) >> 1) - back;
            const std::uint8_t* in =
                src + (static_cast<std::size_t>(y) * width + x) * 4U;
            std::uint8_t* out =
                dst + (static_cast<std::size_t>(y) * width + x) * 4U;
            out[0] = saturate(in[0] + scaled);
            out[1] = saturate(in[1] + scaled);
            out[2] = saturate(in[2] + scaled);
            out[3] = 255;
        }
    }
}

void sepia_pass(const std::uint8_t* src, std::uint8_t* dst, int width,
                int height, std::uint32_t strength)
{
    const unsigned keep = 6U - strength;
    const std::size_t pixels = static_cast<std::size_t>(width) * height;
    for (std::size_t i = 0; i < pixels; ++i) {
        const std::uint8_t* in = src + i * 4U;
        std::uint8_t* out = dst + i * 4U;
        const unsigned l = static_cast<unsigned>(luma(in));
        const unsigned tone[3] = {std::min((l * 307U) >> 8, 255U),
                                  std::min((l * 243U) >> 8, 255U),
                                  std::min((l * 179U) >> 8, 255U)};
        for (int c = 0; c < 3; ++c) {
            const unsigned mixed =
                (static_cast<unsigned>(in[c]) * keep + tone[c] * strength) *
                10923U;
            out[c] = static_cast<std::uint8_t>(mixed >> 16);
        }
        out[3] = 255;
    }
}

/* Blocks of n x n become their average.  Two divisions by n, exactly like the
   DSP: the horizontal sum first, then the sum of n such rows. */
void pixelate_pass(const std::uint8_t* src, std::uint8_t* dst, int width,
                   int height, std::uint32_t strength)
{
    const int n = 1 << static_cast<int>(strength);
    std::vector<std::uint32_t> sums(static_cast<std::size_t>(width) * 3U);
    std::vector<std::uint32_t> block(static_cast<std::size_t>(width) * 3U);

    for (int by = 0; by < height; by += n) {
        std::fill(sums.begin(), sums.end(), 0U);
        for (int k = 0; k < n; ++k) {
            const int y = std::min(by + k, height - 1);
            const std::uint8_t* row =
                src + static_cast<std::size_t>(y) * width * 4U;
            for (int b = 0; b < width; b += n) {
                unsigned sum[3] = {0U, 0U, 0U};
                for (int j = 0; j < n; ++j) {
                    const int x = std::min(b + j, width - 1);
                    for (int c = 0; c < 3; ++c) {
                        sum[c] += row[static_cast<std::size_t>(x) * 4U +
                                      static_cast<std::size_t>(c)];
                    }
                }
                for (int c = 0; c < 3; ++c) {
                    sums[static_cast<std::size_t>(b) * 3U +
                         static_cast<std::size_t>(c)] +=
                        sum[c] / static_cast<unsigned>(n);
                }
            }
        }
        for (int b = 0; b < width; b += n) {
            for (int c = 0; c < 3; ++c) {
                block[static_cast<std::size_t>(b) * 3U +
                      static_cast<std::size_t>(c)] =
                    sums[static_cast<std::size_t>(b) * 3U +
                         static_cast<std::size_t>(c)] /
                    static_cast<unsigned>(n);
            }
        }
        const int last = std::min(by + n, height);
        for (int y = by; y < last; ++y) {
            std::uint8_t* row = dst + static_cast<std::size_t>(y) * width * 4U;
            for (int x = 0; x < width; ++x) {
                const int b = (x / n) * n;
                for (int c = 0; c < 3; ++c) {
                    row[static_cast<std::size_t>(x) * 4U +
                        static_cast<std::size_t>(c)] =
                        static_cast<std::uint8_t>(
                            block[static_cast<std::size_t>(b) * 3U +
                                  static_cast<std::size_t>(c)]);
                }
                row[static_cast<std::size_t>(x) * 4U + 3U] = 255U;
            }
        }
    }
}

void posterize_pass(const std::uint8_t* src, std::uint8_t* dst, int width,
                    int height, std::uint32_t strength)
{
    const unsigned levels = std::min(strength + 1U, 7U);
    const unsigned step = 256U / levels;
    const std::size_t pixels = static_cast<std::size_t>(width) * height;
    for (std::size_t i = 0; i < pixels; ++i) {
        const std::uint8_t* in = src + i * 4U;
        std::uint8_t* out = dst + i * 4U;
        for (int c = 0; c < 3; ++c) {
            out[c] = static_cast<std::uint8_t>(
                std::min((in[c] / step) * step + step / 2U, 255U));
        }
        out[3] = 255;
    }
}

void invert_pass(const std::uint8_t* src, std::uint8_t* dst, int width,
                 int height)
{
    const std::size_t pixels = static_cast<std::size_t>(width) * height;
    for (std::size_t i = 0; i < pixels; ++i) {
        const std::uint8_t* in = src + i * 4U;
        std::uint8_t* out = dst + i * 4U;
        out[0] = static_cast<std::uint8_t>(255U - in[0]);
        out[1] = static_cast<std::uint8_t>(255U - in[1]);
        out[2] = static_cast<std::uint8_t>(255U - in[2]);
        out[3] = 255;
    }
}

std::uint32_t pass_count(std::uint32_t filters)
{
    std::uint32_t count = 0;
    if ((filters & pl::kSharpen) != 0U) {
        ++count;
    }
    if ((filters & pl::kPixelate) != 0U) {
        ++count;
    }
    if ((filters & pl::kEdge) != 0U) {
        ++count;
    }
    if ((filters & pl::kSketch) != 0U) {
        ++count;
    }
    if ((filters & pl::kEmboss) != 0U) {
        ++count;
    }
    if ((filters & pl::kSepia) != 0U) {
        ++count;
    }
    if ((filters & pl::kPosterize) != 0U) {
        ++count;
    }
    if ((filters & pl::kInvert) != 0U) {
        ++count;
    }
    return count;
}

} // namespace

namespace pl {

CpuEngine::CpuEngine(std::uint32_t width, std::uint32_t height,
                     std::uint32_t scale, std::uint32_t filters,
                     std::uint32_t strength, unsigned surface_count, bool agc)
    : width_(width), height_(height), scale_(scale), filters_(filters),
      strength_(strength), agc_(agc)
{
    geometry_ = output_geometry(width, height, scale);
    output_size_ = geometry_.size;
    lanes_.resize(static_cast<std::size_t>(width) * height);
    scratch_.resize(output_size_);
    rows_.resize(static_cast<std::size_t>(width) * 4U * scale);

    surfaces_.resize(surface_count);
    blocks_.resize(surface_count);
    for (unsigned i = 0; i < surface_count; ++i) {
        void* block = rpcmem_alloc(RPCMEM_HEAP_ID_SYSTEM, RPCMEM_FLAG_CACHED,
                                   static_cast<int>(output_size_));
        if (block == nullptr) {
            throw std::runtime_error("rpcmem_alloc for the output surface");
        }
        blocks_[i] = block;
        surfaces_[i].data = block;
        surfaces_[i].size = output_size_;
        surfaces_[i].fd = rpcmem_to_fd(block);
        free_.push_back(static_cast<int>(i));
    }
}

CpuEngine::~CpuEngine()
{
    for (void* block : blocks_) {
        if (block != nullptr) {
            rpcmem_free(block);
        }
    }
}

Surface* CpuEngine::acquire()
{
    if (free_.empty()) {
        return nullptr;
    }
    const int index = free_.back();
    free_.pop_back();
    return &surfaces_[static_cast<std::size_t>(index)];
}

void CpuEngine::release(Surface* surface)
{
    if (surface == nullptr) {
        return;
    }
    free_.push_back(static_cast<int>(surface - surfaces_.data()));
}

void CpuEngine::configure(std::uint32_t filters, std::uint32_t strength)
{
    filters_ = filters;
    strength_ = strength;
}

int CpuEngine::process(const Frame& frame, Surface& out)
{
    if (frame.width != width_ || frame.height != height_) {
        return -1;
    }
    render(frame, static_cast<std::uint8_t*>(out.data));
    return 0;
}

void CpuEngine::render(const Frame& frame, std::uint8_t* out)
{
    const auto width = static_cast<int>(width_);
    const auto height = static_cast<int>(height_);
    const std::uint32_t out_w = geometry_.width;
    const std::uint32_t out_h = geometry_.height;
    const std::uint32_t lanes = geometry_.lanes;
    const std::size_t stride = geometry_.stride;

    for (int y = 0; y < height; ++y) {
        const std::uint8_t* src = static_cast<const std::uint8_t*>(frame.data) +
                                  static_cast<std::size_t>(y) * frame.stride;
        std::uint8_t* dst = lanes_.data() + static_cast<std::size_t>(y) * width;
        if (frame.stride * 4U < static_cast<std::size_t>(width) * 5U) {
            unpack8(src, width_, dst, shift_);
        } else {
            unpack10(src, width_, dst, shift_);
        }
    }

    /* The loop measures the green of the frame it just wrote, after gamma and
       before any filter, the same sample dsp/demosaic.c sums. */
    std::uint32_t green = 0U;

    /* The demosaic lands in whichever buffer makes the last pass end in the
       caller's surface, so no closing copy is ever needed. */
    const std::uint32_t passes = pass_count(filters_);
    std::uint8_t* first = (passes % 2U == 0U) ? out : scratch_.data();

    if (scale_ == 1U) {
        for (std::uint32_t y = 0; y < out_h; ++y) {
            std::uint8_t* row = first + y * stride;
            demosaic_row(lanes_.data(), width, height, static_cast<int>(y),
                         row);
            gamma_row(row, out_w);
            for (std::uint32_t x = 0; x < out_w; ++x) {
                green += row[static_cast<std::size_t>(x) * 4U + 1U];
            }
            pad_row(row, out_w, lanes);
        }
    } else {
        /* scale full-resolution rows at a time, then one box average each:
           the same block average the DSP computes, so a 1:2 window from
           either engine is the same picture. */
        const std::uint32_t divisor = scale_ * scale_;
        const std::size_t raw_row = static_cast<std::size_t>(width) * 4U;
        for (std::uint32_t oy = 0; oy < out_h; ++oy) {
            for (std::uint32_t k = 0; k < scale_; ++k) {
                demosaic_row(lanes_.data(), width, height,
                             static_cast<int>(oy * scale_ + k),
                             rows_.data() + k * raw_row);
            }
            std::uint8_t* dst = first + oy * stride;
            for (std::uint32_t ox = 0; ox < out_w; ++ox) {
                const std::size_t base = static_cast<std::size_t>(ox) * scale_;
                for (int c = 0; c < 3; ++c) {
                    unsigned sum = 0;
                    for (std::uint32_t ky = 0; ky < scale_; ++ky) {
                        const std::uint8_t* row = rows_.data() + ky * raw_row;
                        for (std::uint32_t kx = 0; kx < scale_; ++kx) {
                            sum += row[(base + kx) * 4U +
                                       static_cast<std::size_t>(c)];
                        }
                    }
                    dst[static_cast<std::size_t>(ox) * 4U +
                        static_cast<std::size_t>(c)] =
                        static_cast<std::uint8_t>(sum / divisor);
                }
                dst[static_cast<std::size_t>(ox) * 4U + 3U] = 255U;
            }
            gamma_row(dst, out_w);
            for (std::uint32_t x = 0; x < out_w; ++x) {
                green += dst[static_cast<std::size_t>(x) * 4U + 1U];
            }
            pad_row(dst, out_w, lanes);
        }
    }

    if (agc_) {
        agc_update(green / (out_w * out_h));
    }

    std::uint8_t* current = first;
    std::uint8_t* other = (first == out) ? scratch_.data() : out;
    auto swap = [&current, &other]() {
        std::uint8_t* hold = current;
        current = other;
        other = hold;
    };

    /* The order here is the order in dsp/pipe_hvx_impl.c: a change has to be
       made in both places or the two engines stop agreeing. */
    if ((filters_ & kSharpen) != 0U) {
        sharpen_pass(current, other, static_cast<int>(lanes),
                     static_cast<int>(out_h), strength_);
        swap();
    }
    if ((filters_ & kPixelate) != 0U) {
        pixelate_pass(current, other, static_cast<int>(lanes),
                      static_cast<int>(out_h), strength_);
        swap();
    }
    if ((filters_ & kEdge) != 0U) {
        edge_pass(current, other, static_cast<int>(lanes),
                  static_cast<int>(out_h), strength_);
        swap();
    }
    if ((filters_ & kSketch) != 0U) {
        sketch_pass(current, other, static_cast<int>(lanes),
                    static_cast<int>(out_h), strength_);
        swap();
    }
    if ((filters_ & kEmboss) != 0U) {
        emboss_pass(current, other, static_cast<int>(lanes),
                    static_cast<int>(out_h), strength_);
        swap();
    }
    if ((filters_ & kSepia) != 0U) {
        sepia_pass(current, other, static_cast<int>(lanes),
                   static_cast<int>(out_h), strength_);
        swap();
    }
    if ((filters_ & kPosterize) != 0U) {
        posterize_pass(current, other, static_cast<int>(lanes),
                       static_cast<int>(out_h), strength_);
        swap();
    }
    if ((filters_ & kInvert) != 0U) {
        invert_pass(current, other, static_cast<int>(lanes),
                    static_cast<int>(out_h));
        swap();
    }

    /* The parity above is what makes this hold; a mismatch would be a bug in
       pass_count(), not something to paper over with a copy. */
    assert(current == out);
    (void)output_size_;
}

/* The loop from dsp/agc.c, step for step.  Kept as a copy rather than a shared
   header because the reference engine is a second, independent implementation
   of the kernel: if the two ever disagree, the parity check has to see it. */
namespace {
constexpr std::uint32_t kAeLow = 100U;
constexpr std::uint32_t kAeHigh = 140U;
constexpr std::uint32_t kAePeriod = 3U;
constexpr std::uint32_t kAeMaxShift = 6U;
} // namespace

void CpuEngine::agc_update(std::uint32_t luma)
{
    luma_ = luma;

    /* Zero says "more light" but not how much, so the ratio walk cannot be
       used; two stops a period is the compromise. */
    if (luma == 0U) {
        ema_ = 0U;
        if (++ticks_ < kAePeriod) {
            return;
        }
        ticks_ = 0;
        shift_ = shift_ + 2U > kAeMaxShift ? kAeMaxShift : shift_ + 2U;
        return;
    }

    if (ema_ == 0U) {
        ema_ = luma;
    } else {
        ema_ = (ema_ * 3U + luma) >> 2;
    }

    if (++ticks_ < kAePeriod) {
        return;
    }
    ticks_ = 0;

    /* One stop lifts display luma by 2^(1/2.2) = 1.37. */
    while (shift_ < kAeMaxShift && ema_ < kAeLow) {
        ++shift_;
        ema_ = (ema_ * 137U) / 100U;
    }
    while (shift_ > 0U && ema_ > kAeHigh) {
        --shift_;
        ema_ = (ema_ * 100U) / 137U;
    }
}

bool CpuEngine::exposure(std::uint32_t& shift, std::uint32_t& luma)
{
    shift = shift_;
    luma = luma_;
    return true;
}

} // namespace pl
