#!/usr/bin/env python3
"""Offscreen capture of the Vivid scene renderer through the producer C ABI.

Two frame-loop modes:

  free-running (default)  The renderer's own timer drives draws; the script samples whatever
                          frame is current after each request and sleeps 1/fps. Frame indices
                          are capture indices, not renderer draws.
  --lockstep              The renderer is loaded with WESCENE_LOCKSTEP=1 so its timer never
                          runs; every draw is one explicit step from this script, the scene
                          clock advances by a fixed dt per step, random engines are seeded and
                          the calendar clock is pinned. Each captured step is exactly one draw
                          (verified through the frame's render_sequence), events are applied
                          through a two-thread flush barrier before the step they belong to,
                          and caches/localStorage live in a private directory. Two runs of the
                          same scenario produce byte-identical frames.

Scenario files (--scenario) pin the complete input vector as JSON; CLI flags override them:

  {
    "width": 1600, "height": 1000, "fps": 30, "steps": 120,
    "dt": 0.0333333, "seed": 1, "epoch": 1700000000,
    "content_fit": 1, "mute": true, "volume": 50,
    "reflections": true, "volumetrics": 2, "shadows": 2, "postprocessing": 1,
    "antialiasing": 1, "texture_resolution": 0,
    "user_properties": {"schemecolor": "1 1 1"},
    "media_state": null,
    "pointer": [0.5, 0.5],
    "audio": null,                       // "sweep" | "silence" | path to JSON frames
    "events": [
      {"step": 30, "pointer": [0.0, 0.5]},
      {"step": 40, "button": [1, true]}, {"step": 41, "button": [1, false]},
      {"step": 50, "properties": {"schemecolor": "1 0 0"}},
      {"step": 60, "media_state": {...}},
      {"step": 70, "resize": [800, 500]},
      {"step": 80, "pause_steps": 1},
      {"step": 90, "audio": "sweep"}
    ],
    "snapshots": [0, 59, 119],
    "trace_steps": [0, 59, 119]
  }

Step n renders the (n+1)-th draw, so its scene time is (n+1)*dt. Snapshot n is written next to
--output as <stem>-step<NNN><suffix>; the final step is written to --output itself. With
--lockstep a manifest (<output>.manifest.json) records the effective input vector, environment
fingerprints and per-frame hashes.
"""
import argparse
import ctypes
import hashlib
import json
import math
import mmap
import os
import pathlib
import select
import shutil
import subprocess
import tempfile
import time

from PIL import Image

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
RENDERER_SUBMODULE = REPO_ROOT / "producer/third_party/wallpaper-scene-renderer"
AUDIO_SAMPLE_COUNT = 128  # 64 left + 64 right spectrum bands, the renderer's ABI contract.
DEFAULT_FIXED_EPOCH = 1700000000.0  # 2023-11-14T22:13:20Z
DEFAULT_RANDOM_SEED = 1


MAX_PLANES = 4
MAX_BUFFERS = 3
MAX_CAPS = 64
MAX_DEVICES = 16

# DRM_FORMAT_ABGR8888. On a little-endian host its bytes are laid out as RGBA.
DRM_FORMAT_ABGR8888 = 0x34324241
DRM_FORMAT_MOD_LINEAR = 0
DMABUF_MEMORY_HOST_VISIBLE = 1
DMABUF_MEMORY_DEVICE_LOCAL = 2
RELEASE_GATE_ABI_VERSION = 1


class DmaBufFormatCap(ctypes.Structure):
    _fields_ = [
        ("fourcc", ctypes.c_uint32),
        ("modifier", ctypes.c_uint64),
        ("plane_count", ctypes.c_uint32),
    ]


class GpuDevice(ctypes.Structure):
    _fields_ = [
        ("render_node", ctypes.c_char * 64),
        ("name", ctypes.c_char * 256),
        ("pci_address", ctypes.c_char * 32),
        ("vendor_id", ctypes.c_uint32),
        ("drm_render_major", ctypes.c_uint32),
        ("drm_render_minor", ctypes.c_uint32),
        ("uuid", ctypes.c_uint8 * 16),
        ("driver_uuid", ctypes.c_uint8 * 16),
        ("is_discrete", ctypes.c_int),
        ("vulkan_driver_id", ctypes.c_uint32),
        ("scene_dmabuf_n_caps", ctypes.c_uint32),
        ("scene_dmabuf_caps", DmaBufFormatCap * MAX_CAPS),
    ]


class GpuDeviceList(ctypes.Structure):
    _fields_ = [
        ("n_devices", ctypes.c_uint),
        ("devices", GpuDevice * MAX_DEVICES),
    ]


class Plane(ctypes.Structure):
    _fields_ = [
        ("fd", ctypes.c_int),
        ("stride", ctypes.c_uint32),
        ("offset", ctypes.c_uint32),
    ]


class Buffer(ctypes.Structure):
    _fields_ = [
        ("index", ctypes.c_uint32),
        ("size", ctypes.c_uint64),
        ("n_planes", ctypes.c_uint32),
        ("planes", Plane * MAX_PLANES),
    ]


class BufferSet(ctypes.Structure):
    _fields_ = [
        ("width", ctypes.c_uint32),
        ("height", ctypes.c_uint32),
        ("fourcc", ctypes.c_uint32),
        ("modifier", ctypes.c_uint64),
        ("premultiplied", ctypes.c_int),
        ("n_buffers", ctypes.c_uint32),
        ("buffers", Buffer * MAX_BUFFERS),
    ]


class DmaBufRequest(ctypes.Structure):
    _fields_ = [
        ("fourcc", ctypes.c_uint32),
        ("modifier", ctypes.c_uint64),
        ("plane_count", ctypes.c_uint32),
        ("require_modifier", ctypes.c_int),
        ("memory_preference", ctypes.c_int),
    ]


class Frame(ctypes.Structure):
    _fields_ = [
        ("buffer_index", ctypes.c_uint32),
        ("source_frame_id", ctypes.c_int32),
        ("sequence", ctypes.c_uint64),
        ("target_time_usec", ctypes.c_uint64),
        ("acquire_sync_fd", ctypes.c_int),
        ("render_sequence", ctypes.c_uint64),
    ]


class LockstepProtocolError(RuntimeError):
    """The renderer did not honor the one-step-one-draw contract."""


WaitReleaseCallback = ctypes.CFUNCTYPE(
    ctypes.c_int,
    ctypes.c_void_p,
    ctypes.c_uint32,
    ctypes.c_uint32,
)


class ReleaseGate(ctypes.Structure):
    _fields_ = [
        ("abi_version", ctypes.c_uint32),
        ("user_data", ctypes.c_void_p),
        ("wait_release", WaitReleaseCallback),
    ]


def bind_abi(lib):
    lib.vivid_gpu_devices_enumerate.argtypes = [ctypes.POINTER(GpuDeviceList)]
    lib.vivid_gpu_devices_enumerate.restype = ctypes.c_int

    lib.vivid_scene_producer_new.restype = ctypes.c_void_p
    lib.vivid_scene_producer_free.argtypes = [ctypes.c_void_p]

    lib.vivid_scene_producer_configure.argtypes = [
        ctypes.c_void_p,
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_int,
        ctypes.c_double,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_char_p,
        ctypes.POINTER(GpuDevice),
    ]
    lib.vivid_scene_producer_configure.restype = ctypes.c_int

    lib.vivid_scene_producer_set_playing.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.vivid_scene_producer_set_pointer_motion.argtypes = [
        ctypes.c_void_p,
        ctypes.c_double,
        ctypes.c_double,
    ]
    lib.vivid_scene_producer_set_pointer_button.argtypes = [
        ctypes.c_void_p,
        ctypes.c_uint32,
        ctypes.c_int,
    ]
    lib.vivid_scene_producer_set_media_state_json.argtypes = [
        ctypes.c_void_p,
        ctypes.c_char_p,
    ]
    lib.vivid_scene_producer_set_release_gate.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ReleaseGate),
    ]
    lib.vivid_scene_producer_prepare_buffers_with_request.argtypes = [
        ctypes.c_void_p,
        ctypes.c_uint32,
        ctypes.c_uint32,
        ctypes.c_double,
        ctypes.POINTER(DmaBufRequest),
        ctypes.POINTER(BufferSet),
    ]
    lib.vivid_scene_producer_prepare_buffers_with_request.restype = ctypes.c_int
    lib.vivid_scene_producer_request_frame.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    lib.vivid_scene_producer_next_frame.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(Frame),
    ]
    lib.vivid_scene_producer_next_frame.restype = ctypes.c_int
    lib.vivid_scene_producer_buffer_set_clear.argtypes = [ctypes.POINTER(BufferSet)]
    lib.vivid_scene_producer_set_audio_samples.argtypes = [ctypes.c_void_p, ctypes.c_void_p]

    # The capture driver and producer share one ABI, including the stepped frame controls.
    for name, restype, argtypes in (
        ("vivid_scene_producer_step", ctypes.c_int, [ctypes.c_void_p]),
        ("vivid_scene_producer_flush", ctypes.c_int, [ctypes.c_void_p, ctypes.c_uint]),
        ("vivid_scene_producer_wait_scene_ready", ctypes.c_int, [ctypes.c_void_p, ctypes.c_uint]),
    ):
        function = getattr(lib, name)
        function.restype = restype
        function.argtypes = argtypes


def bind_glib():
    glib = ctypes.CDLL("libglib-2.0.so.0")
    glib.g_variant_new_fixed_array.argtypes = [
        ctypes.c_char_p,
        ctypes.c_void_p,
        ctypes.c_size_t,
        ctypes.c_size_t,
    ]
    glib.g_variant_new_fixed_array.restype = ctypes.c_void_p
    glib.g_variant_ref_sink.argtypes = [ctypes.c_void_p]
    glib.g_variant_ref_sink.restype = ctypes.c_void_p
    glib.g_variant_unref.argtypes = [ctypes.c_void_p]
    return glib


def audio_fixture_frame(fixture, step):
    """128 spectrum values (64 left + 64 right) for one step of a named or file fixture.

    "sweep" is a slow deterministic sine sweep with a per-band decay so low bands lead; it
    exercises spectrum uniforms, particle audio response and script audio buffers without any
    real capture device. A file fixture is a JSON array of 128-value frames cycled per step.
    """
    if fixture in (None, "", "silence"):
        return [0.0] * AUDIO_SAMPLE_COUNT
    if fixture == "sweep":
        phase = 2.0 * math.pi * (step / 120.0)
        left = [
            max(0.0, 0.5 + 0.5 * math.sin(phase + band * 0.35)) * math.exp(-band / 40.0)
            for band in range(64)
        ]
        right = [
            max(0.0, 0.5 + 0.5 * math.sin(phase + band * 0.35 + 0.7)) * math.exp(-band / 40.0)
            for band in range(64)
        ]
        return left + right
    frames = json.loads(pathlib.Path(fixture).read_text())
    if not isinstance(frames, list) or not frames:
        raise ValueError(f"audio fixture {fixture} must be a non-empty JSON array of frames")
    values = frames[step % len(frames)]
    if len(values) != AUDIO_SAMPLE_COUNT:
        raise ValueError(f"audio fixture frames must have {AUDIO_SAMPLE_COUNT} values")
    return [float(value) for value in values]


def push_audio_samples(lib, glib, producer, values):
    array = (ctypes.c_double * len(values))(*values)
    variant = glib.g_variant_ref_sink(
        glib.g_variant_new_fixed_array(b"d", array, len(values), ctypes.sizeof(ctypes.c_double))
    )
    try:
        lib.vivid_scene_producer_set_audio_samples(producer, variant)
    finally:
        glib.g_variant_unref(variant)


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def git_describe(path):
    try:
        head = subprocess.run(
            ["git", "-C", str(path), "rev-parse", "HEAD"], capture_output=True, text=True, check=True
        ).stdout.strip()
        dirty = subprocess.run(
            ["git", "-C", str(path), "status", "--porcelain", "--untracked-files=no"],
            capture_output=True,
            text=True,
            check=True,
        ).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return None
    return {"commit": head, "dirty": bool(dirty)}


def fontconfig_fingerprint():
    try:
        listing = subprocess.run(["fc-list"], capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return None
    return hashlib.sha256("\n".join(sorted(listing.splitlines())).encode()).hexdigest()


def device_render_node(device):
    return bytes(device.render_node).split(b"\0", 1)[0]


def choose_gpu(devices, render_node=None):
    if render_node and render_node != "auto":
        wanted = os.fsencode(render_node)
        for index in range(devices.n_devices):
            if device_render_node(devices.devices[index]) == wanted:
                return index
        available = ", ".join(
            os.fsdecode(device_render_node(devices.devices[index]))
            for index in range(devices.n_devices)
        )
        raise RuntimeError(
            f"render node {render_node} is not a usable Vulkan device (available: {available})"
        )
    for index in range(devices.n_devices):
        if devices.devices[index].is_discrete:
            return index
    return 0


def load_producer_capture_settings(config_path, display_key, project):
    """Resolve the scene inputs stored by the producer for one display/project."""
    root = json.loads(pathlib.Path(config_path).read_text())
    global_config = root.get("global", root)
    display = global_config["per-output-projects"][display_key]
    project = project or display["project-path"]

    # Match producer_apply_config_to_global_renderer / producer_render_route_apply_config:
    # graphics, audio gain and GPU selection come from global configuration; the display
    # can override content-fit and mute. Saved user properties are keyed by the requested
    # project path, so a comparison capture must not borrow the currently selected
    # wallpaper's properties when --project names another wallpaper.
    config_keys = {
        "render_node": "render-device",
        "content_fit": "content-fit",
        "fps": "scene-fps",
        "mute": "mute",
        "volume": "volume",
        "reflections": "gfx-reflections",
        "volumetrics": "gfx-volumetrics",
        "shadows": "gfx-shadows",
        "postprocessing": "gfx-postprocessing",
        "antialiasing": "gfx-antialiasing",
        "texture_resolution": "gfx-texture-resolution",
    }
    settings = {
        argument: global_config[key]
        for argument, key in config_keys.items()
        if key in global_config
    }
    settings["project"] = project
    for argument, key in (("content_fit", "content-fit"), ("mute", "mute")):
        if key in display:
            settings[argument] = display[key]
    saved_project = display.get("saved-projects", {}).get(project, {})
    return settings, dict(saved_project.get("user-properties", {}))


def wait_for_buffers(lib, producer, request, buffers, width, height):
    deadline = time.monotonic() + 120.0
    while time.monotonic() < deadline:
        if lib.vivid_scene_producer_prepare_buffers_with_request(
            producer,
            width,
            height,
            1.0,
            ctypes.byref(request),
            ctypes.byref(buffers),
        ):
            return
        time.sleep(0.05)
    raise RuntimeError("timed out while preparing the scene DMA-BUF set")


def wait_for_frame(lib, producer, frame):
    deadline = time.monotonic() + 30.0
    lib.vivid_scene_producer_request_frame(producer, b"python-capture")
    while time.monotonic() < deadline:
        if lib.vivid_scene_producer_next_frame(producer, ctypes.byref(frame)):
            return
        time.sleep(0.01)
    raise RuntimeError("timed out while waiting for a scene frame")


def lockstep_flush(lib, producer, timeout_ms=10_000):
    if not lib.vivid_scene_producer_flush(producer, timeout_ms):
        raise LockstepProtocolError("renderer flush barrier timed out")


def lockstep_step(lib, producer, frame, expected_render_sequence):
    """Post exactly one draw and return its frame, proving no draw was skipped or duplicated."""
    if not lib.vivid_scene_producer_step(producer):
        raise LockstepProtocolError(
            "step rejected: a draw was still outstanding, so the renderer is not lockstep-idle"
        )
    deadline = time.monotonic() + 30.0
    while time.monotonic() < deadline:
        if lib.vivid_scene_producer_next_frame(producer, ctypes.byref(frame)):
            if frame.render_sequence != expected_render_sequence:
                raise LockstepProtocolError(
                    f"expected render_sequence={expected_render_sequence} but received "
                    f"{frame.render_sequence}; a draw was skipped or published unrequested"
                )
            return
        time.sleep(0.001)
    raise LockstepProtocolError(
        f"no frame published for render_sequence={expected_render_sequence} within 30s "
        "(the draw returned without publishing, or the scene is not loaded)"
    )


def wait_for_acquire_fence(frame):
    if frame.acquire_sync_fd < 0:
        return

    acquire_sync_fd = frame.acquire_sync_fd
    try:
        poller = select.poll()
        poller.register(acquire_sync_fd, select.POLLIN)
        if not poller.poll(10_000):
            raise RuntimeError("timed out while waiting for the acquire fence")
    finally:
        os.close(acquire_sync_fd)
        frame.acquire_sync_fd = -1


def save_frame(buffers, frame, output):
    selected = None
    for index in range(buffers.n_buffers):
        candidate = buffers.buffers[index]
        if candidate.index == frame.buffer_index:
            selected = candidate
            break

    if selected is None:
        raise RuntimeError(f"unknown buffer index {frame.buffer_index}")
    if selected.n_planes != 1:
        raise RuntimeError(f"expected one plane, received {selected.n_planes}")

    plane = selected.planes[0]
    mapping = mmap.mmap(
        plane.fd,
        selected.size,
        flags=mmap.MAP_SHARED,
        prot=mmap.PROT_READ,
    )
    try:
        byte_count = plane.stride * buffers.height
        pixels = mapping[plane.offset : plane.offset + byte_count]
        image = Image.frombytes(
            "RGBA",
            (buffers.width, buffers.height),
            pixels,
            "raw",
            "RGBA",
            plane.stride,
            1,
        )
        image.save(output)
    finally:
        mapping.close()

    return plane.stride


SCENARIO_SETTING_KEYS = (
    "width",
    "height",
    "fps",
    "content_fit",
    "mute",
    "volume",
    "reflections",
    "volumetrics",
    "shadows",
    "postprocessing",
    "antialiasing",
    "texture_resolution",
)


def parse_step_spec(parser, text, option, expected_parts, step_limit):
    parts = text.split(":")
    if len(parts) != expected_parts:
        parser.error(f"{option} expects {expected_parts - 1} ':'-separated values after STEP")
    try:
        step = int(parts[0])
    except ValueError:
        parser.error(f"{option} STEP must be an integer")
    if not 0 <= step < step_limit:
        parser.error(f"{option} STEP must be within the captured step range")
    return step, parts[1:]


def load_scenario(parser, path):
    try:
        scenario = json.loads(pathlib.Path(path).read_text())
    except (OSError, json.JSONDecodeError) as error:
        parser.error(f"--scenario: {error}")
    if not isinstance(scenario, dict):
        parser.error("--scenario must contain a JSON object")
    # Keep media fixtures portable with the scenario that owns them. Resolve both startup
    # artwork and later replacement events before recording the effective input vector;
    # a missing fixture must fail instead of silently exercising the no-artwork state.
    media_states = [scenario.get("media_state")]
    media_states.extend(event.get("media_state") for event in scenario.get("events", [])
                        if isinstance(event, dict))
    for state in media_states:
        if not isinstance(state, dict) or not state.get("thumbnailPath"):
            continue
        if not isinstance(state["thumbnailPath"], str):
            parser.error("scenario thumbnailPath must be a string")
        thumbnail = pathlib.Path(state["thumbnailPath"])
        if not thumbnail.is_absolute():
            thumbnail = pathlib.Path(path).resolve().parent / thumbnail
        if not thumbnail.is_file():
            parser.error(f"scenario media thumbnail not found: {thumbnail}")
        state["thumbnailPath"] = str(thumbnail.resolve())
    return scenario


def build_parser():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--library", required=True)
    parser.add_argument(
        "--project",
        help="Project directory; defaults to the selected display's project.",
    )
    parser.add_argument(
        "--config",
        help="Producer config-v1.json supplying scene settings and saved properties.",
    )
    parser.add_argument("--display-key", help="Display entry to use from --config.")
    parser.add_argument(
        "--scenario",
        help="JSON scenario pinning the input vector (see module docstring); CLI flags override.",
    )
    parser.add_argument("--output", required=True)
    parser.add_argument("--width", type=int, default=1600)
    parser.add_argument("--height", type=int, default=1000)
    parser.add_argument("--frames", type=int, default=90, help="Number of steps (draws) to run.")
    parser.add_argument("--fps", type=int, default=30)
    parser.add_argument("--content-fit", type=int, choices=(1, 2, 3), default=1)
    parser.add_argument("--mute", action=argparse.BooleanOptionalAction, default=False)
    parser.add_argument("--volume", type=int, choices=range(101), default=50, metavar="0..100")
    parser.add_argument(
        "--reflections",
        action=argparse.BooleanOptionalAction,
        default=True,
    )
    parser.add_argument("--volumetrics", type=int, choices=range(5), default=2)
    parser.add_argument("--shadows", type=int, choices=range(5), default=2)
    parser.add_argument("--postprocessing", type=int, choices=range(4), default=1)
    parser.add_argument("--antialiasing", type=int, choices=range(4), default=1)
    parser.add_argument("--texture-resolution", type=int, choices=range(3), default=0)
    parser.add_argument("--pointer-x", type=float, default=0.5)
    parser.add_argument("--pointer-y", type=float, default=0.5)
    parser.add_argument(
        "--pointer-at",
        action="append",
        default=[],
        metavar="STEP:X:Y",
        help="Move the pointer to normalized X/Y before this step; repeat for a path.",
    )
    parser.add_argument(
        "--click-frame",
        type=int,
        default=-1,
        help="Press a pointer button before this step; negative disables click injection.",
    )
    parser.add_argument(
        "--click-hold-frames",
        type=int,
        default=1,
        help="Number of steps to keep the injected pointer button pressed.",
    )
    parser.add_argument(
        "--click-button",
        type=int,
        default=1,
        help="Pointer button id to inject (1 is the left button).",
    )
    parser.add_argument(
        "--snapshot",
        action="append",
        default=[],
        metavar="STEP:PATH",
        help="Save an additional step; may be supplied more than once.",
    )
    parser.add_argument("--properties", default="{}")
    parser.add_argument(
        "--properties-at",
        action="append",
        default=[],
        metavar="STEP:JSON",
        help="Merge user properties into the running producer before this step.",
    )
    parser.add_argument("--media-state", default="")
    parser.add_argument(
        "--resize-at",
        action="append",
        default=[],
        metavar="STEP:WxH",
        help="Reconfigure the exported buffers to a new size before this step.",
    )
    parser.add_argument(
        "--pause-at",
        action="append",
        default=[],
        metavar="STEP",
        help="Pause and resume playback before this step (exercises the pause path).",
    )
    parser.add_argument(
        "--audio",
        default=None,
        help="Audio spectrum fixture fed every step: 'sweep', 'silence' or a JSON frames file.",
    )
    parser.add_argument(
        "--render-node",
        default=os.environ.get("VIVID_CAPTURE_RENDER_NODE", ""),
        help="DRM render node to render on (default: first discrete GPU, else device 0).",
    )
    parser.add_argument(
        "--modifier",
        type=lambda text: int(text, 0),
        default=DRM_FORMAT_MOD_LINEAR,
        help="DRM format modifier to request for the exported buffers (default: LINEAR). "
        "Non-linear captures save the raw tiled bytes; use them to check that frames "
        "are produced, not for pixel comparison.",
    )
    parser.add_argument(
        "--memory",
        choices=("host-visible", "device-local"),
        default="host-visible",
        help="Memory preference for the exported buffers (default: host-visible).",
    )

    lockstep = parser.add_argument_group("lockstep (deterministic) capture")
    lockstep.add_argument(
        "--lockstep",
        action=argparse.BooleanOptionalAction,
        default=False,
        help="Drive every draw from this script with a fixed scene step (see module docstring).",
    )
    lockstep.add_argument(
        "--fixed-dt",
        type=float,
        default=None,
        help="Scene seconds per step in lockstep mode (default: 1/fps).",
    )
    lockstep.add_argument("--seed", type=int, default=None, help="Random seed (default: 1).")
    lockstep.add_argument(
        "--epoch",
        type=float,
        default=None,
        help="Unix time of scene time zero for clocks (default: 1700000000).",
    )
    lockstep.add_argument(
        "--cache-dir",
        default=None,
        help="Cache/localStorage root for this run (default: a fresh temporary directory).",
    )
    lockstep.add_argument(
        "--keep-cache",
        action="store_true",
        help="Do not delete the temporary cache directory when the capture finishes.",
    )
    lockstep.add_argument(
        "--manifest",
        default=None,
        help="Manifest path (default: <output>.manifest.json in lockstep mode).",
    )
    lockstep.add_argument(
        "--trace-dir",
        default=None,
        help="Directory for renderer frame-trace dumps (WESCENE_DUMP_FRAME_TRACE).",
    )
    lockstep.add_argument(
        "--trace-steps",
        default=None,
        help="Comma-separated steps to trace (default: the snapshot steps).",
    )
    lockstep.add_argument(
        "--vk-validation",
        action="store_true",
        help="Enable the Vulkan validation layer in the renderer (WESCENE_VK_VALIDATION).",
    )
    return parser


def main():
    parser = build_parser()
    args = parser.parse_args()

    scenario = load_scenario(parser, args.scenario) if args.scenario else {}
    # Precedence: producer config < scenario < explicit CLI flags. argparse cannot tell an
    # explicit flag from a default, so defaults are re-installed from the lower layers and the
    # command line is parsed again on top of them.
    layered_defaults = {}
    saved_properties = {}
    if args.config:
        if not args.display_key:
            parser.error("--config requires --display-key")
        settings, saved_properties = load_producer_capture_settings(
            args.config, args.display_key, args.project
        )
        layered_defaults.update(settings)
    elif args.display_key:
        parser.error("--display-key requires --config")
    for key in SCENARIO_SETTING_KEYS:
        if key in scenario:
            layered_defaults[key] = scenario[key]
    if "steps" in scenario:
        layered_defaults["frames"] = int(scenario["steps"])
    for key in ("dt", "seed", "epoch", "audio"):
        if key in scenario and scenario[key] is not None:
            layered_defaults["fixed_dt" if key == "dt" else key] = scenario[key]
    if "project" in scenario and not args.project:
        layered_defaults["project"] = scenario["project"]
    if layered_defaults:
        parser.set_defaults(**layered_defaults)
        args = parser.parse_args()
    if not args.project:
        parser.error("provide --project, a scenario 'project', or --config with --display-key")

    scenario_properties = scenario.get("user_properties", {})
    if not isinstance(scenario_properties, dict):
        parser.error("scenario 'user_properties' must be an object")
    saved_properties.update(scenario_properties)
    property_overrides = json.loads(args.properties)
    if not isinstance(property_overrides, dict):
        parser.error("--properties must be a JSON object")
    saved_properties.update(property_overrides)
    args.properties = json.dumps(saved_properties, ensure_ascii=False)

    if args.fps <= 0:
        parser.error("--fps must be greater than zero")
    if args.click_frame >= 0 and args.click_hold_frames <= 0:
        parser.error("--click-hold-frames must be greater than zero")
    step_count = max(1, args.frames)

    if not args.media_state and scenario.get("media_state"):
        args.media_state = json.dumps(scenario["media_state"], ensure_ascii=False)
    if "pointer" in scenario and scenario["pointer"] is not None:
        pointer = scenario["pointer"]
        if not (isinstance(pointer, list) and len(pointer) == 2):
            parser.error("scenario 'pointer' must be [x, y]")
        # Explicit --pointer-x/--pointer-y still win; argparse defaults are 0.5.
        if args.pointer_x == 0.5 and args.pointer_y == 0.5:
            args.pointer_x, args.pointer_y = float(pointer[0]), float(pointer[1])

    # Events keyed by step. Each entry is a list of (kind, payload) applied in order before the
    # step's draw. Scenario events come first, then CLI events.
    events = {}

    def add_event(step, kind, payload):
        if not 0 <= step < step_count:
            parser.error(f"event '{kind}' at step {step} is outside the captured step range")
        events.setdefault(step, []).append((kind, payload))

    for event in scenario.get("events", []):
        if not isinstance(event, dict) or "step" not in event:
            parser.error("scenario events must be objects with a 'step'")
        step = int(event["step"])
        for kind in ("pointer", "button", "properties", "media_state", "resize", "pause_steps", "audio"):
            if kind in event:
                add_event(step, kind, event[kind])

    for spec in args.pointer_at:
        step, (x_text, y_text) = parse_step_spec(parser, spec, "--pointer-at", 3, step_count)
        try:
            position = [float(x_text), float(y_text)]
        except ValueError:
            parser.error("--pointer-at expects numeric X/Y")
        if not all(0.0 <= coordinate <= 1.0 for coordinate in position):
            parser.error("--pointer-at X/Y must be finite normalized coordinates in [0, 1]")
        add_event(step, "pointer", position)
    for spec in args.properties_at:
        step_text, separator, values_text = spec.partition(":")
        if not separator:
            parser.error("--properties-at expects STEP:JSON")
        try:
            step = int(step_text)
            values = json.loads(values_text)
        except (ValueError, json.JSONDecodeError):
            parser.error("--properties-at expects an integer step and valid JSON")
        if not isinstance(values, dict):
            parser.error("--properties-at JSON must be an object")
        add_event(step, "properties", values)
    for spec in args.resize_at:
        step, (size_text,) = parse_step_spec(parser, spec, "--resize-at", 2, step_count)
        try:
            width_text, height_text = size_text.lower().split("x")
            size = [int(width_text), int(height_text)]
        except ValueError:
            parser.error("--resize-at expects STEP:WxH")
        add_event(step, "resize", size)
    for spec in args.pause_at:
        try:
            step = int(spec)
        except ValueError:
            parser.error("--pause-at expects an integer STEP")
        add_event(step, "pause_steps", 1)
    if args.click_frame >= 0:
        add_event(args.click_frame, "button", [args.click_button, True])
        release_step = args.click_frame + args.click_hold_frames
        if release_step < step_count:
            add_event(release_step, "button", [args.click_button, False])

    output_path = pathlib.Path(args.output)
    snapshot_outputs = {}
    for step in scenario.get("snapshots", []):
        step = int(step)
        if not 0 <= step < step_count:
            parser.error("scenario snapshots must be within the captured step range")
        snapshot_outputs[step] = str(
            output_path.with_name(f"{output_path.stem}-step{step:03d}{output_path.suffix}")
        )
    for snapshot_spec in args.snapshot:
        step_text, separator, snapshot_path = snapshot_spec.partition(":")
        if not separator or not step_text or not snapshot_path:
            parser.error("--snapshot expects STEP:PATH")
        try:
            snapshot_step = int(step_text)
        except ValueError:
            parser.error("--snapshot STEP must be an integer")
        if snapshot_step < 0:
            parser.error("--snapshot STEP must not be negative")
        snapshot_outputs[snapshot_step] = snapshot_path

    trace_steps = None
    if args.trace_steps:
        trace_steps = sorted({int(text) for text in args.trace_steps.split(",") if text.strip()})
    elif scenario.get("trace_steps"):
        trace_steps = sorted({int(step) for step in scenario["trace_steps"]})
    elif args.trace_dir:
        trace_steps = sorted(set(snapshot_outputs) | {step_count - 1})

    # --- Environment for the renderer. Everything must be in place before the DSO is loaded:
    # the renderer reads these variables at first use and GLib resolves XDG_CACHE_HOME once.
    fixed_dt = args.fixed_dt if args.fixed_dt else 1.0 / args.fps
    seed = args.seed if args.seed is not None else DEFAULT_RANDOM_SEED
    epoch = args.epoch if args.epoch is not None else DEFAULT_FIXED_EPOCH
    temp_cache_dir = None
    if args.lockstep:
        os.environ["WESCENE_LOCKSTEP"] = "1"
        os.environ["WESCENE_FIXED_DT"] = repr(fixed_dt)
        os.environ["WESCENE_RANDOM_SEED"] = str(seed)
        os.environ["WESCENE_FIXED_EPOCH"] = repr(epoch)
        os.environ["TZ"] = "UTC"
        os.environ["LC_ALL"] = "C.UTF-8"
        os.environ["LANG"] = "C.UTF-8"
        if args.cache_dir:
            cache_dir = pathlib.Path(args.cache_dir)
            cache_dir.mkdir(parents=True, exist_ok=True)
        else:
            temp_cache_dir = tempfile.mkdtemp(prefix="vivid-capture-cache-")
            cache_dir = pathlib.Path(temp_cache_dir)
        os.environ["XDG_CACHE_HOME"] = str(cache_dir)
    if args.trace_dir:
        pathlib.Path(args.trace_dir).mkdir(parents=True, exist_ok=True)
        os.environ["WESCENE_DUMP_FRAME_TRACE"] = str(pathlib.Path(args.trace_dir).resolve())
        if trace_steps:
            # The renderer counts draws from one; steps count from zero.
            os.environ["WESCENE_DUMP_FRAME_TRACE_DRAWS"] = ",".join(
                str(step + 1) for step in trace_steps
            )
    if args.vk_validation:
        os.environ["WESCENE_VK_VALIDATION"] = "1"

    effective_scenario = {
        "project": args.project,
        "width": args.width,
        "height": args.height,
        "fps": args.fps,
        "steps": step_count,
        "dt": fixed_dt if args.lockstep else None,
        "seed": seed if args.lockstep else None,
        "epoch": epoch if args.lockstep else None,
        "content_fit": args.content_fit,
        "mute": args.mute,
        "volume": args.volume,
        "reflections": args.reflections,
        "volumetrics": args.volumetrics,
        "shadows": args.shadows,
        "postprocessing": args.postprocessing,
        "antialiasing": args.antialiasing,
        "texture_resolution": args.texture_resolution,
        "user_properties": saved_properties,
        "media_state": json.loads(args.media_state) if args.media_state else None,
        "pointer": [args.pointer_x, args.pointer_y],
        "audio": args.audio,
        "events": [
            {"step": step, kind: payload}
            for step in sorted(events)
            for kind, payload in events[step]
        ],
        "snapshots": sorted(snapshot_outputs),
        "trace_steps": trace_steps or [],
        "modifier": args.modifier,
        "memory": args.memory,
    }
    print(
        "capture configuration: "
        + json.dumps(
            {
                "config": str(pathlib.Path(args.config).resolve()) if args.config else None,
                "display-key": args.display_key,
                "scenario-file": args.scenario,
                "lockstep": args.lockstep,
                "render-device": args.render_node or "auto",
                **effective_scenario,
            },
            ensure_ascii=False,
        ),
        flush=True,
    )

    library_path = pathlib.Path(args.library).resolve()
    lib = ctypes.CDLL(str(library_path), mode=ctypes.RTLD_LOCAL)
    bind_abi(lib)
    glib = bind_glib() if args.audio or any(
        kind == "audio" for step_events in events.values() for kind, _ in step_events
    ) else None

    devices = GpuDeviceList()
    if not lib.vivid_gpu_devices_enumerate(ctypes.byref(devices)) or not devices.n_devices:
        raise RuntimeError("no usable Vulkan render device was found")

    device_index = choose_gpu(devices, args.render_node)
    device = devices.devices[device_index]
    render_node = device_render_node(device)
    device_name = bytes(device.name).split(b"\0", 1)[0].decode(errors="replace")
    print(f"rendering on {os.fsdecode(render_node)} ({device_name})", flush=True)

    producer = lib.vivid_scene_producer_new()
    if not producer:
        raise RuntimeError("failed to create VividSceneProducer")

    @WaitReleaseCallback
    def release_ready(_user_data, _buffer_index, _timeout_ms):
        # The standalone capture owns no downstream consumer. It consumes each acquire fence
        # before requesting another frame, and no external process retains a producer slot.
        return 1

    release_gate = ReleaseGate(RELEASE_GATE_ABI_VERSION, None, release_ready)
    buffers = BufferSet()
    output_size = [args.width, args.height]
    pointer = [args.pointer_x, args.pointer_y]
    audio_fixture = args.audio
    manifest_frames = []

    def configure_current_properties():
        # The same ABI configuration supplies initial playback and live property changes.
        # Preserve every effective producer setting and the complete accumulated property
        # map; changing only its values must not create a new producer or reload the project.
        if not lib.vivid_scene_producer_configure(
            producer,
            os.fsencode(args.project),
            os.fsencode(json.dumps(saved_properties, ensure_ascii=False)),
            args.mute,
            args.volume / 100.0,
            args.content_fit,
            args.fps,
            args.reflections,
            args.volumetrics,
            args.shadows,
            args.postprocessing,
            args.antialiasing,
            args.texture_resolution,
            render_node,
            ctypes.byref(device),
        ):
            raise RuntimeError("failed to configure the scene producer")

    def send_pointer():
        # The input ABI receives output-space pixels and needs the current output dimensions.
        lib.vivid_scene_producer_set_pointer_motion(
            producer, pointer[0] * output_size[0], pointer[1] * output_size[1]
        )

    def make_request():
        return DmaBufRequest(
            DRM_FORMAT_ABGR8888,
            args.modifier,
            1,
            1,
            DMABUF_MEMORY_DEVICE_LOCAL
            if args.memory == "device-local"
            else DMABUF_MEMORY_HOST_VISIBLE,
        )

    def record_frame(step, frame, path):
        stride = save_frame(buffers, frame, path)
        entry = {
            "step": step,
            "render_sequence": frame.render_sequence,
            "sequence": frame.sequence,
            "scene_time": (step + 1) * fixed_dt if args.lockstep else None,
            "path": str(pathlib.Path(path).resolve()),
            "sha256": sha256_file(path),
            "size": list(output_size),
        }
        manifest_frames.append(entry)
        print(
            f"saved {path} step={step} render-sequence={frame.render_sequence} "
            f"sequence={frame.sequence} source-frame-id={frame.source_frame_id} "
            f"buffer={frame.buffer_index} stride={stride} sha256={entry['sha256'][:16]}",
            flush=True,
        )

    def apply_event(step, kind, payload):
        nonlocal buffers, audio_fixture
        if kind == "pointer":
            pointer[0], pointer[1] = float(payload[0]), float(payload[1])
            send_pointer()
            print(f"event step={step} pointer x={pointer[0]:.9f} y={pointer[1]:.9f}", flush=True)
        elif kind == "button":
            button, pressed = int(payload[0]), bool(payload[1])
            lib.vivid_scene_producer_set_pointer_button(producer, button, 1 if pressed else 0)
            print(f"event step={step} button={button} pressed={pressed}", flush=True)
        elif kind == "properties":
            saved_properties.update(payload)
            configure_current_properties()
            print(f"event step={step} properties " + json.dumps(payload, ensure_ascii=False), flush=True)
        elif kind == "media_state":
            lib.vivid_scene_producer_set_media_state_json(
                producer, os.fsencode(json.dumps(payload, ensure_ascii=False))
            )
            print(f"event step={step} media-state applied", flush=True)
        elif kind == "resize":
            new_width, new_height = int(payload[0]), int(payload[1])
            previous = buffers
            buffers = BufferSet()
            wait_for_buffers(lib, producer, make_request(), buffers, new_width, new_height)
            if previous.n_buffers:
                lib.vivid_scene_producer_buffer_set_clear(ctypes.byref(previous))
            output_size[0], output_size[1] = new_width, new_height
            send_pointer()
            print(f"event step={step} resize {new_width}x{new_height}", flush=True)
        elif kind == "pause_steps":
            # Pause and resume around a short real-time hold. No draw happens while paused, so
            # scene time does not advance; the pause/resume code paths (video, sound) still run.
            lib.vivid_scene_producer_set_playing(producer, 0)
            if args.lockstep:
                lockstep_flush(lib, producer)
            time.sleep(0.05)
            lib.vivid_scene_producer_set_playing(producer, 1)
            print(f"event step={step} pause/resume", flush=True)
        elif kind == "audio":
            audio_fixture = payload
            print(f"event step={step} audio fixture={payload}", flush=True)
        else:
            raise RuntimeError(f"unknown event kind {kind}")

    try:
        lib.vivid_scene_producer_set_release_gate(producer, ctypes.byref(release_gate))
        configure_current_properties()

        lib.vivid_scene_producer_set_playing(producer, 1)
        if args.media_state:
            lib.vivid_scene_producer_set_media_state_json(
                producer,
                os.fsencode(args.media_state),
            )

        wait_for_buffers(lib, producer, make_request(), buffers, args.width, args.height)

        if args.lockstep:
            # prepare_buffers only proves the swapchain exists. Wait for the parsed scene to be
            # installed so step 0 renders content at scene time dt and not an empty draw.
            if not lib.vivid_scene_producer_wait_scene_ready(producer, 180_000):
                raise LockstepProtocolError("scene was not installed within 180s")
            # Nothing may have been drawn yet: an implicit startup draw would shift every step's
            # scene time by one dt and leave an unread frame for the latest-wins ring to drop.
            probe = Frame()
            if lib.vivid_scene_producer_next_frame(producer, ctypes.byref(probe)):
                raise LockstepProtocolError(
                    f"renderer published render_sequence={probe.render_sequence} before the first step"
                )

        send_pointer()

        frame = Frame()
        for step in range(step_count):
            for kind, payload in events.get(step, []):
                apply_event(step, kind, payload)
            if glib is not None:
                push_audio_samples(lib, glib, producer, audio_fixture_frame(audio_fixture, step))
            if args.lockstep:
                # Everything applied above must reach both renderer threads before the draw.
                lockstep_flush(lib, producer)
                lockstep_step(lib, producer, frame, step + 1)
            else:
                wait_for_frame(lib, producer, frame)
            wait_for_acquire_fence(frame)
            if step in snapshot_outputs and step != step_count - 1:
                record_frame(step, frame, snapshot_outputs[step])
            if not args.lockstep and step + 1 < step_count:
                time.sleep(1.0 / args.fps)

        final_path = snapshot_outputs.get(step_count - 1, args.output)
        record_frame(step_count - 1, frame, final_path)
        if final_path != args.output:
            shutil.copyfile(final_path, args.output)

        if args.lockstep or args.manifest:
            manifest_path = pathlib.Path(args.manifest or f"{args.output}.manifest.json")
            manifest = {
                "schema": 1,
                "lockstep": args.lockstep,
                "library": {"path": str(library_path), "sha256": sha256_file(library_path)},
                "renderer": git_describe(RENDERER_SUBMODULE),
                "repository": git_describe(REPO_ROOT),
                "gpu": {
                    "name": device_name,
                    "render_node": os.fsdecode(render_node),
                    "vendor_id": device.vendor_id,
                    "vulkan_driver_id": device.vulkan_driver_id,
                    "driver_uuid": bytes(device.driver_uuid).hex(),
                },
                "environment": {
                    key: os.environ.get(key)
                    for key in (
                        "WESCENE_LOCKSTEP",
                        "WESCENE_FIXED_DT",
                        "WESCENE_RANDOM_SEED",
                        "WESCENE_FIXED_EPOCH",
                        "WESCENE_DUMP_FRAME_TRACE",
                        "WESCENE_DUMP_FRAME_TRACE_DRAWS",
                        "WESCENE_VK_VALIDATION",
                        "TZ",
                        "LC_ALL",
                        "XDG_CACHE_HOME",
                    )
                },
                "fontconfig_sha256": fontconfig_fingerprint(),
                "scenario": effective_scenario,
                "scenario_sha256": hashlib.sha256(
                    json.dumps(effective_scenario, sort_keys=True, ensure_ascii=False).encode()
                ).hexdigest(),
                "frames": manifest_frames,
            }
            manifest_path.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n")
            print(f"manifest written to {manifest_path}", flush=True)
    finally:
        lib.vivid_scene_producer_set_release_gate(producer, None)
        if buffers.n_buffers:
            lib.vivid_scene_producer_buffer_set_clear(ctypes.byref(buffers))
        lib.vivid_scene_producer_free(producer)
        if temp_cache_dir and not args.keep_cache:
            shutil.rmtree(temp_cache_dir, ignore_errors=True)


if __name__ == "__main__":
    main()
