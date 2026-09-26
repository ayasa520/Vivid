"""Extend the existing material consumer with two- and three-component value calls."""
import copy
import json
from pathlib import Path


HERE = Path(__file__).resolve().parent


def make_fixture(base):
    fixture = copy.deepcopy(base)
    inputs = json.loads((HERE / 'vec2_vec3_cases.json').read_text())
    helpers = (HERE / 'vec2_vec3_methods.js').read_text()
    script = fixture['layers'][0]['text']['script']
    declaration = 'const vec23Inputs = ' + json.dumps(inputs, separators=(',', ':')) + ';\n'
    declaration += 'const vec23UseExpected = false;\n'
    assert script.count('function inspect(layer, control, label) {') == 1
    script = script.replace('function inspect(layer, control, label) {',
        declaration + helpers + '\nfunction inspect(layer, control, label) {')
    marker = '    const markers = records.map((_,index) => marker(index,expected));'
    assert script.count(marker) == 1
    script = script.replace(marker,
        '    if (phase >= vec23Inputs.first_phase &&\n'
        '        phase < vec23Inputs.first_phase + vec23Inputs.cases.length)\n'
        '        observeVec23Methods(materials,check,label,expected);\n' + marker)
    # The existing triple shader has a fixed-value control because earlier phases never
    # change its value. Extend that same control to independent case tuples, retaining
    # exactly the previous colors when an earlier phase has no triple expectation.
    triple = '    if (index === 3) return new Vec3(0.28125, 0.359375, 0.4375);'
    assert script.count(triple) == 1
    script = script.replace(triple,
        '    if (index === 3) {\n'
        '        const triple = expected.triple ?? [0.25,0.375,0.5];\n'
        '        return new Vec3(0.125 + triple[0] * 0.625,\n'
        '                        0.125 + triple[1] * 0.625,\n'
        '                        0.125 + triple[2] * 0.625);\n'
        '    }')
    fixture['layers'][0]['text']['script'] = script
    fixture['properties']['phase']['max'] = inputs['first_phase'] + len(inputs['cases']) - 1
    fixture['description'] += (
        f' The next {len(inputs["cases"])} states drive existing scalar, pair and triple image/text materials'
        ' with Vec2/Vec3 value methods and the spherical constructor, retaining independent'
        ' expected values, degree units and input/result ownership observations.'
    )
    fixture['log_expectations']['INFO SceneScript log: Vec23Methods verified'] = [
        f'INFO SceneScript log: Vec23Methods verified {label} {i} true'
        for i in range(len(inputs['cases'])) for label in ['image', 'text']
    ]
    return fixture
