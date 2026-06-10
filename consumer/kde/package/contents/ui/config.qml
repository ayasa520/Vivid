/*
    SPDX-License-Identifier: GPL-3.0-or-later
*/

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

ColumnLayout {
    id: root

    spacing: Kirigami.Units.largeSpacing

    property string cfg_DisplayName
    property string cfg_SocketPath
    property bool cfg_ShowDiagnostics
    property bool cfg_MouseForward

    property string _probeError: ""
    property string _moduleVersion: ""

    function _probeSurface() {
        const component = Qt.createComponent("ImportTest.qml", Component.PreferSynchronous, root);
        if (!component) {
            root._probeError = "Failed to create ImportTest.qml";
            root._moduleVersion = "";
            return;
        }

        const finish = () => {
            if (component.status === Component.Error) {
                root._probeError = component.errorString();
                root._moduleVersion = "";
                return;
            }
            if (component.status !== Component.Ready)
                return;

            const object = component.createObject(root);
            root._probeError = "";
            root._moduleVersion = object ? object.version : "";
            if (object)
                object.destroy();
        };

        if (component.status === Component.Loading)
            component.statusChanged.connect(finish);
        else
            finish();
    }

    Component.onCompleted: root._probeSurface()

    Kirigami.FormLayout {
        Layout.fillWidth: true
        Layout.alignment: Qt.AlignTop

        QQC2.TextField {
            Kirigami.FormData.label: i18nd("plasma_wallpaper_dev.rikka.vivid.consumer.kde", "Display name:")
            placeholderText: i18nd("plasma_wallpaper_dev.rikka.vivid.consumer.kde", "Auto")
            text: cfg_DisplayName
            onTextChanged: cfg_DisplayName = text
        }

        QQC2.TextField {
            Kirigami.FormData.label: i18nd("plasma_wallpaper_dev.rikka.vivid.consumer.kde", "Socket path:")
            placeholderText: i18nd("plasma_wallpaper_dev.rikka.vivid.consumer.kde", "Default display socket")
            text: cfg_SocketPath
            onTextChanged: cfg_SocketPath = text
        }

        QQC2.CheckBox {
            Kirigami.FormData.label: i18nd("plasma_wallpaper_dev.rikka.vivid.consumer.kde", "Mouse forwarding:")
            checked: cfg_MouseForward
            onToggled: cfg_MouseForward = checked
        }

        QQC2.CheckBox {
            Kirigami.FormData.label: i18nd("plasma_wallpaper_dev.rikka.vivid.consumer.kde", "Diagnostics:")
            checked: cfg_ShowDiagnostics
            onToggled: cfg_ShowDiagnostics = checked
        }

        QQC2.Button {
            Kirigami.FormData.label: i18nd("plasma_wallpaper_dev.rikka.vivid.consumer.kde", "WebUI:")
            icon.name: "internet-web-browser"
            text: i18nd("plasma_wallpaper_dev.rikka.vivid.consumer.kde", "Open WebUI")
            onClicked: Qt.openUrlExternally("http://127.0.0.1:8765/")
        }

        QQC2.Label {
            Kirigami.FormData.label: i18nd("plasma_wallpaper_dev.rikka.vivid.consumer.kde", "Display module:")
            visible: root._moduleVersion.length > 0
            text: root._moduleVersion
        }
    }

    Kirigami.InlineMessage {
        Layout.fillWidth: true
        visible: root._probeError.length > 0
        type: Kirigami.MessageType.Error
        text: i18nd("plasma_wallpaper_dev.rikka.vivid.consumer.kde",
                    "Failed to load the embedded display module.")
    }

    QQC2.TextArea {
        Layout.fillWidth: true
        Layout.fillHeight: true
        visible: root._probeError.length > 0
        readOnly: true
        wrapMode: TextEdit.Wrap
        textFormat: TextEdit.PlainText
        font.family: "monospace"
        selectByMouse: true
        text: root._probeError
    }
}
