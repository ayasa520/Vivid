This directory is for third-party source dependencies used by Vivid's bundled renderer modules.

<!-- Documentation note: this file intentionally documents only source trees that Vivid builds
directly. Runtime-installed shared libraries and typelibs are described in the root README. -->

Current layout:
- `wallpaper-scene-renderer/`: Wallpaper Engine scene renderer source used by `producer/src/renderers/scene`.

Runtime builds should only ship Vivid's compiled artifacts, not these source trees.
