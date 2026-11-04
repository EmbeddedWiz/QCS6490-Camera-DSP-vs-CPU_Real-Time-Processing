/*
 * dsp_engine.cpp - the host half of the FastRPC session.
 *
 * The session is opened once. Camera buffers are registered by descriptor,
 * and after that each frame is one render call. Only addresses and arguments
 * cross the bus; the pixels stay in the DMA-BUFs.
 */
#include "dsp_engine.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

#include <unistd.h>

#include <fastrpc/remote.h>
#include <fastrpc/rpcmem.h>

#include "pipe_hvx.h"

namespace {

constexpr std::uint32_t kRaw8 = 0x1U;
constexpr std::uint32_t kAgc = 0x2U;

/* The loader reads DSP_LIBRARY_PATH from the environment of the process that
   calls pipe_hvx_open().  A stale value (the QNN export in ~/.bashrc) makes
   the open fail with AEE_EUNABLETOLOAD, which looks like a DSP fault, so the
   skel next to the binary wins over anything already set. */
bool has_skel(const std::string& directory)
{
    if (directory.empty()) {
        return false;
    }
    return ::access((directory + "/libpipe_hvx_skel.so").c_str(), R_OK) == 0;
}

std::string executable_directory()
{
    char buffer[4096];
    const ssize_t length =
        ::readlink("/proc/self/exe", buffer, sizeof buffer - 1);
    if (length <= 0) {
        return {};
    }
    buffer[length] = '\0';
    const std::string path(buffer);
    const std::size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string{} : path.substr(0, slash);
}

void prepare_library_path()
{
    const std::string exe = executable_directory();
    const std::string candidates[] = {
        exe + "/dsp",    /* build/pipeline -> build/dsp */
        exe + "/../dsp", /* build/host/live -> build/dsp */
        exe,
    };
    for (const std::string& directory : candidates) {
        if (has_skel(directory)) {
            ::setenv("DSP_LIBRARY_PATH", directory.c_str(), 1);
            return;
        }
    }
    /* Nothing found: leave the environment alone so the loader error names the
       path it actually tried. */
}

std::string cdsp_state()
{
    const std::string base = "/sys/class/remoteproc";
    for (int index = 0; index < 8; ++index) {
        const std::string root = base + "/remoteproc" + std::to_string(index);
        std::FILE* name_file = std::fopen((root + "/name").c_str(), "r");
        if (name_file == nullptr) {
            continue;
        }
        char name[64] = {};
        const bool matched = std::fscanf(name_file, "%63s", name) == 1 &&
                             std::strcmp(name, "cdsp") == 0;
        std::fclose(name_file);
        if (!matched) {
            continue;
        }
        std::FILE* state_file = std::fopen((root + "/state").c_str(), "r");
        if (state_file == nullptr) {
            return {};
        }
        char state[64] = {};
        const bool read = std::fscanf(state_file, "%63s", state) == 1;
        std::fclose(state_file);
        return read ? std::string(state) : std::string{};
    }
    return {};
}

} // namespace

namespace pl {

DspEngine::DspEngine(std::uint32_t width, std::uint32_t height,
                     std::uint32_t scale, std::uint32_t filters,
                     std::uint32_t strength, unsigned surface_count, bool raw8,
                     bool agc)
    : width_(width), height_(height), scale_(scale), filters_(filters),
      strength_(strength), raw8_(raw8), agc_(agc)
{
    output_size_ = output_geometry(width, height, scale).size;
    if (output_size_ > static_cast<std::size_t>(0x7FFFFFFF)) {
        throw std::invalid_argument("output frame does not fit an IDL length");
    }
    if (surface_count < 2U) {
        throw std::invalid_argument(
            "the DSP engine needs at least two surfaces");
    }

    try {
        open_session();

        surfaces_.resize(surface_count);
        blocks_.resize(surface_count, nullptr);
        for (unsigned i = 0; i < surface_count; ++i) {
            void* block =
                rpcmem_alloc(RPCMEM_HEAP_ID_SYSTEM, RPCMEM_FLAG_CACHED,
                             static_cast<int>(output_size_));
            if (block == nullptr) {
                throw std::runtime_error("rpcmem_alloc for the DSP surface");
            }
            blocks_[i] = block;
            surfaces_[i].data = block;
            surfaces_[i].size = output_size_;
            surfaces_[i].fd = rpcmem_to_fd(block);
            if (surfaces_[i].fd < 0) {
                throw std::runtime_error("rpcmem_to_fd for the DSP surface");
            }
            free_.push_back(static_cast<int>(i));
        }
    } catch (...) {
        for (void* block : blocks_) {
            if (block != nullptr) {
                rpcmem_free(block);
            }
        }
        if (handle_ != 0) {
            pipe_hvx_close(handle_);
            handle_ = 0;
        }
        throw;
    }
}

DspEngine::~DspEngine()
{
    /* Hand the camera mappings back before the session goes.  Closing the
       session releases them too, but a mapping left behind when the camera
       buffer is recycled is what turns into the SMMU fault this board cannot
       recover from. */
    for (const auto& entry : registered_) {
        remote_register_buf(entry.second.first,
                            static_cast<int>(entry.second.second), -1);
    }
    registered_.clear();
    if (handle_ != 0) {
        pipe_hvx_close(handle_);
        handle_ = 0;
    }
    for (void* block : blocks_) {
        if (block != nullptr) {
            rpcmem_free(block);
        }
    }
}

void DspEngine::open_session()
{
    prepare_library_path();

    remote_rpc_control_unsigned_module unsigned_module{};
    unsigned_module.domain = CDSP_DOMAIN_ID;
    unsigned_module.enable = 1;
    const int control = remote_session_control(
        DSPRPC_CONTROL_UNSIGNED_MODULE, &unsigned_module,
        static_cast<std::uint32_t>(sizeof(unsigned_module)));
    if (control != 0) {
        throw std::runtime_error("remote_session_control failed: " +
                                 std::to_string(control));
    }

    const std::string uri = std::string(pipe_hvx_URI) + CDSP_DOMAIN;
    remote_handle64 handle = 0;
    const int opened = pipe_hvx_open(uri.c_str(), &handle);
    if (opened != 0) {
        std::string hint = "set DSP_LIBRARY_PATH to the directory holding "
                           "libpipe_hvx_skel.so";
        const std::string state = cdsp_state();
        if (!state.empty() && state != "attached" && state != "running") {
            hint += "; CDSP state is \"" + state +
                    "\". It crashed, and only a reboot reloads its firmware";
        }
        const std::string name = (static_cast<unsigned>(opened) == 0x80000406U)
                                     ? " (AEE_EUNABLETOLOAD)"
                                     : std::string();
        throw std::runtime_error("pipe_hvx_open failed: " +
                                 std::to_string(opened) + name + ": " + hint);
    }
    handle_ = handle;
}

Surface* DspEngine::acquire()
{
    if (free_.empty()) {
        return nullptr;
    }
    const int index = free_.back();
    free_.pop_back();
    return &surfaces_[static_cast<std::size_t>(index)];
}

void DspEngine::release(Surface* surface)
{
    if (surface == nullptr) {
        return;
    }
    free_.push_back(static_cast<int>(surface - surfaces_.data()));
}

void DspEngine::configure(std::uint32_t filters, std::uint32_t strength)
{
    filters_ = filters;
    strength_ = strength;
}

/*
 * Register the camera buffer once.  What is registered is the mapping, not the
 * descriptor: when V4L2 recycles a buffer the DMA mapping is torn down, and a
 * descriptor-only registration leaves the CDSP pointing at pages that no
 * longer belong to it. That is an SMMU fault, which on this board is fatal.
 */
void DspEngine::register_frame(const Frame& frame)
{
    const auto known = registered_.find(frame.fd);
    if (known != registered_.end() && known->second.first == frame.data &&
        known->second.second == frame.size) {
        return;
    }
    if (frame.size > static_cast<std::size_t>(0x7FFFFFFF)) {
        throw std::invalid_argument("camera mapping is too large for FastRPC");
    }

    /* V4L2 recycles buffers, and a recycled buffer keeps its descriptor but
       names different pages.  The old mapping has to go first: a registration
       that outlives its pages is exactly the SMMU fault this board does not
       survive. */
    if (known != registered_.end()) {
        remote_register_buf(known->second.first,
                            static_cast<int>(known->second.second), -1);
        registered_.erase(known);
    }

    errno = 0;
    remote_register_buf(frame.data, static_cast<int>(frame.size), frame.fd);
    if (errno != 0) {
        throw std::runtime_error(std::string("remote_register_buf: ") +
                                 std::strerror(errno));
    }
    registered_.emplace(frame.fd, std::make_pair(frame.data, frame.size));
}

int DspEngine::process(const Frame& frame, Surface& out)
{
    if (frame.data == nullptr || frame.fd < 0) {
        return -1;
    }
    if (frame.width != width_ || frame.height != height_) {
        return -1;
    }

    register_frame(frame);

    std::uint32_t flags = 0U;
    if (raw8_) {
        flags |= kRaw8;
    }
    if (agc_) {
        flags |= kAgc;
    }

    /* The camera mapping goes over as it is: no copy, no staging buffer. */
    const int result = pipe_hvx_render(
        handle_, static_cast<const std::uint8_t*>(frame.data),
        static_cast<int>(frame.size), frame.stride, width_, height_, scale_,
        filters_, strength_, flags, static_cast<std::uint8_t*>(out.data),
        static_cast<int>(output_size_));
    return result == 0 ? 0 : -1;
}

bool DspEngine::exposure(std::uint32_t& shift, std::uint32_t& luma)
{
    /* One round trip, on the panel's clock rather than the camera's.  Read it
       even when the loop is off: the gain it last settled on is still applied
       to every frame, and the panel has to show that. */
    if (pipe_hvx_state(handle_, &agc_shift_, &agc_luma_) != 0) {
        return false;
    }
    shift = agc_shift_;
    luma = agc_luma_;
    return true;
}

/* Switching the loop off freezes the exposure where it stands: the gain stays
   on the DSP and keeps being applied, so the picture does not jump back to
   unity, and switching it on again resumes from there. */
void DspEngine::set_exposure(bool on)
{
    agc_ = on;
}

} // namespace pl
