// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Window
import QtQuick.Controls

Window {
    id: root

    property var sources: []
    property int index: 0
    property int intervalMs: 4000
    property int transitionMs: 280
    property bool playing: true
    property bool shuffle: false

    // Which of the two buffers is currently shown, and which (if any) is loading.
    property int front: 0
    property int loadingInto: -1
    property bool showName: false

    readonly property int count: sources.length
    readonly property string currentSource: (index >= 0 && index < count) ? sources[index] : ""
    readonly property string currentName: currentSource === ""
        ? "" : decodeURIComponent(currentSource.substring(currentSource.lastIndexOf('/') + 1))

    signal toggleShuffleRequested()

    // The paused controls hint auto-hides after a few seconds, and reappears on
    // activity (mouse move or navigation) so it isn't gone for the whole pause.
    property bool hintShown: true

    Timer {
        id: hintHideTimer
        interval: 3000
        onTriggered: root.hintShown = false
    }

    function pokeHint() {
        hintShown = true
        if (!playing)
            hintHideTimer.restart()
        else
            hintHideTimer.stop()
    }

    onPlayingChanged: pokeHint()

    width: 960
    height: 640
    color: "black"
    visibility: Window.Hidden
    title: qsTr("Omapic — slideshow")

    function clampIndex() {
        if (count === 0) { index = 0; return }
        if (index < 0) index = 0
        if (index >= count) index = count - 1
    }

    function start() { startAt(0, true) }

    function startAt(i, play) {
        index = i
        clampIndex()
        if (count === 0)
            return
        front = 0
        loadingInto = -1
        img0.source = currentSource
        img1.source = ""
        playing = (play !== false) // default to running when unspecified
        visibility = Window.FullScreen
        keyCatcher.forceActiveFocus()
    }

    function stop() {
        playing = false
        visibility = Window.Hidden
    }

    // Swap in a new playlist without restarting; the shown image keeps showing.
    function setSources(list, i) {
        sources = list
        index = Math.max(0, Math.min(list.length - 1, i))
    }

    // Load target index into the back buffer; it crossfades in once Ready.
    function go(i) {
        if (count === 0)
            return
        index = (i % count + count) % count
        const back = (front === 0) ? 1 : 0
        const backImg = (back === 0) ? img0 : img1
        loadingInto = back
        if (String(backImg.source) === currentSource && backImg.status === Image.Ready) {
            // The back buffer already holds this image (e.g. navigating back, a
            // wrap, or a short playlist). Re-assigning the same source fires no
            // status change, so flip now instead of waiting for a Ready that
            // never comes.
            front = back
            loadingInto = -1
        } else {
            backImg.source = currentSource
        }
    }

    function next() { go(index + 1) }
    function prev() { go(index - 1) }

    // Double-buffered crossfade stage.
    Item {
        anchors.fill: parent
        anchors.margins: 24

        Image {
            id: img0
            anchors.fill: parent
            fillMode: Image.PreserveAspectFit
            asynchronous: true
            cache: true
            opacity: root.front === 0 ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: root.transitionMs; easing.type: Easing.InOutQuad } }
            onStatusChanged: {
                if (status === Image.Ready && root.loadingInto === 0) {
                    root.front = 0
                    root.loadingInto = -1
                }
            }
        }
        Image {
            id: img1
            anchors.fill: parent
            fillMode: Image.PreserveAspectFit
            asynchronous: true
            cache: true
            opacity: root.front === 1 ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: root.transitionMs; easing.type: Easing.InOutQuad } }
            onStatusChanged: {
                if (status === Image.Ready && root.loadingInto === 1) {
                    root.front = 1
                    root.loadingInto = -1
                }
            }
        }
    }

    Timer {
        interval: root.intervalMs
        repeat: true
        running: root.visible && root.playing && root.count > 1
        onTriggered: root.next()
    }

    // Transient toast when shuffle is toggled.
    Rectangle {
        id: toast
        anchors.top: parent.top
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.topMargin: 32
        width: toastLabel.implicitWidth + 28
        height: toastLabel.implicitHeight + 14
        radius: 6
        color: Qt.rgba(0, 0, 0, 0.65)
        opacity: 0

        Label {
            id: toastLabel
            anchors.centerIn: parent
            color: "white"
            text: root.shuffle ? qsTr("Shuffle: On") : qsTr("Shuffle: Off")
        }

        function flash() { toastAnim.restart() }

        SequentialAnimation {
            id: toastAnim
            NumberAnimation { target: toast; property: "opacity"; to: 1; duration: 150 }
            PauseAnimation { duration: 900 }
            NumberAnimation { target: toast; property: "opacity"; to: 0; duration: 400 }
        }
    }

    onShuffleChanged: toast.flash()

    // Filename overlay (toggled with N).
    Rectangle {
        id: namePlate
        visible: root.showName && root.currentName !== ""
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.margins: 24
        radius: 6
        color: Qt.rgba(0, 0, 0, 0.6)
        width: Math.min(nameLabel.implicitWidth + 24, root.width - 48)
        height: nameLabel.implicitHeight + 12

        Label {
            id: nameLabel
            anchors.centerIn: parent
            width: namePlate.width - 24
            text: root.currentName
            color: "white"
            elide: Text.ElideMiddle
            horizontalAlignment: Text.AlignHCenter
        }
    }

    // Pause / help hint.
    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 24
        width: hint.implicitWidth + 24
        height: hint.implicitHeight + 12
        radius: 6
        color: Qt.rgba(0, 0, 0, 0.6)
        opacity: (!root.playing && root.hintShown) ? 1.0 : 0.0
        Behavior on opacity { NumberAnimation { duration: 300 } }

        Label {
            id: hint
            anchors.centerIn: parent
            color: "white"
            text: qsTr("Paused  ·  %1 / %2  ·  Space: play/pause   ←/→: prev/next   S: shuffle   N: filename   Esc: exit")
                  .arg(root.index + 1).arg(root.count)
        }
    }

    // Mouse movement counts as activity: bring the paused hint back.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.NoButton
        hoverEnabled: true
        onPositionChanged: root.pokeHint()
    }

    Item {
        id: keyCatcher
        anchors.fill: parent
        focus: true
        Keys.onEscapePressed: root.stop()
        Keys.onLeftPressed: { root.prev(); root.pokeHint() }
        Keys.onRightPressed: { root.next(); root.pokeHint() }
        Keys.onSpacePressed: root.playing = !root.playing
        Keys.onPressed: (event) => {
            if (event.key === Qt.Key_S) {
                root.toggleShuffleRequested()
                root.pokeHint()
                event.accepted = true
            } else if (event.key === Qt.Key_N) {
                root.showName = !root.showName
                root.pokeHint()
                event.accepted = true
            }
        }
    }

    onVisibleChanged: if (visible) keyCatcher.forceActiveFocus()
}
