/*
 * sensor.h - the camera controls.
 *
 * Before any image processing, how bright a frame is depends on two things:
 * how long the sensor collects light (exposure) and how much it amplifies
 * (gain). Those are the only controls this class moves. Everything else about
 * the frame is the kernel's business.
 *
 * The driver clamps every value, so after each write we read the value back.
 * Exposure cannot go past the frame length, which is the mode height plus the
 * vertical blanking. On this board the driver stops at 1703 lines in the small
 * mode and at 3522 in the large one, and only the read-back tells us which
 * happened.
 */
#pragma once

#include <cstdint>
#include <string>

namespace pl {

class Sensor {
public:
    /* `node` is the sensor's subdev (/dev/v4l-subdevN), the last entity of the
       routed path. */
    explicit Sensor(const std::string& node);
    ~Sensor();

    Sensor(const Sensor&) = delete;
    Sensor& operator=(const Sensor&) = delete;

    /* Both light knobs were found.  A route without them still runs, on
       whatever the driver left behind. */
    bool ok() const { return fd_ >= 0 && exposure_.id != 0U && gain_.id != 0U; }
    const std::string& node() const { return node_; }

    std::uint32_t exposure() const { return wide(exposure_.value); }
    std::uint32_t exposure_max() const { return wide(exposure_.max); }
    std::uint32_t gain() const { return wide(gain_.value); }
    std::uint32_t gain_max() const { return wide(gain_.max); }

    /* Frame length in lines: height plus vertical blanking.  The exposure
       cannot pass it, so it is also the ceiling the exposure loop has to raise
       when it wants a longer integration. */
    std::uint32_t height() const { return height_; }
    std::uint32_t frame_length() const { return height_ + vblank(); }
    std::uint32_t vblank() const { return wide(vblank_.value); }
    std::uint32_t vblank_max() const { return wide(vblank_.max); }

    /* Each returns the value the driver actually took. */
    std::uint32_t set_exposure(std::uint32_t lines);
    std::uint32_t set_gain(std::uint32_t code);
    std::uint32_t set_vblank(std::uint32_t lines);

    /* The sensor's black-level correction: leave it at maximum. */
    void maximise_pixel_values();

    /* One line for the log: the node and what the controls read right now. */
    std::string detail() const;

private:
    struct Knob {
        std::uint32_t id = 0U;
        std::int32_t min = 0;
        std::int32_t max = 0;
        std::int32_t value = 0;
    };

    void discover();
    /* A control is asked for by its V4L2 id, not by name: the kernel reports
       "Exposure" while v4l2-ctl prints "exposure", and the ids are stable
       ABI while the names are not. */
    bool probe(Knob& knob, std::uint32_t id);
    /* Re-read a knob's range: the exposure maximum follows the frame length,
       so it changes whenever the blanking does. */
    void refresh(Knob& knob);
    std::int32_t put(Knob& knob, std::uint32_t value);

    static std::uint32_t wide(std::int32_t value)
    {
        return value < 0 ? 0U : static_cast<std::uint32_t>(value);
    }

    std::string node_;
    int fd_ = -1;
    Knob exposure_;
    Knob gain_;
    Knob vblank_;
    std::uint32_t pixel_values_ = 0U; /* how many were pushed to maximum */
    std::uint32_t height_ = 0U;
};

} // namespace pl