/*
 * Protocol optimization changes in this file are derived from waywallen.
 * Source: local reference tree waywallen/ and upstream https://github.com/waywallen/waywallen.
 * Copyright owner for the waywallen-derived protocol optimization code:
 * https://github.com/hypengw <hypengwip@gmail.com>.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define VIVID_RENDERER_RELEASE_GATE_ABI_VERSION 1u

typedef struct _VividRendererReleaseGate VividRendererReleaseGate;

/*
 * Internal producer/renderer ABI used to move consumer-release back-pressure to
 * the slot reuse point. The core producer owns the DRM syncobj timeline and
 * exposes this small gate to renderer DMA-BUF producers; renderer code calls it
 * immediately before writing into a reusable export slot. Returning FALSE means
 * the slot is still owned by a slow consumer after timeout_ms and the current
 * renderer frame must be skipped without publishing that slot again.
 */
struct _VividRendererReleaseGate
{
    guint32  abi_version;
    gpointer user_data;
    gboolean (*wait_release)(gpointer user_data,
                             guint32   buffer_index,
                             guint32   timeout_ms);
};

G_END_DECLS
