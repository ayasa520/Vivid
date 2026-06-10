/*
    SPDX-License-Identifier: GPL-3.0-or-later

    Session-wide window fact collector for the KDE consumer. Plasma creates one
    wallpaper item per screen, so every instance reports the same global facts
    instead of letting per-screen messages overwrite each other in the producer.
*/

import QtQuick
import org.kde.taskmanager 0.1 as TaskManager

Item {
    id: wm

    readonly property int flags: _flags
    property int _flags: 0

    readonly property var windows: _windows
    property var _windows: []

    TaskManager.ActivityInfo { id: activityInfo }
    TaskManager.VirtualDesktopInfo { id: vdInfo }

    TaskManager.TasksModel {
        id: tasksModel
        sortMode:               TaskManager.TasksModel.SortVirtualDesktop
        groupMode:              TaskManager.TasksModel.GroupDisabled
        filterByVirtualDesktop: true
        virtualDesktop:         vdInfo.currentDesktop
        filterByScreen:         false

        onActiveTaskChanged: wm.recompute()
        onDataChanged:       wm.recompute()
        onCountChanged:      wm.recompute()
    }

    Component.onCompleted: recompute()

    function _role(idx, name) {
        return tasksModel.data(idx, TaskManager.AbstractTasksModel[name]);
    }

    function recompute() {
        let nextFlags = 0;
        const nextWindows = [];
        const currentActivity = activityInfo.currentActivity;

        for (let i = 0; i < tasksModel.count; i++) {
            const idx = tasksModel.makeModelIndex(i);
            if (_role(idx, "IsWindow") !== true)
                continue;

            const activities = _role(idx, "Activities");
            if (activities && activities.length && activities.indexOf(currentActivity) === -1)
                continue;

            const minimized = _role(idx, "IsMinimized") === true;
            const active = _role(idx, "IsActive") === true;
            const fullscreen = _role(idx, "IsFullScreen") === true;
            const maximized = _role(idx, "IsMaximized") === true;

            nextWindows.push({
                title: tasksModel.data(idx, 0) || "",
                app: _role(idx, "AppName") || "",
                minimized,
                active,
                maximized,
                fullscreen,
            });

            if (minimized)
                continue;
            nextFlags |= 1;
            if (active)
                nextFlags |= 2;
            if (maximized)
                nextFlags |= 4;
            if (fullscreen)
                nextFlags |= 8;
        }

        if (nextFlags !== _flags)
            _flags = nextFlags;
        _windows = nextWindows;
    }
}
