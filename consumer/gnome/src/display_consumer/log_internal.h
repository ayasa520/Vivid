/*
 * Protocol optimization changes in this file are derived from waywallen.
 * Source: local reference tree waywallen/ and upstream https://github.com/waywallen/waywallen.
 * Copyright owner for the waywallen-derived protocol optimization code:
 * https://github.com/hypengw <hypengwip@gmail.com>.
 */

#ifndef VIVID_DISPLAY_CONSUMER_WAYWALLEN_LOG_INTERNAL_H
#define VIVID_DISPLAY_CONSUMER_WAYWALLEN_LOG_INTERNAL_H

#include <glib.h>

typedef enum {
    WAYWALLEN_LOG_DEBUG,
    WAYWALLEN_LOG_INFO,
    WAYWALLEN_LOG_WARN,
    WAYWALLEN_LOG_ERROR,
} waywallen_log_level_t;

static inline GLogLevelFlags
vivid_waywallen_log_level_to_glib(waywallen_log_level_t level)
{
    switch (level) {
    case WAYWALLEN_LOG_DEBUG:
        return G_LOG_LEVEL_DEBUG;
    case WAYWALLEN_LOG_INFO:
        return G_LOG_LEVEL_MESSAGE;
    case WAYWALLEN_LOG_WARN:
        return G_LOG_LEVEL_WARNING;
    case WAYWALLEN_LOG_ERROR:
        return G_LOG_LEVEL_CRITICAL;
    default:
        return G_LOG_LEVEL_MESSAGE;
    }
}

#define ww_log(level, ...) \
    g_log("VividDisplayConsumer", vivid_waywallen_log_level_to_glib(level), __VA_ARGS__)

#endif
