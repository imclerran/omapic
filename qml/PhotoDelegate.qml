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
    property bool favorite: false

    signal clicked(int modifiers)
    signal doubleClicked()
    signal rightClicked()
    signal favoriteToggled()

    SystemPalette { id: sys }

    // Reveal the (empty) star on hover; a favorited photo always shows its star.
    HoverHandler { id: cardHover }

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
            id: thumb
            anchors.fill: parent
            anchors.margins: 4
            source: "image://thumbs/" + encodeURIComponent(root.filePath)
            fillMode: root.crop ? Image.PreserveAspectCrop : Image.PreserveAspectFit
            asynchronous: true
            cache: true
            // Request at the cell's own size (Qt folds in the device pixel ratio)
            // so thumbnails stay crisp on large / HiDPI grids. The provider caches
            // each size separately and builds them lazily, so only the sizes
            // actually shown are ever generated.
            sourceSize.width: root.width > 0 ? root.width : 320
            sourceSize.height: root.height > 0 ? root.height : 320
            clip: true
        }

        // Placeholder for a thumbnail that couldn't be decoded — most often a
        // HEIC/HEIF when no HEIF image plugin is installed, but also any corrupt
        // file. Shows the file type so a broken tile reads as "unsupported" rather
        // than a mysterious blank card.
        Column {
            anchors.centerIn: parent
            spacing: 4
            visible: thumb.status === Image.Error
            opacity: 0.55
            Label {
                anchors.horizontalCenter: parent.horizontalCenter
                text: {
                    const dot = root.filePath.lastIndexOf(".")
                    return dot >= 0 ? root.filePath.substring(dot + 1).toUpperCase()
                                    : qsTr("?")
                }
                font.pixelSize: 18
                font.bold: true
                color: sys.text
            }
            Label {
                anchors.horizontalCenter: parent.horizontalCenter
                text: qsTr("can't preview")
                font.pixelSize: 11
                color: sys.text
            }
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

    // Favorite star, top-left. Declared after the click MouseArea so it sits on
    // top and its own clicks toggle the favorite instead of selecting the photo.
    // Shown when favorited, or on hover so it reads as "click to favorite".
    Rectangle {
        id: starButton
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.margins: 10
        width: 28
        height: 28
        radius: width / 2
        visible: root.favorite || cardHover.hovered || starArea.containsMouse
        color: Qt.rgba(0, 0, 0, 0.45)

        Label {
            anchors.centerIn: parent
            // Nudge up a touch so the glyph looks optically centered in the disc.
            anchors.verticalCenterOffset: -1
            text: root.favorite ? "★" : "☆"
            font.pixelSize: 18
            color: root.favorite ? "#ffd24a" : "white"
        }

        MouseArea {
            id: starArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: root.favoriteToggled()
        }
    }
}
