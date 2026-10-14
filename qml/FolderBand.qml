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

    // "#rrggbb" for a color, for embedding in the folder icon's SVG fill.
    function toHex(c) {
        function h(v) { const s = Math.round(v * 255).toString(16); return s.length < 2 ? "0" + s : s }
        return "#" + h(c.r) + h(c.g) + h(c.b)
    }

    // An inline SVG icon (a single 24x24 path) filled with `c`, as a data URI.
    // Vectors instead of emoji glyphs, so the tiles render consistently and
    // follow the active theme. Re-evaluated when the color changes, so the icons
    // recolor live with the theme.
    function iconSource(pathData, c) {
        return "data:image/svg+xml," + encodeURIComponent(
            "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'><path fill='"
            + band.toHex(c) + "' d='" + pathData + "'/></svg>")
    }
    function folderIcon(c) {
        return band.iconSource("M10 4H4c-1.1 0-2 .9-2 2v12c0 1.1.9 2 2 2h16c1.1 0 2-.9 2-2V8c0-1.1-.9-2-2-2h-8l-2-2z", c)
    }
    function upIcon(c) {
        // A solid block up-arrow (wide head, thick shaft) rather than a thin line arrow.
        return band.iconSource("M12 3 L20 11 L15 11 L15 20 L9 20 L9 11 L4 11 Z", c)
    }

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

                        // Themed folder glyph for folders; the Up tile keeps its
                        // arrow (tinted to match).
                        Item {
                            Layout.preferredWidth: 16
                            Layout.preferredHeight: 16
                            Image {
                                anchors.centerIn: parent
                                visible: !tile.isUp
                                source: band.folderIcon(band.accent)
                                sourceSize.width: 16
                                sourceSize.height: 16
                                smooth: true
                            }
                            Image {
                                anchors.centerIn: parent
                                visible: tile.isUp
                                source: band.upIcon(band.accent)
                                sourceSize.width: 16
                                sourceSize.height: 16
                                smooth: true
                            }
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
