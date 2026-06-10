/*
    SPDX-License-Identifier: GPL-3.0-or-later
*/

import QtQuick
import "VividDisplayEmbed" 1.0 as Wallpaper

Wallpaper.VividDisplay {
    id: display

    property string socketPathBinding
    property string displayNameBinding
    property string screenNameBinding
    property string instanceIdBinding
    property int consumerOutputIdBinding: 0
    property int monitorIndexBinding: 0
    property int displayXBinding: 0
    property int displayYBinding: 0
    property int logicalWidthBinding: 1
    property int logicalHeightBinding: 1
    property int refreshRateMhzBinding: 0
    property bool mouseForwardBinding: true
    property int windowStateFlagsBinding: 0

    socketPath:          socketPathBinding
    displayName:         displayNameBinding
    screenName:          screenNameBinding
    instanceId:          instanceIdBinding
    consumerOutputId:    consumerOutputIdBinding
    monitorIndex:        monitorIndexBinding
    displayX:            displayXBinding
    displayY:            displayYBinding
    logicalWidth:        logicalWidthBinding
    logicalHeight:       logicalHeightBinding
    refreshRateMhz:      refreshRateMhzBinding
    mouseForwardEnabled: mouseForwardBinding
    windowStateFlags:    windowStateFlagsBinding
    autoReconnect:       true

    Wallpaper.VividMediaBridge {
        display: display
        enabled: true
    }
}
