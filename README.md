# Vivid

<p align="center">
  <img src="producer/resources/io.github.ayasa520.Vivid.svg" alt="Vivid thumbnail" width="160">
</p>

Vivid is an open-source reimplementation of Wallpaper Engine for Linux.

**THIS PROJECT USES VIBE CODING.**

## Build

Build artifacts are written under `producer/.build`.

### Flatpak

```sh
tools/vivid.sh flatpak prefetch
tools/vivid.sh build flatpak
tools/vivid.sh flatpak run-appdir
```

`tools/vivid.sh flatpak prefetch` downloads and pins the Flatpak sources first.
After that, an offline/cached build can be run with:

```sh
VIVID_FLATPAK_DISABLE_DOWNLOAD=1 tools/vivid.sh build flatpak
```

The Flatpak manifest is rendered from
`producer/packaging/flatpak/io.github.ayasa520.Vivid.yml` into
`producer/.build/flatpak-manifest`. The bundle is written to
`producer/.build/io.github.ayasa520.Vivid-1.0.0.flatpak` by default.

Set the Flatpak software version with:

```sh
VIVID_FLATPAK_APP_VERSION=1.0.0 \
VIVID_FLATPAK_RELEASE_DATE=2026-06-18 \
  tools/vivid.sh build flatpak
```

Useful cache locations:

- `producer/.build/flatpak-builder-state`
- `producer/.build/flatpak-builder-state/ccache`
- `producer/.build/flatpak-native-cache/native-build`
- `producer/.build/flatpak-repo/vivid-producer`

### Direct Run

```sh
tools/vivid.sh build direct-run
tools/vivid.sh direct-run run
```

Direct-run artifacts stay in `producer/.build/direct-run`.

### Clean

```sh
tools/vivid.sh clean flatpak
tools/vivid.sh clean direct-run
```

Credits:

1. [waywallen](https://github.com/waywallen)
