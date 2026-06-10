/*
    SPDX-License-Identifier: GPL-3.0-or-later
*/

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.plasma.plasmoid

WallpaperItem {
    id: root

    property bool _initDone: false

    readonly property string defaultDisplayName: {
        const manufacturer = (Screen.manufacturer || "").trim();
        const model = (Screen.model || "").trim();
        const vendor = [manufacturer, model].filter(v => v.length > 0).join(" ");
        if (vendor.length > 0)
            return vendor;
        if (Screen.name && Screen.name.length > 0)
            return Screen.name;
        return "kde-plasma";
    }

    readonly property int stableOutputId: {
        const text = root.configuration.DisplayInstanceId || root.defaultDisplayName;
        let hash = 2166136261;
        for (let i = 0; i < text.length; i++) {
            hash ^= text.charCodeAt(i);
            hash = Math.imul(hash, 16777619);
        }
        return (hash & 0x7fffffff) || 1;
    }

    function _generateUuidV4() {
        return "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx".replace(/[xy]/g, function (c) {
            const r = Math.random() * 16 | 0;
            const v = c === "x" ? r : (r & 0x3 | 0x8);
            return v.toString(16);
        });
    }

    function _refreshRateMhz() {
        const rate = Number(Screen.refreshRate || 0);
        return rate > 0 ? Math.round(rate * 1000) : 0;
    }

    Rectangle {
        anchors.fill: parent
        color: "black"
    }

    WindowModel {
        id: windowModel
    }

    Loader {
        id: surfaceLoader
        anchors.fill: parent
        asynchronous: false
        active: root._initDone
        source: "WallpaperSurface.qml"

        onLoaded: {
            item.socketPathBinding = Qt.binding(() => root.configuration.SocketPath);
            item.displayNameBinding = Qt.binding(() =>
                root.configuration.DisplayName.length > 0
                    ? root.configuration.DisplayName
                    : root.defaultDisplayName);
            item.screenNameBinding = Qt.binding(() => Screen.name || "");
            item.instanceIdBinding = Qt.binding(() => root.configuration.DisplayInstanceId);
            item.consumerOutputIdBinding = Qt.binding(() => root.stableOutputId);
            item.monitorIndexBinding = 0;
            item.displayXBinding = Qt.binding(() => Math.round(Screen.virtualX));
            item.displayYBinding = Qt.binding(() => Math.round(Screen.virtualY));
            item.logicalWidthBinding = Qt.binding(() => Math.max(1, Math.round(root.width)));
            item.logicalHeightBinding = Qt.binding(() => Math.max(1, Math.round(root.height)));
            item.refreshRateMhzBinding = Qt.binding(() => root._refreshRateMhz());
            item.mouseForwardBinding = Qt.binding(() => root.configuration.MouseForward);
            item.windowStateFlagsBinding = Qt.binding(() => windowModel.flags);
        }
    }

    Loader {
        anchors.centerIn: parent
        width: Math.min(parent.width - 80, 680)
        active: surfaceLoader.status === Loader.Error
        visible: active
        sourceComponent: Rectangle {
            color: Qt.rgba(0, 0, 0, 0.72)
            radius: 8
            implicitHeight: column.implicitHeight + 28

            ColumnLayout {
                id: column
                anchors.fill: parent
                anchors.margins: 14
                spacing: 8

                QQC2.Label {
                    Layout.fillWidth: true
                    color: "white"
                    font.bold: true
                    wrapMode: Text.WordWrap
                    text: i18nd("plasma_wallpaper_dev.rikka.vivid.consumer.kde",
                                "Wallpaper display module failed to load")
                }

                QQC2.Label {
                    Layout.fillWidth: true
                    color: "#9fc5ff"
                    font.family: "monospace"
                    wrapMode: Text.WrapAnywhere
                    text: surfaceLoader.item ? "" : surfaceLoader.source
                }
            }
        }
    }

    Loader {
        anchors { top: parent.top; left: parent.left; margins: 12 }
        active: root.configuration.ShowDiagnostics && surfaceLoader.status === Loader.Ready
        sourceComponent: Rectangle {
            id: diagBox
            width: diagText.implicitWidth + 16
            height: diagText.implicitHeight + 12
            color: Qt.rgba(0, 0, 0, 0.58)
            radius: 6

            readonly property var display: surfaceLoader.item

            Text {
                id: diagText
                x: 8
                y: 6
                color: "#d7e0ff"
                font.pixelSize: 13
                font.family: "monospace"
                text: {
                    const d = diagBox.display;
                    let s = "name:   " + d.displayName;
                    s += "\noutput: " + (d.outputId === 0 ? "-" : d.outputId)
                         + "  consumer: " + d.consumerOutputId;
                    s += "\nscreen: " + Screen.name + " "
                         + Screen.width + "x" + Screen.height;
                    s += "\nconn:   " + connText(d.connState)
                         + "  stream: " + streamText(d.streamState);
                    s += "\nframes: " + d.framesReceived;
                    s += "\nclear:  " + d.clearColor.toString();
                    s += "\nwindows: flags=0x" + windowModel.flags.toString(16)
                         + " count=" + windowModel.windows.length;
                    if (d.lastError.length > 0)
                        s += "\nerror:  " + d.lastError;
                    return s;
                }

                function connText(value) {
                    switch (value) {
                    case 0: return "disconnected";
                    case 1: return "connecting";
                    case 2: return "handshaking";
                    case 3: return "connected";
                    case 4: return "error";
                    }
                    return "unknown";
                }

                function streamText(value) {
                    switch (value) {
                    case 0: return "inactive";
                    case 1: return "active";
                    }
                    return "unknown";
                }
            }
        }
    }

    Component.onCompleted: {
        if (root.configuration.DisplayInstanceId.length === 0) {
            root.configuration.DisplayInstanceId = root._generateUuidV4();
            root.configuration.writeConfig();
        }
        root._initDone = true;
        root.loading = false;
    }
}
