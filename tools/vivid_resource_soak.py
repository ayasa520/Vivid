#!/usr/bin/env python3
"""Run a repeatable multi-display resource-leak soak test.

The test deliberately keeps one producer/Plasma session alive while repeatedly
switching clone/independent routing and/or adding, replacing, and removing a
wallpaper on the secondary output.  It records process CPU/RSS, smaps residency
breakdowns, descriptors/threads, and NVIDIA GPU utilisation, framebuffer usage,
power, and temperature.  A final checkpoint after every iteration makes
monotonic growth easier to distinguish from a short-lived transition spike.

The script does not edit source files.  It only drives the already-running
WebUI API and writes measurement artifacts under --output-dir.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import pathlib
import statistics
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from typing import Any


DEFAULT_PROJECT_A = "/media/rikka/Data/steam/steamapps/workshop/content/431960/3308867900"
DEFAULT_PROJECT_B = "/media/rikka/Data/steam/steamapps/workshop/content/431960/3480697015"
SCENARIOS = (
    "mixed",
    "mode-only",
    "secondary-wallpaper",
    "secondary-project",
    "both-project",
    "primary-wallpaper",
)


def now_wall() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%S%z")


def percentile(values: list[float], p: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    rank = (len(ordered) - 1) * p
    low = math.floor(rank)
    high = math.ceil(rank)
    if low == high:
        return ordered[low]
    return ordered[low] + (ordered[high] - ordered[low]) * (rank - low)


def finite_float(value: Any) -> float | None:
    try:
        result = float(value)
    except (TypeError, ValueError):
        return None
    return result if math.isfinite(result) else None


def read_json(url: str, timeout: float) -> dict[str, Any]:
    request = urllib.request.Request(url, method="GET")
    with urllib.request.urlopen(request, timeout=timeout) as response:
        payload = json.loads(response.read().decode("utf-8"))
    if not isinstance(payload, dict):
        raise RuntimeError(f"unexpected JSON response from {url}")
    return payload


def post_json(url: str, body: dict[str, Any], timeout: float) -> tuple[int, dict[str, Any]]:
    encoded = json.dumps(body, separators=(",", ":")).encode("utf-8")
    request = urllib.request.Request(
        url,
        data=encoded,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    started = time.monotonic()
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            status = response.status
            payload = json.loads(response.read().decode("utf-8"))
    except (urllib.error.HTTPError, urllib.error.URLError, TimeoutError) as error:
        elapsed = time.monotonic() - started
        raise RuntimeError(f"POST {url} failed after {elapsed:.1f}s: {error}") from error
    if not isinstance(payload, dict):
        raise RuntimeError(f"unexpected JSON response from {url}")
    return status, payload


def producer_state(base_url: str, timeout: float) -> dict[str, Any]:
    response = read_json(f"{base_url}/api/state", timeout)
    control = response.get("control")
    if not isinstance(control, dict):
        raise RuntimeError("/api/state did not contain control")
    payload = control.get("payload")
    if not isinstance(payload, dict):
        raise RuntimeError("/api/state did not contain a state payload")
    return payload


def proc_stat(pid: int) -> tuple[int, int, int, int] | None:
    try:
        raw = pathlib.Path(f"/proc/{pid}/stat").read_text(encoding="utf-8")
        # comm can contain spaces and ')' characters. The final ')' before the
        # state field is the reliable delimiter for the remaining fields.
        close = raw.rfind(")")
        fields = raw[close + 2 :].split()
        ppid = int(fields[1])
        utime = int(fields[11])
        stime = int(fields[12])
        vsz = int(fields[20])
        rss_pages = int(fields[21])
        return ppid, utime + stime, vsz, rss_pages
    except (FileNotFoundError, PermissionError, ValueError, IndexError):
        return None


def process_command(pid: int) -> str:
    try:
        raw = pathlib.Path(f"/proc/{pid}/cmdline").read_bytes()
    except (FileNotFoundError, PermissionError):
        return ""
    return raw.replace(b"\0", b" ").decode("utf-8", errors="replace").strip()


def proc_smaps_rollup(pid: int) -> dict[str, int]:
    """Return selected /proc smaps_rollup values in kB."""
    fields = {
        "rss_kb": "Rss:",
        "pss_kb": "Pss:",
        "private_dirty_kb": "Private_Dirty:",
        "anonymous_kb": "Anonymous:",
        "anon_huge_pages_kb": "AnonHugePages:",
        "swap_kb": "Swap:",
    }
    result = {name: 0 for name in fields}
    try:
        text = pathlib.Path(f"/proc/{pid}/smaps_rollup").read_text(encoding="utf-8")
    except (FileNotFoundError, PermissionError):
        return result
    for line in text.splitlines():
        key, _, value = line.partition(":")
        for name, wanted in fields.items():
            if key + ":" == wanted:
                try:
                    result[name] = int(value.strip().split()[0])
                except (ValueError, IndexError):
                    pass
    return result


def proc_fd_count(pid: int) -> int:
    try:
        return sum(1 for _ in pathlib.Path(f"/proc/{pid}/fd").iterdir())
    except (FileNotFoundError, PermissionError):
        return 0


def proc_fd_breakdown(pid: int) -> dict[str, int]:
    result = {
        "render_d128": 0,
        "nvidiactl": 0,
        "nvidia_device": 0,
        "dmabuf": 0,
        "sync_file": 0,
        "syncobj_file": 0,
    }
    try:
        entries = list(pathlib.Path(f"/proc/{pid}/fd").iterdir())
    except (FileNotFoundError, PermissionError):
        return result
    for entry in entries:
        try:
            target = os.readlink(entry)
        except (FileNotFoundError, PermissionError, OSError):
            continue
        if target == "/dev/dri/renderD128":
            result["render_d128"] += 1
        if target == "/dev/nvidiactl":
            result["nvidiactl"] += 1
        if target.startswith("/dev/nvidia"):
            result["nvidia_device"] += 1
        if "/dmabuf:" in target or target.startswith("dmabuf:"):
            result["dmabuf"] += 1
        if target == "anon_inode:sync_file":
            result["sync_file"] += 1
        if target == "anon_inode:syncobj_file":
            result["syncobj_file"] += 1
    return result


def proc_thread_count(pid: int) -> int:
    try:
        text = pathlib.Path(f"/proc/{pid}/status").read_text(encoding="utf-8")
    except (FileNotFoundError, PermissionError):
        return 0
    for line in text.splitlines():
        if line.startswith("Threads:"):
            try:
                return int(line.split()[1])
            except (ValueError, IndexError):
                return 0
    return 0


def process_roles() -> dict[str, list[int]]:
    roles: dict[str, list[int]] = {
        "producer": [],
        "plasmashell": [],
        "web_helper": [],
    }
    for entry in pathlib.Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        pid = int(entry.name)
        command = process_command(pid)
        if "vivid-producer-dev" in command:
            roles["producer"].append(pid)
        elif pathlib.Path(command.split(" ", 1)[0] if command else "").name == "plasmashell":
            roles["plasmashell"].append(pid)
        elif "vivid-web-helper" in command:
            roles["web_helper"].append(pid)
    return roles


def process_tree() -> dict[int, int]:
    result: dict[int, int] = {}
    for entry in pathlib.Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        pid = int(entry.name)
        stat = proc_stat(pid)
        if stat:
            result[pid] = stat[0]
    return result


def descendants(root: int, parents: dict[int, int]) -> set[int]:
    found = {root}
    changed = True
    while changed:
        changed = False
        for pid, ppid in parents.items():
            if ppid in found and pid not in found:
                found.add(pid)
                changed = True
    return found


def nvidia_global() -> dict[str, float | None]:
    command = [
        "nvidia-smi",
        "--query-gpu=utilization.gpu,utilization.memory,memory.used,memory.free,memory.total,power.draw,temperature.gpu",
        "--format=csv,noheader,nounits",
    ]
    try:
        result = subprocess.run(command, check=False, capture_output=True, text=True, timeout=5)
    except (OSError, subprocess.TimeoutExpired):
        return {}
    if result.returncode != 0 or not result.stdout.strip():
        return {}
    fields = [part.strip() for part in result.stdout.splitlines()[0].split(",")]
    if len(fields) < 7:
        return {}
    names = (
        "gpu_util_pct",
        "mem_util_pct",
        "gpu_mem_used_mb",
        "gpu_mem_free_mb",
        "gpu_mem_total_mb",
        "gpu_power_w",
        "gpu_temp_c",
    )
    return {name: finite_float(value) for name, value in zip(names, fields)}


def nvidia_pmon() -> dict[int, float]:
    try:
        result = subprocess.run(
            ["nvidia-smi", "pmon", "-c", "1", "-s", "m"],
            check=False,
            capture_output=True,
            text=True,
            timeout=8,
        )
    except (OSError, subprocess.TimeoutExpired):
        return {}
    values: dict[int, float] = {}
    for line in result.stdout.splitlines():
        fields = line.split()
        if len(fields) < 5 or not fields[0].isdigit() or not fields[1].isdigit():
            continue
        try:
            values[int(fields[1])] = float(fields[3])
        except ValueError:
            continue
    return values


@dataclass
class Sampler:
    output_dir: pathlib.Path
    interval: float
    pmon_every: int
    phase_lock: threading.Lock = field(default_factory=threading.Lock)
    phase: str = "startup"
    iteration: int = -1
    stop: threading.Event = field(default_factory=threading.Event)
    thread: threading.Thread | None = None
    rows: list[dict[str, Any]] = field(default_factory=list)
    previous_ticks: dict[int, tuple[int, float]] = field(default_factory=dict)
    sample_count: int = 0

    def set_phase(self, phase: str, iteration: int | None = None) -> None:
        with self.phase_lock:
            self.phase = phase
            if iteration is not None:
                self.iteration = iteration

    def current_phase(self) -> tuple[str, int]:
        with self.phase_lock:
            return self.phase, self.iteration

    def start(self) -> None:
        self.thread = threading.Thread(target=self._run, name="vivid-resource-sampler", daemon=True)
        self.thread.start()

    def join(self) -> None:
        self.stop.set()
        if self.thread:
            self.thread.join(timeout=max(5.0, self.interval * 3.0))

    def _run(self) -> None:
        hz = os.sysconf(os.sysconf_names["SC_CLK_TCK"])
        page_kb = os.sysconf("SC_PAGE_SIZE") // 1024
        next_pmon = 0
        pmon: dict[int, float] = {}
        while not self.stop.is_set():
            started = time.monotonic()
            phase, iteration = self.current_phase()
            roles = process_roles()
            parents = process_tree()
            row: dict[str, Any] = {
                "timestamp": now_wall(),
                "monotonic": started,
                "phase": phase,
                "iteration": iteration,
            }
            for role, pids in roles.items():
                role_cpu = 0.0
                role_rss = 0
                role_vsz = 0
                role_smaps = {
                    "pss_kb": 0,
                    "private_dirty_kb": 0,
                    "anonymous_kb": 0,
                    "anon_huge_pages_kb": 0,
                    "swap_kb": 0,
                }
                role_fds = 0
                role_fd_types = {
                    "render_d128": 0,
                    "nvidiactl": 0,
                    "nvidia_device": 0,
                    "dmabuf": 0,
                    "sync_file": 0,
                    "syncobj_file": 0,
                }
                role_threads = 0
                role_pids: list[int] = []
                for pid in pids:
                    stat = proc_stat(pid)
                    if not stat:
                        continue
                    _ppid, ticks, vsz, rss_pages = stat
                    previous = self.previous_ticks.get(pid)
                    if previous:
                        elapsed = max(0.001, started - previous[1])
                        role_cpu += max(0, ticks - previous[0]) / hz / elapsed * 100.0
                    self.previous_ticks[pid] = (ticks, started)
                    role_rss += rss_pages * page_kb
                    role_vsz += vsz // 1024
                    smaps = proc_smaps_rollup(pid)
                    for key in role_smaps:
                        role_smaps[key] += smaps[key]
                    role_fds += proc_fd_count(pid)
                    fd_types = proc_fd_breakdown(pid)
                    for key in role_fd_types:
                        role_fd_types[key] += fd_types[key]
                    role_threads += proc_thread_count(pid)
                    role_pids.append(pid)
                row[f"{role}_pids"] = ",".join(str(pid) for pid in role_pids)
                row[f"{role}_cpu_pct"] = round(role_cpu, 3)
                row[f"{role}_rss_mb"] = round(role_rss / 1024.0, 3)
                row[f"{role}_vsz_mb"] = round(role_vsz / 1024.0, 3)
                row[f"{role}_pss_mb"] = round(role_smaps["pss_kb"] / 1024.0, 3)
                row[f"{role}_private_dirty_mb"] = round(
                    role_smaps["private_dirty_kb"] / 1024.0, 3
                )
                row[f"{role}_anonymous_mb"] = round(role_smaps["anonymous_kb"] / 1024.0, 3)
                row[f"{role}_anon_huge_pages_mb"] = round(
                    role_smaps["anon_huge_pages_kb"] / 1024.0, 3
                )
                row[f"{role}_swap_mb"] = round(role_smaps["swap_kb"] / 1024.0, 3)
                row[f"{role}_fd_count"] = role_fds
                for key, value in role_fd_types.items():
                    row[f"{role}_fd_{key}"] = value
                row[f"{role}_thread_count"] = role_threads

            if self.sample_count >= next_pmon:
                pmon = nvidia_pmon()
                next_pmon = self.sample_count + self.pmon_every
            producer_pids = set(roles["producer"])
            producer_tree = set()
            for pid in producer_pids:
                producer_tree |= descendants(pid, parents)
            row["producer_tree_count"] = len(producer_tree)
            row["producer_tree_rss_mb"] = round(
                sum((proc_stat(pid)[3] * page_kb) for pid in producer_tree if proc_stat(pid))
                / 1024.0,
                3,
            )
            row["producer_gpu_fb_mb"] = round(sum(pmon.get(pid, 0.0) for pid in producer_tree), 3)
            row["plasmashell_gpu_fb_mb"] = round(
                sum(pmon.get(pid, 0.0) for pid in roles["plasmashell"]), 3
            )
            row["web_helper_gpu_fb_mb"] = round(
                sum(pmon.get(pid, 0.0) for pid in roles["web_helper"]), 3
            )
            row.update(nvidia_global())
            self.rows.append(row)
            self.sample_count += 1
            elapsed = time.monotonic() - started
            self.stop.wait(max(0.05, self.interval - elapsed))

    def write_csv(self) -> pathlib.Path:
        path = self.output_dir / "samples.csv"
        if not self.rows:
            path.write_text("", encoding="utf-8")
            return path
        keys: list[str] = []
        for row in self.rows:
            for key in row:
                if key not in keys:
                    keys.append(key)
        with path.open("w", encoding="utf-8", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=keys)
            writer.writeheader()
            writer.writerows(self.rows)
        return path


def wait_seconds(sampler: Sampler, seconds: float, phase: str, iteration: int) -> None:
    sampler.set_phase(phase, iteration)
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        time.sleep(min(0.25, max(0.01, deadline - time.monotonic())))


def run_action(
    base_url: str,
    sampler: Sampler,
    actions_stream: Any,
    iteration: int,
    name: str,
    path: str,
    body: dict[str, Any],
    timeout: float,
    settle: float,
) -> None:
    sampler.set_phase(name, iteration)
    started = time.monotonic()
    record: dict[str, Any] = {
        "timestamp": now_wall(),
        "iteration": iteration,
        "action": name,
        "path": path,
        "body": body,
    }
    try:
        status, response = post_json(f"{base_url}{path}", body, timeout)
        record.update({"ok": True, "status": status, "response": response})
    except Exception as error:  # keep sampling even when one API action fails
        record.update({"ok": False, "error": str(error)})
        print(f"ACTION FAILED iteration={iteration} action={name}: {error}", file=sys.stderr)
    record["elapsed_sec"] = round(time.monotonic() - started, 3)
    actions_stream.write(json.dumps(record, ensure_ascii=False) + "\n")
    actions_stream.flush()
    wait_seconds(sampler, settle, f"settle:{name}", iteration)


def state_config(state: dict[str, Any]) -> dict[str, Any]:
    global_config = state.get("global")
    return global_config if isinstance(global_config, dict) else {}


def choose_outputs(state: dict[str, Any]) -> tuple[str, str]:
    config = state_config(state)
    primary = config.get("primary-display-key")
    outputs = state.get("outputs") if isinstance(state.get("outputs"), list) else []
    keys = [
        output.get("displayKey")
        for output in outputs
        if isinstance(output, dict) and isinstance(output.get("displayKey"), str)
    ]
    if not primary:
        for output in outputs:
            if isinstance(output, dict) and output.get("primary") and output.get("displayKey"):
                primary = output["displayKey"]
                break
    if not primary and keys:
        primary = keys[0]
    secondary = next((key for key in keys if key != primary), "")
    if not primary or not secondary:
        raise RuntimeError(f"need two live outputs, got primary={primary!r} keys={keys!r}")
    return str(primary), str(secondary)


def summary_for(rows: list[dict[str, Any]], metric: str) -> dict[str, Any]:
    values = [finite_float(row.get(metric)) for row in rows]
    values = [value for value in values if value is not None]
    if not values:
        return {}
    return {
        "count": len(values),
        "first": round(values[0], 3),
        "last": round(values[-1], 3),
        "min": round(min(values), 3),
        "max": round(max(values), 3),
        "mean": round(statistics.fmean(values), 3),
        "p95": round(percentile(values, 0.95) or 0.0, 3),
    }


def checkpoint_rows(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    grouped: dict[int, list[dict[str, Any]]] = {}
    for row in rows:
        iteration = row.get("iteration")
        phase = str(row.get("phase", ""))
        if (
            isinstance(iteration, int)
            and iteration >= 0
            and phase in {"checkpoint", "settle:mode-clone", "final-observation"}
        ):
            grouped.setdefault(iteration, []).append(row)
    result: list[dict[str, Any]] = []
    for iteration in sorted(grouped):
        values = grouped[iteration]
        # Use the median of the last few checkpoint samples to suppress one
        # frame or compositor spike from the leak trend.
        tail = values[-min(5, len(values)) :]
        item: dict[str, Any] = {"iteration": iteration, "samples": len(tail)}
        for metric in (
            "producer_cpu_pct",
            "producer_rss_mb",
            "producer_pss_mb",
            "producer_private_dirty_mb",
            "producer_anonymous_mb",
            "producer_anon_huge_pages_mb",
            "producer_swap_mb",
            "producer_fd_count",
            "producer_fd_render_d128",
            "producer_fd_nvidiactl",
            "producer_fd_nvidia_device",
            "producer_fd_dmabuf",
            "producer_fd_sync_file",
            "producer_fd_syncobj_file",
            "producer_thread_count",
            "producer_tree_count",
            "producer_tree_rss_mb",
            "plasmashell_rss_mb",
            "plasmashell_pss_mb",
            "plasmashell_private_dirty_mb",
            "plasmashell_anonymous_mb",
            "plasmashell_anon_huge_pages_mb",
            "plasmashell_swap_mb",
            "plasmashell_fd_count",
            "plasmashell_fd_render_d128",
            "plasmashell_fd_nvidiactl",
            "plasmashell_fd_nvidia_device",
            "plasmashell_fd_dmabuf",
            "plasmashell_fd_sync_file",
            "plasmashell_fd_syncobj_file",
            "plasmashell_thread_count",
            "web_helper_rss_mb",
            "web_helper_pss_mb",
            "web_helper_private_dirty_mb",
            "web_helper_anonymous_mb",
            "web_helper_fd_count",
            "web_helper_thread_count",
            "producer_gpu_fb_mb",
            "plasmashell_gpu_fb_mb",
            "gpu_mem_used_mb",
            "gpu_mem_free_mb",
            "gpu_util_pct",
            "gpu_power_w",
            "gpu_temp_c",
        ):
            numbers = [finite_float(row.get(metric)) for row in tail]
            numbers = [number for number in numbers if number is not None]
            if numbers:
                item[metric] = round(statistics.median(numbers), 3)
        result.append(item)
    return result


METRIC_TREND_LIMITS: dict[str, tuple[float, float]] = {
    # (steady-state absolute delta, slope per iteration).  Warm-up steps are
    # reported separately; these limits are for repeatable per-cycle growth.
    "producer_rss_mb": (12.0, 1.0),
    "producer_pss_mb": (12.0, 1.0),
    "producer_private_dirty_mb": (12.0, 1.0),
    "producer_anonymous_mb": (12.0, 1.0),
    "producer_anon_huge_pages_mb": (12.0, 1.0),
    "producer_swap_mb": (4.0, 0.25),
    "producer_fd_count": (3.0, 0.25),
    "producer_fd_render_d128": (1.0, 0.1),
    "producer_fd_nvidiactl": (1.0, 0.1),
    "producer_fd_dmabuf": (2.0, 0.2),
    "producer_fd_sync_file": (2.0, 0.2),
    "producer_fd_syncobj_file": (2.0, 0.2),
    "producer_thread_count": (2.0, 0.15),
    "producer_tree_count": (2.0, 0.15),
    "producer_gpu_fb_mb": (24.0, 2.0),
    "gpu_mem_used_mb": (32.0, 2.5),
    "plasmashell_rss_mb": (12.0, 1.0),
    "plasmashell_pss_mb": (12.0, 1.0),
    "plasmashell_private_dirty_mb": (12.0, 1.0),
    "plasmashell_anonymous_mb": (12.0, 1.0),
    "plasmashell_anon_huge_pages_mb": (12.0, 1.0),
    "plasmashell_swap_mb": (4.0, 0.25),
    "plasmashell_fd_count": (3.0, 0.25),
    "plasmashell_fd_render_d128": (1.0, 0.1),
    "plasmashell_fd_nvidiactl": (1.0, 0.1),
    "plasmashell_fd_dmabuf": (2.0, 0.2),
    "plasmashell_fd_sync_file": (2.0, 0.2),
    "plasmashell_fd_syncobj_file": (2.0, 0.2),
    "plasmashell_thread_count": (2.0, 0.15),
    "plasmashell_gpu_fb_mb": (24.0, 2.0),
    "web_helper_rss_mb": (12.0, 1.0),
}


def trend_from_points(
    points: list[dict[str, Any]], metric: str, start_index: int
) -> dict[str, Any]:
    values = [(float(point["iteration"]), finite_float(point.get(metric))) for point in points]
    values = [(x, y) for x, y in values if y is not None]
    all_values = values
    start_index = max(0, start_index)
    steady_values = values[start_index:]
    if len(steady_values) < 2:
        return {
            "metric": metric,
            "points": len(steady_values),
            "all_points": len(all_values),
            "steady_start_index": start_index,
        }

    xs = [x for x, _ in steady_values]
    ys = [y for _, y in steady_values]
    mean_x = statistics.fmean(xs)
    mean_y = statistics.fmean(ys)
    denominator = sum((x - mean_x) ** 2 for x in xs)
    slope = (
        sum((x - mean_x) * (y - mean_y) for x, y in steady_values) / denominator
        if denominator
        else 0.0
    )
    # GPU process accounting and allocators can move in discrete chunks.  A
    # median over the first/last few steady checkpoints avoids declaring a
    # leak merely because one endpoint caught the low or high side of an
    # otherwise repeating oscillation.
    endpoint_window = min(5, max(1, len(ys) // 3))
    first = statistics.median(ys[:endpoint_window])
    last = statistics.median(ys[-endpoint_window:])
    delta = last - first
    absolute_limit, slope_limit = METRIC_TREND_LIMITS.get(metric, (20.0, 3.0))

    initial_jump = None
    if len(all_values) >= 2 and start_index > 0:
        warm_values = all_values[: min(len(all_values), start_index + 1)]
        jumps = [
            abs(warm_values[i][1] - warm_values[i - 1][1])
            for i in range(1, len(warm_values))
        ]
        if jumps:
            initial_jump = max(jumps)

    suspect_growth = delta > absolute_limit and slope > slope_limit
    classification = "steady-state-growth" if suspect_growth else "stable-or-noisy"
    if not suspect_growth and initial_jump is not None and initial_jump > absolute_limit:
        classification = "bounded-warmup-step"

    return {
        "metric": metric,
        "points": len(steady_values),
        "all_points": len(all_values),
        "steady_start_index": start_index,
        "first": round(first, 3),
        "last": round(last, 3),
        "raw_first": round(ys[0], 3),
        "raw_last": round(ys[-1], 3),
        "endpoint_window": endpoint_window,
        "delta": round(delta, 3),
        "slope_per_iteration": round(slope, 3),
        "initial_warmup_jump": round(initial_jump, 3) if initial_jump is not None else None,
        "absolute_limit": absolute_limit,
        "slope_limit": slope_limit,
        "suspect_growth": suspect_growth,
        "classification": classification,
    }


def trend(points: list[dict[str, Any]], metric: str) -> dict[str, Any]:
    return trend_from_points(points, metric, 0)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-url", default="http://127.0.0.1:8765")
    parser.add_argument("--iterations", type=int, default=8)
    parser.add_argument("--scenario", choices=SCENARIOS, default="mixed")
    parser.add_argument("--settle", type=float, default=3.0)
    parser.add_argument("--warmup", type=float, default=12.0)
    parser.add_argument("--checkpoint-settle", type=float, default=7.0)
    parser.add_argument("--sample-interval", type=float, default=1.0)
    parser.add_argument("--pmon-every", type=int, default=5)
    parser.add_argument(
        "--steady-warmup-iterations",
        type=int,
        default=3,
        help="checkpoint iterations excluded from the steady-state leak trend",
    )
    parser.add_argument("--api-timeout", type=float, default=45.0)
    parser.add_argument("--project-a", default=DEFAULT_PROJECT_A)
    parser.add_argument("--project-b", default=DEFAULT_PROJECT_B)
    parser.add_argument("--project-a-type", choices=("scene", "video", "web"), default="scene")
    parser.add_argument("--project-b-type", choices=("scene", "video", "web"), default="scene")
    parser.add_argument("--output-dir", type=pathlib.Path, required=True)
    args = parser.parse_args()

    if args.iterations < 1:
        parser.error("--iterations must be positive")
    if args.steady_warmup_iterations < 0:
        parser.error("--steady-warmup-iterations cannot be negative")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    base_url = args.base_url.rstrip("/")
    initial_state = producer_state(base_url, args.api_timeout)
    (args.output_dir / "initial_state.json").write_text(
        json.dumps(initial_state, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    primary, secondary = choose_outputs(initial_state)
    initial_config = state_config(initial_state)
    restore_patch = {
        "multi-display-mode": initial_config.get("multi-display-mode", "clone"),
        "primary-display-key": initial_config.get("primary-display-key", primary),
        "per-output-projects": initial_config.get("per-output-projects", {}),
    }

    sampler = Sampler(args.output_dir, args.sample_interval, args.pmon_every)
    actions_path = args.output_dir / "actions.jsonl"
    actions_stream = actions_path.open("w", encoding="utf-8")
    sampler.start()
    try:
        warmup_mode = "independent" if args.scenario == "both-project" else "clone"
        run_action(
            base_url,
            sampler,
            actions_stream,
            -1,
            "warmup-config",
            "/api/config",
            {"multi-display-mode": warmup_mode, "primary-display-key": primary},
            args.api_timeout,
            0.5,
        )
        if args.scenario == "both-project":
            run_action(
                base_url,
                sampler,
                actions_stream,
                -1,
                "warmup-primary-select-a",
                "/api/wallpaper/select",
                {
                    "displayKey": primary,
                    "projectPath": args.project_a,
                    "projectType": args.project_a_type,
                },
                args.api_timeout,
                args.settle,
            )
            run_action(
                base_url,
                sampler,
                actions_stream,
                -1,
                "warmup-secondary-select-a",
                "/api/wallpaper/select",
                {
                    "displayKey": secondary,
                    "projectPath": args.project_a,
                    "projectType": args.project_a_type,
                },
                args.api_timeout,
                args.settle,
            )
        wait_seconds(sampler, args.warmup, "warmup", -1)

        for iteration in range(args.iterations):
            def action(name: str, path: str, body: dict[str, Any], settle: float = args.settle) -> None:
                run_action(
                    base_url,
                    sampler,
                    actions_stream,
                    iteration,
                    name,
                    path,
                    body,
                    args.api_timeout,
                    settle,
                )

            if args.scenario in {"mixed", "mode-only", "secondary-wallpaper", "secondary-project", "primary-wallpaper"}:
                action(
                    "mode-independent",
                    "/api/config",
                    {"multi-display-mode": "independent", "primary-display-key": primary},
                )

            if args.scenario in {"mixed", "secondary-wallpaper"}:
                action("secondary-remove", "/api/wallpaper/remove", {"displayKey": secondary})
                action(
                    "secondary-select-a",
                    "/api/wallpaper/select",
                    {"displayKey": secondary, "projectPath": args.project_a, "projectType": args.project_a_type},
                )
                action(
                    "secondary-select-b",
                    "/api/wallpaper/select",
                    {"displayKey": secondary, "projectPath": args.project_b, "projectType": args.project_b_type},
                )
                action("secondary-remove-again", "/api/wallpaper/remove", {"displayKey": secondary})
                action(
                    "secondary-select-a-again",
                    "/api/wallpaper/select",
                    {"displayKey": secondary, "projectPath": args.project_a, "projectType": args.project_a_type},
                )
            elif args.scenario == "secondary-project":
                action(
                    "secondary-select-a",
                    "/api/wallpaper/select",
                    {"displayKey": secondary, "projectPath": args.project_a, "projectType": args.project_a_type},
                )
                action(
                    "secondary-select-b",
                    "/api/wallpaper/select",
                    {"displayKey": secondary, "projectPath": args.project_b, "projectType": args.project_b_type},
                )
                action(
                    "secondary-select-a-again",
                    "/api/wallpaper/select",
                    {"displayKey": secondary, "projectPath": args.project_a, "projectType": args.project_a_type},
                )
            elif args.scenario == "both-project":
                action(
                    "primary-select-b",
                    "/api/wallpaper/select",
                    {"displayKey": primary, "projectPath": args.project_b, "projectType": args.project_b_type},
                )
                action(
                    "secondary-select-b",
                    "/api/wallpaper/select",
                    {"displayKey": secondary, "projectPath": args.project_b, "projectType": args.project_b_type},
                )
                action(
                    "primary-select-a",
                    "/api/wallpaper/select",
                    {"displayKey": primary, "projectPath": args.project_a, "projectType": args.project_a_type},
                )
                action(
                    "secondary-select-a",
                    "/api/wallpaper/select",
                    {"displayKey": secondary, "projectPath": args.project_a, "projectType": args.project_a_type},
                )
            elif args.scenario == "primary-wallpaper":
                action(
                    "primary-select-a",
                    "/api/wallpaper/select",
                    {"displayKey": primary, "projectPath": args.project_a, "projectType": args.project_a_type},
                )
                action(
                    "primary-select-b",
                    "/api/wallpaper/select",
                    {"displayKey": primary, "projectPath": args.project_b, "projectType": args.project_b_type},
                )
                action(
                    "primary-select-a-again",
                    "/api/wallpaper/select",
                    {"displayKey": primary, "projectPath": args.project_a, "projectType": args.project_a_type},
                )

            if args.scenario in {"mixed", "mode-only", "secondary-wallpaper", "secondary-project", "primary-wallpaper"}:
                action(
                    "mode-clone",
                    "/api/config",
                    {"multi-display-mode": "clone", "primary-display-key": primary},
                    args.checkpoint_settle,
                )
            elif args.scenario == "both-project":
                wait_seconds(sampler, args.checkpoint_settle, "checkpoint", iteration)
            else:
                wait_seconds(sampler, args.checkpoint_settle, "checkpoint", iteration)

            # Keep a compact API-state snapshot at every checkpoint.  This lets
            # the report prove that the test still had exactly two outputs and
            # which project was assigned when a memory sample was taken.
            try:
                checkpoint_state = producer_state(base_url, args.api_timeout)
                (args.output_dir / f"state-{iteration:04d}.json").write_text(
                    json.dumps(checkpoint_state, ensure_ascii=False, indent=2), encoding="utf-8"
                )
            except Exception as error:
                print(f"STATE SNAPSHOT FAILED iteration={iteration}: {error}", file=sys.stderr)

        sampler.set_phase("final-observation", args.iterations - 1)
        wait_seconds(sampler, args.warmup, "final-observation", args.iterations - 1)
    finally:
        try:
            status, response = post_json(f"{base_url}/api/config", restore_patch, args.api_timeout)
            actions_stream.write(
                json.dumps(
                    {
                        "timestamp": now_wall(),
                        "iteration": -1,
                        "action": "restore-original-config",
                        "ok": True,
                        "status": status,
                        "response": response,
                    },
                    ensure_ascii=False,
                )
                + "\n"
            )
        except Exception as error:
            print(f"RESTORE FAILED: {error}", file=sys.stderr)
        sampler.join()
        actions_stream.close()

    samples_path = sampler.write_csv()
    checkpoints = checkpoint_rows(sampler.rows)
    trend_metrics = (
        "producer_rss_mb",
        "producer_pss_mb",
        "producer_private_dirty_mb",
        "producer_anonymous_mb",
        "producer_anon_huge_pages_mb",
        "producer_swap_mb",
        "producer_fd_count",
        "producer_fd_render_d128",
        "producer_fd_nvidiactl",
        "producer_fd_nvidia_device",
        "producer_fd_dmabuf",
        "producer_fd_sync_file",
        "producer_fd_syncobj_file",
        "producer_thread_count",
        "producer_tree_count",
        "producer_gpu_fb_mb",
        "gpu_mem_used_mb",
        "plasmashell_rss_mb",
        "plasmashell_pss_mb",
        "plasmashell_private_dirty_mb",
        "plasmashell_anonymous_mb",
        "plasmashell_anon_huge_pages_mb",
        "plasmashell_swap_mb",
        "plasmashell_fd_count",
        "plasmashell_fd_render_d128",
        "plasmashell_fd_nvidiactl",
        "plasmashell_fd_nvidia_device",
        "plasmashell_fd_dmabuf",
        "plasmashell_fd_sync_file",
        "plasmashell_fd_syncobj_file",
        "plasmashell_thread_count",
        "plasmashell_gpu_fb_mb",
        "web_helper_rss_mb",
    )
    steady_start_index = min(args.steady_warmup_iterations, max(0, len(checkpoints) - 2))
    trends = [
        trend_from_points(checkpoints, metric, steady_start_index)
        for metric in trend_metrics
    ]
    summary = {
        "created": now_wall(),
        "output_dir": str(args.output_dir),
        "iterations": args.iterations,
        "scenario": args.scenario,
        "settle_sec": args.settle,
        "sample_interval_sec": args.sample_interval,
        "steady_warmup_iterations": steady_start_index,
        "primary_display_key": primary,
        "secondary_display_key": secondary,
        "projects": {
            "a": {"path": args.project_a, "type": args.project_a_type},
            "b": {"path": args.project_b, "type": args.project_b_type},
        },
        "sample_count": len(sampler.rows),
        "artifacts": {"samples": str(samples_path), "actions": str(actions_path)},
        "overall": {
            metric: summary_for(sampler.rows, metric)
            for metric in (
                "producer_cpu_pct",
                "producer_rss_mb",
                "producer_pss_mb",
                "producer_private_dirty_mb",
                "producer_anonymous_mb",
                "producer_anon_huge_pages_mb",
                "producer_swap_mb",
                "producer_fd_count",
                "producer_thread_count",
                "producer_tree_count",
                "producer_tree_rss_mb",
                "plasmashell_cpu_pct",
        "plasmashell_rss_mb",
        "plasmashell_pss_mb",
        "plasmashell_private_dirty_mb",
        "plasmashell_anonymous_mb",
        "plasmashell_anon_huge_pages_mb",
        "plasmashell_swap_mb",
        "plasmashell_fd_count",
        "plasmashell_thread_count",
        "web_helper_cpu_pct",
        "web_helper_rss_mb",
        "web_helper_pss_mb",
        "web_helper_private_dirty_mb",
        "web_helper_anonymous_mb",
        "web_helper_fd_count",
        "web_helper_thread_count",
                "producer_gpu_fb_mb",
                "plasmashell_gpu_fb_mb",
                "gpu_util_pct",
                "gpu_mem_used_mb",
                "gpu_mem_free_mb",
                "gpu_mem_total_mb",
                "gpu_power_w",
                "gpu_temp_c",
            )
        },
        "checkpoints": checkpoints,
        "trends": trends,
    }
    (args.output_dir / "summary.json").write_text(
        json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
