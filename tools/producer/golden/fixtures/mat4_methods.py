"""Append finite matrix operations to the existing transform and attachment consumers."""

import copy
import json
from pathlib import Path


HERE = Path(__file__).resolve().parent


def make_fixture(base):
    fixture = copy.deepcopy(base)
    inputs = json.loads((HERE / "mat4_cases.json").read_text())
    helpers = (HERE / "mat4_methods.js").read_text()
    disc = next(layer for layer in fixture["layers"] if layer["name"] == "disc")
    # Retain initialization and the first ninety attachment frames verbatim. The added
    # callback advances once per draw, then consumes one selected matrix result per step.
    script = """
const mat4Inputs = MATRIX_INPUTS;
const mat4Observe = true;
let mat4Step = 0;
export function update(value) {
    const step = mat4Step++;
    const index = step - 90;
    if (index < 0 || index >= mat4Inputs.cases.length) return value;
    let record;
    try {
        record = mat4Case(mat4Inputs.cases[index], mat4Inputs.absolute_tolerance,
                          mat4Inputs.relative_tolerance);
    } catch (error) {
        if (mat4Observe) {
            console.log('Mat4Methods missing', index, String(error));
            console.log('Mat4Methods verified', index, false);
        }
        return value;
    }
    const finite = record.result.values.length > 0 && record.result.values.every(Number.isFinite);
    const origin = finite ? mat4Origin(record.result.values) : value;
    record.step = step;
    record.origin = [origin.x, origin.y, origin.z];
    if (mat4Observe) {
        console.log('Mat4Methods result', JSON.stringify(record));
        console.log('Mat4Methods verified', index, record.valid);
    }
    return origin;
}
""".replace("MATRIX_INPUTS", json.dumps(inputs, separators=(",", ":")))
    disc["origin_binding"]["script"] += helpers + script
    fixture["description"] += (
        " After the original states, 46 finite Mat4 operations drive the disc origin;"
        " full result components, value types and independent storage are observed."
    )
    fixture["log_expectations"]["INFO SceneScript log: Mat4Methods verified"] = [
        f"INFO SceneScript log: Mat4Methods verified {index} true"
        for index in range(len(inputs["cases"]))
    ]
    return fixture
