/*
 * v4l2_capture.cpp - buffers, and the interrupt that fills them.
 *
 * Every buffer is requested as MMAP, which gives us an address to read, and
 * exported as a DMA-BUF, which gives us a descriptor to share. Waiting is an
 * epoll on the node, woken by the CSI DMA-done handler. No polling thread, no
 * copy.
 */
#include "v4l2_capture.h"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {

void check(int result, const char* what)
{
    if (result < 0) {
        const int error = errno;
        std::string text = std::string(what) + ": " + std::strerror(error);
        /* The camera is not ours alone: a qcam, a libcamera daemon or a
           leftover pipeline would keep reprogramming the sensor underneath us,
           and the frame would no longer be the one this program asked for.
           That is a refusal, not a warning. */
        if (error == EBUSY) {
            text += " (another process holds the camera; the sensor may only "
                    "be driven by this program; stop qcam/cam/pipeline first)";
        }
        throw std::runtime_error(text);
    }
}

} // namespace

namespace pl {

Capture::Capture(const std::string& device, std::uint32_t width,
                 std::uint32_t height, bool raw8, unsigned buffer_count)
    : width_(width), height_(height)
{
    fd_ = ::open(device.c_str(), O_RDWR | O_CLOEXEC);
    if (fd_ < 0) {
        throw std::runtime_error("open " + device + ": " +
                                 std::strerror(errno));
    }

    v4l2_capability caps{};
    check(::ioctl(fd_, VIDIOC_QUERYCAP, &caps), "VIDIOC_QUERYCAP");
    if ((caps.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE) == 0U ||
        (caps.capabilities & V4L2_CAP_STREAMING) == 0U) {
        throw std::runtime_error(device + " is not a streaming mplane node");
    }

    v4l2_format format{};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    format.fmt.pix_mp.width = width;
    format.fmt.pix_mp.height = height;
    format.fmt.pix_mp.pixelformat =
        raw8 ? V4L2_PIX_FMT_SRGGB8 : V4L2_PIX_FMT_SRGGB10P;
    format.fmt.pix_mp.field = V4L2_FIELD_NONE;
    check(::ioctl(fd_, VIDIOC_S_FMT, &format), "VIDIOC_S_FMT");

    if (format.fmt.pix_mp.pixelformat !=
            (raw8 ? V4L2_PIX_FMT_SRGGB8 : V4L2_PIX_FMT_SRGGB10P) ||
        format.fmt.pix_mp.num_planes != 1U) {
        throw std::runtime_error("driver refused the requested Bayer format");
    }
    width_ = format.fmt.pix_mp.width;
    height_ = format.fmt.pix_mp.height;
    stride_ = format.fmt.pix_mp.plane_fmt[0].bytesperline;
    if (stride_ == 0U) {
        throw std::runtime_error("driver returned a zero stride");
    }

    v4l2_requestbuffers request{};
    request.count = buffer_count;
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    request.memory = V4L2_MEMORY_MMAP;
    check(::ioctl(fd_, VIDIOC_REQBUFS, &request), "VIDIOC_REQBUFS");
    if (request.count < 2U) {
        throw std::runtime_error("driver granted fewer than two buffers");
    }

    buffers_.resize(request.count);
    for (std::uint32_t index = 0; index < request.count; ++index) {
        v4l2_plane planes[VIDEO_MAX_PLANES]{};
        v4l2_buffer buffer{};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = index;
        buffer.length = 1;
        buffer.m.planes = planes;
        check(::ioctl(fd_, VIDIOC_QUERYBUF, &buffer), "VIDIOC_QUERYBUF");

        Buffer& slot = buffers_[index];
        slot.length = planes[0].length;
        slot.data =
            ::mmap(nullptr, slot.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                   fd_, static_cast<off_t>(planes[0].m.mem_offset));
        if (slot.data == MAP_FAILED) {
            throw std::runtime_error(std::string("mmap capture buffer: ") +
                                     std::strerror(errno));
        }

        /* The dma-buf descriptor is what makes the rest of the pipeline
           copy-free: FastRPC registers these pages, the compositor imports
           them. */
        v4l2_exportbuffer exported{};
        exported.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        exported.index = index;
        exported.plane = 0;
        exported.flags = O_CLOEXEC;
        check(::ioctl(fd_, VIDIOC_EXPBUF, &exported), "VIDIOC_EXPBUF");
        slot.fd = exported.fd;

        check(::ioctl(fd_, VIDIOC_QBUF, &buffer), "VIDIOC_QBUF");
    }

    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    check(::ioctl(fd_, VIDIOC_STREAMON, &type), "VIDIOC_STREAMON");
    streaming_ = true;
}

Capture::~Capture()
{
    if (fd_ < 0) {
        return;
    }
    if (streaming_) {
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        ::ioctl(fd_, VIDIOC_STREAMOFF, &type);
    }
    for (const Buffer& buffer : buffers_) {
        if (buffer.data != nullptr) {
            ::munmap(buffer.data, buffer.length);
        }
        if (buffer.fd >= 0) {
            ::close(buffer.fd);
        }
    }
    ::close(fd_);
}

bool Capture::dequeue(Frame& frame)
{
    v4l2_plane planes[VIDEO_MAX_PLANES]{};
    v4l2_buffer buffer{};
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.length = 1;
    buffer.m.planes = planes;

    if (::ioctl(fd_, VIDIOC_DQBUF, &buffer) < 0) {
        if (errno == EAGAIN) {
            return false;
        }
        throw std::runtime_error(std::string("VIDIOC_DQBUF: ") +
                                 std::strerror(errno));
    }

    const Buffer& slot = buffers_[buffer.index];
    frame.fd = slot.fd;
    frame.data = slot.data;
    frame.size = slot.length;
    frame.width = width_;
    frame.height = height_;
    frame.stride = stride_;
    frame.index = buffer.index;
    frame.sequence = buffer.sequence;
    frame.timestamp_ns =
        static_cast<std::uint64_t>(buffer.timestamp.tv_sec) * 1000000000ULL +
        static_cast<std::uint64_t>(buffer.timestamp.tv_usec) * 1000ULL;
    return true;
}

void Capture::requeue(std::uint32_t index)
{
    v4l2_plane planes[VIDEO_MAX_PLANES]{};
    v4l2_buffer buffer{};
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.index = index;
    buffer.length = 1;
    buffer.m.planes = planes;
    check(::ioctl(fd_, VIDIOC_QBUF, &buffer), "VIDIOC_QBUF");
}

} // namespace pl
