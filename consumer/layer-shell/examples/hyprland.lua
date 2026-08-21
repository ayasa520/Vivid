-- Add these declarations to ~/.config/hypr/hyprland.lua.
-- Namespace: vivid  (hyprctl layers)

hl.on("hyprland.start", function()
    hl.exec_cmd("vivid-layer-shell-consumer")
end)

hl.layer_rule({
    name = "vivid-wallpaper",
    match = { namespace = "^vivid$" },
    no_anim = true,
})

-- Do not start hyprpaper or swaybg alongside this consumer.
hl.config({
    misc = {
        force_default_wallpaper = 0,
        disable_hyprland_logo = true,
    },
})
