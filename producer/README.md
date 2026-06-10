# Vivid Wallpaper

This directory contains the out-of-process wallpaper producer and its WebUI.
The temporary application id is `io.github.ayasa520.Vivid`, and the
installed Flatpak command is `vivid`.

Kept producer routes:

- `daemon`: producer process under `src/daemon`
- `protocol`: display protocol codec under `src/protocol`
- `graphics`: GPU device discovery under `src/graphics`
- `renderer_api`: C/C++ ABI shared by the daemon and renderer modules
- `renderers/scene`: Wallpaper Engine scene renderer module
- `renderers/video`: producer-owned GStreamer renderer module
- `renderers/web`: CEF renderer module
- `webui`: controller UI under `src/webui`
- `third_party/wallpaper-scene-renderer`: scene renderer source submodule

The old GNOME-extension-side renderer tree is not part of this directory.

## Direct Run

```sh
tools/vivid.sh direct-run build
tools/vivid.sh direct-run run
```

Direct-run artifacts are kept in `.build/direct-run`. Re-running the build keeps
the existing CMake build directories and CEF extraction, so normal incremental
rebuilds do not start from scratch.

## Flatpak

```sh
tools/vivid.sh flatpak prefetch
VIVID_FLATPAK_JOBS="$(nproc)" tools/vivid.sh flatpak build
```

Flatpak artifacts stay under `.build`:

- app dir: `.build/flatpak/vivid-producer`
- local repository: `.build/flatpak-repo/vivid-producer`
- builder state and ccache: `.build/flatpak-builder-state`
- download-only work dir: `.build/flatpak-download`

`packaging/flatpak/io.github.ayasa520.Vivid.yml` is the Flatpak manifest passed
directly to flatpak-builder. Third-party URL sources are listed in the manifest
with sha256 checksums, and `tools/vivid.sh flatpak prefetch` delegates to
`flatpak-builder --download-only` instead of maintaining a separate download
script. The CEF binary bundle is a large local archive source at
`packaging/flatpak/sources/cef_binary_current_minimal.tar.bz2`; the manifest
verifies and extracts it into the module source tree during the build.

The local checkout is included through explicit `type: dir` sources for the
producer tree and helper scripts. Keep `.build/flatpak-builder-state` if you
want subsequent Flatpak builds to reuse downloaded sources and ccache.

To run the built app dir without installing it:

```sh
tools/vivid.sh flatpak run-appdir
```

To export a single-file Flatpak bundle from the local repository:

```sh
mkdir -p .build/flatpak-bundles
flatpak build-bundle \
  .build/flatpak-repo/vivid-producer \
  .build/flatpak-bundles/vivid-producer.flatpak \
  io.github.ayasa520.Vivid
```

The debug ref is also exported to the local repository. If a debug bundle is
needed, export it explicitly:

```sh
flatpak build-bundle \
  --runtime \
  .build/flatpak-repo/vivid-producer \
  .build/flatpak-bundles/vivid-debug.flatpak \
  io.github.ayasa520.Vivid.Debug
```

## Directory Overrides

All producer scripts keep their default artifacts under `.build`, but the base
directory can be moved without editing scripts:

```sh
VIVID_BUILD_ROOT=/path/to/build tools/vivid.sh direct-run build
VIVID_BUILD_ROOT=/path/to/build tools/vivid.sh flatpak build
```

More specific overrides are available for direct-run and Flatpak paths, such as
`VIVID_DIRECT_RUN_BUILD_DIR`, `VIVID_FLATPAK_BUILD_DIR`,
`VIVID_FLATPAK_REPO_DIR`, `VIVID_FLATPAK_STATE_DIR`,
`VIVID_FLATPAK_DOWNLOAD_DIR`, and `VIVID_FLATPAK_RUN_DIR`.
