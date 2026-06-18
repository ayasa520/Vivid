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
VIVID_FLATPAK_JOBS="$(nproc)" tools/vivid.sh build flatpak
```

`tools/vivid.sh flatpak build` is an equivalent spelling for the build command.
After prefetching, a no-download build can be run with:

```sh
VIVID_FLATPAK_DISABLE_DOWNLOAD=1 tools/vivid.sh build flatpak
```

Flatpak artifacts stay under `.build`:

- app dir: `.build/flatpak/vivid-producer`
- local repository: `.build/flatpak-repo/vivid-producer`
- builder state and ccache: `.build/flatpak-builder-state`
- stable native CMake cache: `.build/flatpak-native-cache/native-build`
- download-only work dir: `.build/flatpak-download`
- generated manifest and source lock: `.build/flatpak-manifest`

`packaging/flatpak/io.github.ayasa520.Vivid.yml` is a template manifest. The
wrapper scripts render the concrete manifest into `.build/flatpak-manifest` with
the Vivid GitHub source pinned to an exact commit. By default the source is
`https://github.com/ayasa520/Vivid.git` on `main`; set
`VIVID_FLATPAK_GIT_URL`, `VIVID_FLATPAK_GIT_BRANCH`, or
`VIVID_FLATPAK_GIT_COMMIT` to override it. Flatpak builds committed GitHub
sources, so uncommitted local changes are not included.

The software version shown by Flatpak/AppStream is controlled by the release
entry in the installed metainfo file. The build defaults to `1.0.0` and can be
overridden with:

```sh
VIVID_FLATPAK_APP_VERSION=1.0.0 \
VIVID_FLATPAK_RELEASE_DATE=2026-06-18 \
  tools/vivid.sh build flatpak
```

Third-party URL sources are listed in the manifest with sha256 checksums, and
`tools/vivid.sh flatpak prefetch` delegates to `flatpak-builder --download-only`
instead of maintaining a separate download script. The CEF binary bundle is a
large local archive source at
`packaging/flatpak/sources/cef_binary_current_minimal.tar.bz2`; the manifest
verifies and extracts it into the module source tree during the build.

Keep `.build/flatpak-builder-state` to reuse downloaded Flatpak sources and
ccache. Keep `.build/flatpak-native-cache/native-build` to reuse the renderer
CMake build directories across Flatpak module rebuilds.

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
