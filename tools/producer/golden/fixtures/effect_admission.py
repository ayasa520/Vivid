"""Paired effect-record consumers with literal, independently specified expectations."""

import copy
import json


MISSING = object()
TARGETS = ["_rt_AdmissionA", "_rt_AdmissionB", "_rt_AdmissionC"]
CANDIDATE = "_rt_AdmissionCandidate"
COLORS = [[0.875, 0.125, 0.25], [0.125, 0.25, 0.875],
          [0.75, 0.625, 0.125], [0.875, 0.125, 0.75], [0.125, 0.75, 0.625]]
CLEARS = [[0.125, 0.375, 0.625, 0], [0.375, 0.625, 0.125, 0],
          [0.625, 0.125, 0.375, 0]]


def condition_cases():
    # Expected admission is authored data, not an implementation of the predicate under test.
    # Each record is exercised on both image and text owners against unconditional writers.
    return [
        ("omitted", MISSING, MISSING, True),
        ("nonarray", {"KEY": 99}, {"KEY": 1}, True),
        ("empty-array", [], {"KEY": 1}, True),
        ("ignored-entries", [None, 7, False, "x", [], {}], {}, True),
        ("equal", [{"KEY": 1}], {"KEY": 1}, True),
        ("unequal", [{"KEY": 1}], {"KEY": 0}, False),
        ("missing-zero", [{"KEY": 0}], {}, True),
        ("missing-one", [{"KEY": 1}], {}, False),
        ("string-zero", [{"KEY": 0}], {"KEY": "1"}, True),
        ("boolean-one", [{"KEY": 1}], {"KEY": True}, False),
        ("wrapped-one", [{"KEY": 1}], {"KEY": {"value": 1}}, False),
        ("nonobject-combos", [{"KEY": 0}], [1], True),
        ("low-word", [{"KEY": 1}], {"KEY": 4294967297}, True),
        ("signed-word", [{"KEY": -1}], {"KEY": 4294967295}, True),
        ("negative-fraction", [{"KEY": -1}], {"KEY": -1.9}, True),
        ("fractional-rule", [{"KEY": 2.1}], {"KEY": 2.9}, True),
        ("all-keys", [{"A": 1, "B": 2}], {"A": 1, "B": 3}, False),
        ("all-objects", [{"A": 1}, {"B": 2}], {"A": 1, "B": 3}, False),
        ("ignored-values", [{"A": None, "B": [], "C": "1", "D": True}], {}, True),
        ("ignored-after-failure", [{"A": 1, "B": None}], {}, False),
        ("ge-equal", [{"KEY": {"op": "ge", "value": -2}}], {"KEY": -2}, True),
        ("ge-less", [{"KEY": {"op": "ge", "value": -2}}], {"KEY": -3}, False),
        ("gt-more", [{"KEY": {"op": "gt", "value": -2}}], {"KEY": -1}, True),
        ("gt-equal", [{"KEY": {"op": "gt", "value": -2}}], {"KEY": -2}, False),
        ("le-equal", [{"KEY": {"op": "le", "value": -2}}], {"KEY": -2}, True),
        ("le-more", [{"KEY": {"op": "le", "value": -2}}], {"KEY": -1}, False),
        ("lt-less", [{"KEY": {"op": "lt", "value": -2}}], {"KEY": -3}, True),
        ("lt-equal", [{"KEY": {"op": "lt", "value": -2}}], {"KEY": -2}, False),
        ("unknown-op-equality", [{"KEY": {"op": "ne", "value": 2}}], {"KEY": 2}, True),
        ("uppercase-op", [{"KEY": {"op": "GT", "value": 1}}], {"KEY": 2}, False),
        ("missing-value", [{"KEY": {"op": "ge"}}], {}, True),
        ("nonnumeric-value", [{"KEY": {"op": 7, "value": True}}], {}, True),
        ("case-sensitive-key", [{"KEY": 1}], {"key": 1}, False),
        ("large-double", [{"KEY": -2147483648}], {"KEY": 1e30}, True),
        ("large-rule-double", [{"KEY": {"value": 1e30}}], {"KEY": -2147483648}, True),
        ("unsigned-low-word", [{"KEY": -1}], {"KEY": 18446744073709551615}, True),
    ]


def material(shader, color=None):
    value = {"shader": shader, "blending": "normal", "cullmode": "nocull",
             "depthtest": "disabled", "depthwrite": "disabled", "textures": []}
    if color is not None:
        value["constantshadervalues"] = {"color": color, "opacity": 0.875, "write": 1}
    return {"passes": [value]}


def fbo(name, color):
    return {"name": name, "format": "rgba8888", "unique": True,
            "width": 64, "height": 40, "fit": 32,
            "clear": " ".join(str(value) for value in color[:3])}


def base_fixture(name):
    source = material("genericimage2")
    source["passes"][0]["textures"] = ["admission-source"]
    return {
        "description": "Effect record admission on image/text owners with independent color consumers.",
        "docs": ["scene/scenescript/reference/class/IEffect.md",
                 "scene/scenescript/reference/class/IMaterial.md",
                 "scene/scenescript/reference/class/ITextLayer.md",
                 "scene/scenescript/reference/class/IScene.md"],
        "scenario": "fixture-" + name,
        "asset_files": ["shaders/golden/effect-clear-" + part + extension
                        for part in ("write", "read") for extension in (".vert", ".frag")],
        "texture_assets": {"admission-source": {"kind": "checker", "size": [64, 40]}},
        "properties": {"phase": {"type": "slider", "value": 0, "min": 0, "max": 4}},
        "files": {"materials/admission-source.json": source,
                  "models/admission-source.json": {"material": "materials/admission-source.json",
                       "autosize": False, "fullscreen": False, "width": 64, "height": 40},
                  "materials/admission/write.json": material("golden/effect-clear-write"),
                  "materials/admission/read.json": material("golden/effect-clear-read")},
        "objects": [], "layers": [],
    }


def read_pass():
    return {"material": "materials/admission/read.json",
            "bind": [{"index": index, "name": target} for index, target in enumerate(TARGETS)]}


def owner(index, case_index, label, kind, control, resource, combos=MISSING, overrides=None):
    # Integer capture-pixel translations make a whole rectangular pair comparable, including
    # the background around a text card. Every target remains unique to its authored effect.
    x = (case_index % 6) * 320 + ((208 if kind == "text" else 40) + (72 if control else 0))
    y = 1000 - (case_index // 6) * 124
    effect = {"id": 10000 + index, "name": "admission", "file": resource}
    if combos is not MISSING:
        effect["combos"] = combos
    if overrides is not None:
        effect["passes"] = overrides
    result = {"id": 100 + index, "name": label + "-" + kind + ("-control" if control else ""),
              "origin": f"{x} {y} 0", "angles": "0 0 0", "scale": "1 1 1",
              "visible": True, "size": "64 40", "effects": [effect]}
    if kind == "image":
        result["image"] = "models/admission-source.json"
    else:
        result.update({"text": "FX", "scale": "0.08 0.08 1", "alpha": 1,
                       "anchor": "0 0", "brightness": 1, "color": "1 1 1", "colorBlendMode": 0,
                       "copybackground": False, "opaquebackground": False,
                       "backgroundcolor": "0 0 0", "backgroundbrightness": 1,
                       "horizontalalign": "center", "verticalalign": "center", "padding": 0,
                       "parallaxDepth": "0 0", "perspective": False, "pointsize": 64, "solid": True})
    return result


def add_controller(fixture, rows, body):
    # Keep construction input as JSON text. Turning a uint64 into a JavaScript Number before
    # recreating the owner would change the low-word numeric case instead of testing it twice.
    configs = [json.dumps(obj, separators=(",", ":"), ensure_ascii=False) for obj in fixture["objects"]]
    script = "'use strict';\nconst configs = " + json.dumps(configs, ensure_ascii=False) + ";\n"
    script += "const rows = " + json.dumps(rows, separators=(",", ":")) + ";\n"
    script += "const seeds = " + json.dumps(COLORS) + ";\nconst clears = " + json.dumps(CLEARS) + ";\n"
    script += """
let phase = 0, applied = -1, age = 0, status = 'pending';
let owners = [];
export function init() { owners = rows.map(row => thisScene.getLayer(row.name)); }
export function applyUserProperties(value) {
    if (value.phase !== undefined) phase = Number(value.phase);
}
function recreate() {
    for (const item of owners) thisScene.destroyLayer(item);
    owners = configs.map(config => thisScene.createLayer(config));
}
function write(material, color, enabled) {
    if (material === null) return;
    material.write = enabled ? 1 : 0;
    material.color = new Vec3(color[0], color[1], color[2]);
    material.opacity = color.length === 4 ? color[3] : 0.875;
}
function result(checks) {
    const failures = checks.filter(check => !check[1]).length;
    status = 'admission phase=' + phase + ' failures=' + failures;
    localStorage.set('effect-admission-' + phase, {phase, failures, checks});
    return status;
}
""" + body
    fixture["layers"] = [{"kind": "text", "name": "admission-controller", "origin": [960, 1060, 0],
                          "size": [1800, 40], "pointsize": 4, "padding": 0,
                          "text": {"value": "pending", "script": script}}]
    fixture["files"]["admission-expectations.json"] = rows


FBO_SCRIPT = """
function prepare() {
    if (phase === 3) recreate();
    for (let index = 0; index < owners.length; index++) {
        const item = owners[index];
        if (phase === 4 && rows[index].kind === 'text') item.text = 'FX\\nFX';
        const effect = item.getEffect('admission');
        if (effect === null) continue;
        for (let slot = 0; slot < 3; slot++) write(effect.getMaterial(slot), seeds[slot], true);
    }
}
export function update() {
    if (applied !== phase) { applied = phase; age = 0; }
    if (age++ === 0) { prepare(); return status; }
    if (age !== 3) return status;
    const checks = [];
    for (let index = 0; index < owners.length; index++) {
        const row = rows[index], effect = owners[index].getEffect('admission');
        checks.push([row.name + ':effect', effect !== null]);
        if (effect === null) continue;
        checks.push([row.name + ':count', effect.getMaterialCount() === 4, effect.getMaterialCount(), 4]);
        let expected = seeds.slice(0, 3).map(value => value.slice());
        if (phase !== 0) {
            if (phase === 2) {
                if (!row.admitted || row.named) expected[0] = clears[0];
            } else {
                expected[0] = clears[0];
                if (!row.admitted) expected[1] = clears[1];
            }
        }
        if (row.control) {
            for (let slot = 0; slot < 3; slot++) write(effect.getMaterial(slot), expected[slot], true);
        } else {
            for (let slot = 0; slot < 3; slot++) write(effect.getMaterial(slot), seeds[slot], false);
            if (phase !== 0) effect.executeMaterialFunction(phase === 2 ? 'mixed' : 'probe');
        }
    }
    return result(checks);
}
"""


PASS_SCRIPT = """
function prepare() {
    if (phase === 1) recreate();
    for (let index = 0; index < owners.length; index++) {
        if (phase === 2 && rows[index].kind === 'text') owners[index].text = 'FX\\nFX';
        if (phase !== 3) continue;
        const row = rows[index], effect = owners[index].getEffect('admission');
        if (effect === null) continue;
        const color = [0.375, 0.875, 0.25];
        if (row.control) {
            for (const slot of (row.admitted ? [1, 2] : [0])) write(effect.getMaterial(slot), color, true);
        } else {
            write(effect.getMaterial(row.admitted ? 3 : 0), color, true);
        }
    }
}
export function update() {
    if (applied !== phase) { applied = phase; age = 0; }
    if (age++ === 0) { prepare(); return status; }
    if (age !== 3) return status;
    const checks = [];
    for (let index = 0; index < owners.length; index++) {
        const row = rows[index], effect = owners[index].getEffect('admission');
        const check = (name, value, wanted) => checks.push([row.name + ':' + name, Object.is(value, wanted), value, wanted]);
        check('effect', effect !== null, true);
        if (effect === null) continue;
        const records = row.records.map(value => Array.isArray(value) ? value.slice() : value);
        if (phase === 3) {
            const positions = row.control ? (row.admitted ? [1, 2] : [0]) : [row.admitted ? 3 : 0];
            for (const slot of positions) records[slot] = [0.375, 0.875, 0.25];
        }
        check('count', effect.getMaterialCount(), records.length);
        for (let slot = 0; slot < records.length; slot++) {
            const material = effect.getMaterial(slot), wanted = records[slot];
            check('record-' + slot, material !== null, wanted !== null);
            if (material === null || !Array.isArray(wanted)) continue;
            const color = material.color;
            for (let channel = 0; channel < 3; channel++)
                check('color-' + slot + '-' + channel, color[['x', 'y', 'z'][channel]], wanted[channel]);
        }
        check('end', effect.getMaterial(records.length), null);
    }
    return result(checks);
}
"""


def make_fbo_fixture():
    fixture = base_fixture("fbo-record-admission")
    fixture["description"] = "Literal FBO name/format gates and combo conditions, observed through target allocation and named-clear prefixes on cold/recreated image/text owners."
    cases = [(label, condition, combos, admitted, {})
             for label, condition, combos, admitted in condition_cases()]
    cases += [("name-missing", MISSING, {}, False, {"name": MISSING}),
              ("name-null", MISSING, {}, False, {"name": None}),
              ("name-number", MISSING, {}, False, {"name": 17}),
              ("name-wrapped", MISSING, {}, False, {"name": {"value": CANDIDATE}}),
              ("name-empty", MISSING, {}, True, {"name": ""}),
              ("format-missing", MISSING, {}, False, {"format": MISSING}),
              ("format-null", MISSING, {}, False, {"format": None}),
              ("format-number", MISSING, {}, False, {"format": 17}),
              ("format-wrapped", MISSING, {}, False, {"format": {"value": "rgba8888"}}),
              ("format-empty", MISSING, {}, True, {"format": ""}),
              ("condition-before-fields", [{"KEY": 1}], {}, False, {"name": 17}),
              ("object-fbos", [{"KEY": 1}], {}, False, {})]
    base = {"version": 1, "fbos": [fbo(name, color) for name, color in zip(TARGETS, CLEARS)],
            "passes": [{"material": "materials/admission/write.json", "target": target}
                       for target in TARGETS] + [read_pass()],
            "functions": {"probe": {"action": "clear", "fbos": [TARGETS[2], TARGETS[1]]},
                          "mixed": {"action": "clear", "fbos": [TARGETS[1], CANDIDATE]}}}
    fixture["files"]["effects/admission/control.json"] = copy.deepcopy(base)
    rows = []
    for case_index, (label, condition, combos, admitted, fields) in enumerate(cases):
        record = fbo(CANDIDATE, [0.75, 0.5, 0.25])
        for key, value in fields.items():
            if value is MISSING:
                record.pop(key, None)
            else:
                record[key] = value
        if condition is not MISSING:
            record["conditions"] = condition
        resource = "effects/admission/" + label + ".json"
        effect = copy.deepcopy(base)
        effect["fbos"].insert(0, record)
        if label == "object-fbos":
            effect["fbos"] = {str(index): item for index, item in enumerate(effect["fbos"])}
        fixture["files"][resource] = effect
        for kind in ("image", "text"):
            for control in (False, True):
                obj = owner(len(rows), case_index, label, kind, control,
                            "effects/admission/control.json" if control else resource, combos)
                fixture["objects"].append(obj)
                rows.append({"name": obj["name"], "id": obj["id"], "effect_id": obj["effects"][0]["id"],
                             "case": label, "kind": kind, "control": control, "admitted": admitted,
                             "named": admitted and record.get("name") == CANDIDATE,
                             "origin": obj["origin"], "candidate_name": record.get("name")})
    add_controller(fixture, rows, FBO_SCRIPT)
    return fixture


def make_pass_fixture():
    fixture = base_fixture("effect-pass-conditions")
    fixture["description"] = "Conditional material/copy/swap admission, original override indices, compact material selectors, and actual paired image/text shader consumers."
    cases = condition_cases() + [("rejected-empty-body", [{"KEY": 1}], {}, False)]
    targets = [fbo(name, color) for name, color in zip(TARGETS, CLEARS)]
    passes = [{"material": "materials/admission/write.json", "target": TARGETS[index]} for index in range(3)]
    passes += [{"material": "materials/admission/write.json", "target": TARGETS[0]},
               {"command": "copy", "source": TARGETS[0], "target": TARGETS[1]},
               {"material": "materials/admission/write.json", "target": TARGETS[2]},
               {"command": "swap", "source": TARGETS[0], "target": TARGETS[2]}, read_pass()]
    overrides = [{"constantshadervalues": {"color": color, "opacity": 0.875, "write": 1}}
                 if color is not None else {} for color in COLORS[:4] + [None, COLORS[4], None, None]]
    # Controls write the final three colors directly. They have no conditional passes, no
    # commands, and no instance overrides, so they cannot share the filtering/indexing defect.
    for admitted in (False, True):
        colors = [COLORS[4], COLORS[3], COLORS[3]] if admitted else [COLORS[0], COLORS[1], COLORS[4]]
        control_passes = []
        for index, color in enumerate(colors):
            path = f"materials/admission/control-{int(admitted)}-{index}.json"
            fixture["files"][path] = material("golden/effect-clear-write", color)
            control_passes.append({"material": path, "target": TARGETS[index]})
        fixture["files"][f"effects/admission/control-{int(admitted)}.json"] = {
            "version": 1, "fbos": copy.deepcopy(targets), "passes": control_passes + [read_pass()]}
    rows = []
    for case_index, (label, condition, combos, admitted) in enumerate(cases):
        selected = copy.deepcopy(passes)
        if condition is not MISSING:
            for position in (3, 4, 6):
                selected[position]["conditions"] = copy.deepcopy(condition)
        if label == "rejected-empty-body":
            selected[3] = {"conditions": condition}
        resource = "effects/admission/" + label + ".json"
        fixture["files"][resource] = {"version": 1, "fbos": copy.deepcopy(targets), "passes": selected}
        for kind in ("image", "text"):
            for control in (False, True):
                obj = owner(len(rows), case_index, label, kind, control,
                            f"effects/admission/control-{int(admitted)}.json" if control else resource,
                            combos, None if control else overrides)
                fixture["objects"].append(obj)
                records = (COLORS[:4] + [None, COLORS[4], None, "read"] if admitted
                           else COLORS[:3] + [COLORS[4], "read"])
                if control:
                    records = ([COLORS[4], COLORS[3], COLORS[3]] if admitted
                               else [COLORS[0], COLORS[1], COLORS[4]]) + ["read"]
                rows.append({"name": obj["name"], "id": obj["id"], "effect_id": obj["effects"][0]["id"],
                             "case": label, "kind": kind, "control": control, "admitted": admitted,
                             "origin": obj["origin"], "records": records})
    add_controller(fixture, rows, PASS_SCRIPT)
    return fixture


BIND_SCRIPT = """
function prepare() {
    if (phase === 1) recreate();
    for (let index = 0; index < owners.length; index++) {
        if (phase === 2 && rows[index].kind === 'text') owners[index].text = 'FX\\nFX';
        if (phase === 3)
            write(owners[index].getEffect('admission').getMaterial(0), [0.375, 0.875, 0.25], true);
    }
}
export function update() {
    if (applied !== phase) { applied = phase; age = 0; }
    if (age++ === 0) { prepare(); return status; }
    if (age !== 3) return status;
    const checks = [];
    const expected = phase === 3 ? [0.375, 0.875, 0.25] : seeds[0];
    for (let index = 0; index < owners.length; index++) {
        const row = rows[index], effect = owners[index].getEffect('admission');
        const check = (name, value, wanted) =>
            checks.push([row.name + ':' + name, Object.is(value, wanted), value, wanted]);
        check('effect', effect !== null, true);
        if (effect === null) continue;
        check('count', effect.getMaterialCount(), 2);
        const writer = effect.getMaterial(0);
        check('writer', writer !== null, true);
        if (writer !== null) {
            const color = writer.color;
            for (let channel = 0; channel < 3; channel++)
                check('color-' + channel, color[['x', 'y', 'z'][channel]], expected[channel]);
        }
        check('reader', effect.getMaterial(1) !== null, true);
        check('end', effect.getMaterial(2), null);
    }
    return result(checks);
}
"""


def make_bind_fixture():
    fixture = base_fixture("effect-bind-admission")
    fixture["description"] = "Literal binding records and combo conditions retain an authored sampler or select an independently colored target on cold/recreated image/text owners."
    reader = material("golden/effect-clear-read")
    reader["passes"][0]["textures"] = ["admission-source"] * 3
    fixture["files"]["materials/admission/bind-read.json"] = reader
    fixture["files"]["materials/admission/bind-write.json"] = material(
        "golden/effect-clear-write", COLORS[0])
    cases = [(label, condition, combos, admitted, {})
             for label, condition, combos, admitted in condition_cases()]
    # Missing/ill-typed indices use an undeclared name in the pre-repair reproduction:
    # its name lookup fails before the old parser's uninitialized slot can be consumed.
    # The accepted parser must reject these records before either operation is attempted.
    absent = "_rt_AdmissionUndeclared"
    cases += [
        ("name-missing", MISSING, {}, False, {"name": MISSING}),
        ("name-null", MISSING, {}, False, {"name": None}),
        ("name-number", MISSING, {}, False, {"name": 17}),
        ("name-wrapped", MISSING, {}, False, {"name": {"value": CANDIDATE}}),
        ("index-missing", MISSING, {}, False, {"index": MISSING, "name": absent}),
        ("index-null", MISSING, {}, False, {"index": None, "name": absent}),
        ("index-boolean", MISSING, {}, False, {"index": True, "name": absent}),
        ("index-string", MISSING, {}, False, {"index": "2", "name": absent}),
        ("index-wrapped", MISSING, {}, False, {"index": {"value": 2}}),
        ("index-fraction", MISSING, {}, True, {"index": 2.9}),
        ("index-low-word", MISSING, {}, True, {"index": 4294967298}),
        ("index-negative-word", MISSING, {}, True, {"index": -4294967294}),
        ("index-unsigned-word", MISSING, {}, True, {"index": 18446744069414584322}),
        ("bind-object", MISSING, {}, False, {}),
        ("bind-null", MISSING, {}, False, {}),
        ("entry-number", MISSING, {}, False, {}),
        ("entry-array", MISSING, {}, False, {}),
    ]
    # The control has no conditional or malformed input. It chooses either the authored
    # checker texture or a separate target written directly with the expected color.
    for admitted in (False, True):
        fixture["files"][f"effects/admission/bind-control-{int(admitted)}.json"] = {
            "version": 1, "fbos": [fbo(TARGETS[2], CLEARS[2])],
            "passes": [
                {"material": "materials/admission/bind-write.json", "target": TARGETS[2]},
                {"material": "materials/admission/bind-read.json",
                 "bind": [{"index": 2, "name": TARGETS[2]}] if admitted else []},
            ],
        }
    rows = []
    for case_index, (label, condition, combos, admitted, fields) in enumerate(cases):
        binding = {"index": 2, "name": CANDIDATE}
        for key, value in fields.items():
            if value is MISSING:
                binding.pop(key, None)
            else:
                binding[key] = value
        if condition is not MISSING:
            binding["conditions"] = condition
        bindings = [binding]
        if label == "bind-object":
            bindings = {"0": binding}
        elif label == "bind-null":
            bindings = None
        elif label == "entry-number":
            bindings = [17]
        elif label == "entry-array":
            bindings = [[]]
        resource = "effects/admission/bind-" + label + ".json"
        fixture["files"][resource] = {
            "version": 1, "fbos": [fbo(CANDIDATE, CLEARS[0])],
            "passes": [
                {"material": "materials/admission/bind-write.json", "target": CANDIDATE},
                {"material": "materials/admission/bind-read.json", "bind": bindings},
            ],
        }
        for kind in ("image", "text"):
            for control in (False, True):
                obj = owner(len(rows), case_index, label, kind, control,
                            f"effects/admission/bind-control-{int(admitted)}.json"
                            if control else resource, combos)
                x = obj["origin"].split()[0]
                obj["origin"] = f"{x} {1000 - (case_index // 6) * 110} 0"
                fixture["objects"].append(obj)
                rows.append({"name": obj["name"], "id": obj["id"],
                             "effect_id": obj["effects"][0]["id"],
                             "case": label, "kind": kind, "control": control,
                             "admitted": admitted, "origin": obj["origin"],
                             "sampled_target": (TARGETS[2] if control else CANDIDATE)
                             if admitted else None})
    add_controller(fixture, rows, BIND_SCRIPT)
    return fixture


def make_fixtures():
    return {"fbo-record-admission": make_fbo_fixture(),
            "effect-pass-conditions": make_pass_fixture(),
            "effect-bind-admission": make_bind_fixture()}
