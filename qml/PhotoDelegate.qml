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

    signal clicked(int modifiers)
    signal doubleClicked()

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
            source: "image://thumbs/" + encodeURIComponent(model.filePath)
            fillMode: root.crop ? Image.PreserveAspectCrop : Image.PreserveAspectFit
            asynchronous: true
            cache: true
            sourceSize.width: 320
            sourceSize.height: 320
            clip: true
        }

        // Tag-count badge.
        Rectangle {
            visible: model.tags.length > 0
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
                text: "🏷 " + model.tags.length
                color: "white"
                font.pixelSize: 12
            }
        }
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton
        onClicked: (mouse) => root.clicked(mouse.modifiers)
        onDoubleClicked: root.doubleClicked()
    }
}
