/*
 * frame.h - one frame and one surface.
 *
 * A frame is the DMA-BUF the sensor wrote. A surface is the DMA-BUF the engine
 * writes the picture into, and the compositor reads that same buffer. No copy
 * is made anywhere in between.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace pl {

struct Frame {
    int fd = -1;          /* dma-buf, exported from V4L2 */
    void* data = nullptr; /* CPU mapping of the same pages */
    std::size_t size = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride = 0;
    std::uint32_t index = 0; /* V4L2 buffer index, for requeue */
    std::uint32_t sequence = 0;
    std::uint64_t timestamp_ns = 0; /* kernel stamp at end of frame */
};

struct Surface {
    int fd = -1;
    void* data = nullptr;
    std::size_t size = 0;
};

} // namespace pl
