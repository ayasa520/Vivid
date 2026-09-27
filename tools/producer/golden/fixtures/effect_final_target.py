"""Final effect cards with named targets, independent geometry and raster readers."""


def material(shader, textures=()):
    return {"passes": [{"shader": shader, "textures": list(textures), "blending": "normal",
                        "cullmode": "nocull", "depthtest": "disabled", "depthwrite": "disabled"}]}


def make_fixture():
    sizes = [[320, 180], [640, 180], [160, 300], [320, 180]]
    resize = """'use strict';
const sizes = [[320, 180], [640, 180], [160, 300], [320, 180]];
let step = 0;
export function update(value) {
    if (step % 10 === 0) thisLayer.size = new Vec2(...sizes[Math.min(3, Math.floor(step / 10))]);
    ++step;
    return value;
}
"""
    declarations = """attribute vec3 a_Position;
attribute vec2 a_TexCoord;
varying vec2 v_Position;
"""
    # Controls derive geometry from authored dimensions and placement from the fixed
    # canvas. They never read the candidate's vertices or transformation uniforms.
    literal_size = """uniform float g_Time;
vec2 selectedSize() {
    int phase = min(3, int(floor((g_Time * 30.0 - 0.5) / 10.0)));
    if (phase == 1) return vec2(640.0, 180.0);
    if (phase == 2) return vec2(160.0, 300.0);
    return vec2(320.0, 180.0);
}
"""
    fragment = """varying vec2 v_Position;
void main() { gl_FragColor = vec4(0.5 + v_Position / 1024.0, 0.75, 1.0); }
"""
    files = {
        "materials/source.json": material("genericimage2", ["final-target-source"]),
        "models/source.json": {"material": "materials/source.json", "width": 320, "height": 180,
                               "autosize": False, "fullscreen": False},
        "shaders/target-reader.vert": """attribute vec3 a_Position;
attribute vec2 a_TexCoord;
uniform mat4 g_ModelViewProjectionMatrix;
varying vec2 v_TexCoord;
void main() {
    v_TexCoord = a_TexCoord;
    gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);
}
""",
        "shaders/target-reader.frag": """uniform sampler2D g_Texture0;
varying vec2 v_TexCoord;
void main() {
    gl_FragColor = vec4(texSample2D(g_Texture0, vec2(v_TexCoord.x, 1.0 - v_TexCoord.y)).rgb, 1.0);
}
""",
    }
    owners, readers, expectations = [], [], []
    for index, (raster, literal) in enumerate([(False, False), (False, True),
                                              (True, False), (True, True)]):
        label = ("raster" if raster else "geometry") + ("-literal" if literal else "-query")
        owner_id, effect_id = 481 + index, 4810 + index
        target = "_rt_FinalCard_" + str(effect_id)
        vertex = declarations + (literal_size if literal else "")
        if raster and not literal:
            vertex += "uniform mat4 g_ModelViewProjectionMatrix;\n"
        position = ("vec2(a_TexCoord.x - 0.5, 0.5 - a_TexCoord.y) * selectedSize()"
                    if literal else "a_Position.xy")
        clip = "vec4(2.0 * a_TexCoord - 1.0, 0.5, 1.0)"
        if raster:
            clip = ("vec4((position + vec2(720.0, 360.0)) * vec2(2.0 / 1920.0, 2.0 / 1080.0) - 1.0, 0.5, 1.0)"
                    if literal else "mul(vec4(position, 0.0, 1.0), g_ModelViewProjectionMatrix)")
        vertex += ("void main() {\n    vec2 position = " + position + ";\n"
                   "    v_Position = position;\n    gl_Position = " + clip + ";\n}\n")
        files["shaders/" + label + ".vert"] = vertex
        files["shaders/" + label + ".frag"] = fragment
        files["materials/" + label + ".json"] = material(label)
        effect = "effects/" + label + ".json"
        # A named target retains uncovered pixels between draws. The raster pair follows
        # the same four size states, while the geometry pair overwrites its full target.
        files[effect] = {"name": label, "version": 1,
                         "fbos": [{"name": "_rt_FinalCard", "format": "rgba8", "unique": True,
                                   "width": 512, "height": 288, "clear": "0 0 0 1"}],
                         "passes": [{"material": "materials/" + label + ".json",
                                     "target": "_rt_FinalCard"}]}
        owners.append({"id": owner_id, "name": label, "image": "models/source.json",
                       "origin": "720 360 0", "size": "320 180", "angles": "0 0 0",
                       "scale": "1 1 1", "parallaxDepth": "0 0",
                       "visible": {"value": True, "script": resize},
                       "effects": [{"id": effect_id, "file": effect}]})
        reader_material = "materials/" + label + "-reader.json"
        reader_model = "models/" + label + "-reader.json"
        files[reader_material] = material("target-reader", [target])
        files[reader_model] = {"material": reader_material, "width": 840, "height": 480,
                               "autosize": False, "fullscreen": False}
        x, y = (1440 if literal else 480), (270 if raster else 810)
        readers.append({"id": 491 + index, "name": label + "-reader", "image": reader_model,
                        "origin": f"{x} {y} 0", "angles": "0 0 0", "scale": "1 1 1",
                        "visible": True, "parallaxDepth": "0 0"})
        expectations.append({"name": label, "owner": owner_id, "reader": 491 + index,
                             "target": target, "center": [x / 2, (1080 - y) / 2]})
    files["final-target-expectations.json"] = {"sizes": sizes, "panels": expectations}
    return {"description": "Final named-target material follows live image dimensions and owner placement; "
                           "independent geometry and raster controls are sampled by later scene readers.",
            "docs": ["scene/shader/overview.md"], "scenario": "fixture-final-target",
            "texture_assets": {"final-target-source": {"kind": "gradient", "size": [101, 67]}},
            "general": {"clearcolor": "0 0 0"}, "files": files, "objects": owners + readers,
            "layers": []}
