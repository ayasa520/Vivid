/*
 * Protocol optimization changes in this file are derived from waywallen.
 * Source: local reference tree waywallen/ and upstream https://github.com/waywallen/waywallen.
 * Copyright owner for the waywallen-derived protocol optimization code:
 * https://github.com/hypengw <hypengwip@gmail.com>.
 */

#pragma once

#include <glib.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iterator>

namespace vivid::producer
{

constexpr guint kFrameRouteMaxPlanes = 4u;
constexpr guint kFrameRouteMaxBuffers = 3u;

struct DmabufPlaneView
{
    int fd { -1 };
    uint32_t stride { 0 };
    uint32_t offset { 0 };
};

struct DmabufBufferView
{
    uint32_t index { 0 };
    uint64_t size { 0 };
    uint32_t n_planes { 0 };
    std::array<DmabufPlaneView, kFrameRouteMaxPlanes> planes {};
};

struct DmabufBufferSetView
{
    uint32_t width { 0 };
    uint32_t height { 0 };
    uint32_t fourcc { 0 };
    uint64_t modifier { 0 };
    gboolean premultiplied { FALSE };
    uint32_t n_buffers { 0 };
    std::array<DmabufBufferView, kFrameRouteMaxBuffers> buffers {};
};

template<typename BufferSet>
void
init_dmabuf_buffer_set(BufferSet& set)
{
    std::memset(&set, 0, sizeof(set));
    for (auto& buffer : set.buffers) {
        for (auto& plane : buffer.planes)
            plane.fd = -1;
    }
}

template<typename BufferSet>
void
clear_dmabuf_buffer_set(BufferSet& set)
{
    for (guint buffer = 0; buffer < set.n_buffers; buffer++) {
        auto& entry = set.buffers[buffer];
        for (guint plane = 0; plane < entry.n_planes; plane++) {
            if (entry.planes[plane].fd >= 0) {
                close(entry.planes[plane].fd);
                entry.planes[plane].fd = -1;
            }
        }
    }
    init_dmabuf_buffer_set(set);
}

template<typename BufferSet>
bool
copy_dmabuf_buffer_set(const DmabufBufferSetView& route_set,
                       BufferSet&                 out_set,
                       const char*                log_domain)
{
    init_dmabuf_buffer_set(out_set);
    out_set.width = route_set.width;
    out_set.height = route_set.height;
    out_set.fourcc = route_set.fourcc;
    out_set.modifier = route_set.modifier;
    out_set.premultiplied = route_set.premultiplied;

    const auto out_buffer_capacity = static_cast<uint32_t>(std::size(out_set.buffers));
    if (route_set.n_buffers == 0) {
        g_warning("%s: route published no DMA-BUF buffers", log_domain);
        return false;
    }
    if (route_set.n_buffers > out_buffer_capacity) {
        g_warning("%s: route published too many buffers count=%u capacity=%u",
                  log_domain,
                  route_set.n_buffers,
                  out_buffer_capacity);
        return false;
    }
    out_set.n_buffers = route_set.n_buffers;

    for (uint32_t buffer = 0; buffer < out_set.n_buffers; buffer++) {
        const auto& src = route_set.buffers[buffer];
        auto& dst = out_set.buffers[buffer];
        dst.index = src.index;
        dst.size = src.size;

        const auto out_plane_capacity = static_cast<uint32_t>(std::size(dst.planes));
        if (src.n_planes == 0) {
            g_warning("%s: route buffer=%u has no DMA-BUF planes",
                      log_domain,
                      src.index);
            clear_dmabuf_buffer_set(out_set);
            return false;
        }
        if (src.n_planes > out_plane_capacity) {
            g_warning("%s: route buffer=%u published too many planes count=%u capacity=%u",
                      log_domain,
                      src.index,
                      src.n_planes,
                      out_plane_capacity);
            clear_dmabuf_buffer_set(out_set);
            return false;
        }
        dst.n_planes = src.n_planes;

        for (uint32_t plane = 0; plane < dst.n_planes; plane++) {
            const int source_fd = src.planes[plane].fd;
            if (source_fd < 0) {
                g_warning("%s: route buffer=%u plane=%u has no DMA-BUF fd",
                          log_domain,
                          src.index,
                          plane);
                clear_dmabuf_buffer_set(out_set);
                return false;
            }

            const int dup_fd = dup(source_fd);
            if (dup_fd < 0) {
                g_warning("%s: failed to duplicate route DMA-BUF fd "
                          "buffer=%u plane=%u: %s",
                          log_domain,
                          src.index,
                          plane,
                          g_strerror(errno));
                clear_dmabuf_buffer_set(out_set);
                return false;
            }
            dst.planes[plane].fd = dup_fd;
            dst.planes[plane].stride = src.planes[plane].stride;
            dst.planes[plane].offset = src.planes[plane].offset;
        }
    }

    return out_set.n_buffers > 0;
}

class ProducerFrameRoute
{
public:
    explicit ProducerFrameRoute(const char* log_domain)
        : log_domain_(log_domain)
    {
    }

    void reset() { sequence_ = 0; }

    template<typename BufferSet>
    bool publish_buffer_set(const DmabufBufferSetView& route_set,
                            BufferSet&                 out_set) const
    {
        return copy_dmabuf_buffer_set(route_set, out_set, log_domain_);
    }

    /*
     * Scene and video are equal producers on the same output route: once either
     * backend has published into one of the route slots, this helper emits the
     * common FRAME_READY shape used by the display transport. Keeping sequence
     * and target-time generation here prevents each backend from growing its
     * own slightly different frame clock contract.
     */
    template<typename Frame>
    void write_ready_frame(uint32_t buffer_index, int32_t source_frame_id, Frame& out_frame)
    {
        std::memset(&out_frame, 0, sizeof(out_frame));
        out_frame.buffer_index = buffer_index;
        out_frame.source_frame_id = source_frame_id;
        out_frame.sequence = ++sequence_;
        out_frame.target_time_usec = static_cast<guint64>(g_get_monotonic_time());
        out_frame.acquire_sync_fd = -1;
    }

private:
    const char* log_domain_ { "VividProducer" };
    uint64_t sequence_ { 0 };
};

} // namespace vivid::producer
