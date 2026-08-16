#!/usr/bin/env python3
"""Exercise scene DMA-BUF export generations without a desktop consumer.

This is a focused companion to vivid_resource_soak.py.  It loads the built
scene producer module in this process, alternates two valid NVIDIA DRM
modifiers, closes every duplicated public buffer fd, and records RSS/FD/GPU FB
after each reconfiguration.  It directly covers the VulkanExSwapchain
generation-reclamation path that is otherwise reached only after a consumer
reports BIND_FAILED.
"""

from __future__ import annotations

import argparse
import ctypes
import json
import os
import pathlib
import subprocess
import time
from typing import Any


MAX_PLANES = 4
MAX_BUFFERS = 3
MAX_GPU_CAPS = 64
MAX_GPU_DEVICES = 16
ABGR8888 = 0x34324241
RELEASE_GATE_ABI_VERSION = 1
MEMORY_DEVICE_LOCAL = 2


class GpuDmaBufCap(ctypes.Structure):
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
        ("scene_dmabuf_n_caps", ctypes.c_uint32),
        ("scene_dmabuf_caps", GpuDmaBufCap * MAX_GPU_CAPS),
    ]


class GpuDeviceList(ctypes.Structure):
    _fields_ = [
        ("n_devices", ctypes.c_uint),
        ("devices", GpuDevice * MAX_GPU_DEVICES),
    ]


class ScenePlane(ctypes.Structure):
    _fields_ = [
        ("fd", ctypes.c_int),
        ("stride", ctypes.c_uint32),
        ("offset", ctypes.c_uint32),
    ]


class SceneBuffer(ctypes.Structure):
    _fields_ = [
        ("index", ctypes.c_uint32),
        ("size", ctypes.c_uint64),
        ("n_planes", ctypes.c_uint32),
        ("planes", ScenePlane * MAX_PLANES),
    ]


class SceneBufferSet(ctypes.Structure):
    _fields_ = [
        ("width", ctypes.c_uint32),
        ("height", ctypes.c_uint32),
        ("fourcc", ctypes.c_uint32),
        ("modifier", ctypes.c_uint64),
        ("premultiplied", ctypes.c_int),
        ("n_buffers", ctypes.c_uint32),
        ("buffers", SceneBuffer * MAX_BUFFERS),
    ]


class SceneRequest(ctypes.Structure):
    _fields_ = [
        ("fourcc", ctypes.c_uint32),
        ("modifier", ctypes.c_uint64),
        ("plane_count", ctypes.c_uint32),
        ("require_modifier", ctypes.c_int),
        ("memory_preference", ctypes.c_int),
    ]


WaitReleaseCallback = ctypes.CFUNCTYPE(
    ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32
)


class ReleaseGate(ctypes.Structure):
    _fields_ = [
        ("abi_version", ctypes.c_uint32),
        ("user_data", ctypes.c_void_p),
        ("wait_release", WaitReleaseCallback),
    ]


def proc_metrics() -> dict[str, float | int]:
    status: dict[str, int] = {}
    for line in pathlib.Path("/proc/self/status").read_text(encoding="utf-8").splitlines():
        key, _, value = line.partition(":")
        if key in {"VmRSS", "VmSwap", "Threads"}:
            status[key] = int(value.strip().split()[0])
    return {
        "rss_mb": round(status.get("VmRSS", 0) / 1024.0, 3),
        "swap_mb": round(status.get("VmSwap", 0) / 1024.0, 3),
        "fd_count": len(list(pathlib.Path("/proc/self/fd").iterdir())),
        "thread_count": status.get("Threads", 0),
    }


def gpu_fb_mb(pid: int) -> float | None:
    try:
        result = subprocess.run(
            ["nvidia-smi", "pmon", "-c", "1", "-s", "m"],
            check=False,
            capture_output=True,
            text=True,
            timeout=8,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    for line in result.stdout.splitlines():
        fields = line.split()
        if len(fields) >= 4 and fields[1].isdigit() and int(fields[1]) == pid:
            try:
                return float(fields[3])
            except ValueError:
                return None
    return None


def c_text(value: bytes) -> str:
    return value.split(b"\0", 1)[0].decode("utf-8", errors="replace")


def load_scene_library(path: pathlib.Path) -> ctypes.CDLL:
    library = ctypes.CDLL(str(path), mode=ctypes.RTLD_LOCAL)
    library.vivid_gpu_devices_enumerate.argtypes = [ctypes.POINTER(GpuDeviceList)]
    library.vivid_gpu_devices_enumerate.restype = ctypes.c_int
    library.vivid_scene_producer_new.argtypes = []
    library.vivid_scene_producer_new.restype = ctypes.c_void_p
    library.vivid_scene_producer_free.argtypes = [ctypes.c_void_p]
    library.vivid_scene_producer_configure.argtypes = [
        ctypes.c_void_p,
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_int,
        ctypes.c_double,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_char_p,
        ctypes.POINTER(GpuDevice),
    ]
    library.vivid_scene_producer_configure.restype = ctypes.c_int
    library.vivid_scene_producer_set_release_gate.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ReleaseGate),
    ]
    library.vivid_scene_producer_set_playing.argtypes = [ctypes.c_void_p, ctypes.c_int]
    library.vivid_scene_producer_prepare_buffers_with_request.argtypes = [
        ctypes.c_void_p,
        ctypes.c_uint32,
        ctypes.c_uint32,
        ctypes.c_double,
        ctypes.POINTER(SceneRequest),
        ctypes.POINTER(SceneBufferSet),
    ]
    library.vivid_scene_producer_prepare_buffers_with_request.restype = ctypes.c_int
    library.vivid_scene_producer_buffer_set_clear.argtypes = [ctypes.POINTER(SceneBufferSet)]
    return library


def choose_device_and_modifiers(library: ctypes.CDLL) -> tuple[GpuDevice, list[int]]:
    devices = GpuDeviceList()
    if not library.vivid_gpu_devices_enumerate(ctypes.byref(devices)) or devices.n_devices == 0:
        raise RuntimeError("no Vulkan GPU devices were enumerated")
    device = devices.devices[0]
    modifiers: list[int] = []
    for index in range(min(device.scene_dmabuf_n_caps, MAX_GPU_CAPS)):
        cap = device.scene_dmabuf_caps[index]
        if cap.fourcc == ABGR8888 and cap.plane_count == 1 and cap.modifier not in modifiers:
            modifiers.append(int(cap.modifier))
    non_linear = [modifier for modifier in modifiers if modifier != 0]
    if len(non_linear) >= 2:
        return device, non_linear[:2]
    if len(modifiers) >= 2:
        return device, modifiers[:2]
    raise RuntimeError(f"need two ABGR8888 export modifiers, found {modifiers}")


def prepare(
    library: ctypes.CDLL,
    producer: int,
    request: SceneRequest,
    width: int,
    height: int,
    scale: float,
) -> SceneBufferSet | None:
    buffer_set = SceneBufferSet()
    ok = library.vivid_scene_producer_prepare_buffers_with_request(
        producer,
        width,
        height,
        scale,
        ctypes.byref(request),
        ctypes.byref(buffer_set),
    )
    return buffer_set if ok else None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--library",
        type=pathlib.Path,
        default=pathlib.Path("producer/.build/direct-run/scene-build/out/libVividScene.so"),
    )
    parser.add_argument(
        "--project",
        default="/media/rikka/Data/steam/steamapps/workshop/content/431960/3308867900",
    )
    parser.add_argument("--iterations", type=int, default=20)
    parser.add_argument("--width", type=int, default=1920)
    parser.add_argument("--height", type=int, default=1080)
    parser.add_argument("--scale", type=float, default=1.0)
    parser.add_argument("--settle", type=float, default=0.5)
    parser.add_argument("--init-timeout", type=float, default=120.0)
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()
    if args.iterations < 1:
        parser.error("--iterations must be positive")

    library = load_scene_library(args.library.resolve())
    device, modifiers = choose_device_and_modifiers(library)
    producer = library.vivid_scene_producer_new()
    if not producer:
        raise RuntimeError("vivid_scene_producer_new returned NULL")

    @WaitReleaseCallback
    def release_ready(_user_data: int, _buffer_index: int, _timeout_ms: int) -> int:
        return 1

    gate = ReleaseGate(RELEASE_GATE_ABI_VERSION, None, release_ready)
    rows: list[dict[str, Any]] = []
    try:
        library.vivid_scene_producer_set_release_gate(producer, ctypes.byref(gate))
        if not library.vivid_scene_producer_configure(
            producer,
            os.fsencode(args.project),
            b"{}",
            1,
            0.0,
            1,
            30,
            1,
            bytes(device.render_node).split(b"\0", 1)[0],
            ctypes.byref(device),
        ):
            raise RuntimeError("scene producer configure failed")
        library.vivid_scene_producer_set_playing(producer, 1)

        requests = [
            SceneRequest(ABGR8888, modifier, 1, 1, MEMORY_DEVICE_LOCAL)
            for modifier in modifiers
        ]
        deadline = time.monotonic() + args.init_timeout
        initial: SceneBufferSet | None = None
        while time.monotonic() < deadline:
            initial = prepare(
                library, producer, requests[0], args.width, args.height, args.scale
            )
            if initial is not None:
                break
            time.sleep(0.1)
        if initial is None:
            raise RuntimeError("scene DMA-BUF swapchain did not become ready")
        library.vivid_scene_producer_buffer_set_clear(ctypes.byref(initial))

        rows.append(
            {
                "iteration": -1,
                "modifier": f"0x{modifiers[0]:016x}",
                **proc_metrics(),
                "gpu_fb_mb": gpu_fb_mb(os.getpid()),
            }
        )
        for iteration in range(args.iterations):
            request = requests[(iteration + 1) % len(requests)]
            buffer_set = prepare(
                library, producer, request, args.width, args.height, args.scale
            )
            if buffer_set is None:
                raise RuntimeError(
                    f"reconfigure failed at iteration {iteration} modifier=0x{request.modifier:016x}"
                )
            library.vivid_scene_producer_buffer_set_clear(ctypes.byref(buffer_set))
            time.sleep(args.settle)
            rows.append(
                {
                    "iteration": iteration,
                    "modifier": f"0x{request.modifier:016x}",
                    **proc_metrics(),
                    "gpu_fb_mb": gpu_fb_mb(os.getpid()),
                }
            )
    finally:
        library.vivid_scene_producer_set_release_gate(producer, None)
        library.vivid_scene_producer_free(producer)

    summary = {
        "pid": os.getpid(),
        "library": str(args.library.resolve()),
        "project": args.project,
        "device": c_text(bytes(device.name)),
        "render_node": c_text(bytes(device.render_node)),
        "modifiers": [f"0x{modifier:016x}" for modifier in modifiers],
        "iterations": args.iterations,
        "rows": rows,
        "delta": {
            key: round(float(rows[-1][key]) - float(rows[0][key]), 3)
            for key in ("rss_mb", "swap_mb", "fd_count", "thread_count", "gpu_fb_mb")
            if rows[0].get(key) is not None and rows[-1].get(key) is not None
        },
    }
    encoded = json.dumps(summary, ensure_ascii=False, indent=2)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded + "\n", encoding="utf-8")
    print(encoded)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
