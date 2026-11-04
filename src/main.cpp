/*
 * main.cpp - capture, process, show.
 *
 * One epoll set waits for everything: the camera node, which becomes readable
 * when the CSI DMA-done interrupt fires, and stdin when the panel is up. No
 * sleep, no polling loop, and no thread in the frame path.
 */
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <linux/dma-buf.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fastrpc/rpcmem.h>

#include "cpu_engine.h"
#include "dsp_engine.h"
#include "metrics.h"
#include "panel.h"
#include "route.h"
#include "sensor.h"
#include "sink.h"
#include "v4l2_capture.h"

namespace {

volatile std::sig_atomic_t g_stop = 0;

void on_signal(int)
{
    g_stop = 1;
}

std::uint32_t parse_filters(const std::string& value)
{
    if (value.empty() || value == "none") {
        return 0U;
    }
    if (value == "all") {
        return pl::kAllFilters;
    }
    std::uint32_t mask = 0U;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t comma = value.find(',', start);
        const std::string name = value.substr(start, comma - start);
        if (name == "edge") {
            mask |= pl::kEdge;
        } else if (name == "emboss") {
            mask |= pl::kEmboss;
        } else if (name == "posterize") {
            mask |= pl::kPosterize;
        } else if (name == "invert") {
            mask |= pl::kInvert;
        } else if (name == "pixelate") {
            mask |= pl::kPixelate;
        } else if (name == "sketch") {
            mask |= pl::kSketch;
        } else if (name == "sepia") {
            mask |= pl::kSepia;
        } else if (name == "sharpen") {
            mask |= pl::kSharpen;
        } else {
            throw std::invalid_argument("unknown filter: " + name);
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1U;
    }
    return mask;
}

/* The other direction, for a restart: a fresh process gets the same filters
   it had, spelled the way the command line spells them. */
std::string filter_names(std::uint32_t mask)
{
    std::string out;
    auto add = [&out, mask](const char* name, std::uint32_t bit) {
        if ((mask & bit) == 0U) {
            return;
        }
        if (!out.empty()) {
            out += ',';
        }
        out += name;
    };
    add("edge", pl::kEdge);
    add("emboss", pl::kEmboss);
    add("posterize", pl::kPosterize);
    add("invert", pl::kInvert);
    add("pixelate", pl::kPixelate);
    add("sketch", pl::kSketch);
    add("sepia", pl::kSepia);
    add("sharpen", pl::kSharpen);
    return out.empty() ? std::string("none") : out;
}

/* The modes the imx219 driver on this board actually offers: anything else is
   snapped to the nearest of these by the sensor.  The panel cycles this list
   instead of a made-up one, so every step is a real mode. */
struct Mode {
    std::uint32_t width;
    std::uint32_t height;
};

const Mode kModes[] = {
    {640U, 480U},
    {1640U, 1232U},
    {1920U, 1080U},
    {3280U, 2464U},
};

Mode next_mode(std::uint32_t width, std::uint32_t height)
{
    constexpr std::size_t count = sizeof kModes / sizeof kModes[0];
    for (std::size_t i = 0; i < count; ++i) {
        if (kModes[i].width == width && kModes[i].height == height) {
            return kModes[(i + 1U) % count];
        }
    }
    /* The sensor snapped to something off this list: start from the top. */
    return kModes[0];
}

/* One place that decides which engine runs, so the live path and the replay
   path cannot drift apart. */
std::unique_ptr<pl::Engine>
make_engine(const std::string& name, std::uint32_t width, std::uint32_t height,
            std::uint32_t scale, std::uint32_t filters, std::uint32_t strength,
            unsigned surfaces, bool raw8, bool agc)
{
    if (name == "dsp") {
        return std::make_unique<pl::DspEngine>(width, height, scale, filters,
                                               strength, surfaces, raw8, agc);
    }
    std::cerr
        << "[pipeline] --engine cpu: every pixel is processed on the CPU; "
           "the DSP is the default."
        << std::endl;
    return std::make_unique<pl::CpuEngine>(width, height, scale, filters,
                                           strength, surfaces, agc);
}

/* A saved frame, read straight into a dma-buf: the same kind of memory the
   capture node hands over, so the engine path is the live one with the camera
   replaced by a file. */
struct RawFrame {
    void* block = nullptr;
    pl::Frame frame{};
};

RawFrame load_raw(const std::string& path, std::uint32_t width,
                  std::uint32_t height, std::uint32_t stride)
{
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        throw std::runtime_error("cannot open " + path + ": " +
                                 std::strerror(errno));
    }
    struct stat info{};
    if (::fstat(fd, &info) != 0) {
        const int error = errno;
        ::close(fd);
        throw std::runtime_error("cannot stat " + path + ": " +
                                 std::strerror(error));
    }
    /* The sensor's row can be wider than the frame (1640 comes back with
       bytesperline 1648), and --dump keeps the driver's layout.  So the file
       itself says what the stride was: one frame is exactly stride*height
       bytes.  An explicit --stride still wins, for cropped files. */
    if (stride == 0U) {
        if (info.st_size <= 0 ||
            (static_cast<std::size_t>(info.st_size) % height) != 0U) {
            ::close(fd);
            throw std::runtime_error(
                "replay file is not a whole number of rows: pass --stride");
        }
        stride = static_cast<std::uint32_t>(info.st_size / height);
    }
    const std::size_t size = static_cast<std::size_t>(stride) * height;
    if (static_cast<std::size_t>(info.st_size) < size) {
        ::close(fd);
        throw std::runtime_error("replay file is shorter than one frame");
    }
    /* Uncached on purpose: read() then lands in memory, and neither the host
       nor the DSP has to think about cache maintenance for a file. */
    void* block =
        rpcmem_alloc(RPCMEM_HEAP_ID_SYSTEM, RPCMEM_FLAG_UNCACHED, size);
    if (block == nullptr) {
        ::close(fd);
        throw std::runtime_error("rpcmem_alloc for the replayed frame");
    }
    std::size_t filled = 0U;
    while (filled < size) {
        const ssize_t got =
            ::read(fd, static_cast<char*>(block) + filled, size - filled);
        if (got <= 0) {
            break;
        }
        filled += static_cast<std::size_t>(got);
    }
    ::close(fd);
    if (filled < size) {
        rpcmem_free(block);
        throw std::runtime_error("replay file is shorter than one frame");
    }

    RawFrame raw;
    raw.block = block;
    raw.frame.fd = rpcmem_to_fd(block);
    raw.frame.data = block;
    raw.frame.size = size;
    raw.frame.width = width;
    raw.frame.height = height;
    raw.frame.stride = stride;
    return raw;
}

/*
 * Write one rendered frame as plain RGBA, rows back to back.
 *
 * The surface stride is width*4 by construction now (output_geometry rounds the
 * visible width down to a whole vector), but the copy stays: it is the contract
 * of the dump, and it is what caught the earlier geometry. Back then a
 * 1640-pixel picture sat in a 1664-pixel row, and every viewer shifted each row
 * a little further than the one above. The picture came out as diagonal
 * stripes.
 */
std::size_t write_rgba_dump(const std::string& path, const pl::Surface& surface,
                            const pl::OutputGeometry& out)
{
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        return 0U;
    }
    const std::size_t row_bytes = static_cast<std::size_t>(out.width) * 4U;
    std::size_t written = 0U;
    for (std::uint32_t y = 0; y < out.height; ++y) {
        const auto* row = static_cast<const std::uint8_t*>(surface.data) +
                          static_cast<std::size_t>(y) * out.stride;
        written += std::fwrite(row, 1, row_bytes, file);
    }
    std::fclose(file);
    return written;
}

/*
 * Replay one saved raw frame through either engine.
 *
 * Two live runs never see the same frame, so the only way to prove the DSP
 * and the CPU reference agree is to hand both of them the same bytes.
 */
int replay(const std::string& path, std::uint32_t width, std::uint32_t height,
           std::uint32_t stride, std::uint32_t scale, std::uint32_t filters,
           std::uint32_t strength, const std::string& engine_name, bool raw8,
           bool agc, const std::string& dump_path)
{
    RawFrame raw = load_raw(path, width, height, stride);
    const pl::Frame& frame = raw.frame;

    const pl::OutputGeometry out = pl::output_geometry(width, height, scale);
    int result = 0;
    {
        auto engine = make_engine(engine_name, width, height, scale, filters,
                                  strength, 2U, raw8, agc);
        pl::Surface* surface = engine->acquire();
        if (surface == nullptr) {
            throw std::runtime_error("engine handed out no surface");
        }
        result = engine->process(frame, *surface);
        if (result == 0) {
            std::cout << "[replay] " << path << " " << width << "x" << height
                      << " stride " << frame.stride
                      << (raw8 ? " RAW8" : " RAW10") << " -> " << engine_name
                      << " " << out.width << "x" << out.height
                      << " (1:" << scale << ")" << std::endl;
            if (!dump_path.empty()) {
                const std::size_t written =
                    write_rgba_dump(dump_path, *surface, out);
                if (written != 0U) {
                    std::cout << "[dump] " << dump_path << " " << written
                              << " bytes " << out.width << "x" << out.height
                              << " tight RGBA" << std::endl;
                }
            }
        }
        engine->release(surface);
    }
    rpcmem_free(raw.block);
    return result == 0 ? 0 : 1;
}

/*
 * Compare the DSP against the CPU reference on one frame, inside this process.
 *
 * The matrix is every filter at every strength plus the whole chain, and it
 * costs exactly one DSP session, which is also why it is the right way to run
 * a long test on this board: the CDSP here does not survive hundreds of
 * short-lived sessions, and one process is one session.
 *
 * `bad` counts bytes that differ by more than three levels; the pass criterion
 * is the maximum difference against --tolerance.
 */
int parity(const std::string& path, std::uint32_t width, std::uint32_t height,
           std::uint32_t stride, std::uint32_t scale, std::uint32_t filters,
           std::uint32_t strength, bool matrix, bool raw8,
           std::uint32_t tolerance, bool agc)
{
    RawFrame raw = load_raw(path, width, height, stride);
    const pl::OutputGeometry out = pl::output_geometry(width, height, scale);
    const std::uint32_t row_bytes = out.width * 4U;

    /* With the loop on, both engines walk the same gain over the same cases:
       that compares the two loops, not just the two kernels. */
    auto dsp = std::make_unique<pl::DspEngine>(width, height, scale, filters,
                                               strength, 2U, raw8, agc);
    auto cpu = std::make_unique<pl::CpuEngine>(width, height, scale, filters,
                                               strength, 2U, agc);

    std::vector<std::uint32_t> sets;
    if (matrix) {
        /* 0 first: the bare demosaic has to match before any filter can.  It
           was missing from this list, so a tone regression in the skel could
           hide behind the filter cases for as long as nobody read the numbers.
         */
        sets = {0U,          pl::kSharpen,   pl::kPixelate, pl::kEdge,
                pl::kSketch, pl::kEmboss,    pl::kSepia,    pl::kPosterize,
                pl::kInvert, pl::kAllFilters};
    } else {
        sets = {filters};
    }
    std::vector<std::uint32_t> levels;
    if (matrix) {
        levels = {1U, 2U, 3U, 4U, 5U, 6U};
    } else {
        levels = {strength};
    }

    std::cout << "[parity] " << path << " " << width << "x" << height
              << " stride " << raw.frame.stride << (raw8 ? " RAW8" : " RAW10")
              << " -> " << out.width << "x" << out.height << " (1:" << scale
              << "), tolerance " << tolerance << std::endl;

    std::uint32_t worst = 0;
    std::size_t worst_bad = 0;
    std::uint32_t cases = 0;
    bool sync_ok = true;
    for (const std::uint32_t set : sets) {
        for (const std::uint32_t level : levels) {
            dsp->configure(set, level);
            cpu->configure(set, level);
            pl::Surface* a = dsp->acquire();
            pl::Surface* b = cpu->acquire();
            if (a == nullptr || b == nullptr) {
                throw std::runtime_error("engine handed out no surface");
            }
            if (dsp->process(raw.frame, *a) != 0 ||
                cpu->process(raw.frame, *b) != 0) {
                throw std::runtime_error("parity render failed");
            }
            /* The host is about to read a surface it just handed to the DSP,
               so the kernel has to invalidate the CPU's lines for it: without
               this the second case in the matrix could answer from cache with
               the frame from the case before. */
            struct dma_buf_sync sync{};
            sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ;
            const bool synced = ::ioctl(a->fd, DMA_BUF_IOCTL_SYNC, &sync) == 0;
            sync_ok = sync_ok && synced;

            std::uint32_t max_delta = 0;
            std::size_t bad = 0;
            for (std::uint32_t y = 0; y < out.height; ++y) {
                const auto* p = static_cast<const std::uint8_t*>(a->data) +
                                static_cast<std::size_t>(y) * out.stride;
                const auto* q = static_cast<const std::uint8_t*>(b->data) +
                                static_cast<std::size_t>(y) * out.stride;
                if (std::memcmp(p, q, row_bytes) == 0) {
                    continue;
                }
                for (std::uint32_t x = 0; x < row_bytes; ++x) {
                    const unsigned delta = static_cast<unsigned>(
                        p[x] > q[x] ? p[x] - q[x] : q[x] - p[x]);
                    if (delta > max_delta) {
                        max_delta = delta;
                    }
                    if (delta > 3U) {
                        ++bad;
                    }
                }
            }
            if (synced) {
                sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
                ::ioctl(a->fd, DMA_BUF_IOCTL_SYNC, &sync);
            }
            dsp->release(a);
            cpu->release(b);
            ++cases;
            worst = std::max(worst, max_delta);
            worst_bad = std::max(worst_bad, bad);
            std::cout << "[parity] " << filter_names(set) << " s" << level
                      << "  max|d| " << max_delta << "  bad " << bad
                      << std::endl;
        }
    }
    /* Close the DSP session before the frame it was reading goes away. */
    dsp.reset();
    cpu.reset();
    rpcmem_free(raw.block);

    if (!sync_ok) {
        std::cerr << "[parity] warning: DMA_BUF_IOCTL_SYNC was refused, so a "
                     "repeat read may come from the CPU cache"
                  << std::endl;
    }
    std::cout << "[parity] worst " << worst << ", bad " << worst_bad << " over "
              << cases << " case" << (cases == 1U ? "" : "s") << std::endl;
    return worst <= tolerance ? 0 : 1;
}

void usage(const char* program)
{
    std::cout
        << "usage: " << program << " [options]\n"
        << "  --device PATH     capture node, default /dev/video9\n"
        << "  --media PATH      media device for routing, default /dev/media0\n"
        << "  --sink NAME       wayland | none (default none)\n"
           "                    wayland: a window owned by the sink,\n"
           "                    rebuilt when the surface size changes\n"
        << "  --width N         default 640; the sensor snaps to its nearest\n"
        << "                    mode: 640x480, 1640x1232, 1920x1080, "
           "3280x2464\n"
        << "  --height N        default 480\n"
        << "  --fps N           advertised rate, default 30\n"
        << "  --scale N         window scale 1..6, done on the DSP (default "
           "1)\n"
        << "  --raw8            capture 8-bit Bayer (default: packed 10-bit)\n"
        << "  --filters LIST    none|all|edge,emboss,posterize,invert,\n"
        << "                    pixelate,sketch,sepia,sharpen\n"
        << "  --strength N      1..6 (default 2)\n"
        << "  --engine NAME     dsp (default) | cpu\n"
        << "  --agc             let the exposure loop pick the gain (default)\n"
           "  --no-agc          freeze the exposure loop at its current gain;\n"
           "                    a dim room then renders almost black, because\n"
           "                    the sensor is already at its exposure and "
           "gain\n"
           "                    limits\n"
        << "  --exposure N      sensor exposure in lines, read back after the\n"
           "                    write (default: whatever the driver has)\n"
        << "  --gain N          sensor analogue gain, driver code 0..232\n"
        << "  --dump PATH       write one rendered frame as raw RGBA, rows\n"
           "                    packed tight (diagnostic)\n"
        << "  --dump-after N    dump the Nth frame instead of the first "
           "(default 1)\n"
        << "  --replay PATH     render one saved raw frame and exit "
           "(diagnostic)\n"
        << "  --parity          with --replay: run the DSP and the CPU\n"
           "                    reference on that frame and compare them\n"
           "                    (every filter and strength unless --filters\n"
           "                    is given), exit 1 above --tolerance\n"
        << "  --tolerance N     largest difference --parity accepts (default "
           "2)\n"
        << "  --stride N        row bytes of the replayed frame, default from "
           "the file\n"
        << "  --tui             draw the panel in the terminal\n"
        << "  --seconds N       stop after N seconds (0 = until Ctrl-C)\n"
        << "  --no-route        do not touch the media links before starting\n";
}

} // namespace

int main(int argc, char** argv)
{
    std::string device = "/dev/video9";
    std::string media = "/dev/media0";
    std::string sink_name = "none";
    std::string engine_name = "dsp";
    std::uint32_t width = 640;
    std::uint32_t height = 480;
    std::uint32_t fps = 30;
    std::uint32_t filters = 0U;
    std::uint32_t strength = 2U;
    std::uint32_t scale = 1U;
    bool raw8 = false;
    /* On by default: the sensor on this board is already at its exposure and
       gain limits, so with the loop off a dim room renders almost black. */
    bool agc = true;
    bool exposure_given = false;
    bool gain_given = false;
    std::uint32_t exposure = 0U;
    std::uint32_t gain = 0U;
    bool tui = false;
    bool no_route = false;
    std::string dump_path;
    std::uint32_t dump_after = 1U;
    std::string replay_path;
    std::uint32_t replay_stride = 0U;
    bool parity_mode = false;
    bool filters_given = false;
    std::uint32_t tolerance = 2U;
    double seconds = 0.0;

    try {
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            auto next = [&]() -> std::string {
                if (i + 1 >= argc) {
                    throw std::invalid_argument("missing value for " + arg);
                }
                return argv[++i];
            };
            if (arg == "--device") {
                device = next();
            } else if (arg == "--media") {
                media = next();
            } else if (arg == "--sink") {
                sink_name = next();
            } else if (arg == "--width") {
                width = static_cast<std::uint32_t>(std::stoul(next()));
            } else if (arg == "--height") {
                height = static_cast<std::uint32_t>(std::stoul(next()));
            } else if (arg == "--fps") {
                fps = static_cast<std::uint32_t>(std::stoul(next()));
            } else if (arg == "--scale") {
                scale = static_cast<std::uint32_t>(std::stoul(next()));
            } else if (arg == "--raw8") {
                raw8 = true;
            } else if (arg == "--filters") {
                filters = parse_filters(next());
                filters_given = true;
            } else if (arg == "--strength") {
                strength = static_cast<std::uint32_t>(std::stoul(next()));
            } else if (arg == "--engine") {
                engine_name = next();
            } else if (arg == "--agc") {
                agc = true;
            } else if (arg == "--no-agc") {
                agc = false;
            } else if (arg == "--exposure") {
                exposure = static_cast<std::uint32_t>(std::stoul(next()));
                exposure_given = true;
            } else if (arg == "--gain") {
                gain = static_cast<std::uint32_t>(std::stoul(next()));
                gain_given = true;
            } else if (arg == "--dump") {
                dump_path = next();
            } else if (arg == "--dump-after") {
                dump_after = static_cast<std::uint32_t>(std::stoul(next()));
                if (dump_after == 0U) {
                    dump_after = 1U;
                }
            } else if (arg == "--replay") {
                replay_path = next();
            } else if (arg == "--parity") {
                parity_mode = true;
            } else if (arg == "--tolerance") {
                tolerance = static_cast<std::uint32_t>(std::stoul(next()));
            } else if (arg == "--stride") {
                replay_stride = static_cast<std::uint32_t>(std::stoul(next()));
            } else if (arg == "--tui") {
                tui = true;
            } else if (arg == "--seconds") {
                seconds = std::stod(next());
            } else if (arg == "--no-route") {
                no_route = true;
            } else if (arg == "--help" || arg == "-h") {
                usage(argv[0]);
                return 0;
            } else {
                throw std::invalid_argument("unknown option: " + arg);
            }
        }
        if (width < 64U || (width % 2U) != 0U) {
            throw std::invalid_argument("width must be even and at least 64");
        }
        if (scale < 1U || scale > pl::kMaxScale) {
            throw std::invalid_argument("--scale is 1..6");
        }
        if (pl::output_geometry(width, height, scale).width < 32U) {
            throw std::invalid_argument("width/scale must be at least 32");
        }
        if (width > pl::kMaxWidth) {
            throw std::invalid_argument("width is limited to 3328 pixels");
        }
        if (engine_name != "cpu" && engine_name != "dsp") {
            throw std::invalid_argument("--engine is dsp or cpu");
        }
        if (strength < 1U || strength > 6U) {
            throw std::invalid_argument("--strength is 1..6");
        }

        std::signal(SIGINT, on_signal);
        std::signal(SIGTERM, on_signal);

        /* A replay needs no camera, no routing and no sink: it is one frame
           through the engine, so it can run while the pipeline is live. */
        if (parity_mode) {
            if (replay_path.empty()) {
                throw std::invalid_argument("--parity needs --replay");
            }
            return parity(replay_path, width, height, replay_stride, scale,
                          filters, strength, !filters_given, raw8, tolerance,
                          agc);
        }
        if (!replay_path.empty()) {
            return replay(replay_path, width, height, replay_stride, scale,
                          filters, strength, engine_name, raw8, agc, dump_path);
        }

        /* The media graph has to describe the size we are about to ask for;
           otherwise the sensor keeps its old mode and S_FMT is refused. */
        pl::RouteResult routed;
        if (!no_route) {
            routed = pl::route_camera(media, device, width, height, raw8);
            std::cout << "[route] " << routed.detail << std::endl;
            if (!routed.ok) {
                std::cerr << "[route] warning: some steps were refused; "
                             "continuing with the current topology"
                          << std::endl;
            }
        }

        auto capture =
            std::make_unique<pl::Capture>(device, width, height, raw8);

        /* The sensor's knobs are moved only once the mode is final: the
           exposure ceiling is the frame length, and the mode decides that. */
        pl::Sensor sensor(routed.sensor);
        if (sensor.ok()) {
            sensor.maximise_pixel_values();
            if (exposure_given) {
                sensor.set_exposure(exposure);
            }
            if (gain_given) {
                sensor.set_gain(gain);
            }
            std::cout << "[sensor] " << sensor.detail() << std::endl;
        } else if (!routed.sensor.empty()) {
            std::cerr << "[sensor] " << routed.sensor
                      << ": exposure or analogue_gain not available"
                      << std::endl;
        }
        const pl::OutputGeometry out =
            pl::output_geometry(capture->width(), capture->height(), scale);
        auto sink = pl::make_sink(sink_name, out.width, out.height,
                                  static_cast<std::uint32_t>(out.stride), fps);

        std::unique_ptr<pl::Engine> engine =
            make_engine(engine_name, capture->width(), capture->height(), scale,
                        filters, strength, sink->surfaces_needed(), raw8, agc);

        std::cout << "[pipeline] " << device << " " << capture->width() << "x"
                  << capture->height() << " stride " << capture->stride()
                  << (raw8 ? " RAW8" : " RAW10") << " -> " << engine_name
                  << (agc ? "+agc" : "") << " -> " << sink_name << " "
                  << out.width << "x" << out.height << " (1:" << scale
                  << ", stride " << out.stride << ")" << std::endl;

        pl::Panel panel;
        const bool panel_up = tui && panel.open();
        pl::Metrics metrics;
        bool help = false;
        bool redraw = true;
        bool dumped = false;
        std::string note;
        auto last_draw = std::chrono::steady_clock::now();

        /* The two ends of the frame path as a rate rather than a size: the
           camera's plane and the surface the DSP fills, both times the frame
           rate.  The capture plane is the driver's, the output is ours, so
           the difference is the window scale. */
        std::size_t frame_bytes = 0U;
        const std::size_t out_bytes =
            out.stride * static_cast<std::size_t>(out.height);
        auto mbps = [](double fps, std::size_t bytes) {
            return fps * static_cast<double>(bytes) / 1.0e6;
        };

        const int epoll_fd = ::epoll_create1(EPOLL_CLOEXEC);
        if (epoll_fd < 0) {
            throw std::runtime_error(std::string("epoll_create1: ") +
                                     std::strerror(errno));
        }
        epoll_event interest{};
        interest.events = EPOLLIN;
        interest.data.fd = capture->fd();
        ::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, capture->fd(), &interest);
        if (panel_up) {
            interest.data.fd = STDIN_FILENO;
            ::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, STDIN_FILENO, &interest);
        }

        const auto started = std::chrono::steady_clock::now();
        bool warned_empty = false;
        auto state = [&]() {
            pl::PanelState snapshot;
            snapshot.engine = engine_name;
            snapshot.sink = sink_name;
            snapshot.width = capture->width();
            snapshot.height = capture->height();
            snapshot.filters = filters;
            snapshot.strength = strength;
            snapshot.scale = scale;
            snapshot.raw8 = raw8;
            snapshot.fps = metrics.fps();
            snapshot.peak = metrics.peak();
            snapshot.lat = metrics.lat();
            snapshot.lat_max = metrics.lat_max();
            snapshot.mbps_in = mbps(metrics.fps(), frame_bytes);
            snapshot.mbps_out = mbps(metrics.fps(), out_bytes);
            snapshot.frames = metrics.frames();
            std::uint32_t shift = 0U;
            std::uint32_t luma = 0U;
            const bool loop = engine->exposure(shift, luma);
            snapshot.agc = loop && engine->agc_running();
            snapshot.agc_shift = shift;
            snapshot.agc_luma = luma;
            snapshot.note = note;
            return snapshot;
        };
        auto apply = [&]() {
            engine->configure(filters, strength);
            char text[64];
            std::snprintf(text, sizeof text, "filters 0x%03x  strength %u",
                          filters, strength);
            note = text;
            redraw = true;
        };

        /* A new size means a new V4L2 session and a new sensor mode, so the
           cleanest thing is to become a fresh process with the same choices.
           Everything is torn down first: the camera must be free before the
           new process opens it. */
        auto relaunch = [&](std::uint32_t next_width, std::uint32_t next_height,
                            const std::string& next_engine,
                            std::uint32_t next_scale) {
            std::vector<std::string> args;
            args.push_back("/proc/self/exe");
            args.push_back("--device");
            args.push_back(device);
            args.push_back("--media");
            args.push_back(media);
            args.push_back("--sink");
            args.push_back(sink_name);
            args.push_back("--width");
            args.push_back(std::to_string(next_width));
            args.push_back("--height");
            args.push_back(std::to_string(next_height));
            args.push_back("--scale");
            args.push_back(std::to_string(next_scale));
            args.push_back("--fps");
            args.push_back(std::to_string(fps));
            args.push_back("--engine");
            args.push_back(next_engine);
            args.push_back("--strength");
            args.push_back(std::to_string(strength));
            args.push_back("--filters");
            args.push_back(filter_names(filters));
            /* The sensor knobs and the run length are not state the panel
               can reproduce from scratch: a restart that forgets them changes
               how much light a frame carries. */
            if (exposure_given) {
                args.push_back("--exposure");
                args.push_back(std::to_string(exposure));
            }
            if (gain_given) {
                args.push_back("--gain");
                args.push_back(std::to_string(gain));
            }
            if (seconds > 0.0) {
                args.push_back("--seconds");
                args.push_back(std::to_string(seconds));
            }
            if (raw8) {
                args.push_back("--raw8");
            }
            if (agc) {
                args.push_back("--agc");
            } else {
                args.push_back("--no-agc");
            }
            if (!dump_path.empty()) {
                args.push_back("--dump");
                args.push_back(dump_path);
                args.push_back("--dump-after");
                args.push_back(std::to_string(dump_after));
            }
            if (tui) {
                args.push_back("--tui");
            }
            if (no_route) {
                args.push_back("--no-route");
            }
            std::vector<char*> pointers;
            pointers.reserve(args.size() + 1U);
            for (std::string& arg : args) {
                pointers.push_back(arg.data());
            }
            pointers.push_back(nullptr);

            sink->eos();
            sink.reset();
            engine.reset();
            capture.reset();
            panel.close();
            ::close(epoll_fd);
            ::execv(pointers[0], pointers.data());
            std::perror("execv");
            std::exit(1);
        };

        while (g_stop == 0) {
            if (seconds > 0.0 && std::chrono::duration<double>(
                                     std::chrono::steady_clock::now() - started)
                                         .count() >= seconds) {
                break;
            }

            /* Silence is the worst failure mode here: the process runs, the
               panel shows 0 fps and nothing says why.  A wrong media path or
               a stuck CAMSS looks exactly like that. */
            if (!warned_empty && metrics.frames() == 0 &&
                std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                              started)
                        .count() >= 1.5) {
                warned_empty = true;
                std::fprintf(stderr,
                             "[pipeline] no frames after 1.5 s: check the "
                             "[route] line, then reset CAMSS\n");
            }

            epoll_event events[4];
            const int ready = ::epoll_wait(epoll_fd, events, 4, 200);
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw std::runtime_error(std::string("epoll_wait: ") +
                                         std::strerror(errno));
            }

            for (int i = 0; i < ready; ++i) {
                if (events[i].data.fd == capture->fd()) {
                    pl::Frame frame{};
                    if (!capture->dequeue(frame)) {
                        continue;
                    }
                    frame_bytes = frame.size;
                    pl::Surface* surface = engine->acquire();
                    if (surface == nullptr) {
                        /* The sink still holds both surfaces: drop this frame
                           rather than stall the camera. */
                        static unsigned starved = 0;
                        if ((++starved % 120U) == 1U) {
                            std::fprintf(stderr,
                                         "[pipeline] starved: the sink holds "
                                         "every surface, dropping frames\n");
                        }
                        capture->requeue(frame.index);
                        continue;
                    }
                    const auto t0 = std::chrono::steady_clock::now();
                    const int result = engine->process(frame, *surface);
                    const auto t1 = std::chrono::steady_clock::now();
                    const double latency =
                        std::chrono::duration<double, std::milli>(t1 - t0)
                            .count();
                    capture->requeue(frame.index);
                    if (result != 0) {
                        engine->release(surface);
                        throw std::runtime_error("engine rejected the frame");
                    }
                    metrics.frame(latency);
                    if (!dump_path.empty() && !dumped &&
                        metrics.frames() >= dump_after) {
                        const std::size_t written =
                            write_rgba_dump(dump_path, *surface, out);
                        if (written != 0U) {
                            std::cout << "[dump] " << dump_path << " "
                                      << written << " bytes " << out.width
                                      << "x" << out.height << " tight RGBA"
                                      << std::endl;
                        }
                        /* The raw frame that produced it, so a bad picture can
                           be traced back to its input without guessing. */
                        const std::string raw_path = dump_path + ".raw";
                        std::FILE* raw_file =
                            std::fopen(raw_path.c_str(), "wb");
                        if (raw_file != nullptr) {
                            std::fwrite(frame.data, 1, frame.size, raw_file);
                            std::fclose(raw_file);
                        }
                        dumped = true;
                    }
                    pl::Surface held = *surface;
                    sink->push(held, [engine = engine.get(), surface] {
                        engine->release(surface);
                    });
                } else {
                    const pl::PanelEvent event = panel.read();
                    redraw = true;
                    switch (event.key) {
                    case pl::PanelKey::None:
                        break;
                    case pl::PanelKey::Quit:
                        g_stop = 1;
                        break;
                    case pl::PanelKey::Filter:
                        filters ^= event.mask;
                        apply();
                        break;
                    case pl::PanelKey::Clear:
                        filters = 0U;
                        apply();
                        break;
                    case pl::PanelKey::Strength: {
                        const int next =
                            static_cast<int>(strength) + event.delta;
                        strength = static_cast<std::uint32_t>(
                            next < 1 ? 1 : (next > 6 ? 6 : next));
                        apply();
                        break;
                    }
                    case pl::PanelKey::Resolution: {
                        const Mode next =
                            next_mode(capture->width(), capture->height());
                        relaunch(next.width, next.height, engine_name, scale);
                        break;
                    }
                    case pl::PanelKey::Scale:
                        /* The window scale changes the surface size, so it
                           takes the same restart path a resolution does. */
                        relaunch(capture->width(), capture->height(),
                                 engine_name,
                                 (scale >= pl::kMaxScale) ? 1U : scale + 1U);
                        break;
                    case pl::PanelKey::Engine:
                        /* Switching engines restarts the process for the same
                           reason a resolution change does: the camera and the
                           DSP session are torn down in a known order. */
                        relaunch(capture->width(), capture->height(),
                                 engine_name == "dsp" ? "cpu" : "dsp", scale);
                        break;
                    case pl::PanelKey::Agc:
                        /* AGC is a per-frame flag, so it flips in place: no
                           restart, and nothing else on the panel changes.
                           Both engines have the loop; the CPU one mirrors it
                           so a reference run shows the same picture. */
                        agc = !agc;
                        engine->set_exposure(agc);
                        note = agc ? "agc on" : "agc off, gain held";
                        redraw = true;
                        break;
                    case pl::PanelKey::Help:
                        help = !help;
                        break;
                    }
                }
            }

            if (metrics.tick()) {
                if (panel_up) {
                    redraw = true;
                } else {
                    std::uint32_t shift = 0U;
                    std::uint32_t luma = 0U;
                    const bool loop = engine->exposure(shift, luma);
                    std::printf(
                        "[status] engine=%s fps=%.1f peak=%.1f "
                        "frames=%llu lat=%.2f latmax=%.2f "
                        "filters=0x%03x strength=%u scale=1:%u",
                        engine_name.c_str(), metrics.fps(), metrics.peak(),
                        static_cast<unsigned long long>(metrics.frames()),
                        metrics.lat(), metrics.lat_max(), filters, strength,
                        scale);
                    if (loop) {
                        if (engine->agc_running()) {
                            std::printf(" agc=x%u/%u", 1U << shift, luma);
                        } else if (shift > 0U) {
                            /* Off, but the gain it froze is still applied. */
                            std::printf(" agc=held/x%u", 1U << shift);
                        } else {
                            std::printf(" agc=off");
                        }
                    }
                    std::printf("\n");
                    std::fflush(stdout);
                }
            }

            if (panel_up) {
                const auto now = std::chrono::steady_clock::now();
                if (redraw ||
                    std::chrono::duration<double>(now - last_draw).count() >=
                        0.25) {
                    panel.draw(state(), help);
                    last_draw = now;
                    redraw = false;
                }
            }
        }

        sink->eos();
        /* Teardown order matters: dropping the sink releases every buffer the
           compositor still holds, and each release calls back into the engine.
           So the sink must go first. */
        sink.reset();
        engine.reset();
        capture.reset();
        panel.close();
        ::close(epoll_fd);

        std::cout << "[pipeline] stopped after " << metrics.frames()
                  << " frames, mean " << metrics.lat_mean_all() << " ms, max "
                  << metrics.lat_max_all() << " ms, peak " << metrics.peak()
                  << " fps" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "pipeline: " << error.what() << std::endl;
        return 1;
    }
}
