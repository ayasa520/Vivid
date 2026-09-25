#!/usr/bin/env python3
"""Deterministic golden-frame regression harness for the scene renderer.

Every capture runs the renderer in lockstep mode (see capture_vivid_scene.py --lockstep): the
harness owns the frame clock, seeds and calendar clock, so two runs of the same scenario are
byte-identical. Regressions are therefore detected by exact comparison first and by a small
per-tier tolerance only where a wallpaper class is documented as non-reproducible (video
textures). Failures are explained by the structural frame trace when the renderer wrote one.

Layout
  tools/producer/golden/tiers.json        committed: wallpaper ids per tier, per-id overrides
  tools/producer/golden/scenarios/*.json  committed: input vectors (size, steps, events, ...)
  .build/golden/config.json               machine-local: self-check results per wallpaper
  .build/golden/baseline/<id>/<scenario>/ machine-local accepted frames + manifest + history
  .build/golden/runs/<run-id>/            machine-local candidates, diffs, reports, packets

Commands
  probe     capture each wallpaper briefly and record which renderer mechanisms it exercises
  init      determinism self-check: capture N times, require identical hashes, classify
  run       capture the tier, compare with the baseline, write a report; exit 1 on failure
  promote   accept candidate frames of a run as the new baseline (records the reason)
  packet    build a bounded review packet (composite + crops + trace diff) for one failure
  bisect-run  build the renderer and run one tier; usable as `git bisect run`
  status    list baseline entries with their renderer commit and environment fingerprints

Workshop content is machine-local (VIVID_WORKSHOP_ROOT) and never part of the repository.
Set that variable or the workshop_root field in .build/golden/config.json before selecting
Workshop wallpapers. Fixture captures may instead use VIVID_SCENE_ASSETS_DIR directly.
"""

import argparse
import datetime as dt
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
GOLDEN_DIR = Path(__file__).resolve().parent
CAPTURE = GOLDEN_DIR / "capture_vivid_scene.py"
SCENARIOS_DIR = GOLDEN_DIR / "scenarios"
FIXTURE_GENERATOR = GOLDEN_DIR / "fixtures" / "generate.py"
TIERS_PATH = GOLDEN_DIR / "tiers.json"
LIBRARY = REPO_ROOT / "producer/.build/direct-run/scene-build/out/libVividScene.so"
SUBMODULE = REPO_ROOT / "producer/third_party/wallpaper-scene-renderer"
STATE_ROOT = REPO_ROOT / ".build/golden"
CONFIG_PATH = STATE_ROOT / "config.json"
BASELINE_ROOT = STATE_ROOT / "baseline"
RUNS_ROOT = STATE_ROOT / "runs"
FIXTURES_DIR = STATE_ROOT / "fixtures"  # generated scenes, never committed

# Renderer log tags that identify which mechanisms a wallpaper exercises. The probe report
# counts occurrences per tag; extend freely when new mechanisms gain logs.
MARKERS = {
    "effect": r"SceneEffectParsed:",
    "puppet": r"ScenePuppetAttachmentBind|ScenePuppetVertexInput|PuppetSkeletonOnly",
    "text": r"SceneTextLayoutContract",
    "dynamic-layers": r"SceneScriptCreateLayer",
    "video": r"VideoTextureDecoderGraph|VideoTextureDecoderSelect",
    "camera-layer": r"SceneCameraLayerActive",
    "light": r"SceneLightParsed",
    "model": r"ModelRenderOrder|Scene3DModelCameraPath",
    "particle": r"SceneParticleSystemParsed:",
    "bloom": r"SceneBloomGraphBind",
    "volumetrics": r"SceneVolumetrics",
    "script": r"SceneScript",
}

DEFAULT_TOLERANCE = {"max_channel_delta": 0, "max_changed_pct": 0.0}
TOLERANT_TOLERANCE = {"max_channel_delta": 255, "max_changed_pct": 100.0}

# Log lines that count as runtime witnesses. Numbers that legitimately vary between runs
# (pointers, durations) are normalized before comparing witness sets.
WITNESS_PATTERN = re.compile(r"^(ERROR|WARN)\b.*|^validation layer warning: .*")
WITNESS_NORMALIZE = [
    (re.compile(r"0x[0-9a-fA-F]+"), "0x?"),
    (re.compile(r"duration=[0-9.]+ms"), "duration=?"),
    (re.compile(r"/tmp/[^\s'\"]+"), "/tmp/?"),
]
VALIDATION_PATTERN = re.compile(r"^validation layer: (.*)$")


def now_id():
    return dt.datetime.now().strftime("%Y%m%d-%H%M%S")


def workshop_root():
    configured = os.environ.get("VIVID_WORKSHOP_ROOT") or load_json(
        CONFIG_PATH, default={}).get("workshop_root")
    if not configured:
        sys.exit("error: set VIVID_WORKSHOP_ROOT or workshop_root in .build/golden/config.json")
    path = Path(configured).expanduser().resolve()
    if not path.is_dir():
        sys.exit(f"error: Workshop directory does not exist: {path}")
    return path


def assets_dir():
    """Wallpaper Engine shared assets: explicit VIVID_SCENE_ASSETS_DIR, else next to the workshop."""
    explicit = os.environ.get("VIVID_SCENE_ASSETS_DIR")
    if explicit and Path(explicit).is_dir():
        return Path(explicit)
    for current in [workshop_root(), *workshop_root().parents]:
        candidate = current / "steamapps/common/wallpaper_engine/assets"
        if candidate.is_dir():
            return candidate
    sys.exit("error: Wallpaper Engine assets not found; set VIVID_SCENE_ASSETS_DIR")


def ensure_fixtures():
    """Regenerate the fixture scenes (cheap, deterministic) and return their index."""
    global _fixtures_generated
    _fixtures_generated = True
    result = subprocess.run(
        [sys.executable, str(FIXTURE_GENERATOR), "--assets", str(assets_dir()), "--output", str(FIXTURES_DIR)],
        capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit("error: fixture generation failed:\n" + result.stdout + result.stderr)
    return load_json(FIXTURES_DIR / "index.json", default={})


def load_json(path, default=None):
    if not Path(path).exists():
        if default is not None:
            return default
        sys.exit(f"error: {path} not found")
    return json.loads(Path(path).read_text())


def save_json(path, data):
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_text(json.dumps(data, indent=2, ensure_ascii=False, sort_keys=True) + "\n")


def load_tiers():
    return load_json(TIERS_PATH, default={"tiers": {}, "wallpapers": {}, "default_scenario": "default"})


def project_type(project):
    try:
        return str(json.loads((project / "project.json").read_text()).get("type", "")).lower()
    except (OSError, json.JSONDecodeError):
        return ""


def scene_wallpapers(root):
    return sorted(p.name for p in root.iterdir() if p.is_dir() and project_type(p) == "scene")


def resolve_ids(args, tiers):
    """Wallpaper ids (or fixture names) selected by --ids / --tier."""
    if getattr(args, "ids", None):
        return [wid for wid in args.ids.split(",") if wid]
    tier = getattr(args, "tier", None)
    if tier == "all":
        return scene_wallpapers(workshop_root())
    if tier == "fixtures":
        return sorted(f"fixture:{name}" for name in ensure_fixtures())
    if tier:
        ids = tiers.get("tiers", {}).get(tier)
        if ids is None:
            sys.exit(f"error: tier '{tier}' is not defined in {TIERS_PATH}")
        return list(ids)
    sys.exit("error: select wallpapers with --ids or --tier")


_fixtures_generated = False


def project_dir(wallpaper_id):
    global _fixtures_generated
    if wallpaper_id.startswith("fixture:"):
        # Fixtures are cheap to regenerate; always rebuild them once per invocation so the
        # generator (not a stale build-tree copy) is what gets captured.
        if not _fixtures_generated:
            ensure_fixtures()
            _fixtures_generated = True
        return FIXTURES_DIR / wallpaper_id.split(":", 1)[1]
    return workshop_root() / wallpaper_id


def scenario_for(wallpaper_id, tiers, override=None):
    name = override or tiers.get("wallpapers", {}).get(wallpaper_id, {}).get("scenario")
    if name is None and wallpaper_id.startswith("fixture:"):
        # An explicit fixture id reaches scenario selection before project_dir(). Generate its
        # index here so the scene and its input vector come from the same fixture declaration.
        if not _fixtures_generated:
            ensure_fixtures()
        index = load_json(FIXTURES_DIR / "index.json", default={})
        name = index.get(wallpaper_id.split(":", 1)[1], {}).get("scenario") or "fixture"
    if name is None:
        name = tiers.get("default_scenario", "default")
    path = SCENARIOS_DIR / f"{name}.json"
    if not path.exists():
        sys.exit(f"error: scenario {path} not found")
    return name, path


def tolerance_for(wallpaper_id, tiers, config):
    entry = tiers.get("wallpapers", {}).get(wallpaper_id, {})
    tolerance = dict(DEFAULT_TOLERANCE)
    classification = config.get("wallpapers", {}).get(wallpaper_id, {}).get("class")
    if entry.get("class", classification) == "tolerant":
        tolerance = dict(TOLERANT_TOLERANCE)
    tolerance.update(entry.get("tolerance", {}))
    return tolerance, entry.get("mask")


def run_capture(project, scenario_path, out_dir, *, extra=(), size=None, steps=None):
    """Capture one scenario into out_dir; returns (ok, log_path)."""
    if not LIBRARY.exists():
        sys.exit(f"error: {LIBRARY} not found; run `tools/vivid.sh build direct-run` first")
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    trace_dir = out_dir / "trace"
    cmd = [
        sys.executable,
        str(CAPTURE),
        "--library",
        str(LIBRARY),
        "--project",
        str(project),
        "--scenario",
        str(scenario_path),
        "--output",
        str(out_dir / "final.png"),
        "--manifest",
        str(out_dir / "manifest.json"),
        "--trace-dir",
        str(trace_dir),
        "--lockstep",
    ]
    if size:
        cmd += ["--width", str(size[0]), "--height", str(size[1])]
    if steps:
        cmd += ["--frames", str(steps)]
    cmd += list(extra)
    log_path = out_dir / "capture.log"
    env = dict(os.environ)
    env.setdefault("VIVID_SCENE_ASSETS_DIR", str(assets_dir()))
    with open(log_path, "w") as log:
        result = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, text=True, env=env)
    ok = result.returncode == 0 and (out_dir / "manifest.json").exists()
    return ok, log_path


def frame_hashes(out_dir):
    manifest = load_json(Path(out_dir) / "manifest.json", default={})
    return {frame["step"]: frame["sha256"] for frame in manifest.get("frames", [])}


def scan_markers(log_text):
    return {name: len(re.findall(pattern, log_text)) for name, pattern in MARKERS.items()}


def collect_witnesses(log_text):
    witnesses = set()
    validation = []
    for line in log_text.splitlines():
        match = VALIDATION_PATTERN.match(line)
        if match:
            validation.append(match.group(1))
            continue
        if WITNESS_PATTERN.match(line):
            for pattern, replacement in WITNESS_NORMALIZE:
                line = pattern.sub(replacement, line)
            witnesses.add(line)
    return sorted(witnesses), validation


def check_fixture_log(wallpaper_id, log_text):
    """Match only explicitly authored output consumers; unrelated runtime INFO is not a gate."""
    if not wallpaper_id.startswith("fixture:"):
        return []
    entry = load_json(FIXTURES_DIR / "index.json")[wallpaper_id.split(":", 1)[1]]
    failures = []
    for prefix, expected in entry.get("log_expectations", {}).items():
        # A log API has no pixel consumer. Preserve order, duplicates and trailing spaces so
        # missing output or partial parameter conversion cannot pass an unchanged image gate.
        actual = [line for line in log_text.splitlines() if line.startswith(prefix)]
        if actual != expected:
            failures.append(f"log output {prefix!r}: expected {expected!r}, actual {actual!r}")
    return failures


# --- image comparison -----------------------------------------------------------------------


def compare_images(base_path, new_path, tolerance, mask=None, heatmap_path=None):
    """Returns a verdict dict for two PNGs.

    A mask is a list of [x0, y0, x1, y1] rectangles (fractions of the image) excluded from the
    comparison, used for video-textured regions whose decoder is not lockstep-driven.
    """
    import numpy as np
    from PIL import Image

    a = np.asarray(Image.open(base_path).convert("RGBA"), dtype=np.int16)
    b = np.asarray(Image.open(new_path).convert("RGBA"), dtype=np.int16)
    if a.shape != b.shape:
        return {"status": "FAIL", "reason": f"size {a.shape[1]}x{a.shape[0]} vs {b.shape[1]}x{b.shape[0]}"}
    delta = np.abs(a - b).max(axis=2)
    if mask:
        height, width = delta.shape
        for x0, y0, x1, y1 in mask:
            delta[int(y0 * height) : int(y1 * height), int(x0 * width) : int(x1 * width)] = 0
    changed = delta > 0
    changed_count = int(changed.sum())
    total = delta.size
    max_delta = int(delta.max()) if changed_count else 0
    changed_pct = changed_count * 100.0 / total
    verdict = {
        "status": "PASS",
        "max_channel_delta": max_delta,
        "changed_pixels": changed_count,
        "changed_pct": round(changed_pct, 4),
    }
    if changed_count:
        ys, xs = np.nonzero(changed)
        verdict["bbox"] = [int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1]
        # Coarse 64px tiles ranked by changed pixel count give the packet its crop targets.
        tile = 64
        tiles = {}
        for y, x in zip(ys[:: max(1, len(ys) // 200000)], xs[:: max(1, len(xs) // 200000)]):
            key = (int(x) // tile, int(y) // tile)
            tiles[key] = tiles.get(key, 0) + 1
        ranked = sorted(tiles.items(), key=lambda item: -item[1])[:3]
        verdict["hot_tiles"] = [
            [tx * tile, ty * tile, (tx + 1) * tile, (ty + 1) * tile] for (tx, ty), _ in ranked
        ]
        if heatmap_path:
            heat = np.zeros((*delta.shape, 3), dtype=np.uint8)
            scaled = np.clip(delta.astype(np.int32) * 4, 0, 255).astype(np.uint8)
            heat[..., 0] = scaled
            heat[..., 1] = (changed * 40).astype(np.uint8)
            Image.fromarray(heat, "RGB").save(heatmap_path)
            verdict["heatmap"] = str(heatmap_path)
    if max_delta > tolerance["max_channel_delta"] or changed_pct > tolerance["max_changed_pct"]:
        verdict["status"] = "FAIL"
    return verdict


# --- trace comparison -----------------------------------------------------------------------


def json_diff(a, b, path="", out=None, limit=200):
    """Structural diff of two JSON values as a list of human-readable lines."""
    if out is None:
        out = []
    if len(out) >= limit:
        return out
    if isinstance(a, dict) and isinstance(b, dict):
        for key in sorted(set(a) | set(b)):
            sub = f"{path}.{key}" if path else key
            if key not in a:
                out.append(f"+ {sub}: {json.dumps(b[key], ensure_ascii=False)[:160]}")
            elif key not in b:
                out.append(f"- {sub}: {json.dumps(a[key], ensure_ascii=False)[:160]}")
            else:
                json_diff(a[key], b[key], sub, out, limit)
    elif isinstance(a, list) and isinstance(b, list):
        if len(a) != len(b):
            out.append(f"~ {path}: length {len(a)} -> {len(b)}")
        for index, (item_a, item_b) in enumerate(zip(a, b)):
            json_diff(item_a, item_b, f"{path}[{index}]", out, limit)
    elif a != b:
        out.append(f"~ {path}: {json.dumps(a, ensure_ascii=False)[:120]} -> {json.dumps(b, ensure_ascii=False)[:120]}")
    return out


def command_label(command):
    return (f"#{command.get('index')} {command.get('kind')}/{command.get('result')} "
            f"layer={command.get('layer')} out='{command.get('output')}'")


def frame_trace_diff(a, b, limit=200):
    """Trace-aware diff: names commands and uniforms instead of positional indices."""
    out = []
    for key in ("draw", "scene_time", "phase", "output", "resources"):
        if a.get(key) != b.get(key):
            out.append(f"~ {key}: {a.get(key)} -> {b.get(key)}")
    ca, cb = a.get("commands", []), b.get("commands", [])
    if len(ca) != len(cb):
        out.append(f"~ command count {len(ca)} -> {len(cb)}")
    for command_a, command_b in zip(ca, cb):
        if len(out) >= limit:
            break
        label = command_label(command_b)
        for key in ("kind", "result", "output", "layer", "reflection", "count", "extent", "samples", "allocation"):
            if command_a.get(key) != command_b.get(key):
                out.append(f"~ {label}: {key} {command_a.get(key)} -> {command_b.get(key)}")
        ia = {(i.get("role"), i.get("key"), i.get("binding")): i for i in command_a.get("inputs", [])}
        ib = {(i.get("role"), i.get("key"), i.get("binding")): i for i in command_b.get("inputs", [])}
        for ikey in sorted(set(ia) | set(ib), key=str):
            if ikey not in ia:
                out.append(f"+ {label}: input {ikey[0]} '{ikey[1]}'")
            elif ikey not in ib:
                out.append(f"- {label}: input {ikey[0]} '{ikey[1]}'")
            elif ia[ikey] != ib[ikey]:
                out.append(f"~ {label}: input {ikey[0]} '{ikey[1]}' {ia[ikey]} -> {ib[ikey]}")
        ua = {u["name"]: u for u in command_a.get("uniforms", [])}
        ub = {u["name"]: u for u in command_b.get("uniforms", [])}
        for name in sorted(set(ua) | set(ub)):
            if name not in ua:
                out.append(f"+ {label}: uniform {name}")
            elif name not in ub:
                out.append(f"- {label}: uniform {name}")
            elif ua[name] != ub[name]:
                va, vb = ua[name].get("values", []), ub[name].get("values", [])
                changed = [f"[{i}] {x} -> {y}" for i, (x, y) in enumerate(zip(va, vb)) if x != y]
                if len(va) != len(vb):
                    changed.append(f"length {len(va)} -> {len(vb)}")
                out.append(f"~ {label}: uniform {name} " + ", ".join(changed[:6])
                           + (f" (+{len(changed) - 6} more)" if len(changed) > 6 else ""))
    return out[:limit]


def compare_traces(base_dir, new_dir):
    base_traces = {p.name: p for p in (Path(base_dir) / "trace").glob("*.json")} if (Path(base_dir) / "trace").is_dir() else {}
    new_traces = {p.name: p for p in (Path(new_dir) / "trace").glob("*.json")} if (Path(new_dir) / "trace").is_dir() else {}
    results = {}
    for name in sorted(set(base_traces) | set(new_traces)):
        if name not in base_traces or name not in new_traces:
            results[name] = ["trace present on one side only"]
            continue
        if base_traces[name].read_bytes() == new_traces[name].read_bytes():
            results[name] = []
            continue
        results[name] = frame_trace_diff(load_json(base_traces[name]), load_json(new_traces[name]))
    return results


# --- commands -------------------------------------------------------------------------------


def cmd_probe(args):
    tiers = load_tiers()
    ids = resolve_ids(args, tiers) if (args.ids or args.tier) else scene_wallpapers(workshop_root())
    _, scenario_path = scenario_for("", tiers, override=args.scenario or "probe")
    report = {}
    for wallpaper_id in ids:
        out_dir = RUNS_ROOT / "probe" / wallpaper_id
        ok, log_path = run_capture(project_dir(wallpaper_id), scenario_path, out_dir)
        if not ok:
            report[wallpaper_id] = {"ok": False}
            print(f"{wallpaper_id}: capture failed (see {log_path})")
            continue
        markers = {k: v for k, v in scan_markers(log_path.read_text()).items() if v > 0}
        report[wallpaper_id] = {"ok": True, "markers": markers}
        print(f"{wallpaper_id}: " + (", ".join(f"{k}={v}" for k, v in markers.items()) or "(none)"))
    save_json(RUNS_ROOT / "probe" / "probe-report.json", report)
    print(f"report written to {RUNS_ROOT / 'probe' / 'probe-report.json'}")


def cmd_init(args):
    """Determinism self-check: N captures per wallpaper must hash identically at every step."""
    tiers = load_tiers()
    ids = resolve_ids(args, tiers) if (args.ids or args.tier) else scene_wallpapers(workshop_root())
    config = load_json(CONFIG_PATH, default={"wallpapers": {}})
    _, scenario_path = scenario_for("", tiers, override=args.scenario or "selfcheck")
    summary = {"same": 0, "diff": 0, "failed": 0}
    for wallpaper_id in ids:
        hashes = []
        logs = []
        for run in range(args.runs):
            out_dir = RUNS_ROOT / "selfcheck" / wallpaper_id / f"run{run}"
            if out_dir.exists():
                shutil.rmtree(out_dir)
            ok, log_path = run_capture(project_dir(wallpaper_id), scenario_path, out_dir)
            if not ok:
                hashes = None
                break
            hashes.append(frame_hashes(out_dir))
            logs.append(log_path.read_text())
        entry = config["wallpapers"].setdefault(wallpaper_id, {})
        entry["checked"] = now_id()
        if hashes is None:
            entry.update({"class": "broken", "reason": "capture failed"})
            summary["failed"] += 1
            print(f"{wallpaper_id}: capture FAILED")
            continue
        markers = {k: v for k, v in scan_markers(logs[0]).items() if v > 0}
        entry["markers"] = markers
        identical = all(h == hashes[0] for h in hashes[1:])
        if identical and markers.get("video"):
            # A live video decoder is paced by its own clock. Two runs can still agree when the
            # decoder happened to deliver the same frames, so the class follows the mechanism,
            # not one lucky pair.
            entry.update({"class": "tolerant", "reason": "video texture (decoder clock); identical in this check"})
            summary["same"] += 1
            print(f"{wallpaper_id}: SAME but video-textured -> tolerant")
        elif identical:
            entry.update({"class": "exact", "reason": None})
            summary["same"] += 1
            print(f"{wallpaper_id}: SAME ({len(hashes[0])} sampled steps identical over {args.runs} runs)")
        else:
            differing = sorted(step for step in hashes[0] if any(h.get(step) != hashes[0][step] for h in hashes[1:]))
            reason = "video texture (decoder clock)" if markers.get("video") else "unexplained"
            entry.update({"class": "tolerant" if markers.get("video") else "nondeterministic",
                          "reason": reason, "differing_steps": differing})
            # Keep the trace diff of the first differing step for attribution.
            if len(hashes) > 1:
                diff_lines = compare_traces(RUNS_ROOT / "selfcheck" / wallpaper_id / "run0",
                                            RUNS_ROOT / "selfcheck" / wallpaper_id / "run1")
                entry["trace_diff"] = {k: v[:20] for k, v in diff_lines.items() if v}
            summary["diff"] += 1
            print(f"{wallpaper_id}: DIFF at steps {differing} ({reason})")
    save_json(CONFIG_PATH, config)
    print(f"\nself-check: same={summary['same']} diff={summary['diff']} failed={summary['failed']}")
    print(f"config written to {CONFIG_PATH}")
    sys.exit(1 if summary["diff"] or summary["failed"] else 0)


def baseline_dir(wallpaper_id, scenario_name):
    return BASELINE_ROOT / wallpaper_id.replace(":", "_") / scenario_name


def cmd_run(args):
    tiers = load_tiers()
    config = load_json(CONFIG_PATH, default={"wallpapers": {}})
    ids = resolve_ids(args, tiers)
    run_id = args.run_id or now_id()
    run_dir = RUNS_ROOT / run_id
    report = {"run": run_id, "tier": args.tier, "results": {}, "renderer": None}
    failures = 0
    capture_extra = [] if args.no_vk_validation else ["--vk-validation"]
    for wallpaper_id in ids:
        scenario_name, scenario_path = scenario_for(wallpaper_id, tiers, override=args.scenario)
        tolerance, mask = tolerance_for(wallpaper_id, tiers, config)
        candidate = run_dir / wallpaper_id.replace(":", "_") / scenario_name
        ok, log_path = run_capture(project_dir(wallpaper_id), scenario_path, candidate,
                                   extra=capture_extra)
        result = {"scenario": scenario_name, "status": "PASS", "frames": {}, "notes": []}
        report["results"][wallpaper_id] = result
        if not ok:
            result["status"] = "FAIL"
            result["notes"].append(f"capture failed: {log_path}")
            failures += 1
            print(f"{wallpaper_id}/{scenario_name}: CAPTURE FAILED")
            continue
        manifest = load_json(candidate / "manifest.json")
        report["renderer"] = manifest.get("renderer")
        log_text = log_path.read_text()
        witnesses, validation = collect_witnesses(log_text)
        save_json(candidate / "witnesses.json", {"witnesses": witnesses, "validation": validation})
        if validation:
            result["status"] = "FAIL"
            result["notes"].append(f"{len(validation)} Vulkan validation messages")

        log_failures = check_fixture_log(wallpaper_id, log_text)
        if log_failures:
            result["status"] = "FAIL"
            result["notes"].extend(log_failures)
            failures += 1
            print(f"{wallpaper_id}/{scenario_name}: LOG OUTPUT FAILED")
            continue

        base = baseline_dir(wallpaper_id, scenario_name)
        if not (base / "manifest.json").exists():
            if args.update_baseline:
                shutil.copytree(candidate, base, dirs_exist_ok=True)
                save_json(base / "history.json", [{"run": run_id, "reason": "initial baseline",
                                                    "date": now_id(),
                                                    "renderer": manifest.get("renderer")}])
                result["status"] = "NEW"
                print(f"{wallpaper_id}/{scenario_name}: NEW baseline recorded")
            else:
                result["status"] = "NOBASE"
                failures += 1
                result["notes"].append("no baseline; run with --update-baseline to record one")
                print(f"{wallpaper_id}/{scenario_name}: NO BASELINE")
            continue

        base_hashes = frame_hashes(base)
        new_hashes = frame_hashes(candidate)
        base_manifest = load_json(base / "manifest.json")
        if base_manifest.get("scenario_sha256") != manifest.get("scenario_sha256"):
            result["notes"].append("scenario changed since the baseline was recorded")
        env_keys = ("gpu", "fontconfig_sha256")
        for key in env_keys:
            if base_manifest.get(key) != manifest.get(key):
                result["notes"].append(f"environment changed: {key}")
        for step, new_hash in sorted(new_hashes.items()):
            base_hash = base_hashes.get(step)
            frame_name = f"step{step:03d}"
            base_png = next((Path(f["path"]) for f in base_manifest["frames"] if f["step"] == step), None)
            new_png = next((Path(f["path"]) for f in manifest["frames"] if f["step"] == step), None)
            if base_hash is None or base_png is None or not base_png.exists():
                result["frames"][frame_name] = {"status": "NOBASE"}
                # A newly sampled frame has no accepted result yet. Even when every shared
                # pixel and trace matches, this entry still needs an explicit promotion;
                # recording only a per-frame note would let the process exit successfully.
                result["status"] = "FAIL"
                result["notes"].append(f"{frame_name}: no accepted baseline frame")
                continue
            if base_hash == new_hash:
                result["frames"][frame_name] = {"status": "PASS", "exact": True}
                continue
            verdict = compare_images(base_png, new_png, tolerance, mask,
                                     heatmap_path=candidate / f"diff-{frame_name}.png")
            verdict["exact"] = False
            result["frames"][frame_name] = verdict
            if verdict["status"] == "FAIL":
                result["status"] = "FAIL"
        trace_diffs = compare_traces(base, candidate)
        changed_traces = {k: v for k, v in trace_diffs.items() if v}
        if changed_traces:
            result["trace_diff"] = {k: v[:40] for k, v in changed_traces.items()}
            if result["status"] == "PASS":
                result["status"] = "FAIL"
                result["notes"].append("frame trace changed while pixel comparison passed")
        base_witnesses = set(load_json(base / "witnesses.json", default={}).get("witnesses", []))
        new_witnesses = set(witnesses) - base_witnesses
        if new_witnesses:
            result["new_witnesses"] = sorted(new_witnesses)
            result["status"] = "FAIL"
            result["notes"].append(f"{len(new_witnesses)} new ERROR/WARN witnesses")
        if result["status"] == "FAIL":
            failures += 1
        exact = sum(1 for f in result["frames"].values() if f.get("exact"))
        print(f"{wallpaper_id}/{scenario_name}: {result['status']} "
              f"(exact {exact}/{len(result['frames'])} frames"
              + (f"; {'; '.join(result['notes'])}" if result["notes"] else "") + ")")
        for frame_name, verdict in result["frames"].items():
            if verdict.get("status") == "FAIL":
                print(f"    {frame_name}: max-delta={verdict.get('max_channel_delta')} "
                      f"changed={verdict.get('changed_pct')}% bbox={verdict.get('bbox')}")
        for trace_name, lines in result.get("trace_diff", {}).items():
            print(f"    trace {trace_name}:")
            for line in lines[:8]:
                print(f"      {line}")
    save_json(run_dir / "report.json", report)
    write_markdown_report(run_dir / "report.md", report)
    print(f"\nreport: {run_dir / 'report.md'}  ({failures} failing)")
    sys.exit(1 if failures else 0)


def write_markdown_report(path, report):
    lines = [f"# golden run {report['run']}", ""]
    if report.get("renderer"):
        lines.append(f"renderer: `{report['renderer'].get('commit')}`"
                     + (" (dirty)" if report["renderer"].get("dirty") else ""))
        lines.append("")
    for wallpaper_id, result in report["results"].items():
        lines.append(f"## {wallpaper_id} / {result['scenario']} — {result['status']}")
        for note in result.get("notes", []):
            lines.append(f"- {note}")
        for frame_name, verdict in result.get("frames", {}).items():
            if verdict.get("exact"):
                continue
            lines.append(f"- {frame_name}: {verdict.get('status')} max-delta={verdict.get('max_channel_delta')} "
                         f"changed={verdict.get('changed_pct')}% bbox={verdict.get('bbox')}")
        for trace_name, diff_lines in result.get("trace_diff", {}).items():
            lines.append(f"- trace `{trace_name}`:")
            lines.extend(f"    - `{line}`" for line in diff_lines[:20])
        for witness in result.get("new_witnesses", []):
            lines.append(f"- new witness: `{witness}`")
        lines.append("")
    Path(path).write_text("\n".join(lines))


def cmd_promote(args):
    run_dir = RUNS_ROOT / args.run
    report = load_json(run_dir / "report.json")
    selected = args.ids.split(",") if args.ids else [
        wid for wid, result in report["results"].items() if result["status"] in ("FAIL", "NOBASE")
    ]
    for wallpaper_id in selected:
        result = report["results"].get(wallpaper_id)
        if not result:
            print(f"{wallpaper_id}: not part of run {args.run}")
            continue
        candidate = run_dir / wallpaper_id.replace(":", "_") / result["scenario"]
        base = baseline_dir(wallpaper_id, result["scenario"])
        if not (candidate / "manifest.json").exists():
            print(f"{wallpaper_id}: candidate has no manifest, skipped")
            continue
        history = load_json(base / "history.json", default=[])
        if base.exists():
            shutil.rmtree(base)
        shutil.copytree(candidate, base)
        history.append({"run": args.run, "reason": args.reason, "date": now_id(),
                        "renderer": load_json(candidate / "manifest.json").get("renderer")})
        save_json(base / "history.json", history)
        print(f"{wallpaper_id}/{result['scenario']}: promoted ({args.reason})")


def cmd_packet(args):
    """Composite (baseline | candidate | heatmap) plus crops of the hottest tiles and text."""
    from PIL import Image, ImageDraw

    run_dir = RUNS_ROOT / args.run
    report = load_json(run_dir / "report.json")
    result = report["results"].get(args.id)
    if not result:
        sys.exit(f"error: {args.id} is not part of run {args.run}")
    candidate = run_dir / args.id.replace(":", "_") / result["scenario"]
    base = baseline_dir(args.id, result["scenario"])
    packet_dir = run_dir / "packets" / args.id.replace(":", "_")
    packet_dir.mkdir(parents=True, exist_ok=True)
    text = [f"# review packet {args.id} / {result['scenario']} (run {args.run})", ""]
    base_manifest = load_json(base / "manifest.json")
    new_manifest = load_json(candidate / "manifest.json")
    for frame_name, verdict in result["frames"].items():
        if verdict.get("status") != "FAIL":
            continue
        step = int(frame_name[4:])
        base_png = next(Path(f["path"]) for f in base_manifest["frames"] if f["step"] == step)
        new_png = next(Path(f["path"]) for f in new_manifest["frames"] if f["step"] == step)
        a = Image.open(base_png).convert("RGB")
        b = Image.open(new_png).convert("RGB")
        heat = Image.open(verdict["heatmap"]).convert("RGB") if verdict.get("heatmap") else Image.new("RGB", a.size)
        scale = min(1.0, 600 / a.width)
        thumb = (int(a.width * scale), int(a.height * scale))
        composite = Image.new("RGB", (thumb[0] * 3 + 20, thumb[1] + 30), (24, 24, 24))
        for index, (image, label) in enumerate(((a, "baseline"), (b, "candidate"), (heat, "delta x4"))):
            composite.paste(image.resize(thumb), (index * (thumb[0] + 10), 30))
            ImageDraw.Draw(composite).text((index * (thumb[0] + 10) + 4, 8), label, fill=(230, 230, 230))
        # Crops of the hottest tiles at 2x, baseline over candidate.
        crops = []
        for x0, y0, x1, y1 in verdict.get("hot_tiles", [])[:3]:
            box = (max(0, x0 - 32), max(0, y0 - 32), min(a.width, x1 + 32), min(a.height, y1 + 32))
            ca = a.crop(box).resize(((box[2] - box[0]) * 2, (box[3] - box[1]) * 2), Image.NEAREST)
            cb = b.crop(box).resize(ca.size, Image.NEAREST)
            pair = Image.new("RGB", (ca.width, ca.height * 2 + 4), (24, 24, 24))
            pair.paste(ca, (0, 0))
            pair.paste(cb, (0, ca.height + 4))
            crops.append((box, pair))
        if crops:
            width = sum(c.width for _, c in crops) + 10 * (len(crops) - 1)
            height = max(c.height for _, c in crops)
            strip = Image.new("RGB", (width, height), (24, 24, 24))
            x = 0
            for _, crop in crops:
                strip.paste(crop, (x, 0))
                x += crop.width + 10
            merged = Image.new("RGB", (max(composite.width, strip.width), composite.height + strip.height + 10), (24, 24, 24))
            merged.paste(composite, (0, 0))
            merged.paste(strip, (0, composite.height + 10))
            composite = merged
        out = packet_dir / f"{frame_name}.png"
        composite.save(out)
        text.append(f"## {frame_name}")
        text.append(f"- composite: `{out}` (baseline | candidate | delta; crops below are baseline over candidate at 2x)")
        text.append(f"- max channel delta {verdict['max_channel_delta']}, changed {verdict['changed_pct']}% , bbox {verdict.get('bbox')}")
        text.append(f"- crops: {[c[0] for c in crops]}")
        text.append("")
    for trace_name, lines in result.get("trace_diff", {}).items():
        text.append(f"## trace diff {trace_name}")
        text.extend(f"- `{line}`" for line in lines)
        text.append("")
    for witness in result.get("new_witnesses", []):
        text.append(f"- new witness: `{witness}`")
    (packet_dir / "packet.md").write_text("\n".join(text) + "\n")
    print(f"packet written to {packet_dir / 'packet.md'}")


def build_scene_renderer():
    script = (
        'ROOT_DIR="$PWD/producer"; source tools/producer/build_env.sh && '
        'cmake -S "$VIVID_SCENE_SOURCE_DIR" -B "$VIVID_DIRECT_RUN_SCENE_BUILD_DIR" '
        '-DCMAKE_BUILD_TYPE="$VIVID_CMAKE_BUILD_TYPE" > /dev/null && '
        'cmake --build "$VIVID_DIRECT_RUN_SCENE_BUILD_DIR" --target "$VIVID_SCENE_TARGET" '
        '--target VividScene --parallel "$(nproc)"'
    )
    result = subprocess.run(["bash", "-c", script], cwd=REPO_ROOT, capture_output=True, text=True)
    if result.returncode != 0:
        sys.stderr.write(result.stdout[-4000:] + result.stderr[-4000:])
    return result.returncode == 0


def cmd_bisect_run(args):
    """Exit 0 when the tier passes, 1 when it fails, 125 when the build fails (git bisect skip)."""
    if not build_scene_renderer():
        sys.exit(125)
    cmd = [sys.executable, str(Path(__file__)), "run", "--run-id", f"bisect-{now_id()}"]
    if args.ids:
        cmd += ["--ids", args.ids]
    else:
        cmd += ["--tier", args.tier or "quick"]
    sys.exit(subprocess.run(cmd).returncode)


def cmd_status(args):
    if not BASELINE_ROOT.is_dir():
        print("no baselines recorded")
        return
    for manifest_path in sorted(BASELINE_ROOT.glob("*/*/manifest.json")):
        manifest = load_json(manifest_path)
        renderer = manifest.get("renderer") or {}
        history = load_json(manifest_path.parent / "history.json", default=[])
        print(f"{manifest_path.parent.parent.name}/{manifest_path.parent.name}: "
              f"renderer={str(renderer.get('commit'))[:12]}{'+' if renderer.get('dirty') else ''} "
              f"gpu={manifest.get('gpu', {}).get('name')} frames={len(manifest.get('frames', []))} "
              f"promotions={len(history)} last={history[-1]['reason'] if history else '-'}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    def selection(p, tier_required=False):
        p.add_argument("--ids", help="comma-separated wallpaper ids (or fixture:<name>)")
        p.add_argument("--tier", help="tier name from tiers.json, or all / fixtures", required=False)
        p.add_argument("--scenario", help="scenario name overriding the per-wallpaper default")

    p = sub.add_parser("probe", help="scan wallpapers for mechanism coverage")
    selection(p)
    p.set_defaults(func=cmd_probe)

    p = sub.add_parser("init", help="determinism self-check and classification")
    selection(p)
    p.add_argument("--runs", type=int, default=2)
    p.set_defaults(func=cmd_init)

    p = sub.add_parser("run", help="capture a tier and compare with the baseline")
    selection(p)
    p.add_argument("--run-id")
    p.add_argument("--update-baseline", action="store_true", help="record missing baselines")
    p.add_argument("--no-vk-validation", action="store_true",
                   help="skip the Vulkan validation layer (enabled by default)")
    p.set_defaults(func=cmd_run)

    p = sub.add_parser("promote", help="accept candidates of a run as the new baseline")
    p.add_argument("--run", required=True)
    p.add_argument("--ids", help="comma-separated ids (default: every failing/new entry)")
    p.add_argument("--reason", required=True)
    p.set_defaults(func=cmd_promote)

    p = sub.add_parser("packet", help="build a review packet for one failing entry")
    p.add_argument("--run", required=True)
    p.add_argument("--id", required=True)
    p.set_defaults(func=cmd_packet)

    p = sub.add_parser("bisect-run", help="build + run a tier; exit code for git bisect run")
    selection(p)
    p.set_defaults(func=cmd_bisect_run)

    p = sub.add_parser("status", help="list recorded baselines")
    p.set_defaults(func=cmd_status)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
