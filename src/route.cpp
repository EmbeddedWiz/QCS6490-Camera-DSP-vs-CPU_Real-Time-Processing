/*
 * route.cpp - the media graph, set up from C++.
 *
 * The shell version of this is media-ctl plus v4l2-ctl. Here the same kernel
 * calls are made in process: list the entities, enable the path from the
 * sensor to the capture node, set one format end to end. The path is found by
 * walking up from the capture node, so another CSI/CSID/VFE pairing still
 * works.
 */
#include "route.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/media.h>
#include <linux/v4l2-subdev.h>
#include <linux/videodev2.h>

namespace {

/* The legacy MEDIA_IOC_ENUM_LINKS reports zero links on this kernel; the
   topology ioctl is the one that actually carries the graph. */
struct Graph {
    std::vector<media_v2_entity> entities;
    std::vector<media_v2_interface> interfaces;
    std::vector<media_v2_pad> pads;
    std::vector<media_v2_link> links;
    std::map<std::uint32_t, std::uint32_t> slot; /* pad id -> index in entity */
};

std::string name_of(const media_v2_entity& entity)
{
    return std::string(entity.name);
}

std::string base_name(const std::string& path)
{
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1U);
}

bool load_graph(int fd, Graph& graph)
{
    media_v2_topology topology{};
    if (::ioctl(fd, MEDIA_IOC_G_TOPOLOGY, &topology) != 0) {
        return false;
    }
    graph.entities.resize(topology.num_entities);
    graph.interfaces.resize(topology.num_interfaces);
    graph.pads.resize(topology.num_pads);
    graph.links.resize(topology.num_links);

    topology.ptr_entities =
        reinterpret_cast<std::uintptr_t>(graph.entities.data());
    topology.ptr_interfaces =
        reinterpret_cast<std::uintptr_t>(graph.interfaces.data());
    topology.ptr_pads = reinterpret_cast<std::uintptr_t>(graph.pads.data());
    topology.ptr_links = reinterpret_cast<std::uintptr_t>(graph.links.data());
    if (::ioctl(fd, MEDIA_IOC_G_TOPOLOGY, &topology) != 0) {
        return false;
    }

    std::map<std::uint32_t, std::uint32_t> counters;
    for (const media_v2_pad& pad : graph.pads) {
        graph.slot[pad.id] = counters[pad.entity_id]++;
    }
    return true;
}

const media_v2_entity* find_entity(const Graph& graph, std::uint32_t id)
{
    for (const media_v2_entity& entity : graph.entities) {
        if (entity.id == id) {
            return &entity;
        }
    }
    return nullptr;
}

const media_v2_entity* find_entity(const Graph& graph, const std::string& name)
{
    for (const media_v2_entity& entity : graph.entities) {
        if (name == name_of(entity)) {
            return &entity;
        }
    }
    return nullptr;
}

const media_v2_pad* find_pad(const Graph& graph, std::uint32_t id)
{
    for (const media_v2_pad& pad : graph.pads) {
        if (pad.id == id) {
            return &pad;
        }
    }
    return nullptr;
}

std::uint32_t slot_of(const Graph& graph, std::uint32_t pad_id)
{
    const auto found = graph.slot.find(pad_id);
    return found == graph.slot.end() ? 0U : found->second;
}

bool is_data_link(const media_v2_link& link)
{
    return (link.flags & MEDIA_LNK_FL_LINK_TYPE) == MEDIA_LNK_FL_DATA_LINK;
}

/* Every video node and subdev announces its entity name in sysfs; that is the
   only stable way to map an entity to a /dev node. */
std::map<std::string, std::string> nodes_by_name()
{
    std::map<std::string, std::string> out;
    std::error_code error;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator("/sys/class/video4linux", error)) {
        std::ifstream in(entry.path() / "name");
        std::string name;
        if (!std::getline(in, name) || name.empty()) {
            continue;
        }
        out[name] = "/dev/" + entry.path().filename().string();
    }
    return out;
}

bool setup_link(int fd, const Graph& graph, const media_v2_link& link, bool on)
{
    const media_v2_pad* source = find_pad(graph, link.source_id);
    const media_v2_pad* sink = find_pad(graph, link.sink_id);
    if (source == nullptr || sink == nullptr) {
        return false;
    }
    media_link_desc desc{};
    desc.source.entity = source->entity_id;
    desc.source.index = slot_of(graph, source->id);
    desc.sink.entity = sink->entity_id;
    desc.sink.index = slot_of(graph, sink->id);
    desc.flags = on ? MEDIA_LNK_FL_ENABLED : 0U;
    return ::ioctl(fd, MEDIA_IOC_SETUP_LINK, &desc) == 0;
}

/* Leave exactly one path enabled: disable every changeable data link first.
   Immutable links (sensor -> csiphy) are not ours to touch. */
void reset_links(int fd, const Graph& graph)
{
    for (const media_v2_link& link : graph.links) {
        if (!is_data_link(link) || (link.flags & MEDIA_LNK_FL_IMMUTABLE) != 0 ||
            (link.flags & MEDIA_LNK_FL_ENABLED) == 0) {
            continue;
        }
        setup_link(fd, graph, link, false);
    }
}

/* A VFE input can be fed by any of five CSIDs, and only one of them has a
   sensor behind it.  So collect every upstream path and score it: the branch
   that terminates at a sensor (the sensor link is immutable, which is how we
   recognise it) and, above all, the CSID that matches its CSIPHY. */
struct Candidate {
    std::vector<const media_v2_entity*> entities;
    std::vector<const media_v2_link*> links;
};

int trailing_index(const std::string& name)
{
    std::size_t end = name.size();
    while (end > 0U &&
           std::isdigit(static_cast<unsigned char>(name[end - 1U])) != 0) {
        --end;
    }
    if (end == name.size()) {
        return -1;
    }
    return std::stoi(name.substr(end));
}

void collect_paths(const Graph& graph, const media_v2_entity* current,
                   std::vector<const media_v2_entity*>& stack,
                   std::vector<const media_v2_link*>& links,
                   std::vector<Candidate>& out, unsigned depth)
{
    if (depth > 16U) {
        return;
    }
    bool descended = false;
    for (const media_v2_link& link : graph.links) {
        if (!is_data_link(link) || slot_of(graph, link.sink_id) != 0U) {
            continue;
        }
        const media_v2_pad* sink = find_pad(graph, link.sink_id);
        if (sink == nullptr || sink->entity_id != current->id) {
            continue;
        }
        const media_v2_pad* source = find_pad(graph, link.source_id);
        if (source == nullptr) {
            continue;
        }
        const media_v2_entity* upstream = find_entity(graph, source->entity_id);
        if (upstream == nullptr ||
            std::find(stack.begin(), stack.end(), upstream) != stack.end()) {
            continue;
        }
        descended = true;
        stack.push_back(upstream);
        links.push_back(&link);
        collect_paths(graph, upstream, stack, links, out, depth + 1U);
        links.pop_back();
        stack.pop_back();
    }
    if (!descended) {
        out.push_back(Candidate{stack, links});
    }
}

int score_path(const Graph& graph, const Candidate& candidate)
{
    int score = static_cast<int>(candidate.entities.size());
    for (const media_v2_link* link : candidate.links) {
        if ((link->flags & MEDIA_LNK_FL_ENABLED) != 0) {
            /* Only a tie-break: a leftover link from an older run must not
               outvote the path that physically carries this camera. */
            score += 10;
        }
    }
    /* The sensor hangs off its CSIPHY through an immutable link, so a path
       that ends at an entity with such an input is the one with a camera. */
    for (const media_v2_link& link : graph.links) {
        if ((link.flags & MEDIA_LNK_FL_IMMUTABLE) == 0) {
            continue;
        }
        const media_v2_pad* sink = find_pad(graph, link.sink_id);
        if (sink != nullptr &&
            sink->entity_id == candidate.entities.back()->id) {
            score += 50;
        }
    }
    /* csiphyN only reaches the VFE through csidN: the other four CSIDs accept
       the link, the formats and the stream-on, and then deliver nothing at all
       (0 frames, no error).  Pair them by index. */
    int phy = -1;
    int csid = -1;
    for (const media_v2_entity* entity : candidate.entities) {
        const std::string name = name_of(*entity);
        if (name.rfind("msm_csiphy", 0U) == 0U) {
            phy = trailing_index(name);
        } else if (name.rfind("msm_csid", 0U) == 0U) {
            csid = trailing_index(name);
        }
    }
    if (phy >= 0 && phy == csid) {
        score += 1000;
    }
    return score;
}

Candidate best_path(const Graph& graph, const media_v2_entity* start)
{
    std::vector<Candidate> paths;
    std::vector<const media_v2_entity*> stack{start};
    std::vector<const media_v2_link*> links;
    collect_paths(graph, start, stack, links, paths, 0U);
    Candidate best;
    int best_score = -1;
    for (const Candidate& candidate : paths) {
        const int score = score_path(graph, candidate);
        if (score > best_score) {
            best_score = score;
            best = candidate;
        }
    }
    return best; /* capture node first, sensor last */
}

bool enable_path(int fd, const Graph& graph, const Candidate& candidate)
{
    bool ok = true;
    for (const media_v2_link* link : candidate.links) {
        /* Immutable links are enabled by the kernel and cannot be changed. */
        if ((link->flags & MEDIA_LNK_FL_IMMUTABLE) != 0) {
            continue;
        }
        if (!setup_link(fd, graph, *link, true)) {
            ok = false;
        }
    }
    return ok;
}

std::uint32_t pad_count(const Graph& graph, std::uint32_t entity_id)
{
    std::uint32_t count = 0U;
    for (const media_v2_pad& pad : graph.pads) {
        if (pad.entity_id == entity_id) {
            ++count;
        }
    }
    return count;
}

bool set_pad_format(const std::string& node, std::uint32_t pad,
                    std::uint32_t width, std::uint32_t height,
                    std::uint32_t code)
{
    if (node.empty()) {
        return false;
    }
    const int fd = ::open(node.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    v4l2_subdev_format format{};
    format.which = V4L2_SUBDEV_FORMAT_ACTIVE;
    format.pad = pad;
    format.format.width = width;
    format.format.height = height;
    format.format.code = code;
    format.format.field = V4L2_FIELD_NONE;
    format.format.colorspace = V4L2_COLORSPACE_SRGB;
    const bool ok = ::ioctl(fd, VIDIOC_SUBDEV_S_FMT, &format) == 0;
    ::close(fd);
    return ok;
}

} // namespace

namespace pl {

RouteResult route_camera(const std::string& media, const std::string& video,
                         std::uint32_t width, std::uint32_t height, bool raw8)
{
    RouteResult result;
    const int fd = ::open(media.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        result.detail = "cannot open " + media + ": " + std::strerror(errno);
        return result;
    }

    Graph graph;
    if (!load_graph(fd, graph) || graph.entities.empty()) {
        ::close(fd);
        result.detail = "cannot read the topology of " + media;
        return result;
    }

    /* The capture node names its own entity in sysfs. */
    std::string wanted;
    {
        std::ifstream in("/sys/class/video4linux/" + base_name(video) +
                         "/name");
        std::getline(in, wanted);
    }
    const media_v2_entity* capture = find_entity(graph, wanted);
    if (capture == nullptr) {
        ::close(fd);
        result.detail = "no entity for " + video + " (" + wanted + ")";
        return result;
    }

    const Candidate path = best_path(graph, capture);
    if (path.entities.size() < 2U) {
        ::close(fd);
        result.detail = "no upstream path from " + name_of(*capture) + " (" +
                        std::to_string(graph.links.size()) + " links, " +
                        std::to_string(graph.pads.size()) + " pads)";
        return result;
    }

    reset_links(fd, graph);
    const bool linked = enable_path(fd, graph, path);

    const std::map<std::string, std::string> nodes = nodes_by_name();
    const std::uint32_t code =
        raw8 ? MEDIA_BUS_FMT_SRGGB8_1X8 : MEDIA_BUS_FMT_SRGGB10_1X10;
    bool formatted = true;
    std::string chain;
    std::string refused;
    for (const media_v2_entity* entity : path.entities) {
        if (entity->id == capture->id) {
            continue;
        }
        const auto node = nodes.find(name_of(*entity));
        if (node == nodes.end()) {
            formatted = false;
            refused += name_of(*entity) + ":no-node ";
            continue;
        }
        const std::uint32_t pads = pad_count(graph, entity->id);
        for (std::uint32_t pad = 0; pad < pads && pad < 2U; ++pad) {
            if (!set_pad_format(node->second, pad, width, height, code)) {
                formatted = false;
                refused += name_of(*entity) + ":" + std::to_string(pad) + " ";
            }
        }
        chain += name_of(*entity);
        chain += " → ";
    }
    const auto sensor = nodes.find(name_of(*path.entities.back()));
    if (sensor != nodes.end()) {
        result.sensor = sensor->second;
    }
    ::close(fd);

    result.ok = linked && formatted;
    char text[128];
    std::snprintf(text, sizeof text, "%ux%u %s", width, height,
                  raw8 ? "SRGGB8_1X8" : "SRGGB10_1X10");
    result.detail = chain + text;
    if (!linked) {
        result.detail += " (links refused)";
    } else if (!formatted) {
        result.detail += " (formats refused: " + refused + ")";
    }
    return result;
}

} // namespace pl
