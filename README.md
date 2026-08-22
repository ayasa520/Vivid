# Vivid

<p align="center">
  <img src="producer/resources/io.github.ayasa520.Vivid.svg" alt="Vivid thumbnail" width="160">
</p>

Vivid is an open-source reimplementation of Wallpaper Engine for Linux.

**THIS PROJECT USES VIBE CODING.**

## Install a release

Download files from the [GitHub Releases](https://github.com/ayasa520/Vivid/releases)
page. Use the assets attached to a release, not the artifact ZIPs from the
Actions page. Release assets are already unpacked into installable files.

Choose `x86_64` for 64-bit Intel/AMD systems or `aarch64` for 64-bit Arm
systems. `uname -m` prints the architecture of the current system. Each Vivid
installation needs the Flatpak producer and one consumer matching the desktop:

| Desktop | Release asset |
| --- | --- |
| Producer and controller | `io.github.ayasa520.Vivid-<version>-<arch>.flatpak` |
| GNOME Shell | `vivid-consumer-gnome-<version>-<arch>.zip` |
| KDE Plasma | `vivid-consumer-kde-<version>-<arch>.zip` |
| Hyprland, Sway, niri, and other layer-shell compositors | `vivid-layer-shell-consumer-<version>-<arch>` |

The commands below use the `1.0.6` x86_64 assets as examples. Run them from the
directory containing the downloaded files.

### Producer

Install the Flatpak bundle for the current user, then start Vivid from the
application launcher or the command line:

```sh
flatpak install --user ./io.github.ayasa520.Vivid-1.0.6-x86_64.flatpak
flatpak run io.github.ayasa520.Vivid
```

### GNOME Shell consumer

The release ZIP is ready for `gnome-extensions` and supports GNOME Shell 45
through 50:

```sh
gnome-extensions install --force ./vivid-consumer-gnome-1.0.6-x86_64.zip
gnome-extensions enable vivid-consumer-gnome@rikka.local
```

Log out and back in if GNOME Shell has not loaded the newly installed
extension.

### KDE Plasma consumer

Extract the package, install it with KPackage, then select **Vivid** in
**Desktop and Wallpaper** settings:

```sh
vivid_kde_stage="$(mktemp -d)"
unzip ./vivid-consumer-kde-1.0.6-x86_64.zip -d "${vivid_kde_stage}"
kpackagetool6 --type Plasma/Wallpaper \
  --install "${vivid_kde_stage}/dev.rikka.vivid.consumer.kde"
```

Use `--upgrade` instead of `--install` when updating an existing KDE package.

### Layer-shell consumer

Install the executable in the user-local binary directory:

```sh
install -Dm755 ./vivid-layer-shell-consumer-1.0.6-x86_64 \
  "${HOME}/.local/bin/vivid-layer-shell-consumer"
```

Start the producer first, then run `vivid-layer-shell-consumer`. See the
[layer-shell consumer guide](consumer/layer-shell/README.md) for compositor
autostart configuration.

## Build

Producer build artifacts are written under `producer/.build`.

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
`producer/.build/io.github.ayasa520.Vivid-1.0.0-<arch>.flatpak` by default.

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

### Desktop consumers

GNOME Shell and KDE Plasma have dedicated plugins:

```sh
tools/vivid.sh gnome build
tools/vivid.sh kde build
```

Other Wayland compositors that speak `zwlr_layer_shell_v1` (Sway, Hyprland, labwc, river, wayfire, niri, …) use the catch-all client:

```sh
tools/vivid.sh layer-shell build
tools/vivid.sh layer-shell run
```

See `consumer/layer-shell/README.md` and `consumer/layer-shell/examples/` for Hyprland Lua autostart / `hl.layer_rule`, Sway `exec`, and niri autostart. GNOME still needs the Shell extension; Mutter does not implement layer-shell.

### Clean

```sh
tools/vivid.sh clean flatpak
tools/vivid.sh clean direct-run
```

Credits:

1. [waywallen](https://github.com/waywallen)
