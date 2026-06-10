#!/usr/bin/env -S gjs -m

// Protocol optimization changes in this file are derived from waywallen.
// Source: local reference tree waywallen/ and upstream https://github.com/waywallen/waywallen.
// Copyright owner for the waywallen-derived protocol optimization code:
// https://github.com/hypengw <hypengwip@gmail.com>.


import Gdk from 'gi://Gdk?version=4.0';
import GIRepository from 'gi://GIRepository';
import Gio from 'gi://Gio';
import GioUnix from 'gi://GioUnix?version=2.0';
import GLib from 'gi://GLib';
import Gtk from 'gi://Gtk?version=4.0';
import cairo from 'cairo';
import system from 'system';

const APPLICATION_ID = 'dev.rikka.VividWallpaper.Helper';
const TITLE_PREFIX = `@${APPLICATION_ID}!`;
const PROTOCOL_NAME = 'vivid-display-v1';
const PROTOCOL_VERSION = 1;
const MAX_BODY_BYTES = 65531;
const FRAME_HEADER_BYTES = 4;
const POINTER_MOTION_BODY_BYTES = 28;
const POINTER_BUTTON_BODY_BYTES = 36;
const POINTER_AXIS_BODY_BYTES = 48;
const FRAME_READY_BODY_BYTES = 36;
const FRAME_READY_FD_COUNT = 2;
const UNBIND_BODY_BYTES = 12;

const REQ_HELLO = 1;
const REQ_REGISTER_OUTPUT = 2;
const REQ_UPDATE_OUTPUT = 3;
const REQ_CONSUMER_CAPS = 4;
const REQ_POINTER_MOTION = 7;
const REQ_POINTER_BUTTON = 8;
const REQ_POINTER_AXIS = 9;
const REQ_WINDOW_STATE = 10;
const REQ_MEDIA_STATE = 12;
const REQ_AUDIO_SAMPLES = 13;
const REQ_BIND_FAILED = 14;
const REQ_UNBIND_DONE = 15;

const EVT_WELCOME = 1;
const EVT_OUTPUT_ACCEPTED = 2;
const EVT_BIND_BUFFERS = 3;
const EVT_SET_CONFIG = 4;
const EVT_FRAME_READY = 5;
const EVT_UNBIND = 6;
const EVT_ERROR = 9;

const DRM_FORMAT_XRGB8888 = 0x34325258; // 'XR24'
const DRM_FORMAT_ARGB8888 = 0x34325241; // 'AR24'
const DRM_FORMAT_XBGR8888 = 0x34324258; // 'XB24'
const DRM_FORMAT_ABGR8888 = 0x34324241; // 'AB24'
const DRM_FORMAT_MOD_LINEAR = 0;
const DRM_FORMAT_MOD_INVALID = 0x00ffffffffffffffn;
const FRAME_SYNC_WAIT_TIMEOUT_MSEC = 1000;
const VIVID_RGBA_FOURCCS = [
    DRM_FORMAT_XRGB8888,
    DRM_FORMAT_ARGB8888,
    DRM_FORMAT_XBGR8888,
    DRM_FORMAT_ABGR8888,
];

const POINTER_BUTTON_RELEASED = 0;
const POINTER_BUTTON_PRESSED = 1;
const POINTER_AXIS_WHEEL = 0;
const FRAME_INTERVAL_WARN_USEC = 50_000;
const TEXTURE_REFRESH_WARN_USEC = 8_000;
const RELEASE_FLUSH_WARN_USEC = 8_000;
const FRAME_DIAGNOSTIC_LOG_INTERVAL_USEC = 1_000_000;

const encoder = new TextEncoder();
const decoder = new TextDecoder();
const giRepository = GIRepository.Repository.dup_default();

const moduleFile = GLib.filename_from_uri(import.meta.url)[0];
const moduleDir = GLib.path_get_dirname(moduleFile);
const extensionDir = GLib.path_get_dirname(GLib.path_get_dirname(moduleDir));
const commonDir = GLib.build_filenamev([extensionDir, 'common']);
const displayConsumerDir = GLib.build_filenamev([extensionDir, 'display_consumer']);
const displayConsumerTypelibDir = GLib.build_filenamev([displayConsumerDir, 'girepository-1.0']);

imports.searchPath.unshift(commonDir);
giRepository.prepend_search_path(displayConsumerTypelibDir);
giRepository.prepend_library_path(displayConsumerDir);

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

function gdkDmabufFormatAt(formats, index) {
    const result = formats.get_format(index);
    if (Array.isArray(result))
        return [Number(result[0]), result[1]];

    /*
     * GJS normally maps the two out parameters to an array. Keep a defensive
     * object path here because this is an introspection boundary and the caps
     * packet should fall back cleanly instead of crashing the helper if the
     * binding shape changes.
     */
    if (result && typeof result === 'object') {
        const fourcc = result.fourcc ?? result.format ?? result[0];
        const modifier = result.modifier ?? result[1];
        if (fourcc !== undefined && modifier !== undefined)
            return [Number(fourcc), modifier];
    }

    return [0, DRM_FORMAT_MOD_INVALID];
}

function buildDmaBufCaps() {
    const caps = {
        version: 3,
        backend: 'gnome-gtk4-gdk-dmabuf-texture-builder',
        probe: 'unprobed',
        relayModes: ['direct-import-v1', 'shadow-copy-v1'],
        renderNode: '',
        deviceUuid: '',
        driverUuid: '',
        vendor: '',
        pciAddress: '',
        fourccs: [],
        modifiers: [],
        implicitLinearFourccs: [],
        memoryHints: ['host-visible', 'implicit-linear'],
        syncCaps: ['implicit', 'explicit-sync-fd', 'drm-syncobj-release'],
        colorCaps: ['srgb', 'limited-range', 'premultiplied-alpha'],
        extentMax: { width: 0, height: 0 },
        textureTarget: 'GdkDmabufTexture',
        skipsExternalOnlyModifiers: false,
        diagnostics: '',
    };

    try {
        const display = Gdk.Display.get_default();
        caps.renderNode = stringFromDisplayConsumer(
            callDisplayConsumerFunction('dmabuf_texture_get_render_node', display));
        caps.deviceUuid = stringFromDisplayConsumer(
            callDisplayConsumerFunction('dmabuf_texture_get_device_uuid', display));
        caps.driverUuid = stringFromDisplayConsumer(
            callDisplayConsumerFunction('dmabuf_texture_get_driver_uuid', display));
        caps.vendor = stringFromDisplayConsumer(
            callDisplayConsumerFunction('dmabuf_texture_get_vendor', display));
        caps.pciAddress = stringFromDisplayConsumer(
            callDisplayConsumerFunction('dmabuf_texture_get_pci_address', display));

        const formats = display?.get_dmabuf_formats?.();
        const nFormats = formats?.get_n_formats?.() ?? 0;
        for (let index = 0; index < nFormats; index++) {
            const [fourcc, modifier] = gdkDmabufFormatAt(formats, index);
            if (!isVividRgbaFourcc(fourcc))
                continue;

            appendUniqueNumber(caps.fourccs, fourcc);
            if (uint64Equals(modifier, DRM_FORMAT_MOD_LINEAR) ||
                uint64IsDrmModifierInvalid(modifier)) {
                appendUniqueNumber(caps.implicitLinearFourccs, fourcc);
                continue;
            }

            /*
             * GdkDmabufFormats exposes the importable fourcc/modifier pairs but
             * not plane counts. The display consumer helper resolves GDK's actual
             * Wayland/EGL render node and performs a tiny GBM BO allocation to
             * obtain the same (fourcc, modifier, plane_count) tuple shape that
             * waywallen negotiates. Unknown counts deliberately fall back to 1:
             * the producer strict-matches planeCount, so this prevents choosing
             * a multi-plane modifier unless the consumer has proved that count.
             */
            const probedPlaneCount =
                Number(callDisplayConsumerFunction(
                    'dmabuf_texture_probe_plane_count',
                    display,
                    fourcc,
                    modifier) ?? 0);
            caps.modifiers.push({
                fourcc,
                modifier: uint64ToProtocolValue(modifier),
                planeCount: probedPlaneCount > 0 ? probedPlaneCount : 1,
            });
        }

        if (caps.fourccs.length > 0) {
            caps.probe = caps.renderNode
                ? 'gdk-display-dmabuf-formats+egl-gbm-plane-probe'
                : 'gdk-display-dmabuf-formats';
        }
    } catch (error) {
        caps.diagnostics = `GDK DMA-BUF format query failed: ${error}`;
        log(`GDK DMA-BUF format query failed; sending v3 diagnostics without fake caps: ${error}`);
    }

    if (caps.fourccs.length === 0) {
        caps.probe = caps.probe === 'unprobed' ? 'probe-empty' : caps.probe;
        caps.memoryHints = [];
        caps.skipsExternalOnlyModifiers = true;
    } else if (caps.implicitLinearFourccs.length === 0) {
        caps.memoryHints = ['device-local'];
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
    };

    for (let i = 0; i < argv.length; i++) {
        if (argv[i] === '--socket' && i + 1 < argv.length)
            opts.socketPath = argv[++i];
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

const decodeFrameReadyBody = body => {
    if (body.length !== FRAME_READY_BODY_BYTES)
        throw new Error(`invalid FRAME_READY body length ${body.length}`);

    return {
        outputId: readUint32LE(body, 0),
        generation: readUint64LE(body, 4),
        bufferIndex: readUint32LE(body, 12),
        sequence: readUint64LE(body, 16),
        targetTimeUsec: readUint64LE(body, 24),
        flags: readUint32LE(body, 32),
    };
};

const decodeUnbindBody = body => {
    if (body.length !== UNBIND_BODY_BYTES)
        throw new Error(`invalid UNBIND body length ${body.length}`);

    return {
        outputId: readUint32LE(body, 0),
        generation: readUint64LE(body, 4),
    };
};

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

const WEB_AUDIO_UPDATE_INTERVAL_NS = 16_666_667;
const WEB_AUDIO_OUTPUT_BANDS_PER_CHANNEL = 64;
const WEB_AUDIO_FRAME_LENGTH = WEB_AUDIO_OUTPUT_BANDS_PER_CHANNEL * 2;
const WEB_AUDIO_POLL_INTERVAL_MS = Math.max(1, Math.round(WEB_AUDIO_UPDATE_INTERVAL_NS / 1_000_000));
const WEB_AUDIO_RESTART_DELAY_MS = 1000;
const WEB_AUDIO_SAMPLE_RATE = 44100;
const WEB_AUDIO_FFT_SIZE = 2048;
const WEB_AUDIO_SLOW_PROCESSING_LOG_THRESHOLD_US = 8000;
const WEB_AUDIO_FRAME_LOG_INTERVAL_FRAMES = Math.max(1, Math.round(5000 / WEB_AUDIO_POLL_INTERVAL_MS));
const WEB_AUDIO_MIN_FREQUENCY_HZ = 30;
const WEB_AUDIO_MAX_FREQUENCY_HZ = 18000;
const WEB_AUDIO_MIN_DB = -80;
const WEB_AUDIO_MAX_DB = 0;
const WEB_AUDIO_SILENCE_RMS_THRESHOLD = 0.003;
const WEB_AUDIO_SPECTRUM_OUTPUT_GAIN = 4.0;
const WEB_AUDIO_BAND_PEAK_BLEND = 0.35;
const AUDIO_SAMPLE_MAX_VALUES = 512;

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
    if (!samples || typeof samples.length !== 'number')
        return buildSilentWebAudioFrame();

    const limit = Math.min(AUDIO_SAMPLE_MAX_VALUES, Math.max(0, samples.length));
    const normalized = [];
    for (let index = 0; index < limit; index++) {
        const value = Number(samples[index] ?? 0);
        normalized.push(Number.isFinite(value) ? clipNumber(value, 0, 1) : 0);
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

const buildLogBandEdgesHz = () => Array.from(
    {length: WEB_AUDIO_OUTPUT_BANDS_PER_CHANNEL + 1},
    (_unused, index) => {
        const t = index / WEB_AUDIO_OUTPUT_BANDS_PER_CHANNEL;
        return WEB_AUDIO_MIN_FREQUENCY_HZ *
            Math.pow(WEB_AUDIO_MAX_FREQUENCY_HZ / WEB_AUDIO_MIN_FREQUENCY_HZ, t);
    }
);

const WEB_AUDIO_BAND_EDGES_HZ = buildLogBandEdgesHz();
const WEB_AUDIO_BAND_CENTERS_HZ = WEB_AUDIO_BAND_EDGES_HZ.slice(0, -1).map((edge, index) =>
    Math.sqrt(edge * WEB_AUDIO_BAND_EDGES_HZ[index + 1])
);
const WEB_AUDIO_FREQUENCIES_HZ = Array.from(
    {length: Math.floor(WEB_AUDIO_FFT_SIZE / 2) + 1},
    (_unused, index) => (index * WEB_AUDIO_SAMPLE_RATE) / WEB_AUDIO_FFT_SIZE
);
const WEB_AUDIO_WINDOW = Float32Array.from(
    {length: WEB_AUDIO_FFT_SIZE},
    (_unused, index) => 0.5 * (1 - Math.cos((2 * Math.PI * index) / (WEB_AUDIO_FFT_SIZE - 1)))
);
const WEB_AUDIO_MAGNITUDE_REFERENCE = Math.max(
    1,
    WEB_AUDIO_WINDOW.reduce((sum, sample) => sum + sample, 0) * 0.5
);
const WEB_AUDIO_BAND_BIN_RANGES = WEB_AUDIO_BAND_EDGES_HZ.slice(0, -1).map((_edge, index) => {
    const low = WEB_AUDIO_BAND_EDGES_HZ[index];
    const high = WEB_AUDIO_BAND_EDGES_HZ[index + 1];
    let begin = 0;
    while (begin < WEB_AUDIO_FREQUENCIES_HZ.length && WEB_AUDIO_FREQUENCIES_HZ[begin] < low)
        begin++;
    let end = begin;
    while (end < WEB_AUDIO_FREQUENCIES_HZ.length && WEB_AUDIO_FREQUENCIES_HZ[end] < high)
        end++;
    return {begin, end};
});

const createSpectrumProcessorState = () => ({
    smoothed: new Float32Array(WEB_AUDIO_OUTPUT_BANDS_PER_CHANNEL),
    lastDb: new Float32Array(WEB_AUDIO_OUTPUT_BANDS_PER_CHANNEL).fill(WEB_AUDIO_MIN_DB),
    bandDb: new Float32Array(WEB_AUDIO_OUTPUT_BANDS_PER_CHANNEL),
    normalized: new Float32Array(WEB_AUDIO_OUTPUT_BANDS_PER_CHANNEL),
    horizontallySmoothed: new Float32Array(WEB_AUDIO_OUTPUT_BANDS_PER_CHANNEL),
    real: new Float64Array(WEB_AUDIO_FFT_SIZE),
    imag: new Float64Array(WEB_AUDIO_FFT_SIZE),
    magnitudes: new Float32Array(Math.floor(WEB_AUDIO_FFT_SIZE / 2) + 1),
    normalizedMagnitudes: new Float32Array(Math.floor(WEB_AUDIO_FFT_SIZE / 2) + 1),
    binDb: new Float32Array(Math.floor(WEB_AUDIO_FFT_SIZE / 2) + 1),
});

const appendInterleavedStereoChunk = (leftBuffer, rightBuffer, interleaved, frameCount) => {
    if (!(leftBuffer instanceof Float32Array) || !(rightBuffer instanceof Float32Array) ||
        !(interleaved instanceof Float32Array) || frameCount <= 0)
        return;

    const capacity = Math.min(leftBuffer.length, rightBuffer.length);
    if (frameCount >= capacity) {
        const startFrame = frameCount - capacity;
        for (let i = 0; i < capacity; i++) {
            const sourceIndex = (startFrame + i) * 2;
            const left = interleaved[sourceIndex] ?? 0;
            leftBuffer[i] = left;
            rightBuffer[i] = interleaved[sourceIndex + 1] ?? left;
        }
        return;
    }

    leftBuffer.copyWithin(0, frameCount);
    rightBuffer.copyWithin(0, frameCount);
    const writeOffset = capacity - frameCount;
    for (let i = 0; i < frameCount; i++) {
        const sourceIndex = i * 2;
        const left = interleaved[sourceIndex] ?? 0;
        leftBuffer[writeOffset + i] = left;
        rightBuffer[writeOffset + i] = interleaved[sourceIndex + 1] ?? left;
    }
};

const interpolateLinearly = (xs, ys, target) => {
    if (!Array.isArray(xs) || !(ys instanceof Float32Array) || xs.length === 0 || ys.length === 0)
        return WEB_AUDIO_MIN_DB;

    if (target <= xs[0])
        return ys[0];
    const lastIndex = Math.min(xs.length, ys.length) - 1;
    if (target >= xs[lastIndex])
        return ys[lastIndex];

    let lowerIndex = 0;
    while (lowerIndex < lastIndex && xs[lowerIndex + 1] < target)
        lowerIndex++;

    const upperIndex = Math.min(lastIndex, lowerIndex + 1);
    const lowerX = xs[lowerIndex];
    const upperX = xs[upperIndex];
    if (upperX <= lowerX)
        return ys[lowerIndex];

    const mix = (target - lowerX) / (upperX - lowerX);
    return ys[lowerIndex] + (ys[upperIndex] - ys[lowerIndex]) * mix;
};

const resampleSpectrumValues = (values, resolution) => {
    const targetSize = Math.max(0, Number(resolution) || 0);
    const sourceSize = values?.length ?? 0;
    const result = new Array(targetSize).fill(0);
    if (targetSize === 0 || sourceSize === 0)
        return result;

    if (targetSize === sourceSize)
        return Array.from(values);

    for (let i = 0; i < targetSize; i++) {
        const sourcePosition = ((i + 0.5) * sourceSize / targetSize) - 0.5;
        const clampedPosition = clipNumber(sourcePosition, 0, sourceSize - 1);
        const lowerIndex = Math.floor(clampedPosition);
        const upperIndex = Math.min(sourceSize - 1, lowerIndex + 1);
        const mix = clampedPosition - lowerIndex;
        const lowerValue = Number(values[lowerIndex] ?? 0);
        const upperValue = Number(values[upperIndex] ?? 0);
        result[i] = lowerValue + (upperValue - lowerValue) * mix;
    }
    return result;
};

const shouldLogWebAudioFrame = frameCount =>
    frameCount <= 8 || frameCount % WEB_AUDIO_FRAME_LOG_INTERVAL_FRAMES === 0;

const normalizeAbsoluteSpectrumMagnitude = magnitude =>
    clipNumber(((Number(magnitude) || 0) / WEB_AUDIO_MAGNITUDE_REFERENCE) * WEB_AUDIO_SPECTRUM_OUTPUT_GAIN, 0, 1);

const magnitudeToDb = magnitude => {
    if (!Number.isFinite(magnitude) || magnitude <= 0)
        return WEB_AUDIO_MIN_DB;
    return clipNumber(20 * Math.log10(magnitude + 1e-12), WEB_AUDIO_MIN_DB, WEB_AUDIO_MAX_DB);
};

const computeSceneLow16Pair = values => {
    const bins16 = resampleSpectrumValues(values, 16);
    return [Number(bins16[0] ?? 0), Number(bins16[1] ?? 0)];
};

const computeSceneLow16Average = (leftPair, rightPair) =>
    (Number(leftPair[0] ?? 0) + Number(leftPair[1] ?? 0) +
        Number(rightPair[0] ?? 0) + Number(rightPair[1] ?? 0)) * 0.25;

const formatSpectrumPair = values =>
    `[${Number(values[0] ?? 0).toFixed(4)}, ${Number(values[1] ?? 0).toFixed(4)}]`;

const computeRfftMagnitudes = (pcm, state) => {
    const real = state.real;
    const imag = state.imag;
    const magnitudes = state.magnitudes;

    real.fill(0);
    imag.fill(0);

    for (let i = 0; i < WEB_AUDIO_FFT_SIZE; i++)
        real[i] = (pcm[i] ?? 0) * WEB_AUDIO_WINDOW[i];

    for (let i = 1, j = 0; i < WEB_AUDIO_FFT_SIZE; i++) {
        let bit = WEB_AUDIO_FFT_SIZE >> 1;
        while (j & bit) {
            j ^= bit;
            bit >>= 1;
        }
        j ^= bit;
        if (i < j) {
            const realValue = real[i];
            real[i] = real[j];
            real[j] = realValue;
            const imagValue = imag[i];
            imag[i] = imag[j];
            imag[j] = imagValue;
        }
    }

    for (let size = 2; size <= WEB_AUDIO_FFT_SIZE; size <<= 1) {
        const halfSize = size >> 1;
        const theta = (-2 * Math.PI) / size;
        const phaseRealStep = Math.cos(theta);
        const phaseImagStep = Math.sin(theta);
        for (let offset = 0; offset < WEB_AUDIO_FFT_SIZE; offset += size) {
            let phaseReal = 1;
            let phaseImag = 0;
            for (let i = 0; i < halfSize; i++) {
                const evenIndex = offset + i;
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

    for (let index = 0; index < magnitudes.length; index++)
        magnitudes[index] = Math.hypot(real[index], imag[index]);
    return magnitudes;
};

const applyHorizontalSmoothing = (values, target) => {
    for (let i = 0; i < values.length; i++) {
        const left = values[Math.max(0, i - 1)] ?? 0;
        const center = values[i] ?? 0;
        const right = values[Math.min(values.length - 1, i + 1)] ?? 0;
        target[i] = left * 0.08 + center * 0.84 + right * 0.08;
    }
    return target;
};

const processSpectrumFrame = (pcm, state) => {
    if (!(pcm instanceof Float32Array) || !(state?.smoothed instanceof Float32Array)) {
        return {
            values: new Float32Array(WEB_AUDIO_OUTPUT_BANDS_PER_CHANNEL),
            dbValues: new Float32Array(WEB_AUDIO_OUTPUT_BANDS_PER_CHANNEL).fill(WEB_AUDIO_MIN_DB),
            rms: 0,
            framePeak: 0,
        };
    }

    const rms = Math.sqrt(pcm.reduce((sum, sample) => sum + sample * sample, 0) /
        Math.max(1, pcm.length) + 1e-12);
    if (rms < WEB_AUDIO_SILENCE_RMS_THRESHOLD) {
        for (let i = 0; i < WEB_AUDIO_OUTPUT_BANDS_PER_CHANNEL; i++)
            state.smoothed[i] *= 0.82;
        state.lastDb.fill(WEB_AUDIO_MIN_DB);
        return {
            values: state.smoothed,
            dbValues: state.lastDb,
            rms,
            framePeak: 0,
        };
    }

    const magnitudes = computeRfftMagnitudes(pcm, state);
    const normalizedMagnitudes = state.normalizedMagnitudes;
    let framePeak = 0;
    const binDb = state.binDb;
    for (let i = 0; i < magnitudes.length; i++) {
        const normalizedMagnitude = normalizeAbsoluteSpectrumMagnitude(magnitudes[i]);
        normalizedMagnitudes[i] = normalizedMagnitude;
        framePeak = Math.max(framePeak, normalizedMagnitude);
        binDb[i] = magnitudeToDb(normalizedMagnitude);
    }

    const bandDb = state.bandDb;
    const normalized = state.normalized;
    for (let i = 0; i < WEB_AUDIO_OUTPUT_BANDS_PER_CHANNEL; i++) {
        const centerMagnitude = interpolateLinearly(
            WEB_AUDIO_FREQUENCIES_HZ,
            normalizedMagnitudes,
            WEB_AUDIO_BAND_CENTERS_HZ[i]
        );

        let powerSum = 0;
        let sampleCount = 0;
        let peakMagnitude = 0;
        const {begin, end} = WEB_AUDIO_BAND_BIN_RANGES[i];
        for (let bin = begin; bin < end; bin++) {
            const magnitude = normalizedMagnitudes[bin] ?? 0;
            powerSum += magnitude * magnitude;
            peakMagnitude = Math.max(peakMagnitude, magnitude);
            sampleCount++;
        }

        let bandMagnitude = centerMagnitude;
        if (sampleCount > 0) {
            const rmsMagnitude = Math.sqrt(powerSum / sampleCount + 1e-12);
            const blendedPeakMagnitude =
                peakMagnitude * WEB_AUDIO_BAND_PEAK_BLEND + rmsMagnitude * (1 - WEB_AUDIO_BAND_PEAK_BLEND);
            bandMagnitude = Math.max(centerMagnitude, blendedPeakMagnitude);
        }

        bandMagnitude = clipNumber(bandMagnitude, 0, 1);
        normalized[i] = bandMagnitude;
        bandDb[i] = magnitudeToDb(bandMagnitude);
    }

    state.lastDb.set(bandDb);
    const horizontallySmoothed = applyHorizontalSmoothing(normalized, state.horizontallySmoothed);
    for (let i = 0; i < WEB_AUDIO_OUTPUT_BANDS_PER_CHANNEL; i++) {
        const target = horizontallySmoothed[i];
        const current = state.smoothed[i];
        state.smoothed[i] = target > current
            ? current * 0.25 + target * 0.75
            : current * 0.75 + target * 0.25;
    }

    return {
        values: state.smoothed,
        dbValues: state.lastDb,
        rms,
        framePeak,
    };
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
        this._pollSourceId = 0;
        this._processingSourceId = 0;
        this._restartSourceId = 0;
        this._shouldRun = false;
        this._isAvailable = true;
        this._lastFrame = buildSilentWebAudioFrame();
        this._workingFrame = buildSilentWebAudioFrame();
        this._pendingInterleavedChunk = null;
        this._pendingInterleavedFrameCount = 0;
        this._leftSampleBuffer = new Float32Array(WEB_AUDIO_FFT_SIZE * 2);
        this._rightSampleBuffer = new Float32Array(WEB_AUDIO_FFT_SIZE * 2);
        this._leftProcessorState = createSpectrumProcessorState();
        this._rightProcessorState = createSpectrumProcessorState();
        this._emittedFrameCount = 0;
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

    stop({emitSilence = true, reason = 'unspecified'} = {}) {
        this._shouldRun = false;
        this._cancelRestart();
        this._teardownPipeline();

        if (emitSilence) {
            log(`audio sample capture stopped reason=${reason}; emitting silence`);
            this._emitFrame(buildSilentWebAudioFrame());
        }
    }

    destroy() {
        this.stop({emitSilence: false, reason: 'destroy'});
        this._onFrame = null;
    }

    _resetSpectrumState() {
        this._leftSampleBuffer.fill(0);
        this._rightSampleBuffer.fill(0);
        this._leftProcessorState = createSpectrumProcessorState();
        this._rightProcessorState = createSpectrumProcessorState();
        this._emittedFrameCount = 0;
        this._pendingInterleavedChunk = null;
        this._pendingInterleavedFrameCount = 0;
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

        try {
            this._pipeline = Gst.parse_launch(
                'pulsesrc device=@DEFAULT_MONITOR@ client-name=VividWallpaperVisualizer do-timestamp=true ! ' +
                'audioconvert ! audioresample ! ' +
                `audio/x-raw,format=F32LE,channels=2,rate=${WEB_AUDIO_SAMPLE_RATE} ! ` +
                'appsink name=audio_sink emit-signals=false max-buffers=1 drop=true sync=false'
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

        this._resetSpectrumState();

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
            log(`audio sample capture started bands=${WEB_AUDIO_FRAME_LENGTH} sampleRate=${WEB_AUDIO_SAMPLE_RATE}`);
            this._pollSourceId = GLib.timeout_add(
                GLib.PRIORITY_DEFAULT,
                WEB_AUDIO_POLL_INTERVAL_MS,
                () => {
                    try {
                        this._pullLatestAudioSample();
                    } catch (error) {
                        log(`audio sample polling failed: ${error}`);
                    }
                    return GLib.SOURCE_CONTINUE;
                }
            );
        }
    }

    _pullLatestAudioSample() {
        if (!this._appsink)
            return;

        const sample = this._appsink.emit('try-pull-sample', 0);
        if (!sample)
            return;

        const buffer = sample.get_buffer?.();
        if (!buffer)
            return;

        const [mapped, mapInfo] = buffer.map(Gst.MapFlags.READ);
        if (!mapped || !mapInfo?.data || mapInfo.size < Float32Array.BYTES_PER_ELEMENT) {
            if (mapped)
                buffer.unmap(mapInfo);
            return;
        }

        const interleaved = new Float32Array(
            mapInfo.data.buffer,
            mapInfo.data.byteOffset,
            Math.floor(mapInfo.size / Float32Array.BYTES_PER_ELEMENT)
        );
        const frameCount = Math.floor(interleaved.length / 2);
        const interleavedCopy = new Float32Array(frameCount * 2);
        interleavedCopy.set(interleaved.subarray(0, frameCount * 2));
        buffer.unmap(mapInfo);

        this._pendingInterleavedChunk = interleavedCopy;
        this._pendingInterleavedFrameCount = frameCount;
        this._schedulePendingAudioProcessing();
    }

    _schedulePendingAudioProcessing() {
        if (this._processingSourceId)
            return;

        this._processingSourceId = GLib.idle_add(GLib.PRIORITY_DEFAULT_IDLE, () => {
            this._processingSourceId = 0;
            try {
                this._processPendingAudioChunk();
            } catch (error) {
                log(`audio sample processing failed: ${error}`);
            }
            return GLib.SOURCE_REMOVE;
        });
    }

    _processPendingAudioChunk() {
        const interleaved = this._pendingInterleavedChunk;
        const frameCount = this._pendingInterleavedFrameCount;
        this._pendingInterleavedChunk = null;
        this._pendingInterleavedFrameCount = 0;
        if (!(interleaved instanceof Float32Array) || frameCount <= 0)
            return;

        const startedAtUs = GLib.get_monotonic_time();
        appendInterleavedStereoChunk(this._leftSampleBuffer, this._rightSampleBuffer, interleaved, frameCount);

        const leftProcessed = processSpectrumFrame(
            this._leftSampleBuffer.subarray(this._leftSampleBuffer.length - WEB_AUDIO_FFT_SIZE),
            this._leftProcessorState
        );
        const rightProcessed = processSpectrumFrame(
            this._rightSampleBuffer.subarray(this._rightSampleBuffer.length - WEB_AUDIO_FFT_SIZE),
            this._rightProcessorState
        );
        const normalized = this._workingFrame;
        for (let i = 0; i < WEB_AUDIO_OUTPUT_BANDS_PER_CHANNEL; i++) {
            normalized[i] = leftProcessed.values[i];
            normalized[i + WEB_AUDIO_OUTPUT_BANDS_PER_CHANNEL] = rightProcessed.values[i];
        }

        this._emittedFrameCount++;
        if (shouldLogWebAudioFrame(this._emittedFrameCount)) {
            const leftSceneLow16 = computeSceneLow16Pair(leftProcessed.values);
            const rightSceneLow16 = computeSceneLow16Pair(rightProcessed.values);
            const sceneLow16Average = computeSceneLow16Average(leftSceneLow16, rightSceneLow16);
            const frameMax = normalized.reduce((max, value) => Math.max(max, Number(value) || 0), 0);
            log(
                `audio samples frame=${this._emittedFrameCount} ` +
                `rms=${leftProcessed.rms.toFixed(4)}/${rightProcessed.rms.toFixed(4)} ` +
                `peak=${leftProcessed.framePeak.toFixed(4)}/${rightProcessed.framePeak.toFixed(4)} ` +
                `max=${frameMax.toFixed(4)} low16avg=${sceneLow16Average.toFixed(4)} ` +
                `low16=${formatSpectrumPair(leftSceneLow16)}/${formatSpectrumPair(rightSceneLow16)} ` +
                `bands=${normalized.length}`
            );
        }

        this._emitFrame(normalized);

        const elapsedUs = GLib.get_monotonic_time() - startedAtUs;
        if (elapsedUs >= WEB_AUDIO_SLOW_PROCESSING_LOG_THRESHOLD_US) {
            log(
                `audio sample processing slow: ${(elapsedUs / 1000).toFixed(2)}ms ` +
                `frameCount=${frameCount} pollIntervalMs=${WEB_AUDIO_POLL_INTERVAL_MS}`
            );
        }

        if (this._pendingInterleavedChunk)
            this._schedulePendingAudioProcessing();
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
        if (this._pollSourceId) {
            GLib.source_remove(this._pollSourceId);
            this._pollSourceId = 0;
        }

        if (this._processingSourceId) {
            GLib.source_remove(this._processingSourceId);
            this._processingSourceId = 0;
        }

        this._pendingInterleavedChunk = null;
        this._pendingInterleavedFrameCount = 0;

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
            this._audioCapture.stop({emitSilence: false, reason: 'transport-disconnected'});
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
    constructor(app, monitor, monitorIndex) {
        this.monitor = monitor;
        this.monitorIndex = monitorIndex;
        this.consumerOutputId = monitorIndex;
        this.backendOutputId = null;
        this._bufferGenerations = new Map();
        this._currentGeneration = null;
        this._lastFrameUsec = 0;
        this._lastDiagnosticLogUsec = 0;
        this._suppressedFrameDiagnostics = 0;
        this._onBindFailed = null;
        this.paintable = DisplayConsumer.BufferPaintable.new();

        this.geometry = monitor.get_geometry();
        this.scale = monitorScale(monitor);
        this.logicalWidth = Math.max(1, Math.round(this.geometry.width));
        this.logicalHeight = Math.max(1, Math.round(this.geometry.height));
        this.physicalWidth = Math.max(1, Math.round(this.geometry.width * this.scale));
        this.physicalHeight = Math.max(1, Math.round(this.geometry.height * this.scale));
        this.refreshRate = monitorRefreshRate(monitor);

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
            application: app,
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
        this.window.set_title(`${TITLE_PREFIX}${JSON.stringify(state)}|${monitorIndex}`);
        this.window.connect('realize', () => this._onRealize());
        this.window.set_size_request(this.logicalWidth, this.logicalHeight);
        this.window.set_focus_on_map?.(false);
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
            consumerOutputId: this.consumerOutputId,
            monitorIndex: this.monitorIndex,
            x: this.geometry.x,
            y: this.geometry.y,
            /*
             * width/height describe the desktop logical surface. physicalWidth
             * and physicalHeight describe the DMA-BUF render target requested
             * from the producer. Keeping both values in the registration makes
             * the protocol explicit: GNOME Shell clones a logical-size helper
             * window, while scene/web/video renderers can still allocate a
             * scale-aware physical backing buffer and publish that size in
             * BIND_BUFFERS.
             */
            width: this.logicalWidth,
            height: this.logicalHeight,
            scale: this.scale,
            physicalWidth: this.physicalWidth,
            physicalHeight: this.physicalHeight,
            transform: 'normal',
            refreshRateMhz: this.refreshRate,
            desktop: 'gnome-shell-helper',
        };
    }

    bindBuffers(payload, bindJson, fdList) {
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
        const renderNode = payload['render-node'] ?? payload.renderNode ?? '(unknown)';
        const vendor = payload.vendor ?? 'unknown';
        const pciAddress = payload['pci-address'] ?? '(unknown)';
        const producerRenderNode = payload.producerRenderNode ?? renderNode;
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
            `memory-hint=${payload.memoryHint ?? '(missing)'} render-node=${renderNode} ` +
            `producer-render-node=${producerRenderNode} producer-drm=${producerDrm} ` +
            `consumer-render-node=${consumerRenderNode} consumer-drm=${consumerDrm} ` +
            `vendor=${vendor} pci=${pciAddress} ` +
            `modifier=${payload.modifier ?? '(missing)'} premultiplied=${!!payload.premultiplied} ` +
            `buffers=${formatBufferSummary(payload)}`);
    }

    setConfig(payload) {
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
        const renderNode = generation?.payload?.['render-node'] ??
            generation?.payload?.renderNode ?? '';
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

    _flushPendingReleaseSyncobj(reason) {
        /*
         * Mirror waywallen's EGL/QML display path: release the previously
         * accepted frame as soon as the next FRAME_READY arrives. GTK/GDK does
         * not expose a compositor release fence for GdkDmabufTexture, so waiting
         * for a later snapshot pass can hold the producer's per-frame release
         * syncobj for hundreds of milliseconds if Shell rendering stalls. The
         * display paintable still signals again from snapshot as a harmless
         * no-op fallback when no newer frame arrived.
         */
        const releaseStartUsec = GLib.get_monotonic_time();
        try {
            this.paintable.flush_pending_release_syncobj(reason);
        } catch (error) {
            log(`output ${this.backendOutputId}: release syncobj flush failed ` +
                `context=${reason}: ${error}`);
            return;
        }

        const releaseUsec = GLib.get_monotonic_time() - releaseStartUsec;
        if (releaseUsec >= RELEASE_FLUSH_WARN_USEC) {
            log(`output ${this.backendOutputId}: release syncobj flush was slow ` +
                `context=${reason} duration=${(releaseUsec / 1000).toFixed(2)}ms`);
        }
    }

    showFrame(frame, fdList) {
        let acquireFd = takeFrameFd(fdList, 0, 'acquire sync_file');
        let releaseFd = takeFrameFd(fdList, 1, 'release syncobj');
        this._flushPendingReleaseSyncobj('frame-ready');
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
            if (generation.payload?.presentationPath === 'shadow-copy') {
                if (typeof this.paintable.show_frame_with_sync !== 'function')
                    throw new Error('display paintable lacks show_frame_with_sync for shadow-copy');
                /*
                 * The shadow-copy path mirrors waywallen's DMABUF_RELAY:
                 * display module Vulkan imports the acquire sync_file as a temporary
                 * semaphore, waits in the blit submit, and signals the release
                 * syncobj after the shadow copy fence completes. Ownership of
                 * both fds transfers to the display module here.
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
            } else {
                const acquireReady = callDisplayConsumerFunction(
                    'dmabuf_texture_wait_sync_file',
                    acquireFd,
                    FRAME_SYNC_WAIT_TIMEOUT_MSEC
                );
                closeDisplayConsumerFd(acquireFd);
                acquireFd = -1;
                if (acquireReady !== true) {
                    this._signalReleaseSyncobj(generation, releaseFd, 'acquire-wait-failed');
                    closeDisplayConsumerFd(releaseFd);
                    releaseFd = -1;
                    log(`output ${this.backendOutputId}: FRAME_READY acquire wait failed ` +
                        `generation=${frame.generation} buffer=${frame.bufferIndex}`);
                    return;
                }
                this.paintable.show_frame(frame.generation, frame.bufferIndex);
                this.paintable.attach_release_syncobj(frame.generation, frame.bufferIndex, releaseFd);
                releaseFd = -1;
            }
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
        this._cancellable = new Gio.Cancellable();
        this._reconnectSourceId = 0;
        this._lastWindowState = null;
        this._lastMediaState = defaultMediaStatePayload();
        this._lastAudioSamples = buildSilentWebAudioFrame();
        this._audioSamplesSentCount = 0;
        this._mediaRuntime = new MediaRuntimeBridge({
            onMediaState: payload => this.sendMediaState(payload),
            onAudioSamples: samples => this.sendAudioSamples(samples),
        });
        for (const output of this._outputs)
            output.setBindFailedReporter(payload => this._sendBindFailed(payload));
    }

    start() {
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
                    log(`connected to ${this._opts.socketPath}`);
                } catch (error) {
                    log(`connect failed at ${this._opts.socketPath}: ${error}`);
                    this._scheduleReconnect();
                }
            }
        );
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
        let frame = null;
        try {
            frame = encodeJsonFrame(REQ_AUDIO_SAMPLES, {
                samples: this._lastAudioSamples,
                timeUsec: GLib.get_monotonic_time(),
            });
        } catch (error) {
            log(`audio samples encode failed: ${error}`);
            return false;
        }

        const queued = this._queueFrame(frame);
        if (queued) {
            this._audioSamplesSentCount++;
            if (shouldLogWebAudioFrame(this._audioSamplesSentCount)) {
                const maxSample = this._lastAudioSamples.reduce(
                    (max, value) => Math.max(max, Number(value) || 0),
                    0
                );
                log(`audio samples queued frame=${this._audioSamplesSentCount} ` +
                    `count=${this._lastAudioSamples.length} max=${maxSample.toFixed(4)}`);
            }
        }
        return queued;
    }

    _sendBindFailed(payload) {
        const framePayload = {
            outputId: Number(payload.outputId ?? 0),
            generation: Number(payload.generation ?? 0),
            fourcc: Number(payload.fourcc ?? 0),
            modifier: String(payload.modifier ?? '0'),
            bufferIndex: payload.bufferIndex === null || payload.bufferIndex === undefined
                ? null
                : Number(payload.bufferIndex),
            reason: Number(payload.reason ?? 1),
            message: String(payload.message ?? 'DMA-BUF import failed'),
        };
        if (!framePayload.fourcc) {
            log(`skip BIND_FAILED without fourcc: ${JSON.stringify(framePayload)}`);
            return false;
        }

        log(`BIND_FAILED output=${framePayload.outputId} generation=${framePayload.generation} ` +
            `fourcc=0x${framePayload.fourcc.toString(16).padStart(8, '0')} ` +
            `modifier=${framePayload.modifier} reason=${framePayload.reason} ` +
            `message=${framePayload.message}`);
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
            protocol: PROTOCOL_NAME,
            version: PROTOCOL_VERSION,
            clientName: 'gnome-display-helper',
            role: 'consumer',
            features: [
                'dmabuf-gdk-texture-v1',
                'dmabuf-caps-v3',
                'explicit-sync-fd-v1',
                'dmabuf-bind-failed-v1',
                'dmabuf-unbind-done-v1',
                'dmabuf-shadow-copy-v1',
                'pointer-events-v1',
                'media-state-v1',
                'audio-samples-v1',
            ],
        }));
    }

    _sendConsumerCaps() {
        this._queueFrame(encodeJsonFrame(REQ_CONSUMER_CAPS, {
            bufferImports: [{
                memoryType: 'dmabuf',
                renderer: 'gtk4-gdk-dmabuf-texture-builder',
                fourcc: ['XRGB8888', 'ARGB8888', 'XBGR8888', 'ABGR8888'],
                modifiers: true,
                relayModes: ['direct-import-v1', 'shadow-copy-v1'],
            }],
            dmabufCaps: buildDmaBufCaps(),
            explicitSync: true,
            pointerEvents: true,
            mediaState: true,
            audioSamples: {
                format: 'spectrum-f32-json',
                bands: WEB_AUDIO_FRAME_LENGTH,
                sampleRate: WEB_AUDIO_SAMPLE_RATE,
            },
        }));
    }

    _sendRegisterOutput(output) {
        log(`register monitor=${output.monitorIndex} logical=${output.logicalWidth}x${output.logicalHeight} ` +
            `physical=${output.physicalWidth}x${output.physicalHeight} scale=${output.scale} ` +
            `refresh=${output.refreshRate}`);
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
            const body = new Uint8Array(POINTER_MOTION_BODY_BYTES);
            const view = new DataView(body.buffer, body.byteOffset, body.byteLength);
            writeUint32LE(body, 0, outputId);
            view.setFloat64(4, x, true);
            view.setFloat64(12, y, true);
            writeUint64LE(body, 20, timeUsec);
            frame = encodeFrame(REQ_POINTER_MOTION, body);
        } else if (type === 'mousedown' || type === 'mouseup') {
            const body = new Uint8Array(POINTER_BUTTON_BODY_BYTES);
            const view = new DataView(body.buffer, body.byteOffset, body.byteLength);
            writeUint32LE(body, 0, outputId);
            view.setFloat64(4, x, true);
            view.setFloat64(12, y, true);
            writeUint32LE(body, 20, Number(event.button ?? 0));
            writeUint32LE(body, 24, type === 'mousedown'
                ? POINTER_BUTTON_PRESSED
                : POINTER_BUTTON_RELEASED);
            writeUint64LE(body, 28, timeUsec);
            frame = encodeFrame(REQ_POINTER_BUTTON, body);
        } else if (type === 'wheel') {
            const body = new Uint8Array(POINTER_AXIS_BODY_BYTES);
            const view = new DataView(body.buffer, body.byteOffset, body.byteLength);
            writeUint32LE(body, 0, outputId);
            view.setFloat64(4, x, true);
            view.setFloat64(12, y, true);
            view.setFloat64(20, Number(event.deltaX ?? 0), true);
            view.setFloat64(28, Number(event.deltaY ?? 0), true);
            writeUint32LE(body, 36, POINTER_AXIS_WHEEL);
            writeUint64LE(body, 40, timeUsec);
            frame = encodeFrame(REQ_POINTER_AXIS, body);
        }

        return frame ? this._queueFrame(frame) : false;
    }

    _queueFrame(bytes) {
        if (!this._output)
            return false;

        this._writeQueue.push(bytes);
        this._flushWriteQueue();
        return true;
    }

    _flushWriteQueue() {
        if (!this._output || this._writePending || this._writeQueue.length === 0)
            return;

        const bytes = this._writeQueue[0];
        this._writePending = true;
        this._output.write_all_async(bytes, GLib.PRIORITY_DEFAULT, this._cancellable, (stream, result) => {
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

            this._writePending = false;
            this._flushWriteQueue();
        });
    }

    _handleFrame(opcode, body, fdList) {
        try {
            switch (opcode) {
            case EVT_WELCOME:
                log(`welcome ${JSON.stringify(decodeJsonPayload(body))}`);
                break;
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
            case EVT_ERROR:
                log(`producer error ${JSON.stringify(decodeJsonPayload(body))}`);
                break;
            default:
                break;
            }
        } catch (error) {
            log(`failed to handle opcode=${opcode}: ${error}`);
        }
    }

    _handleOutputAccepted(payload) {
        const consumerOutputId = Number(payload.consumerOutputId);
        const outputId = Number(payload.outputId);
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
        const outputId = Number(payload.outputId);
        const output = this._outputsByBackendId.get(outputId);
        if (!output) {
            log(`BIND_BUFFERS for unknown output=${outputId}`);
            return;
        }

        output.bindBuffers(payload, decoded.text, fdList);
    }

    _handleSetConfig(payload) {
        const outputId = Number(payload.outputId);
        const output = this._outputsByBackendId.get(outputId);
        if (!output) {
            log(`SET_CONFIG for unknown output=${payload.outputId}`);
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
        const output = this._outputsByBackendId.get(Number(payload.outputId));
        if (!output) {
            log(`UNBIND for unknown output=${payload.outputId} generation=${payload.generation}`);
            return;
        }
        output.unbindGeneration(payload.generation);
        this._queueFrame(encodeJsonFrame(REQ_UNBIND_DONE, {
            outputId: Number(payload.outputId ?? 0),
            generation: Number(payload.generation ?? 0),
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

    const monitors = display.get_monitors();
    const outputs = [];
    for (let i = 0; i < monitors.get_n_items(); i++) {
        const monitor = monitors.get_item(i);
        if (monitor)
            outputs.push(new OutputWindow(application, monitor, i));
    }

    if (outputs.length === 0) {
        log('no GDK monitors available');
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
