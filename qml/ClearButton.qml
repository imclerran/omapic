// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls

// A small ✕ affordance for clearing a TextField. Drop it inside the field:
//     TextField { id: f; ClearButton { field: f } }
// It reserves room on the field's right edge, appears only while the field has
// text, and clears (then refocuses) the field when clicked. If the field
// defines a refresh()/updateSuggestions() hook — the autocomplete boxes do —
// that's called after clearing so any open suggestion popup closes too.
Item {
    id: root

    // The TextField to clear. Pass it explicitly; falls back to the parent when
    // the button is nested directly inside the field.
    property TextField field: (parent as TextField)
    readonly property bool active: field && field.length > 0

    width: 16
    height: 16
    visible: active
    anchors.right: parent.right
    anchors.rightMargin: 6
    anchors.verticalCenter: parent.verticalCenter

    // Reserve space on the field's right so typed text never runs under the
    // button. Added to whatever right padding the style already applies.
    property real _basePadding: 0
    Component.onCompleted: {
        if (field) {
            _basePadding = field.rightPadding
            field.rightPadding = Qt.binding(function () {
                return root._basePadding + root.width + root.anchors.rightMargin
            })
        }
    }

    SystemPalette { id: sys }

    Rectangle {
        anchors.fill: parent
        radius: width / 2
        color: tap.containsMouse ? sys.mid : "transparent"
        Label {
            anchors.centerIn: parent
            text: "✕"
            font.pixelSize: 11
            color: sys.text
            opacity: tap.containsMouse ? 1.0 : 0.55
        }
    }

    MouseArea {
        id: tap
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: {
            if (!root.field)
                return
            root.field.text = ""
            if (typeof root.field.refresh === "function")
                root.field.refresh()
            if (typeof root.field.updateSuggestions === "function")
                root.field.updateSuggestions()
            root.field.forceActiveFocus()
        }
    }
}
