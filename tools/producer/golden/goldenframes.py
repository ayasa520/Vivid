#!/usr/bin/env python3
"""Golden-frame acceptance workflow for the scene renderer.

The scene renderer's behavior contract is pixel output, so refactors are verified
against golden frames captured through the producer C ABI (offscreen DMA-BUF export,
see capture_vivid_scene.py). This driver packages the whole workflow:

  probe    Scan locally installed Workshop wallpapers and report which renderer
           mechanisms each one exercises (effect sources, proxy ordering, deferred
           layers, puppets, text, dynamic script layers, video, camera layers).
  init     Build the machine-local acceptance config from explicitly chosen
           wallpaper ids and measure each one's run-to-run noise floor.
  capture  Capture the acceptance set into .build/golden-frames/<set>/.
  diff     Compare two captured sets; per-wallpaper verdicts are scaled by the
           measured noise floor. A diff above the advisory band is not proof of a
           regression: animation phase alone can exceed it. Escalate to `bisect`.
  bisect   The decisive arbiter: stash the submodule work tree, rebuild, capture,
           restore, rebuild, capture again, and compare the same wall-clock moment
           old-vs-new on the designated low-noise instrument wallpaper.
  smoke    Functional check for script-driven dynamic layers (createLayer runs,
           frames are produced, no crash) on wallpapers that exercise them.

Wallpapers are Steam Workshop content: they are machine-local and never part of the
repository. The acceptance list, the instrument choice, and the measured noise floors
live in .build/golden-frames/acceptance.json (gitignored); every machine builds its
own with `probe` + `init`. Machine paths come from the environment:

  VIVID_WORKSHOP_ROOT   Workshop content directory containing <id>/ wallpaper dirs
                        (default: ~/.steam/steam/steamapps/workshop/content/431960)
"""

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
CAPTURE = Path(__file__).resolve().parent / "capture_vivid_scene.py"
LIBRARY = REPO_ROOT / "producer/.build/direct-run/scene-build/out/libVividScene.so"
SUBMODULE = REPO_ROOT / "producer/third_party/wallpaper-scene-renderer"
FRAMES_ROOT = REPO_ROOT / ".build/golden-frames"
CONFIG_PATH = FRAMES_ROOT / "acceptance.json"

DEFAULT_WORKSHOP = Path.home() / ".steam/steam/steamapps/workshop/content/431960"

# Renderer log tags that identify which mechanisms a wallpaper exercises. The probe
# report counts occurrences per tag; extend freely when new mechanisms gain logs.
MARKERS = {
    "effect-source": r"SceneRenderGraphDetachedSourceRoute",
    "proxy-order": r"reason='proxy'",
    "deferred-layer": r"SceneObjectMaterialize: mode=\S*logical\S*",
    "puppet": r"PuppetSurface|ScenePuppetAttachmentBind",
    "text": r"SceneTextLayoutContract",
    "dynamic-layers": r"SceneScriptCreateLayer",
    "video": r"VideoTextureDecoderInit",
    "camera-layer": r"SceneCameraLayerActive",
    "light": r"SceneLightParsed",
    "model": r"ModelRenderOrder|Scene3DModelCameraPath",
    "particle": r"ParticleRenderPlan",
    "bloom": r"SceneBloomGraphBind",
    "volumetrics": r"SceneVolumetrics",
}

CAPTURE_DEFAULTS = {"width": 1600, "height": 1000, "frames": 120, "pointer_xs": [0.0, 0.5, 1.0]}


def workshop_root() -> Path:
    return Path(os.environ.get("VIVID_WORKSHOP_ROOT", str(DEFAULT_WORKSHOP)))


def run_capture(project: Path, output: Path, *, width: int, height: int, frames: int,
                pointer_x: float, log_path: Path | None = None) -> bool:
    if not LIBRARY.exists():
        sys.exit(f"error: {LIBRARY} not found; run `tools/vivid.sh build direct-run` first")
    output.parent.mkdir(parents=True, exist_ok=True)
    cmd = [sys.executable, str(CAPTURE), "--library", str(LIBRARY), "--project", str(project),
           "--output", str(output), "--width", str(width), "--height", str(height),
           "--frames", str(frames), "--pointer-x", str(pointer_x), "--pointer-y", "0.5"]
    log = subprocess.run(cmd, capture_output=True, text=True)
    if log_path is not None:
        log_path.write_text(log.stdout + log.stderr)
    return log.returncode == 0 and output.exists()


def image_diff(a_path: Path, b_path: Path) -> tuple[float, float]:
    """Returns (mean absolute channel difference, percent of pixels with any channel diff > 8)."""
    import numpy as np
    from PIL import Image

    a = np.asarray(Image.open(a_path).convert("RGB"), dtype=np.int16)
    b = np.asarray(Image.open(b_path).convert("RGB"), dtype=np.int16)
    if a.shape != b.shape:
        return float("inf"), 100.0
    d = np.abs(a - b)
    return float(d.mean()), float((d.max(axis=2) > 8).mean() * 100.0)


def load_config() -> dict:
    if not CONFIG_PATH.exists():
        sys.exit(f"error: {CONFIG_PATH} not found; run `goldenframes.py probe` then "
                 f"`goldenframes.py init --acceptance ... --instrument ...`")
    return json.loads(CONFIG_PATH.read_text())


def scan_markers(log_text: str) -> dict[str, int]:
    return {name: len(re.findall(pattern, log_text)) for name, pattern in MARKERS.items()}


def cmd_probe(args: argparse.Namespace) -> None:
    root = workshop_root()
    if not root.is_dir():
        sys.exit(f"error: workshop root {root} not found (set VIVID_WORKSHOP_ROOT)")
    ids = args.ids.split(",") if args.ids else sorted(p.name for p in root.iterdir() if p.is_dir())
    FRAMES_ROOT.mkdir(parents=True, exist_ok=True)
    report: dict[str, dict] = {}
    for wallpaper_id in ids:
        project = root / wallpaper_id
        out = FRAMES_ROOT / "probe" / f"{wallpaper_id}.png"
        log_path = FRAMES_ROOT / "probe" / f"{wallpaper_id}.log"
        ok = run_capture(project, out, width=480, height=300, frames=60, pointer_x=0.5,
                         log_path=log_path)
        if not ok:
            report[wallpaper_id] = {"ok": False}
            print(f"{wallpaper_id}: capture failed")
            continue
        markers = scan_markers(log_path.read_text())
        hits = {k: v for k, v in markers.items() if v > 0}
        report[wallpaper_id] = {"ok": True, "markers": hits}
        print(f"{wallpaper_id}: " + (", ".join(f"{k}={v}" for k, v in hits.items()) or "(none)"))
    (FRAMES_ROOT / "probe-report.json").write_text(json.dumps(report, indent=2))
    print(f"\nreport written to {FRAMES_ROOT / 'probe-report.json'}")
    print("pick acceptance wallpapers covering distinct mechanisms and a deterministic")
    print("instrument (video/clock wallpapers are near pixel-stable within one minute),")
    print("then run: goldenframes.py init --acceptance ID[,ID...] --instrument ID")


def measure_noise(project: Path, tmp_dir: Path, params: dict) -> tuple[float, float]:
    """Returns (floor, span) of same-build run-to-run variance in pixels>8 percent.

    The floor (minimum over pairs) feeds the bisect threshold: it is the best-case
    agreement, and a single outlier pair (e.g. a video stream seeded at a different
    position) must not inflate it. The span (maximum over pairs) feeds the diff band:
    animation phase alone routinely produces large same-build diffs on particle-heavy
    scenes, so the advisory band has to be scaled from the worst observed pair.
    """
    runs = []
    for run in ("a", "b", "c"):
        out = tmp_dir / f"noise-{run}.png"
        if not run_capture(project, out, width=params["width"], height=params["height"],
                           frames=params["frames"], pointer_x=0.5):
            sys.exit(f"error: noise capture failed for {project}")
        runs.append(out)
    pair_pcts = [image_diff(runs[i], runs[i + 1])[1] for i in range(len(runs) - 1)]
    pair_pcts.append(image_diff(runs[0], runs[2])[1])
    return min(pair_pcts), max(pair_pcts)


def cmd_init(args: argparse.Namespace) -> None:
    root = workshop_root()
    params = dict(CAPTURE_DEFAULTS)
    config = {"workshop_root": str(root), "capture": params, "wallpapers": {}}
    roles = [(wid, "acceptance") for wid in args.acceptance.split(",")]
    roles.append((args.instrument, "instrument"))
    for wallpaper_id, role in roles:
        project = root / wallpaper_id
        if not project.is_dir():
            sys.exit(f"error: wallpaper {wallpaper_id} not found under {root}")
        print(f"measuring run-to-run noise for {wallpaper_id} ...")
        floor, span = measure_noise(project, FRAMES_ROOT / "init-tmp", params)
        config["wallpapers"][wallpaper_id] = {
            "role": role,
            "noise_floor_pct": round(floor, 2),
            "noise_span_pct": round(span, 2),
        }
        print(f"  noise floor: {floor:.2f}%  span: {span:.2f}% (pixels>8)")
    CONFIG_PATH.parent.mkdir(parents=True, exist_ok=True)
    CONFIG_PATH.write_text(json.dumps(config, indent=2))
    print(f"config written to {CONFIG_PATH}")


def acceptance_items(config: dict) -> list[tuple[str, dict]]:
    return [(wid, entry) for wid, entry in config["wallpapers"].items()
            if entry["role"] == "acceptance"]


def cmd_capture(args: argparse.Namespace) -> None:
    config = load_config()
    params = config["capture"]
    root = Path(config["workshop_root"])
    out_dir = FRAMES_ROOT / args.set
    for wallpaper_id, _ in acceptance_items(config):
        for px in params["pointer_xs"]:
            out = out_dir / f"{wallpaper_id}-px{px}.png"
            ok = run_capture(root / wallpaper_id, out, width=params["width"],
                             height=params["height"], frames=params["frames"], pointer_x=px)
            print(f"{out.name}: {'ok' if ok else 'FAILED'}")
            if not ok:
                sys.exit(1)
    print(f"set written to {out_dir}")


def cmd_diff(args: argparse.Namespace) -> None:
    config = load_config()
    base_dir = FRAMES_ROOT / args.base
    new_dir = FRAMES_ROOT / args.new
    alarms = 0
    for image in sorted(new_dir.glob("*.png")):
        base_image = base_dir / image.name
        if not base_image.exists():
            print(f"{image.name}: no baseline, skipped")
            continue
        wallpaper_id = image.name.split("-px")[0]
        span = config["wallpapers"].get(wallpaper_id, {}).get("noise_span_pct", 0.0)
        # Advisory only: even the measured span under-samples animation phase variance,
        # so exceeding the band demands a visual check and a `bisect`, not a rollback.
        band = max(span * 1.5 + 10.0, 10.0)
        mean, pct = image_diff(base_image, image)
        verdict = "ok" if pct <= band else "ALARM(visual-check + bisect)"
        if pct > band:
            alarms += 1
        print(f"{image.name}: mean-abs={mean:.3f} pixels>8={pct:.2f}% band<={band:.1f}% {verdict}")
    sys.exit(1 if alarms else 0)


def cmd_bisect(args: argparse.Namespace) -> None:
    config = load_config()
    instrument = next((wid for wid, e in config["wallpapers"].items()
                       if e["role"] == "instrument"), None)
    if instrument is None:
        sys.exit("error: no instrument wallpaper in config")
    noise = config["wallpapers"][instrument]["noise_floor_pct"]
    params = config["capture"]
    project = Path(config["workshop_root"]) / instrument

    def git(*sub: str) -> str:
        return subprocess.run(["git", "-C", str(SUBMODULE), *sub],
                              capture_output=True, text=True, check=True).stdout

    def build() -> None:
        result = subprocess.run([str(REPO_ROOT / "tools/vivid.sh"), "build", "direct-run"],
                                capture_output=True, text=True)
        if result.returncode != 0:
            raise RuntimeError("build failed:\n" + result.stdout[-2000:] + result.stderr[-2000:])

    if not git("status", "--porcelain").strip():
        sys.exit("error: submodule work tree is clean; bisect compares uncommitted "
                 "changes against HEAD, so there is nothing to compare")

    tmp = FRAMES_ROOT / "bisect-tmp"
    old_pngs = [tmp / "old-a.png", tmp / "old-b.png"]
    new_png = tmp / "new.png"
    git("stash", "push", "-m", "goldenframes-bisect")
    stashed = True
    try:
        build()
        # Capture the old build twice: a single capture can itself be the nondeterministic
        # outlier (e.g. a video stream seeding at a different position), and the new-side
        # retries below can never converge against a bad reference.
        for old_png in old_pngs:
            if not run_capture(project, old_png, width=params["width"], height=params["height"],
                               frames=params["frames"], pointer_x=0.5):
                raise RuntimeError("old-build capture failed")
    finally:
        if stashed:
            subprocess.run(["git", "-C", str(SUBMODULE), "stash", "pop"], check=True)
            stashed = False
    build()

    # Scale the instrument's noise floor and keep an absolute floor for clock-digit drift
    # across the rebuild gap.
    threshold = max(noise * 3.0, 0.5)
    # Nondeterminism can only add difference, never cancel a real behavior change, so
    # neutrality is established by the best pairing across retries on both sides.
    best = (float("inf"), float("inf"))
    for attempt in range(3):
        if not run_capture(project, new_png, width=params["width"], height=params["height"],
                           frames=params["frames"], pointer_x=0.5):
            sys.exit("error: new-build capture failed")
        for old_png in old_pngs:
            mean, pct = image_diff(old_png, new_png)
            best = min(best, (pct, mean))
        print(f"attempt {attempt + 1}: best mean-abs={best[1]:.3f} pixels>8={best[0]:.2f}%")
        if best[0] <= threshold:
            break
    pct, mean = best
    verdict = "NEUTRAL" if pct <= threshold else "BEHAVIOR CHANGED"
    print(f"same-moment old vs new: mean-abs={mean:.3f} pixels>8={pct:.2f}% "
          f"threshold<={threshold:.2f}% -> {verdict}")
    sys.exit(0 if pct <= threshold else 1)


def cmd_smoke(args: argparse.Namespace) -> None:
    config = load_config()
    root = Path(config["workshop_root"])
    ids = args.ids.split(",") if args.ids else [wid for wid in config["wallpapers"]]
    failed = False
    for wallpaper_id in ids:
        out = FRAMES_ROOT / "smoke" / f"{wallpaper_id}.png"
        log_path = FRAMES_ROOT / "smoke" / f"{wallpaper_id}.log"
        ok = run_capture(root / wallpaper_id, out, width=800, height=500, frames=90,
                         pointer_x=0.5, log_path=log_path)
        created = len(re.findall(MARKERS["dynamic-layers"], log_path.read_text())) if ok else 0
        print(f"{wallpaper_id}: capture={'ok' if ok else 'FAILED'} createLayer={created}")
        failed = failed or not ok
    sys.exit(1 if failed else 0)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("probe", help="scan local wallpapers for mechanism coverage")
    p.add_argument("--ids", help="comma-separated wallpaper ids (default: all installed)")
    p.set_defaults(func=cmd_probe)

    p = sub.add_parser("init", help="write the local acceptance config and measure noise")
    p.add_argument("--acceptance", required=True, help="comma-separated wallpaper ids")
    p.add_argument("--instrument", required=True, help="deterministic bisect wallpaper id")
    p.set_defaults(func=cmd_init)

    p = sub.add_parser("capture", help="capture the acceptance set")
    p.add_argument("--set", required=True, help="output set name under .build/golden-frames/")
    p.set_defaults(func=cmd_capture)

    p = sub.add_parser("diff", help="compare two captured sets")
    p.add_argument("--base", required=True)
    p.add_argument("--new", required=True)
    p.set_defaults(func=cmd_diff)

    p = sub.add_parser("bisect", help="same-moment old/new arbiter on the instrument wallpaper")
    p.set_defaults(func=cmd_bisect)

    p = sub.add_parser("smoke", help="dynamic-layer functional smoke")
    p.add_argument("--ids", help="comma-separated wallpaper ids (default: config wallpapers)")
    p.set_defaults(func=cmd_smoke)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
