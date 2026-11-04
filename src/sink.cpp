/*
 * sink.cpp - the last stop before the screen.
 *
 * The surface is wrapped in a GstBuffer that points at the DMA-BUF, so
 * waylandsink imports it and the compositor reads the pages the engine wrote.
 * The lease on that buffer is how the engine learns the surface is free again.
 */
#include "sink.h"

#include <cstdio>
#include <stdexcept>
#include <utility>

#include <unistd.h>

#include <gst/allocators/gstdmabuf.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/video/gstvideometa.h>
#include <gst/video/video-info-dma.h>

namespace {

/* Attached to every pushed buffer: the surface goes back to the engine when
   the compositor is done with it, not when push() returns. */
struct Lease {
    std::function<void()> done;
};

void lease_finalized(gpointer data, GstMiniObject*)
{
    auto* lease = static_cast<Lease*>(data);
    lease->done();
    delete lease;
}

/* The sink runs on the pipeline thread, but its complaints are the only clue
   when a buffer is never returned, so surface them immediately. */
GstBusSyncReply on_bus_message(GstBus*, GstMessage* message, gpointer)
{
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
        GError* error = nullptr;
        gchar* debug = nullptr;
        gst_message_parse_error(message, &error, &debug);
        std::fprintf(stderr, "[sink] error: %s%s%s\n",
                     error != nullptr ? error->message : "unknown",
                     debug != nullptr ? " | " : "",
                     debug != nullptr ? debug : "");
        g_clear_error(&error);
        g_free(debug);
    } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_WARNING) {
        GError* error = nullptr;
        gchar* debug = nullptr;
        gst_message_parse_warning(message, &error, &debug);
        std::fprintf(stderr, "[sink] warning: %s%s%s\n",
                     error != nullptr ? error->message : "unknown",
                     debug != nullptr ? " | " : "",
                     debug != nullptr ? debug : "");
        g_clear_error(&error);
        g_free(debug);
    }
    return GST_BUS_PASS;
}

} // namespace

namespace pl {

namespace {

class NullSink : public Sink {
public:
    bool push(const Surface&, std::function<void()> done) override
    {
        done();
        return true;
    }
};

class WaylandSink : public Sink {
public:
    WaylandSink(std::uint32_t width, std::uint32_t height, std::uint32_t stride,
                std::uint32_t fps)
        : width_(width), height_(height), stride_(stride), fps_(fps)
    {
        gst_init(nullptr, nullptr);
        start("appsrc name=src is-live=true format=time do-timestamp=false "
              "! waylandsink sync=false enable-last-sample=false");
    }

    ~WaylandSink() override
    {
        if (pipeline_ != nullptr) {
            gst_element_set_state(pipeline_, GST_STATE_NULL);
            gst_object_unref(pipeline_);
        }
        if (appsrc_ != nullptr) {
            gst_object_unref(appsrc_);
        }
        if (allocator_ != nullptr) {
            gst_object_unref(allocator_);
        }
    }

    bool push(const Surface& surface, std::function<void()> done) override
    {
        /* The allocator takes ownership of the descriptor, so hand it a dup. */
        const int fd = ::dup(surface.fd);
        if (fd < 0) {
            done();
            return false;
        }
        GstBuffer* buffer = gst_buffer_new();
        GstMemory* memory =
            gst_dmabuf_allocator_alloc(allocator_, fd, surface.size);
        gst_buffer_append_memory(buffer, memory);
        /* The engine writes whole 32-pixel vectors, so a row can be wider
           than the visible picture; the meta is what tells the compositor
           the true stride, and the caps keep the true width. */
        gsize offsets[1] = {0U};
        gint strides[1] = {static_cast<gint>(stride_)};
        gst_buffer_add_video_meta_full(
            buffer, GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_FORMAT_RGBA,
            static_cast<unsigned>(width_), static_cast<unsigned>(height_), 1U,
            offsets, strides);

        auto* lease = new Lease{std::move(done)};
        gst_mini_object_weak_ref(GST_MINI_OBJECT(buffer), lease_finalized,
                                 lease);

        const GstFlowReturn flow =
            gst_app_src_push_buffer(GST_APP_SRC(appsrc_), buffer);
        if (flow != GST_FLOW_OK) {
            std::fprintf(stderr, "[sink] push flow=%s\n",
                         gst_flow_get_name(flow));
            /* The buffer is already unreffed by push_buffer on failure; the
               weak ref has run, so the surface is back with the engine. */
            return false;
        }
        return true;
    }

    void eos() override
    {
        if (appsrc_ != nullptr) {
            gst_app_src_end_of_stream(GST_APP_SRC(appsrc_));
        }
    }

    unsigned surfaces_needed() const override { return 4U; }

protected:
    /* Build the pipeline and describe the surface.  A dma-buf surface needs
       caps that carry the DMABuf feature and the DRM fourcc, otherwise the
       sink refuses to negotiate and the buffers never come back. */
    void start(const char* graph)
    {
        GError* error = nullptr;
        pipeline_ = gst_parse_launch(graph, &error);
        if (pipeline_ == nullptr) {
            const std::string message =
                error != nullptr ? error->message : "unknown error";
            if (error != nullptr) {
                g_error_free(error);
            }
            throw std::runtime_error("GStreamer: " + message);
        }
        appsrc_ = gst_bin_get_by_name(GST_BIN(pipeline_), "src");
        if (appsrc_ == nullptr) {
            throw std::runtime_error("GStreamer: appsrc missing");
        }

        /* Caps for a dma-buf surface are not a plain video/x-raw: they must
           carry the DMABuf feature and the DRM fourcc, otherwise the sink
           refuses to negotiate and the buffers never come back.  The public
           way to build them is GstVideoInfoDmaDrm. */
        GstVideoInfo info;
        gst_video_info_set_format(&info, GST_VIDEO_FORMAT_RGBA, width_,
                                  height_);
        GST_VIDEO_INFO_FPS_N(&info) = static_cast<int>(fps_);
        GST_VIDEO_INFO_FPS_D(&info) = 1;

        GstVideoInfoDmaDrm dma_info;
        gst_video_info_dma_drm_init(&dma_info);
        if (!gst_video_info_dma_drm_from_video_info(&dma_info, &info, 0U)) {
            throw std::runtime_error("cannot describe RGBA as a DMA-BUF");
        }
        GstCaps* caps = gst_video_info_dma_drm_to_caps(&dma_info);
        if (caps == nullptr) {
            throw std::runtime_error("cannot build DMA-BUF caps");
        }
        gst_app_src_set_caps(GST_APP_SRC(appsrc_), caps);
        {
            gchar* text = gst_caps_to_string(caps);
            std::fprintf(stderr, "[sink] caps: %s\n", text);
            g_free(text);
        }
        gst_caps_unref(caps);
        g_object_set(G_OBJECT(appsrc_), "max-buffers", 2U, "block", FALSE,
                     nullptr);

        allocator_ = gst_dmabuf_allocator_new();
        gst_bus_set_sync_handler(gst_pipeline_get_bus(GST_PIPELINE(pipeline_)),
                                 on_bus_message, nullptr, nullptr);

        if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) ==
            GST_STATE_CHANGE_FAILURE) {
            throw std::runtime_error("GStreamer: cannot start waylandsink");
        }
    }

private:
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::uint32_t stride_ = 0;
    std::uint32_t fps_ = 30;
    GstElement* pipeline_ = nullptr;
    GstElement* appsrc_ = nullptr;
    GstAllocator* allocator_ = nullptr;
};

} // namespace

std::unique_ptr<Sink> make_sink(const std::string& name, std::uint32_t width,
                                std::uint32_t height, std::uint32_t stride,
                                std::uint32_t fps)
{
    if (name == "none") {
        return std::make_unique<NullSink>();
    }
    if (name == "wayland") {
        return std::make_unique<WaylandSink>(width, height, stride, fps);
    }
    throw std::invalid_argument("unknown sink: " + name);
}

} // namespace pl
