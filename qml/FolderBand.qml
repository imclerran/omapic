// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Folder-view navigation strip: a breadcrumb plus clickable tiles — the library
// root folders at the top level, or an "Up" tile and the current folder's
// subfolders once you've descended. `entries` comes from Library.folderEntries()
// as a list of { type: "folder"|"up", name, path, count }.
Rectangle {
    id: band

    property var entries: []
    property string currentFolder: ""
    property color accent: "#4a90d9"

    // Requests navigation to `path` ("" returns to the root listing).
    signal navigate(string path)

    color: palette.alternateBase
    implicitHeight: layout.implicitHeight + 16

    ColumnLayout {
        id: layout
        anchors.fill: parent
        anchors.margins: 8
        spacing: 6

        Label {
            Layout.fillWidth: true
            text: band.currentFolder === "" ? qsTr("Library") : band.currentFolder
            elide: Text.ElideMiddle
            font.pixelSize: 12
            color: palette.placeholderText
        }

        Flow {
            Layout.fillWidth: true
            spacing: 8

            Repeater {
                model: band.entries

                delegate: Rectangle {
                    id: tile
                    required property var modelData
                    readonly property bool isUp: modelData.type === "up"

                    implicitWidth: Math.min(220, row.implicitWidth + 20)
                    height: 36
                    radius: 6
                    color: mouse.containsMouse ? Qt.rgba(band.accent.r, band.accent.g, band.accent.b, 0.18)
                                               : palette.base
                    border.color: tile.isUp ? band.accent : palette.mid
                    border.width: 1

                    RowLayout {
                        id: row
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 10
                        spacing: 6

                        Label {
                            text: tile.isUp ? "⬆" : "📁"
                            font.pixelSize: 14
                        }
                        Label {
                            Layout.fillWidth: true
                            text: tile.isUp ? qsTr("Up") : tile.modelData.name
                            elide: Text.ElideRight
                            color: palette.text
                        }
                        Label {
                            // The "up" tile carries no count; guard so the binding
                            // never assigns undefined.
                            visible: !tile.isUp
                            text: tile.isUp ? "" : tile.modelData.count
                            font.pixelSize: 11
                            color: palette.placeholderText
                        }
                    }

                    MouseArea {
                        id: mouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: band.navigate(tile.modelData.path)
                    }

                    ToolTip.visible: mouse.containsMouse && !tile.isUp
                    ToolTip.text: tile.modelData.path
                }
            }
        }
    }
}
