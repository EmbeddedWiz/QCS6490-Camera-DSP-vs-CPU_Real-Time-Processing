/*
 * sensor.cpp - exposure and gain, written and read back.
 *
 * The driver clamps every value, so the read-back is the truth. The panel and
 * the log report what the sensor is really doing, not what we asked for.
 */
#include "sensor.h"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/v4l2-subdev.h>
#include <linux/videodev2.h>

namespace {

/* True when `haystack` contains `needle`, ignoring case.  Control names are
   the kernel's ("Red Pixel Value"), and the class headers are the ones to
   skip. */
bool contains_ci(const char* haystack, const char* needle)
{
    const std::size_t length = std::strlen(needle);
    for (const char* p = haystack; *p != '\0'; ++p) {
        std::size_t i = 0;
        while (i < length && p[i] != '\0' &&
               std::tolower(static_cast<unsigned char>(p[i])) ==
                   std::tolower(static_cast<unsigned char>(needle[i]))) {
            ++i;
        }
        if (i == length) {
            return true;
        }
    }
    return false;
}

} // namespace

namespace pl {

Sensor::Sensor(const std::string& node) : node_(node)
{
    fd_ = ::open(node_.c_str(), O_RDWR | O_CLOEXEC);
    if (fd_ < 0) {
        return;
    }
    /* The height is the mode's, and the mode is already set by the route: the
       sensor snaps to the nearest one when the pad format is written. */
    v4l2_subdev_format format{};
    format.which = V4L2_SUBDEV_FORMAT_ACTIVE;
    format.pad = 0U;
    if (::ioctl(fd_, VIDIOC_SUBDEV_G_FMT, &format) == 0) {
        height_ = format.format.height;
    }
    discover();
}

Sensor::~Sensor()
{
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

bool Sensor::probe(Knob& knob, std::uint32_t id)
{
    v4l2_query_ext_ctrl query{};
    query.id = id;
    if (::ioctl(fd_, VIDIOC_QUERY_EXT_CTRL, &query) != 0) {
        return false;
    }
    if ((query.flags & V4L2_CTRL_FLAG_DISABLED) != 0U ||
        (query.flags & V4L2_CTRL_FLAG_READ_ONLY) != 0U) {
        return false;
    }
    knob.id = id;
    knob.min = static_cast<std::int32_t>(query.minimum);
    knob.max = static_cast<std::int32_t>(query.maximum);
    v4l2_control control{};
    control.id = id;
    if (::ioctl(fd_, VIDIOC_G_CTRL, &control) == 0) {
        knob.value = control.value;
    }
    return true;
}

void Sensor::discover()
{
    probe(exposure_, V4L2_CID_EXPOSURE);
    probe(gain_, V4L2_CID_ANALOGUE_GAIN);
    probe(vblank_, V4L2_CID_VBLANK);
}

void Sensor::refresh(Knob& knob)
{
    if (knob.id == 0U) {
        return;
    }
    v4l2_query_ext_ctrl query{};
    query.id = knob.id;
    if (::ioctl(fd_, VIDIOC_QUERY_EXT_CTRL, &query) == 0) {
        knob.min = query.minimum;
        knob.max = query.maximum;
    }
}

std::int32_t Sensor::put(Knob& knob, std::uint32_t value)
{
    if (knob.id == 0U) {
        return 0;
    }
    /* The range may have moved since the knob was found (the exposure
       maximum follows the frame length), so it is re-read before clamping. */
    refresh(knob);
    const std::int32_t wanted =
        value < static_cast<std::uint32_t>(knob.min)
            ? knob.min
            : (value > static_cast<std::uint32_t>(knob.max)
                   ? knob.max
                   : static_cast<std::int32_t>(value));
    v4l2_control control{};
    control.id = knob.id;
    control.value = wanted;
    if (::ioctl(fd_, VIDIOC_S_CTRL, &control) == 0) {
        knob.value = control.value;
    }
    /* Whatever the driver took, not what was asked: that is the whole point
       of the read-back. */
    v4l2_control read{};
    read.id = knob.id;
    if (::ioctl(fd_, VIDIOC_G_CTRL, &read) == 0) {
        knob.value = read.value;
    }
    return knob.value;
}

std::uint32_t Sensor::set_exposure(std::uint32_t lines)
{
    return wide(put(exposure_, lines));
}

std::uint32_t Sensor::set_gain(std::uint32_t code)
{
    return wide(put(gain_, code));
}

std::uint32_t Sensor::set_vblank(std::uint32_t lines)
{
    const std::uint32_t taken = wide(put(vblank_, lines));
    /* The exposure's ceiling is frame_length - 4, and the frame length just
       changed, so the exposure range has to be re-read before the next write
       clamps against it. */
    refresh(exposure_);
    return taken;
}

void Sensor::maximise_pixel_values()
{
    v4l2_query_ext_ctrl query{};
    query.id = V4L2_CTRL_FLAG_NEXT_CTRL;
    while (::ioctl(fd_, VIDIOC_QUERY_EXT_CTRL, &query) == 0) {
        const char* name = reinterpret_cast<const char*>(query.name);
        if (contains_ci(name, "pixel value") &&
            (query.flags & V4L2_CTRL_FLAG_DISABLED) == 0U &&
            (query.flags & V4L2_CTRL_FLAG_READ_ONLY) == 0U) {
            v4l2_control control{};
            control.id = query.id;
            control.value = query.maximum;
            if (::ioctl(fd_, VIDIOC_S_CTRL, &control) == 0) {
                ++pixel_values_;
            }
        }
        query.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
    }
}

std::string Sensor::detail() const
{
    char text[192];
    if (!ok()) {
        std::snprintf(text, sizeof text, "%s: no exposure/analogue_gain",
                      node_.c_str());
        return text;
    }
    std::snprintf(text, sizeof text,
                  "%s height %u, exposure %u/%u lines, gain %u/%u, frame %u "
                  "lines, %u pixel values at max",
                  node_.c_str(), height_, exposure(), exposure_max(), gain(),
                  gain_max(), frame_length(), pixel_values_);
    return text;
}

} // namespace pl