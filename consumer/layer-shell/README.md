# Vivid Layer-Shell Consumer

Catch-all **layer-shell** wallpaper client for compositors that advertise
`zwlr_layer_shell_v1`. That includes wlroots compositors (Sway, Hyprland, labwc,
river, wayfire) and others such as niri. It is **one process**, not a per-DE
fork.

GNOME Shell / Mutter does not implement layer-shell; keep using `consumer/gnome`.
Plasma already has `consumer/kde`. This client can still run on Plasma as a
generic layer-shell wallpaper, but the native plugin is the better integration
there.

## Build

```sh
tools/vivid.sh layer-shell build
```

## Run

Start the producer first (`tools/vivid.sh direct-run run` or an already-running
daemon). Then:

```sh
tools/vivid.sh layer-shell run
```

Layer override:

```
--layer background|bottom|top|overlay  # default: bottom; niri: background
```

The default display-v1 socket is `$VIVID_DISPLAY_SOCKET` or
`$XDG_RUNTIME_DIR/vivid/display-v1.sock`.

The wallpaper surface uses namespace `vivid`, keyboard interactivity none,
exclusive zone `-1`, and a full opaque region. Pointer interaction is enabled:
the consumer binds `wl_seat`, maps surface-local coordinates to producer pixels,
and forwards motion, buttons, and scroll axes over display-v1.

A layer surface only receives pointer events where it is the compositor's
input target. Windows above it continue to receive their own input. Keep the
niri surface out of `place-within-backdrop`, because niri explicitly suppresses
all input for surfaces moved there.

## Presentation and diagnostics

Vivid's producer has its own render clock. The consumer waits for the producer
acquire fence, copies into its EGL window, signals the producer release
immediately after `glFinish`, then requests `wl_surface.frame`, swaps, damages,
and commits. It asks for swap interval 0. If a compositor omits the callback, a
refresh-period timer schedules another swap attempt; like any main-thread timer,
it cannot interrupt an `eglSwapBuffers` call already waiting inside the EGL
driver.

## Compositor notes

Hyprland:

- Layer namespace is `vivid` (`hyprctl layers`)
- A `no_anim` layer rule so workspace animations do not treat the wallpaper as a panel
- Run only one wallpaper client at a time
- `windowrule` does nothing here: this is a layer-shell surface, not an xdg-toplevel
- `misc.vfr = false` is a diagnostic setting, not part of the required wallpaper configuration

Sway needs no extra layer-rule. One consumer attaches to every output.

niri uses the regular **background** layer by default. No layer rule or
transparent workspace background is needed; this keeps normal pointer input
targeting available when the wallpaper is not covered.

Copy-paste snippets live in `examples/`.

### Hyprland 0.55+ (`~/.config/hypr/hyprland.lua`)

```lua
hl.on("hyprland.start", function()
    hl.exec_cmd("vivid-layer-shell-consumer")
end)

hl.layer_rule({
    name = "vivid-wallpaper",
    match = { namespace = "^vivid$" },
    no_anim = true,
})

hl.config({
    misc = {
        force_default_wallpaper = 0,
        disable_hyprland_logo = true,
    },
})
```

The complete snippet is `examples/hyprland.lua`. Hyprland 0.54 and earlier use
the hyprlang syntax in `examples/hyprland.conf`. Do not start another wallpaper
client alongside this one.

### Sway (`~/.config/sway/config`)

```
# Keep a solid color until the consumer attaches (same idea as output * bg).
output * bg #24273a solid_color
exec vivid-layer-shell-consumer
```

### niri (`~/.config/niri/config.kdl`)

```
spawn-at-startup "vivid-layer-shell-consumer"
// Do not start another wallpaper client; two BACKGROUND surfaces will stack.
```

Force the niri layer if auto-detect misses: `vivid-layer-shell-consumer --layer background`.

### XDG autostart

See `examples/vivid-layer-shell.desktop`. Copy to `~/.config/autostart/` only if the
compositor actually launches XDG autostart entries (many tiling WMs do not;
they want `exec-once` / `exec` / `spawn-at-startup` instead).
