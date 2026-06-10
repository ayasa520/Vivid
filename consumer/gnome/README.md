# Vivid Consumer GNOME

This directory contains the GNOME Shell consumer for the display-v1 transport.

It installs a Shell extension plus the display receiver. The old
in-process renderer backends are intentionally not part of this tree.

## Build

```sh
tools/vivid.sh gnome build
```

To install for the current user:

```sh
tools/vivid.sh gnome install
```

## Package

```sh
tools/vivid.sh gnome zip
```
