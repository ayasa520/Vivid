// The existing literal codec preserves signed zero and non-finite inputs across all
// vector widths. Expected values are supplied independently in the case data; only
// transcendental cases allow their explicit floating-point tolerance.
function vec23Components(value, dimensions) {
    return ['x', 'y', 'z'].slice(0, dimensions).map(axis => vec4Encode(value[axis]));
}
function vec23RunCase(input, useExpected) {
    const Vector = input.dimensions === 2 ? Vec2 : Vec3;
    const axes = ['x', 'y', 'z'].slice(0, input.dimensions);
    const source = new Vector(...input.source.map(vec4Decode));
    const args = input.args.map(value => typeof value === 'number' ? value
        : value.self ? source : new Vector(...value.vector.map(vec4Decode)));
    const before = args.map(value => typeof value === 'number' ? value
        : vec23Components(value, input.dimensions));
    const vector = Array.isArray(input.expected);
    const result = useExpected
        ? (vector ? new Vector(...input.expected.map(vec4Decode)) : input.expected)
        : (input.static ? Vector[input.method](...args) : source[input.method](...args));
    const checks = [];
    const check = (name, valid) => checks.push([name, valid]);
    const equal = (value, expected) => {
        const encoded = vec4Encode(value);
        return Object.is(encoded, expected) ||
            (input.tolerance > 0 && typeof encoded === 'number' &&
             typeof expected === 'number' && Number.isFinite(encoded) &&
             Math.abs(encoded - expected) <= input.tolerance);
    };
    const values = vector ? vec23Components(result, input.dimensions) : vec4Encode(result);
    if (vector) {
        check('instance', result instanceof Vector);
        check('fresh', result !== source && args.every(value => result !== value));
        for (let i = 0; i < axes.length; i++)
            check('component-' + i, equal(result[axes[i]], input.expected[i]));
        // Observe ownership independently of numeric equality. A write to the result
        // must not leak into its source or arguments before the material consumes it.
        const x = result.x;
        result.x = x + 0.125;
        for (let i = 0; i < axes.length; i++)
            check('source-after-result-write-' + i,
                Object.is(vec4Encode(source[axes[i]]), input.source[i]));
        for (let i = 0; i < args.length; i++) if (typeof args[i] !== 'number')
            for (let j = 0; j < axes.length; j++)
                check('argument-after-result-write-' + i + '-' + j,
                    Object.is(vec4Encode(args[i][axes[j]]), before[i][j]));
        result.x = x;
    } else {
        check('type', typeof result === typeof input.expected);
        check('value', equal(result, input.expected));
    }
    for (let i = 0; i < axes.length; i++)
        check('source-' + i, Object.is(vec4Encode(source[axes[i]]), input.source[i]));
    for (let i = 0; i < args.length; i++) if (typeof args[i] !== 'number')
        for (let j = 0; j < axes.length; j++)
            check('argument-' + i + '-' + j,
                Object.is(vec4Encode(args[i][axes[j]]), before[i][j]));
    return {result, record: {id: input.id, dimensions: input.dimensions,
        method: input.method, static: Boolean(input.static), values,
        source: vec23Components(source, input.dimensions), args: before,
        checks, valid: checks.every(item => item[1])}};
}
function vec23Uniform(values, input) {
    const scale = input.uniform_scale ?? 0.125;
    return values.map(value => Math.fround(0.5 + vec4Decode(value) * scale));
}
function observeVec23Methods(materials, check, label, expected) {
    const index = phase - vec23Inputs.first_phase;
    const input = vec23Inputs.cases[index];
    const vector = Array.isArray(input.expected);
    const field = input.dimensions === 2 ? 'pair' : 'triple';
    const material = materials[input.dimensions === 2 ? 2 : 3];
    if (vector) expected[field] = vec23Uniform(input.expected, input);
    else expected.gain = vec4Gain(input.expected);
    let record;
    try {
        const observed = vec23RunCase(input, vec23UseExpected);
        record = observed.record;
        for (const [name, valid] of record.checks) check('vec23-' + name, valid, true);
        if (vector) {
            const Vector = input.dimensions === 2 ? Vec2 : Vec3;
            const uniform = vec23Uniform(vec23Components(observed.result, input.dimensions), input);
            material[field] = new Vector(...uniform);
            record.material = vec23Components(material[field], input.dimensions);
            for (let i = 0; i < input.dimensions; i++)
                check('vec23-material-' + i, record.material[i], expected[field][i]);
            record.valid = record.valid && record.material.every((value, i) =>
                Object.is(value, expected[field][i]));
        } else {
            const gain = vec4Gain(observed.result);
            materials[0].gain = materials[1].gain = gain;
            record.material = [materials[0].gain, materials[1].gain];
            for (let i = 0; i < 2; i++) check('vec23-material-' + i, record.material[i], expected.gain);
            record.valid = record.valid && record.material.every(value => Object.is(value, expected.gain));
        }
    } catch (error) {
        record = {id: input.id, dimensions: input.dimensions, method: input.method,
            valid: false, error: String(error)};
        check('vec23-operation', false, true);
        console.log('Vec23Methods missing', label, index, String(error));
    }
    record.label = label;
    record.index = index;
    localStorage.set('vec23-method-' + label + '-' + index, record);
    console.log('Vec23Methods result', JSON.stringify(record));
    console.log('Vec23Methods verified', label, index, record.valid);
}
