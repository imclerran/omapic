// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Window
import QtQuick.Controls
import QtMultimedia

// Single-item viewer: fit / pan / zoom / 1:1 for images, plain playback for
// videos, with left/right navigation across a `sources` playlist. Driven by a
// plain list of file:// URLs (no Library dependency), so it serves both the
// in-gallery "open" action and the standalone `omapic <file>` launch mode.
Window {
    id: root

    property var sources: []
    property int index: 0
    // The viewer's slide/zoom should reset when the shown item changes.
    property bool muted: true

    readonly property int count: sources.length
    readonly property string currentSource: (index >= 0 && index < count) ? sources[index] : ""
    readonly property string currentName: currentSource === ""
        ? "" : decodeURIComponent(String(currentSource).substring(String(currentSource).lastIndexOf('/') + 1))
    readonly property bool currentIsVideo: /\.(mp4|m4v|webm|mkv|mov|avi)$/i.test(String(currentSource))

    property bool showName: false

    // Visibility to show at on open: windowed for the standalone viewer, full
    // screen when it stands in for the gallery slideshow.
    property int preferredVisibility: Window.Windowed

    // Ask the host to start a slideshow over the current set, and to close.
    signal slideshowRequested()
    signal closeRequested()

    width: 1100
    height: 760
    color: "black"
    title: currentName === "" ? qsTr("Omapic — viewer") : (currentName + qsTr(" — Omapic"))

    function clampIndex() {
        if (count === 0) { index = 0; return }
        if (index < 0) index = 0
        if (index >= count) index = count - 1
    }

    // Open at item i (and show the window if hidden).
    function openAt(i) {
        index = i
        clampIndex()
        if (visibility === Window.Hidden)
            visibility = preferredVisibility
        requestActivate()
        keyCatcher.forceActiveFocus()
    }

    function go(i) {
        if (count === 0)
            return
        index = (i % count + count) % count
    }
    function next() { go(index + 1) }
    function prev() { go(index - 1) }

    // --- Image fit / zoom state -------------------------------------------
    // natW/natH are the image's natural pixel size; zoom is the absolute scale
    // (1.0 == 1:1). fitScale fits the whole image within the viewport.
    property real natW: 0
    property real natH: 0
    property real zoom: 1.0
    // True while the image is shown fit-to-window (the default). Tracked as a flag
    // rather than compared to fitScale so a resize can't leave a stale "zoomed"
    // badge, and so a resize re-fits only when the user hasn't zoomed in.
    property bool atFit: true
    readonly property real fitScale: (natW > 0 && natH > 0)
        ? Math.min(stage.width / natW, stage.height / natH) : 1.0
    readonly property real minZoom: Math.min(fitScale, 1.0) * 0.5
    readonly property real maxZoom: Math.max(fitScale, 1.0) * 8.0
    readonly property bool zoomable: !currentIsVideo && natW > 0

    function fit() { zoom = fitScale; centerImage(); atFit = true }
    function oneToOne() { zoomAtCenter(1.0) }
    function clampZoom(z) { return Math.max(minZoom, Math.min(maxZoom, z)) }

    function centerImage() {
        img.x = (stage.width - img.width) / 2
        img.y = (stage.height - img.height) / 2
    }

    // Keep the image within the viewport: center it on any axis where it's
    // smaller than the stage, otherwise clamp panning to the edges.
    function clampPan() {
        if (img.width <= stage.width)
            img.x = (stage.width - img.width) / 2
        else
            img.x = Math.max(stage.width - img.width, Math.min(0, img.x))
        if (img.height <= stage.height)
            img.y = (stage.height - img.height) / 2
        else
            img.y = Math.max(stage.height - img.height, Math.min(0, img.y))
    }

    // Zoom keeping the viewport point (vx,vy) anchored over the same image pixel.
    function zoomAt(newZoom, vx, vy) {
        const z = clampZoom(newZoom)
        const fx = img.width > 0 ? (vx - img.x) / img.width : 0.5
        const fy = img.height > 0 ? (vy - img.y) / img.height : 0.5
        zoom = z
        img.x = vx - fx * img.width
        img.y = vy - fy * img.height
        clampPan()
        atFit = Math.abs(zoom - fitScale) < 0.001
    }
    function zoomAtCenter(newZoom) { zoomAt(newZoom, stage.width / 2, stage.height / 2) }

    onCurrentSourceChanged: { natW = 0; natH = 0; zoom = fitScale; atFit = true }

    // --- Image stage ------------------------------------------------------
    Item {
        id: stage
        anchors.fill: parent
        clip: true
        visible: !root.currentIsVideo

        Image {
            id: img
            source: root.currentIsVideo ? "" : root.currentSource
            width: root.natW * root.zoom
            height: root.natH * root.zoom
            fillMode: Image.Stretch // exact aspect: width/height derive from natW/natH
            autoTransform: true     // honour EXIF orientation, like the thumbnails
            asynchronous: true
            cache: true
            smooth: true
            mipmap: true
            onStatusChanged: {
                if (status === Image.Ready) {
                    root.natW = implicitWidth
                    root.natH = implicitHeight
                    root.fit()
                }
            }
        }

        MouseArea {
            id: panArea
            anchors.fill: parent
            enabled: root.zoomable
            cursorShape: (img.width > stage.width || img.height > stage.height)
                         ? Qt.OpenHandCursor : Qt.ArrowCursor
            property real startX: 0
            property real startY: 0
            property real imgStartX: 0
            property real imgStartY: 0
            onPressed: (m) => { startX = m.x; startY = m.y; imgStartX = img.x; imgStartY = img.y }
            onPositionChanged: (m) => {
                img.x = imgStartX + (m.x - startX)
                img.y = imgStartY + (m.y - startY)
                root.clampPan()
            }
            onDoubleClicked: {
                if (Math.abs(root.zoom - root.fitScale) < 0.001)
                    root.zoomAt(1.0, mouseX, mouseY) // fit -> 1:1 under cursor
                else
                    root.fit()                        // anything -> fit
            }
            onWheel: (wheel) => {
                const factor = wheel.angleDelta.y > 0 ? 1.15 : (1 / 1.15)
                root.zoomAt(root.zoom * factor, wheel.x, wheel.y)
            }
        }

        BusyIndicator {
            anchors.centerIn: parent
            running: img.status === Image.Loading
        }
    }

    // --- Video stage ------------------------------------------------------
    VideoOutput {
        id: videoOut
        anchors.fill: parent
        anchors.margins: 12
        fillMode: VideoOutput.PreserveAspectFit
        visible: root.currentIsVideo
    }
    MediaPlayer {
        id: player
        source: root.currentIsVideo ? root.currentSource : ""
        videoOutput: videoOut
        audioOutput: AudioOutput { muted: root.muted }
        onSourceChanged: if (root.currentIsVideo) play()
    }

    // Filename overlay (toggle with N).
    Rectangle {
        visible: root.showName && root.currentName !== ""
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.margins: 16
        radius: 6
        color: Qt.rgba(0, 0, 0, 0.6)
        width: Math.min(nameLabel.implicitWidth + 24, root.width - 32)
        height: nameLabel.implicitHeight + 12
        Label {
            id: nameLabel
            anchors.centerIn: parent
            width: parent.width - 24
            text: root.currentName + (root.count > 1 ? ("   " + (root.index + 1) + " / " + root.count) : "")
            color: "white"
            elide: Text.ElideMiddle
            horizontalAlignment: Text.AlignHCenter
        }
    }

    // Zoom readout, shown briefly while not at fit.
    Rectangle {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 16
        radius: 6
        color: Qt.rgba(0, 0, 0, 0.6)
        visible: root.zoomable && !root.atFit
        width: zoomLabel.implicitWidth + 20
        height: zoomLabel.implicitHeight + 10
        Label {
            id: zoomLabel
            anchors.centerIn: parent
            color: "white"
            text: Math.round(root.zoom * 100) + "%"
        }
    }

    Menu {
        id: contextMenu
        MenuItem { text: qsTr("Fit");        onTriggered: root.fit() }
        MenuItem { text: qsTr("Actual size (1:1)"); onTriggered: root.oneToOne() }
        MenuSeparator {}
        MenuItem { text: qsTr("Start slideshow"); onTriggered: root.slideshowRequested() }
        MenuItem {
            text: root.showName ? qsTr("Hide filename") : qsTr("Show filename")
            onTriggered: root.showName = !root.showName
        }
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.RightButton
        onClicked: contextMenu.popup()
        z: -1 // sit below the pan area; only unhandled right-clicks reach here
    }

    Item {
        id: keyCatcher
        anchors.fill: parent
        focus: true
        Keys.onEscapePressed: root.closeRequested()
        Keys.onLeftPressed: root.prev()
        Keys.onRightPressed: root.next()
        Keys.onPressed: (event) => {
            switch (event.key) {
            case Qt.Key_Plus:
            case Qt.Key_Equal:
                if (root.zoomable) root.zoomAtCenter(root.zoom * 1.25)
                event.accepted = true; break
            case Qt.Key_Minus:
                if (root.zoomable) root.zoomAtCenter(root.zoom / 1.25)
                event.accepted = true; break
            case Qt.Key_0:
            case Qt.Key_F:
                root.fit(); event.accepted = true; break
            case Qt.Key_1:
                if (root.zoomable) root.oneToOne()
                event.accepted = true; break
            case Qt.Key_N:
                root.showName = !root.showName; event.accepted = true; break
            case Qt.Key_S:
                root.slideshowRequested(); event.accepted = true; break
            case Qt.Key_Space:
                if (root.currentIsVideo)
                    player.playbackState === MediaPlayer.PlayingState ? player.pause() : player.play()
                event.accepted = true; break
            case Qt.Key_M:
                if (root.currentIsVideo) root.muted = !root.muted
                event.accepted = true; break
            }
        }
    }

    // Re-fit on resize while the image is shown fit-to-window, so a resize keeps
    // the whole image visible; a zoomed-in view keeps its scale and just re-clamps.
    onWidthChanged: atFit ? fit() : clampPan()
    onHeightChanged: atFit ? fit() : clampPan()

    onVisibleChanged: if (visible) keyCatcher.forceActiveFocus()
}
