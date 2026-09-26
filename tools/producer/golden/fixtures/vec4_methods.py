"""Extend the existing scalar and quad material consumer with Vec4 value operations."""
import copy
import json
from pathlib import Path


HERE = Path(__file__).resolve().parent


def make_fixture(base):
    fixture = copy.deepcopy(base)
    inputs = json.loads((HERE / 'vec4_cases.json').read_text())
    helpers = (HERE / 'vec4_methods.js').read_text()
    script = fixture['layers'][0]['text']['script']
    declaration = 'const vec4Inputs = ' + json.dumps(inputs, separators=(',', ':')) + ';\n'
    declaration += 'const vec4UseExpected = false;\n'
    assert script.count('function inspect(layer, control, label) {') == 1
    script = script.replace('function inspect(layer, control, label) {',
        declaration + helpers + '\nfunction inspect(layer, control, label) {')
    marker = '    const markers = records.map((_,index) => marker(index,expected));'
    assert script.count(marker) == 1
    script = script.replace(marker,
        '    if (phase >= vec4Inputs.first_phase)\n'
        '        observeVec4Methods(materials,check,label,expected);\n' + marker)
    fixture['layers'][0]['text']['script'] = script
    fixture['properties']['phase']['max'] = inputs['first_phase'] + len(inputs['cases']) - 1
    fixture['description'] += (
        ' After the original 44 states, 46 Vec4 value operations drive existing scalar'
        ' and quad image/text materials; signed zero, ownership and independent colors are observed.'
    )
    fixture['log_expectations']['INFO SceneScript log: Vec4Methods verified'] = [
        f'INFO SceneScript log: Vec4Methods verified {label} {i} true'
        for i in range(len(inputs['cases'])) for label in ['image', 'text']
    ]
    return fixture
