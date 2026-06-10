# Vivid Consumer KDE

This directory contains a Plasma 6 display consumer package for the existing
display-v1 transport.

The package embeds its Qt QML display module under
`contents/ui/VividDisplayEmbed/`, so the wallpaper can carry the display
library `.so` inside the extension package instead of depending on a
system-wide QML module.

## Build

```sh
tools/vivid.sh kde build
```

## Install For The Current User

```sh
tools/vivid.sh kde install
```

After upgrading the QML display module, restart plasmashell so Qt reloads the
plugin from the package:

```sh
systemctl --user restart plasma-plasmashell.service
```

## Package

```sh
tools/vivid.sh kde zip
```
