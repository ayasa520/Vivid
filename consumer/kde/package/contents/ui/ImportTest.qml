/*
    SPDX-License-Identifier: GPL-3.0-or-later

    Probe stub for the bundled contents/ui/VividDisplayEmbed QML module.
    Plasma loads wallpaper config pages in the same QML engine as the package,
    so compiling this file proves the embedded display module is reachable.
*/

import QtQuick
import "VividDisplayEmbed" 1.0 as Wallpaper

Wallpaper.PluginInfo {}
