// Decode only the explicitly selected fixture inputs. Matrix operations are always invoked
// on the scene runtime classes; expected components are used solely for verification.
function mat4Input(encoded, receiver) {
    switch (encoded.kind) {
        case 'Mat4': return new Mat4(encoded.values);
        case 'Vec2': return new Vec2(...encoded.values);
        case 'Vec3': return new Vec3(...encoded.values);
        case 'Vec4': return new Vec4(...encoded.values);
        case 'Number': return encoded.values[0];
        case 'Self': return receiver;
    }
    throw new Error('Unsupported matrix fixture input: ' + encoded.kind);
}

function mat4Describe(value) {
    if (value instanceof Mat4) return {kind: 'Mat4', values: value.m.slice()};
    if (value instanceof Mat3) return {kind: 'Mat3', values: value.m.slice()};
    if (value instanceof Vec4) return {kind: 'Vec4', values: [value.x, value.y, value.z, value.w]};
    if (value instanceof Vec3) return {kind: 'Vec3', values: [value.x, value.y, value.z]};
    if (value instanceof Vec2) return {kind: 'Vec2', values: [value.x, value.y]};
    if (typeof value === 'number') return {kind: 'Number', values: [value]};
    if (typeof value === 'boolean') return {kind: 'Boolean', values: [value ? 1 : 0]};
    if (typeof value === 'string') {
        return {kind: 'String', values: value.split(' ').map(Number), text: value};
    }
    if (value && value.translation instanceof Vec3 && value.rotation instanceof Vec3 &&
        value.scale instanceof Vec3 && Object.keys(value).sort().join(',') === 'rotation,scale,translation') {
        return {kind: 'Decomposition', values: [
            value.translation.x, value.translation.y, value.translation.z,
            value.rotation.x, value.rotation.y, value.rotation.z,
            value.scale.x, value.scale.y, value.scale.z]};
    }
    throw new Error('Unexpected matrix fixture result type: ' + typeof value);
}

// Perturb the second result after saving the first. A valid value operation gives both
// results independent storage and leaves every input intact, including a self operand.
function mat4Perturb(value) {
    if (value instanceof Mat4 || value instanceof Mat3) value.m[0] += 123;
    else if (value instanceof Vec2 || value instanceof Vec3 || value instanceof Vec4) value.x += 123;
    else if (value && value.translation instanceof Vec3) {
        value.translation.x += 123;
        value.rotation.y += 123;
        value.scale.z += 123;
    }
}

function mat4Case(row, absoluteTolerance, relativeTolerance) {
    const receiver = mat4Input(row.receiver);
    const args = row.arguments.map(value => mat4Input(value, receiver));
    const inputs = [receiver, ...args];
    const before = JSON.stringify(inputs.map(mat4Describe));
    const owner = row.static ? Mat4 : receiver;
    const result = owner[row.method](...args);
    const observed = mat4Describe(result);
    const saved = JSON.stringify(observed);
    const repeated = owner[row.method](...args);
    const repeatable = saved === JSON.stringify(mat4Describe(repeated));
    let fresh = true;
    if (typeof result === 'object') {
        fresh = result !== repeated && inputs.every(value => value !== result);
        if (result instanceof Mat4 || result instanceof Mat3) {
            fresh = fresh && result.m !== repeated.m && inputs.every(value => value.m !== result.m);
        } else if (observed.kind === 'Decomposition') {
            const vectors = [result.translation, result.rotation, result.scale];
            fresh = fresh && new Set(vectors).size === 3 && vectors.every(value =>
                inputs.every(input => input !== value) &&
                value !== repeated.translation && value !== repeated.rotation && value !== repeated.scale);
        }
    }
    const unchanged = before === JSON.stringify(inputs.map(mat4Describe));
    mat4Perturb(repeated);
    const independent = saved === JSON.stringify(mat4Describe(result)) &&
        before === JSON.stringify(inputs.map(mat4Describe));
    const expected = row.expected;
    const matches = observed.kind === expected.kind && observed.values.length === expected.values.length &&
        observed.values.every((value, index) => Number.isFinite(value) &&
            Math.abs(value - expected.values[index]) <= absoluteTolerance +
                relativeTolerance * Math.abs(expected.values[index])) &&
        (expected.kind !== 'String' || observed.text === expected.text);
    return {index: row.index, label: row.label, result: observed,
        repeatable, fresh, unchanged, independent, matches,
        valid: repeatable && fresh && unchanged && independent && matches};
}

// Every returned component contributes to the visible disc origin. Distinct weights keep
// translation, basis, scalar, string and decomposition results observable through one
// existing layer, while a separate resolved project supplies the control positions.
function mat4Origin(values) {
    let x = 0, y = 0;
    for (let index = 0; index < values.length; ++index) {
        x += values[index] * (index + 1) / values.length;
        y += values[index] * ((index * 3) % 7 - 3) / values.length;
    }
    return new Vec3(740 + 6 * x, 500 + 6 * y, 0);
}
