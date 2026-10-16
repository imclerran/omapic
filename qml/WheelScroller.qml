// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick

// The app's single, shared wheel/trackpad scroll handler, so every scrollable
// panel feels identical and any future one inherits the behavior by dropping in
// one line. Overlay it on a scroll surface (as a sibling filling the same area)
// and point `flickable` at the thing that actually scrolls:
//
//   • a ListView / GridView is itself a Flickable → pass it directly
//   • a ScrollView wraps one                       → pass its `contentItem`
//
// Trackpad pixel-scrolling is amplified to one app-wide speed (`pixelFactor` —
// the single place that number lives). Mouse-wheel notches glide one `step` px
// each with momentum; when `step` is 0 they fall through to the surface's own
// native wheel handling.
//
// NOTE: this is for *scrolling* only. Surfaces that use the wheel to zoom (the
// photo viewer) own their wheel handling and must not layer this on top.
MouseArea {
    id: root

    // The surface that actually scrolls — any Flickable: a ListView/GridView
    // directly, or a ScrollView's `contentItem`. (Typed var because ScrollView
    // declares contentItem as Item even though it is a Flickable at runtime.)
    required property var flickable
    // The one trackpad multiplier shared across the whole app. Change it here to
    // retune every panel at once.
    property real pixelFactor: 4.0
    // Mouse-wheel distance per notch, in px (e.g. a grid's cellHeight so one
    // notch steps a single row). 0 leaves mouse-wheel notches to the surface.
    property real step: 0

    anchors.fill: parent
    acceptedButtons: Qt.NoButton   // clicks & hover pass straight through to content
    propagateComposedEvents: true

    // Momentum glide for mouse-wheel notches (the trackpad path is immediate).
    NumberAnimation {
        id: glide
        target: root.flickable
        property: "contentY"
        duration: 220
        easing.type: Easing.OutCubic
    }

    onWheel: (wheel) => {
        const f = root.flickable
        // A view's top extent is originY, not 0 — it drifts as delegates above
        // the viewport are recycled; clamp to it so the wheel can't push content
        // above the first row.
        const minY = f.originY
        const maxY = Math.max(minY, f.originY + f.contentHeight - f.height)
        if (wheel.pixelDelta.y !== 0) {
            // High-resolution trackpad: amplified, immediate.
            glide.stop()
            f.contentY = Math.max(minY, Math.min(maxY, f.contentY - wheel.pixelDelta.y * root.pixelFactor))
        } else if (root.step > 0) {
            // Mouse-wheel notches: glide one step with momentum.
            const d = (wheel.angleDelta.y / 120) * root.step
            glide.stop()
            glide.to = Math.max(minY, Math.min(maxY, f.contentY - d))
            glide.start()
        } else {
            // No custom mouse-wheel behavior here: let the surface handle it.
            wheel.accepted = false
        }
    }
}
