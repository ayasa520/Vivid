"""Dynamic scene zoom observed through the existing camera projection material."""

from camera_path_pose import make_projection_fixture


def make_fixture():
    values = [0.5, 0.75, 1.25, 2.5, 0.5]
    products = [1, 1.5, 2.5, 5, 1]
    pose = {"timestamp": 0, "eye": [0, 0, 0], "center": [0, 0, -1],
            "up": [0, 1, 0], "zoom": 2}
    points = [[0, 0, 0], [100, 0, 0], [0, 100, 0], [0, 0, 100]]
    # Keep expected general/selected products explicit. The material projects fixed world
    # points, so a successful script callback or property readback alone cannot pass it.
    samples = [{"step": i*15, "general_zoom": value, "selected_zoom": 2, "product": product,
                "points": points,
                "clip": [[(x/480-1)*product, (y/300-1)*product, z/10000+0.5, 1]
                         for x, y, z in points], "frame_eye": [480, 300, 2000]}
               for i, (value, product) in enumerate(zip(values, products))]
    fixture = make_projection_fixture(samples, [pose])
    fixture["description"] = "Scene-owned zoom scripts update raw storage and the selected orthographic projection."
    fixture["general"]["zoom"] = {"value": 0.5, "script": """let tick = 0;
const values = [0.5, 0.75, 1.25, 2.5, 0.5];
export function init(value) {
    console.log('GENERAL_ZOOM_INIT', thisScene.zoom, value);
    return value;
}
export function update(value) {
    const phase = Math.min(4, Math.floor(tick / 15));
    const next = values[phase];
    if (tick % 15 === 0) console.log('GENERAL_ZOOM_PHASE', phase, 'previous', thisScene.zoom, 'next', next);
    tick++;
    return next;
}
"""}
    prefix = "INFO SceneScript log: GENERAL_ZOOM"
    fixture["log_expectations"] = {prefix: [
        prefix + "_INIT 0.5 0.5",
        prefix + "_PHASE 0 previous 0.5 next 0.5",
        prefix + "_PHASE 1 previous 0.5 next 0.75",
        prefix + "_PHASE 2 previous 0.75 next 1.25",
        prefix + "_PHASE 3 previous 1.25 next 2.5",
        prefix + "_PHASE 4 previous 2.5 next 0.5",
    ]}
    return fixture
