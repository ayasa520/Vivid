"""Selected camera-path samples consumed by a material's view projection and frame eye."""

import math


def make_fixture():
    first = {"timestamp": 0, "eye": [96, -54, 20], "center": [96, -54, -180],
             "up": [0, 1, 0], "zoom": 1}
    last = {"timestamp": 2, "eye": [192, -114, 80], "center": [252, -84, -80],
            "up": [0.5, 1, 0.25], "zoom": 1}
    # These five authored weights are independent sample data, not a copy of the runtime
    # interpolator. Quarter samples distinguish the curve from a linear path; the endpoints
    # and midpoint ensure the diagnostic also rejects a constant or shifted sample cursor.
    weights = [0, 0.203125, 0.5, 0.796875, 1]
    points = [[0, 0, 0], [100, 0, 0], [0, 100, 0], [0, 0, 100]]

    def unit(v):
        length = math.sqrt(sum(x * x for x in v))
        return [x / length for x in v]

    def cross(a, b):
        return [a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]]

    samples = []
    for index, weight in enumerate(weights):
        pose = {key: [a + (b-a)*weight for a, b in zip(first[key], last[key])]
                for key in ("eye", "center", "up")}
        eye = pose["eye"]
        z = unit([a-b for a, b in zip(eye, pose["center"])])
        x = unit(cross(pose["up"], z))
        y = unit(cross(z, x))
        # Independent world-point projection: first express the point in the selected
        # look-at basis, then apply the canvas anchor and symmetric reversed-depth volume.
        # No model transform participates in g_ViewProjectionMatrix.
        clips = []
        for point in points:
            relative = [a-b for a, b in zip(point, eye)]
            view = [sum(a*b for a, b in zip(axis, relative)) for axis in (x, y, z)]
            clips.append([view[0]/480-1, view[1]/300-1, view[2]/10000+0.5, 1])
        frame_eye = [480+eye[0], 300+eye[1], 2000]
        samples.append({"step": index*15, "weight": weight, **pose,
                        "points": points, "clip": clips, "frame_eye": frame_eye})
    return make_projection_fixture(samples, [first, last])


def make_projection_fixture(samples, transforms):
    """Reuse the actual projection/eye material consumer with independently authored inputs."""
    def literal(v):
        return "vec" + str(len(v)) + "(" + ",".join(f"{x:.10f}" for x in v) + ")"

    branches = []
    for index, sample in enumerate(samples):
        clips, frame_eye = sample["clip"], sample["frame_eye"]
        body = "\n".join(f"        expected{i} = {literal(p)};" for i, p in enumerate(clips))
        body += "\n        expectedEye = " + literal(frame_eye) + ";"
        branches.append(("    if" if index == 0 else "    else if") +
                        f" (sample == {index}) {{\n{body}\n    }}")

    vertex = """attribute vec3 a_Position;
attribute vec2 a_TexCoord;
uniform mat4 g_ViewProjectionMatrix;
uniform vec3 g_EyePosition;
uniform float g_Time;
varying vec2 v_Uv;
varying vec4 v_ProjectionErrors;
varying float v_EyeError;
void main() {
    // Raster placement is fixed so a bad camera cannot hide its own diagnostic. The
    // colored bands consume four world points transformed by the actual uploaded view.
    gl_Position = vec4(a_TexCoord * 2.0 - 1.0, 0.5, 1.0);
    v_Uv = a_TexCoord;
    int sample = int(floor((g_Time - 1.0/30.0) * 2.0 + 0.5));
    vec4 expected0 = vec4(0,0,0,0), expected1 = vec4(0,0,0,0);
    vec4 expected2 = vec4(0,0,0,0), expected3 = vec4(0,0,0,0);
    vec3 expectedEye = vec3(0,0,0);
""" + "\n".join(branches) + """
    v_ProjectionErrors = vec4(
        length(mul(vec4(0,0,0,1),g_ViewProjectionMatrix)-expected0),
        length(mul(vec4(100,0,0,1),g_ViewProjectionMatrix)-expected1),
        length(mul(vec4(0,100,0,1),g_ViewProjectionMatrix)-expected2),
        length(mul(vec4(0,0,100,1),g_ViewProjectionMatrix)-expected3));
    v_EyeError = length(g_EyePosition-expectedEye);
}
"""
    fragment = """varying vec2 v_Uv;
varying vec4 v_ProjectionErrors;
varying float v_EyeError;
void main() {
    float error = v_Uv.y < 0.2 ? v_ProjectionErrors.x :
        v_Uv.y < 0.4 ? v_ProjectionErrors.y :
        v_Uv.y < 0.6 ? v_ProjectionErrors.z :
        v_Uv.y < 0.8 ? v_ProjectionErrors.w : v_EyeError;
    float tolerance = v_Uv.y < 0.8 ? 0.000002 : 0.001;
    bool matches = v_Uv.x > 0.5 || error < tolerance;
    gl_FragColor = matches ? vec4(0.125,0.75,0.25,1) : vec4(0.875,0.125,0.75,1);
}
"""
    def material(shader):
        return {"passes": [{"shader": shader, "textures": ["util/white"],
                            "blending": "normal", "cullmode": "nocull",
                            "depthtest": "disabled", "depthwrite": "disabled"}]}

    return {
        "description": "Five camera-path pose samples reach view-projected world points and frame-eye bands.",
        "scenario": "fixture-camera-path-pose", "docs": ["scene/models/camera.md"],
        "general": {"orthogonalprojection": {"width": 960, "height": 600},
                    "camerafade": False, "farz": 4000},
        "camera": {"eye": "0 0 0", "center": "0 0 -1", "up": "0 1 0",
                   "paths": ["scripts/pose.json"]},
        "files": {
            "scripts/pose.json": {"paths": [{"name": "varying-pose", "duration": 1000,
                                               "transforms": transforms}]},
            "pose-expectations.json": samples,
            "shaders/pose.vert": vertex, "shaders/pose.frag": fragment,
            "materials/white.json": material("genericimage2"),
            "materials/pose.json": material("pose"),
            "models/white.json": {"material": "materials/white.json", "width": 960, "height": 600},
            "effects/pose/effect.json": {"name": "pose", "version": 1,
                                         "passes": [{"material": "materials/pose.json"}]},
        },
        "objects": [{"id": 77001, "name": "path pose consumer", "image": "models/white.json",
                     "origin": "480 300 0", "scale": "1 1 1", "angles": "0 0 0",
                     "visible": True, "reflected": False,
                     "effects": [{"file": "effects/pose/effect.json", "id": 77030}]}],
        "layers": [],
    }
