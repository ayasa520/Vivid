#!/usr/bin/env python3

import argparse
import json
import mimetypes
import os
import pathlib
import socket
import struct
import time
import urllib.parse
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


PROTOCOL_NAME = "vivid-display-v1"
PROTOCOL_VERSION = 1

REQ_HELLO = 1
REQ_CONTROL = 11

EVT_WELCOME = 1
EVT_CONTROL = 7

CONTROL_GET_STATE = 1
CONTROL_STATE_SNAPSHOT = 2
CONTROL_SET_PROJECT = 3
CONTROL_SET_MUTED = 5
CONTROL_SET_VOLUME = 6
CONTROL_SET_CONTENT_FIT = 7
CONTROL_SET_SCENE_FPS = 8
CONTROL_SET_USER_PROPERTIES = 9
CONTROL_ACK = 11
CONTROL_ERROR = 12
CONTROL_SET_STATE = 13

CONTROL_HEADER_BYTES = 8
FRAME_HEADER_BYTES = 4
MAX_BODY_BYTES = 65531


CONTROL_ACTIONS = {
    "getState": CONTROL_GET_STATE,
    "setProject": CONTROL_SET_PROJECT,
    "setMuted": CONTROL_SET_MUTED,
    "setVolume": CONTROL_SET_VOLUME,
    "setContentFit": CONTROL_SET_CONTENT_FIT,
    "setSceneFps": CONTROL_SET_SCENE_FPS,
    "setUserProperties": CONTROL_SET_USER_PROPERTIES,
    "patchState": CONTROL_SET_STATE,
}

WALLPAPER_ENGINE_WORKSHOP_APP_ID = "431960"
PROJECT_TYPES = {"video", "web", "scene"}
PROJECT_CONTENT_RATINGS = ("Everyone", "Questionable", "Mature")
SCENE_PROPERTY_TYPES = {
    "bool",
    "color",
    "combo",
    "directory",
    "file",
    "group",
    "scenetexture",
    "slider",
    "text",
    "textinput",
}
SCENE_PROPERTY_STRING_TYPES = {
    "color",
    "combo",
    "directory",
    "file",
    "group",
    "scenetexture",
    "text",
    "textinput",
}
SCENE_PROPERTY_EDITABLE_TYPES = {
    "bool",
    "color",
    "combo",
    "directory",
    "file",
    "scenetexture",
    "slider",
    "textinput",
}
THUMBNAIL_EXTENSIONS = {".gif", ".jpg", ".jpeg", ".png", ".webp", ".bmp"}
REMOTE_IMAGE_MAX_BYTES = 16 * 1024 * 1024
REMOTE_IMAGE_TIMEOUT_SECONDS = 8
REMOTE_IMAGE_USER_AGENT = (
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) VividWebUI/0.1 Safari/537.36"
)


def default_socket_path():
    env_socket = os.environ.get("VIVID_DISPLAY_SOCKET")
    if env_socket:
        return env_socket

    runtime_dir = os.environ.get("XDG_RUNTIME_DIR", "/tmp")
    host_runtime_dir = runtime_dir

    # Flatpak gives the app an app-private runtime directory. The producer
    # intentionally publishes the display socket in the host runtime directory
    # so the unsandboxed GNOME Shell consumer can connect to the same endpoint.
    if pathlib.Path("/.flatpak-info").exists():
        runtime_path = pathlib.Path(runtime_dir)
        if runtime_path.parent.name == "app":
            host_runtime_dir = str(runtime_path.parent.parent)

    return str(pathlib.Path(host_runtime_dir) / "vivid" / "display-v1.sock")


def encode_frame(opcode, body=b""):
    if len(body) > MAX_BODY_BYTES:
        raise ValueError(f"frame body too large: {len(body)} bytes")
    return struct.pack("<HH", opcode, len(body) + FRAME_HEADER_BYTES) + body


def encode_json_frame(opcode, payload):
    return encode_frame(opcode, json.dumps(payload, separators=(",", ":")).encode("utf-8"))


def encode_control_frame(control_opcode, payload):
    body_json = json.dumps(payload or {}, separators=(",", ":")).encode("utf-8")
    control_header = struct.pack("<HHI", control_opcode, 0, len(body_json))
    return encode_frame(REQ_CONTROL, control_header + body_json)


def read_exact(sock, size):
    chunks = []
    remaining = size
    while remaining:
        chunk = sock.recv(remaining)
        if not chunk:
            raise ConnectionError("producer closed the socket")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def read_frame(sock):
    header = read_exact(sock, FRAME_HEADER_BYTES)
    opcode, total_length = struct.unpack("<HH", header)
    if total_length < FRAME_HEADER_BYTES:
        raise ValueError(f"invalid frame length: {total_length}")
    body_length = total_length - FRAME_HEADER_BYTES
    if body_length > MAX_BODY_BYTES:
        raise ValueError(f"frame body too large: {body_length}")
    return opcode, read_exact(sock, body_length)


def decode_json(body):
    if not body:
        return {}
    text = body.decode("utf-8")
    return json.loads(text) if text.strip() else {}


def decode_control(body):
    if len(body) < CONTROL_HEADER_BYTES:
        raise ValueError("control frame body is too small")
    opcode, flags, json_length = struct.unpack("<HHI", body[:CONTROL_HEADER_BYTES])
    payload = body[CONTROL_HEADER_BYTES:]
    if len(payload) != json_length:
        raise ValueError(f"control json length mismatch: header={json_length} body={len(payload)}")
    return {
        "opcode": opcode,
        "flags": flags,
        "payload": decode_json(payload),
    }


def producer_control(socket_path, control_opcode, payload=None):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
        sock.settimeout(5.0)
        sock.connect(socket_path)
        sock.sendall(encode_json_frame(REQ_HELLO, {
            "protocol": PROTOCOL_NAME,
            "version": PROTOCOL_VERSION,
            "clientName": "wallpaper-webui",
            "role": "controller",
            "features": ["socket-control-v1"],
        }))

        opcode, body = read_frame(sock)
        if opcode != EVT_WELCOME:
            raise RuntimeError(f"expected WELCOME, got opcode {opcode}")

        sock.sendall(encode_control_frame(control_opcode, payload or {}))

        for _ in range(32):
            opcode, body = read_frame(sock)
            if opcode != EVT_CONTROL:
                continue

            control = decode_control(body)
            if control["opcode"] in (CONTROL_STATE_SNAPSHOT, CONTROL_ACK, CONTROL_ERROR):
                return control

        raise TimeoutError("producer did not return a control response")


def response_payload(response):
    return response.get("payload") if isinstance(response, dict) else {}


def is_dir(path):
    return bool(path) and pathlib.Path(path).is_dir()


def has_wallpaper_engine_install(steam_library_path):
    if not steam_library_path:
        return False
    return (pathlib.Path(steam_library_path) /
            "steamapps" / "common" / "wallpaper_engine").is_dir()


def normalize_library_root_path(path):
    if not path:
        return ""

    current = pathlib.Path(path).expanduser()
    try:
        current = current.resolve(strict=False)
    except OSError:
        current = current.absolute()

    probe = current
    while True:
        if has_wallpaper_engine_install(str(probe)):
            return str(probe)
        if probe.parent == probe:
            break
        probe = probe.parent

    return str(current)


def wallpaper_engine_project_roots(library_path):
    normalized = normalize_library_root_path(library_path)
    if not normalized:
        return []

    if not has_wallpaper_engine_install(normalized):
        return [normalized] if is_dir(normalized) else []

    root = pathlib.Path(normalized)
    candidates = [
        root / "steamapps" / "workshop" / "content" / WALLPAPER_ENGINE_WORKSHOP_APP_ID,
        root / "Steamapps" / "Workshop" / "content" / WALLPAPER_ENGINE_WORKSHOP_APP_ID,
        root / "Steamapps" / "Workshop" / "Content" / WALLPAPER_ENGINE_WORKSHOP_APP_ID,
        root / "steamapps" / "Workshop" / "Content" / WALLPAPER_ENGINE_WORKSHOP_APP_ID,
        root / "steamapps" / "common" / "wallpaper_engine" / "projects" / "defaultprojects",
        root / "steamapps" / "common" / "wallpaper_engine" / "projects" / "myprojects",
    ]
    seen = set()
    roots = []
    for candidate in candidates:
        try:
            resolved = candidate.resolve(strict=False)
        except OSError:
            resolved = candidate.absolute()
        key = str(resolved)
        if key not in seen and resolved.is_dir():
            roots.append(key)
            seen.add(key)
    return roots


def read_project_json(project_dir):
    try:
        with open(pathlib.Path(project_dir) / "project.json", "r", encoding="utf-8") as file:
            data = json.load(file)
    except (OSError, json.JSONDecodeError, UnicodeDecodeError):
        return None
    return data if isinstance(data, dict) else None


def resolve_legacy_project_type(manifest):
    entry = str(manifest.get("file") or "").strip().lower()
    if not entry:
        return None
    if entry.endswith((".json", ".pkg")):
        return "scene"
    if entry.endswith((".html", ".htm")):
        return "web"
    return None


def resolve_project_type(manifest):
    raw_type = str(manifest.get("type") or "").strip().lower()
    if raw_type in PROJECT_TYPES:
        return raw_type
    if raw_type:
        return None
    return resolve_legacy_project_type(manifest)


def resolve_entry_file(manifest, project_type):
    if isinstance(manifest.get("file"), str) and manifest["file"]:
        return manifest["file"]
    if project_type == "web":
        return "index.html"
    return None


def resolve_regular_file(project_dir, relative_path):
    if not relative_path:
        return None
    path = pathlib.Path(project_dir) / relative_path
    try:
        if path.is_file():
            return str(path)
    except OSError:
        return None
    return None


def resolve_preview_file(project_dir, manifest):
    candidates = [
        "preview.gif",
        manifest.get("preview"),
        "preview.jpg",
        "preview.jpeg",
        "preview.png",
        "preview.webp",
    ]
    for candidate in candidates:
        if not candidate:
            continue
        path = resolve_regular_file(project_dir, candidate)
        if path:
            return path
    return None


def normalize_project_tags(manifest):
    tags = manifest.get("tags")
    if not isinstance(tags, list):
        return []
    return [tag for tag in tags if isinstance(tag, str) and tag]


def normalize_project_content_rating(value):
    rating = value.strip() if isinstance(value, str) else ""
    return rating or PROJECT_CONTENT_RATINGS[0]


def resolve_project_config_id(project_dir, manifest):
    workshop_id = manifest.get("workshopid")
    if workshop_id is not None and str(workshop_id) != "":
        return f"workshop:{workshop_id}"
    return f"path:{project_dir}"


def normalize_scene_property_type(value):
    normalized = str(value or "").strip().lower()
    return normalized if normalized in SCENE_PROPERTY_TYPES else (normalized or None)


def normalize_scene_property_value(property_type, value, fallback=None):
    normalized_type = normalize_scene_property_type(property_type)
    if normalized_type == "bool":
        if isinstance(value, bool):
            return value
        if isinstance(value, (int, float)):
            return abs(value) >= 0.0001
        if isinstance(value, str):
            lowered = value.strip().lower()
            if lowered in ("", "0", "false", "off", "no"):
                return False
            if lowered in ("1", "true", "on", "yes"):
                return True
        return bool(fallback)
    if normalized_type == "slider":
        if isinstance(value, (int, float)):
            return float(value)
        if isinstance(value, str):
            try:
                return float(value.strip())
            except ValueError:
                pass
        return float(fallback) if isinstance(fallback, (int, float)) else 0.0
    if normalized_type in SCENE_PROPERTY_STRING_TYPES:
        if isinstance(value, list):
            return " ".join(str(item) for item in value)
        if isinstance(value, str):
            return value.strip() if normalized_type == "color" else value
        if isinstance(value, bool):
            return "true" if value else "false"
        if isinstance(value, (int, float)):
            return str(value)
        return fallback if isinstance(fallback, str) else ""
    return value if value is not None else fallback


def normalize_scene_property_options(property_data):
    options = property_data.get("options")
    if not isinstance(options, list):
        return []
    normalized = []
    for option in options:
        if not isinstance(option, dict):
            continue
        text = option.get("label") if isinstance(option.get("label"), str) else option.get("text")
        if not isinstance(text, str):
            text = str(option.get("value", ""))
        raw_value = option.get("value", text)
        value = normalize_scene_property_value("combo", raw_value, "")
        normalized.append({
            "text": text,
            "value": value,
            "rawValue": raw_value if raw_value is not None else text,
            "condition": option.get("condition", "").strip()
            if isinstance(option.get("condition"), str) else "",
        })
    return normalized


def normalize_scene_property(name, property_data):
    if not isinstance(property_data, dict):
        return None
    property_type = normalize_scene_property_type(property_data.get("type"))
    if not property_type:
        return None
    default_value = normalize_scene_property_value(property_type, property_data.get("value"), "")
    return {
        "name": name,
        "type": property_type,
        "text": property_data.get("text") if isinstance(property_data.get("text"), str) else name,
        "order": property_data.get("order") if isinstance(property_data.get("order"), (int, float)) else 0,
        "condition": property_data.get("condition", "").strip()
        if isinstance(property_data.get("condition"), str) else "",
        "mode": property_data.get("mode", "").strip().lower()
        if isinstance(property_data.get("mode"), str) else "",
        "min": property_data.get("min") if isinstance(property_data.get("min"), (int, float)) else None,
        "max": property_data.get("max") if isinstance(property_data.get("max"), (int, float)) else None,
        "step": property_data.get("step") if isinstance(property_data.get("step"), (int, float)) else None,
        "defaultValue": default_value,
        "editable": property_type in SCENE_PROPERTY_EDITABLE_TYPES,
        "storesString": property_type in SCENE_PROPERTY_STRING_TYPES,
        "options": normalize_scene_property_options(property_data),
    }


def normalize_scene_properties(manifest, project_type):
    if project_type not in ("scene", "web"):
        return []
    general = manifest.get("general")
    if not isinstance(general, dict):
        return []
    properties = general.get("properties")
    if not isinstance(properties, dict):
        return []
    result = [
        normalized
        for name, property_data in properties.items()
        if (normalized := normalize_scene_property(name, property_data)) is not None
    ]
    return sorted(result, key=lambda item: (item["order"], item["name"]))


def directory_modified_time(path):
    try:
        return pathlib.Path(path).stat().st_mtime
    except OSError:
        return 0


def load_project(project_dir):
    manifest = read_project_json(project_dir)
    if manifest is None:
        return None
    project_type = resolve_project_type(manifest)
    if not project_type:
        return None
    entry = resolve_entry_file(manifest, project_type)
    entry_path = resolve_regular_file(project_dir, entry)
    if project_type == "scene" and not entry_path:
        entry_path = resolve_regular_file(project_dir, "scene.pkg")
    if entry and not entry_path and project_type != "scene":
        return None

    project_path = str(pathlib.Path(project_dir))
    title = manifest.get("title").strip() if isinstance(manifest.get("title"), str) else ""
    if not title:
        title = pathlib.Path(project_dir).name

    return {
        "path": project_path,
        "basename": pathlib.Path(project_dir).name,
        "title": title,
        "description": manifest.get("description") if isinstance(manifest.get("description"), str) else "",
        "tags": normalize_project_tags(manifest),
        "contentrating": normalize_project_content_rating(manifest.get("contentrating")),
        "workshopId": manifest.get("workshopid"),
        "type": project_type,
        "entry": entry,
        "entryPath": entry_path,
        "preview": manifest.get("preview") if isinstance(manifest.get("preview"), str) else None,
        "previewPath": resolve_preview_file(project_dir, manifest),
        "configId": resolve_project_config_id(project_path, manifest),
        "sceneProperties": normalize_scene_properties(manifest, project_type),
        "updatedTime": int(directory_modified_time(project_path)),
    }


def list_projects(library_path):
    roots = wallpaper_engine_project_roots(library_path)
    projects = []
    seen = set()
    for root in roots:
        try:
            children = sorted(pathlib.Path(root).iterdir(), key=lambda child: child.name)
        except OSError:
            continue
        for child in children:
            if not child.is_dir():
                continue
            project = load_project(str(child))
            if not project or project["path"] in seen:
                continue
            seen.add(project["path"])
            projects.append(project)
    return sorted(projects, key=lambda item: item["path"])


def parse_query(path):
    parsed = urllib.parse.urlparse(path)
    query = urllib.parse.parse_qs(parsed.query, keep_blank_values=True)
    return parsed.path, {key: values[-1] if values else "" for key, values in query.items()}


def validate_remote_image_url(url):
    try:
        parsed = urllib.parse.urlsplit(url)
    except ValueError as error:
        raise ValueError(f"invalid remote image URL: {error}") from error
    if parsed.scheme not in ("http", "https") or not parsed.netloc:
        raise ValueError("remote image URL must use http or https")
    return parsed


class VividWebUIHandler(BaseHTTPRequestHandler):
    server_version = "VividWebUI/0.1"

    def _send_json(self, status, payload):
        body = json.dumps(payload, indent=2, sort_keys=True).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _read_json_body(self):
        length = int(self.headers.get("Content-Length", "0") or "0")
        if length <= 0:
            return {}
        return json.loads(self.rfile.read(length).decode("utf-8"))

    def _serve_static(self):
        request_path = urllib.parse.urlparse(self.path).path
        rel_path = request_path.lstrip("/") or "index.html"
        if rel_path.endswith("/"):
            rel_path += "index.html"

        root = pathlib.Path(self.server.web_root).resolve()
        target = (root / rel_path).resolve()
        if not str(target).startswith(str(root)) or not target.is_file():
            self.send_error(404)
            return

        data = target.read_bytes()
        content_type = mimetypes.guess_type(str(target))[0] or "application/octet-stream"
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _serve_file(self, path):
        target = pathlib.Path(path)
        if target.suffix.lower() not in THUMBNAIL_EXTENSIONS or not target.is_file():
            self.send_error(404)
            return

        data = target.read_bytes()
        content_type = mimetypes.guess_type(str(target))[0] or "application/octet-stream"
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "private, max-age=60")
        self.end_headers()
        self.wfile.write(data)

    def _serve_remote_image(self, url):
        parsed = validate_remote_image_url(url)
        origin = f"{parsed.scheme}://{parsed.netloc}/"
        request = urllib.request.Request(url, headers={
            "User-Agent": REMOTE_IMAGE_USER_AGENT,
            "Accept": "image/avif,image/webp,image/apng,image/svg+xml,image/*,*/*;q=0.8",
            "Referer": origin,
        })

        try:
            with urllib.request.urlopen(
                request,
                timeout=REMOTE_IMAGE_TIMEOUT_SECONDS,
            ) as response:
                content_length = response.headers.get("Content-Length")
                if content_length and int(content_length) > REMOTE_IMAGE_MAX_BYTES:
                    raise ValueError(f"remote image is too large: {content_length} bytes")

                data = response.read(REMOTE_IMAGE_MAX_BYTES + 1)
                if len(data) > REMOTE_IMAGE_MAX_BYTES:
                    raise ValueError(
                        f"remote image exceeded {REMOTE_IMAGE_MAX_BYTES} bytes"
                    )

                content_type = response.headers.get_content_type()
                if not content_type.startswith("image/"):
                    raise ValueError(f"remote response is not an image: {content_type}")
        except (OSError, urllib.error.HTTPError, urllib.error.URLError, ValueError) as error:
            self.send_error(502, str(error))
            return

        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "private, max-age=3600")
        self.end_headers()
        self.wfile.write(data)

    def _producer_state(self):
        control = producer_control(self.server.socket_path, CONTROL_GET_STATE)
        payload = response_payload(control)
        return control, payload

    def do_GET(self):
        request_path, query = parse_query(self.path)

        if request_path == "/api/state":
            try:
                control = producer_control(self.server.socket_path, CONTROL_GET_STATE)
                self._send_json(200, {"ok": True, "control": control})
            except Exception as error:
                self._send_json(502, {"ok": False, "error": str(error)})
            return

        if request_path == "/api/projects":
            try:
                control, state = self._producer_state()
                global_config = state.get("global", {}) if isinstance(state, dict) else {}
                library_path = query.get("libraryPath") or global_config.get("change-wallpaper-directory-path", "")
                normalized_library_path = normalize_library_root_path(library_path)
                projects = list_projects(normalized_library_path) if normalized_library_path else []
                self._send_json(200, {
                    "ok": True,
                    "libraryPath": normalized_library_path,
                    "roots": wallpaper_engine_project_roots(normalized_library_path),
                    "projects": projects,
                    "state": state,
                    "control": control,
                    "timestamp": int(time.time()),
                })
            except Exception as error:
                self._send_json(502, {"ok": False, "error": str(error)})
            return

        if request_path == "/api/thumbnail":
            thumbnail_path = query.get("path", "")
            try:
                self._serve_file(thumbnail_path)
            except Exception as error:
                self._send_json(404, {"ok": False, "error": str(error)})
            return

        if request_path == "/api/remote-image":
            try:
                self._serve_remote_image(query.get("url", ""))
            except Exception as error:
                self._send_json(502, {"ok": False, "error": str(error)})
            return

        self._serve_static()

    def do_POST(self):
        request_path, _query = parse_query(self.path)

        if request_path == "/api/config":
            try:
                payload = self._read_json_body()
                control = producer_control(self.server.socket_path, CONTROL_SET_STATE, payload)
                self._send_json(200, {"ok": True, "control": control})
            except Exception as error:
                self._send_json(502, {"ok": False, "error": str(error)})
            return

        if request_path == "/api/wallpaper/select":
            try:
                request = self._read_json_body()
                project_path = request.get("projectPath") or request.get("path") or ""
                if not project_path:
                    raise ValueError("projectPath is required")
                properties = request.get("properties")
                if properties is None:
                    properties = {}

                # Selecting a wallpaper is intentionally a two-step socket
                # transaction: first switch the project path, then publish that
                # wallpaper's own property payload. Without the second frame a
                # producer that just rendered another wallpaper would keep the
                # previous active user-properties JSON until the next property
                # edit, which is exactly the class of cross-wallpaper state leak
                # this split architecture is meant to avoid.
                select_control = producer_control(
                    self.server.socket_path,
                    CONTROL_SET_STATE,
                    {"project-path": project_path},
                )
                properties_control = producer_control(
                    self.server.socket_path,
                    CONTROL_SET_USER_PROPERTIES,
                    {
                        "wallpaperId": request.get("wallpaperId") or project_path,
                        "projectType": request.get("projectType") or request.get("type") or "",
                        "properties": properties,
                    },
                )
                self._send_json(200, {
                    "ok": True,
                    "control": properties_control,
                    "selectControl": select_control,
                })
            except Exception as error:
                self._send_json(502, {"ok": False, "error": str(error)})
            return

        if request_path == "/api/wallpaper/properties":
            try:
                request = self._read_json_body()
                project_path = request.get("projectPath") or request.get("path") or ""
                properties = request.get("properties")
                if properties is None:
                    properties = {}
                control = producer_control(
                    self.server.socket_path,
                    CONTROL_SET_USER_PROPERTIES,
                    {
                        "wallpaperId": request.get("wallpaperId") or project_path,
                        "projectType": request.get("projectType") or request.get("type") or "",
                        "properties": properties,
                    },
                )
                self._send_json(200, {"ok": True, "control": control})
            except Exception as error:
                self._send_json(502, {"ok": False, "error": str(error)})
            return

        if request_path == "/api/control":
            try:
                request = self._read_json_body()
                action = request.get("action")
                opcode = int(request.get("opcode") or CONTROL_ACTIONS.get(action, CONTROL_SET_STATE))
                payload = request.get("payload")
                if payload is None:
                    payload = {
                        key: value
                        for key, value in request.items()
                        if key not in ("action", "opcode", "payload")
                    }
                control = producer_control(self.server.socket_path, opcode, payload)
                self._send_json(200, {"ok": True, "control": control})
            except Exception as error:
                self._send_json(502, {"ok": False, "error": str(error)})
            return

        self.send_error(404)

    def log_message(self, fmt, *args):
        return


def main():
    parser = argparse.ArgumentParser(description="Vivid producer WebUI socket bridge")
    parser.add_argument("--host", default=os.environ.get("VIVID_WEBUI_HOST", "127.0.0.1"))
    parser.add_argument("--port", type=int, default=int(os.environ.get("VIVID_WEBUI_PORT", "8765")))
    parser.add_argument("--socket", default=default_socket_path())
    parser.add_argument("--web-root", default=str(pathlib.Path(__file__).resolve().parent))
    args = parser.parse_args()

    httpd = ThreadingHTTPServer((args.host, args.port), VividWebUIHandler)
    httpd.socket_path = args.socket
    httpd.web_root = args.web_root
    print(f"VividWebUI: http://{args.host}:{args.port}", flush=True)
    httpd.serve_forever()


if __name__ == "__main__":
    main()
