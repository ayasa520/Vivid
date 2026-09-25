"""Live combo selectors and retained/recreated conditional effect consumers."""

import copy
import json

from effect_admission import COLORS, TARGETS, base_fixture, fbo, material, owner


OPTIONS = ["snow", "true", "false", "0", "1", " 1", "1 ", "01", "雪"]
CHOICES = [*OPTIONS, "snow", "rain", "true", "true"]
PHASE_STEPS = 12
CHECKPOINTS = [phase * PHASE_STEPS + 6 for phase in range(len(CHOICES))]


def effect_resource(mode=None):
    """Author the conditional resource or one explicitly resolved control topology."""
    targets = [fbo(name, color) for name, color in zip(TARGETS, COLORS)]
    passes = [{"material": "materials/live/write.json", "target": target}
              for target in TARGETS]
    command = {"command": "copy", "source": TARGETS[0], "target": TARGETS[1]}
    bindings = [{"index": index, "name": target} for index, target in enumerate(TARGETS)]
    if mode is None:
        # The authored condition and each resolved control are separate inputs. The controls
        # contain no predicates and therefore cannot pass by repeating the renderer's decision.
        for record in (targets[2], passes[2], command, bindings[2]):
            record["conditions"] = [{"MODE": 1}]
        passes.append(command)
    elif mode == 0:
        targets = targets[:2]
        passes = passes[:2]
        bindings = bindings[:2]
    else:
        passes.append(command)
    passes.append({"material": "materials/live/read.json", "bind": bindings})
    return {"name": "Live topology", "fbos": targets, "passes": passes,
            "functions": {"wipe": {"action": "clear", "fbos": list(reversed(TARGETS))}}}


CONTROLLER = """
let phase = 0, choice = 'snow', mode = '0', applied = -1, age = 0, status = 'pending';
const active = {};
const notifications = [];

export function init() {
    for (const row of rows) {
        active[row.key] = {api: thisScene.getLayer(row.api.name),
                           control: thisScene.getLayer(row.control.name), mode: row.mode};
    }
}

export function applyUserProperties(value) {
    if (value.phase !== undefined) phase = Number(value.phase);
    if (value.choice !== undefined) choice = value.choice;
    if (value.MODE !== undefined) mode = value.MODE;
    notifications.push({phase, keys: Object.keys(value).sort(), choice: value.choice});
}

function recreate(row) {
    const pair = active[row.key];
    thisScene.destroyLayer(pair.api);
    thisScene.destroyLayer(pair.control);
    const api = JSON.parse(row.apiText), control = JSON.parse(row.controlText);
    let selected = Number(mode);
    if (row.raw === 'inverse') selected = 1 - selected;
    if (row.raw === 'wrapped') {
        api.effects[0].combos.MODE = {value: selected, user: 'MODE'};
        selected = 0;
    } else {
        api.effects[0].combos.MODE = selected;
    }
    control.effects[0].file = 'effects/live-control-' + selected + '/effect.json';
    active[row.key] = {api: thisScene.createLayer(api), control: thisScene.createLayer(control),
                       mode: selected};
}

function write(effect, selected, enabled) {
    const count = selected === 1 ? 3 : 2;
    for (let index = 0; index < count; index++) {
        const item = effect.getMaterial(index);
        if (item === null) continue;
        item.color = new Vec3(colors[index][0], colors[index][1], colors[index][2]);
        item.opacity = 0.875;
        item.write = enabled ? 1 : 0;
    }
}

function prepare() {
    for (const row of rows) {
        if (row.section !== 'topology') continue;
        if (row.cohort === 'fresh' && phase !== 0) recreate(row);
        const pair = active[row.key];
        if (row.kind === 'text') {
            // Relayout and visibility changes consume retained records. Fresh controls also
            // exercise a new constructor under the current input without mutating old owners.
            pair.api.text = pair.control.text = phase % 4 === 3 ? 'FX\\nFX' : 'FX';
        }
        for (const item of [pair.api, pair.control]) {
            const effect = item.getEffect('admission');
            effect.visible = phase % 3 !== 2;
            write(effect, pair.mode, true);
        }
    }
}

function check(checks, key, actual, expected) {
    checks.push([key, JSON.stringify(actual) === JSON.stringify(expected), actual, expected]);
}

export function update() {
    if (applied !== phase) { applied = phase; age = 0; prepare(); }
    const step = age++;
    for (const row of rows) {
        if (row.section !== 'selector') continue;
        const pair = active[row.key], selected = choices[phase] === row.option;
        if (row.target === 'callback') pair.api.visible = choice === row.option;
        if (row.target !== 'effect') pair.control.visible = selected;
        else pair.control.getEffect('admission').visible = selected;
    }
    if (step === 3 && phase % 2 === 1) {
        for (const row of rows) {
            if (row.section !== 'topology') continue;
            const pair = active[row.key];
            for (const item of [pair.api, pair.control]) {
                const effect = item.getEffect('admission');
                write(effect, pair.mode, false);
                effect.executeMaterialFunction('wipe');
            }
        }
    }
    if (step !== 5) return status;
    const checks = [], topology = [];
    check(checks, 'input-choice', choice, choices[phase]);
    check(checks, 'input-mode', mode, String(phase % 2));
    const notification = notifications[notifications.length - 1];
    check(checks, 'notification-choice-present', notification.keys.includes('choice'),
          phase === 0 || choices[phase] !== choices[phase - 1]);
    for (const row of rows) {
        const pair = active[row.key], effect = pair.api.getEffect('admission');
        check(checks, row.key + ':effect-count', pair.api.getEffectCount(), 1);
        if (row.section === 'selector') {
            const selected = choices[phase] === row.option;
            check(checks, row.key + ':selected',
                  row.target !== 'effect' ? pair.api.visible : effect.visible, selected);
            check(checks, row.key + ':material-count', effect.getMaterialCount(), 1);
            continue;
        }
        const selected = pair.mode, count = selected === 1 ? 5 : 3;
        check(checks, row.key + ':visible', effect.visible, phase % 3 !== 2);
        check(checks, row.key + ':material-count', effect.getMaterialCount(), count);
        const records = [];
        for (let index = 0; index < count; index++) {
            const item = effect.getMaterial(index);
            const expected = index === count - 1 ? 'materials/live/read.json'
                : selected === 1 && index === 3 ? null : 'materials/live/write.json';
            records.push(item !== null);
            check(checks, row.key + ':record-' + index, item !== null, expected !== null);
            if (item !== null && expected === 'materials/live/write.json') {
                const color = item.color;
                check(checks, row.key + ':color-' + index, [color.x, color.y, color.z], colors[index]);
            }
        }
        check(checks, row.key + ':read-name', effect.getMaterial('materials/live/read.json') !== null, true);
        check(checks, row.key + ':end', effect.getMaterial(count), null);
        topology.push({key: row.key, selected, records, cohort: row.cohort, raw: row.raw});
    }
    const failures = checks.filter(item => !item[1]).length;
    status = 'live topology phase=' + phase + ' failures=' + failures;
    localStorage.set('live-topology-' + phase, {phase, choice, mode, notification, failures, checks, topology});
    return status;
}
"""


def make_fixture():
    fixture = base_fixture("effect-live-topology")
    fixture["description"] = "Literal combo option visibility and conditional effect records across live input, relayout and reconstruction."
    fixture["docs"].append("scene/userproperties/combo.md")
    fixture["docs"].append("scene/scenescript/reference/event/applyUserProperties.md")
    fixture["properties"] = {
        "phase": {"type": "slider", "value": 0, "min": 0, "max": len(CHOICES) - 1},
        "choice": {"type": "combo", "value": "snow",
                   "options": [{"label": option, "value": option} for option in [*OPTIONS, "rain"]]},
        "MODE": {"type": "combo", "value": "0",
                 "options": [{"label": "Zero", "value": "0"}, {"label": "One", "value": "1"}]},
    }
    read = material("golden/effect-clear-read")
    read["passes"][0]["textures"] = ["admission-source"] * 3
    fixture["files"].update({
        "materials/live/write.json": material("golden/effect-clear-write", COLORS[0]),
        "materials/live/read.json": read,
        "effects/live-selection/effect.json": {"name": "Selection", "passes": [
            {"material": "materials/live/write.json"}]},
        "effects/live-conditional/effect.json": effect_resource(),
        "effects/live-control-0/effect.json": effect_resource(0),
        "effects/live-control-1/effect.json": effect_resource(1),
    })
    rows = []
    case_index = 0

    def add_pair(label, kind, resource, combos, **extra):
        index = len(fixture["objects"])
        api = owner(index, case_index, label, kind, False, resource, combos)
        control = owner(index + 1, case_index, label, kind, True, resource)
        row = {"key": label + "-" + kind, "kind": kind, "api": api, "control": control,
               "apiX": float(api["origin"].split()[0]),
               "controlX": float(control["origin"].split()[0]),
               "y": float(api["origin"].split()[1]), **extra}
        rows.append(row)
        fixture["objects"].extend([api, control])
        return row

    for option in OPTIONS:
        targets = ("layer", "effect", "callback") if option in (" 1", "1 ", "01") else ("layer", "effect")
        for target in targets:
            for kind in ("image", "text"):
                row = add_pair("selector-" + str(case_index), kind,
                               "effects/live-selection/effect.json", {}, section="selector",
                               option=option, target=target, mode=0)
                binding = {"value": case_index % 2 == 0,
                           "user": {"name": "choice", "condition": option}}
                if target == "layer":
                    row["api"]["visible"] = binding
                elif target == "effect":
                    row["api"]["effects"][0]["visible"] = binding
            case_index += 1

    for cohort in ("retained", "fresh"):
        for raw, combo, selected in (("follow", 0, 0), ("inverse", 1, 1),
                                     ("wrapped", {"value": 1, "user": "MODE"}, 0)):
            for kind in ("image", "text"):
                row = add_pair("topology-" + cohort + "-" + raw, kind,
                               "effects/live-conditional/effect.json", {"MODE": copy.deepcopy(combo)},
                               section="topology", cohort=cohort, raw=raw, mode=selected)
                row["control"]["effects"][0]["file"] = f"effects/live-control-{selected}/effect.json"
            case_index += 1

    for row in rows:
        row["apiText"] = json.dumps(row["api"], ensure_ascii=False, separators=(",", ":"))
        row["controlText"] = json.dumps(row["control"], ensure_ascii=False, separators=(",", ":"))
    script = "'use strict';\nconst rows = " + json.dumps(rows, ensure_ascii=False) + ";\n"
    script += "const colors = " + json.dumps(COLORS) + ";\n"
    script += "const choices = " + json.dumps(CHOICES, ensure_ascii=False) + ";\n" + CONTROLLER
    fixture["layers"] = [{"kind": "text", "name": "live-topology-controller", "origin": [960, 1060, 0],
                          "size": [1800, 40], "pointsize": 4, "padding": 0,
                          "text": {"value": "pending", "script": script}}]
    fixture["files"]["live-topology-expectations.json"] = rows
    return fixture
