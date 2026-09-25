#!/usr/bin/env python3
"""Collect Vivid diagnostics locally using only the Python standard library."""

import argparse
import datetime
import json
import os
from pathlib import Path
import platform
import signal
import subprocess
import sys
import tarfile
import tempfile
import time


def timestamp():
    return datetime.datetime.now().astimezone().isoformat(timespec="seconds")


def host_package_query():
    # Extra package-manager executables can be installed on any distribution. Select the
    # query from the host's declared OS family so it addresses that system's package database.
    release = platform.freedesktop_os_release()
    families = [release["ID"], *release.get("ID_LIKE", "").split()]
    queries = {
        "fedora": ["rpm", "-q", "gnome-shell", "mutter", "mesa-vulkan-drivers",
                   "mesa-dri-drivers", "flatpak", "vulkan-tools", "kernel-core"],
        "debian": ["dpkg-query", "-W", "gnome-shell", "mutter", "mesa-vulkan-drivers",
                   "libgl1-mesa-dri", "flatpak", "vulkan-tools"],
        "arch": ["pacman", "-Q", "gnome-shell", "mutter", "mesa", "vulkan-radeon",
                 "vulkan-intel", "nvidia-utils", "flatpak", "vulkan-tools"],
    }
    for family in families:
        if family in queries:
            return queries[family]
    return None


class Collector:
    def __init__(self, directory, timeout):
        self.directory = directory
        self.timeout = timeout
        self.records = []

    def run(self, name, argv):
        print(f"Collecting {name} ...", flush=True)
        record = {"file": name, "argv": argv, "started": timestamp()}
        started = time.monotonic()
        with (self.directory / name).open("w") as output:
            try:
                # Each probe owns its process group. On timeout, terminate its
                # children too so an inherited output FD cannot stall collection.
                # Output goes directly to disk, including diagnostics on stderr.
                process = subprocess.Popen(
                    argv, stdout=output, stderr=subprocess.STDOUT,
                    start_new_session=True,
                )
                try:
                    record["returncode"] = process.wait(timeout=self.timeout)
                    record["status"] = "ok" if process.returncode == 0 else "error"
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
                    record.update(status="timeout", returncode=process.returncode)
                    output.write(f"\nCollector: timed out after {self.timeout}s\n")
                except BaseException:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
                    raise
            except OSError as error:
                record.update(status="unavailable", error=str(error))
                output.write(f"Collector: {error}\n")
        record["duration_seconds"] = round(time.monotonic() - started, 3)
        self.records.append(record)

    def copy(self, name, source):
        record = {"file": name, "source": str(source)}
        try:
            # Copy only explicitly selected files; wallpaper assets and unrelated
            # application data must never enter the archive through directory walks.
            (self.directory / name).write_bytes(source.read_bytes())
            record["status"] = "ok"
        except OSError as error:
            record.update(status="unavailable", error=str(error))
        self.records.append(record)

    def configs(self, phase, app_id, extra):
        home = Path.home()
        config_home = Path(os.environ.get("XDG_CONFIG_HOME", home / ".config"))
        sources = [
            ("native", config_home / "vivid-producer/config-v1.json"),
            ("flatpak", home / ".var/app" / app_id / "config/vivid-producer/config-v1.json"),
        ]
        sources.extend((f"extra-{index}", path) for index, path in enumerate(extra))
        for label, source in sources:
            self.copy(f"config-{phase}-{label}.json", source)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, default=Path.cwd())
    parser.add_argument("--app-id", default="io.github.ayasa520.Vivid")
    parser.add_argument("--extension-uuid", default="vivid-consumer-gnome@rikka.local")
    parser.add_argument("--config", type=Path, action="append", default=[],
                        help="Additional producer config file (repeatable)")
    parser.add_argument("--timeout", type=float, default=60,
                        help="Timeout in seconds for each command (default: 60)")
    parser.add_argument("--no-wait", action="store_true",
                        help="Collect an already reproduced problem without prompts")
    parser.add_argument("--since", default="10 minutes ago",
                        help="Journal start for --no-wait (default: 10 minutes ago)")
    parser.add_argument("--notes", default="", help="Wallpaper IDs, symptoms and display settings")
    args = parser.parse_args()
    if not 0 < args.timeout < float("inf"):
        parser.error("--timeout must be finite and positive")
    if "/" in args.app_id or args.app_id in ("", ".", ".."):
        parser.error("--app-id must be an application ID, not a path")
    if not args.no_wait and not sys.stdin.isatty():
        parser.error("Use --no-wait when stdin is not a terminal")
    if os.geteuid() == 0:
        parser.error("Run as the desktop user so session and Flatpak information match Vivid")

    # Restrict both the raw report and archive to the collecting user. Keep the
    # raw report after packaging so failed collection/packaging is inspectable.
    os.umask(0o077)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    directory = Path(tempfile.mkdtemp(
        prefix=f"vivid-debug-{datetime.datetime.now():%Y%m%d-%H%M%S}-",
        dir=args.output_dir.resolve(),
    ))
    collector = Collector(directory, args.timeout)
    metadata = {"started": timestamp(), "app_id": args.app_id, "notes": args.notes}
    # Freeze the end of an existing incident before slow Vulkan probes run.
    # Interactive captures instead bracket the user's reproduction explicitly.
    since, until = args.since, timestamp()
    print(f"Report directory: {directory}")
    print("Logs can contain usernames, paths and desktop application messages. Review before sharing.")
    collector.configs("before", args.app_id, args.config)
    if not args.no_wait:
        input("准备好复现壁纸问题后按 Enter 开始记录复现时间：")
        since = timestamp()
        print("依次播放正常壁纸 → 故障壁纸 → 正常壁纸，每个约 15 秒。记录壁纸 ID 和表现。")
        input("复现完成后按 Enter：")
        until = timestamp()
        notes = input("补充壁纸 ID、黑屏/卡住、HDR/VRR/缩放、抗锯齿设置（可留空）：")
        metadata["notes"] += "\n" + notes
    metadata.update(journal_since=since, journal_until=until)
    collector.configs("after", args.app_id, args.config)
    for name, argv in [
        ("journal-user.txt", ["journalctl", "--user", "-b", "--since", since,
                              "--until", until, "-o", "short-precise", "--no-pager"]),
        ("journal-kernel.txt", ["journalctl", "-k", "-b", "--since", since,
                                "--until", until, "-o", "short-precise", "--no-pager"]),
        ("uname.txt", ["uname", "-a"]),
        ("pci.txt", ["lspci", "-nnk"]),
        ("dri-devices.txt", ["ls", "-l", "/dev/dri", "/dev/dri/by-path"]),
        ("vulkan-host.txt", ["vulkaninfo", "--show-formats"]),
        ("flatpak-info.txt", ["flatpak", "info", args.app_id]),
        ("flatpak-extensions.txt", ["flatpak", "info", "--show-extensions", args.app_id]),
        ("flatpak-runtimes.txt", ["flatpak", "list", "--runtime",
                                  "--columns=application,arch,branch,version,active"]),
        ("flatpak-overrides-user.txt", ["flatpak", "override", "--user", "--show"]),
        ("flatpak-overrides-system.txt", ["flatpak", "override", "--system", "--show"]),
        ("flatpak-overrides-app-user.txt", ["flatpak", "override", "--user", "--show", args.app_id]),
        ("flatpak-overrides-app-system.txt", ["flatpak", "override", "--system", "--show", args.app_id]),
        ("flatpak-environment.txt", ["flatpak", "run", "--command=cat", args.app_id, "/.flatpak-info"]),
        ("vulkan-flatpak.txt", ["flatpak", "run", "--command=vulkaninfo", args.app_id, "--show-formats"]),
        ("gnome-version.txt", ["gnome-shell", "--version"]),
        ("gnome-extension.txt", ["gnome-extensions", "info", args.extension_uuid]),
        ("gnome-extensions-enabled.txt", ["gnome-extensions", "list", "--enabled"]),
        ("gnome-displays.txt", ["gdbus", "call", "--session", "--dest", "org.gnome.Mutter.DisplayConfig",
                                "--object-path", "/org/gnome/Mutter/DisplayConfig",
                                "--method", "org.gnome.Mutter.DisplayConfig.GetCurrentState"]),
    ]:
        collector.run(name, argv)
    collector.copy("os-release.txt", Path("/etc/os-release"))
    package_query = host_package_query()
    if package_query:
        collector.run("packages.txt", package_query)
    environment_keys = (
        "XDG_SESSION_TYPE", "XDG_CURRENT_DESKTOP", "XDG_SESSION_DESKTOP", "DESKTOP_SESSION",
        "DISPLAY", "WAYLAND_DISPLAY", "DRI_PRIME", "MESA_LOADER_DRIVER_OVERRIDE",
        "VK_DRIVER_FILES", "VK_ICD_FILENAMES", "VK_INSTANCE_LAYERS", "VK_LAYER_PATH",
        "AMD_VULKAN_ICD", "RADV_PERFTEST", "FLATPAK_GL_DRIVERS",
    )
    # Deliberately select graphics/session variables rather than exporting the
    # entire environment, which may contain unrelated credentials.
    metadata["environment"] = {key: os.environ[key] for key in environment_keys if key in os.environ}
    metadata.update(finished=timestamp(), probes=collector.records)
    (directory / "manifest.json").write_text(json.dumps(metadata, ensure_ascii=False, indent=2) + "\n")
    archive = directory.with_suffix(".tar.gz")
    with archive.open("xb") as output:
        with tarfile.open(fileobj=output, mode="w:gz") as bundle:
            bundle.add(directory, arcname=directory.name)
    failures = sum(record["status"] != "ok" for record in collector.records)
    print(f"\nArchive: {archive}\nRaw report: {directory}")
    print(f"Unavailable/failed probes: {failures}; see manifest.json and individual logs.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
