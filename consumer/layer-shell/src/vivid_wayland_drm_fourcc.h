/*
 * Protocol optimization changes in this file are derived from waywallen.
 * Source: local reference tree waywallen/ and upstream https://github.com/waywallen/waywallen.
 * Copyright owner for the waywallen-derived protocol optimization code:
 * https://github.com/hypengw <hypengwip@gmail.com>.
 */

#ifndef VIVID_WAYLAND_DRM_FOURCC_H
#define VIVID_WAYLAND_DRM_FOURCC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WW_DRM_FORMAT_ABGR8888 0x34324241u /* AB24 */
#define WW_DRM_FORMAT_XBGR8888 0x34324258u /* XB24 */
#define WW_DRM_FORMAT_ARGB8888 0x34325241u /* AR24 */
#define WW_DRM_FORMAT_XRGB8888 0x34325258u /* XR24 */
#define WW_DRM_FORMAT_RGBA8888 0x41424752u /* RGBA */
#define WW_DRM_FORMAT_BGRA8888 0x41524742u /* BGRA */
#define WW_DRM_FORMAT_RGBX8888 0x58424752u /* RGBX */
#define WW_DRM_FORMAT_BGRX8888 0x58524742u /* BGRX */

#define WW_DRM_FORMAT_MOD_LINEAR 0ULL

static const uint32_t WW_DRM_FORMAT_RGBA_8888[8] = {
    WW_DRM_FORMAT_ABGR8888, WW_DRM_FORMAT_XBGR8888, WW_DRM_FORMAT_ARGB8888, WW_DRM_FORMAT_XRGB8888,
    WW_DRM_FORMAT_RGBA8888, WW_DRM_FORMAT_BGRA8888, WW_DRM_FORMAT_RGBX8888, WW_DRM_FORMAT_BGRX8888,
};

static inline bool
ww_drm_fourcc_supported(uint32_t fourcc)
{
    for (size_t i = 0; i < sizeof(WW_DRM_FORMAT_RGBA_8888) / sizeof(WW_DRM_FORMAT_RGBA_8888[0]);
         ++i) {
        if (fourcc == WW_DRM_FORMAT_RGBA_8888[i])
            return true;
    }
    return false;
}

#ifdef __cplusplus
}
#endif

#endif
