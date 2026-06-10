/*
 * Protocol optimization changes in this file are derived from waywallen.
 * Source: local reference tree waywallen/ and upstream https://github.com/waywallen/waywallen.
 * Copyright owner for the waywallen-derived protocol optimization code:
 * https://github.com/hypengw <hypengwip@gmail.com>.
 */

#ifndef VIVID_KDE_WAYWALLEN_LOG_INTERNAL_H
#define VIVID_KDE_WAYWALLEN_LOG_INTERNAL_H

typedef enum {
    WAYWALLEN_LOG_DEBUG,
    WAYWALLEN_LOG_INFO,
    WAYWALLEN_LOG_WARN,
    WAYWALLEN_LOG_ERROR,
} waywallen_log_level_t;

#ifdef __cplusplus

#include <QDebug>
#include <QLoggingCategory>

Q_DECLARE_LOGGING_CATEGORY(lcWallpaperKdeVulkan)

inline void vividKdeVulkanLog(waywallen_log_level_t level, const char* message)
{
    switch (level) {
    case WAYWALLEN_LOG_DEBUG:
        qCDebug(lcWallpaperKdeVulkan, "%s", message);
        break;
    case WAYWALLEN_LOG_INFO:
        qCInfo(lcWallpaperKdeVulkan, "%s", message);
        break;
    case WAYWALLEN_LOG_WARN:
        qCWarning(lcWallpaperKdeVulkan, "%s", message);
        break;
    case WAYWALLEN_LOG_ERROR:
        qCCritical(lcWallpaperKdeVulkan, "%s", message);
        break;
    }
}

#define ww_log(level, ...)                                      \
    do {                                                        \
        QByteArray vividKdeVulkanLogMessage =                   \
            QByteArray::asprintf(__VA_ARGS__);                  \
        vividKdeVulkanLog(level, vividKdeVulkanLogMessage.constData()); \
    } while (0)

#else

#include <stdarg.h>
#include <stdio.h>

static inline const char*
vivid_kde_vulkan_log_level_name(waywallen_log_level_t level)
{
    switch (level) {
    case WAYWALLEN_LOG_DEBUG:
        return "debug";
    case WAYWALLEN_LOG_INFO:
        return "info";
    case WAYWALLEN_LOG_WARN:
        return "warning";
    case WAYWALLEN_LOG_ERROR:
        return "error";
    }
    return "unknown";
}

/*
 * The Vulkan backend/blitter are plain C translation units. Keep their
 * diagnostics independent from Qt headers so they can be compiled by the C
 * compiler while still preserving every real error line.
 */
static inline void
vividKdeVulkanLogC(waywallen_log_level_t level, const char* format, ...)
{
    va_list args;
    fprintf(stderr, "vivid-kde-vulkan[%s]: ", vivid_kde_vulkan_log_level_name(level));
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
}

#define ww_log(level, ...) vividKdeVulkanLogC(level, __VA_ARGS__)

#endif

#endif
