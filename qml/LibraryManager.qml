// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

Dialog {
    id: root

    property var library
    property color accent: "#4a90d9"

    title: qsTr("Library Folders")
    modal: true
    anchors.centerIn: parent
    width: Math.min(620, (parent ? parent.width : 620) - 80)
    standardButtons: Dialog.Close

    // Folder the user is about to remove; -1 when no confirmation is pending.
    property int pendingId: -1
    property string pendingPath: ""

    FolderDialog {
        id: addDialog
        title: qsTr("Choose a folder to add")
        onAccepted: root.library.importDirectory(selectedFolder)
    }

    Dialog {
        id: confirmDialog
        title: qsTr("Remove folder?")
        modal: true
        anchors.centerIn: parent
        width: Math.min(460, root.width - 40)
        standardButtons: Dialog.Yes | Dialog.No

        onAccepted: {
            root.library.removeFolder(root.pendingId)
            root.pendingId = -1
            root.pendingPath = ""
        }
        onRejected: {
            root.pendingId = -1
            root.pendingPath = ""
        }

        Label {
            width: parent.width
            wrapMode: Text.WordWrap
            text: qsTr("Remove “%1” from the library?\n\nIts photos will be removed from the gallery. Their tags are remembered by content, so re-adding the folder restores them. The files on disk are not touched.")
                  .arg(root.pendingPath)
        }
    }

    Dialog {
        id: confirmOrphans
        title: qsTr("Remove loose photos?")
        modal: true
        anchors.centerIn: parent
        width: Math.min(460, root.width - 40)
        standardButtons: Dialog.Yes | Dialog.No

        onAccepted: root.library.removeOrphanPhotos()

        Label {
            width: parent.width
            wrapMode: Text.WordWrap
            text: qsTr("Remove %1 photo(s) that aren't in any folder?\n\nTheir tags are remembered by content and will be restored if you add a folder that contains them again. The files on disk are not touched.")
                  .arg(root.library.orphanPhotoCount)
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 12

        Frame {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 220
            padding: 0

            ListView {
                id: list
                anchors.fill: parent
                clip: true
                model: root.library.folders
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}

                delegate: ItemDelegate {
                    id: row
                    width: ListView.view.width
                    hoverEnabled: true
                    highlighted: false

                    required property var modelData

                    contentItem: RowLayout {
                        spacing: 12

                        Switch {
                            checked: row.modelData.enabled
                            ToolTip.visible: hovered
                            ToolTip.text: checked ? qsTr("Shown in library — click to hide")
                                                  : qsTr("Hidden from library — click to show")
                            onToggled: root.library.setFolderEnabled(row.modelData.id, checked)
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            // Dim the row when the folder is hidden.
                            opacity: row.modelData.enabled ? 1.0 : 0.5
                            Label {
                                Layout.fillWidth: true
                                text: row.modelData.path
                                elide: Text.ElideMiddle
                                font.pixelSize: 14
                            }
                            Label {
                                text: {
                                    const n = row.modelData.count
                                    const base = n === 1 ? qsTr("1 photo") : qsTr("%1 photos").arg(n)
                                    return row.modelData.enabled ? base : base + qsTr(" · hidden")
                                }
                                font.pixelSize: 12
                                color: palette.placeholderText
                            }
                        }

                        Button {
                            text: qsTr("Remove")
                            onClicked: {
                                root.pendingId = row.modelData.id
                                root.pendingPath = row.modelData.path
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
                    text: qsTr("No folders yet. Add one to build your library.")
                    color: palette.placeholderText
                }
            }
        }

        // Photos imported before folder tracking (or otherwise left behind) that
        // aren't under any registered folder.
        Frame {
            Layout.fillWidth: true
            visible: root.library.orphanPhotoCount > 0

            RowLayout {
                anchors.fill: parent
                spacing: 12

                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("%1 photo(s) aren't in any folder.")
                          .arg(root.library.orphanPhotoCount)
                }
                Button {
                    text: qsTr("Remove")
                    onClicked: confirmOrphans.open()
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Button {
                text: qsTr("Add Folder…")
                onClicked: addDialog.open()
            }
            Item { Layout.fillWidth: true }
            Button {
                text: qsTr("Rescan")
                enabled: list.count > 0
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Pick up new, moved, and deleted files on disk")
                onClicked: root.library.rescan()
            }
        }
    }
}
