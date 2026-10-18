// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A folder-view grid cell: a navigable folder shown as a 2x2 cover mosaic of its
// first four images, with the name and photo count overlaid along the bottom.
Item {
    id: root

    property var entry: ({})
    property color accent: "#4a90d9"
    property bool selected: false
    signal clicked(int modifiers)
    signal doubleClicked()
    signal rightClicked()

    readonly property var thumbs: (entry && entry.thumbs) ? entry.thumbs : []

    SystemPalette { id: sys }

    Rectangle {
        id: card
        anchors.fill: parent
        anchors.margins: 4
        radius: 6
        color: sys.alternateBase
        border.color: (root.selected || mouse.containsMouse) ? root.accent : sys.mid
        border.width: root.selected ? 3 : 1
        clip: true

        // 2x2 cover mosaic of the first four images.
        GridLayout {
            anchors.fill: parent
            anchors.margins: 3
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

        // Smart-album badge (a funnel) so a saved-query album reads differently
        // from a regular one.
        Rectangle {
            visible: root.entry && root.entry.smart === true
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.margins: 6
            width: 20
            height: 20
            radius: 10
            color: Qt.rgba(0, 0, 0, 0.6)
            Image {
                anchors.centerIn: parent
                source: "data:image/svg+xml," + encodeURIComponent(
                    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'>"
                    + "<path fill='white' d='M3 4 h18 l-7 8 v6 l-4 2 v-8 z'/></svg>")
                sourceSize.width: 12
                sourceSize.height: 12
                smooth: true
            }
        }

        // Name + count band.
        Rectangle {
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
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        cursorShape: Qt.PointingHandCursor
        onClicked: (mouse) => {
            if (mouse.button === Qt.RightButton)
                root.rightClicked()
            else
                root.clicked(mouse.modifiers)
        }
        onDoubleClicked: root.doubleClicked()
    }
}
