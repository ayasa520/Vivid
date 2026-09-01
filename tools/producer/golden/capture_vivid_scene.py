#!/usr/bin/env python3
import argparse
import ctypes
import mmap
import os
import pathlib
import select
import time

from PIL import Image


MAX_PLANES = 4
MAX_BUFFERS = 3
MAX_CAPS = 64
MAX_DEVICES = 16

# DRM_FORMAT_ABGR8888. On a little-endian host its bytes are laid out as RGBA.
DRM_FORMAT_ABGR8888 = 0x34324241
DRM_FORMAT_MOD_LINEAR = 0
DMABUF_MEMORY_HOST_VISIBLE = 1
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
    ]


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


def choose_gpu(devices):
    for index in range(devices.n_devices):
        if devices.devices[index].is_discrete:
            return index
    return 0


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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--library", required=True)
    parser.add_argument("--project", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--width", type=int, default=1600)
    parser.add_argument("--height", type=int, default=1000)
    parser.add_argument("--frames", type=int, default=90)
    parser.add_argument("--fps", type=int, default=30)
    parser.add_argument("--pointer-x", type=float, default=0.5)
    parser.add_argument("--pointer-y", type=float, default=0.5)
    parser.add_argument("--properties", default="{}")
    parser.add_argument("--media-state", default="")
    args = parser.parse_args()

    library_path = pathlib.Path(args.library).resolve()
    lib = ctypes.CDLL(str(library_path), mode=ctypes.RTLD_LOCAL)
    bind_abi(lib)

    devices = GpuDeviceList()
    if not lib.vivid_gpu_devices_enumerate(ctypes.byref(devices)) or not devices.n_devices:
        raise RuntimeError("no usable Vulkan render device was found")

    device_index = choose_gpu(devices)
    device = devices.devices[device_index]
    render_node = bytes(device.render_node).split(b"\0", 1)[0]

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

    try:
        lib.vivid_scene_producer_set_release_gate(producer, ctypes.byref(release_gate))

        if not lib.vivid_scene_producer_configure(
            producer,
            os.fsencode(args.project),
            os.fsencode(args.properties),
            1,
            0.0,
            1,
            args.fps,
            1,
            0,
            4,
            0,
            0,
            2,
            render_node,
            ctypes.byref(device),
        ):
            raise RuntimeError("failed to configure the scene producer")

        lib.vivid_scene_producer_set_playing(producer, 1)
        if args.media_state:
            lib.vivid_scene_producer_set_media_state_json(
                producer,
                os.fsencode(args.media_state),
            )

        request = DmaBufRequest(
            DRM_FORMAT_ABGR8888,
            DRM_FORMAT_MOD_LINEAR,
            1,
            1,
            DMABUF_MEMORY_HOST_VISIBLE,
        )
        wait_for_buffers(
            lib,
            producer,
            request,
            buffers,
            args.width,
            args.height,
        )

        # The input ABI receives output-space pixels and needs the prepared output dimensions.
        lib.vivid_scene_producer_set_pointer_motion(
            producer,
            args.pointer_x * args.width,
            args.pointer_y * args.height,
        )

        frame = Frame()
        for frame_index in range(max(1, args.frames)):
            wait_for_frame(lib, producer, frame)
            wait_for_acquire_fence(frame)
            if frame_index + 1 < max(1, args.frames):
                time.sleep(1.0 / 30.0)

        stride = save_frame(buffers, frame, args.output)
        print(
            f"saved {args.output} sequence={frame.sequence} "
            f"buffer={frame.buffer_index} stride={stride}"
        )
    finally:
        lib.vivid_scene_producer_set_release_gate(producer, None)
        if buffers.n_buffers:
            lib.vivid_scene_producer_buffer_set_clear(ctypes.byref(buffers))
        lib.vivid_scene_producer_free(producer)


if __name__ == "__main__":
    main()
