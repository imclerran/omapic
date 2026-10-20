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

    // The control bar auto-hides when the pointer is still, like the slideshow's
    // hint. Shown on any pointer movement, hidden after a quiet spell, and kept up
    // while the pointer is over the bar so it never vanishes mid-click.
    property bool controlsShown: true
    function pokeControls() {
        controlsShown = true
        controlsTimer.restart()
    }

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
        pokeControls()
        applyMedia() // (re)start the video even if it's the same one as last time
    }

    function go(i) {
        if (count === 0)
            return
        // Double-buffer the advance: paint the outgoing photo into the hold layer
        // (it's cached, so it shows at once) so the gap while the next one decodes
        // shows the previous photo instead of flashing to black.
        holdImg.source = (!currentIsVideo && currentSource !== "") ? currentSource : ""
        index = (i % count + count) % count
        applyMedia() // switch to the player for a video, or stop it for an image
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

        // Hold layer: shows the outgoing photo (fit, from cache) behind the live
        // image while the next one decodes, so advancing never flashes to black.
        // Cleared once the live image is ready — see img.onStatusChanged.
        Image {
            id: holdImg
            anchors.fill: parent
            fillMode: Image.PreserveAspectFit
            autoTransform: true
            cache: true
            z: -1
        }

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
                    holdImg.source = "" // live image is up; drop the held frame
                }
            }
        }

        MouseArea {
            id: panArea
            anchors.fill: parent
            enabled: root.zoomable
            cursorShape: !root.controlsShown ? Qt.BlankCursor
                         : (img.width > stage.width || img.height > stage.height)
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
                // Scale the zoom step by the actual wheel delta rather than a fixed
                // factor per event: a mouse notch is one event of 120 (→ 1.15x, the
                // old feel), while a trackpad streams many small deltas per gesture —
                // with a fixed factor each of those zoomed a full step, which made
                // trackpad zoom run away. angleDelta is used when present; some
                // trackpads report only high-res pixels, so fall back to those.
                let notches = wheel.angleDelta.y / 120
                if (notches === 0 && wheel.pixelDelta.y !== 0)
                    notches = wheel.pixelDelta.y / 120
                if (notches === 0)
                    return
                root.zoomAt(root.zoom * Math.pow(1.15, notches), wheel.x, wheel.y)
            }
        }

        BusyIndicator {
            anchors.centerIn: parent
            running: img.status === Image.Loading
        }

        // Error overlay: an image that failed to decode — a corrupt file, or (most
        // often) a HEIC/HEIF/AVIF when the matching image plugin isn't installed.
        Column {
            anchors.centerIn: parent
            spacing: 8
            width: Math.min(parent.width - 48, 420)
            visible: img.status === Image.Error && !root.currentIsVideo
            Label {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("This image couldn't be displayed.")
                color: "white"
                font.pixelSize: 16
            }
            Label {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                // A plugin-backed format (HEIC/HEIF or AVIF) whose plugin is absent.
                visible: (!heifSupported && /\.(heic|heif)$/i.test(String(root.currentSource)))
                         || (!avifSupported && /\.avif$/i.test(String(root.currentSource)))
                text: qsTr("This format needs a Qt image plugin "
                           + "(on Arch: kimageformats, plus libheif/libavif).")
                color: "#cccccc"
                font.pixelSize: 13
            }
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
        videoOutput: videoOut
        audioOutput: AudioOutput { muted: root.muted }
        loops: MediaPlayer.Infinite // loop videos while the viewer is open
    }
    // Drive the player imperatively (like SlideshowView) rather than binding
    // MediaPlayer.source. Called from openAt()/go() on every open and navigation —
    // not just when currentSource changes — so re-opening the same video still
    // plays. Clearing the source first forces a reload from the start even when
    // the URL is unchanged (re-opening, or replaying after it reached the end);
    // otherwise MediaPlayer ignores the no-op assignment and shows a black frame.
    function applyMedia() {
        player.stop()
        player.source = ""
        if (root.currentIsVideo) {
            player.source = root.currentSource
            player.play()
        }
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

    // Hide the controls after a quiet spell (same dwell as the slideshow hint);
    // stay up while the pointer is over the bar.
    Timer {
        id: controlsTimer
        interval: 3000
        onTriggered: if (!barHover.hovered) root.controlsShown = false
    }
    // Any pointer movement over the window brings the controls back. A passive
    // handler, so it never steals clicks/wheel/hover from the image or the bar.
    // Only a real change in cursor position counts: toggling the bar's visibility
    // makes Qt re-deliver a hover event at the same spot, and reacting to that
    // would re-show the bar the instant it hid (a flicker that never settles).
    HoverHandler {
        property real lastX: -1
        property real lastY: -1
        onPointChanged: {
            if (point.position.x !== lastX || point.position.y !== lastY) {
                lastX = point.position.x
                lastY = point.position.y
                root.pokeControls()
            }
        }
    }

    // Auto-hiding control bar: navigation, zoom (fit / 1:1 / ± with a readout),
    // slideshow, and close — mirroring the keyboard shortcuts.
    Rectangle {
        id: controlBar
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 24
        radius: 8
        color: Qt.rgba(0, 0, 0, 0.72)
        opacity: root.controlsShown ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 200 } }
        implicitWidth: barRow.implicitWidth + 20
        implicitHeight: barRow.implicitHeight + 12
        width: implicitWidth
        height: implicitHeight

        // Keep the bar up while the pointer rests on it; restart the dwell on exit.
        HoverHandler {
            id: barHover
            onHoveredChanged: hovered ? root.pokeControls() : controlsTimer.restart()
        }

        // Flat, white-on-dark button that never steals keyboard focus from the
        // key handler (so the arrow keys keep working after a click).
        component Ctl: ToolButton {
            id: ctl
            flat: true
            focusPolicy: Qt.NoFocus
            implicitHeight: 32
            padding: 8
            contentItem: Label {
                text: ctl.text
                color: ctl.enabled ? "white" : Qt.rgba(1, 1, 1, 0.35)
                font.pixelSize: 13
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                radius: 5
                color: ctl.down ? Qt.rgba(1, 1, 1, 0.25)
                     : ctl.hovered ? Qt.rgba(1, 1, 1, 0.14) : "transparent"
            }
        }

        Row {
            id: barRow
            anchors.centerIn: parent
            spacing: 2

            Ctl {
                text: "‹"
                enabled: root.count > 1
                ToolTip.visible: hovered; ToolTip.text: qsTr("Previous  (←)")
                onClicked: { root.prev(); root.pokeControls() }
            }
            Ctl {
                text: "›"
                enabled: root.count > 1
                ToolTip.visible: hovered; ToolTip.text: qsTr("Next  (→)")
                onClicked: { root.next(); root.pokeControls() }
            }

            Rectangle { width: 1; height: 22; anchors.verticalCenter: parent.verticalCenter; color: Qt.rgba(1, 1, 1, 0.18) }

            Ctl {
                text: "−"
                enabled: root.zoomable
                ToolTip.visible: hovered; ToolTip.text: qsTr("Zoom out  (-)")
                onClicked: { root.zoomAtCenter(root.zoom / 1.25); root.pokeControls() }
            }
            Label {
                anchors.verticalCenter: parent.verticalCenter
                width: 46
                horizontalAlignment: Text.AlignHCenter
                text: root.zoomable ? Math.round(root.zoom * 100) + "%" : "—"
                color: "white"
                font.pixelSize: 12
            }
            Ctl {
                text: "+"
                enabled: root.zoomable
                ToolTip.visible: hovered; ToolTip.text: qsTr("Zoom in  (+)")
                onClicked: { root.zoomAtCenter(root.zoom * 1.25); root.pokeControls() }
            }
            Ctl {
                text: qsTr("Fit")
                enabled: root.zoomable && !root.atFit
                ToolTip.visible: hovered; ToolTip.text: qsTr("Fit to window  (F)")
                onClicked: { root.fit(); root.pokeControls() }
            }
            Ctl {
                text: qsTr("1:1")
                enabled: root.zoomable
                ToolTip.visible: hovered; ToolTip.text: qsTr("Actual size  (1)")
                onClicked: { root.oneToOne(); root.pokeControls() }
            }

            Rectangle { width: 1; height: 22; anchors.verticalCenter: parent.verticalCenter; color: Qt.rgba(1, 1, 1, 0.18) }

            Ctl {
                text: "▶"
                ToolTip.visible: hovered; ToolTip.text: qsTr("Start slideshow  (S)")
                onClicked: root.slideshowRequested()
            }
            Ctl {
                text: "✕"
                ToolTip.visible: hovered; ToolTip.text: qsTr("Close  (Esc)")
                onClicked: root.closeRequested()
            }
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
        Keys.onLeftPressed: { root.prev(); root.pokeControls() }
        Keys.onRightPressed: { root.next(); root.pokeControls() }
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

    onVisibleChanged: if (visible) { keyCatcher.forceActiveFocus(); pokeControls() }
}
