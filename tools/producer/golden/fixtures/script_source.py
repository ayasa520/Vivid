"""Authored script text, callback admission and actual text/visibility consumers."""

import json


def cases():
    return [
        ("single", "'export value'", "export value", "", "export "),
        ("double", '"export function value"', "export function value", "", "export "),
        ("strict-text", '"\'use strict\';"', "'use strict';", "", "export "),
        ("nbsp-data", "'left\u00a0right'", "left\u00a0right", "", "export\u00a0"),
        ("template", "`export value\nimport text`", "export value\nimport text", "", "export "),
        ("nested-template", '`outer ${`export ${"value"}`} tail`', "outer export value tail", "", "export "),
        ("regexp", r"/export [}]+/.source", "export [}]+", "", "export "),
        ("comment-end", "'comment kept'", "comment kept", "/* comment\nimport ignored */\n", "export "),
        ("export-line", "'line boundary'", "line boundary", "", "export\n"),
        ("export-comment", "'comment boundary'", "comment boundary", "", "export/* declaration */\n"),
        ("namespace-import", "String(WEMath.clamp(0.375, 0, 1))", "0.375",
         "import * as WEMath from 'WEMath';\n", "export "),
        ("same-line-import", "String(WEMath.clamp(0.625, 0, 1))", "0.625",
         "import * as WEMath from 'WEMath'; ", "export "),
        ("nested-strict", "strictCheck() + ':' + looseCheck()", "true:false",
         "function strictCheck() { 'use strict'; const x = Object.freeze({a: 1}); "
         "try { x.a = 2; } catch (e) { return e instanceof TypeError && "
         "(function() { 'use strict'; return this === undefined; })(); } return false; }\n"
         "function looseCheck() { return this === undefined; }\n", "export "),
        ("division-regexp", "String(value) + ':' + pattern.source", "4:export [}]",
         "const value = (12 / 3); let pattern; if (value) pattern = /export [}]/;\n", "export "),
        ("async-regexp", "value", "good",
         "let value = 'good'; async function nothing() {} "
         "/export function/.test(String.fromCodePoint(101,120,112,111,114,116,32,102,117,110,99,116,105,111,110)) "
         "|| (value = 'bad');\n", "export "),
        ("class-division", "String(Number.isNaN(value)) + ':' + marker", "true:class",
         "const value = class {} / 2; export var marker = 'class';\n", "export "),
    ]


def make_fixture():
    files = {}
    objects = []
    rows = []
    for index, (name, expression, expected, preamble, export) in enumerate(cases()):
        # Expected text is supplied as ordinary scene data and independently encoded code
        # points. Repeating the same literal inside the probe would let source corruption
        # change both sides of the comparison and hide the defect.
        source = preamble + "function sample() { return " + expression + "; }\n"
        source += export + "function init() { return sample(); }\n"
        source += export + "function update() { const value = sample(); localStorage.set("
        source += json.dumps("script-source-" + name) + ", value); return value; }\n"
        x = 330 + 960 * (index % 2)
        y = 1010 - 128 * (index // 2)
        rows.append({"name": name, "codes": [ord(c) for c in expected],
                     "marker": [x / 2, (1080 - y + 60) / 2]})
        for control in (False, True):
            label = name + ("-control" if control else "-candidate")
            objects.append({"id": 100 + 2 * index + int(control), "name": label,
                            "text": expected if control else {"value": "pending", "script": source},
                            "origin": f"{x + (420 if control else 0)} {y} 0",
                            "angles": "0 0 0", "scale": "1 1 1", "visible": True,
                            "pointsize": 8, "padding": 0, "horizontalalign": "center",
                            "verticalalign": "center", "color": "1 1 1", "alpha": 1})
        codes = json.dumps(rows[-1]["codes"])
        script = ("const expected = String.fromCodePoint(..." + codes + ");\n"
                  "export function update() { const value = thisScene.getLayer(" +
                  json.dumps(name + "-candidate") + ").text; const pass = value === expected; "
                  "localStorage.set(" + json.dumps("script-consumer-" + name) +
                  ", {value, expected, pass}); return pass; }\n")
        for control in (False, True):
            label = name + ("-marker-control" if control else "-marker")
            material = "materials/" + label + ".json"
            files[material] = {"passes": [{"shader": "golden/effect-clear-write", "textures": [],
                "blending": "normal", "cullmode": "nocull", "depthtest": "disabled",
                "depthwrite": "disabled", "constantshadervalues": {
                    "color": [0.125, 0.75, 0.375],
                    "opacity": 1, "write": 1}}]}
            model = "models/" + label + ".json"
            files[model] = {"material": material, "width": 280, "height": 32,
                            "autosize": False, "fullscreen": False}
            objects.append({"id": 1000 + 2 * index + int(control), "name": label, "image": model,
                            "origin": f"{x + (420 if control else 0)} {y - 60} 0",
                            "angles": "0 0 0", "scale": "1 1 1", "size": "280 32",
                            "visible": True if control else {"value": True, "script": script}})
    files["script-source-expectations.json"] = rows
    return {"description": "Preserve authored strings, comments, templates, regexp text and strict "
                           "directives while adapting callback exports and namespace import syntax.",
            "docs": ["scene/scenescript/introduction.md", "scene/scenescript/reference/event/update.md",
                     "scene/scenescript/reference/module/WEMath.md"],
            "scenario": "fixture-script-source",
            "asset_files": ["shaders/golden/effect-clear-write.vert", "shaders/golden/effect-clear-write.frag"],
            "files": files, "objects": objects, "layers": []}
