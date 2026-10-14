// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls

Item {
    id: root

    property bool selected: false
    property bool crop: true
    property color accent: "#4a90d9"

    // Content, passed in explicitly by the caller so this delegate works both in
    // a model-backed view (the grid) and when driven by a plain data object (the
    // timeline's per-month repeaters).
    property string filePath: ""
    property bool isVideo: false
    property int tagCount: 0

    signal clicked(int modifiers)
    signal doubleClicked()
    signal rightClicked()

    SystemPalette { id: sys }

    Rectangle {
        anchors.fill: parent
        anchors.margins: 4
        // Cropped thumbnails fill the card, so a distinct card color reads well.
        // Fitted thumbnails letterbox, so match the gallery background and let
        // the empty space blend in rather than showing a grey box.
        color: root.crop ? sys.alternateBase : sys.base
        border.color: root.selected ? root.accent : "transparent"
        border.width: 3
        radius: 4

        Image {
            anchors.fill: parent
            anchors.margins: 4
            source: "image://thumbs/" + encodeURIComponent(root.filePath)
            fillMode: root.crop ? Image.PreserveAspectCrop : Image.PreserveAspectFit
            asynchronous: true
            cache: true
            sourceSize.width: 320
            sourceSize.height: 320
            clip: true
        }

        // Play indicator for video items: a semi-opaque disc with a triangle.
        Rectangle {
            visible: root.isVideo
            anchors.centerIn: parent
            width: 44
            height: 44
            radius: width / 2
            color: Qt.rgba(0, 0, 0, 0.5)
            border.color: Qt.rgba(1, 1, 1, 0.85)
            border.width: 2

            // Equilateral "play" triangle, nudged right to look centered.
            Canvas {
                anchors.centerIn: parent
                anchors.horizontalCenterOffset: 2
                width: 16
                height: 18
                onPaint: {
                    const ctx = getContext("2d")
                    ctx.reset()
                    ctx.fillStyle = "white"
                    ctx.beginPath()
                    ctx.moveTo(0, 0)
                    ctx.lineTo(width, height / 2)
                    ctx.lineTo(0, height)
                    ctx.closePath()
                    ctx.fill()
                }
            }
        }

        // Tag-count badge.
        Rectangle {
            visible: root.tagCount > 0
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 8
            width: badge.implicitWidth + 12
            height: badge.implicitHeight + 6
            radius: height / 2
            color: Qt.rgba(0, 0, 0, 0.65)

            Label {
                id: badge
                anchors.centerIn: parent
                text: "🏷 " + root.tagCount
                color: "white"
                font.pixelSize: 12
            }
        }
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onClicked: (mouse) => {
            if (mouse.button === Qt.RightButton)
                root.rightClicked()
            else
                root.clicked(mouse.modifiers)
        }
        onDoubleClicked: root.doubleClicked()
    }
}
