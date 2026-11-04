/*
 * panel.cpp - a terminal in raw mode, one escape sequence per redraw.
 *
 * The panel only reads state and prints it. It never touches a frame and does
 * not allocate on the frame path. Keys come through the same epoll set as the
 * camera, so a keypress costs one wakeup.
 */
#include "panel.h"

#include <cstdio>
#include <cstring>

#include <termios.h>
#include <unistd.h>

namespace {

struct FilterSpec {
    const char* name;
    std::uint32_t bit;
};

const FilterSpec kFilters[] = {
    {"edge", 0x100U},   {"emboss", 0x200U},    {"posterize", 0x400U},
    {"invert", 0x800U}, {"pixelate", 0x1000U}, {"sketch", 0x2000U},
    {"sepia", 0x4000U}, {"sharpen", 0x8000U},
};

termios g_saved{};

} // namespace

namespace pl {

bool Panel::open()
{
    if (active_) {
        return true;
    }
    if (::isatty(STDIN_FILENO) != 1) {
        return false;
    }
    if (::tcgetattr(STDIN_FILENO, &g_saved) != 0) {
        return false;
    }
    termios raw = g_saved;
    raw.c_lflag &= static_cast<tcflag_t>(~(ECHO | ICANON));
    raw.c_iflag &= static_cast<tcflag_t>(~(IXON | ICRNL));
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (::tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
        return false;
    }
    active_ = true;
    std::fputs("\x1b[?25l\x1b[2J", stdout);
    std::fflush(stdout);
    return true;
}

void Panel::close()
{
    if (!active_) {
        return;
    }
    active_ = false;
    std::fputs("\x1b[?25h\x1b[0m\r\n", stdout);
    std::fflush(stdout);
    ::tcsetattr(STDIN_FILENO, TCSANOW, &g_saved);
}

void Panel::draw(const PanelState& state, bool help)
{
    char line[256];
    std::string out = "\x1b[H";
    auto put = [&out](const std::string& text) {
        out += text;
        out += "\x1b[K\r\n";
    };

    std::snprintf(line, sizeof line,
                  " pipeline   \x1b[1m%s\x1b[0m   sink %s   %ux%u %s",
                  state.engine.c_str(), state.sink.c_str(), state.width,
                  state.height, state.raw8 ? "RAW8" : "RAW10");
    put(line);
    put(" ----------------------------------------------------------------");
    std::snprintf(line, sizeof line, " fps %7.1f   peak %7.1f      frames %llu",
                  state.fps, state.peak,
                  static_cast<unsigned long long>(state.frames));
    put(line);
    std::snprintf(line, sizeof line, " engine %6.2f ms   max %6.2f ms",
                  state.lat, state.lat_max);
    put(line);
    std::snprintf(line, sizeof line, " stream %7.1f MB/s in   %7.1f MB/s out ",
                  state.mbps_in, state.mbps_out);
    put(line);
    std::snprintf(line, sizeof line, " scale 1:%u      window %ux%u",
                  state.scale, state.width / state.scale,
                  state.height / state.scale);
    put(line);
    if (state.agc) {
        std::snprintf(line, sizeof line, " agc gain x%u   luma %u",
                      state.agc_shift < 8U ? (1U << state.agc_shift) : 0U,
                      state.agc_luma);
    } else if (state.agc_shift > 0U) {
        std::snprintf(line, sizeof line, " agc off   gain x%u held",
                      state.agc_shift < 8U ? (1U << state.agc_shift) : 0U);
    } else {
        std::snprintf(line, sizeof line, " agc off");
    }
    put(line);
    put("");

    std::snprintf(line, sizeof line, " filters 0x%03x    strength %u (+/-)",
                  state.filters, state.strength);
    put(line);
    for (const FilterSpec& filter : kFilters) {
        std::snprintf(line, sizeof line, "   [%c] %-10s",
                      (state.filters & filter.bit) != 0U ? 'x' : ' ',
                      filter.name);
        put(line);
    }
    put("");
    put(state.note.empty() ? std::string() : " " + state.note);
    if (help) {
        put(" digits toggle filters   0 clear   +/- strength");
        put(" s scale   r resolution   e engine   a agc   q quit   h help");
    } else {
        put(" h help");
    }
    std::fwrite(out.data(), 1, out.size(), stdout);
    std::fflush(stdout);
}

PanelEvent Panel::read()
{
    PanelEvent event;
    if (!active_) {
        return event;
    }
    char buffer[32];
    const ssize_t got = ::read(STDIN_FILENO, buffer, sizeof buffer);
    if (got <= 0) {
        return event;
    }
    for (ssize_t i = 0; i < got; ++i) {
        const char key = buffer[i];
        if (key >= '1' && key <= '8') {
            const std::size_t index = static_cast<std::size_t>(key - '1');
            if (index < sizeof kFilters / sizeof kFilters[0]) {
                event.key = PanelKey::Filter;
                event.mask = kFilters[index].bit;
                return event;
            }
        }
        switch (key) {
        case '0':
            event.key = PanelKey::Clear;
            return event;
        case '+':
        case '=':
            event.key = PanelKey::Strength;
            event.delta = 1;
            return event;
        case '-':
        case '_':
            event.key = PanelKey::Strength;
            event.delta = -1;
            return event;
        case 'r':
        case 'R':
            event.key = PanelKey::Resolution;
            return event;
        case 's':
        case 'S':
            event.key = PanelKey::Scale;
            return event;
        case 'e':
        case 'E':
            event.key = PanelKey::Engine;
            return event;
        case 'a':
        case 'A':
            event.key = PanelKey::Agc;
            return event;
        case 'h':
        case 'H':
        case '?':
            event.key = PanelKey::Help;
            return event;
        case 'q':
        case 'Q':
            event.key = PanelKey::Quit;
            return event;
        default:
            break;
        }
    }
    return event;
}

} // namespace pl
