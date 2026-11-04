/*
 * v4l2_capture.h - getting frames out of the camera.
 *
 * Buffers are mapped (MMAP) so we have an address, and exported as DMA-BUFs
 * so the engine and the compositor can share the pages. A frame is ready when
 * the CSI DMA-done interrupt fires; epoll on fd() wakes us. No polling thread
 * and no copy.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "frame.h"

namespace pl {

class Capture {
public:
    Capture(const std::string& device, std::uint32_t width,
            std::uint32_t height, bool raw8, unsigned buffer_count = 4);
    ~Capture();

    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;

    /* For epoll. */
    int fd() const { return fd_; }
    std::uint32_t width() const { return width_; }
    std::uint32_t height() const { return height_; }
    std::uint32_t stride() const { return stride_; }

    /* After the fd is readable.  False when the driver had nothing (spurious
       wake-up), which is not an error. */
    bool dequeue(Frame& frame);
    void requeue(std::uint32_t index);

private:
    struct Buffer {
        void* data = nullptr;
        std::size_t length = 0;
        int fd = -1;
    };

    int fd_ = -1;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::uint32_t stride_ = 0;
    std::vector<Buffer> buffers_;
    bool streaming_ = false;
};

} // namespace pl
