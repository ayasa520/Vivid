// Keep signed zero and non-finite components distinguishable in stored observations.
// Every expected result is an input literal; it is never obtained from the operation
// whose return value drives the material. Negative vector values are mapped into a
// visible range only after their full components and ownership have been checked.
const vec4MethodAxes = ['x', 'y', 'z', 'w'];
function vec4Decode(value) {
    if (value === '-0') return -0;
    if (value === 'NaN') return NaN;
    if (value === 'Infinity') return Infinity;
    if (value === '-Infinity') return -Infinity;
    return value;
}
function vec4Encode(value) {
    if (Object.is(value, -0)) return '-0';
    if (Number.isNaN(value)) return 'NaN';
    if (value === Infinity) return 'Infinity';
    if (value === -Infinity) return '-Infinity';
    return value;
}
function vec4Components(value) {
    return vec4MethodAxes.map(axis => vec4Encode(value[axis]));
}
function vec4Arguments(input, source) {
    return input.args.map(value => {
        if (typeof value === 'number') return value;
        if (value.self) return source;
        if (value.vector) return new Vec4(...value.vector.map(vec4Decode));
        return Object.fromEntries(vec4MethodAxes.map((axis, i) => [axis, vec4Decode(value.object[i])]));
    });
}
function vec4RunCase(input, useExpected) {
    const source = new Vec4(...input.source.map(vec4Decode));
    const args = vec4Arguments(input, source);
    const before = args.map(value => typeof value === 'number' ? value : vec4Components(value));
    const result = useExpected
        ? (Array.isArray(input.expected) ? new Vec4(...input.expected.map(vec4Decode)) : input.expected)
        : source[input.method](...args);
    const vector = Array.isArray(input.expected);
    const checks = [];
    const check = (name, value, expected) => checks.push([name, Object.is(value, expected)]);
    const values = vector ? vec4Components(result) : vec4Encode(result);
    if (vector) {
        check('instance', result instanceof Vec4, true);
        check('fresh', result !== source && args.every(value => result !== value), true);
        for (let i = 0; i < 4; i++) check('component-' + i, values[i], input.expected[i]);
        // Mutation of a returned vector must not change its receiver or operands. Restore
        // the checked result afterwards so the material receives the selected operation.
        const x = result.x;
        result.x = x + 0.125;
        for (let i = 0; i < 4; i++) check('source-after-result-write-' + i,
            vec4Encode(source[vec4MethodAxes[i]]), input.source[i]);
        result.x = x;
    } else {
        check('type', typeof result, typeof input.expected);
        check('value', values, input.expected);
    }
    for (let i = 0; i < 4; i++) check('source-' + i, vec4Encode(source[vec4MethodAxes[i]]), input.source[i]);
    for (let i = 0; i < args.length; i++) {
        if (typeof args[i] !== 'number') for (let j = 0; j < 4; j++)
            check('argument-' + i + '-' + j, vec4Encode(args[i][vec4MethodAxes[j]]), before[i][j]);
    }
    return {result, record: {id: input.id, method: input.method,
        kind: vector ? 'vector' : typeof result, values, source: vec4Components(source),
        args: args.map(value => typeof value === 'number' ? value : vec4Components(value)),
        checks, valid: checks.every(check => check[1])}};
}
function vec4Uniform(values) {
    return values.map(value => Math.fround(0.5 + vec4Decode(value) * 0.125));
}
function vec4Gain(value) {
    return typeof value === 'boolean' ? (value ? 0.875 : 0.125) : Math.fround(value);
}
function observeVec4Methods(materials, check, label, expected) {
    const index = phase - vec4Inputs.first_phase;
    const input = vec4Inputs.cases[index];
    const vector = Array.isArray(input.expected);
    if (vector) expected.quad = vec4Uniform(input.expected);
    else expected.gain = vec4Gain(input.expected);
    let record;
    try {
        const observed = vec4RunCase(input, vec4UseExpected);
        record = observed.record;
        for (const [name, valid] of record.checks) check('vec4-' + name, valid, true);
        if (vector) {
            const uniform = vec4Uniform(vec4Components(observed.result));
            materials[4].quad = new Vec4(...uniform);
            record.material = vec4Components(materials[4].quad);
            for (let i = 0; i < 4; i++) check('vec4-material-' + i, record.material[i], expected.quad[i]);
            record.valid = record.valid && record.material.every((value, i) => Object.is(value, expected.quad[i]));
        } else {
            const gain = vec4Gain(observed.result);
            materials[0].gain = materials[1].gain = gain;
            record.material = [materials[0].gain, materials[1].gain];
            for (let i = 0; i < 2; i++) check('vec4-material-' + i, record.material[i], expected.gain);
            record.valid = record.valid && record.material.every(value => Object.is(value, expected.gain));
        }
    } catch (error) {
        record = {id: input.id, method: input.method, valid: false, error: String(error)};
        check('vec4-operation', false, true);
        console.log('Vec4Methods missing', label, index, String(error));
    }
    record.label = label;
    record.index = index;
    localStorage.set('vec4-method-' + label + '-' + index, record);
    console.log('Vec4Methods result', JSON.stringify(record));
    console.log('Vec4Methods verified', label, index, record.valid);
}

// This last state starts with a value read from the real material, so constructing
// vectors in script cannot hide a missing prototype on the getter's result. The
// literal mode supplies only the independent drawing control for an earlier DSO.
const materialReadbackUseExpected = false;
function observeMaterialQuadReadback(material, check, label) {
    const seed = [0.25, 0.375, 0.5, 0.625];
    const expected = [0.125, 0.1875, 0.25, 0.3125];
    const record = {label, dimensions: 4};
    const checks = [];
    const verify = (name, value, wanted) => {
        checks.push([name, Object.is(value, wanted)]);
        check('material-readback-' + name, value, wanted);
    };
    try {
        const first = materialReadbackUseExpected ? new Vec4(...seed) : material.quad;
        const peer = materialReadbackUseExpected ? new Vec4(...seed) : material.quad;
        Object.assign(record, {
            values: vec4Components(first), instance: first instanceof Vec4,
            prototype: Object.getPrototypeOf(first) === Vec4.prototype,
            fresh: first !== peer, keys: Object.keys(first),
            descriptors: vec4MethodAxes.map(axis => {
                const item = Object.getOwnPropertyDescriptor(first, axis);
                return [item.writable, item.enumerable, item.configurable];
            })
        });
        for (const key of ['instance', 'prototype', 'fresh']) verify(key, record[key], true);
        verify('keys', record.keys.join(' '), 'x y z w');
        for (let i = 0; i < 4; i++) for (let flag = 0; flag < 3; flag++)
            verify('descriptor-' + i + '-' + flag, record.descriptors[i][flag], true);
        const copied = materialReadbackUseExpected ? new Vec4(...seed) : first.copy();
        const product = materialReadbackUseExpected ? new Vec4(...expected) : first.multiply(0.5);
        Object.assign(record, {
            copied: vec4Components(copied), product: vec4Components(product),
            resultsFresh: copied !== first && product !== first && copied !== product,
            resultInstances: copied instanceof Vec4 && product instanceof Vec4
        });
        verify('results-fresh', record.resultsFresh, true);
        verify('result-instances', record.resultInstances, true);
        first.x = 7;
        record.peerAfterMutation = vec4Components(peer);
        record.copyAfterMutation = vec4Components(copied);
        record.storageAfterMutation = vec4Components(material.quad);
        material.quad = product;
        record.material = vec4Components(material.quad);
        for (const field of ['values', 'copied', 'peerAfterMutation', 'copyAfterMutation',
                             'storageAfterMutation', 'product', 'material']) {
            const wanted = field === 'product' || field === 'material' ? expected : seed;
            for (let i = 0; i < 4; i++) verify(field + '-' + i, record[field][i], wanted[i]);
        }
    } catch (error) {
        record.error = String(error);
        verify('methods', false, true);
        console.log('MaterialReadback missing', label, record.error);
    }
    record.checks = checks;
    record.valid = checks.every(item => item[1]);
    localStorage.set('material-readback-' + label, record);
    console.log('MaterialReadback result', JSON.stringify(record));
    console.log('MaterialReadback verified', label, record.valid);
    return expected;
}
