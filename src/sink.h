/*
 * sink.h - where the picture leaves us.
 *
 * A sink gets a surface that is already a DMA-BUF and must not copy it. The
 * engine can reuse the surface only after done() fires; for the Wayland sink
 * that means the compositor has released the buffer.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "frame.h"

namespace pl {

class Sink {
public:
    virtual ~Sink() = default;
    virtual bool push(const Surface& surface, std::function<void()> done) = 0;
    virtual void eos() {}

    /* How many surfaces the engine must keep in flight.  A null sink hands the
       surface straight back, so ping-pong is enough; a compositor holds one
       while presenting and one pending, so it needs a deeper ring. */
    virtual unsigned surfaces_needed() const { return 2U; }
};

/* `wayland` presents through GStreamer's waylandsink; `none` throws every
   frame away, which is the reference path for measuring the engine alone. */
std::unique_ptr<Sink> make_sink(const std::string& name, std::uint32_t width,
                                std::uint32_t height, std::uint32_t stride,
                                std::uint32_t fps);

} // namespace pl
