#!/usr/bin/env -S gjs -m

// Protocol optimization changes in this file are derived from waywallen.
// Source: local reference tree waywallen/ and upstream https://github.com/waywallen/waywallen.
// Copyright owner for the waywallen-derived protocol optimization code:
// https://github.com/hypengw <hypengwip@gmail.com>.


import Gdk from 'gi://Gdk?version=4.0';
import Gio from 'gi://Gio';
import GioUnix from 'gi://GioUnix?version=2.0';
import GLib from 'gi://GLib';
import Gtk from 'gi://Gtk?version=4.0';
import cairo from 'cairo';
import system from 'system';

import {
    VIVID_DISPLAY_AUDIO_SAMPLES_BIN_MAX_COUNT,
    VIVID_DISPLAY_BUTTON_PRESSED,
    VIVID_DISPLAY_BUTTON_RELEASED,
    VIVID_DISPLAY_AXIS_WHEEL,
    VIVID_DISPLAY_CODEC_MAX_BODY_BYTES,
    VIVID_DISPLAY_EVT_BIND_BUFFERS,
    VIVID_DISPLAY_EVT_ERROR,
    VIVID_DISPLAY_EVT_FRAME_READY,
    VIVID_DISPLAY_EVT_OUTPUT_ACCEPTED,
    VIVID_DISPLAY_EVT_SET_CONFIG,
    VIVID_DISPLAY_EVT_UNBIND,
    VIVID_DISPLAY_EVT_WELCOME,
    VIVID_DISPLAY_FRAME_READY_BODY_BYTES,
    VIVID_DISPLAY_FRAME_READY_FD_COUNT,
    VIVID_DISPLAY_POINTER_AXIS_BODY_BYTES,
    VIVID_DISPLAY_POINTER_BUTTON_BODY_BYTES,
    VIVID_DISPLAY_POINTER_MOTION_BODY_BYTES,
    VIVID_DISPLAY_PROTOCOL_NAME,
    VIVID_DISPLAY_PROTOCOL_VERSION,
    VIVID_DISPLAY_REQ_AUDIO_SAMPLES_BIN,
    VIVID_DISPLAY_REQ_BIND_FAILED,
    VIVID_DISPLAY_REQ_CONSUMER_CAPS,
    VIVID_DISPLAY_REQ_HELLO,
    VIVID_DISPLAY_REQ_MEDIA_STATE,
    VIVID_DISPLAY_REQ_POINTER_AXIS,
    VIVID_DISPLAY_REQ_POINTER_BUTTON,
    VIVID_DISPLAY_REQ_POINTER_MOTION,
    VIVID_DISPLAY_REQ_REGISTER_OUTPUT,
    VIVID_DISPLAY_REQ_UNBIND_DONE,
    VIVID_DISPLAY_REQ_WINDOW_STATE,
    VIVID_DISPLAY_UNBIND_BODY_BYTES,
} from './protocol-constants.js';
import {
    decodeFrameReady,
    decodeUnbind,
    encodeAudioSamplesBin,
    encodePointerAxis,
    encodePointerButton,
    encodePointerMotion,
} from './protocol-codec.js';
import {
    bindFailedReasonName,
    errorDomain,
    errorName,
} from './protocol-meta.js';
import { J } from './protocol-json-fields.js';

const APPLICATION_ID = 'dev.rikka.VividWallpaper.Helper';
const TITLE_PREFIX = `@${APPLICATION_ID}!`;
const PROTOCOL_NAME = VIVID_DISPLAY_PROTOCOL_NAME;
const PROTOCOL_VERSION = VIVID_DISPLAY_PROTOCOL_VERSION;
const MAX_BODY_BYTES = VIVID_DISPLAY_CODEC_MAX_BODY_BYTES;
const FRAME_HEADER_BYTES = 4;
const POINTER_MOTION_BODY_BYTES = VIVID_DISPLAY_POINTER_MOTION_BODY_BYTES;
const POINTER_BUTTON_BODY_BYTES = VIVID_DISPLAY_POINTER_BUTTON_BODY_BYTES;
const POINTER_AXIS_BODY_BYTES = VIVID_DISPLAY_POINTER_AXIS_BODY_BYTES;
const FRAME_READY_BODY_BYTES = VIVID_DISPLAY_FRAME_READY_BODY_BYTES;
const FRAME_READY_FD_COUNT = VIVID_DISPLAY_FRAME_READY_FD_COUNT;
const UNBIND_BODY_BYTES = VIVID_DISPLAY_UNBIND_BODY_BYTES;

const REQ_HELLO = VIVID_DISPLAY_REQ_HELLO;
const REQ_REGISTER_OUTPUT = VIVID_DISPLAY_REQ_REGISTER_OUTPUT;
const REQ_CONSUMER_CAPS = VIVID_DISPLAY_REQ_CONSUMER_CAPS;
const REQ_POINTER_MOTION = VIVID_DISPLAY_REQ_POINTER_MOTION;
const REQ_POINTER_BUTTON = VIVID_DISPLAY_REQ_POINTER_BUTTON;
const REQ_POINTER_AXIS = VIVID_DISPLAY_REQ_POINTER_AXIS;
const REQ_WINDOW_STATE = VIVID_DISPLAY_REQ_WINDOW_STATE;
const REQ_MEDIA_STATE = VIVID_DISPLAY_REQ_MEDIA_STATE;
const REQ_AUDIO_SAMPLES_BIN = VIVID_DISPLAY_REQ_AUDIO_SAMPLES_BIN;
const REQ_BIND_FAILED = VIVID_DISPLAY_REQ_BIND_FAILED;
const REQ_UNBIND_DONE = VIVID_DISPLAY_REQ_UNBIND_DONE;

const EVT_WELCOME = VIVID_DISPLAY_EVT_WELCOME;
const EVT_OUTPUT_ACCEPTED = VIVID_DISPLAY_EVT_OUTPUT_ACCEPTED;
const EVT_BIND_BUFFERS = VIVID_DISPLAY_EVT_BIND_BUFFERS;
const EVT_SET_CONFIG = VIVID_DISPLAY_EVT_SET_CONFIG;
const EVT_FRAME_READY = VIVID_DISPLAY_EVT_FRAME_READY;
const EVT_UNBIND = VIVID_DISPLAY_EVT_UNBIND;
const EVT_ERROR = VIVID_DISPLAY_EVT_ERROR;

const DRM_FORMAT_XRGB8888 = 0x34325258; // 'XR24'
const DRM_FORMAT_ARGB8888 = 0x34325241; // 'AR24'
const DRM_FORMAT_XBGR8888 = 0x34324258; // 'XB24'
const DRM_FORMAT_ABGR8888 = 0x34324241; // 'AB24'
const DRM_FORMAT_MOD_LINEAR = 0;
const DRM_FORMAT_MOD_INVALID = 0x00ffffffffffffffn;
const VIVID_RGBA_FOURCCS = [
    DRM_FORMAT_XRGB8888,
    DRM_FORMAT_ARGB8888,
    DRM_FORMAT_XBGR8888,
    DRM_FORMAT_ABGR8888,
];

const POINTER_BUTTON_RELEASED = VIVID_DISPLAY_BUTTON_RELEASED;
const POINTER_BUTTON_PRESSED = VIVID_DISPLAY_BUTTON_PRESSED;
const POINTER_AXIS_WHEEL = VIVID_DISPLAY_AXIS_WHEEL;
const FRAME_INTERVAL_WARN_USEC = 50_000;
const TEXTURE_REFRESH_WARN_USEC = 8_000;
const FRAME_DIAGNOSTIC_LOG_INTERVAL_USEC = 1_000_000;
const SOCKET_WRITE_WARN_USEC = 16_667;
const SOCKET_DIAGNOSTIC_LOG_INTERVAL_USEC = 1_000_000;
const CONNECT_FAILURE_LOG_INTERVAL_USEC = 30_000_000;

const encoder = new TextEncoder();
const decoder = new TextDecoder();

const moduleFile = GLib.filename_from_uri(import.meta.url)[0];
const moduleDir = GLib.path_get_dirname(moduleFile);
const extensionDir = GLib.path_get_dirname(GLib.path_get_dirname(moduleDir));
const commonDir = GLib.build_filenamev([extensionDir, 'common']);

imports.searchPath.unshift(commonDir);

/*
 * The display_consumer typelib and shared library are located through
 * GI_TYPELIB_PATH and LD_LIBRARY_PATH, which the shell-side service already
 * prepends when it spawns this helper. Do not register those paths through
 * GIRepository from here. GIRepository exposes two incompatible typelibs:
 * GIRepository-2.0 (libgirepository-1.0) has Repository.get_default() plus
 * static path setters, while GIRepository-3.0 (libgirepository-2.0) has
 * Repository.dup_default() plus instance path setters. Which one an unversioned
 * import resolves to is decided by the girepository generation the host GJS was
 * built against, not by anything this extension controls, so calling either API
 * aborts the helper at module load time on part of the supported shell range.
 */

let Mpris = null;
try {
    Mpris = imports.mpris;
} catch (error) {
    printerr(`Vivid Consumer helper: MPRIS monitor unavailable: ${error}`);
}

let GdkPixbuf = null;
try {
    GdkPixbuf = (await import('gi://GdkPixbuf?version=2.0')).default;
} catch (error) {
    printerr(`Vivid Consumer helper: GdkPixbuf unavailable; media thumbnails disabled: ${error}`);
}

let Gst = null;
try {
    Gst = (await import('gi://Gst?version=1.0')).default;
    Gst.init(null);
} catch (error) {
    Gst = null;
    printerr(`Vivid Consumer helper: GStreamer unavailable; audio samples disabled: ${error}`);
}

let GstApp = null;
if (Gst) {
    try {
        GstApp = (await import('gi://GstApp?version=1.0')).default;
    } catch (error) {
        printerr(`Vivid Consumer helper: GstApp unavailable; audio samples disabled: ${error}`);
    }
}

let DisplayConsumer = null;
try {
    DisplayConsumer = (await import('gi://VividDisplayConsumer?version=1.0')).default;
} catch (error) {
    printerr(`Vivid Consumer helper: display consumer receiver unavailable: ${error}`);
    system.exit(1);
}

const log = message => printerr(`Vivid Consumer helper: ${message}`);

function isVividRgbaFourcc(fourcc) {
    return VIVID_RGBA_FOURCCS.includes(fourcc);
}

function appendUniqueNumber(array, value) {
    if (!array.includes(value))
        array.push(value);
}

function uint64Equals(value, expected) {
    if (typeof value === 'bigint')
        return value === BigInt(expected);
    return Number(value) === expected;
}

function uint64IsDrmModifierInvalid(value) {
    if (typeof value === 'bigint')
        return value === DRM_FORMAT_MOD_INVALID;
    return String(value) === DRM_FORMAT_MOD_INVALID.toString();
}

function uint64ToProtocolValue(value) {
    return typeof value === 'bigint' ? value.toString() : String(value);
}

function appendDmaBufModifierCap(caps, fourcc, modifier, planeCount = 1) {
    if (!isVividRgbaFourcc(fourcc))
        return;

    appendUniqueNumber(caps.fourccs, fourcc);
    if (uint64Equals(modifier, DRM_FORMAT_MOD_LINEAR) ||
        uint64IsDrmModifierInvalid(modifier)) {
        appendUniqueNumber(caps.implicitLinearFourccs, fourcc);
        return;
    }

    caps.modifiers.push({
        fourcc,
        modifier: uint64ToProtocolValue(modifier),
        planeCount: Number(planeCount) > 0 ? Number(planeCount) : 1,
    });
}

function callDisplayConsumerFunction(name, ...args) {
    const fn = DisplayConsumer?.[name];
    if (typeof fn !== 'function')
        return null;

    try {
        return fn(...args);
    } catch (error) {
        log(`display consumer DisplayConsumer.${name} failed: ${error}`);
        return null;
    }
}

function takeFrameFd(fdList, index, _label) {
    try {
        if (!fdList || typeof fdList.get_length !== 'function' ||
            fdList.get_length() <= index)
            return -1;
        const fd = fdList.get(index);
        return Number.isFinite(fd) ? fd : -1;
    } catch (_error) {
        return -1;
    }
}

function closeFrameFdList(fdList) {
    const length = fdList?.get_length?.() ?? 0;
    for (let index = 0; index < length; index++)
        closeDisplayConsumerFd(takeFrameFd(fdList, index));
}

function closeDisplayConsumerFd(fd) {
    if (!Number.isFinite(fd) || fd < 0)
        return;
    callDisplayConsumerFunction('dmabuf_texture_close_fd', fd);
}

function stringFromDisplayConsumer(value) {
    return typeof value === 'string' ? value.trim() : '';
}

function queryVulkanRelayCaps() {
    const text = stringFromDisplayConsumer(
        callDisplayConsumerFunction('dmabuf_texture_query_vulkan_relay_caps_json'));
    if (!text)
        return null;

    try {
        const parsed = JSON.parse(text);
        return parsed && typeof parsed === 'object' ? parsed : null;
    } catch (error) {
        log(`Vulkan relay caps JSON parse failed: ${error}; payload=${text}`);
        return {
            available: false,
            probe: 'vulkan-relay-json-parse-failed',
            diagnostics: String(error),
        };
    }
}

function buildDmaBufCaps() {
    const caps = {
        version: 3,
        backend: 'gnome-gtk4-vulkan-dmabuf-relay-gdk-shadow',
        probe: 'vulkan-relay-unprobed',
        relayModes: ['shadow-copy-v1'],
        renderNode: '',
        deviceUuid: '',
        driverUuid: '',
        vendor: '',
        pciAddress: '',
        fourccs: [],
        modifiers: [],
        implicitLinearFourccs: [],
        memoryHints: [],
        syncCaps: ['implicit', 'explicit-sync-fd', 'drm-syncobj-release'],
        colorCaps: ['srgb', 'limited-range', 'premultiplied-alpha'],
        extentMax: { width: 0, height: 0 },
        textureTarget: 'VulkanRelayShadowGdkDmabufTexture',
        skipsExternalOnlyModifiers: true,
        diagnostics: '',
    };

    /*
     * Match waywallen's GNOME path: a private Vulkan relay imports producer
     * DMA-BUFs with native modifiers/device-local memory, then exports a LINEAR
     * shadow for GDK. These caps describe the relay import leg. There is no
     * direct GDK import fallback because producer-owned textures must never
     * enter GSK's presentation lifetime.
     */
    const relayCaps = queryVulkanRelayCaps();
    if (relayCaps?.available && Array.isArray(relayCaps.formats)) {
        const relayFormats = relayCaps.formats
            .filter(entry => isVividRgbaFourcc(Number(entry?.fourcc)));
        if (relayFormats.length > 0) {
            caps.probe = String(relayCaps.probe ?? 'vulkan-relay-format-probe');
            caps.renderNode = String(relayCaps.renderNode ?? '');
            caps.deviceUuid = String(relayCaps.deviceUuid ?? '');
            caps.driverUuid = String(relayCaps.driverUuid ?? '');
            caps.memoryHints = relayCaps.supportsDeviceLocal
                ? ['device-local', 'host-visible']
                : ['host-visible'];
            caps.diagnostics =
                `relayDrm=${relayCaps.drmRenderMajor ?? 0}:${relayCaps.drmRenderMinor ?? 0}`;

            for (const entry of relayFormats) {
                appendDmaBufModifierCap(caps,
                                        Number(entry.fourcc),
                                        entry.modifier,
                                        Number(entry.planeCount ?? 1));
            }
            if (caps.modifiers.length === 0)
                caps.memoryHints.push('implicit-linear');
            return caps;
        }

        caps.diagnostics = 'vulkan relay caps contained no supported RGBA formats';
    } else if (relayCaps) {
        caps.probe = String(relayCaps.probe ?? 'vulkan-relay-unavailable');
        caps.diagnostics =
            `vulkan relay unavailable stage=${relayCaps.stage ?? '(unknown)'} ` +
            `rc=${relayCaps.rc ?? '(unknown)'}`;
    } else {
        caps.probe = 'vulkan-relay-unavailable';
        caps.diagnostics = 'Vulkan relay capability query returned no result';
    }

    return caps;
}

const defaultSocketPath = () => {
    const runtimeDir = GLib.getenv('XDG_RUNTIME_DIR') || GLib.get_tmp_dir();
    return GLib.build_filenamev([runtimeDir, 'vivid', 'display-v1.sock']);
};

const parseArgs = argv => {
    const opts = {
        socketPath: defaultSocketPath(),
        outputs: null,
    };

    for (let i = 0; i < argv.length; i++) {
        if (argv[i] === '--socket' && i + 1 < argv.length)
            opts.socketPath = argv[++i];
        else if (argv[i] === '--outputs-json' && i + 1 < argv.length) {
            const json = argv[++i];
            try {
                const outputs = JSON.parse(json);
                opts.outputs = Array.isArray(outputs) ? outputs : null;
            } catch (error) {
                log(`invalid --outputs-json ignored: ${error}`);
            }
        }
    }

    return opts;
};

const bytesFromGBytes = bytes => {
    const data = bytes.get_data();
    return data instanceof Uint8Array ? data : new Uint8Array(data);
};

const readUint16LE = (bytes, offset) =>
    (bytes[offset] ?? 0) | ((bytes[offset + 1] ?? 0) << 8);

const readUint32LE = (bytes, offset) =>
    (bytes[offset] ?? 0) |
    ((bytes[offset + 1] ?? 0) << 8) |
    ((bytes[offset + 2] ?? 0) << 16) |
    ((bytes[offset + 3] ?? 0) << 24);

const readUint64LE = (bytes, offset) => {
    const low = readUint32LE(bytes, offset) >>> 0;
    const high = readUint32LE(bytes, offset + 4) >>> 0;
    return high * 0x100000000 + low;
};

const writeUint16LE = (bytes, offset, value) => {
    bytes[offset] = value & 0xff;
    bytes[offset + 1] = (value >> 8) & 0xff;
};

const writeUint32LE = (bytes, offset, value) => {
    bytes[offset] = value & 0xff;
    bytes[offset + 1] = (value >> 8) & 0xff;
    bytes[offset + 2] = (value >> 16) & 0xff;
    bytes[offset + 3] = (value >> 24) & 0xff;
};

const writeFloat32LE = (bytes, offset, value) => {
    const view = new DataView(bytes.buffer, bytes.byteOffset + offset, 4);
    view.setFloat32(0, Number(value) || 0, true);
};

const writeUint64LE = (bytes, offset, value) => {
    const normalized = Math.max(0, Math.floor(Number(value) || 0));
    writeUint32LE(bytes, offset, normalized >>> 0);
    writeUint32LE(bytes, offset + 4, Math.floor(normalized / 0x100000000) >>> 0);
};

const encodeFrame = (opcode, body = new Uint8Array(0)) => {
    if (body.length > MAX_BODY_BYTES)
        throw new Error(`frame body too large: ${body.length}`);

    const frame = new Uint8Array(FRAME_HEADER_BYTES + body.length);
    writeUint16LE(frame, 0, opcode);
    writeUint16LE(frame, 2, frame.length);
    frame.set(body, FRAME_HEADER_BYTES);
    return frame;
};

const encodeJsonFrame = (opcode, payload = {}) =>
    encodeFrame(opcode, encoder.encode(JSON.stringify(payload)));

const decodeJsonPayload = bytes => {
    if (!bytes || bytes.length === 0)
        return {};
    const text = decoder.decode(bytes);
    if (text.trim() === '')
        return {};
    const parsed = JSON.parse(text);
    return parsed && typeof parsed === 'object' ? parsed : {};
};

const decodeJsonTextPayload = bytes => {
    const text = !bytes || bytes.length === 0 ? '{}' : decoder.decode(bytes);
    const parsed = text.trim() === '' ? {} : JSON.parse(text);
    return {
        text,
        payload: parsed && typeof parsed === 'object' ? parsed : {},
    };
};

const decodeFrameReadyBody = decodeFrameReady;
const decodeUnbindBody = decodeUnbind;

const formatGenerationKeys = generations =>
    [...generations.keys()].sort((a, b) => a - b).join(',') || '(none)';

const formatBufferSummary = payload => {
    const buffers = Array.isArray(payload.buffers) ? payload.buffers : [];
    return buffers.map(buffer => {
        const planes = Array.isArray(buffer.planes) ? buffer.planes : [];
        const planeSummary = planes.map((plane, planeIndex) =>
            `p${planeIndex}:fd=${plane.fdIndex ?? planeIndex}/stride=${plane.stride}` +
            `/offset=${plane.offset ?? 0}`).join(';');
        return `b${buffer.index}:size=${buffer.size}:planes=[${planeSummary}]`;
    }).join(' ') || '(none)';
};

const formatUsec = usec => `${(Number(usec) / 1000).toFixed(2)}ms`;

const transformCode = value => {
    if (Number.isFinite(Number(value)))
        return Number(value);
    switch (String(value ?? 'normal').toLowerCase()) {
    case '90':
    case 'rotate-90':
    case 'rotated-90':
        return 1;
    case '180':
    case 'rotate-180':
    case 'rotated-180':
        return 2;
    case '270':
    case 'rotate-270':
    case 'rotated-270':
        return 3;
    case 'flipped':
    case 'flipped-normal':
        return 4;
    case 'flipped-90':
        return 5;
    case 'flipped-180':
        return 6;
    case 'flipped-270':
        return 7;
    case 'normal':
    default:
        return 0;
    }
};

const monitorScale = monitor => {
    try {
        if (typeof monitor.get_scale === 'function')
            return Number(monitor.get_scale());
    } catch (_e) {
    }
    try {
        if (typeof monitor.get_scale_factor === 'function')
            return Number(monitor.get_scale_factor());
    } catch (_e) {
    }
    return 1;
};

const monitorRefreshRate = monitor => {
    try {
        return Number(monitor.get_refresh_rate?.() ?? 60000);
    } catch (_e) {
        return 60000;
    }
};

const monitorDisplayName = monitor => {
    const parts = [];
    for (const getter of ['get_connector', 'get_manufacturer', 'get_model']) {
        try {
            const value = monitor?.[getter]?.();
            if (value)
                parts.push(String(value));
        } catch (_e) {
        }
    }
    return parts.join(' ').trim();
};

const monitorConnector = monitor => {
    try {
        const value = monitor?.get_connector?.();
        return typeof value === 'string' ? value.trim() : '';
    } catch (_e) {
        return '';
    }
};

const finiteNumber = (value, fallback) => {
    const number = Number(value);
    return Number.isFinite(number) ? number : fallback;
};

const positiveInt = (value, fallback) =>
    Math.max(1, Math.round(finiteNumber(value, fallback)));

const monitorInfoFromGdkMonitor = (monitor, monitorIndex) => {
    const geometry = monitor.get_geometry();
    const scale = Math.max(1, finiteNumber(monitorScale(monitor), 1));
    const logicalWidth = positiveInt(geometry.width, 1);
    const logicalHeight = positiveInt(geometry.height, 1);
    return {
        monitorIndex,
        consumerOutputId: monitorIndex + 1,
        displayKey: monitorConnector(monitor),
        x: Math.round(finiteNumber(geometry.x, 0)),
        y: Math.round(finiteNumber(geometry.y, 0)),
        logicalWidth,
        logicalHeight,
        scale,
        physicalWidth: positiveInt(logicalWidth * scale, logicalWidth),
        physicalHeight: positiveInt(logicalHeight * scale, logicalHeight),
        refreshRate: Math.max(0, Math.round(finiteNumber(monitorRefreshRate(monitor), 60000))),
        displayName: monitorDisplayName(monitor),
    };
};

const isPlaceholderDisplayName = (name, monitorIndex) => {
    const text = String(name ?? '').trim();
    if (!text)
        return true;
    if (text === `Monitor ${monitorIndex + 1}`)
        return true;
    return /^Monitor \d+$/.test(text);
};

const sameMonitorGeometry = (left, right) =>
    left.x === right.x &&
    left.y === right.y &&
    left.logicalWidth === right.logicalWidth &&
    left.logicalHeight === right.logicalHeight;

const findGdkDisplayNameForMonitorInfo = (display, target) => {
    const monitors = display?.get_monitors?.();
    const nItems = monitors?.get_n_items?.() ?? 0;
    for (let i = 0; i < nItems; i++) {
        const monitor = monitors.get_item(i);
        if (!monitor)
            continue;
        const info = monitorInfoFromGdkMonitor(monitor, i);
        if (sameMonitorGeometry(info, target) && info.displayName)
            return info.displayName;
    }

    const monitor = target.monitorIndex >= 0 && target.monitorIndex < nItems
        ? monitors.get_item(target.monitorIndex)
        : null;
    return monitor ? monitorDisplayName(monitor) : '';
};

const findGdkConnectorForMonitorInfo = (display, target) => {
    const monitors = display?.get_monitors?.();
    const nItems = monitors?.get_n_items?.() ?? 0;
    for (let i = 0; i < nItems; i++) {
        const monitor = monitors.get_item(i);
        if (!monitor)
            continue;
        const info = monitorInfoFromGdkMonitor(monitor, i);
        if (sameMonitorGeometry(info, target) && info.displayKey)
            return info.displayKey;
    }

    const monitor = target.monitorIndex >= 0 && target.monitorIndex < nItems
        ? monitors.get_item(target.monitorIndex)
        : null;
    return monitor ? monitorConnector(monitor) : '';
};

const monitorInfoFromPayload = (payload, fallbackIndex, display = null) => {
    const monitorIndex = Math.max(0, Math.round(finiteNumber(payload?.monitorIndex, fallbackIndex)));
    const scale = Math.max(1, finiteNumber(payload?.scale, 1));
    const logicalWidth = positiveInt(payload?.width ?? payload?.logicalWidth, 1);
    const logicalHeight = positiveInt(payload?.height ?? payload?.logicalHeight, 1);
    const info = {
        monitorIndex,
        consumerOutputId: positiveInt(payload?.consumerOutputId, monitorIndex + 1),
        layoutOutputCount: positiveInt(payload?.layoutOutputCount, 0),
        x: Math.round(finiteNumber(payload?.x, 0)),
        y: Math.round(finiteNumber(payload?.y, 0)),
        logicalWidth,
        logicalHeight,
        scale,
        physicalWidth: positiveInt(payload?.physicalWidth, logicalWidth * scale),
        physicalHeight: positiveInt(payload?.physicalHeight, logicalHeight * scale),
        refreshRate: Math.max(0, Math.round(finiteNumber(payload?.refreshRateMhz, 0))),
        displayKey: String(payload?.displayKey ?? '').trim(),
        displayName: String(payload?.displayName ?? `Monitor ${monitorIndex + 1}`).trim(),
    };
    if (!info.displayKey)
        info.displayKey = findGdkConnectorForMonitorInfo(display, info);
    if (isPlaceholderDisplayName(info.displayName, monitorIndex)) {
        const gdkName = findGdkDisplayNameForMonitorInfo(display, info);
        info.displayName = gdkName || `Monitor ${monitorIndex + 1}`;
    }
    return info;
};

const getSceneMediaCacheDir = () => {
    /*
     * Media thumbnails are handed to the producer by pathname, not by bytes.
     * A Flatpak producer gets a private /tmp, so host-side files under /tmp are
     * invisible even when the consumer wrote them successfully. The display
     * socket already lives under xdg-run/vivid, and the Flatpak manifest grants
     * access to that subtree, so keep media cache files beside the socket.
     */
    const runtimeDir = GLib.get_user_runtime_dir();
    if (runtimeDir)
        return GLib.build_filenamev([runtimeDir, 'vivid', 'scene-media-cache']);

    return GLib.build_filenamev([GLib.get_tmp_dir(), 'vivid-scene-media-cache']);
};

const MEDIA_CACHE_DIR = getSceneMediaCacheDir();
const MEDIA_PLAYBACK_STOPPED = 0;
const MEDIA_PLAYBACK_PLAYING = 1;
const MEDIA_PLAYBACK_PAUSED = 2;
const MEDIA_PLAYBACK_OTHER = 3;
const SCENE_MEDIA_DEBOUNCE_DELAY_MS = 80;
const SCENE_MEDIA_POLL_INTERVAL_MS = 1000;
const SCENE_MEDIA_SLOW_OPERATION_THRESHOLD_US = 20_000;
const THUMBNAIL_DECODE_SIZE = 512;

// Palette constants are intentionally grouped because they tune one algorithm:
// album-art quantization. Keeping the thresholds here makes it clear that media
// color extraction is a runtime payload concern, independent from display or
// socket transport code.
const MEDIA_PALETTE_SAMPLE_GRID_SIZE = 48;
const MEDIA_PALETTE_OCTREE_MAX_DEPTH = 6;
const MEDIA_PALETTE_MAX_SWATCHES = 12;
const MEDIA_PALETTE_MINIMUM_ALPHA = 16;
const MEDIA_PALETTE_DISTINCT_COLOR_DISTANCE = 0.045;
const MEDIA_PALETTE_HIGH_CONTRAST_LUMINANCE = 0.55;
const MEDIA_PALETTE_DARK_TEXT_COLOR = [0.05, 0.05, 0.05];
const MEDIA_PALETTE_LIGHT_TEXT_COLOR = [0.95, 0.95, 0.95];
const MEDIA_PALETTE_EMPTY_PRIMARY_COLOR = [0, 0, 0];
const MEDIA_PALETTE_EMPTY_SECONDARY_COLOR = [1, 1, 1];
const MEDIA_PALETTE_SECONDARY_PRIMARY_WEIGHT = 0.7;
const MEDIA_PALETTE_SECONDARY_TEXT_WEIGHT = 0.3;
const MEDIA_PALETTE_RANK_POPULATION_FLOOR = 0.35;
const MEDIA_PALETTE_RANK_LUMINANCE_WEIGHT = 1.15;
const MEDIA_PALETTE_RANK_TARGET_LUMINANCE = 0.52;

const WEB_AUDIO_UPDATE_INTERVAL_NS = 33_000_000;
const WEB_AUDIO_UPDATE_INTERVAL_MS = Math.max(1, Math.round(WEB_AUDIO_UPDATE_INTERVAL_NS / 1_000_000));
const WEB_AUDIO_OUTPUT_BANDS_PER_CHANNEL = 64;
const WEB_AUDIO_FRAME_LENGTH = WEB_AUDIO_OUTPUT_BANDS_PER_CHANNEL * 2;
const WEB_AUDIO_CAPTURE_POLL_INTERVAL_MS = 5;
const WEB_AUDIO_CAPTURE_LATENCY_US = Math.max(1, Math.round(WEB_AUDIO_UPDATE_INTERVAL_NS / 1000));
const WEB_AUDIO_CAPTURE_BUFFER_US = WEB_AUDIO_CAPTURE_LATENCY_US * 2;
const WEB_AUDIO_CAPTURE_QUEUE_BUFFERS = Math.ceil(
    WEB_AUDIO_CAPTURE_BUFFER_US / WEB_AUDIO_CAPTURE_LATENCY_US
);
const WEB_AUDIO_RESTART_DELAY_MS = 1000;
const WEB_AUDIO_REFERENCE_SAMPLE_RATE = 44100;
const WEB_AUDIO_WINDOW_PARAMETER = 30.0;
const WEB_AUDIO_STEP_PARAMETER = 10.0;
const WEB_AUDIO_INPUT_BIAS = 127.0;
const WEB_AUDIO_BAND_EXPONENT = 0.25;
const WEB_AUDIO_WEIGHT_CENTER = 0.5009999871;
const WEB_AUDIO_OUTPUT_GAIN = 1.0;
const AUDIO_SAMPLE_MAX_VALUES = VIVID_DISPLAY_AUDIO_SAMPLES_BIN_MAX_COUNT;

const cloneArray = values => {
    if (!values || typeof values.length !== 'number')
        return [];
    return Array.from(values, value => Number(value) || 0);
};
const clampColorChannel = value => Math.max(0, Math.min(1, Number(value) || 0));
const clampByte = value => Math.max(0, Math.min(255, Number(value) || 0));
const clipNumber = (value, min, max) => Math.max(min, Math.min(max, value));
const cloneMediaColor = color => color.map(channel => clampColorChannel(channel));
const normalizeByteColor = color => color.map(channel => clampColorChannel(channel / 255));
const formatMediaColorForLog = color => (Array.isArray(color) ? color : [])
    .map(channel => clampColorChannel(channel).toFixed(3))
    .join(',');
const colorLuminance = color =>
    0.2126 * color[0] + 0.7152 * color[1] + 0.0722 * color[2];
const colorDistanceSquared = (left, right) =>
    (left[0] - right[0]) ** 2 + (left[1] - right[1]) ** 2 + (left[2] - right[2]) ** 2;
const deriveSecondaryColor = (primaryColor, textColor) => primaryColor.map(
    (channel, index) => clampColorChannel(
        channel * MEDIA_PALETTE_SECONDARY_PRIMARY_WEIGHT +
        (textColor[index] ?? 1) * MEDIA_PALETTE_SECONDARY_TEXT_WEIGHT
    )
);

const defaultMediaStatePayload = () => ({
    title: '',
    artist: '',
    albumTitle: '',
    albumArtist: '',
    subTitle: '',
    genres: '',
    contentType: '',
    hasThumbnail: false,
    playbackState: MEDIA_PLAYBACK_STOPPED,
    primaryColor: [0, 0, 0],
    secondaryColor: [1, 1, 1],
    tertiaryColor: [1, 1, 1],
    textColor: [1, 1, 1],
    highContrastColor: [1, 1, 1],
    thumbnailPath: '',
});

const normalizeMediaStatePayload = payload => {
    const normalized = defaultMediaStatePayload();
    if (!payload || typeof payload !== 'object')
        return normalized;

    for (const key of ['title', 'artist', 'albumTitle', 'albumArtist', 'subTitle', 'genres', 'contentType', 'thumbnailPath'])
        normalized[key] = String(payload[key] ?? normalized[key]);

    normalized.hasThumbnail = Boolean(payload.hasThumbnail);
    normalized.playbackState = Number.isFinite(Number(payload.playbackState))
        ? Math.max(0, Math.min(3, Math.floor(Number(payload.playbackState))))
        : normalized.playbackState;

    for (const key of ['primaryColor', 'secondaryColor', 'tertiaryColor', 'textColor', 'highContrastColor']) {
        if (Array.isArray(payload[key]) && payload[key].length >= 3)
            normalized[key] = payload[key].slice(0, 3).map(channel => clampColorChannel(channel));
    }

    return normalized;
};

const normalizeAudioSamplesPayload = samples => {
    if (!samples || samples.length !== AUDIO_SAMPLE_MAX_VALUES)
        return buildSilentWebAudioFrame();

    const normalized = buildSilentWebAudioFrame();
    for (let index = 0; index < AUDIO_SAMPLE_MAX_VALUES; index++) {
        const value = Number(samples[index] ?? 0);
        normalized[index] = Number.isFinite(value) && value > 0 ? value : 0;
    }
    return normalized;
};

const createOctreeColorNode = (level, maxDepth) => ({
    level,
    isLeaf: level >= maxDepth,
    count: 0,
    r: 0,
    g: 0,
    b: 0,
    children: level >= maxDepth ? null : new Array(8).fill(null),
});

const insertOctreeColor = (node, r, g, b, maxDepth) => {
    node.count++;
    node.r += r;
    node.g += g;
    node.b += b;
    if (node.isLeaf)
        return;

    const bit = 7 - node.level;
    const childIndex =
        (((r >> bit) & 1) << 2) |
        (((g >> bit) & 1) << 1) |
        ((b >> bit) & 1);
    if (!node.children[childIndex])
        node.children[childIndex] = createOctreeColorNode(node.level + 1, maxDepth);
    insertOctreeColor(node.children[childIndex], r, g, b, maxDepth);
};

const countOctreeLeaves = node => {
    if (!node)
        return 0;
    if (node.isLeaf)
        return 1;
    return node.children.reduce((sum, child) => sum + countOctreeLeaves(child), 0);
};

const findOctreeReductionCandidate = node => {
    if (!node || node.isLeaf)
        return null;

    let candidate = null;
    for (const child of node.children) {
        const childCandidate = findOctreeReductionCandidate(child);
        if (!childCandidate)
            continue;
        if (!candidate ||
            childCandidate.level > candidate.level ||
            (childCandidate.level === candidate.level && childCandidate.count < candidate.count))
            candidate = childCandidate;
    }

    const childCount = node.children.filter(Boolean).length;
    if (childCount > 0) {
        if (!candidate ||
            node.level > candidate.level ||
            (node.level === candidate.level && node.count < candidate.count))
            candidate = node;
    }

    return candidate;
};

const reduceOctreeColorNode = node => {
    if (!node || node.isLeaf)
        return 0;

    /*
     * Collapse the deepest low-population branch while preserving accumulated
     * RGB totals. This keeps representative album-art accents without storing
     * every sampled pixel or passing raw image bytes through the display socket.
     */
    const removedLeaves = countOctreeLeaves(node);
    node.isLeaf = true;
    node.children = null;
    return Math.max(0, removedLeaves - 1);
};

const collectOctreeSwatches = (node, swatches = []) => {
    if (!node)
        return swatches;
    if (node.isLeaf) {
        if (node.count > 0) {
            swatches.push({
                count: node.count,
                color: [node.r / node.count, node.g / node.count, node.b / node.count],
            });
        }
        return swatches;
    }

    node.children.forEach(child => collectOctreeSwatches(child, swatches));
    return swatches;
};

const extractOctreePalette = (samples, maxSwatches) => {
    const root = createOctreeColorNode(0, MEDIA_PALETTE_OCTREE_MAX_DEPTH);
    samples.forEach(([r, g, b]) => insertOctreeColor(root, r, g, b, MEDIA_PALETTE_OCTREE_MAX_DEPTH));

    let leafCount = countOctreeLeaves(root);
    while (leafCount > maxSwatches) {
        const candidate = findOctreeReductionCandidate(root);
        if (!candidate)
            break;
        const removedLeaves = reduceOctreeColorNode(candidate);
        if (removedLeaves <= 0)
            break;
        leafCount -= removedLeaves;
    }

    return collectOctreeSwatches(root);
};

const rankPaletteSwatch = swatch => {
    const maxChannel = Math.max(swatch.color[0], swatch.color[1], swatch.color[2]);
    const minChannel = Math.min(swatch.color[0], swatch.color[1], swatch.color[2]);
    const saturation = maxChannel <= 0 ? 0 : (maxChannel - minChannel) / maxChannel;
    const luminance = colorLuminance(normalizeByteColor(swatch.color));

    /*
     * Album art often has large black or white borders. Ranking by population,
     * saturation, and mid-tone luminance produces a useful accent color for
     * scene scripts instead of a flat average that ignores the actual cover.
     */
    return swatch.count * (MEDIA_PALETTE_RANK_POPULATION_FLOOR + saturation) *
        (MEDIA_PALETTE_RANK_LUMINANCE_WEIGHT - Math.abs(luminance - MEDIA_PALETTE_RANK_TARGET_LUMINANCE));
};

const computeArtworkPalette = pixbuf => {
    const width = pixbuf.get_width();
    const height = pixbuf.get_height();
    const rowstride = pixbuf.get_rowstride();
    const channels = pixbuf.get_n_channels();
    const pixels = pixbuf.get_pixels();
    const samples = [];
    const stepY = Math.max(1, Math.floor(height / MEDIA_PALETTE_SAMPLE_GRID_SIZE));
    const stepX = Math.max(1, Math.floor(width / MEDIA_PALETTE_SAMPLE_GRID_SIZE));
    let totalR = 0;
    let totalG = 0;
    let totalB = 0;
    let count = 0;

    for (let y = 0; y < height; y += stepY) {
        for (let x = 0; x < width; x += stepX) {
            const offset = y * rowstride + x * channels;
            const alpha = channels >= 4 ? pixels[offset + 3] : 255;
            if (alpha < MEDIA_PALETTE_MINIMUM_ALPHA)
                continue;

            const r = clampByte(pixels[offset]);
            const g = clampByte(pixels[offset + 1]);
            const b = clampByte(pixels[offset + 2]);
            totalR += r;
            totalG += g;
            totalB += b;
            count++;
            samples.push([r, g, b]);
        }
    }

    if (count === 0) {
        return {
            primaryColor: cloneMediaColor(MEDIA_PALETTE_EMPTY_PRIMARY_COLOR),
            secondaryColor: cloneMediaColor(MEDIA_PALETTE_EMPTY_SECONDARY_COLOR),
            tertiaryColor: cloneMediaColor(MEDIA_PALETTE_EMPTY_SECONDARY_COLOR),
            textColor: cloneMediaColor(MEDIA_PALETTE_EMPTY_SECONDARY_COLOR),
            highContrastColor: cloneMediaColor(MEDIA_PALETTE_EMPTY_SECONDARY_COLOR),
        };
    }

    const average = normalizeByteColor([totalR / count, totalG / count, totalB / count]);
    const ranked = extractOctreePalette(samples, MEDIA_PALETTE_MAX_SWATCHES)
        .sort((left, right) => rankPaletteSwatch(right) - rankPaletteSwatch(left));
    const chooseDistinct = fallback => {
        const picked = [];
        for (const swatch of ranked) {
            const normalized = normalizeByteColor(swatch.color);
            if (picked.every(color =>
                colorDistanceSquared(color, normalized) > MEDIA_PALETTE_DISTINCT_COLOR_DISTANCE))
                picked.push(normalized);
            if (picked.length >= 3)
                break;
        }
        while (picked.length < 3)
            picked.push(picked[picked.length - 1] ?? fallback);
        return picked;
    };
    const [primaryColor, secondaryCandidate, tertiaryCandidate] = chooseDistinct(average);
    const highContrastColor = colorLuminance(primaryColor) > MEDIA_PALETTE_HIGH_CONTRAST_LUMINANCE
        ? cloneMediaColor(MEDIA_PALETTE_DARK_TEXT_COLOR)
        : cloneMediaColor(MEDIA_PALETTE_LIGHT_TEXT_COLOR);
    const secondaryColor = secondaryCandidate ?? deriveSecondaryColor(primaryColor, highContrastColor);
    const tertiaryColor = tertiaryCandidate ?? average;

    return {
        primaryColor,
        secondaryColor,
        tertiaryColor,
        textColor: highContrastColor,
        highContrastColor,
    };
};

const mapPlaybackState = playbackStatus => {
    switch (String(playbackStatus ?? '')) {
    case 'Playing':
        return MEDIA_PLAYBACK_PLAYING;
    case 'Paused':
        return MEDIA_PLAYBACK_PAUSED;
    case 'Stopped':
    case '':
        return MEDIA_PLAYBACK_STOPPED;
    default:
        return MEDIA_PLAYBACK_OTHER;
    }
};

const readFileAsync = file => new Promise((resolve, reject) => {
    file.read_async(GLib.PRIORITY_DEFAULT, null, (source, result) => {
        try {
            resolve(source.read_finish(result));
        } catch (error) {
            reject(error);
        }
    });
});

const replaceFileAsync = file => new Promise((resolve, reject) => {
    file.replace_async(
        null,
        false,
        Gio.FileCreateFlags.REPLACE_DESTINATION,
        GLib.PRIORITY_DEFAULT,
        null,
        (source, result) => {
            try {
                resolve(source.replace_finish(result));
            } catch (error) {
                reject(error);
            }
        }
    );
});

const closeStreamAsync = stream => new Promise((resolve, reject) => {
    stream.close_async(GLib.PRIORITY_DEFAULT, null, (source, result) => {
        try {
            resolve(source.close_finish(result));
        } catch (error) {
            reject(error);
        }
    });
});

const loadScaledPixbufAsync = stream => new Promise((resolve, reject) => {
    if (!GdkPixbuf) {
        reject(new Error('GdkPixbuf is unavailable'));
        return;
    }

    GdkPixbuf.Pixbuf.new_from_stream_at_scale_async(
        stream,
        THUMBNAIL_DECODE_SIZE,
        THUMBNAIL_DECODE_SIZE,
        true,
        null,
        (_source, result) => {
            try {
                resolve(GdkPixbuf.Pixbuf.new_from_stream_finish(result));
            } catch (error) {
                reject(error);
            }
        }
    );
});

const savePixbufToPngStreamAsync = (pixbuf, outputStream) => new Promise((resolve, reject) => {
    pixbuf.save_to_streamv_async(outputStream, 'png', [], [], null, (source, result) => {
        try {
            resolve(GdkPixbuf.Pixbuf.save_to_stream_finish(result));
        } catch (error) {
            reject(error);
        }
    });
});

const getLocalArtworkPath = artUrl => {
    if (!artUrl)
        return null;

    if (!GLib.uri_parse_scheme(artUrl))
        return artUrl;

    try {
        const file = Gio.File.new_for_uri(artUrl);
        return file.is_native() ? file.get_path() : null;
    } catch (_e) {
        return null;
    }
};

const closeStreamQuietlyAsync = async stream => {
    if (!stream)
        return;

    try {
        await closeStreamAsync(stream);
    } catch (_e) {
    }
};

class SceneMediaMonitor {
    constructor(onStateChanged) {
        this._onStateChanged = onStateChanged;
        this._thumbnailCache = new Map();
        this._thumbnailLoads = new Map();
        this._lastPayloadJson = '';
        this._lastPayload = defaultMediaStatePayload();
        this._pendingActive = null;
        this._recomputeSourceId = 0;
        this._pollSourceId = 0;
        this._pollInFlight = false;
        this._lastPolledSnapshot = null;
        this._lastPolledSignature = '';
        this._recomputeSerial = 0;
        this._destroyed = false;
        GLib.mkdir_with_parents(MEDIA_CACHE_DIR, 0o755);
    }

    start() {
        this._emitPayload(defaultMediaStatePayload(), {force: true});

        if (!Mpris?.MprisMonitor) {
            log('media monitor disabled: common/mpris.js was not loaded');
            return;
        }

        this._monitor = new Mpris.MprisMonitor({
            warn: message => log(`media monitor: ${message}`),
            onChanged: ({active, snapshots}) => {
                this._scheduleRecompute(active);
            },
        });
        this._startPolling();
        const initialActive = this._monitor.getActiveSnapshot();
        if (initialActive)
            this._scheduleRecompute(initialActive);
    }

    refresh() {
        if (!this._monitor)
            return;

        /*
         * The helper can outlive a direct-run producer across many reconnects.
         * Refreshing from the current MPRIS snapshot when the transport comes
         * back makes the media payload self-healing even if a player appeared
         * while the producer socket was down or a previous DBus notification was
         * coalesced before the display connection existed.
         */
        this._scheduleRecompute(this._monitor.getActiveSnapshot() ?? this._lastPolledSnapshot);
    }

    destroy() {
        this._destroyed = true;
        this._recomputeSerial++;
        if (this._recomputeSourceId) {
            GLib.source_remove(this._recomputeSourceId);
            this._recomputeSourceId = 0;
        }
        if (this._pollSourceId) {
            GLib.source_remove(this._pollSourceId);
            this._pollSourceId = 0;
        }

        this._monitor?.destroy?.();
        this._monitor = null;
        this._pendingActive = null;
        this._lastPolledSnapshot = null;
        this._thumbnailCache.clear();
        this._thumbnailLoads.clear();
    }

    get currentPayload() {
        return normalizeMediaStatePayload(this._lastPayload);
    }

    _startPolling() {
        const hasSnapshotQuery = typeof Mpris?.queryMprisSnapshotsSync === 'function';
        if (this._pollSourceId || !hasSnapshotQuery)
            return;

        /*
         * DBus PropertiesChanged is the fast path, but the display helper is a
         * long-lived GTK process that may reconnect to direct-run producers while
         * players are already present. A low-frequency poll makes media delivery
         * self-healing without touching the high-rate audio sample path.
         */
        this._pollSourceId = GLib.timeout_add(
            GLib.PRIORITY_DEFAULT,
            SCENE_MEDIA_POLL_INTERVAL_MS,
            () => {
                void this._pollActiveSnapshotAsync();
                return GLib.SOURCE_CONTINUE;
            }
        );
        void this._pollActiveSnapshotAsync();
    }

    async _pollActiveSnapshotAsync() {
        if (this._destroyed || this._pollInFlight)
            return;

        this._pollInFlight = true;
        try {
            const snapshots = Mpris.queryMprisSnapshotsSync(Gio.DBus.session);
            const active = snapshots[0] ?? null;
            const signature = active
                ? `${active.name}|${active.playbackStatus}|${active.title}|${active.artist}|${active.artUrl}`
                : '(none)';
            if (signature === this._lastPolledSignature)
                return;

            this._lastPolledSignature = signature;
            this._lastPolledSnapshot = active ? {...active} : null;
            this._scheduleRecompute(this._lastPolledSnapshot);
        } catch (error) {
            log(`media poll failed: ${error}`);
        } finally {
            this._pollInFlight = false;
        }
    }

    _scheduleRecompute(active) {
        this._pendingActive = active ? {...active} : null;

        if (this._recomputeSourceId) {
            GLib.source_remove(this._recomputeSourceId);
            this._recomputeSourceId = 0;
        }

        this._recomputeSourceId = GLib.timeout_add(
            GLib.PRIORITY_DEFAULT,
            SCENE_MEDIA_DEBOUNCE_DELAY_MS,
            () => {
                this._recomputeSourceId = 0;
                const nextActive = this._pendingActive;
                this._pendingActive = null;
                void this._recomputeAsync(nextActive);
                return GLib.SOURCE_REMOVE;
            }
        );
    }

    async _writeThumbnailAsync(pixbuf, thumbnailPath) {
        const file = Gio.File.new_for_path(thumbnailPath);
        let outputStream = null;

        try {
            outputStream = await replaceFileAsync(file);
            await savePixbufToPngStreamAsync(pixbuf, outputStream);
        } finally {
            await closeStreamQuietlyAsync(outputStream);
        }
    }

    _writeThumbnailSync(pixbuf, thumbnailPath) {
        pixbuf.savev(thumbnailPath, 'png', [], []);
    }

    async _loadThumbnailAsync(artUrl) {
        if (!artUrl || !GdkPixbuf)
            return null;

        const cached = this._thumbnailCache.get(artUrl);
        if (cached)
            return cached;

        const loading = this._thumbnailLoads.get(artUrl);
        if (loading)
            return loading;

        const loadPromise = this._loadThumbnailUncachedAsync(artUrl);
        this._thumbnailLoads.set(artUrl, loadPromise);

        try {
            const payload = await loadPromise;
            if (payload)
                this._thumbnailCache.set(artUrl, payload);
            return payload;
        } finally {
            this._thumbnailLoads.delete(artUrl);
        }
    }

    _loadThumbnailSyncIfLocal(artUrl) {
        if (!artUrl || !GdkPixbuf)
            return null;

        const cached = this._thumbnailCache.get(artUrl);
        if (cached)
            return cached;

        const localPath = getLocalArtworkPath(artUrl);
        if (!localPath)
            return null;

        /*
         * Local MPRIS artwork is the common path for GNOME media players. Decode
         * and cache it synchronously before emitting media state so the first
         * payload sent to the producer already contains title, colors, and the
         * thumbnail path. This mirrors the legacy single complete media state
         * update and avoids startup empty-state races in display-helper.
        */
        const startedAtUs = GLib.get_monotonic_time();
        try {
            const pixbuf = GdkPixbuf.Pixbuf.new_from_file_at_scale(
                localPath,
                THUMBNAIL_DECODE_SIZE,
                THUMBNAIL_DECODE_SIZE,
                true
            );
            const hash = GLib.compute_checksum_for_string(GLib.ChecksumType.SHA256, artUrl, -1);
            const thumbnailPath = GLib.build_filenamev([MEDIA_CACHE_DIR, `${hash}.png`]);
            this._writeThumbnailSync(pixbuf, thumbnailPath);

            const palette = computeArtworkPalette(pixbuf);
            const payload = {thumbnailPath, ...palette};
            this._thumbnailCache.set(artUrl, payload);

            const elapsedUs = GLib.get_monotonic_time() - startedAtUs;
            if (elapsedUs >= SCENE_MEDIA_SLOW_OPERATION_THRESHOLD_US) {
                log(
                    `media thumbnail load slow: ${(elapsedUs / 1000).toFixed(2)}ms ` +
                    `artUrl=${artUrl} cachePath=${thumbnailPath} ` +
                    `primary=${formatMediaColorForLog(palette.primaryColor)} ` +
                    `secondary=${formatMediaColorForLog(palette.secondaryColor)}`
                );
            }
            return payload;
        } catch (error) {
            log(`media thumbnail local load failed path=${localPath}: ${error}`);
            return null;
        }
    }

    async _loadThumbnailUncachedAsync(artUrl) {
        const startedAtUs = GLib.get_monotonic_time();
        let stream = null;
        try {
            const file = GLib.uri_parse_scheme(artUrl)
                ? Gio.File.new_for_uri(artUrl)
                : Gio.File.new_for_path(artUrl);
            stream = await readFileAsync(file);
            const pixbuf = await loadScaledPixbufAsync(stream);
            const hash = GLib.compute_checksum_for_string(GLib.ChecksumType.SHA256, artUrl, -1);
            const thumbnailPath = GLib.build_filenamev([MEDIA_CACHE_DIR, `${hash}.png`]);
            await this._writeThumbnailAsync(pixbuf, thumbnailPath);

            const palette = computeArtworkPalette(pixbuf);
            const payload = {thumbnailPath, ...palette};
            const elapsedUs = GLib.get_monotonic_time() - startedAtUs;
            if (elapsedUs >= SCENE_MEDIA_SLOW_OPERATION_THRESHOLD_US) {
                log(
                    `media thumbnail load slow: ${(elapsedUs / 1000).toFixed(2)}ms ` +
                    `artUrl=${artUrl} cachePath=${thumbnailPath} ` +
                    `primary=${formatMediaColorForLog(palette.primaryColor)} ` +
                    `secondary=${formatMediaColorForLog(palette.secondaryColor)}`
                );
            }
            return payload;
        } catch (error) {
            log(`media thumbnail load failed artUrl=${artUrl}: ${error}`);
            return null;
        } finally {
            await closeStreamQuietlyAsync(stream);
        }
    }

    _emitPayload(payload, {force = false} = {}) {
        const normalized = normalizeMediaStatePayload(payload);
        const nextJson = JSON.stringify(normalized);
        if (!force && nextJson === this._lastPayloadJson)
            return;

        this._lastPayloadJson = nextJson;
        this._lastPayload = normalized;
        this._onStateChanged?.(normalized);
    }

    async _recomputeAsync(active) {
        const recomputeSerial = ++this._recomputeSerial;
        let payload = defaultMediaStatePayload();

        if (active) {
            payload.title = active.title || '';
            payload.artist = active.artist || '';
            payload.albumTitle = active.albumTitle || '';
            payload.albumArtist = active.albumArtist || '';
            payload.subTitle = active.subTitle || '';
            payload.genres = active.genres || '';
            payload.contentType = active.contentType || '';
            payload.playbackState = mapPlaybackState(active.playbackStatus);

            let thumbnail = this._loadThumbnailSyncIfLocal(active.artUrl);
            if (thumbnail) {
                payload = {
                    ...payload,
                    hasThumbnail: true,
                    primaryColor: thumbnail.primaryColor,
                    secondaryColor: thumbnail.secondaryColor,
                    tertiaryColor: thumbnail.tertiaryColor,
                    textColor: thumbnail.textColor,
                    highContrastColor: thumbnail.highContrastColor,
                    thumbnailPath: thumbnail.thumbnailPath,
                };
                this._emitPayload(payload);
                return;
            }

            this._emitPayload(payload);

            thumbnail = await this._loadThumbnailAsync(active.artUrl);
            if (this._destroyed || recomputeSerial !== this._recomputeSerial)
                return;

            if (thumbnail) {
                payload = {
                    ...payload,
                    hasThumbnail: true,
                    primaryColor: thumbnail.primaryColor,
                    secondaryColor: thumbnail.secondaryColor,
                    tertiaryColor: thumbnail.tertiaryColor,
                    textColor: thumbnail.textColor,
                    highContrastColor: thumbnail.highContrastColor,
                    thumbnailPath: thumbnail.thumbnailPath,
                };
            }
        }

        if (this._destroyed || recomputeSerial !== this._recomputeSerial)
            return;

        this._emitPayload(payload);
    }
}

const buildSilentWebAudioFrame = () => new Array(WEB_AUDIO_FRAME_LENGTH).fill(0);

/**
 * Read the PulseAudio default sink mix rate instead of asking GStreamer to choose a
 * client-side default. Wallpaper Engine derives its transform length from the
 * render endpoint sample rate, so resampling a 48 kHz desktop mix to 44.1 kHz
 * changes the FFT length, populated input count, leakage shape, and output gain.
 */
const detectPulseDefaultSinkSampleRate = () => {
    const pactl = GLib.find_program_in_path('pactl');
    if (!pactl)
        throw new Error('pactl is required to query the PulseAudio server sample rate');

    const runJson = argumentsList => {
        const process = Gio.Subprocess.new(
            [pactl, '-f', 'json', ...argumentsList],
            Gio.SubprocessFlags.STDOUT_PIPE | Gio.SubprocessFlags.STDERR_PIPE
        );
        const [communicated, stdout, stderr] = process.communicate_utf8(null, null);
        if (!communicated || !process.get_successful()) {
            throw new Error(
                `pactl ${argumentsList.join(' ')} failed: ${String(stderr ?? '').trim()}`
            );
        }
        return JSON.parse(stdout);
    };

    const serverInfo = runJson(['info']);
    const sinks = runJson(['list', 'sinks']);
    const defaultSink = Array.isArray(sinks)
        ? sinks.find(sink => sink?.name === serverInfo.default_sink_name)
        : null;
    const specification = String(
        defaultSink?.sample_specification ?? serverInfo.default_sample_specification ?? ''
    );
    const match = specification.match(/\b(\d+)Hz\b/);
    const sampleRate = match ? Number(match[1]) : 0;
    if (!Number.isInteger(sampleRate) || sampleRate <= 0)
        throw new Error(`invalid PulseAudio sample specification: ${specification}`);
    return sampleRate;
};

/**
 * Execute an unscaled forward or scaled inverse radix-2 FFT in place. Wallpaper
 * Engine uses a sample-rate-derived transform length that is generally not a
 * power of two, so Bluestein wraps this primitive without changing that length.
 */
const transformRadix2InPlace = (real, imag, inverse) => {
    const size = Math.min(real.length, imag.length);
    for (let index = 1, reversed = 0; index < size; index++) {
        let bit = size >> 1;
        while (reversed & bit) {
            reversed ^= bit;
            bit >>= 1;
        }
        reversed ^= bit;
        if (index < reversed) {
            const realValue = real[index];
            real[index] = real[reversed];
            real[reversed] = realValue;
            const imagValue = imag[index];
            imag[index] = imag[reversed];
            imag[reversed] = imagValue;
        }
    }

    for (let blockSize = 2; blockSize <= size; blockSize <<= 1) {
        const halfSize = blockSize >> 1;
        const angle = (inverse ? 2 : -2) * Math.PI / blockSize;
        const phaseRealStep = Math.cos(angle);
        const phaseImagStep = Math.sin(angle);
        for (let offset = 0; offset < size; offset += blockSize) {
            let phaseReal = 1;
            let phaseImag = 0;
            for (let index = 0; index < halfSize; index++) {
                const evenIndex = offset + index;
                const oddIndex = evenIndex + halfSize;
                const oddReal = real[oddIndex] * phaseReal - imag[oddIndex] * phaseImag;
                const oddImag = real[oddIndex] * phaseImag + imag[oddIndex] * phaseReal;
                real[oddIndex] = real[evenIndex] - oddReal;
                imag[oddIndex] = imag[evenIndex] - oddImag;
                real[evenIndex] += oddReal;
                imag[evenIndex] += oddImag;
                const nextPhaseReal = phaseReal * phaseRealStep - phaseImag * phaseImagStep;
                const nextPhaseImag = phaseReal * phaseImagStep + phaseImag * phaseRealStep;
                phaseReal = nextPhaseReal;
                phaseImag = nextPhaseImag;
            }
        }
    }

    if (inverse) {
        for (let index = 0; index < size; index++) {
            real[index] /= size;
            imag[index] /= size;
        }
    }
};

/**
 * Precompute the convolution kernel for the exact complex DFT length. The plan
 * is immutable and shared by both channels; only per-channel workspaces mutate
 * while processing captured PCM.
 */
const createBluesteinPlan = transformSize => {
    let convolutionSize = 1;
    while (convolutionSize < transformSize * 2 - 1)
        convolutionSize <<= 1;

    const chirpReal = new Float64Array(transformSize);
    const chirpImag = new Float64Array(transformSize);
    const kernelReal = new Float64Array(convolutionSize);
    const kernelImag = new Float64Array(convolutionSize);
    for (let index = 0; index < transformSize; index++) {
        const angle = Math.PI * ((index * index) % (transformSize * 2)) / transformSize;
        const cosine = Math.cos(angle);
        const sine = Math.sin(angle);
        chirpReal[index] = cosine;
        chirpImag[index] = -sine;
        kernelReal[index] = cosine;
        kernelImag[index] = sine;
        if (index > 0) {
            kernelReal[convolutionSize - index] = cosine;
            kernelImag[convolutionSize - index] = sine;
        }
    }
    transformRadix2InPlace(kernelReal, kernelImag, false);
    return {convolutionSize, chirpReal, chirpImag, kernelReal, kernelImag};
};

const createSpectrumConfiguration = sampleRate => {
    if (!Number.isInteger(sampleRate) || sampleRate <= 0)
        throw new Error(`invalid audio sample rate ${sampleRate}`);

    const fftSize = Math.trunc(
        Math.max(sampleRate / WEB_AUDIO_REFERENCE_SAMPLE_RATE, 1.0) *
        WEB_AUDIO_OUTPUT_BANDS_PER_CHANNEL *
        WEB_AUDIO_WINDOW_PARAMETER
    );
    const analysisBinCount = Math.trunc(
        WEB_AUDIO_OUTPUT_BANDS_PER_CHANNEL * WEB_AUDIO_STEP_PARAMETER
    );
    const pcmSampleCount = Math.trunc(
        fftSize - (WEB_AUDIO_STEP_PARAMETER / WEB_AUDIO_WINDOW_PARAMETER) * fftSize
    );

    return {
        sampleRate,
        fftSize,
        pcmSampleCount,
        analysisBinCount,
        plan: createBluesteinPlan(fftSize),
    };
};

const createSpectrumProcessorState = configuration => ({
    real: new Float64Array(configuration.plan.convolutionSize),
    imag: new Float64Array(configuration.plan.convolutionSize),
    values: new Float32Array(WEB_AUDIO_OUTPUT_BANDS_PER_CHANNEL),
});

/**
 * Packet boundaries from PulseAudio and GStreamer are transport details. This
 * ring preserves the continuous PCM timeline and lets the independent 33 ms
 * analysis clock take the newest populated Wallpaper Engine window regardless
 * of how the server split or combined capture buffers.
 */
class StereoPcmRing {
    constructor(capacityFrames) {
        this._capacity = Math.max(1, Math.trunc(capacityFrames));
        this._left = new Float32Array(this._capacity);
        this._right = new Float32Array(this._capacity);
        this.reset();
    }

    get totalFrames() {
        return this._totalFrames;
    }

    reset() {
        this._left.fill(0);
        this._right.fill(0);
        this._writeIndex = 0;
        this._availableFrames = 0;
        this._totalFrames = 0;
    }

    appendInterleaved(interleaved, frameCount) {
        if (!(interleaved instanceof Float32Array) || frameCount <= 0)
            return 0;

        const availableInputFrames = Math.min(
            Math.trunc(frameCount),
            Math.floor(interleaved.length / 2)
        );
        for (let frame = 0; frame < availableInputFrames; frame++) {
            const sourceIndex = frame * 2;
            const left = Number(interleaved[sourceIndex] ?? 0);
            this._left[this._writeIndex] = left;
            this._right[this._writeIndex] = Number(interleaved[sourceIndex + 1] ?? left);
            this._writeIndex = (this._writeIndex + 1) % this._capacity;
        }

        this._availableFrames = Math.min(
            this._capacity,
            this._availableFrames + availableInputFrames
        );
        this._totalFrames += availableInputFrames;
        return availableInputFrames;
    }

    copyLatest(leftTarget, rightTarget) {
        if (!(leftTarget instanceof Float32Array) || !(rightTarget instanceof Float32Array) ||
            leftTarget.length !== rightTarget.length)
            throw new Error('invalid PCM snapshot buffers');

        leftTarget.fill(0);
        rightTarget.fill(0);
        const copyCount = Math.min(leftTarget.length, this._availableFrames);
        const targetOffset = leftTarget.length - copyCount;
        const sourceStart = (
            this._writeIndex - copyCount + this._capacity
        ) % this._capacity;
        for (let frame = 0; frame < copyCount; frame++) {
            const sourceIndex = (sourceStart + frame) % this._capacity;
            leftTarget[targetOffset + frame] = this._left[sourceIndex];
            rightTarget[targetOffset + frame] = this._right[sourceIndex];
        }
        return copyCount;
    }
}

const processSpectrumFrame = (pcm, state, configuration) => {
    if (!(pcm instanceof Float32Array) || !(state?.values instanceof Float32Array))
        return new Float32Array(WEB_AUDIO_OUTPUT_BANDS_PER_CHANNEL);

    const real = state.real;
    const imag = state.imag;
    const plan = configuration.plan;
    real.fill(0);
    imag.fill(0);

    /*
     * Wallpaper Engine derives both counts from the endpoint rate. It fills the
     * populated prefix from PCM and leaves the remaining complex slots at the
     * encoding of a zero sample. This biased reciprocal representation is not a
     * conventional real-input FFT and materially affects the result.
     */
    for (let index = 0; index < configuration.fftSize; index++) {
        const sample = index < configuration.pcmSampleCount ? Number(pcm[index] ?? 0) : 0;
        const inputReal = WEB_AUDIO_INPUT_BIAS * (sample + 1);
        const inputImag = 1 / inputReal;
        const chirpReal = plan.chirpReal[index];
        const chirpImag = plan.chirpImag[index];
        real[index] = inputReal * chirpReal - inputImag * chirpImag;
        imag[index] = inputReal * chirpImag + inputImag * chirpReal;
    }

    transformRadix2InPlace(real, imag, false);
    for (let index = 0; index < plan.convolutionSize; index++) {
        const valueReal = real[index];
        const valueImag = imag[index];
        const kernelReal = plan.kernelReal[index];
        const kernelImag = plan.kernelImag[index];
        real[index] = valueReal * kernelReal - valueImag * kernelImag;
        imag[index] = valueReal * kernelImag + valueImag * kernelReal;
    }
    transformRadix2InPlace(real, imag, true);

    for (let index = 0; index < configuration.fftSize; index++) {
        const valueReal = real[index];
        const valueImag = imag[index];
        const chirpReal = plan.chirpReal[index];
        const chirpImag = plan.chirpImag[index];
        real[index] = valueReal * chirpReal - valueImag * chirpImag;
        imag[index] = valueReal * chirpImag + valueImag * chirpReal;
    }

    const values = state.values;
    values.fill(0);
    let previousBand = 0;
    for (let bin = 1; bin < configuration.analysisBinCount; bin++) {
        const realValue = real[bin];
        const imagValue = imag[bin];
        let magnitudeSquared = realValue * realValue + imagValue * imagValue;
        if (!Number.isFinite(magnitudeSquared))
            magnitudeSquared = 0;

        const position = (bin - 1) / (configuration.analysisBinCount - 1);
        const rawBand = Math.trunc(
            Math.pow(position, WEB_AUDIO_BAND_EXPONENT) * WEB_AUDIO_OUTPUT_BANDS_PER_CHANNEL
        ) % WEB_AUDIO_OUTPUT_BANDS_PER_CHANNEL;
        const band = Math.min(rawBand, previousBand + 1);
        previousBand = band;

        const weight = WEB_AUDIO_WEIGHT_CENTER -
            Math.cos(Math.PI * position) * (1 - WEB_AUDIO_WEIGHT_CENTER);
        const magnitude = Math.sqrt(magnitudeSquared * weight);
        values[band] = Math.max(values[band], magnitude);
    }

    const outputScale = WEB_AUDIO_OUTPUT_GAIN * 0.001 * configuration.analysisBinCount /
        (configuration.fftSize * 0.5);
    for (let band = 0; band < values.length; band++)
        values[band] *= outputScale;
    return values;
};

const hasGstElementFactory = factoryName => {
    try {
        return Boolean(Gst?.ElementFactory.find(factoryName));
    } catch (_e) {
        return false;
    }
};

class WebAudioVisualizerCapture {
    constructor(onFrame) {
        this._onFrame = onFrame;
        this._pipeline = null;
        this._appsink = null;
        this._bus = null;
        this._busSignalIds = [];
        this._capturePollSourceId = 0;
        this._analysisSourceId = 0;
        this._processingSourceId = 0;
        this._restartSourceId = 0;
        this._shouldRun = false;
        this._isAvailable = true;
        this._lastFrame = buildSilentWebAudioFrame();
        this._workingFrame = buildSilentWebAudioFrame();
        this._configuration = null;
        this._pcmRing = null;
        this._leftSampleBuffer = new Float32Array(0);
        this._rightSampleBuffer = new Float32Array(0);
        this._leftProcessorState = null;
        this._rightProcessorState = null;
        this._lastAnalyzedTotalFrames = 0;
    }

    get currentFrame() {
        return cloneArray(this._lastFrame);
    }

    start() {
        this._shouldRun = true;
        if (this._pipeline || !this._isAvailable)
            return;

        this._cancelRestart();
        this._startPipeline();
    }

    stop({emitSilence = true} = {}) {
        this._shouldRun = false;
        this._cancelRestart();
        this._teardownPipeline();

        if (emitSilence)
            this._emitFrame(buildSilentWebAudioFrame());
    }

    destroy() {
        this.stop({emitSilence: false});
        this._onFrame = null;
    }

    _resetSpectrumState() {
        this._pcmRing?.reset();
        this._leftSampleBuffer.fill(0);
        this._rightSampleBuffer.fill(0);
        if (this._configuration) {
            this._leftProcessorState = createSpectrumProcessorState(this._configuration);
            this._rightProcessorState = createSpectrumProcessorState(this._configuration);
        }
        this._workingFrame.fill(0);
        this._lastAnalyzedTotalFrames = 0;
    }

    _configureSpectrum(sampleRate) {
        const configuration = createSpectrumConfiguration(sampleRate);
        const captureFramesPerTick = Math.ceil(
            sampleRate * WEB_AUDIO_UPDATE_INTERVAL_MS / 1000
        );
        const ringCapacity = configuration.pcmSampleCount + captureFramesPerTick * 2;

        this._configuration = configuration;
        this._pcmRing = new StereoPcmRing(ringCapacity);
        this._leftSampleBuffer = new Float32Array(configuration.pcmSampleCount);
        this._rightSampleBuffer = new Float32Array(configuration.pcmSampleCount);
        this._resetSpectrumState();
    }

    _startPipeline() {
        if (!Gst || !GstApp) {
            log('audio sample capture unavailable: GStreamer or GstApp typelib is missing');
            this._isAvailable = false;
            this._emitFrame(buildSilentWebAudioFrame());
            return;
        }

        if (!hasGstElementFactory('pulsesrc')) {
            log('audio sample capture unavailable: GStreamer pulsesrc plugin is missing');
            this._isAvailable = false;
            this._emitFrame(buildSilentWebAudioFrame());
            return;
        }

        if (!hasGstElementFactory('appsink')) {
            log('audio sample capture unavailable: GStreamer appsink plugin is missing');
            this._isAvailable = false;
            this._emitFrame(buildSilentWebAudioFrame());
            return;
        }

        let sampleRate = 0;
        try {
            sampleRate = detectPulseDefaultSinkSampleRate();
            this._configureSpectrum(sampleRate);
            this._pipeline = Gst.parse_launch(
                'pulsesrc device=@DEFAULT_MONITOR@ client-name=VividWallpaperVisualizer ' +
                `do-timestamp=true latency-time=${WEB_AUDIO_CAPTURE_LATENCY_US} ` +
                `buffer-time=${WEB_AUDIO_CAPTURE_BUFFER_US} ! ` +
                'audioconvert ! audioresample ! ' +
                `audio/x-raw,format=F32LE,channels=2,rate=${sampleRate} ! ` +
                `appsink name=audio_sink emit-signals=false ` +
                `max-buffers=${WEB_AUDIO_CAPTURE_QUEUE_BUFFERS} drop=true sync=false`
            );
        } catch (error) {
            log(`audio sample pipeline create failed: ${error}`);
            this._scheduleRestart();
            return;
        }

        this._appsink = this._pipeline.get_by_name('audio_sink');
        if (!this._appsink) {
            log('audio sample pipeline did not expose appsink');
            this._scheduleRestart();
            return;
        }

        this._bus = this._pipeline.get_bus();
        this._bus.add_signal_watch();
        this._busSignalIds = [
            this._bus.connect('message::error', (_bus, message) => {
                let details = '';
                try {
                    const [error, debugInfo] = message.parse_error();
                    details = error?.message ?? String(error ?? '');
                    if (debugInfo)
                        details = `${details} (${debugInfo})`;
                } catch (error) {
                    details = String(error);
                }
                log(`audio sample pipeline error: ${details}`);
                this._scheduleRestart();
            }),
            this._bus.connect('message::eos', () => {
                log('audio sample pipeline reached EOS unexpectedly');
                this._scheduleRestart();
            }),
        ];

        const stateChange = this._pipeline.set_state(Gst.State.PLAYING);
        if (stateChange === Gst.StateChangeReturn.FAILURE) {
            log('audio sample pipeline failed to enter PLAYING state');
            this._scheduleRestart();
        } else {
            this._capturePollSourceId = GLib.timeout_add(
                GLib.PRIORITY_DEFAULT,
                WEB_AUDIO_CAPTURE_POLL_INTERVAL_MS,
                () => {
                    try {
                        this._pullAvailableAudioSamples();
                    } catch (error) {
                        log(`audio sample polling failed: ${error}`);
                    }
                    return GLib.SOURCE_CONTINUE;
                }
            );
            this._analysisSourceId = GLib.timeout_add(
                GLib.PRIORITY_DEFAULT,
                WEB_AUDIO_UPDATE_INTERVAL_MS,
                () => {
                    this._scheduleSpectrumProcessing();
                    return GLib.SOURCE_CONTINUE;
                }
            );
        }
    }

    _pullAvailableAudioSamples() {
        if (!this._appsink || !this._configuration || !this._pcmRing)
            return;

        while (true) {
            const sample = this._appsink.emit('try-pull-sample', 0);
            if (!sample)
                break;

            const caps = sample.get_caps?.();
            const structure = caps?.get_structure?.(0);
            const negotiatedRate = Number(structure?.get_value?.('rate') ?? 0);
            if (negotiatedRate !== this._configuration.sampleRate) {
                log(
                    `audio sample rate changed expected=${this._configuration.sampleRate} ` +
                    `negotiated=${negotiatedRate}`
                );
                this._scheduleRestart();
                return;
            }

            const buffer = sample.get_buffer?.();
            if (!buffer)
                continue;

            const [mapped, mapInfo] = buffer.map(Gst.MapFlags.READ);
            if (!mapped || !mapInfo?.data || mapInfo.size < Float32Array.BYTES_PER_ELEMENT) {
                if (mapped)
                    buffer.unmap(mapInfo);
                continue;
            }

            const interleaved = new Float32Array(
                mapInfo.data.buffer,
                mapInfo.data.byteOffset,
                Math.floor(mapInfo.size / Float32Array.BYTES_PER_ELEMENT)
            );
            this._pcmRing.appendInterleaved(interleaved, Math.floor(interleaved.length / 2));
            buffer.unmap(mapInfo);
        }
    }

    _scheduleSpectrumProcessing() {
        if (this._processingSourceId || !this._pcmRing ||
            this._pcmRing.totalFrames === this._lastAnalyzedTotalFrames)
            return;

        this._processingSourceId = GLib.idle_add(GLib.PRIORITY_DEFAULT_IDLE, () => {
            this._processingSourceId = 0;
            try {
                this._processLatestAudioWindow();
            } catch (error) {
                log(`audio sample processing failed: ${error}`);
            }
            return GLib.SOURCE_REMOVE;
        });
    }

    _processLatestAudioWindow() {
        if (!this._configuration || !this._pcmRing ||
            !this._leftProcessorState || !this._rightProcessorState)
            return;

        const snapshotTotalFrames = this._pcmRing.totalFrames;
        this._pcmRing.copyLatest(
            this._leftSampleBuffer,
            this._rightSampleBuffer
        );

        const leftValues = processSpectrumFrame(
            this._leftSampleBuffer,
            this._leftProcessorState,
            this._configuration
        );
        const rightValues = processSpectrumFrame(
            this._rightSampleBuffer,
            this._rightProcessorState,
            this._configuration
        );
        const normalized = this._workingFrame;
        for (let i = 0; i < WEB_AUDIO_OUTPUT_BANDS_PER_CHANNEL; i++) {
            normalized[i] = leftValues[i];
            normalized[i + WEB_AUDIO_OUTPUT_BANDS_PER_CHANNEL] = rightValues[i];
        }

        this._emitFrame(normalized);
        this._lastAnalyzedTotalFrames = snapshotTotalFrames;
    }

    _emitFrame(frame) {
        const emittedFrame = cloneArray(frame);
        this._lastFrame = emittedFrame;
        this._onFrame?.(emittedFrame);
    }

    _scheduleRestart() {
        this._teardownPipeline();
        if (!this._shouldRun || this._restartSourceId)
            return;

        this._restartSourceId = GLib.timeout_add(
            GLib.PRIORITY_DEFAULT,
            WEB_AUDIO_RESTART_DELAY_MS,
            () => {
                this._restartSourceId = 0;
                if (this._shouldRun)
                    this._startPipeline();
                return GLib.SOURCE_REMOVE;
            }
        );
    }

    _cancelRestart() {
        if (!this._restartSourceId)
            return;

        GLib.source_remove(this._restartSourceId);
        this._restartSourceId = 0;
    }

    _teardownPipeline() {
        if (this._capturePollSourceId) {
            GLib.source_remove(this._capturePollSourceId);
            this._capturePollSourceId = 0;
        }

        if (this._analysisSourceId) {
            GLib.source_remove(this._analysisSourceId);
            this._analysisSourceId = 0;
        }

        if (this._processingSourceId) {
            GLib.source_remove(this._processingSourceId);
            this._processingSourceId = 0;
        }

        this._pcmRing?.reset();
        this._lastAnalyzedTotalFrames = 0;

        if (this._bus) {
            this._busSignalIds.forEach(signalId => {
                try {
                    this._bus.disconnect(signalId);
                } catch (_e) {
                }
            });
            this._busSignalIds = [];
            try {
                this._bus.remove_signal_watch();
            } catch (_e) {
            }
            this._bus = null;
        }

        if (this._pipeline) {
            const pipeline = this._pipeline;
            this._pipeline = null;
            this._appsink = null;
            try {
                pipeline.set_state(Gst.State.NULL);
                pipeline.get_state(Gst.SECOND);
            } catch (error) {
                log(`audio sample pipeline stop wait failed: ${error}`);
            }
        }
    }
}

class MediaRuntimeBridge {
    constructor({onMediaState, onAudioSamples}) {
        this._onMediaState = onMediaState;
        this._onAudioSamples = onAudioSamples;
        this._mediaMonitor = new SceneMediaMonitor(payload => this._onMediaState?.(payload));
        this._audioCapture = new WebAudioVisualizerCapture(samples => this._onAudioSamples?.(samples));
        this._transportConnected = false;
        this._started = false;
    }

    start() {
        if (this._started)
            return;

        this._started = true;
        this._mediaMonitor.start();
    }

    stop() {
        if (!this._started)
            return;

        this._started = false;
        this._transportConnected = false;
        this._audioCapture.destroy();
        this._mediaMonitor.destroy();
    }

    setTransportConnected(connected) {
        connected = Boolean(connected);
        if (this._transportConnected === connected)
            return;

        this._transportConnected = connected;
        if (connected) {
            this._mediaMonitor.refresh();
            this._onMediaState?.(this.currentMediaState);
            this._onAudioSamples?.(this.currentAudioSamples);
            this._audioCapture.start();
        } else {
            this._audioCapture.stop({emitSilence: false});
        }
    }

    get currentMediaState() {
        return this._mediaMonitor.currentPayload;
    }

    get currentAudioSamples() {
        return this._audioCapture.currentFrame;
    }
}

class OutputWindow {
    constructor(app, monitorInfo) {
        this._app = app;
        this.monitorIndex = monitorInfo.monitorIndex;
        this.consumerOutputId = monitorInfo.consumerOutputId;
        this.layoutOutputCount = monitorInfo.layoutOutputCount;
        this.backendOutputId = null;
        this._bufferGenerations = new Map();
        this._currentGeneration = null;
        this._lastFrameUsec = 0;
        this._lastDiagnosticLogUsec = 0;
        this._suppressedFrameDiagnostics = 0;
        this._onBindFailed = null;
        this.paintable = DisplayConsumer.BufferPaintable.new();

        this.geometry = {
            x: monitorInfo.x,
            y: monitorInfo.y,
            width: monitorInfo.logicalWidth,
            height: monitorInfo.logicalHeight,
        };
        this.scale = monitorInfo.scale;
        this.logicalWidth = monitorInfo.logicalWidth;
        this.logicalHeight = monitorInfo.logicalHeight;
        this.physicalWidth = monitorInfo.physicalWidth;
        this.physicalHeight = monitorInfo.physicalHeight;
        this.refreshRate = monitorInfo.refreshRate;
        this.displayKey = monitorInfo.displayKey;
        this.displayName = monitorInfo.displayName;
        this.picture = null;
        this.window = null;
        this._windowRealizeId = 0;
    }

    ensureWindow() {
        if (this.window)
            return;

        this.picture = new Gtk.Picture({
            can_shrink: true,
            content_fit: Gtk.ContentFit.FILL,
            width_request: this.logicalWidth,
            height_request: this.logicalHeight,
        });
        this.picture.set_paintable(this.paintable);
        this.picture.set_can_target(false);
        this.picture.set_can_focus(false);

        this.window = new Gtk.ApplicationWindow({
            application: this._app,
            decorated: false,
            resizable: false,
            default_width: this.logicalWidth,
            default_height: this.logicalHeight,
        });
        this.window.set_child(this.picture);
        this.window.set_can_target(false);
        this.window.set_can_focus(false);

        /*
         * Match the legacy renderer-window contract: the GTK helper
         * window is only a compositor-side source for Clutter.Clone, while the
         * visible wallpaper actor lives inside GNOME Shell's BackgroundActor.
         *
         * The helper must stay minimized. If it remains a visible normal
         * toplevel, Mutter treats it like a real desktop window and constrains it
         * to the work area, which removes the top-panel strip from the source
         * actor. Keeping the full-monitor helper minimized lets WindowManager
         * pin it at the monitor origin without letting the implementation window
         * cover panels, docks, overview, or user applications.
         */
        const state = {
            keepAtBottom: true,
            keepMinimized: true,
            keepPosition: true,
            position: [this.geometry.x, this.geometry.y],
        };
        this.window.set_title(`${TITLE_PREFIX}${JSON.stringify(state)}|${this.monitorIndex}`);
        this._windowRealizeId = this.window.connect('realize', () => this._onRealize());
        this.window.set_size_request(this.logicalWidth, this.logicalHeight);
        this.window.set_focus_on_map?.(false);

        /*
         * Bind the window to its target monitor before the first map
         * (waywallen renderer.js does the same). Without this, Mutter places
         * the brand-new toplevel on whatever monitor is "active" (pointer /
         * focus), and the Shell-side WindowManager only pins the position
         * after map. During that gap MetaWindow.get_monitor() reports the
         * wrong monitor, which is exactly when LiveWallpaper picks its clone
         * source; a helper for monitor 0 could then be cloned onto monitor 1.
         */
        const gdkMonitor = this._findGdkMonitor();
        if (gdkMonitor)
            this.window.fullscreen_on_monitor(gdkMonitor);
        this.window.present();
    }

    setBindFailedReporter(callback) {
        this._onBindFailed = callback;
    }

    _reportBindFailed(payload, message, reason = 1, bufferIndex = null) {
        if (!payload || !this._onBindFailed)
            return;

        this._onBindFailed({
            outputId: Number(payload.outputId ?? this.backendOutputId ?? 0),
            generation: Number(payload.generation ?? 0),
            fourcc: Number(payload.fourcc ?? 0),
            modifier: payload.modifier ?? '0',
            bufferIndex: bufferIndex ?? payload.bufferIndex ?? payload.buffer ?? null,
            reason,
            message: String(message ?? 'DMA-BUF import failed'),
        });
    }

    outputPayload() {
        return {
            [J.REGISTER_OUTPUT.consumerOutputId]: this.consumerOutputId,
            [J.REGISTER_OUTPUT.monitorIndex]: this.monitorIndex,
            [J.REGISTER_OUTPUT.layoutOutputCount]: this.layoutOutputCount,
            [J.REGISTER_OUTPUT.x]: this.geometry.x,
            [J.REGISTER_OUTPUT.y]: this.geometry.y,
            [J.REGISTER_OUTPUT.width]: this.logicalWidth,
            [J.REGISTER_OUTPUT.height]: this.logicalHeight,
            [J.REGISTER_OUTPUT.scale]: this.scale,
            [J.REGISTER_OUTPUT.physicalWidth]: this.physicalWidth,
            [J.REGISTER_OUTPUT.physicalHeight]: this.physicalHeight,
            [J.REGISTER_OUTPUT.transform]: 'normal',
            [J.REGISTER_OUTPUT.refreshRateMhz]: this.refreshRate,
            [J.REGISTER_OUTPUT.desktop]: 'gnome-shell-helper',
            [J.REGISTER_OUTPUT.displayKey]: this.displayKey,
            [J.REGISTER_OUTPUT.displayName]: this.displayName,
        };
    }

    bindBuffers(payload, bindJson, fdList) {
        this.ensureWindow();
        this.unbindGeneration(payload.generation, {logMissing: false});

        const generation = {
            payload,
            bindJson,
            configured: false,
            configGeneration: 0,
        };
        this._lastFrameUsec = 0;
        this._lastDiagnosticLogUsec = 0;
        this._suppressedFrameDiagnostics = 0;

        try {
            this.paintable.bind_json(bindJson, fdList);
        } catch (error) {
            log(`output ${this.backendOutputId}: display paintable BIND_BUFFERS failed: ${error}`);
            this._reportBindFailed(payload, error, 1);
            return;
        }

        this._bufferGenerations.set(Number(payload.generation), generation);
        const producerRenderNode = payload.producerRenderNode ?? '(missing)';
        const producerVendor = payload.producerVendor ?? '(missing)';
        const producerPciAddress = payload.producerPciAddress ?? '(missing)';
        const consumerRenderNode = payload.consumerRenderNode ?? '(unknown)';
        const producerDrm = `${payload.producerDrmRenderMajor ?? 0}:${payload.producerDrmRenderMinor ?? 0}`;
        const consumerDrm = `${payload.consumerDrmRenderMajor ?? 0}:${payload.consumerDrmRenderMinor ?? 0}`;
        log(`output ${this.backendOutputId}: bound generation=${payload.generation} ` +
            `buffer=${payload.width}x${payload.height} ` +
            `logical=${payload.logicalWidth ?? this.logicalWidth}x${payload.logicalHeight ?? this.logicalHeight} ` +
            `helper-window=${this.logicalWidth}x${this.logicalHeight} scale=${payload.scale ?? this.scale} ` +
            `memory=${payload.memoryType ?? '(missing)'} fourcc=${payload.fourcc ?? '(missing)'} ` +
            `path=${payload.negotiatedPath ?? '(missing)'} presentation=${payload.presentationPath ?? '(missing)'} ` +
            `memory-source=${payload.memorySource ?? '(missing)'} ` +
            `memory-hint=${payload.memoryHint ?? '(missing)'} ` +
            `producerRenderNode=${producerRenderNode} producer-drm=${producerDrm} ` +
            `consumerRenderNode=${consumerRenderNode} consumer-drm=${consumerDrm} ` +
            `producerVendor=${producerVendor} producerPciAddress=${producerPciAddress} ` +
            `modifier=${payload.modifier ?? '(missing)'} planesPerBuffer=${payload.planesPerBuffer ?? '(missing)'} ` +
            `premultiplied=${!!payload.premultiplied} ` +
            `buffers=${formatBufferSummary(payload)}`);
    }

    setConfig(payload) {
        this.ensureWindow();
        const source = payload.source ?? {};
        const destination = payload.destination ?? {};
        const clear = Array.isArray(payload.clearColor) ? payload.clearColor : [0, 0, 0, 1];
        const scale = Number(this.scale) > 0 ? Number(this.scale) : 1;
        const generationId = Number(payload.generation ?? 0);
        let generation = generationId > 0
            ? this._bufferGenerations.get(generationId)
            : this._latestPendingConfigGeneration();
        if (!generation && generationId <= 0)
            generation = this._latestLiveGeneration();
        if (!generation) {
            log(`output ${this.backendOutputId}: SET_CONFIG references unknown ` +
                `generation=${generationId || '(latest)'} ` +
                `known-generations=${formatGenerationKeys(this._bufferGenerations)}`);
            return;
        }

        try {
            this.paintable.set_config(
                Number(source.x ?? 0),
                Number(source.y ?? 0),
                Number(source.width ?? source.w ?? this.physicalWidth),
                Number(source.height ?? source.h ?? this.physicalHeight),
                Number(destination.x ?? 0) / scale,
                Number(destination.y ?? 0) / scale,
                Number(destination.width ?? destination.w ?? this.physicalWidth) / scale,
                Number(destination.height ?? destination.h ?? this.physicalHeight) / scale,
                transformCode(payload.transform),
                Number(clear[0] ?? 0),
                Number(clear[1] ?? 0),
                Number(clear[2] ?? 0),
                Number(clear[3] ?? 1)
            );
        } catch (error) {
            log(`output ${this.backendOutputId}: display paintable SET_CONFIG failed: ${error}`);
            return;
        }
        generation.configured = true;
        generation.configGeneration = Number(payload.configGeneration ?? generation.configGeneration ?? 0);
    }

    _latestPendingConfigGeneration() {
        const generations = [...this._bufferGenerations.entries()]
            .sort(([left], [right]) => Number(right) - Number(left));
        for (const [, generation] of generations) {
            if (!generation.configured)
                return generation;
        }
        return null;
    }

    _latestLiveGeneration() {
        const generations = [...this._bufferGenerations.entries()]
            .sort(([left], [right]) => Number(right) - Number(left));
        return generations.length > 0 ? generations[0][1] : null;
    }

    _signalReleaseSyncobj(generation, releaseFd, context) {
        const renderNode = generation?.payload?.producerRenderNode ?? '';
        const ok = callDisplayConsumerFunction(
            'dmabuf_texture_signal_release_syncobj',
            renderNode,
            releaseFd
        );
        if (ok !== true) {
            log(`output ${this.backendOutputId}: release syncobj signal failed ` +
                `context=${context} renderNode=${renderNode || '(missing)'}`);
        }
    }

    showFrame(frame, fdList) {
        let acquireFd = takeFrameFd(fdList, 0, 'acquire sync_file');
        let releaseFd = takeFrameFd(fdList, 1, 'release syncobj');
        const frameStartUsec = GLib.get_monotonic_time();
        const previousFrameUsec = this._lastFrameUsec;
        const frameIntervalUsec = previousFrameUsec > 0 ? frameStartUsec - previousFrameUsec : 0;
        const generation = this._bufferGenerations.get(Number(frame.generation));
        if (!generation) {
            log(`output ${this.backendOutputId}: FRAME_READY has no texture ` +
                `generation=${frame.generation} buffer=${frame.bufferIndex} ` +
                `known-generations=${formatGenerationKeys(this._bufferGenerations)}`);
            closeDisplayConsumerFd(acquireFd);
            closeDisplayConsumerFd(releaseFd);
            return;
        }
        if (acquireFd < 0 || releaseFd < 0) {
            closeDisplayConsumerFd(acquireFd);
            if (releaseFd >= 0)
                this._signalReleaseSyncobj(generation, releaseFd, 'missing-frame-fd');
            closeDisplayConsumerFd(releaseFd);
            log(`output ${this.backendOutputId}: FRAME_READY rejected because explicit sync fds are incomplete`);
            return;
        }
        if (!generation.configured) {
            this._signalReleaseSyncobj(generation, releaseFd, 'pending-config');
            closeDisplayConsumerFd(acquireFd);
            closeDisplayConsumerFd(releaseFd);
            log(`output ${this.backendOutputId}: FRAME_READY rejected before SET_CONFIG ` +
                `generation=${frame.generation} buffer=${frame.bufferIndex}`);
            return;
        }

        let textureRefreshUsec = 0;
        try {
            const textureStartUsec = GLib.get_monotonic_time();
            if (generation.payload?.presentationPath !== 'shadow-copy')
                throw new Error(`unsupported presentation path: ${generation.payload?.presentationPath ?? '(missing)'}`);
            if (typeof this.paintable.show_frame_with_sync !== 'function')
                throw new Error('display paintable lacks show_frame_with_sync for shadow-copy');
            /*
             * Match waywallen's DMABUF_RELAY: the display module imports the
             * acquire sync_file as a temporary Vulkan semaphore, waits in the
             * blit submit, and signals the release syncobj after the shadow copy
             * fence completes. Ownership of both fds transfers here.
             */
            const displayConsumerAcquireFd = acquireFd;
            const displayConsumerReleaseFd = releaseFd;
            acquireFd = -1;
            releaseFd = -1;
            this.paintable.show_frame_with_sync(
                frame.generation,
                frame.bufferIndex,
                displayConsumerAcquireFd,
                displayConsumerReleaseFd
            );
            textureRefreshUsec = GLib.get_monotonic_time() - textureStartUsec;
            this._currentGeneration = Number(frame.generation);
        } catch (error) {
            log(`output ${this.backendOutputId}: display paintable refresh failed ` +
                `generation=${frame.generation} buffer=${frame.bufferIndex}: ${error}`);
            if (releaseFd >= 0)
                this._signalReleaseSyncobj(generation, releaseFd, 'paintable-refresh-failed');
            closeDisplayConsumerFd(releaseFd);
            this._reportBindFailed(generation.payload, error, 2, frame.bufferIndex);
            this.unbindGeneration(frame.generation, {logMissing: false, logSuccess: false});
            return;
        }
        closeDisplayConsumerFd(acquireFd);
        closeDisplayConsumerFd(releaseFd);
        this._lastFrameUsec = frameStartUsec;

        this._maybeLogFrameTiming(frame, {
            frameStartUsec,
            frameIntervalUsec,
            textureRefreshUsec,
        });
    }

    _maybeLogFrameTiming(frame, timing) {
        /*
         * Display jitter can come from two different places: FRAME_READY messages
         * may arrive late from the producer, or the GTK helper may receive them on
         * time but stall while re-importing the DMA-BUF and submitting a repaint.
         * Keep this diagnostic threshold-based and rate-limited so normal 60 FPS
         * playback does not flood GNOME Shell's journal.
         */
        const intervalSlow = timing.frameIntervalUsec > FRAME_INTERVAL_WARN_USEC;
        const refreshSlow = timing.textureRefreshUsec > TEXTURE_REFRESH_WARN_USEC;
        if (!intervalSlow && !refreshSlow)
            return;

        const nowUsec = GLib.get_monotonic_time();
        if (this._lastDiagnosticLogUsec > 0 &&
            nowUsec - this._lastDiagnosticLogUsec < FRAME_DIAGNOSTIC_LOG_INTERVAL_USEC) {
            this._suppressedFrameDiagnostics++;
            return;
        }

        const targetTimeUsec = Number(frame.targetTimeUsec ?? 0);
        const latenessUsec = targetTimeUsec > 0 ? timing.frameStartUsec - targetTimeUsec : 0;
        const suppressed = this._suppressedFrameDiagnostics;
        this._suppressedFrameDiagnostics = 0;
        this._lastDiagnosticLogUsec = nowUsec;

        log(`output ${this.backendOutputId}: slow display frame ` +
            `generation=${frame.generation} buffer=${frame.bufferIndex} sequence=${frame.sequence} ` +
            `interval=${formatUsec(timing.frameIntervalUsec)} ` +
            `textureRefresh=${formatUsec(timing.textureRefreshUsec)} ` +
            `targetLateness=${formatUsec(latenessUsec)} ` +
            `suppressed=${suppressed}`);
    }

    unbindGeneration(generationId, options = {}) {
        const logMissing = options.logMissing ?? true;
        const logSuccess = options.logSuccess ?? true;
        const generation = this._bufferGenerations.get(Number(generationId));
        if (!generation) {
            if (logMissing) {
                log(`output ${this.backendOutputId}: UNBIND ignored missing generation=${generationId} ` +
                    `current=${this._currentGeneration ?? '(none)'} ` +
                    `known-generations=${formatGenerationKeys(this._bufferGenerations)}`);
            }
            return;
        }

        const wasCurrent = this._currentGeneration === Number(generationId);
        try {
            this.paintable.unbind(generationId);
        } catch (error) {
            log(`output ${this.backendOutputId}: display paintable UNBIND failed ` +
                `generation=${generationId}: ${error}`);
        }
        if (this._currentGeneration === Number(generationId))
            this._currentGeneration = null;
        this._bufferGenerations.delete(Number(generationId));
        if (logSuccess) {
            log(`output ${this.backendOutputId}: unbound generation=${generationId} ` +
                `wasCurrent=${wasCurrent} current=${this._currentGeneration ?? '(none)'} ` +
                `remaining=${formatGenerationKeys(this._bufferGenerations)}`);
        }
    }

    clear() {
        for (const generationId of [...this._bufferGenerations.keys()])
            this.unbindGeneration(generationId);
        this.paintable.clear();
        this.backendOutputId = null;
        this._currentGeneration = null;
        this._lastFrameUsec = 0;
        this._lastDiagnosticLogUsec = 0;
        this._suppressedFrameDiagnostics = 0;
        /*
         * Socket teardown is a visibility boundary, not just a buffer boundary.
         * If the producer disappears and the helper window stays mapped, GNOME
         * Shell can keep cloning an empty GTK surface over the native desktop
         * background. Destroy the implementation window here so the Shell-side
         * clone source is removed and the original wallpaper becomes visible
         * while the helper waits for a future producer reconnect.
         */
        this._destroyWindow('connection-clear');
    }

    _deactivatePresentation(reason) {
        this.paintable.clear();
        this._currentGeneration = null;
        this._lastFrameUsec = 0;
        this._lastDiagnosticLogUsec = 0;
        this._suppressedFrameDiagnostics = 0;
        this._destroyWindow(reason);
    }

    deactivate() {
        for (const generationId of [...this._bufferGenerations.keys()])
            this.unbindGeneration(generationId, {logMissing: false});
        this._deactivatePresentation('producer-deactivate');
        log(`output ${this.backendOutputId ?? this.consumerOutputId}: deactivated`);
    }

    _destroyWindow(reason) {
        if (!this.window) {
            this.picture = null;
            this._windowRealizeId = 0;
            return;
        }

        const window = this.window;
        const picture = this.picture;
        this.window = null;
        this.picture = null;

        /*
         * The helper window is a compositor resource as much as a GTK widget:
         * GNOME Shell clones its MetaWindowActor directly. On socket teardown we
         * must sever every GTK-side reference before destroy(), otherwise a late
         * signal callback or stale child can keep the source actor alive long
         * enough for Shell to keep repainting an implementation surface after
         * the producer has gone away.
         */
        if (this._windowRealizeId) {
            try {
                window.disconnect(this._windowRealizeId);
            } catch (_e) {
            }
            this._windowRealizeId = 0;
        }

        try {
            if (picture && window.get_child?.() === picture)
                window.set_child(null);
        } catch (_e) {
        }

        try {
            window.destroy();
            log(`output ${this.backendOutputId ?? this.consumerOutputId}: helper window destroyed reason=${reason}`);
        } catch (error) {
            log(`output ${this.backendOutputId ?? this.consumerOutputId}: helper window destroy failed ` +
                `reason=${reason}: ${error}`);
        }
    }

    _onRealize() {
        try {
            this.window.set_can_target(false);
            this.window.set_can_focus(false);
            const surface = this.window.get_surface();
            surface?.set_input_region?.(new cairo.Region());
        } catch (error) {
            log(`output ${this.monitorIndex}: failed to make helper window input-transparent: ${error}`);
        }
    }

    /*
     * The Gdk monitor list order is not guaranteed to match the Shell monitor
     * index this output was registered with, so match by monitor origin first
     * and only fall back to positional indexing.
     */
    _findGdkMonitor() {
        const display = this.window?.get_display?.() ?? Gdk.Display.get_default();
        const monitors = display?.get_monitors?.();
        const nItems = monitors?.get_n_items?.() ?? 0;
        for (let i = 0; i < nItems; i++) {
            const monitor = monitors.get_item(i);
            if (!monitor)
                continue;
            const geometry = monitor.get_geometry();
            if (geometry.x === this.geometry.x && geometry.y === this.geometry.y)
                return monitor;
        }
        if (this.monitorIndex >= 0 && this.monitorIndex < nItems)
            return monitors.get_item(this.monitorIndex);
        return null;
    }
}

class DisplayConnection {
    constructor(app, opts, outputs) {
        this._app = app;
        this._opts = opts;
        this._outputs = outputs;
        this._outputsByConsumerId = new Map(outputs.map(output => [output.consumerOutputId, output]));
        this._outputsByBackendId = new Map();
        this._socketClient = null;
        this._connection = null;
        this._output = null;
        this._receiver = null;
        this._receiverSignalIds = [];
        this._writeQueue = [];
        this._writePending = false;
        this._lastSocketDiagnosticLogUsec = 0;
        this._suppressedSocketDiagnostics = 0;
        this._lastConnectFailureLogUsec = 0;
        this._suppressedConnectFailures = 0;
        this._coalescedPointerMotionCount = 0;
        this._cancellable = new Gio.Cancellable();
        this._reconnectSourceId = 0;
        this._applicationHoldAcquired = false;
        this._lastWindowState = null;
        this._lastMediaState = defaultMediaStatePayload();
        this._lastAudioSamples = buildSilentWebAudioFrame();
        this._negotiatedVersion = PROTOCOL_VERSION;
        this._mediaRuntime = new MediaRuntimeBridge({
            onMediaState: payload => this.sendMediaState(payload),
            onAudioSamples: samples => this.sendAudioSamples(samples),
        });
        for (const output of this._outputs)
            output.setBindFailedReporter(payload => this._sendBindFailed(payload));
    }

    start() {
        this._acquireApplicationHold();
        this._mediaRuntime.start();
        this._connect();
    }

    stop() {
        this._clearReconnect();
        this._mediaRuntime.stop();
        try {
            this._cancellable.cancel();
        } catch (_e) {
        }
        this._closeConnection(false);
        this._releaseApplicationHold();
    }

    _acquireApplicationHold() {
        if (this._applicationHoldAcquired)
            return;

        /*
         * Unconfigured outputs deliberately do not create GTK helper windows, so
         * the helper may need to keep only the display socket and stdin bridge
         * alive. GtkApplication exits automatically when the last window closes;
         * hold the application while DisplayConnection owns those background
         * transports, and release it during shutdown.
         */
        this._app.hold();
        this._applicationHoldAcquired = true;
    }

    _releaseApplicationHold() {
        if (!this._applicationHoldAcquired)
            return;

        this._applicationHoldAcquired = false;
        this._app.release();
    }

    handleControlMessage(message) {
        if (!message || typeof message !== 'object')
            return;

        if (message.type === 'pointer') {
            this._sendPointerEvent(message.event);
        } else if (message.type === 'window-state') {
            this._lastWindowState = message.payload ?? {};
            this._queueFrame(encodeJsonFrame(REQ_WINDOW_STATE, this._lastWindowState));
        }
    }

    _connect() {
        this._clearReconnect();
        this._socketClient = new Gio.SocketClient();
        this._socketClient.connect_async(
            Gio.UnixSocketAddress.new(this._opts.socketPath),
            this._cancellable,
            (client, result) => {
                try {
                    this._connection = client.connect_finish(result);
                    this._output = this._connection.get_output_stream();
                    this._startReceiver();
                    this._sendHello();
                    this._sendConsumerCaps();
                    for (const output of this._outputs)
                        this._sendRegisterOutput(output);
                    if (this._lastWindowState)
                        this._queueFrame(encodeJsonFrame(REQ_WINDOW_STATE, this._lastWindowState));
                    this._mediaRuntime.setTransportConnected(true);
                    this._logConnected();
                } catch (error) {
                    if (error.matches?.(Gio.IOErrorEnum, Gio.IOErrorEnum.CANCELLED))
                        return;

                    this._logConnectFailure(error);
                    this._scheduleReconnect();
                }
            }
        );
    }

    _logConnected() {
        const suppressed = this._suppressedConnectFailures;
        this._lastConnectFailureLogUsec = 0;
        this._suppressedConnectFailures = 0;
        log(`connected to ${this._opts.socketPath}` +
            (suppressed > 0 ? ` suppressed-connect-failures=${suppressed}` : ''));
    }

    _logConnectFailure(error) {
        const nowUsec = GLib.get_monotonic_time();
        if (this._lastConnectFailureLogUsec > 0 &&
            nowUsec - this._lastConnectFailureLogUsec < CONNECT_FAILURE_LOG_INTERVAL_USEC) {
            this._suppressedConnectFailures++;
            return;
        }

        const suppressed = this._suppressedConnectFailures;
        this._suppressedConnectFailures = 0;
        this._lastConnectFailureLogUsec = nowUsec;
        log(`connect failed at ${this._opts.socketPath}: ${error}` +
            (suppressed > 0 ? ` suppressed=${suppressed}` : ''));
    }

    _startReceiver() {
        this._receiver = DisplayConsumer.Receiver.new(this._connection);
        this._receiverSignalIds = [
            this._receiver.connect('frame', (_receiver, opcode, body, fdList) => {
                this._handleFrame(opcode, bytesFromGBytes(body), fdList);
            }),
            this._receiver.connect('protocol-error', (_receiver, code, message) => {
                log(`protocol error ${code}: ${message}`);
                this._closeConnection(true);
            }),
            this._receiver.connect('closed', () => {
                log('socket closed by producer');
                this._closeConnection(true);
            }),
        ];

        if (!this._receiver.start())
            throw new Error('display consumer receiver failed to start');
    }

    _closeConnection(reconnect) {
        this._mediaRuntime.setTransportConnected(false);
        if (this._receiver) {
            for (const id of this._receiverSignalIds) {
                try {
                    this._receiver.disconnect(id);
                } catch (_e) {
                }
            }
            this._receiverSignalIds = [];
            try {
                this._receiver.stop();
            } catch (_e) {
            }
            this._receiver = null;
        }

        for (const output of this._outputs)
            output.clear();
        this._outputsByBackendId.clear();

        try {
            this._connection?.close(null);
        } catch (_e) {
        }
        this._connection = null;
        this._output = null;
        this._socketClient = null;
        this._writeQueue = [];
        this._writePending = false;
        this._lastSocketDiagnosticLogUsec = 0;
        this._suppressedSocketDiagnostics = 0;
        this._coalescedPointerMotionCount = 0;

        if (reconnect)
            this._scheduleReconnect();
    }

    sendMediaState(payload) {
        this._lastMediaState = normalizeMediaStatePayload(payload);
        let frame = null;
        try {
            frame = encodeJsonFrame(REQ_MEDIA_STATE, this._lastMediaState);
        } catch (error) {
            log(`media state encode failed: ${error}`);
            return false;
        }

        return this._queueFrame(frame);
    }

    sendAudioSamples(samples) {
        this._lastAudioSamples = normalizeAudioSamplesPayload(samples);
        const count = this._lastAudioSamples.length;
        const timeUsec = GLib.get_monotonic_time();
        let frame = null;
        try {
            const body = encodeAudioSamplesBin(count, timeUsec, this._lastAudioSamples);
            frame = encodeFrame(REQ_AUDIO_SAMPLES_BIN, body);
        } catch (error) {
            log(`audio samples encode failed: ${error}`);
            return false;
        }

        return this._queueFrame(frame);
    }

    _sendBindFailed(payload) {
        const framePayload = {
            [J.BIND_FAILED.outputId]: Number(payload.outputId ?? 0),
            [J.BIND_FAILED.generation]: Number(payload.generation ?? 0),
            [J.BIND_FAILED.fourcc]: Number(payload.fourcc ?? 0),
            [J.BIND_FAILED.modifier]: String(payload.modifier ?? '0'),
            bufferIndex: payload.bufferIndex === null || payload.bufferIndex === undefined
                ? null
                : Number(payload.bufferIndex),
            [J.BIND_FAILED.reason]: Number(payload.reason ?? 1),
            [J.BIND_FAILED.message]: String(payload.message ?? 'DMA-BUF import failed'),
        };
        if (!framePayload[J.BIND_FAILED.fourcc]) {
            log(`skip BIND_FAILED without fourcc: ${JSON.stringify(framePayload)}`);
            return false;
        }

        log(`BIND_FAILED output=${framePayload[J.BIND_FAILED.outputId]} generation=${framePayload[J.BIND_FAILED.generation]} ` +
            `fourcc=0x${framePayload[J.BIND_FAILED.fourcc].toString(16).padStart(8, '0')} ` +
            `modifier=${framePayload[J.BIND_FAILED.modifier]} reason=${framePayload[J.BIND_FAILED.reason]} ` +
            `[${bindFailedReasonName(framePayload[J.BIND_FAILED.reason])}] ` +
            `message=${framePayload[J.BIND_FAILED.message]}`);
        return this._queueFrame(encodeJsonFrame(REQ_BIND_FAILED, framePayload));
    }

    _scheduleReconnect() {
        if (this._reconnectSourceId)
            return;

        this._reconnectSourceId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 1000, () => {
            this._reconnectSourceId = 0;
            this._connect();
            return GLib.SOURCE_REMOVE;
        });
    }

    _clearReconnect() {
        if (!this._reconnectSourceId)
            return;

        GLib.source_remove(this._reconnectSourceId);
        this._reconnectSourceId = 0;
    }

    _sendHello() {
        this._queueFrame(encodeJsonFrame(REQ_HELLO, {
            [J.HELLO.protocol]: PROTOCOL_NAME,
            [J.HELLO.version]: PROTOCOL_VERSION,
            [J.HELLO.clientName]: 'gnome-display-helper',
            [J.HELLO.role]: 'consumer',
            [J.HELLO.features]: [
                'dmabuf-gdk-texture-v1',
                'dmabuf-caps-v3',
                'explicit-sync-fd-v1',
                'dmabuf-bind-failed-v1',
                'dmabuf-unbind-done-v1',
                'unbind-v2',
                'dmabuf-shadow-copy-v1',
                'pointer-events-v1',
                'media-state-v1',
                'audio-samples-v1',
            ],
        }));
    }

    _sendConsumerCaps() {
        this._queueFrame(encodeJsonFrame(REQ_CONSUMER_CAPS, {
            [J.CONSUMER_CAPS.dmabufCaps]: buildDmaBufCaps(),
        }));
    }

    _sendRegisterOutput(output) {
        log(`register monitor=${output.monitorIndex} consumerOutputId=${output.consumerOutputId} ` +
            `logical=${output.logicalWidth}x${output.logicalHeight} ` +
            `physical=${output.physicalWidth}x${output.physicalHeight} scale=${output.scale} ` +
            `refresh=${output.refreshRate} name=${output.displayName || '(unnamed)'}`);
        this._queueFrame(encodeJsonFrame(REQ_REGISTER_OUTPUT, output.outputPayload()));
    }

    _sendPointerEvent(event) {
        if (!event)
            return false;

        const monitorIndex = Number(event.monitorIndex);
        const output = this._outputs[monitorIndex];
        const outputId = Number(output?.backendOutputId);
        if (!Number.isFinite(outputId))
            return false;

        const type = String(event.type ?? '');
        const scale = Number(output?.scale ?? 1);
        /*
         * Shell sends pointer coordinates in the logical BackgroundActor
         * coordinate space. The producer renders scene DMA-BUFs in physical
         * pixels and passes the same scale to SceneWallpaper::initVulkan(), so
         * pointer coordinates are converted to the physical render target here
         * before they are normalized by the scene producer.
         */
        const x = Number(event.x) * (Number.isFinite(scale) && scale > 0 ? scale : 1);
        const y = Number(event.y) * (Number.isFinite(scale) && scale > 0 ? scale : 1);
        const timeUsec = Number(event.timeUsec ?? GLib.get_monotonic_time());
        let frame = null;

        if (type === 'mousemove') {
            frame = encodeFrame(REQ_POINTER_MOTION,
                encodePointerMotion(outputId, x, y, timeUsec));
        } else if (type === 'mousedown' || type === 'mouseup') {
            frame = encodeFrame(REQ_POINTER_BUTTON,
                encodePointerButton(outputId,
                    x,
                    y,
                    Number(event.button ?? 0),
                    type === 'mousedown' ? POINTER_BUTTON_PRESSED : POINTER_BUTTON_RELEASED,
                    timeUsec));
        } else if (type === 'wheel') {
            frame = encodeFrame(REQ_POINTER_AXIS,
                encodePointerAxis(outputId,
                    x,
                    y,
                    Number(event.deltaX ?? 0),
                    Number(event.deltaY ?? 0),
                    POINTER_AXIS_WHEEL,
                    timeUsec));
        }

        if (!frame)
            return false;

        const queueKey = type === 'mousemove' ? `pointer-motion:${outputId}` : null;
        const pointerBarrier = type === 'mousedown' || type === 'mouseup' || type === 'wheel';
        return this._queueFrame(frame, {
            queueKey,
            kind: type === 'mousemove' ? 'pointer-motion' : 'pointer-discrete',
            pointerBarrier,
        });
    }

    _queueFrame(bytes, options = {}) {
        if (!this._output)
            return false;

        const queueKey = options.queueKey ?? null;
        const nowUsec = GLib.get_monotonic_time();

        /*
         * Pointer motion is a latest-state signal. When the producer socket is
         * momentarily busy with FRAME_READY traffic or audio samples, replaying
         * every stale motion packet makes parallax visibly lag behind fast
         * cursor movement. Keep only the newest motion per output, but never
         * coalesce across a discrete pointer packet: button and wheel frames
         * carry their own coordinates and define an ordering boundary that must
         * stay intact for click/scroll semantics.
         */
        if (queueKey) {
            for (let i = this._writeQueue.length - 1; i >= 0; i--) {
                const item = this._writeQueue[i];
                if (item?.pointerBarrier)
                    break;
                if (item?.queueKey !== queueKey)
                    continue;

                this._writeQueue[i] = {
                    bytes,
                    queueKey,
                    kind: options.kind ?? item.kind,
                    pointerBarrier: Boolean(options.pointerBarrier),
                    queuedAtUsec: nowUsec,
                    replacedAtUsec: nowUsec,
                };
                this._coalescedPointerMotionCount++;
                this._flushWriteQueue();
                return true;
            }
        }

        this._writeQueue.push({
            bytes,
            queueKey,
            kind: options.kind ?? 'frame',
            pointerBarrier: Boolean(options.pointerBarrier),
            queuedAtUsec: nowUsec,
            replacedAtUsec: 0,
        });
        this._flushWriteQueue();
        return true;
    }

    _flushWriteQueue() {
        if (!this._output || this._writePending || this._writeQueue.length === 0)
            return;

        const item = this._writeQueue[0];
        this._writePending = true;
        const writeStartUsec = GLib.get_monotonic_time();
        this._output.write_all_async(item.bytes, GLib.PRIORITY_DEFAULT, this._cancellable, (stream, result) => {
            if (stream !== this._output)
                return;

            try {
                stream.write_all_finish(result);
                this._writeQueue.shift();
            } catch (error) {
                log(`socket write failed: ${error}`);
                this._closeConnection(true);
                return;
            }

            const nowUsec = GLib.get_monotonic_time();
            this._maybeLogSocketWriteTiming(item, {
                writeUsec: nowUsec - writeStartUsec,
                queuedUsec: nowUsec - (item.queuedAtUsec ?? writeStartUsec),
                remaining: this._writeQueue.length,
            });

            this._writePending = false;
            this._flushWriteQueue();
        });
    }

    _maybeLogSocketWriteTiming(item, timing) {
        if (timing.queuedUsec <= SOCKET_WRITE_WARN_USEC)
            return;

        const nowUsec = GLib.get_monotonic_time();
        if (this._lastSocketDiagnosticLogUsec > 0 &&
            nowUsec - this._lastSocketDiagnosticLogUsec < SOCKET_DIAGNOSTIC_LOG_INTERVAL_USEC) {
            this._suppressedSocketDiagnostics++;
            return;
        }

        const suppressed = this._suppressedSocketDiagnostics;
        const coalesced = this._coalescedPointerMotionCount;
        this._suppressedSocketDiagnostics = 0;
        this._coalescedPointerMotionCount = 0;
        this._lastSocketDiagnosticLogUsec = nowUsec;

        log(`socket write slow kind=${item.kind ?? 'frame'} ` +
            `queued=${formatUsec(timing.queuedUsec)} write=${formatUsec(timing.writeUsec)} ` +
            `remaining=${timing.remaining} coalescedPointerMotion=${coalesced} ` +
            `suppressed=${suppressed}`);
    }

    _handleFrame(opcode, body, fdList) {
        try {
            switch (opcode) {
            case EVT_WELCOME: {
                const welcome = decodeJsonPayload(body);
                this._negotiatedVersion = Number(
                    welcome.negotiatedVersion ?? PROTOCOL_VERSION
                );
                log(`welcome negotiatedVersion=${this._negotiatedVersion} ` +
                    `${JSON.stringify(welcome)}`);
                break;
            }
            case EVT_OUTPUT_ACCEPTED:
                this._handleOutputAccepted(decodeJsonPayload(body));
                break;
            case EVT_BIND_BUFFERS:
                this._handleBindBuffers(decodeJsonTextPayload(body), fdList);
                break;
            case EVT_SET_CONFIG:
                this._handleSetConfig(decodeJsonPayload(body));
                break;
            case EVT_FRAME_READY:
                this._handleFrameReady(decodeFrameReadyBody(body), fdList);
                break;
            case EVT_UNBIND:
                this._handleUnbind(decodeUnbindBody(body));
                break;
            case EVT_ERROR: {
                const errorPayload = decodeJsonPayload(body);
                const code = Number(errorPayload[J.EVT_ERROR.code] ?? 0);
                const fatal = errorPayload[J.EVT_ERROR.fatal] === true;
                const message = String(errorPayload[J.EVT_ERROR.message] ?? '');
                log(`producer error code=${code} name=${errorName(code)} ` +
                    `domain=${errorDomain(code)} fatal=${fatal} message=${message}`);
                if (fatal)
                    this._closeConnection(true);
                break;
            }
            default:
                break;
            }
        } catch (error) {
            log(`failed to handle opcode=${opcode}: ${error}`);
        }
    }

    _handleOutputAccepted(payload) {
        const consumerOutputId = Number(payload[J.OUTPUT_ACCEPTED.consumerOutputId]);
        const outputId = Number(payload[J.OUTPUT_ACCEPTED.outputId]);
        const output = this._outputsByConsumerId.get(consumerOutputId);
        if (!output || !Number.isFinite(outputId)) {
            log(`invalid OUTPUT_ACCEPTED ${JSON.stringify(payload)}`);
            return;
        }

        output.backendOutputId = outputId;
        this._outputsByBackendId.set(outputId, output);
        log(`monitor=${output.monitorIndex} accepted output=${outputId}`);
    }

    _handleBindBuffers(decoded, fdList) {
        const payload = decoded.payload;
        const outputId = Number(payload[J.BIND_BUFFERS.outputId]);
        const output = this._outputsByBackendId.get(outputId);
        if (!output) {
            log(`BIND_BUFFERS for unknown output=${outputId}`);
            return;
        }

        output.bindBuffers(payload, decoded.text, fdList);
    }

    _handleSetConfig(payload) {
        const outputId = Number(payload[J.SET_CONFIG.outputId]);
        const output = this._outputsByBackendId.get(outputId);
        if (!output) {
            log(`SET_CONFIG for unknown output=${payload[J.SET_CONFIG.outputId]}`);
            return;
        }

        output.setConfig(payload);
    }

    _handleFrameReady(frame, fdList) {
        const fdCount = fdList?.get_length?.() ?? 0;
        if (fdCount !== FRAME_READY_FD_COUNT) {
            log(`FRAME_READY invalid explicit sync fd count=${fdCount} expected=${FRAME_READY_FD_COUNT}`);
            closeFrameFdList(fdList);
            return;
        }
        const output = this._outputsByBackendId.get(Number(frame.outputId));
        if (!output) {
            log(`FRAME_READY for unknown output=${frame.outputId} ` +
                `generation=${frame.generation} buffer=${frame.bufferIndex} ` +
                `known-outputs=${[...this._outputsByBackendId.keys()].join(',') || '(none)'}`);
            closeFrameFdList(fdList);
            return;
        }
        output.showFrame(frame, fdList);
    }

    _handleUnbind(payload) {
        const outputId = Number(payload.outputId ?? 0);
        const generation = Number(payload.generation ?? 0);
        const output = this._outputsByBackendId.get(outputId);
        if (!output) {
            log(`UNBIND for unknown output=${outputId} generation=${generation}`);
            this._queueFrame(encodeJsonFrame(REQ_UNBIND_DONE, {
                [J.UNBIND_DONE.outputId]: outputId,
                [J.UNBIND_DONE.generation]: generation,
            }));
            return;
        }
        output.unbindGeneration(generation);
        if (output._bufferGenerations.size === 0)
            output._deactivatePresentation('unbind-last-generation');
        this._queueFrame(encodeJsonFrame(REQ_UNBIND_DONE, {
            [J.UNBIND_DONE.outputId]: outputId,
            [J.UNBIND_DONE.generation]: generation,
        }));
    }
}

class StdinBridge {
    constructor(connection) {
        this._connection = connection;
        this._stream = Gio.DataInputStream.new(new GioUnix.InputStream({
            fd: 0,
            close_fd: false,
        }));
        this._cancellable = new Gio.Cancellable();
    }

    start() {
        this._readNext();
    }

    stop() {
        try {
            this._cancellable.cancel();
        } catch (_e) {
        }
    }

    _readNext() {
        this._stream.read_line_async(GLib.PRIORITY_DEFAULT, this._cancellable, (stream, result) => {
            let line = null;
            let length = 0;
            try {
                [line, length] = stream.read_line_finish_utf8(result);
            } catch (error) {
                if (!error.matches?.(Gio.IOErrorEnum, Gio.IOErrorEnum.CANCELLED))
                    log(`stdin read failed: ${error}`);
                return;
            }

            if (line === null || length === 0) {
                log('stdin closed');
                return;
            }

            try {
                this._connection.handleControlMessage(JSON.parse(line));
            } catch (error) {
                log(`invalid stdin control message: ${error}`);
            }

            this._readNext();
        });
    }
}

const opts = parseArgs(ARGV);
const app = new Gtk.Application({
    application_id: APPLICATION_ID,
    flags: Gio.ApplicationFlags.FLAGS_NONE,
});

let displayConnection = null;
let stdinBridge = null;

app.connect('activate', application => {
    const display = Gdk.Display.get_default();
    if (!display) {
        log('GDK display is unavailable');
        application.quit();
        return;
    }

    const outputs = [];

    if (Array.isArray(opts.outputs) && opts.outputs.length > 0) {
        log(`Shell monitor layout count=${opts.outputs.length}`);
        for (let i = 0; i < opts.outputs.length; i++) {
            const info = monitorInfoFromPayload(opts.outputs[i], i, display);
            log(`Shell monitor[${info.monitorIndex}] consumerOutputId=${info.consumerOutputId} ` +
                `${info.logicalWidth}x${info.logicalHeight}+${info.x}+${info.y} ` +
                `scale=${info.scale} physical=${info.physicalWidth}x${info.physicalHeight} ` +
                `refresh=${info.refreshRate} name=${info.displayName || '(unnamed)'}`);
            outputs.push(new OutputWindow(application, info));
        }
    } else {
        const monitors = display.get_monitors();
        log(`GDK monitor count=${monitors.get_n_items()}`);
        for (let i = 0; i < monitors.get_n_items(); i++) {
            const monitor = monitors.get_item(i);
            if (monitor) {
                const info = monitorInfoFromGdkMonitor(monitor, i);
                log(`GDK monitor[${info.monitorIndex}] consumerOutputId=${info.consumerOutputId} ` +
                    `${info.logicalWidth}x${info.logicalHeight}+${info.x}+${info.y} ` +
                    `scale=${info.scale} physical=${info.physicalWidth}x${info.physicalHeight} ` +
                    `refresh=${info.refreshRate} name=${info.displayName || '(unnamed)'}`);
                outputs.push(new OutputWindow(application, info));
            }
        }
    }

    if (outputs.length === 0) {
        log('no monitor layout available');
        application.quit();
        return;
    }

    displayConnection = new DisplayConnection(application, opts, outputs);
    displayConnection.start();
    stdinBridge = new StdinBridge(displayConnection);
    stdinBridge.start();
});

app.connect('shutdown', () => {
    stdinBridge?.stop();
    displayConnection?.stop();
});

system.exit(app.run([]));
