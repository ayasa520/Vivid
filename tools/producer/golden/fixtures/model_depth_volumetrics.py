"""Observe multisampled scene depth through the ordinary volumetric light consumer."""

import copy


def make_fixture(reflection_fixture):
    # Reuse the existing 3D fixture's camera, model and shader inputs. The hidden model
    # selects scene MSAA; the wall writes the same destination-owned depth attachment.
    # It covers the view at Z=700, ahead of the light volume's frontmost point at Z=600.
    # A visible wall therefore blocks all light, while a hidden wall exposes the stage's
    # zero far-depth clear. The explicit near clip keeps these depths distinguishable
    # in the normalized eight-bit depth texture used by volumetric lighting.
    depth_material = {"passes": [{
        "shader": "golden/pass-input", "textures": ["util/white", "util/white"],
        "blending": "normal", "cullmode": "nocull", "depthtest": "enabled",
        "depthwrite": "enabled", "constantshadervalues": {"gain": 0, "opacity": 1},
    }]}
    return {
        "description": "A foreground depth wall occludes a point-light volume; hiding it exposes the freshly cleared far depth.",
        "docs": ["scene/shader/overview.md"],
        "scenario": "model-depth-volumetrics",
        "asset_files": copy.deepcopy(reflection_fixture["asset_files"]),
        "camera": copy.deepcopy(reflection_fixture["camera"]),
        "general": {**reflection_fixture["general"], "nearz": 100, "clearcolor": "0 0 0"},
        "properties": {"wall": {"type": "bool", "text": "Show depth wall", "value": True}},
        "files": {
            "materials/depth-wall.json": copy.deepcopy(depth_material),
            "models/depth-wall.json": {
                "material": "materials/depth-wall.json", "width": 2000, "height": 2000,
            },
            "materials/models/sphere/sphere.json": copy.deepcopy(depth_material),
        },
        "layers": [{
            "name": "depth-wall", "model": "models/depth-wall.json", "size": [2000, 2000],
            "origin": [0, 0, 700], "visible_binding": {"value": True, "user": "wall"},
        }],
        "objects": [
            {**copy.deepcopy(reflection_fixture["objects"][0]), "name": "scene-model"},
            {"id": 12, "name": "depth-limited-light", "light": "lpoint", "origin": "0 0 0",
             "angles": "0 0 0", "radius": 600, "color": "0.5 0.7 1.0", "intensity": 25,
             "density": 0.75, "exponent": 4, "volumetricsexponent": 2.5,
             "castshadow": False, "castvolumetrics": True},
        ],
    }
