/*
 * cpu_engine.h - the CPU engine, and the reference for the DSP.
 *
 * It runs the same steps as the DSP kernel, one pixel at a time. It is both
 * the fallback when the CDSP is not available and the thing the DSP is
 * measured against. Its output surfaces are DMA-BUFs too, so the sink does
 * not care which engine produced the picture.
 */
#pragma once

#include <cstdint>
#include <vector>

#include "engine.h"

namespace pl {

class CpuEngine : public Engine {
public:
    CpuEngine(std::uint32_t width, std::uint32_t height, std::uint32_t scale,
              std::uint32_t filters, std::uint32_t strength,
              unsigned surface_count = 2, bool agc = false);
    ~CpuEngine() override;

    CpuEngine(const CpuEngine&) = delete;
    CpuEngine& operator=(const CpuEngine&) = delete;

    Surface* acquire() override;
    void release(Surface* surface) override;
    int process(const Frame& frame, Surface& out) override;
    void configure(std::uint32_t filters, std::uint32_t strength) override;
    std::size_t output_size() const override { return output_size_; }

    void set_exposure(bool on) override { agc_ = on; }
    bool agc_running() const override { return agc_; }
    bool exposure(std::uint32_t& shift, std::uint32_t& luma) override;

private:
    void render(const Frame& frame, std::uint8_t* out);

    /* The exposure loop, mirrored from dsp/agc.c: same constants, same integer
       steps, so the two engines apply the same gain to the same frame. */
    void agc_update(std::uint32_t luma);

    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::uint32_t scale_ = 1;
    std::uint32_t filters_ = 0;
    std::uint32_t strength_ = 2;
    bool agc_ = false;
    std::uint32_t shift_ = 0; /* stops of gain, applied while unpacking */
    std::uint32_t ema_ = 0;
    std::uint32_t ticks_ = 0;
    std::uint32_t luma_ = 0;
    OutputGeometry geometry_;
    std::size_t output_size_ = 0;

    std::vector<std::uint8_t> lanes_;   /* one 8-bit sample per pixel */
    std::vector<std::uint8_t> scratch_; /* ping-pong for neighbourhood work */
    std::vector<std::uint8_t> rows_;    /* scale full-resolution RGBA rows */
    std::vector<int> free_;             /* indices into surfaces_ */
    std::vector<Surface> surfaces_;
    std::vector<void*> blocks_;
};

} // namespace pl
