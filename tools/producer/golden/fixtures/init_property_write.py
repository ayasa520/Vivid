"""Observe material initialization through existing image/text color-band consumers."""

import copy
import json
import math
import struct


def float32(value):
    return struct.unpack("f", struct.pack("f", value))[0]


def scalar_marker(tag, value):
    scalar = max(0.0625, min(0.75, 0.5 + value * 0.125))
    return [scalar, tag * 0.0625, 0.75 - scalar * 0.25]


def make_fixture(base):
    fixture = copy.deepcopy(base)
    fixture["description"] = (
        "Material init callbacks preserve direct writes when returning undefined; numeric "
        "returns replace the property, with angular scalars crossing the degree boundary."
    )
    fixture["docs"] = ["scene/scenescript/reference/event/init.md",
                       "scene/scenescript/reference/class/IMaterial.md"]
    fixture["scenario"] = "fixture"
    fixture["properties"] = {}
    fixture["layers"] = []
    fixture["log_expectations"] = {}

    # Reuse the established pass graph and shaders, but remove the bulk writer. Each
    # callback runs once and subsequent updates only observe the property. The right-hand
    # controls are authored from the expected values, never from script readback.
    fixture["files"]["materials/bulk/api-3.json"]["passes"][0]["combos"]["BULK_KIND"] = 5
    radians = float32(-45 * float32(math.pi / 180))
    markers = [scalar_marker(1, 0.625), scalar_marker(2, 0.375),
               [0.28125, 0.359375, 0.1875], scalar_marker(4, radians),
               [0.328125, 0.3125, 0.375], scalar_marker(6, radians),
               scalar_marker(7, -22.5), [0.203125, 0.5, 0.875]]
    records = [0, 1, 2, 4, 5, 6, 7, 8]
    cases = [
        (0, "gain", "direct", 0.125, 0.625, None, 0.625),
        (1, "gain", "return", 0.25, 0.875, "value * 1.5", 0.375),
        (4, "angle", "angle-return", -22.5, None, "value * 2", -45),
        (6, "angle", "angle-direct", 90, -45, None, -45),
        (7, "plainangle", "plain-return", 90, None, "value / -4", -22.5),
    ]
    for owner in fixture["objects"]:
        passes = owner["effects"][0]["passes"]
        for tag, index in enumerate(records, 1):
            passes[index] = {"constantshadervalues": {"tag": tag}}
        if "control" in owner["name"]:
            for index, marker in zip(records, markers):
                passes[index]["constantshadervalues"]["marker"] = " ".join(map(str, marker))
            continue
        label = owner["name"].removeprefix("bulk-api-")
        for index, prop, case, initial, assigned, returned, expected in cases:
            prefix = f"INIT_PROPERTY {label} {case}"
            assignment = f"thisObject.{prop} = {assigned};" if assigned is not None else ""
            result = f"return {returned};" if returned is not None else ""
            source = f"""'use strict';
let observed = false;
export function init(value) {{
    const before = thisObject.{prop};
    {assignment}
    console.log({json.dumps(prefix)}, 'init', value, before, thisObject.{prop});
    {result}
}}
export function update(value) {{
    if (!observed) {{
        observed = true;
        console.log({json.dumps(prefix)}, 'update', value, thisObject.{prop});
    }}
}}
"""
            # Serialized shader constants already use shader units. The public callback
            # crosses the degree boundary after reading those stored radian values.
            authored = float32(initial * float32(math.pi / 180)) if prop == "angle" else initial
            passes[index]["constantshadervalues"][prop] = {"value": authored, "script": source}
            log_prefix = "INFO SceneScript log: " + prefix
            after = assigned if assigned is not None else initial
            fixture["log_expectations"][log_prefix] = [
                f"{log_prefix} init {initial} {initial} {after}",
                f"{log_prefix} update {expected} {expected}",
            ]
    return fixture
