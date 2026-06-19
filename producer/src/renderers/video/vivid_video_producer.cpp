/*
 * Protocol optimization changes in this file are derived from waywallen.
 * Source: local reference tree waywallen/ and upstream https://github.com/waywallen/waywallen.
 * Copyright owner for the waywallen-derived protocol optimization code:
 * https://github.com/hypengw <hypengwip@gmail.com>.
 */

#include "vivid_video_producer.h"
#include "vivid_gpu_devices.h"
#include "vivid_producer_frame_route.hpp"
#include "vivid_video_vulkan_backend.hpp"

#include <ffnvcodec/dynlink_cuda.h>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using vivid::producer::DmabufBufferSetView;
using vivid::producer::ProducerFrameRoute;

namespace
{

constexpr GstMapFlags kGstMapReadCuda =
    static_cast<GstMapFlags>(GST_MAP_READ | (GST_MAP_FLAG_LAST << 1));
constexpr guint32 VIDEO_RELEASE_GATE_TIMEOUT_MSEC = 600u;

extern "C" {
typedef struct _GstCudaMemory GstCudaMemory;
typedef struct _GstCudaContext GstCudaContext;
typedef struct _GstCudaStream GstCudaStream;

gboolean gst_cuda_load_library(void);
gboolean gst_is_cuda_memory(GstMemory* mem);
GstCudaStream* gst_cuda_memory_get_stream(GstCudaMemory* mem);
CUstream gst_cuda_stream_get_handle(GstCudaStream* stream);
gboolean gst_cuda_context_push(GstCudaContext* ctx);
gboolean gst_cuda_context_pop(CUcontext* cuda_ctx);

CUresult CUDAAPI CuGetErrorName(CUresult error, const char** pStr);
CUresult CUDAAPI CuInit(unsigned int Flags);
CUresult CUDAAPI CuDeviceGetCount(int* count);
CUresult CUDAAPI CuDeviceGet(CUdevice* device, int ordinal);
CUresult CUDAAPI CuDeviceGetAttribute(int* pi, CUdevice_attribute attrib, CUdevice dev);
CUresult CUDAAPI CuModuleLoadData(CUmodule* module, const void* image);
CUresult CUDAAPI CuModuleUnload(CUmodule module);
CUresult CUDAAPI CuModuleGetFunction(CUfunction* hfunc, CUmodule hmod, const char* name);
CUresult CUDAAPI CuLaunchKernel(CUfunction f,
                                unsigned int gridDimX,
                                unsigned int gridDimY,
                                unsigned int gridDimZ,
                                unsigned int blockDimX,
                                unsigned int blockDimY,
                                unsigned int blockDimZ,
                                unsigned int sharedMemBytes,
                                CUstream hStream,
                                void** kernelParams,
                                void** extra);
CUresult CUDAAPI CuStreamSynchronize(CUstream hStream);
CUresult CUDAAPI CuCtxSynchronize(void);
}

/*
 * CUDA conversion stays in the producer because it is part of the video sample
 * upload path: NVDEC gives NV12 CUDAMemory, this kernel scales/crops into the
 * backend-owned transfer buffer, and the Vulkan backend publishes that buffer
 * into the stable DMA-BUF ring.
 */
constexpr char kNv12ToRgbaScaleCudaPtx[] = R"ptx(
.version 6.0
.target sm_52
.address_size 64

.visible .entry VividNv12ToRgbaScale(
    .param .u64 y_plane_param,
    .param .u64 uv_plane_param,
    .param .u64 rgba_param,
    .param .u32 src_width_param,
    .param .u32 src_height_param,
    .param .u32 dst_width_param,
    .param .u32 dst_height_param,
    .param .u32 y_pitch_param,
    .param .u32 uv_pitch_param,
    .param .u32 rgba_pitch_param,
    .param .u32 dst_x_param,
    .param .u32 dst_y_param,
    .param .u32 draw_width_param,
    .param .u32 draw_height_param,
    .param .u32 src_x0_fp_param,
    .param .u32 src_y0_fp_param,
    .param .u32 step_x_fp_param,
    .param .u32 step_y_fp_param
)
{
    .reg .pred %p<12>;
    .reg .b32 %r<120>;
    .reg .b64 %rd<32>;

    ld.param.u64 %rd1, [y_plane_param];
    ld.param.u64 %rd2, [uv_plane_param];
    ld.param.u64 %rd3, [rgba_param];
    ld.param.u32 %r1, [src_width_param];
    ld.param.u32 %r2, [src_height_param];
    ld.param.u32 %r3, [dst_width_param];
    ld.param.u32 %r4, [dst_height_param];
    ld.param.u32 %r5, [y_pitch_param];
    ld.param.u32 %r6, [uv_pitch_param];
    ld.param.u32 %r7, [rgba_pitch_param];
    ld.param.u32 %r8, [dst_x_param];
    ld.param.u32 %r9, [dst_y_param];
    ld.param.u32 %r16, [draw_width_param];
    ld.param.u32 %r17, [draw_height_param];
    ld.param.u32 %r18, [src_x0_fp_param];
    ld.param.u32 %r19, [src_y0_fp_param];
    ld.param.u32 %r24, [step_x_fp_param];
    ld.param.u32 %r25, [step_y_fp_param];

    mov.u32 %r10, %ctaid.x;
    mov.u32 %r11, %ntid.x;
    mov.u32 %r12, %tid.x;
    mad.lo.u32 %r20, %r10, %r11, %r12;

    mov.u32 %r13, %ctaid.y;
    mov.u32 %r14, %ntid.y;
    mov.u32 %r15, %tid.y;
    mad.lo.u32 %r21, %r13, %r14, %r15;

    setp.ge.u32 %p1, %r20, %r3;
    @%p1 bra DONE;
    setp.ge.u32 %p2, %r21, %r4;
    @%p2 bra DONE;

    add.u32 %r26, %r8, %r16;
    add.u32 %r27, %r9, %r17;
    setp.lt.u32 %p3, %r20, %r8;
    @%p3 bra BLACK;
    setp.ge.u32 %p4, %r20, %r26;
    @%p4 bra BLACK;
    setp.lt.u32 %p5, %r21, %r9;
    @%p5 bra BLACK;
    setp.ge.u32 %p6, %r21, %r27;
    @%p6 bra BLACK;

    sub.u32 %r28, %r20, %r8;
    sub.u32 %r29, %r21, %r9;

    cvt.u64.u32 %rd4, %r28;
    cvt.u64.u32 %rd5, %r24;
    mul.lo.u64 %rd6, %rd4, %rd5;
    cvt.u64.u32 %rd7, %r18;
    add.u64 %rd6, %rd6, %rd7;
    shr.u64 %rd6, %rd6, 16;
    cvt.u32.u64 %r22, %rd6;

    cvt.u64.u32 %rd8, %r29;
    cvt.u64.u32 %rd9, %r25;
    mul.lo.u64 %rd10, %rd8, %rd9;
    cvt.u64.u32 %rd11, %r19;
    add.u64 %rd10, %rd10, %rd11;
    shr.u64 %rd10, %rd10, 16;
    cvt.u32.u64 %r23, %rd10;

    sub.u32 %r57, %r1, 1;
    setp.ge.u32 %p7, %r22, %r1;
    @%p7 mov.u32 %r22, %r57;
    sub.u32 %r58, %r2, 1;
    setp.ge.u32 %p8, %r23, %r2;
    @%p8 mov.u32 %r23, %r58;

    mad.lo.u32 %r30, %r23, %r5, %r22;
    cvt.u64.u32 %rd12, %r30;
    add.u64 %rd12, %rd1, %rd12;
    ld.global.u8 %r31, [%rd12];

    shr.u32 %r32, %r23, 1;
    and.b32 %r33, %r22, -2;
    mad.lo.u32 %r34, %r32, %r6, %r33;
    cvt.u64.u32 %rd13, %r34;
    add.u64 %rd13, %rd2, %rd13;
    ld.global.u8 %r35, [%rd13];
    add.u64 %rd14, %rd13, 1;
    ld.global.u8 %r36, [%rd14];

    sub.s32 %r37, %r31, 16;
    max.s32 %r37, %r37, 0;
    sub.s32 %r38, %r35, 128;
    sub.s32 %r39, %r36, 128;

    mul.lo.s32 %r40, %r37, 298;

    mul.lo.s32 %r41, %r39, 459;
    add.s32 %r41, %r41, %r40;
    add.s32 %r41, %r41, 128;
    shr.s32 %r41, %r41, 8;
    max.s32 %r41, %r41, 0;
    min.s32 %r41, %r41, 255;

    mul.lo.s32 %r42, %r38, 55;
    sub.s32 %r42, %r40, %r42;
    mul.lo.s32 %r43, %r39, 136;
    sub.s32 %r42, %r42, %r43;
    add.s32 %r42, %r42, 128;
    shr.s32 %r42, %r42, 8;
    max.s32 %r42, %r42, 0;
    min.s32 %r42, %r42, 255;

    mul.lo.s32 %r44, %r38, 541;
    add.s32 %r44, %r44, %r40;
    add.s32 %r44, %r44, 128;
    shr.s32 %r44, %r44, 8;
    max.s32 %r44, %r44, 0;
    min.s32 %r44, %r44, 255;

    bra STORE;

BLACK:
    mov.u32 %r41, 0;
    mov.u32 %r42, 0;
    mov.u32 %r44, 0;

STORE:
    shl.b32 %r50, %r20, 2;
    mad.lo.u32 %r51, %r21, %r7, %r50;
    cvt.u64.u32 %rd15, %r51;
    add.u64 %rd15, %rd3, %rd15;
    st.global.u8 [%rd15], %r41;
    add.u64 %rd16, %rd15, 1;
    st.global.u8 [%rd16], %r42;
    add.u64 %rd16, %rd15, 2;
    st.global.u8 [%rd16], %r44;
    add.u64 %rd16, %rd15, 3;
    mov.u32 %r52, 255;
    st.global.u8 [%rd16], %r52;

DONE:
    ret;
}
)ptx";

const char*
bool_to_string(bool value)
{
    return value ? "true" : "false";
}

const char*
cuda_error_name(CUresult result)
{
    const char* name = nullptr;
    if (CuGetErrorName(result, &name) == CUDA_SUCCESS && name != nullptr)
        return name;
    return "unknown";
}

bool
ensure_cuda_driver_initialized(std::string& diagnostic)
{
    /*
     * NVDEC same-GPU proof uses the CUDA driver API to map the selected DRM
     * render node's PCI address to a CUDA ordinal. cuDeviceGetCount() is not
     * guaranteed to initialize the driver implicitly, so make the boundary
     * explicit and report the real driver failure if initialization is blocked.
     */
    const CUresult result = CuInit(0);
    if (result == CUDA_SUCCESS)
        return true;

    diagnostic = std::string("cuInit failed: ") + cuda_error_name(result);
    return false;
}

std::optional<guint>
cuda_device_id_for_pci_address(const char* pci_address, std::string& diagnostic)
{
    unsigned int domain = 0;
    unsigned int bus = 0;
    unsigned int device = 0;
    unsigned int function = 0;
    if (!pci_address || std::sscanf(pci_address,
                                    "%x:%x:%x.%x",
                                    &domain,
                                    &bus,
                                    &device,
                                    &function) != 4) {
        diagnostic = std::string("invalid PCI address '") +
            (pci_address && *pci_address ? pci_address : "(empty)") + "'";
        return std::nullopt;
    }

    if (!ensure_cuda_driver_initialized(diagnostic))
        return std::nullopt;

    int count = 0;
    CUresult result = CuDeviceGetCount(&count);
    if (result != CUDA_SUCCESS) {
        diagnostic = std::string("cuDeviceGetCount failed: ") + cuda_error_name(result);
        return std::nullopt;
    }

    for (int ordinal = 0; ordinal < count; ordinal++) {
        CUdevice cuda_device = 0;
        result = CuDeviceGet(&cuda_device, ordinal);
        if (result != CUDA_SUCCESS)
            continue;

        int cuda_domain = 0;
        int cuda_bus = 0;
        int cuda_device_number = 0;
        if (CuDeviceGetAttribute(&cuda_domain,
                                 CU_DEVICE_ATTRIBUTE_PCI_DOMAIN_ID,
                                 cuda_device) != CUDA_SUCCESS ||
            CuDeviceGetAttribute(&cuda_bus,
                                 CU_DEVICE_ATTRIBUTE_PCI_BUS_ID,
                                 cuda_device) != CUDA_SUCCESS ||
            CuDeviceGetAttribute(&cuda_device_number,
                                 CU_DEVICE_ATTRIBUTE_PCI_DEVICE_ID,
                                 cuda_device) != CUDA_SUCCESS) {
            continue;
        }

        if (static_cast<unsigned int>(cuda_domain) == domain &&
            static_cast<unsigned int>(cuda_bus) == bus &&
            static_cast<unsigned int>(cuda_device_number) == device) {
            return static_cast<guint>(ordinal);
        }
    }

    char message[128] = {0};
    std::snprintf(message,
                  sizeof(message),
                  "no CUDA device matches PCI %04x:%02x:%02x.%x",
                  domain,
                  bus,
                  device,
                  function);
    diagnostic = message;
    return std::nullopt;
}

std::optional<guint>
cuda_device_id_from_context(GstCudaContext* cuda_context)
{
    if (!cuda_context ||
        !g_object_class_find_property(G_OBJECT_GET_CLASS(cuda_context), "cuda-device-id")) {
        return std::nullopt;
    }

    guint cuda_device_id = G_MAXUINT;
    g_object_get(cuda_context, "cuda-device-id", &cuda_device_id, nullptr);
    if (cuda_device_id == G_MAXUINT)
        return std::nullopt;
    return cuda_device_id;
}

bool
cuda_context_matches_expected(GstCudaContext* cuda_context,
                              int             expected_cuda_device_id,
                              const char*     owner_label)
{
    if (expected_cuda_device_id < 0)
        return true;

    const std::optional<guint> actual = cuda_device_id_from_context(cuda_context);
    if (!actual.has_value()) {
        g_warning("%s: CUDA context has no readable cuda-device-id; refusing "
                  "NVIDIA decode because the selected GPU cannot be proven",
                  owner_label ? owner_label : "VividVideoProducer");
        return false;
    }

    if (actual.value() != static_cast<guint>(expected_cuda_device_id)) {
        g_warning("%s: CUDA context device mismatch expected=%d actual=%u; "
                  "refusing cross-GPU NVIDIA decode",
                  owner_label ? owner_label : "VividVideoProducer",
                  expected_cuda_device_id,
                  actual.value());
        return false;
    }

    return true;
}

bool
decoder_element_matches_expected_cuda_device(GstElement* decoder,
                                             int         expected_cuda_device_id)
{
    if (expected_cuda_device_id < 0)
        return true;

    if (!decoder ||
        !g_object_class_find_property(G_OBJECT_GET_CLASS(decoder), "cuda-device-id")) {
        g_warning("VividVideoProducer: NVIDIA decoder exposes no cuda-device-id; "
                  "refusing decode because selected GPU cannot be proven");
        return false;
    }

    guint actual_cuda_device_id = G_MAXUINT;
    g_object_get(decoder, "cuda-device-id", &actual_cuda_device_id, nullptr);
    if (actual_cuda_device_id != static_cast<guint>(expected_cuda_device_id)) {
        g_warning("VividVideoProducer: NVIDIA decoder CUDA device mismatch "
                  "expected=%d actual=%u; refusing cross-GPU decode",
                  expected_cuda_device_id,
                  actual_cuda_device_id);
        return false;
    }
    return true;
}

guint
element_factory_rank(const char* name)
{
    GstElementFactory* factory = gst_element_factory_find(name);
    if (!factory)
        return GST_RANK_NONE;
    const guint rank = gst_plugin_feature_get_rank(GST_PLUGIN_FEATURE(factory));
    gst_object_unref(factory);
    return rank;
}

bool
has_element_factory(const char* name)
{
    GstElementFactory* factory = gst_element_factory_find(name);
    if (!factory)
        return false;
    gst_object_unref(factory);
    return true;
}

bool
ends_with(std::string_view value, std::string_view suffix)
{
    return value.size() >= suffix.size() &&
        value.substr(value.size() - suffix.size(), suffix.size()) == suffix;
}

enum class VideoCodecKind
{
    Unknown,
    H264,
    Mpeg4,
};

const char*
video_codec_kind_name(VideoCodecKind kind)
{
    switch (kind) {
    case VideoCodecKind::H264:
        return "h264";
    case VideoCodecKind::Mpeg4:
        return "mpeg4-part2";
    case VideoCodecKind::Unknown:
        break;
    }
    return "unknown";
}

VideoCodecKind
video_codec_kind_from_caps(const GstCaps* caps)
{
    if (!caps || gst_caps_is_empty(caps) || gst_caps_is_any(caps))
        return VideoCodecKind::Unknown;

    const GstStructure* structure = gst_caps_get_structure(caps, 0);
    const char* name = structure ? gst_structure_get_name(structure) : nullptr;
    if (!name)
        return VideoCodecKind::Unknown;

    if (g_str_equal(name, "video/x-h264"))
        return VideoCodecKind::H264;

    if (g_str_equal(name, "video/mpeg")) {
        gint mpegversion = 0;
        gboolean systemstream = TRUE;
        if (gst_structure_get_int(structure, "mpegversion", &mpegversion) &&
            mpegversion == 4 &&
            gst_structure_get_boolean(structure, "systemstream", &systemstream) &&
            !systemstream)
            return VideoCodecKind::Mpeg4;
    }

    return VideoCodecKind::Unknown;
}

void
set_plugin_decoder_ranks(const char* plugin_name, guint rank, bool use_stateless)
{
    GstRegistry* registry = gst_registry_get();
    if (!registry)
        return;

    GList* features = gst_registry_get_feature_list_by_plugin(registry, plugin_name);
    for (GList* it = features; it; it = it->next) {
        GstPluginFeature* feature = GST_PLUGIN_FEATURE(it->data);
        if (!feature)
            continue;

        const char* name = gst_plugin_feature_get_name(feature);
        if (!name)
            continue;

        std::string_view feature_name(name);
        if (!ends_with(feature_name, "dec") && !ends_with(feature_name, "postproc"))
            continue;

        const bool is_stateless = feature_name.find("sl") != std::string_view::npos;
        if (is_stateless != use_stateless)
            continue;

        if (gst_plugin_feature_get_rank(feature) != rank)
            gst_plugin_feature_set_rank(feature, rank);
    }
    gst_plugin_feature_list_free(features);
}

std::string
va_element_factory_name(const char* render_node, const char* element_suffix)
{
    const std::string_view node = render_node ? render_node : "";
    constexpr std::string_view default_node = "/dev/dri/renderD128";
    if (node.empty() || node == default_node)
        return std::string("va") + element_suffix;

    const auto slash = node.rfind('/');
    const std::string_view basename =
        slash == std::string_view::npos ? node : node.substr(slash + 1);
    return std::string("va").append(basename).append(element_suffix);
}

std::vector<std::string>
va_element_factory_candidates(const char* render_node, const char* element_suffix)
{
    std::vector<std::string> candidates;
    const std::string node_factory = va_element_factory_name(render_node, element_suffix);
    candidates.push_back(node_factory);
    const std::string plain_factory = std::string("va") + element_suffix;
    if (plain_factory != node_factory)
        candidates.push_back(plain_factory);
    return candidates;
}

std::string
element_device_path(const char* factory_name)
{
    GstElement* element = gst_element_factory_make(factory_name, nullptr);
    if (!element)
        return {};

    std::string path;
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(element), "device-path")) {
        gchar* raw_path = nullptr;
        g_object_get(element, "device-path", &raw_path, nullptr);
        if (raw_path) {
            path = raw_path;
            g_free(raw_path);
        }
    }
    gst_object_unref(element);
    return path;
}

struct VaElementSelection
{
    std::string factory_name;
    std::string device_path;
    guint rank { GST_RANK_NONE };
};

std::optional<VaElementSelection>
select_va_element_factory_for_render_node(const char* render_node,
                                          const char* element_suffix,
                                          bool        require_positive_rank,
                                          std::string& diagnostic)
{
    const std::string selected_node = render_node && *render_node ? render_node : "";
    std::vector<std::string> details;

    if (selected_node.empty()) {
        diagnostic = "selected render-node is empty";
        return std::nullopt;
    }

    /*
     * The va plugin does not always name the selected render node as
     * "varenderDxxx...": on machines where only one VA device is exposed, the
     * plain "vah264dec"/"vapostproc" factories can point at /dev/dri/renderD129.
     * Validate the readable "device-path" property before accepting any factory
     * so a convenient plain name cannot silently decode on a different GPU.
     */
    for (const std::string& candidate :
         va_element_factory_candidates(render_node, element_suffix)) {
        const guint rank = element_factory_rank(candidate.c_str());
        const std::string device_path = element_device_path(candidate.c_str());
        gchar* detail = g_strdup_printf("%s(rank=%u device-path=%s)",
                                        candidate.c_str(),
                                        rank,
                                        device_path.empty() ? "(none)" : device_path.c_str());
        details.emplace_back(detail ? detail : "");
        g_free(detail);

        if (!has_element_factory(candidate.c_str()))
            continue;
        if (require_positive_rank && rank == GST_RANK_NONE)
            continue;
        if (device_path != selected_node)
            continue;
        return VaElementSelection {
            .factory_name = candidate,
            .device_path = device_path,
            .rank = rank,
        };
    }

    diagnostic.clear();
    for (std::size_t i = 0; i < details.size(); i++) {
        if (i > 0)
            diagnostic += ", ";
        diagnostic += details[i];
    }
    return std::nullopt;
}

bool
is_cuda_transfer_path(VideoFrameTransferPath path)
{
    return path == VideoFrameTransferPath::CudaNv12;
}

const char*
video_transfer_path_name(VideoFrameTransferPath path)
{
    switch (path) {
    case VideoFrameTransferPath::CudaNv12: return "cuda-nv12-to-vulkan-dmabuf";
    case VideoFrameTransferPath::VaMemoryBgra: return "va-bgra-to-vulkan-dmabuf";
    case VideoFrameTransferPath::None: return "none";
    }
    return "none";
}

VideoFillMode
parse_fill_mode(int value)
{
    switch (value) {
    case 0: return VideoFillMode::Stretch;
    case 1: return VideoFillMode::AspectFit;
    case 3: return VideoFillMode::ScaleDown;
    case 2:
    default: return VideoFillMode::AspectCrop;
    }
}

const char*
fill_mode_name(VideoFillMode mode)
{
    switch (mode) {
    case VideoFillMode::Stretch: return "stretch";
    case VideoFillMode::AspectFit: return "fit";
    case VideoFillMode::AspectCrop: return "cover";
    case VideoFillMode::ScaleDown: return "scale-down";
    }
    return "cover";
}

bool
video_export_requests_equal(const VividVideoVulkanExportRequest& a,
                            const VividVideoVulkanExportRequest& b)
{
    return a.fourcc == b.fourcc &&
        a.modifier == b.modifier &&
        a.plane_count == b.plane_count &&
        a.require_modifier == b.require_modifier &&
        a.memory == b.memory;
}

VividVideoVulkanExportMemory
video_export_memory_from_request(VividVideoProducerDmaBufMemoryPreference preference)
{
    return preference == VIVID_VIDEO_PRODUCER_DMABUF_MEMORY_DEVICE_LOCAL
        ? VividVideoVulkanExportMemory::DeviceLocal
        : VividVideoVulkanExportMemory::HostVisible;
}

guint32
video_target_fourcc_for_transfer_path(VideoFrameTransferPath)
{
    return DRM_FORMAT_ABGR8888;
}

VividVideoVulkanExportRequest
video_export_request_from_abi(const VividVideoProducerDmaBufRequest* request,
                              VideoFrameTransferPath transfer_path)
{
    VividVideoVulkanExportRequest export_request;
    export_request.fourcc = video_target_fourcc_for_transfer_path(transfer_path);
    export_request.modifier = DRM_FORMAT_MOD_LINEAR;
    export_request.plane_count = 1;
    export_request.require_modifier = true;
    export_request.memory = VividVideoVulkanExportMemory::HostVisible;
    if (request) {
        export_request.fourcc = request->fourcc != 0
            ? request->fourcc
            : video_target_fourcc_for_transfer_path(transfer_path);
        export_request.modifier = request->require_modifier
            ? request->modifier
            : DRM_FORMAT_MOD_LINEAR;
        export_request.plane_count = request->plane_count > 0 ? request->plane_count : 1;
        export_request.require_modifier = request->require_modifier;
        export_request.memory = video_export_memory_from_request(request->memory_preference);
    }
    return export_request;
}

struct VideoFitParameters
{
    uint32_t dst_x { 0 };
    uint32_t dst_y { 0 };
    uint32_t draw_width { 1 };
    uint32_t draw_height { 1 };
    uint32_t src_x0_fp { 0 };
    uint32_t src_y0_fp { 0 };
    uint32_t step_x_fp { 1u << 16 };
    uint32_t step_y_fp { 1u << 16 };
};

uint32_t
clamped_round_to_u32(double value, uint32_t minimum, uint32_t maximum)
{
    if (!std::isfinite(value))
        return minimum;
    value = std::clamp(value, static_cast<double>(minimum), static_cast<double>(maximum));
    return static_cast<uint32_t>(std::llround(value));
}

uint32_t
double_to_fixed_16(double value, gboolean allow_zero)
{
    if (!std::isfinite(value) || value < 0.0)
        return allow_zero ? 0 : 1;
    if (value == 0.0)
        return allow_zero ? 0 : 1;
    if (value < (1.0 / 65536.0))
        return 1;
    const double fixed = std::min(value * 65536.0,
                                  static_cast<double>(std::numeric_limits<uint32_t>::max()));
    return static_cast<uint32_t>(std::llround(fixed));
}

VideoFitParameters
compute_video_fit_parameters(VideoFillMode mode,
                             uint32_t src_width,
                             uint32_t src_height,
                             uint32_t dst_width,
                             uint32_t dst_height)
{
    VideoFitParameters params;
    src_width = std::max(src_width, 1u);
    src_height = std::max(src_height, 1u);
    dst_width = std::max(dst_width, 1u);
    dst_height = std::max(dst_height, 1u);

    double src_x = 0.0;
    double src_y = 0.0;
    double sample_width = static_cast<double>(src_width);
    double sample_height = static_cast<double>(src_height);
    params.draw_width = dst_width;
    params.draw_height = dst_height;

    const double src_aspect = sample_width / sample_height;
    const double dst_aspect =
        static_cast<double>(dst_width) / static_cast<double>(dst_height);

    if (mode == VideoFillMode::AspectFit || mode == VideoFillMode::ScaleDown) {
        double scale = std::min(static_cast<double>(dst_width) / sample_width,
                                static_cast<double>(dst_height) / sample_height);
        if (mode == VideoFillMode::ScaleDown)
            scale = std::min(scale, 1.0);
        params.draw_width =
            clamped_round_to_u32(sample_width * scale, 1, dst_width);
        params.draw_height =
            clamped_round_to_u32(sample_height * scale, 1, dst_height);
        params.dst_x = (dst_width - params.draw_width) / 2u;
        params.dst_y = (dst_height - params.draw_height) / 2u;
    } else if (mode == VideoFillMode::AspectCrop) {
        if (src_aspect > dst_aspect) {
            sample_width = static_cast<double>(src_height) * dst_aspect;
            sample_width = std::clamp(sample_width, 1.0, static_cast<double>(src_width));
            src_x = (static_cast<double>(src_width) - sample_width) * 0.5;
        } else {
            sample_height = static_cast<double>(src_width) / dst_aspect;
            sample_height = std::clamp(sample_height, 1.0, static_cast<double>(src_height));
            src_y = (static_cast<double>(src_height) - sample_height) * 0.5;
        }
    }

    params.src_x0_fp = double_to_fixed_16(src_x, TRUE);
    params.src_y0_fp = double_to_fixed_16(src_y, TRUE);
    params.step_x_fp = double_to_fixed_16(sample_width / params.draw_width, FALSE);
    params.step_y_fp = double_to_fixed_16(sample_height / params.draw_height, FALSE);
    return params;
}

void
configure_decoder_ranks(VividGpuDecoderRoute route)
{
    constexpr guint preferred_rank = GST_RANK_PRIMARY + 4;

    if (route == VIVID_GPU_DECODER_ROUTE_VA) {
        set_plugin_decoder_ranks("va", preferred_rank, false);
        set_plugin_decoder_ranks("nvcodec", GST_RANK_NONE, false);
        set_plugin_decoder_ranks("nvcodec", GST_RANK_NONE, true);
        return;
    }

    /*
     * NVIDIA route: stateful nvh264dec is the stable default, the stateless
     * decoder stays enabled one rank below as an alternate decoder for the
     * same selected CUDA device when the stateful element is missing from this
     * nvcodec build.
     */
    set_plugin_decoder_ranks("va", GST_RANK_NONE, false);
    set_plugin_decoder_ranks("nvcodec", preferred_rank + 1, false);
    set_plugin_decoder_ranks("nvcodec", preferred_rank, true);
}

constexpr const char* kCudaNv12SinkCaps =
    "video/x-raw(memory:CUDAMemory),format=(string)NV12";
constexpr const char* kVaBgraSinkCaps =
    "video/x-raw(memory:VAMemory),format=(string)BGRA";

struct VideoDecoderSelection
{
    VideoFrameTransferPath transfer_path { VideoFrameTransferPath::None };
    std::string decoder_factory;
    std::string parser_factory;
    std::string postproc_factory;
    std::string decoder_device_path;
    std::string postproc_device_path;
    const char* sink_caps { nullptr };
    int expected_cuda_device_id { -1 };
    const char* decode_label { nullptr };
};

std::optional<VideoDecoderSelection>
select_video_decoder(VividGpuDecoderRoute route,
                     const char*           render_node,
                     const char*           pci_address,
                     VideoCodecKind        codec)
{
    configure_decoder_ranks(route);

    if (route == VIVID_GPU_DECODER_ROUTE_NVIDIA) {
        const bool has_cuda_loader = gst_cuda_load_library();
        std::string cuda_diagnostic;
        const auto expected_cuda_device =
            has_cuda_loader
                ? cuda_device_id_for_pci_address(pci_address, cuda_diagnostic)
                : std::optional<guint> {};

        /*
         * NVDEC does not expose a writable render-node/device-path property like
         * VA. The only reliable same-card proof available here is the CUDA
         * ordinal mapped from the selected GPU's PCI address; if we cannot
         * derive it, or if the decoder later reports a different ordinal, the
         * NVIDIA path must fail instead of decoding on CUDA device 0 by habit.
         */
        if (has_cuda_loader && !expected_cuda_device.has_value()) {
            g_warning("VividVideoProducer: selected NVIDIA device %s (%s) has "
                      "no matching CUDA device id: %s; CPU fallback is disabled",
                      render_node && *render_node ? render_node : "(unresolved)",
                      pci_address && *pci_address ? pci_address : "(unknown-pci)",
                      cuda_diagnostic.empty() ? "(unknown)" : cuda_diagnostic.c_str());
            return std::nullopt;
        }

        if (codec == VideoCodecKind::H264) {
            const guint nvh264_rank = element_factory_rank("nvh264dec");
            const guint nvh264sl_rank = element_factory_rank("nvh264sldec");

            if (has_cuda_loader && nvh264_rank > GST_RANK_NONE) {
                return VideoDecoderSelection {
                    .transfer_path = VideoFrameTransferPath::CudaNv12,
                    .decoder_factory = "nvh264dec",
                    .parser_factory = "h264parse",
                    .postproc_factory = {},
                    .sink_caps = kCudaNv12SinkCaps,
                    .expected_cuda_device_id =
                        static_cast<int>(expected_cuda_device.value()),
                    .decode_label = "nvdec-hardware",
                };
            }
            if (has_cuda_loader && nvh264sl_rank > GST_RANK_NONE) {
                g_message("VividVideoProducer: stateful nvh264dec is unavailable; "
                          "using stateless nvh264sldec on the selected CUDA device");
                return VideoDecoderSelection {
                    .transfer_path = VideoFrameTransferPath::CudaNv12,
                    .decoder_factory = "nvh264sldec",
                    .parser_factory = "h264parse",
                    .postproc_factory = {},
                    .sink_caps = kCudaNv12SinkCaps,
                    .expected_cuda_device_id =
                        static_cast<int>(expected_cuda_device.value()),
                    .decode_label = "nvdec-hardware",
                };
            }
            g_warning("VividVideoProducer: NVDEC decoder for the selected NVIDIA "
                      "device is unavailable cuda-loader=%s nvh264dec-rank=%u "
                      "nvh264sldec-rank=%u pci=%s; CPU fallback is disabled",
                      bool_to_string(has_cuda_loader),
                      nvh264_rank,
                      nvh264sl_rank,
                      pci_address && *pci_address ? pci_address : "(unknown-pci)");
            return std::nullopt;
        }

        if (codec == VideoCodecKind::Mpeg4) {
            const guint nvmpeg4_rank = element_factory_rank("nvmpeg4videodec");
            if (has_cuda_loader && nvmpeg4_rank > GST_RANK_NONE) {
                return VideoDecoderSelection {
                    .transfer_path = VideoFrameTransferPath::CudaNv12,
                    .decoder_factory = "nvmpeg4videodec",
                    .parser_factory = "mpeg4videoparse",
                    .postproc_factory = {},
                    .sink_caps = kCudaNv12SinkCaps,
                    .expected_cuda_device_id =
                        static_cast<int>(expected_cuda_device.value()),
                    .decode_label = "nvdec-hardware",
                };
            }
            g_warning("VividVideoProducer: NVDEC MPEG-4 decoder for the selected "
                      "NVIDIA device is unavailable cuda-loader=%s "
                      "nvmpeg4videodec-rank=%u pci=%s; CPU fallback is disabled",
                      bool_to_string(has_cuda_loader),
                      nvmpeg4_rank,
                      pci_address && *pci_address ? pci_address : "(unknown-pci)");
            return std::nullopt;
        }

        g_warning("VividVideoProducer: unsupported video codec %s for the "
                  "NVIDIA decoder route",
                  video_codec_kind_name(codec));
        return std::nullopt;
    }

    if (route == VIVID_GPU_DECODER_ROUTE_VA) {
        std::string postproc_diagnostic;
        const auto postproc =
            select_va_element_factory_for_render_node(render_node,
                                                      "postproc",
                                                      false,
                                                      postproc_diagnostic);
        if (!postproc.has_value()) {
            g_warning("VividVideoProducer: VA postproc for the selected device "
                      "is unavailable node=%s postproc-candidates=[%s]",
                      render_node && *render_node ? render_node : "(unresolved)",
                      postproc_diagnostic.empty() ? "(none)"
                                                  : postproc_diagnostic.c_str());
            return std::nullopt;
        }

        if (codec == VideoCodecKind::H264) {
            std::string decoder_diagnostic;
            const auto decoder =
                select_va_element_factory_for_render_node(render_node,
                                                          "h264dec",
                                                          true,
                                                          decoder_diagnostic);
            if (decoder.has_value()) {
                return VideoDecoderSelection {
                    .transfer_path = VideoFrameTransferPath::VaMemoryBgra,
                    .decoder_factory = decoder->factory_name,
                    .parser_factory = "h264parse",
                    .postproc_factory = postproc->factory_name,
                    .decoder_device_path = decoder->device_path,
                    .postproc_device_path = postproc->device_path,
                    .sink_caps = kVaBgraSinkCaps,
                    .decode_label = "va-hardware",
                };
            }
            g_warning("VividVideoProducer: VA decoder for the selected device is "
                      "unavailable node=%s decoder-candidates=[%s]; CPU fallback "
                      "is disabled",
                      render_node && *render_node ? render_node : "(unresolved)",
                      decoder_diagnostic.empty() ? "(none)"
                                                 : decoder_diagnostic.c_str());
            return std::nullopt;
        }

        if (codec == VideoCodecKind::Mpeg4) {
            /*
             * No VA driver offers MPEG-4 Part 2 decoding (the GStreamer va
             * plugin has no vampeg4dec element at all), so the GPU-only rule
             * cannot apply to this codec. Software decode is the only way to
             * play it; vapostproc still uploads and converts on the selected VA
             * device, so the device-selection guarantee is preserved.
             */
            if (!has_element_factory("avdec_mpeg4")) {
                g_warning("VividVideoProducer: MPEG-4 Part 2 has no VA hardware "
                          "decoder and the avdec_mpeg4 software decoder is also "
                          "unavailable node=%s",
                          render_node && *render_node ? render_node
                                                      : "(unresolved)");
                return std::nullopt;
            }
            g_message("VividVideoProducer: MPEG-4 Part 2 has no VA hardware "
                      "decoder on any driver; decoding on the CPU with "
                      "avdec_mpeg4 and uploading to %s via %s",
                      postproc->device_path.c_str(),
                      postproc->factory_name.c_str());
            return VideoDecoderSelection {
                .transfer_path = VideoFrameTransferPath::VaMemoryBgra,
                .decoder_factory = "avdec_mpeg4",
                .parser_factory = "mpeg4videoparse",
                .postproc_factory = postproc->factory_name,
                .decoder_device_path = {},
                .postproc_device_path = postproc->device_path,
                .sink_caps = kVaBgraSinkCaps,
                .decode_label = "cpu-software",
            };
        }

        g_warning("VividVideoProducer: unsupported video codec %s for the VA "
                  "decoder route",
                  video_codec_kind_name(codec));
        return std::nullopt;
    }

    g_warning("VividVideoProducer: selected GPU has no decoder route");
    return std::nullopt;
}

struct GstCudaMemoryPrefix
{
    GstMemory mem;
    GstCudaContext* context;
    GstVideoInfo info;
};

GstCudaContext*
cuda_context_from_memory(GstMemory* memory)
{
    if (!memory || !gst_is_cuda_memory(memory))
        return nullptr;
    return reinterpret_cast<GstCudaMemoryPrefix*>(memory)->context;
}

GstElement*
make_required_element(const char* factory_name, const char* element_name)
{
    GstElement* element = gst_element_factory_make(factory_name, element_name);
    if (!element) {
        g_warning("VividVideoProducer: GStreamer element '%s' is unavailable",
                  factory_name);
    }
    return element;
}

bool
link_element_sequence(const std::vector<GstElement*>& elements,
                      const char*                    description)
{
    for (std::size_t i = 0; i + 1 < elements.size(); i++) {
        if (!gst_element_link(elements[i], elements[i + 1])) {
            g_warning("VividVideoProducer: failed to link %s at element %zu",
                      description,
                      i);
            return false;
        }
    }
    return true;
}

struct DemuxPadLinkContext
{
    GstElement* video_queue { nullptr };
    /*
     * Non-owning: this context is stored as signal data on qtdemux, which is a
     * child of the pipeline. Holding a strong pipeline reference here creates a
     * cycle (pipeline -> demux -> signal data -> pipeline), preventing old
     * GStreamer/NVDEC pipelines from being finalized after video switches.
     */
    GstElement* pipeline { nullptr };
    GstElement** volume_element_slot { nullptr };
    gchar*      video_path { nullptr };
    gboolean    audio_linked { FALSE };
    gboolean    muted { FALSE };
    gdouble     volume { 1.0 };
};

void
demux_pad_link_context_free(gpointer data, GClosure*)
{
    auto* context = static_cast<DemuxPadLinkContext*>(data);
    if (!context)
        return;

    g_clear_object(&context->video_queue);
    g_free(context->video_path);
    g_free(context);
}

struct DecodebinPadLinkContext
{
    GstElement* audio_convert { nullptr };
    gchar*      video_path { nullptr };
};

void
decodebin_pad_link_context_free(gpointer data, GClosure*)
{
    auto* context = static_cast<DecodebinPadLinkContext*>(data);
    if (!context)
        return;

    g_clear_object(&context->audio_convert);
    g_free(context->video_path);
    g_free(context);
}

bool
pad_name_has_prefix(GstPad* pad, const char* prefix)
{
    if (!pad || !prefix)
        return false;

    gchar* name = gst_pad_get_name(pad);
    const bool matches = name && g_str_has_prefix(name, prefix);
    g_free(name);
    return matches;
}

struct VideoCodecProbe
{
    VideoCodecKind kind { VideoCodecKind::Unknown };
    std::string caps_text;
    bool probed { false };
};

struct CodecProbeState
{
    GMutex   lock;
    GCond    cond;
    GstCaps* video_caps { nullptr };
    gboolean finished { FALSE };
};

void
codec_probe_pad_added(GstElement*, GstPad* pad, gpointer user_data)
{
    auto* state = static_cast<CodecProbeState*>(user_data);
    if (!state || !pad || !pad_name_has_prefix(pad, "video_"))
        return;

    GstCaps* caps = gst_pad_get_current_caps(pad);
    if (!caps)
        caps = gst_pad_query_caps(pad, nullptr);

    g_mutex_lock(&state->lock);
    if (!state->finished && caps) {
        state->video_caps = caps;
        caps = nullptr;
        state->finished = TRUE;
        g_cond_signal(&state->cond);
    }
    g_mutex_unlock(&state->lock);

    if (caps)
        gst_caps_unref(caps);
}

void
codec_probe_no_more_pads(GstElement*, gpointer user_data)
{
    auto* state = static_cast<CodecProbeState*>(user_data);
    if (!state)
        return;

    g_mutex_lock(&state->lock);
    if (!state->finished) {
        state->finished = TRUE;
        g_cond_signal(&state->cond);
    }
    g_mutex_unlock(&state->lock);
}

/*
 * The real pipeline statically links parser and decoder before qtdemux exposes
 * pads, so the demuxed video caps must be known up front. This short-lived
 * filesrc!qtdemux pipeline only parses the container header; no decoder is
 * plugged and bus errors from the intentionally unlinked pads are irrelevant.
 */
VideoCodecProbe
probe_video_codec(const std::string& video_path)
{
    VideoCodecProbe result;

    GstElement* pipeline = gst_pipeline_new("vivid-video-codec-probe");
    GstElement* source = gst_element_factory_make("filesrc", nullptr);
    GstElement* demux = gst_element_factory_make("qtdemux", nullptr);
    if (!pipeline || !source || !demux) {
        g_warning("VividVideoProducer: codec probe elements are unavailable");
        g_clear_object(&pipeline);
        g_clear_object(&source);
        g_clear_object(&demux);
        return result;
    }

    g_object_set(source, "location", video_path.c_str(), nullptr);
    gst_bin_add_many(GST_BIN(pipeline), source, demux, nullptr);
    if (!gst_element_link(source, demux)) {
        g_warning("VividVideoProducer: codec probe failed to link filesrc to qtdemux");
        gst_object_unref(pipeline);
        return result;
    }

    CodecProbeState state {};
    g_mutex_init(&state.lock);
    g_cond_init(&state.cond);

    g_signal_connect(demux, "pad-added", G_CALLBACK(codec_probe_pad_added), &state);
    g_signal_connect(demux,
                     "no-more-pads",
                     G_CALLBACK(codec_probe_no_more_pads),
                     &state);

    if (gst_element_set_state(pipeline, GST_STATE_PAUSED) !=
        GST_STATE_CHANGE_FAILURE) {
        g_mutex_lock(&state.lock);
        const gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
        while (!state.finished) {
            if (!g_cond_wait_until(&state.cond, &state.lock, deadline))
                break;
        }
        g_mutex_unlock(&state.lock);
    } else {
        g_warning("VividVideoProducer: codec probe could not start for %s",
                  video_path.c_str());
    }

    /*
     * NULL joins the demuxer streaming thread, so after this the callbacks can
     * no longer touch the stack-allocated probe state.
     */
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);

    if (state.video_caps) {
        result.probed = true;
        result.kind = video_codec_kind_from_caps(state.video_caps);
        gchar* caps_text = gst_caps_to_string(state.video_caps);
        if (caps_text) {
            result.caps_text = caps_text;
            g_free(caps_text);
        }
        gst_caps_unref(state.video_caps);
    }

    g_mutex_clear(&state.lock);
    g_cond_clear(&state.cond);
    return result;
}

void audio_decodebin_pad_added(GstElement*, GstPad* pad, gpointer user_data);

bool
sync_audio_branch_state(const std::vector<GstElement*>& elements)
{
    for (GstElement* element : elements) {
        if (!gst_element_sync_state_with_parent(element)) {
            g_warning("VividVideoProducer: failed to sync optional audio branch "
                      "element=%s with parent state",
                      GST_ELEMENT_NAME(element));
            return false;
        }
    }
    return true;
}

bool
create_optional_audio_branch(DemuxPadLinkContext* context, GstPad* audio_pad)
{
    if (!context || !context->pipeline || !audio_pad || context->audio_linked)
        return false;

    GstElement* audio_queue = make_required_element("queue", "vivid-video-audio-queue");
    GstElement* audio_decodebin =
        make_required_element("decodebin", "vivid-video-audio-decodebin");
    GstElement* audio_convert =
        make_required_element("audioconvert", "vivid-video-audio-convert");
    GstElement* audio_resample =
        make_required_element("audioresample", "vivid-video-audio-resample");
    GstElement* volume = make_required_element("volume", "audio_volume");
    GstElement* audio_sink =
        gst_element_factory_make("autoaudiosink", "vivid-video-audio-sink");
    if (!audio_sink)
        audio_sink = make_required_element("fakesink", "vivid-video-audio-sink");

    if (!audio_queue || !audio_decodebin || !audio_convert ||
        !audio_resample || !volume || !audio_sink) {
        g_warning("VividVideoProducer: failed to create optional audio branch");
        g_clear_object(&audio_queue);
        g_clear_object(&audio_decodebin);
        g_clear_object(&audio_convert);
        g_clear_object(&audio_resample);
        g_clear_object(&volume);
        g_clear_object(&audio_sink);
        return false;
    }

    g_object_set(audio_queue,
                 "max-size-buffers",
                 0,
                 "max-size-bytes",
                 0,
                 "max-size-time",
                 static_cast<guint64>(1000000000),
                 nullptr);
    g_object_set(volume,
                 "mute",
                 context->muted,
                 "volume",
                 context->volume,
                 nullptr);

    gst_bin_add_many(GST_BIN(context->pipeline),
                     audio_queue,
                     audio_decodebin,
                     audio_convert,
                     audio_resample,
                     volume,
                     audio_sink,
                     nullptr);

    auto remove_audio_branch = [&]() {
        gst_element_set_state(audio_queue, GST_STATE_NULL);
        gst_element_set_state(audio_decodebin, GST_STATE_NULL);
        gst_element_set_state(audio_convert, GST_STATE_NULL);
        gst_element_set_state(audio_resample, GST_STATE_NULL);
        gst_element_set_state(volume, GST_STATE_NULL);
        gst_element_set_state(audio_sink, GST_STATE_NULL);
        gst_bin_remove_many(GST_BIN(context->pipeline),
                            audio_queue,
                            audio_decodebin,
                            audio_convert,
                            audio_resample,
                            volume,
                            audio_sink,
                            nullptr);
    };

    if (!gst_element_link(audio_queue, audio_decodebin) ||
        !link_element_sequence({ audio_convert, audio_resample, volume, audio_sink },
                               "optional audio chain")) {
        g_warning("VividVideoProducer: failed to link optional audio branch");
        remove_audio_branch();
        return false;
    }

    auto* decodebin_context = g_new0(DecodebinPadLinkContext, 1);
    decodebin_context->audio_convert = GST_ELEMENT(gst_object_ref(audio_convert));
    decodebin_context->video_path = g_strdup(context->video_path);
    g_signal_connect_data(audio_decodebin,
                          "pad-added",
                          G_CALLBACK(audio_decodebin_pad_added),
                          decodebin_context,
                          decodebin_pad_link_context_free,
                          GConnectFlags(0));

    /*
     * Optional audio is created only after qtdemux proves that an audio stream
     * exists. That keeps video-only MP4 files from carrying an idle sink that
     * blocks preroll, while still letting audio-capable files join the already
     * running parent pipeline by synchronizing every new element before the
     * demux pad starts pushing data into the branch.
     */
    if (!sync_audio_branch_state(
            { audio_queue, audio_decodebin, audio_convert, audio_resample, volume, audio_sink })) {
        remove_audio_branch();
        return false;
    }

    GstPad* sink_pad = gst_element_get_static_pad(audio_queue, "sink");
    if (!sink_pad) {
        remove_audio_branch();
        return false;
    }

    const GstPadLinkReturn result = gst_pad_link(audio_pad, sink_pad);
    gst_object_unref(sink_pad);
    if (result != GST_PAD_LINK_OK) {
        g_warning("VividVideoProducer: failed to link optional qtdemux audio "
                  "stream project=%s result=%s",
                  context->video_path ? context->video_path : "(unknown)",
                  gst_pad_link_get_name(result));
        remove_audio_branch();
        return false;
    }

    if (context->volume_element_slot) {
        g_clear_object(context->volume_element_slot);
        *context->volume_element_slot = GST_ELEMENT(gst_object_ref(volume));
    }
    context->audio_linked = TRUE;
    g_message("VividVideoProducer: linked optional qtdemux audio stream project=%s",
              context->video_path ? context->video_path : "(unknown)");
    return true;
}

void
demux_pad_added(GstElement*, GstPad* pad, gpointer user_data)
{
    auto* context = static_cast<DemuxPadLinkContext*>(user_data);
    if (!context || !pad)
        return;

    GstElement* target_queue = nullptr;
    const char* media_kind = "unknown";
    if (pad_name_has_prefix(pad, "video_")) {
        target_queue = context->video_queue;
        media_kind = "video";
    } else if (pad_name_has_prefix(pad, "audio_")) {
        create_optional_audio_branch(context, pad);
        return;
    }

    if (!target_queue) {
        gchar* pad_name = gst_pad_get_name(pad);
        g_message("VividVideoProducer: ignoring unsupported qtdemux pad=%s project=%s",
                  pad_name ? pad_name : "(unknown)",
                  context->video_path ? context->video_path : "(unknown)");
        g_free(pad_name);
        return;
    }

    GstPad* sink_pad = gst_element_get_static_pad(target_queue, "sink");
    if (!sink_pad)
        return;

    if (gst_pad_is_linked(sink_pad)) {
        g_message("VividVideoProducer: ignoring duplicate %s stream project=%s",
                  media_kind,
                  context->video_path ? context->video_path : "(unknown)");
        gst_object_unref(sink_pad);
        return;
    }

    const GstPadLinkReturn result = gst_pad_link(pad, sink_pad);
    if (result == GST_PAD_LINK_OK) {
        g_message("VividVideoProducer: linked optional qtdemux %s stream project=%s",
                  media_kind,
                  context->video_path ? context->video_path : "(unknown)");
    } else {
        gchar* pad_name = gst_pad_get_name(pad);
        g_warning("VividVideoProducer: failed to link qtdemux %s pad=%s "
                  "project=%s result=%s",
                  media_kind,
                  pad_name ? pad_name : "(unknown)",
                  context->video_path ? context->video_path : "(unknown)",
                  gst_pad_link_get_name(result));
        g_free(pad_name);
    }

    gst_object_unref(sink_pad);
}

void
audio_decodebin_pad_added(GstElement*, GstPad* pad, gpointer user_data)
{
    auto* context = static_cast<DecodebinPadLinkContext*>(user_data);
    if (!context || !pad || !context->audio_convert)
        return;

    GstPad* sink_pad = gst_element_get_static_pad(context->audio_convert, "sink");
    if (!sink_pad)
        return;

    if (gst_pad_is_linked(sink_pad)) {
        gst_object_unref(sink_pad);
        return;
    }

    const GstPadLinkReturn result = gst_pad_link(pad, sink_pad);
    if (result != GST_PAD_LINK_OK) {
        gchar* caps_text = nullptr;
        GstCaps* caps = gst_pad_get_current_caps(pad);
        if (!caps)
            caps = gst_pad_query_caps(pad, nullptr);
        if (caps) {
            caps_text = gst_caps_to_string(caps);
            gst_caps_unref(caps);
        }
        g_warning("VividVideoProducer: failed to link decoded audio pad "
                  "project=%s caps=%s result=%s",
                  context->video_path ? context->video_path : "(unknown)",
                  caps_text ? caps_text : "(unknown)",
                  gst_pad_link_get_name(result));
        g_free(caps_text);
    }

    gst_object_unref(sink_pad);
}

} // namespace

struct _VividVideoProducer
{
    std::string video_path;
    std::string render_device { "auto" };
    VividGpuDevice gpu_device {};
    bool gpu_device_valid { false };
    VividGpuDecoderRoute decoder_route { VIVID_GPU_DECODER_ROUTE_NONE };
    VideoFrameTransferPath transfer_path { VideoFrameTransferPath::None };
    VideoCodecKind video_codec { VideoCodecKind::Unknown };
    std::string decoder_factory;
    std::string parser_factory;
    std::string postproc_factory;
    std::string decoder_device_path;
    std::string postproc_device_path;
    const char* sink_caps { nullptr };
    const char* decode_label { nullptr };
    int expected_cuda_device_id { -1 };

    bool configured { false };
    bool playing { true };
    bool muted { false };
    double volume { 1.0 };
    VideoFillMode fill_mode { VideoFillMode::AspectCrop };
    int fps { 30 };

    GstElement* pipeline { nullptr };
    GstElement* source { nullptr };
    GstElement* appsink { nullptr };
    GstElement* volume_element { nullptr };
    GstBus* bus { nullptr };
    bool pipeline_failed { false };
    bool logged_waiting_for_sample { false };
    bool logged_unexpected_caps { false };
    guint64 uploaded_samples { 0 };
    guint64 eos_count { 0 };

    VividVideoVulkanBackend vulkan;
    VividVideoVulkanExportRequest export_request {};
    VividVideoCudaExternalBuffer cuda_rgba_buffer;
    CUmodule cuda_module {};
    CUfunction cuda_kernel {};

    guint32 width { 0 };
    guint32 height { 0 };
    double render_scale { 1.0 };
    ProducerFrameRoute frame_route { "VividVideoProducer" };
    VividRendererReleaseGate release_gate {};
    bool release_gate_valid { false };
    bool vulkan_ready { false };
    bool logged_first_upload { false };
};

namespace
{

void
free_cuda_kernel(VividVideoProducer* self)
{
    if (!self || !self->cuda_module)
        return;

    bool pushed = false;
    if (self->cuda_rgba_buffer.cuda_context)
        pushed = gst_cuda_context_push(self->cuda_rgba_buffer.cuda_context);
    (void)CuModuleUnload(self->cuda_module);
    if (pushed) {
        CUcontext popped {};
        (void)gst_cuda_context_pop(&popped);
    }
    self->cuda_module = nullptr;
    self->cuda_kernel = nullptr;
}

void
stop_pipeline(VividVideoProducer* self)
{
    if (!self)
        return;

    if (self->pipeline) {
        gst_element_set_state(self->pipeline, GST_STATE_NULL);
        /*
         * Hardware decoders may release CUDA/NVIDIA allocations from their state
         * transition handlers. Waiting here makes teardown ordering explicit before
         * the producer drops its last pipeline reference and switches projects.
         */
        (void)gst_element_get_state(self->pipeline, nullptr, nullptr, 2 * GST_SECOND);
    }

    if (self->bus)
        gst_object_unref(self->bus);
    if (self->appsink)
        gst_object_unref(self->appsink);
    if (self->source)
        gst_object_unref(self->source);
    if (self->volume_element)
        gst_object_unref(self->volume_element);
    if (self->pipeline)
        gst_object_unref(self->pipeline);

    self->bus = nullptr;
    self->appsink = nullptr;
    self->source = nullptr;
    self->volume_element = nullptr;
    self->pipeline = nullptr;
    self->pipeline_failed = false;
    self->logged_waiting_for_sample = false;
    self->logged_unexpected_caps = false;
}

void
reset_vulkan(VividVideoProducer* self)
{
    if (!self)
        return;

    free_cuda_kernel(self);
    self->cuda_rgba_buffer.reset();
    self->vulkan.reset();
    self->export_request = {};
    self->vulkan_ready = false;
    self->width = 0;
    self->height = 0;
    self->render_scale = 1.0;
    self->logged_first_upload = false;
}

void
apply_audio_state(VividVideoProducer* self)
{
    if (!self || !self->volume_element)
        return;

    g_object_set(self->volume_element,
                 "mute",
                 self->muted,
                 "volume",
                 self->volume,
                 nullptr);
}

void
apply_playback_state(VividVideoProducer* self)
{
    if (!self || !self->pipeline)
        return;

    gst_element_set_state(self->pipeline,
                          self->playing ? GST_STATE_PLAYING : GST_STATE_PAUSED);
}

bool
ensure_pipeline(VividVideoProducer* self)
{
    if (!self || self->pipeline)
        return self && !self->pipeline_failed;
    if (self->decoder_factory.empty() || self->parser_factory.empty() ||
        !self->sink_caps || self->transfer_path == VideoFrameTransferPath::None)
        return false;

    GstCaps* sink_caps = gst_caps_from_string(self->sink_caps);
    if (!sink_caps) {
        g_warning("VividVideoProducer: invalid GPU video sink caps '%s'",
                  self->sink_caps);
        self->pipeline_failed = true;
        return false;
    }

    GstElement* pipeline = gst_pipeline_new("vivid-video-pipeline");
    GstElement* source = make_required_element("filesrc", "src");
    GstElement* demux = make_required_element("qtdemux", "demux");
    GstElement* video_queue = make_required_element("queue", "vivid-video-queue");
    GstElement* parser =
        make_required_element(self->parser_factory.c_str(), "vivid-video-parser");
    GstElement* decoder =
        make_required_element(self->decoder_factory.c_str(), "vivid-video-decoder");
    GstElement* postproc = self->transfer_path == VideoFrameTransferPath::VaMemoryBgra
        ? make_required_element(self->postproc_factory.c_str(), "vivid-video-vapostproc")
        : nullptr;
    GstElement* video_caps = make_required_element("capsfilter", "vivid-video-caps");
    GstElement* appsink = make_required_element("appsink", "video_sink");

    if (!pipeline || !source || !demux || !video_queue || !parser || !decoder ||
        (self->transfer_path == VideoFrameTransferPath::VaMemoryBgra && !postproc) ||
        !video_caps || !appsink) {
        g_warning("VividVideoProducer: failed to create GPU video pipeline elements");
        g_clear_object(&pipeline);
        g_clear_object(&source);
        g_clear_object(&demux);
        g_clear_object(&video_queue);
        g_clear_object(&parser);
        g_clear_object(&decoder);
        g_clear_object(&postproc);
        g_clear_object(&video_caps);
        g_clear_object(&appsink);
        gst_caps_unref(sink_caps);
        self->pipeline_failed = true;
        return false;
    }
    if (!decoder_element_matches_expected_cuda_device(decoder,
                                                      self->expected_cuda_device_id)) {
        g_clear_object(&pipeline);
        g_clear_object(&source);
        g_clear_object(&demux);
        g_clear_object(&video_queue);
        g_clear_object(&parser);
        g_clear_object(&decoder);
        g_clear_object(&postproc);
        g_clear_object(&video_caps);
        g_clear_object(&appsink);
        gst_caps_unref(sink_caps);
        self->pipeline_failed = true;
        return false;
    }

    g_object_set(source, "location", self->video_path.c_str(), nullptr);
    g_object_set(video_queue,
                 "max-size-buffers",
                 3,
                 "max-size-bytes",
                 0,
                 "max-size-time",
                 0,
                 nullptr);
    g_object_set(video_caps, "caps", sink_caps, nullptr);
    g_object_set(appsink,
                 "caps",
                 sink_caps,
                 "sync",
                 TRUE,
                 "max-buffers",
                 1,
                 "drop",
                 TRUE,
                 nullptr);
    /*
     * Audio is optional for Wallpaper Engine video projects. The audio branch is
     * not added here; qtdemux's pad-added callback creates it only when an audio
     * pad actually exists. This keeps video-only MP4 files from waiting forever
     * in preroll on an idle audio sink.
     */
    gst_caps_unref(sink_caps);

    gst_bin_add_many(GST_BIN(pipeline),
                     source,
                     demux,
                     video_queue,
                     parser,
                     decoder,
                     video_caps,
                     appsink,
                     nullptr);
    if (postproc)
        gst_bin_add(GST_BIN(pipeline), postproc);

    if (!gst_element_link(source, demux)) {
        g_warning("VividVideoProducer: failed to link video source to qtdemux");
        gst_object_unref(pipeline);
        self->pipeline_failed = true;
        return false;
    }

    std::vector<GstElement*> video_elements { video_queue, parser, decoder };
    if (postproc)
        video_elements.push_back(postproc);
    video_elements.push_back(video_caps);
    video_elements.push_back(appsink);
    if (!link_element_sequence(video_elements, "GPU video chain")) {
        gst_object_unref(pipeline);
        self->pipeline_failed = true;
        return false;
    }

    auto* demux_context = g_new0(DemuxPadLinkContext, 1);
    demux_context->video_queue = GST_ELEMENT(gst_object_ref(video_queue));
    demux_context->pipeline = pipeline;
    demux_context->volume_element_slot = &self->volume_element;
    demux_context->video_path = g_strdup(self->video_path.c_str());
    demux_context->muted = self->muted;
    demux_context->volume = self->volume;
    g_signal_connect_data(demux,
                          "pad-added",
                          G_CALLBACK(demux_pad_added),
                          demux_context,
                          demux_pad_link_context_free,
                          GConnectFlags(0));

    self->pipeline = pipeline;
    self->source = gst_bin_get_by_name(GST_BIN(self->pipeline), "src");
    self->appsink = gst_bin_get_by_name(GST_BIN(self->pipeline), "video_sink");
    self->bus = gst_element_get_bus(self->pipeline);
    if (!self->source || !self->appsink || !self->bus) {
        g_warning("VividVideoProducer: GPU video pipeline is missing source/appsink/bus");
        stop_pipeline(self);
        self->pipeline_failed = true;
        return false;
    }

    apply_audio_state(self);
    apply_playback_state(self);

    g_message("VividVideoProducer: started GStreamer video path project=%s "
              "codec=%s parser=%s decoder=%s decode=%s transfer-path=%s "
              "decoder-device-path=%s postproc-device-path=%s device=%s (%s) "
              "decoder-route=%s cuda-device=%d audio=optional dynamic-demux",
              self->video_path.c_str(),
              video_codec_kind_name(self->video_codec),
              self->parser_factory.c_str(),
              self->decoder_factory.c_str(),
              self->decode_label ? self->decode_label : "(unknown)",
              video_transfer_path_name(self->transfer_path),
              self->decoder_device_path.empty() ? "(none)" : self->decoder_device_path.c_str(),
              self->postproc_device_path.empty() ? "(none)" : self->postproc_device_path.c_str(),
              self->gpu_device_valid ? self->gpu_device.name : "(unresolved)",
              self->gpu_device_valid && self->gpu_device.render_node[0]
                  ? self->gpu_device.render_node
                  : "unknown-node",
              vivid_gpu_decoder_route_name(self->decoder_route),
              self->expected_cuda_device_id);
    return true;
}

bool
ensure_vulkan(VividVideoProducer* self,
              guint32 width,
              guint32 height,
              double render_scale,
              const VividVideoVulkanExportRequest& export_request)
{
    if (!self)
        return false;

    width = std::clamp(width, 1u, 8192u);
    height = std::clamp(height, 1u, 8192u);
    render_scale = std::max(1.0, render_scale);
    if (self->vulkan_ready &&
        self->width == width &&
        self->height == height &&
        video_export_requests_equal(self->export_request, export_request) &&
        std::abs(self->render_scale - render_scale) < 0.0001)
        return true;

    reset_vulkan(self);
    if (!self->gpu_device_valid) {
        g_warning("VividVideoProducer: cannot create Vulkan route without a resolved GPU device");
        return false;
    }
    if (!self->vulkan.ensure(self->gpu_device,
                             self->transfer_path,
                             width,
                             height,
                             export_request)) {
        reset_vulkan(self);
        return false;
    }

    self->width = width;
    self->height = height;
    self->render_scale = render_scale;
    self->export_request = export_request;
    self->vulkan_ready = true;
    return true;
}

bool
build_video_route_set(VividVideoProducer& self, DmabufBufferSetView& route_set)
{
    route_set = {};
    route_set.width = self.width;
    route_set.height = self.height;
    route_set.fourcc = self.vulkan.drm_fourcc();
    route_set.modifier = self.vulkan.drm_modifier();
    route_set.premultiplied = FALSE;

    /*
     * The video backend owns decode/upload and the common route owns the
     * exported slot contract. Converting the backend's Vulkan export images
     * into a route view keeps video at the same boundary as scene: both
     * producers publish DMA-BUF slot metadata through ProducerFrameRoute, and
     * neither producer knows how the display transport later binds those fds.
     */
    for (const auto& image : self.vulkan.export_images()) {
        if (!image) {
            g_warning("VividVideoProducer: export image index=%u is not ready", image.index);
            return false;
        }

        if (route_set.n_buffers >= vivid::producer::kFrameRouteMaxBuffers) {
            g_warning("VividVideoProducer: too many export images limit=%u",
                      vivid::producer::kFrameRouteMaxBuffers);
            return false;
        }

        auto& buffer = route_set.buffers[route_set.n_buffers++];
        buffer.index = image.index;
        buffer.size = static_cast<guint64>(image.size);
        buffer.n_planes = image.n_planes;
        if (buffer.n_planes == 0 ||
            buffer.n_planes > vivid::producer::kFrameRouteMaxPlanes) {
            g_warning("VividVideoProducer: export image index=%u has invalid "
                      "plane count=%u",
                      image.index,
                      buffer.n_planes);
            return false;
        }
        for (guint plane = 0; plane < buffer.n_planes; plane++) {
            buffer.planes[plane].fd = image.plane_fds[plane];
            buffer.planes[plane].stride = image.plane_strides[plane];
            buffer.planes[plane].offset = image.plane_offsets[plane];
        }
    }

    return route_set.n_buffers > 0;
}

bool
ensure_cuda_buffer(VividVideoProducer* self, GstCudaContext* cuda_context)
{
    if (!self || !cuda_context || !self->vulkan_ready)
        return false;

    const guint64 rgba_size =
        static_cast<guint64>(self->width) * self->height * 4u;
    if (self->cuda_rgba_buffer && self->cuda_rgba_buffer.size == rgba_size)
        return true;

    free_cuda_kernel(self);
    self->cuda_rgba_buffer.reset();
    auto buffer = self->vulkan.create_cuda_external_transfer_buffer(rgba_size, cuda_context);
    if (!buffer.has_value())
        return false;

    self->cuda_rgba_buffer = std::move(buffer.value());
    g_message("VividVideoProducer: created CUDA/Vulkan shared RGBA transfer buffer bytes=%llu",
              static_cast<unsigned long long>(rgba_size));
    return true;
}

bool
ensure_cuda_kernel(VividVideoProducer* self, GstCudaContext* cuda_context)
{
    if (!self || !cuda_context)
        return false;
    if (self->cuda_kernel)
        return true;

    bool pushed = gst_cuda_context_push(cuda_context);
    CUresult result = pushed
        ? CuModuleLoadData(&self->cuda_module, kNv12ToRgbaScaleCudaPtx)
        : CUDA_ERROR_UNKNOWN;
    if (result == CUDA_SUCCESS) {
        result = CuModuleGetFunction(&self->cuda_kernel,
                                     self->cuda_module,
                                     "VividNv12ToRgbaScale");
    }
    if (pushed) {
        CUcontext popped {};
        (void)gst_cuda_context_pop(&popped);
    }

    if (result != CUDA_SUCCESS || !self->cuda_kernel) {
        g_warning("VividVideoProducer: failed to load embedded CUDA NV12 converter result=%s",
                  cuda_error_name(result));
        free_cuda_kernel(self);
        return false;
    }
    return true;
}

bool
upload_sample(VividVideoProducer* self, GstSample* sample)
{
    if (!self || !sample || !self->vulkan_ready)
        return false;

    GstCaps* caps = gst_sample_get_caps(sample);
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    GstVideoInfo info;
    if (!caps || !buffer || !gst_video_info_from_caps(&info, caps))
        return false;

    if (self->transfer_path == VideoFrameTransferPath::VaMemoryBgra) {
        auto imported = self->vulkan.import_va_dmabuf_rgba_image(caps, buffer);
        if (!imported.has_value())
            imported = self->vulkan.export_va_memory_rgba_image(caps, buffer);

        if (!imported.has_value()) {
            if (!self->logged_unexpected_caps) {
                gchar* caps_text = gst_caps_to_string(caps);
                g_warning("VividVideoProducer: VA surface export/import failed caps=%s "
                          "memories=%u; expected RGBA/BGRA VAMemory or RGBA DMA-BUF",
                          caps_text ? caps_text : "(none)",
                          gst_buffer_n_memory(buffer));
                g_free(caps_text);
                self->logged_unexpected_caps = true;
            }
            return false;
        }

        const VideoFitParameters fit =
            compute_video_fit_parameters(self->fill_mode,
                                         imported->width,
                                         imported->height,
                                         self->width,
                                         self->height);
        if (!self->vulkan.submit_imported_image(imported.value(), self->fill_mode)) {
            g_warning("VividVideoProducer: failed to submit VA frame into DMA-BUF ring");
            return false;
        }

        self->uploaded_samples++;
        if (!self->logged_first_upload) {
            g_message("VividVideoProducer: first GPU VA video frame uploaded src=%ux%u "
                      "dst=%ux%u fill=%s draw=%ux%u+%d+%d decoder=%s samples=%"
                      G_GUINT64_FORMAT,
                      imported->width,
                      imported->height,
                      self->width,
                      self->height,
                      fill_mode_name(self->fill_mode),
                      fit.draw_width,
                      fit.draw_height,
                      fit.dst_x,
                      fit.dst_y,
                      self->decoder_factory.empty() ? "(none)" : self->decoder_factory.c_str(),
                      self->uploaded_samples);
            self->logged_first_upload = true;
        }
        return true;
    }

    if (!is_cuda_transfer_path(self->transfer_path))
        return false;

    if (GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_NV12 ||
        gst_buffer_n_memory(buffer) == 0 ||
        !gst_is_cuda_memory(gst_buffer_peek_memory(buffer, 0))) {
        if (!self->logged_unexpected_caps) {
            gchar* caps_text = gst_caps_to_string(caps);
            g_warning("VividVideoProducer: unexpected video sample caps=%s memories=%u; "
                      "expected NV12 CUDAMemory",
                      caps_text ? caps_text : "(none)",
                      gst_buffer_n_memory(buffer));
            g_free(caps_text);
            self->logged_unexpected_caps = true;
        }
        return false;
    }

    GstVideoFrame frame;
    if (!gst_video_frame_map(&frame, &info, buffer, kGstMapReadCuda)) {
        g_warning("VividVideoProducer: failed to map CUDA video frame");
        return false;
    }

    GstMemory* memory = gst_buffer_peek_memory(buffer, 0);
    auto* cuda_memory = reinterpret_cast<GstCudaMemory*>(memory);
    GstCudaContext* cuda_context = cuda_context_from_memory(memory);
    if (!cuda_context) {
        gst_video_frame_unmap(&frame);
        g_warning("VividVideoProducer: CUDA sample memory has no context");
        return false;
    }
    if (!cuda_context_matches_expected(cuda_context,
                                       self->expected_cuda_device_id,
                                       "VividVideoProducer")) {
        gst_video_frame_unmap(&frame);
        self->pipeline_failed = true;
        return false;
    }

    if (!ensure_cuda_buffer(self, cuda_context) ||
        !ensure_cuda_kernel(self, cuda_context)) {
        gst_video_frame_unmap(&frame);
        return false;
    }

    GstCudaStream* gst_stream = gst_cuda_memory_get_stream(cuda_memory);
    CUstream stream = gst_stream ? gst_cuda_stream_get_handle(gst_stream) : nullptr;

    CUdeviceptr y_ptr =
        reinterpret_cast<CUdeviceptr>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
    CUdeviceptr uv_ptr =
        reinterpret_cast<CUdeviceptr>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 1));
    CUdeviceptr rgba_ptr = self->cuda_rgba_buffer.cuda_ptr;
    int src_width = GST_VIDEO_INFO_WIDTH(&info);
    int src_height = GST_VIDEO_INFO_HEIGHT(&info);
    int dst_width = static_cast<int>(self->width);
    int dst_height = static_cast<int>(self->height);
    int y_pitch = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
    int uv_pitch = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 1);
    int rgba_pitch = static_cast<int>(self->width) * 4;
    VideoFitParameters fit =
        compute_video_fit_parameters(self->fill_mode,
                                     static_cast<uint32_t>(src_width),
                                     static_cast<uint32_t>(src_height),
                                     self->width,
                                     self->height);
    void* params[] {
        &y_ptr,
        &uv_ptr,
        &rgba_ptr,
        &src_width,
        &src_height,
        &dst_width,
        &dst_height,
        &y_pitch,
        &uv_pitch,
        &rgba_pitch,
        &fit.dst_x,
        &fit.dst_y,
        &fit.draw_width,
        &fit.draw_height,
        &fit.src_x0_fp,
        &fit.src_y0_fp,
        &fit.step_x_fp,
        &fit.step_y_fp,
    };

    bool pushed = gst_cuda_context_push(cuda_context);
    CUresult cuda_result = pushed
        ? CuLaunchKernel(self->cuda_kernel,
                         (self->width + 15u) / 16u,
                         (self->height + 15u) / 16u,
                         1,
                         16,
                         16,
                         1,
                         0,
                         stream,
                         params,
                         nullptr)
        : CUDA_ERROR_UNKNOWN;
    if (cuda_result == CUDA_SUCCESS)
        cuda_result = stream ? CuStreamSynchronize(stream) : CuCtxSynchronize();
    if (pushed) {
        CUcontext popped {};
        (void)gst_cuda_context_pop(&popped);
    }
    gst_video_frame_unmap(&frame);

    if (cuda_result != CUDA_SUCCESS) {
        g_warning("VividVideoProducer: CUDA NV12->RGBA scale launch failed result=%s",
                  cuda_error_name(cuda_result));
        return false;
    }

    if (!self->vulkan.submit_rgba_buffer(self->cuda_rgba_buffer)) {
        g_warning("VividVideoProducer: failed to submit RGBA frame into DMA-BUF ring");
        return false;
    }

    self->uploaded_samples++;
    if (!self->logged_first_upload) {
        g_message("VividVideoProducer: first GPU video frame uploaded src=%dx%d dst=%ux%u "
                  "fill=%s draw=%ux%u+%u+%u decoder=%s cuda-device=%d "
                  "samples=%" G_GUINT64_FORMAT,
                  src_width,
                  src_height,
                  self->width,
                  self->height,
                  fill_mode_name(self->fill_mode),
                  fit.draw_width,
                  fit.draw_height,
                  fit.dst_x,
                  fit.dst_y,
                  self->decoder_factory.empty() ? "(none)" : self->decoder_factory.c_str(),
                  self->expected_cuda_device_id,
                  self->uploaded_samples);
        self->logged_first_upload = true;
    }
    return true;
}

bool
video_wait_release_gate(VividVideoProducer* self, guint32 buffer_index)
{
    if (!self || !self->release_gate_valid)
        return true;

    const VividRendererReleaseGate gate = self->release_gate;
    if (gate.abi_version != VIVID_RENDERER_RELEASE_GATE_ABI_VERSION ||
        !gate.wait_release) {
        return true;
    }

    if (gate.wait_release(gate.user_data,
                          buffer_index,
                          VIDEO_RELEASE_GATE_TIMEOUT_MSEC)) {
        return true;
    }

    g_warning("VividVideoProducer: release gate timed out for upload slot=%u "
              "timeout-ms=%u; dropping decoded frame",
              buffer_index,
              VIDEO_RELEASE_GATE_TIMEOUT_MSEC);
    return false;
}

void
poll_bus(VividVideoProducer* self)
{
    if (!self || !self->bus || !self->pipeline)
        return;

    while (GstMessage* message = gst_bus_pop(self->bus)) {
        switch (GST_MESSAGE_TYPE(message)) {
        case GST_MESSAGE_EOS:
            self->eos_count++;
            g_message("VividVideoProducer: video reached EOS; looping count=%" G_GUINT64_FORMAT,
                      self->eos_count);
            if (!gst_element_seek(self->pipeline,
                                  1.0,
                                  GST_FORMAT_TIME,
                                  static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH |
                                                            GST_SEEK_FLAG_KEY_UNIT),
                                  GST_SEEK_TYPE_SET,
                                  0,
                                  GST_SEEK_TYPE_NONE,
                                  GST_CLOCK_TIME_NONE)) {
                g_warning("VividVideoProducer: failed to seek video to the beginning");
            }
            apply_playback_state(self);
            break;
        case GST_MESSAGE_ERROR: {
            GError* error = nullptr;
            gchar* debug = nullptr;
            gst_message_parse_error(message, &error, &debug);
            g_warning("VividVideoProducer: pipeline error source=%s message=%s debug=%s",
                      GST_MESSAGE_SRC_NAME(message),
                      error ? error->message : "unknown error",
                      debug ? debug : "none");
            g_clear_error(&error);
            g_free(debug);
            self->pipeline_failed = true;
            break;
        }
        case GST_MESSAGE_WARNING: {
            GError* error = nullptr;
            gchar* debug = nullptr;
            gst_message_parse_warning(message, &error, &debug);
            g_warning("VividVideoProducer: pipeline warning source=%s message=%s debug=%s",
                      GST_MESSAGE_SRC_NAME(message),
                      error ? error->message : "unknown warning",
                      debug ? debug : "none");
            g_clear_error(&error);
            g_free(debug);
            break;
        }
        default:
            break;
        }

        gst_message_unref(message);
    }
}

} // namespace

VividVideoProducer*
vivid_video_producer_new(void)
{
    if (!gst_is_initialized())
        gst_init(nullptr, nullptr);
    return new VividVideoProducer();
}

void
vivid_video_producer_free(VividVideoProducer* self)
{
    if (!self)
        return;

    stop_pipeline(self);
    reset_vulkan(self);
    delete self;
}

gboolean
vivid_video_producer_configure(VividVideoProducer* self,
                                const gchar*         video_path,
                                gboolean             muted,
                                gdouble              volume,
                                gint                 fill_mode,
                                gint                 fps,
                                const gchar*         render_device)
{
    g_return_val_if_fail(self != nullptr, FALSE);
    g_return_val_if_fail(video_path != nullptr, FALSE);

    const std::string next_path = video_path;
    const std::string next_render_device =
        render_device && *render_device ? render_device : "auto";
    const bool path_changed = self->video_path != next_path;
    const bool render_device_changed = self->render_device != next_render_device;

    self->video_path = next_path;
    self->render_device = next_render_device;
    self->muted = !!muted;
    self->volume = std::clamp(volume, 0.0, 1.0);
    self->fill_mode = parse_fill_mode(fill_mode);
    self->fps = std::clamp(fps, 5, 240);
    self->configured = true;

    if (path_changed || render_device_changed) {
        stop_pipeline(self);
        reset_vulkan(self);
        self->frame_route.reset();
        self->uploaded_samples = 0;
        self->eos_count = 0;
        self->transfer_path = VideoFrameTransferPath::None;
        self->decoder_route = VIVID_GPU_DECODER_ROUTE_NONE;
        self->video_codec = VideoCodecKind::Unknown;
        self->decoder_factory.clear();
        self->parser_factory.clear();
        self->postproc_factory.clear();
        self->decoder_device_path.clear();
        self->postproc_device_path.clear();
        self->sink_caps = nullptr;
        self->decode_label = nullptr;
        self->expected_cuda_device_id = -1;
        self->gpu_device = {};
        self->gpu_device_valid =
            vivid_gpu_device_resolve(self->render_device.c_str(), &self->gpu_device);
        if (self->gpu_device_valid) {
            VideoCodecProbe probe = probe_video_codec(self->video_path);
            if (!probe.probed) {
                g_warning("VividVideoProducer: video codec probe found no video "
                          "stream (timeout, unreadable file, or missing video "
                          "track) for %s; assuming h264",
                          self->video_path.c_str());
                probe.kind = VideoCodecKind::H264;
            }
            self->video_codec = probe.kind;
            g_message("VividVideoProducer: probed video codec=%s project=%s caps=%s",
                      video_codec_kind_name(probe.kind),
                      self->video_path.c_str(),
                      probe.caps_text.empty() ? "(none)" : probe.caps_text.c_str());
            self->decoder_route =
                vivid_gpu_decoder_route_for_vendor(self->gpu_device.vendor_id);
            if (auto selection = select_video_decoder(self->decoder_route,
                                                       self->gpu_device.render_node,
                                                       self->gpu_device.pci_address,
                                                       self->video_codec);
                selection.has_value()) {
                self->transfer_path = selection->transfer_path;
                self->decoder_factory = std::move(selection->decoder_factory);
                self->parser_factory = std::move(selection->parser_factory);
                self->postproc_factory = std::move(selection->postproc_factory);
                self->decoder_device_path = std::move(selection->decoder_device_path);
                self->postproc_device_path = std::move(selection->postproc_device_path);
                self->sink_caps = selection->sink_caps;
                self->decode_label = selection->decode_label;
                self->expected_cuda_device_id = selection->expected_cuda_device_id;
            }
        } else {
            g_warning("VividVideoProducer: no usable Vulkan GPU for render-device='%s'",
                      self->render_device.c_str());
        }
    }

    g_message("VividVideoProducer: configure path=%s path-changed=%s render-device=%s "
              "device=%s (%s) decoder-route=%s gpu-changed=%s codec=%s transfer-path=%s "
              "parser=%s decoder=%s decode=%s "
              "decoder-device-path=%s postproc-device-path=%s cuda-device=%d muted=%s "
              "volume=%.3f fill=%s fps=%d",
              self->video_path.c_str(),
              bool_to_string(path_changed),
              self->render_device.c_str(),
              self->gpu_device_valid ? self->gpu_device.name : "(unresolved)",
              self->gpu_device_valid && self->gpu_device.render_node[0]
                  ? self->gpu_device.render_node
                  : "unknown-node",
              vivid_gpu_decoder_route_name(self->decoder_route),
              bool_to_string(render_device_changed),
              video_codec_kind_name(self->video_codec),
              video_transfer_path_name(self->transfer_path),
              !self->parser_factory.empty() ? self->parser_factory.c_str() : "(none)",
              !self->decoder_factory.empty() ? self->decoder_factory.c_str() : "(none)",
              self->decode_label ? self->decode_label : "(none)",
              self->decoder_device_path.empty() ? "(none)" : self->decoder_device_path.c_str(),
              self->postproc_device_path.empty() ? "(none)" : self->postproc_device_path.c_str(),
              self->expected_cuda_device_id,
              bool_to_string(self->muted),
              self->volume,
              fill_mode_name(self->fill_mode),
              self->fps);

    if (self->transfer_path == VideoFrameTransferPath::None || self->decoder_factory.empty())
        return FALSE;

    apply_audio_state(self);
    apply_playback_state(self);
    return TRUE;
}

void
vivid_video_producer_set_audio_state(VividVideoProducer* self,
                                      gboolean             muted,
                                      gdouble              volume)
{
    if (!self)
        return;

    self->muted = !!muted;
    self->volume = std::clamp(volume, 0.0, 1.0);
    apply_audio_state(self);
}

void
vivid_video_producer_set_playing(VividVideoProducer* self, gboolean playing)
{
    if (!self)
        return;

    self->playing = !!playing;
    apply_playback_state(self);
}

void
vivid_video_producer_set_release_gate(VividVideoProducer*          self,
                                      const VividRendererReleaseGate* gate)
{
    g_return_if_fail(self != nullptr);

    if (gate && gate->abi_version == VIVID_RENDERER_RELEASE_GATE_ABI_VERSION &&
        gate->wait_release) {
        self->release_gate = *gate;
        self->release_gate_valid = true;
    } else {
        self->release_gate = {};
        self->release_gate_valid = false;
    }
}

static gboolean
vivid_video_producer_prepare_buffers_internal(
    VividVideoProducer*                    self,
    guint32                                width,
    guint32                                height,
    gdouble                                render_scale,
    const VividVideoVulkanExportRequest&   export_request,
    VividVideoProducerBufferSet*           out_set)
{
    g_return_val_if_fail(self != nullptr, FALSE);
    g_return_val_if_fail(out_set != nullptr, FALSE);

    vivid::producer::init_dmabuf_buffer_set(*out_set);
    if (!self->configured ||
        self->transfer_path == VideoFrameTransferPath::None ||
        self->decoder_factory.empty()) {
        g_warning("VividVideoProducer: cannot prepare buffers before successful configure");
        return FALSE;
    }

    if (!ensure_vulkan(self, width, height, render_scale, export_request))
        return FALSE;
    if (!ensure_pipeline(self))
        return FALSE;

    DmabufBufferSetView route_set;
    if (!build_video_route_set(*self, route_set))
        return FALSE;
    if (!self->frame_route.publish_buffer_set(route_set, *out_set))
        return FALSE;

    g_message("VividVideoProducer: prepared DMA-BUF buffer set %ux%u buffers=%u "
              "fourcc=0x%08x modifier=0x%016" G_GINT64_MODIFIER "x",
              out_set->width,
              out_set->height,
              out_set->n_buffers,
              out_set->fourcc,
              static_cast<guint64>(out_set->modifier));
    return out_set->n_buffers > 0;
}

gboolean
vivid_video_producer_prepare_buffers(VividVideoProducer*          self,
                                      guint32                       width,
                                      guint32                       height,
                                      gdouble                       render_scale,
                                      VividVideoProducerBufferSet* out_set)
{
    const VividVideoVulkanExportRequest export_request =
        video_export_request_from_abi(nullptr,
                                      self ? self->transfer_path : VideoFrameTransferPath::None);
    return vivid_video_producer_prepare_buffers_internal(self,
                                                         width,
                                                         height,
                                                         render_scale,
                                                         export_request,
                                                         out_set);
}

gboolean
vivid_video_producer_query_dmabuf_caps(VividVideoProducer*           self,
                                       VividVideoProducerDmaBufCaps* out_caps)
{
    g_return_val_if_fail(self != nullptr, FALSE);
    g_return_val_if_fail(out_caps != nullptr, FALSE);

    memset(out_caps, 0, sizeof(*out_caps));
    if (!self->gpu_device_valid) {
        g_warning("VividVideoProducer: cannot query DMA-BUF caps without a resolved GPU");
        return FALSE;
    }

    const guint32 target_fourcc = video_target_fourcc_for_transfer_path(self->transfer_path);
    const auto caps = VividVideoVulkanBackend::query_export_caps(
        self->gpu_device,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    for (const auto& cap : caps) {
        if (cap.fourcc != target_fourcc)
            continue;
        if (out_caps->n_caps >= VIVID_VIDEO_PRODUCER_DMABUF_MAX_CAPS)
            break;
        out_caps->caps[out_caps->n_caps++] = {
            .fourcc = cap.fourcc,
            .modifier = cap.modifier,
            .plane_count = cap.plane_count,
        };
    }
    if (out_caps->n_caps == 0) {
        out_caps->caps[out_caps->n_caps++] = {
            .fourcc = target_fourcc,
            .modifier = DRM_FORMAT_MOD_LINEAR,
            .plane_count = 1,
        };
    }
    out_caps->memory_preference = VIVID_VIDEO_PRODUCER_DMABUF_MEMORY_DEVICE_LOCAL;
    return TRUE;
}

gboolean
vivid_video_producer_prepare_buffers_with_request(
    VividVideoProducer*                    self,
    guint32                                width,
    guint32                                height,
    gdouble                                render_scale,
    const VividVideoProducerDmaBufRequest* request,
    VividVideoProducerBufferSet*           out_set)
{
    const guint32 target_fourcc = video_target_fourcc_for_transfer_path(self->transfer_path);
    if (request && request->fourcc != 0 && request->fourcc != target_fourcc) {
        g_warning("VividVideoProducer: requested fourcc=0x%08x but current video "
                  "route exports fourcc=0x%08x",
                  request->fourcc,
                  target_fourcc);
        return FALSE;
    }

    const VividVideoVulkanExportRequest export_request =
        video_export_request_from_abi(request, self->transfer_path);
    if (!vivid_video_producer_prepare_buffers_internal(self,
                                                       width,
                                                       height,
                                                       render_scale,
                                                       export_request,
                                                       out_set))
        return FALSE;

    return TRUE;
}

gboolean
vivid_video_producer_next_frame(VividVideoProducer*      self,
                                 VividVideoProducerFrame* out_frame)
{
    g_return_val_if_fail(self != nullptr, FALSE);
    g_return_val_if_fail(out_frame != nullptr, FALSE);

    memset(out_frame, 0, sizeof(*out_frame));
    if (!self->vulkan_ready || !self->appsink || self->pipeline_failed)
        return FALSE;

    poll_bus(self);
    if (self->pipeline_failed)
        return FALSE;

    GstSample* latest_sample = nullptr;
    while (GstSample* sample =
               gst_app_sink_try_pull_sample(GST_APP_SINK(self->appsink), 0)) {
        if (latest_sample)
            gst_sample_unref(latest_sample);
        latest_sample = sample;
    }

    if (!latest_sample) {
        if (self->uploaded_samples == 0 && !self->logged_waiting_for_sample) {
            g_message("VividVideoProducer: waiting for first decoded GPU video sample "
                      "transfer-path=%s",
                      video_transfer_path_name(self->transfer_path));
            self->logged_waiting_for_sample = true;
        }
        return FALSE;
    }

    self->logged_waiting_for_sample = false;
    const guint32 upload_slot = self->vulkan.in_progress_buffer_index();
    if (!video_wait_release_gate(self, upload_slot)) {
        gst_sample_unref(latest_sample);
        return FALSE;
    }

    const bool uploaded = upload_sample(self, latest_sample);
    gst_sample_unref(latest_sample);
    if (!uploaded)
        return FALSE;

    VividVideoVulkanExportImage* frame = self->vulkan.eat_frame();
    if (!frame)
        return FALSE;

    self->frame_route.write_ready_frame(frame->index,
                                        static_cast<gint32>(frame->index),
                                        *out_frame);
    return TRUE;
}

void
vivid_video_producer_buffer_set_clear(VividVideoProducerBufferSet* set)
{
    if (!set)
        return;

    vivid::producer::clear_dmabuf_buffer_set(*set);
}
