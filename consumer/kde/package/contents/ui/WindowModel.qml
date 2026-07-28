/*
    SPDX-License-Identifier: GPL-3.0-or-later
*/

import QtQuick
import org.kde.taskmanager 0.1 as TaskManager

Item {
    id: wm

    // Match Waywallen's KDE route: let TasksModel perform the compositor-aware
    // per-screen filtering and publish only the compact window-state bitmask.
    property var screenGeometry

    readonly property int flags: _flags
    property int _flags: 0

    // Diagnostics-only snapshot of the windows which survive activity,
    // virtual-desktop, and screen filtering.
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
        filterByScreen:         true
        screenGeometry:         wm.screenGeometry

        onActiveTaskChanged: wm.recompute()
        onDataChanged:       wm.recompute()
        onCountChanged:      wm.recompute()
    }

    Component.onCompleted: recompute()
    onScreenGeometryChanged: recompute()

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
            if (activities && activities.length &&
                activities.indexOf(currentActivity) === -1) {
                continue;
            }

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
            if (fullscreen)
                nextFlags |= 8;
            else if (maximized)
                nextFlags |= 4;
        }

        if (nextFlags !== _flags)
            _flags = nextFlags;
        _windows = nextWindows;
    }
}
