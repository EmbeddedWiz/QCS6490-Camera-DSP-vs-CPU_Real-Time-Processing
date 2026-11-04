/*
 * panel.h - the control panel in the terminal.
 *
 * Plain terminal in raw mode with a few escape sequences. No toolkit, and no
 * access to the frame, so the panel cannot slow the pipeline down.
 */
#pragma once

#include <cstdint>
#include <string>

namespace pl {

struct PanelState {
    std::string engine;
    std::string sink;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t filters = 0;
    std::uint32_t strength = 2;
    std::uint32_t scale = 1;
    bool raw8 = false;
    double fps = 0.0;
    double peak = 0.0;
    double lat = 0.0;
    double lat_max = 0.0;
    /* What the frame path carries, in megabytes per second: `in` is the raw
       Bayer the sensor hands over, `out` the RGBA surface the DSP writes.
       Both move with fps, which is why they belong on the same panel. */
    double mbps_in = 0.0;
    double mbps_out = 0.0;
    std::uint64_t frames = 0;
    bool agc = false;
    std::uint32_t agc_shift = 0;
    std::uint32_t agc_luma = 0;
    std::string note;
};

enum class PanelKey {
    None,
    Quit,
    Filter,
    Clear,
    Strength,
    Scale,
    Resolution,
    Engine,
    Agc,
    Help
};

struct PanelEvent {
    PanelKey key = PanelKey::None;
    std::uint32_t mask = 0;
    int delta = 0;
};

class Panel {
public:
    bool open();
    void close();
    void draw(const PanelState& state, bool help);
    PanelEvent read();

private:
    bool active_ = false;
};

} // namespace pl
