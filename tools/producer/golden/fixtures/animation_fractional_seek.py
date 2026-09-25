"""Exercise fractional seeks and final loop intervals through the three-bone head."""

import json


PERCENTAGES = [0.3125, 0.25, 0.0625, 0, 0.1875, 0.125,
               0.5625, 0.5, 0.6875, 0.625, 0.8125, 0.75]
TERMINAL_SEEKS = [119, 119.125, 119.25, 119.5, 119.75, 119.875, 119.99,
                  119, 118.5, 0]


def make_fixture():
    # Keep the first seek in init. Pausing immediately after it isolates the written
    # pose from elapsed playback; later updates reuse the same percentage helper.
    # Integer inputs interleaved with half frames preserve a control for ordinary seeks.
    script = """'use strict';
export var scriptProperties = createScriptProperties()
    .addSlider({name: 'percentage', label: 'Initial progress', value: 1,
                min: 0, max: 1, integer: false}).finish();
const percentages = PERCENTAGES;
const loopStarts = [118.5, 118.75];
const terminalSeeks = TERMINAL_SEEKS;
let step = 0;
export function init(value) {
    shared.offsetedStartAni('addEndedCallback' in thisObject ? thisObject : thisObject.getAnimation(),
                           scriptProperties.percentage);
    thisObject.pause();
    return value;
}
export function update(value) {
    if (step > 0 && step < percentages.length) {
        shared.offsetedStartAni(thisObject, percentages[step]);
        thisObject.pause();
    }
    // Each appended four-step sequence starts inside the preceding interval and then
    // advances normally through the final stored interval and the loop boundary. This
    // makes the terminal pose observable without seeking directly into that interval.
    const loopStep = step - percentages.length;
    const loopSteps = loopStarts.length * 4;
    if (loopStep >= 0 && loopStep < loopSteps && loopStep % 4 === 0) {
        thisObject.setFrame(loopStarts[loopStep / 4]);
        thisObject.play();
    }
    // Paused requests distinguish the final interval's fractional positions from its
    // first pose. Controls around the sequence also expose a retained seek or clock.
    const terminalStep = loopStep - loopSteps;
    if (terminalStep >= 0 && terminalStep < terminalSeeks.length) {
        thisObject.pause();
        thisObject.setFrame(terminalSeeks[terminalStep]);
    }
    const matrices = [];
    for (let bone = 0; bone < thisLayer.getBoneCount(); bone++) {
        matrices.push(thisLayer.getLocalBoneTransform(bone));
    }
    if (step < percentages.length) {
        console.log('SEEK_LOCAL', JSON.stringify({step: step, percentage: percentages[step],
            requested: thisObject.frameCount * percentages[step], blend: thisObject.blend,
            playing: thisObject.isPlaying(), matrices: matrices}));
    } else if (loopStep < loopSteps) {
        console.log('LOOP_LOCAL', JSON.stringify({step: step, phase: loopStep,
            frame: thisObject.getFrame(), blend: thisObject.blend,
            playing: thisObject.isPlaying(), matrices: matrices}));
    } else {
        console.log('TERMINAL_SEEK_LOCAL', JSON.stringify({step: step, phase: terminalStep,
            requested: terminalSeeks[terminalStep], blend: thisObject.blend,
            playing: thisObject.isPlaying(), matrices: matrices}));
    }
    step++;
    return value;
}
""".replace("PERCENTAGES", json.dumps(PERCENTAGES)).replace("TERMINAL_SEEKS", json.dumps(TERMINAL_SEEKS))
    return {
        "description": "A three-bone animation retains six paused half-frame seeks and six integer controls, plays two sequences across the loop boundary, then pauses at six positions in the final interval and four controls; observe local poses and actual skinning uploads.",
        "docs": ["scene/scenescript/reference/class/IAnimationLayer.md"],
        "scenario": "animation-fractional-seek",
        "asset_files": ["models/头_puppet.mdl", "materials/头.tex"],
        "files": {
            "models/头.json": {"autosize": True, "cropoffset": "-60.00000 -379.00000",
                              "material": "materials/头.json", "puppet": "models/头_puppet.mdl"},
            "materials/头.json": {"passes": [{"alphawriting": "default", "blending": "translucent",
                "combos": {}, "cullmode": "normal", "depthtest": "disabled", "depthwrite": "disabled",
                "shader": "genericimage4", "textures": ["头"]}]},
        },
        "objects": [
            {"id": 10001, "name": "authored-animation-helper", "origin": "0 0 0",
             "visible": {"value": True, "script": """'use strict';
function offsetedStartAni(ani, percentage=1) {
    ani.play();
    ani.setFrame(ani.frameCount * percentage);
}
shared.offsetedStartAni = offsetedStartAni;
"""}},
            {"id": 697, "name": "head-fractional-seek", "image": "models/头.json",
             "origin": "960.00000 540.00000 0.00000", "size": "548.00000 678.00000",
             "castshadow": False, "clampuvs": True, "disablepropagation": False,
             "animationlayers": [
                 {"id": 460, "name": "disabled-base", "animation": 374, "additive": False,
                  "blend": 1.0, "blendin": False, "blendout": False, "blendtime": 0.5,
                  "rate": 1.0, "visible": False},
                 {"id": 695, "name": "percentage-seek", "animation": 690, "additive": True,
                  "blend": 0.74000001, "blendin": False, "blendout": False, "blendtime": 0.5,
                  "rate": 1.0, "visible": {"value": True, "script": script,
                                            "scriptproperties": {"percentage": PERCENTAGES[0]}}},
             ]},
        ],
    }
