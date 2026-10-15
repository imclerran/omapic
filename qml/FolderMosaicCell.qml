// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A folder-view grid cell: a navigable folder shown as a 2x2 cover mosaic of its
// first four images (name + count overlaid), or the "Up" entry shown as an arrow.
Item {
    id: root

    property var entry: ({})
    property color accent: "#4a90d9"
    property bool selected: false
    signal clicked(int modifiers)
    signal doubleClicked()

    readonly property bool isUp: entry && entry.type === "up"
    readonly property var thumbs: (entry && entry.thumbs) ? entry.thumbs : []

    SystemPalette { id: sys }

    // "#rrggbb" for a color, for the Up arrow's SVG fill.
    function hex(c) {
        function h(v) { const s = Math.round(v * 255).toString(16); return s.length < 2 ? "0" + s : s }
        return "#" + h(c.r) + h(c.g) + h(c.b)
    }

    Rectangle {
        id: card
        anchors.fill: parent
        anchors.margins: 4
        radius: 6
        color: sys.alternateBase
        border.color: (root.selected || mouse.containsMouse) ? root.accent : sys.mid
        border.width: root.selected ? 3 : 1
        clip: true

        // --- Folder: 2x2 cover mosaic of the first four images ---
        GridLayout {
            anchors.fill: parent
            anchors.margins: 3
            visible: !root.isUp
            columns: 2
            rowSpacing: 2
            columnSpacing: 2
            Repeater {
                model: 4
                delegate: Rectangle {
                    required property int index
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.preferredWidth: 1
                    Layout.preferredHeight: 1
                    color: sys.base
                    clip: true
                    Image {
                        anchors.fill: parent
                        visible: parent.index < root.thumbs.length
                        source: parent.index < root.thumbs.length
                                ? "image://thumbs/" + encodeURIComponent(root.thumbs[parent.index]) : ""
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        cache: true
                        sourceSize.width: 200
                        sourceSize.height: 200
                    }
                }
            }
        }

        // --- Name + count band for folders ---
        Rectangle {
            visible: !root.isUp
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: bandRow.implicitHeight + 10
            color: Qt.rgba(0, 0, 0, 0.6)
            Row {
                id: bandRow
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                spacing: 6
                Label {
                    width: bandRow.width - countLabel.implicitWidth - bandRow.spacing
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.entry && root.entry.name ? root.entry.name : ""
                    color: "white"
                    font.pixelSize: 12
                    elide: Text.ElideRight
                }
                Label {
                    id: countLabel
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.entry && root.entry.count !== undefined ? root.entry.count : ""
                    color: Qt.rgba(1, 1, 1, 0.7)
                    font.pixelSize: 11
                }
            }
        }

        // --- Up entry: a bold arrow and label ---
        Column {
            anchors.centerIn: parent
            visible: root.isUp
            spacing: 4
            Image {
                anchors.horizontalCenter: parent.horizontalCenter
                source: "data:image/svg+xml," + encodeURIComponent(
                    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'><path fill='"
                    + root.hex(root.accent)
                    + "' d='M12 3 L20 11 L15 11 L15 20 L9 20 L9 11 L4 11 Z'/></svg>")
                sourceSize.width: Math.round(card.width * 0.32)
                sourceSize.height: Math.round(card.width * 0.32)
                smooth: true
            }
            Label {
                anchors.horizontalCenter: parent.horizontalCenter
                text: qsTr("Up")
                color: palette.text
                font.pixelSize: 13
            }
        }
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: (mouse) => root.clicked(mouse.modifiers)
        onDoubleClicked: root.doubleClicked()
    }
}
