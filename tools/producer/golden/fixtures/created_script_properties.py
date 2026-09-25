"""Created-owner initialization and property notification consumers."""

import copy
import json


VARIANTS = ["binding", "alpha", "material", "layer-visible", "effect-visible"]
CHECKPOINTS = [3, 7, 13, 23, 33, 43, 53, 63, 73, 85, 93, 103, 123, 133, 149]


def property_script(key, variant, nested=None):
    source = "'use strict';\nconst key = " + json.dumps(key) + ";\nlet record;\n"
    source += """
export var scriptProperties = createScriptProperties()
    .addSlider({name: 'level', label: 'Level', value: 0.125, min: 0, max: 1}).finish();
export function init(value) {
    // Material and owner listeners need not initialize in the driver's order. Each listener
    // preserves records already written by another listener in the same bootstrap.
    shared.creationInitCounts = shared.creationInitCounts || {};
    shared.creationRecords = shared.creationRecords || {};
    shared.creationDummies = shared.creationDummies || [];
    const counts = shared.creationInitCounts;
    counts[key] = (counts[key] || 0) + 1;
    record = {initCount: counts[key], initValue: value, initLevel: scriptProperties.level,
              events: 0, lastEpoch: -1, payloads: []};
    shared.creationRecords[key] = record;
"""
    if nested is not None:
        source += "    shared.creationDummies.push(thisScene.createLayer(" + json.dumps(nested) + "));\n"
    source += "    return " + ("0.375" if variant == "alpha" else "value") + ";\n}\n"
    source += """
export function applyUserProperties(value) {
    record.events++;
    record.payloads.push({keys: Object.keys(value).sort(), epoch: value.epoch});
    if (value.epoch !== undefined) record.lastEpoch = Number(value.epoch);
}
"""
    if variant == "material":
        source += """
export function update() {
    record.level = scriptProperties.level;
    return new Vec3(record.events / 8, scriptProperties.level, (record.lastEpoch + 1) / 8);
}
"""
    elif variant in ("layer-visible", "effect-visible"):
        source += "export function update() { return record.events % 2 === 0; }\n"
    return source


def owner(index, name, kind, x, y):
    result = {"id": 100 + index, "name": name, "origin": f"{x} {y} 0",
              "angles": "0 0 0", "scale": "1 1 1", "size": "192 64",
              "visible": True, "alpha": 1, "parallaxDepth": "0 0"}
    if kind == "image":
        result["image"] = "models/creation-source.json"
    else:
        result.update({"text": "INIT", "scale": "0.10 0.10 1", "color": "1 1 1",
                       "brightness": 1, "colorBlendMode": 0, "anchor": "0 0",
                       "pointsize": 48, "horizontalalign": "center", "verticalalign": "center",
                       "padding": 0, "copybackground": False, "opaquebackground": False,
                       "backgroundcolor": "0 0 0", "backgroundbrightness": 1,
                       "perspective": False, "solid": True})
    return result


CONTROLLER = """
let epoch = -1, previousEpoch = -2, opacity = 0.75, level = 0.625;
let tick = 0, status = 'pending';
const active = {}, cohorts = {};

function createCohort(name, cold) {
    const prior = cohorts[name];
    cohorts[name] = {birth: cold ? -1 : epoch, generation: prior ? prior.generation + 1 : 1,
                     binding: opacity};
    for (const row of rows.filter(row => row.cohort === name)) {
        const api = thisScene.createLayer(row.api), control = thisScene.createLayer(row.control);
        active[row.key] = {api, control};
        // Record the synchronous return separately from the next update and from the first
        // wallpaper property event. A later draw must not conceal an init result overwritten
        // before createLayer returns.
        shared.creationReturns.push({key: row.key, generation: cohorts[name].generation,
                                     alpha: api.alpha, visible: api.visible,
                                     events: shared.creationRecords[row.key]?.events});
    }
}

export function init() {
    shared.creationRecords = shared.creationRecords || {};
    shared.creationInitCounts = shared.creationInitCounts || {};
    shared.creationReturns = shared.creationReturns || [];
    shared.creationDummies = shared.creationDummies || [];
    cohorts.static = {birth: -1, generation: 1, binding: opacity};
    for (const row of rows.filter(row => row.cohort === 'static')) {
        active[row.key] = {api: thisScene.getLayer(row.api.name),
                           control: thisScene.getLayer(row.control.name)};
    }
    createCohort('init', true);
}

export function applyUserProperties(value) {
    // The driver responds only to its explicit input fields. Empty notifications under
    // investigation cannot advance its independent expectations or hide extra callbacks.
    if (value.epoch !== undefined) epoch = Number(value.epoch);
    if (value.opacity !== undefined) opacity = Number(value.opacity);
    if (value.level !== undefined) level = Number(value.level);
}

function addPlainOwners() {
    for (const config of dummies) shared.creationDummies.push(thisScene.createLayer(config));
}

function manualBindingWrite() {
    for (const cohort of Object.values(cohorts)) cohort.binding = 0.25;
    for (const row of rows) {
        if (row.variant === 'binding' && active[row.key]) active[row.key].api.alpha = 0.25;
    }
}

function checkEqual(checks, name, actual, expected) {
    const equal = typeof expected === 'number' ? Math.abs(actual - expected) < 0.00001
                                               : JSON.stringify(actual) === JSON.stringify(expected);
    checks.push([name, equal, actual, expected]);
}

export function update() {
    const step = tick++;
    if (epoch !== previousEpoch) {
        previousEpoch = epoch;
        for (const cohort of Object.values(cohorts)) cohort.binding = opacity;
    }
    if (step === 5 || step === 60) manualBindingWrite();
    if ([10, 40, 70, 90, 120].includes(step)) addPlainOwners();
    if (step === 20) createCohort('live', false);
    if (step === 80) {
        for (const row of rows.filter(row => row.cohort === 'live')) {
            thisScene.destroyLayer(active[row.key].api);
            thisScene.destroyLayer(active[row.key].control);
            delete active[row.key];
        }
    }
    if (step === 82) createCohort('live', false);

    const checks = [];
    for (const row of rows) {
        const pair = active[row.key];
        if (!pair) continue;
        const cohort = cohorts[row.cohort], events = epoch - cohort.birth;
        const delivered = events > 0;
        const expectedLevel = delivered ? level : 0.125;
        const expectedEpoch = delivered ? epoch : -1;
        let expectedAlpha = 1, expectedVisible = true;
        if (row.variant === 'binding') expectedAlpha = cohort.binding;
        if (row.variant === 'alpha') expectedAlpha = delivered ? opacity : 0.375;
        if (row.variant === 'layer-visible') expectedVisible = events % 2 === 0;
        pair.control.alpha = expectedAlpha;
        pair.control.visible = expectedVisible;
        if (row.variant === 'material') {
            pair.control.getEffect('state').getMaterial(0).tint =
                new Vec3(events / 8, expectedLevel, (expectedEpoch + 1) / 8);
        }
        if (row.variant === 'effect-visible') {
            pair.control.getEffect('state').visible = events % 2 === 0;
        }
        if (!checkpoints.includes(step)) continue;
        checkEqual(checks, row.key + ':alpha', pair.api.alpha, expectedAlpha);
        checkEqual(checks, row.key + ':visible', pair.api.visible, expectedVisible);
        if (row.variant === 'binding') continue;
        const record = shared.creationRecords[row.key];
        checkEqual(checks, row.key + ':initCount', record.initCount, cohort.generation);
        checkEqual(checks, row.key + ':events', record.events, events);
        checkEqual(checks, row.key + ':lastEpoch', record.lastEpoch, expectedEpoch);
        checkEqual(checks, row.key + ':initLevel', record.initLevel, 0.125);
        if (row.variant === 'material') checkEqual(checks, row.key + ':level', record.level, expectedLevel);
        if (row.variant === 'effect-visible') {
            checkEqual(checks, row.key + ':effect-visible', pair.api.getEffect('state').visible, events % 2 === 0);
        }
        const expectedPayloads = [];
        for (let current = cohort.birth + 1; current <= epoch; current++) {
            expectedPayloads.push({keys: ['epoch', 'level', 'opacity'], epoch: current});
        }
        checkEqual(checks, row.key + ':payloads', record.payloads, expectedPayloads);
    }
    if (checkpoints.includes(step)) {
        for (const result of shared.creationReturns) {
            const row = rows.find(row => row.key === result.key);
            if (row.variant !== 'binding') {
                checkEqual(checks, result.key + ':return-events-' + result.generation, result.events, 0);
            }
            if (row.variant === 'alpha') {
                checkEqual(checks, result.key + ':return-alpha-' + result.generation, result.alpha, 0.375);
            }
        }
        const failures = checks.filter(check => !check[1]).length;
        status = 'creation step=' + step + ' failures=' + failures;
        localStorage.set('created-script-properties-' + step,
                         {step, epoch, failures, checks, records: shared.creationRecords,
                          returns: shared.creationReturns});
    }
    return status;
}
"""


def make_fixture():
    source = {"passes": [{"shader": "genericimage2", "blending": "translucent",
                           "cullmode": "nocull", "depthtest": "disabled", "depthwrite": "disabled",
                           "textures": ["creation-source"]}]}
    tint = {"passes": [{"shader": "golden/creation-state", "blending": "normal",
                         "cullmode": "nocull", "depthtest": "disabled", "depthwrite": "disabled",
                         "textures": [], "constantshadervalues": {"tint": "0.75 0.25 0.625"}}]}
    fixture = {
        "description": "Creation preserves existing bindings and callbacks; new image/text scripts initialize before real property notifications.",
        "docs": ["scene/scenescript/reference/event/init.md",
                 "scene/scenescript/reference/event/applyUserProperties.md",
                 "scene/scenescript/reference/class/IScene.md"],
        "scenario": "fixture-created-script-properties",
        "asset_files": ["shaders/golden/creation-state.vert", "shaders/golden/creation-state.frag"],
        "texture_assets": {"creation-source": {"kind": "checker", "size": [192, 64]}},
        "properties": {"epoch": {"type": "slider", "value": 0, "min": 0, "max": 3},
                       "opacity": {"type": "slider", "value": 0.75, "min": 0, "max": 1},
                       "level": {"type": "slider", "value": 0.625, "min": 0, "max": 1}},
        "files": {"materials/creation-source.json": source,
                  "models/creation-source.json": {"material": "materials/creation-source.json",
                       "autosize": False, "fullscreen": False, "width": 192, "height": 64},
                  "materials/creation-state.json": tint,
                  "effects/creation-state.json": {"name": "state", "group": "golden", "passes": [
                      {"material": "materials/creation-state.json"}]}},
        "layers": [], "objects": [],
    }
    rows, index = [], 0
    dummies = [owner(9000 + number, "plain-created-" + kind, kind, -500, -500)
               for number, kind in enumerate(("image", "text"))]
    for column, cohort in enumerate(("static", "init", "live")):
        for variant_index, variant in enumerate(VARIANTS):
            for kind_index, kind in enumerate(("image", "text")):
                row_index = variant_index * 2 + kind_index
                key = cohort + "-" + kind + "-" + variant
                y = 1020 - row_index * 98
                api = owner(index, key, kind, 140 + column * 640, y)
                index += 1
                control = owner(index, key + "-control", kind, 440 + column * 640, y)
                index += 1
                nested = dummies[0] if cohort == "live" and kind == "image" and variant == "material" else None
                script = {"script": property_script(key, variant, nested),
                          "scriptproperties": {"level": {"value": 0.125, "user": "level"}}}
                if variant == "binding":
                    api["alpha"] = {"value": 0.5, "user": "opacity"}
                if variant == "alpha":
                    api["alpha"] = {"value": 0.5, "user": "opacity", **script}
                if variant == "layer-visible":
                    api["visible"] = {"value": True, **script}
                if variant in ("material", "effect-visible"):
                    effect = {"id": 10000 + index, "name": "state", "file": "effects/creation-state.json"}
                    api["effects"] = [copy.deepcopy(effect)]
                    control["effects"] = [copy.deepcopy(effect)]
                    if variant == "material":
                        api["effects"][0]["passes"] = [{"constantshadervalues": {
                            "tint": {"value": "0.75 0.25 0.625", **script}}}]
                    else:
                        api["effects"][0]["visible"] = {"value": True, **script}
                if cohort == "static":
                    fixture["objects"].extend([api, control])
                rows.append({"key": key, "cohort": cohort, "variant": variant, "kind": kind,
                             "api": api, "control": control, "centers": [140 + column * 640, 440 + column * 640, y]})

    controller = "'use strict';\nconst rows = " + json.dumps(rows, separators=(",", ":")) + ";\n"
    controller += "const dummies = " + json.dumps(dummies, separators=(",", ":")) + ";\n"
    controller += "const checkpoints = " + json.dumps(CHECKPOINTS) + ";\n" + CONTROLLER
    fixture["layers"] = [{"kind": "text", "name": "creation-controller", "origin": [960, 24, 0],
                          "size": [1800, 28], "pointsize": 4, "padding": 0,
                          "text": {"value": "pending", "script": controller}}]
    fixture["files"]["creation-expectations.json"] = rows
    return fixture
