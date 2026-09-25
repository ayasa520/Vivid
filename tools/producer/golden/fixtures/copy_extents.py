"""Whole-target copy admission with independently seeded destination readers."""


def material(shader, color=None):
    entry = {"shader": shader, "blending": "normal", "cullmode": "nocull",
             "depthtest": "disabled", "depthwrite": "disabled", "textures": []}
    if color is not None:
        entry["constantshadervalues"] = {"color": color, "opacity": 1, "write": 1}
    return {"passes": [entry]}


def target(name, size):
    return {"name": name, "format": "rgba8888", "unique": True,
            "width": size[0], "height": size[1], "clear": "0 0 0 1"}


def make_fixture():
    source_color = [0.875, 0.125, 0.25]
    destination_color = [0.125, 0.375, 0.875]
    # The expected color is explicit fixture data. Each candidate has an independent
    # authored writer as its control, so rejecting every copy cannot satisfy the equal
    # case. A second, equal-size copy publishes the result after the selected command;
    # its shader reader also proves that a rejected transfer does not stop the stream.
    cases = [
        ("larger-source", [128, 96], [64, 64], False, destination_color),
        ("smaller-source", [32, 32], [64, 64], False, destination_color),
        ("implicit-source", [64, 64], [64, 64], True, destination_color),
        ("equal-source", [64, 64], [64, 64], False, source_color),
    ]
    files = {
        "materials/copy-source.json": material("golden/effect-clear-write", source_color),
        "materials/copy-destination.json": material("golden/effect-clear-write", destination_color),
        "materials/copy-read.json": material("genericimage2"),
        "models/copy-card.json": {"material": "materials/copy-source.json", "autosize": False,
                                  "fullscreen": False, "width": 256, "height": 160},
    }
    objects = []
    expectations = []
    for index, (name, source_size, destination_size, implicit, expected) in enumerate(cases):
        for control in (False, True):
            label = name + ("-control" if control else "-candidate")
            effect_path = "effects/" + label + "/effect.json"
            if control:
                writer = "materials/" + label + ".json"
                files[writer] = material("golden/effect-clear-write", expected)
                passes = [{"material": writer, "target": "C"}]
            else:
                command = {"command": "copy", "target": "B"}
                if not implicit:
                    command["source"] = "A"
                passes = [
                    {"material": "materials/copy-source.json", "target": "A"},
                    {"material": "materials/copy-destination.json", "target": "B"},
                    command,
                    {"command": "copy", "source": "B", "target": "C"},
                ]
            passes.append({"material": "materials/copy-read.json",
                           "bind": [{"index": 0, "name": "C"}]})
            files[effect_path] = {"name": label, "version": 1, "passes": passes,
                                 "fbos": [target("A", source_size), target("B", destination_size),
                                          target("C", destination_size)]}
            x = 380 + 960 * (index % 2) + (320 if control else 0)
            y = 810 - 540 * (index // 2)
            objects.append({"id": 100 + index * 2 + int(control), "name": label,
                            "image": "models/copy-card.json", "origin": f"{x} {y} 0",
                            "angles": "0 0 0", "scale": "1 1 1", "size": "256 160",
                            "visible": True, "effects": [{"id": 1000 + index * 2 + int(control),
                                                            "file": effect_path}]})
            expectations.append({"name": label, "center": [x / 2, (1080 - y) / 2],
                                 "rgb": expected, "source": source_size,
                                 "destination": destination_size, "implicit": implicit})
    files["copy-expectations.json"] = expectations
    return {"description": "Reject unequal whole-target copies, retain seeded destination pixels, "
                           "and execute subsequent equal copies and shader readers.",
            "docs": ["scene/shader/overview.md"],
            "asset_files": ["shaders/golden/effect-clear-write.vert",
                            "shaders/golden/effect-clear-write.frag"],
            "files": files, "objects": objects, "layers": []}
