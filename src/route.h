/*
 * route.h - set up the media graph from C++.
 *
 * The old tree does this with media-ctl and v4l2-ctl. Here the same kernel
 * calls are made directly: read the graph, enable the path from the sensor to
 * the capture node, and set one format end to end.
 *
 * The path is found by walking up from the capture node, so it is not tied to
 * one board revision. The sensor node is handed back to the caller; sensor.h
 * uses it once the mode is final.
 */
#pragma once

#include <cstdint>
#include <string>

namespace pl {

struct RouteResult {
    bool ok = false;
    std::string detail; /* one line: the path that was set, or why it failed */
    /* The sensor's subdev node, empty when the path did not end at one.  The
       exposure and the analogue gain are that node's controls, so the caller
       needs it to reach them. */
    std::string sensor;
};

/* `video` is the capture node (e.g. /dev/video9); `media` is its media
   device (e.g. /dev/media0). */
RouteResult route_camera(const std::string& media, const std::string& video,
                         std::uint32_t width, std::uint32_t height, bool raw8);

} // namespace pl
