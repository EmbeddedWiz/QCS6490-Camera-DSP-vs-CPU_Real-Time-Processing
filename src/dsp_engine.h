/*
 * dsp_engine.h - the engine that runs on the CDSP.
 *
 * The host side is small on purpose: open one session, register the camera
 * buffer once, then ask the DSP to render each frame. There is no pixel loop
 * here and no staging buffer. The bytes the sensor wrote are the bytes the
 * DSP reads.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include "engine.h"

namespace pl {

class DspEngine : public Engine {
public:
    DspEngine(std::uint32_t width, std::uint32_t height, std::uint32_t scale,
              std::uint32_t filters, std::uint32_t strength,
              unsigned surface_count, bool raw8, bool agc);
    ~DspEngine() override;

    DspEngine(const DspEngine&) = delete;
    DspEngine& operator=(const DspEngine&) = delete;

    Surface* acquire() override;
    void release(Surface* surface) override;
    int process(const Frame& frame, Surface& out) override;
    void configure(std::uint32_t filters, std::uint32_t strength) override;
    std::size_t output_size() const override { return output_size_; }

    /* The exposure loop's state, read back from the DSP when the panel asks. */
    bool exposure(std::uint32_t& shift, std::uint32_t& luma) override;
    bool agc_running() const override { return agc_; }
    void set_exposure(bool on) override;

private:
    void open_session();
    void register_frame(const Frame& frame);

    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::uint32_t scale_ = 1;
    std::uint32_t filters_ = 0;
    std::uint32_t strength_ = 2;
    std::size_t output_size_ = 0;
    bool raw8_ = false;
    bool agc_ = false;
    std::uint64_t handle_ = 0;
    std::uint32_t agc_shift_ = 0;
    std::uint32_t agc_luma_ = 0;

    std::vector<Surface> surfaces_;
    std::vector<void*> blocks_;
    std::vector<int> free_;
    /* camera dma-buf fd -> (mapping, size); the mapping has to be handed back
       with the session, not left to the process exit */
    std::map<int, std::pair<void*, std::size_t>> registered_;
};

} // namespace pl
