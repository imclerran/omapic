// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: root

    property var library
    property color accent: "#4a90d9"

    title: qsTr("Manage Tags")
    modal: true
    anchors.centerIn: parent
    width: Math.min(560, (parent ? parent.width : 560) - 80)
    standardButtons: Dialog.Close

    // Tag awaiting delete confirmation; -1 when none is pending.
    property int pendingId: -1
    property string pendingName: ""
    property int pendingCount: 0

    Dialog {
        id: confirmDialog
        title: qsTr("Delete tag?")
        modal: true
        anchors.centerIn: parent
        width: Math.min(460, root.width - 40)
        standardButtons: Dialog.Yes | Dialog.No

        onAccepted: {
            root.library.deleteTag(root.pendingId)
            root.pendingId = -1
            root.pendingName = ""
            root.pendingCount = 0
        }
        onRejected: {
            root.pendingId = -1
            root.pendingName = ""
            root.pendingCount = 0
        }

        Label {
            width: parent.width
            wrapMode: Text.WordWrap
            text: root.pendingCount > 0
                  ? qsTr("Delete the tag “%1”? It will be removed from %2 photo(s). This cannot be undone.")
                        .arg(root.pendingName).arg(root.pendingCount)
                  : qsTr("Delete the unused tag “%1”?").arg(root.pendingName)
        }
    }

    Dialog {
        id: renameDialog
        title: qsTr("Rename tag")
        modal: true
        anchors.centerIn: parent
        width: Math.min(460, root.width - 40)
        closePolicy: Popup.CloseOnEscape

        property int targetId: -1
        property string originalName: ""

        function openFor(id, name) {
            targetId = id
            originalName = name
            field.text = name
            open()
            field.forceActiveFocus()
            field.selectAll()
        }

        // True when another tag already uses this name (case-insensitive). A name
        // that differs from the original only in casing is NOT a conflict.
        function nameTaken(name) {
            const n = name.trim().toLowerCase()
            const all = root.library.allTags
            for (let i = 0; i < all.length; ++i) {
                if (all[i].id !== renameDialog.targetId && all[i].name.toLowerCase() === n)
                    return true
            }
            return false
        }

        readonly property string trimmed: field.text.trim()
        readonly property bool conflict: trimmed.length > 0 && nameTaken(trimmed)
        readonly property bool canApply: trimmed.length > 0 && !conflict
                                         && trimmed !== originalName

        function apply() {
            if (!canApply)
                return
            root.library.renameTag(targetId, trimmed)
            close()
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 10

            TextField {
                id: field
                Layout.fillWidth: true
                selectByMouse: true
                onAccepted: renameDialog.apply()
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                visible: renameDialog.conflict
                text: qsTr("A tag named “%1” already exists.").arg(renameDialog.trimmed)
                color: "#d64545"
                font.pixelSize: 12
            }

            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                Button {
                    text: qsTr("Cancel")
                    onClicked: renameDialog.close()
                }
                Button {
                    text: qsTr("Rename")
                    enabled: renameDialog.canApply
                    onClicked: renameDialog.apply()
                }
            }
        }
    }

    Frame {
        anchors.fill: parent
        padding: 0

        ListView {
            id: list
            anchors.fill: parent
            clip: true
            implicitHeight: 320
            model: root.library.allTags
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}

            delegate: ItemDelegate {
                id: row
                width: ListView.view.width
                hoverEnabled: true

                required property var modelData

                contentItem: RowLayout {
                    spacing: 12

                    Label {
                        Layout.fillWidth: true
                        text: row.modelData.name
                        elide: Text.ElideRight
                        font.pixelSize: 14
                    }
                    Label {
                        text: row.modelData.count === 1
                              ? qsTr("1 photo")
                              : qsTr("%1 photos").arg(row.modelData.count)
                        font.pixelSize: 12
                        color: palette.placeholderText
                    }
                    Button {
                        text: qsTr("Rename")
                        onClicked: renameDialog.openFor(row.modelData.id, row.modelData.name)
                    }
                    Button {
                        text: qsTr("Delete")
                        onClicked: {
                            root.pendingId = row.modelData.id
                            root.pendingName = row.modelData.name
                            root.pendingCount = row.modelData.count
                            confirmDialog.open()
                        }
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                width: parent.width - 32
                visible: list.count === 0
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: qsTr("No tags yet. Select a photo and add some.")
                color: palette.placeholderText
            }
        }
    }
}
