// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Window
import QtQuick.Controls
import QtMultimedia

Window {
    id: root

    property var sources: []
    property int index: 0
    property int intervalMs: 4000
    property int transitionMs: 280
    property bool playing: true
    property bool shuffle: false
    // Loop the current video (persisted; bound from Main). When on, a video
    // repeats and only the arrow keys move to the next item.
    property bool loop: false
    // Video audio starts muted; toggled from the context menu / M key.
    property bool muted: true

    // Which of the two buffers is currently shown, and which (if any) is loading.
    property int front: 0
    property int loadingInto: -1
    property bool showName: false
    // True while the video layer (rather than the image buffers) is on screen.
    property bool showingVideo: false

    readonly property int count: sources.length
    readonly property string currentSource: (index >= 0 && index < count) ? sources[index] : ""
    readonly property string currentName: currentSource === ""
        ? "" : decodeURIComponent(currentSource.substring(currentSource.lastIndexOf('/') + 1))
    readonly property bool currentIsVideo: isVideoSource(currentSource)

    // Keep this pattern in sync with the scanner globs and Photo::isVideo.
    function isVideoSource(s) {
        return /\.(mp4|m4v|webm|mkv|mov|avi)$/i.test(String(s))
    }

    signal toggleShuffleRequested()
    signal toggleLoopRequested()
    // Theme/wallpaper actions on the current photo. Handled in Main.qml (which
    // owns the Library and reports the result via flashMessage). `source` is a
    // file:// URL.
    signal addWallpaperRequested(url source)
    signal editInAetherRequested(url source)
    signal setAetherThemeRequested(url source)
    signal setAsWallpaperRequested(url source)

    // Which desktop integrations are available; set from Main.qml's Library so the
    // context menu shows only the actions that can work (see photoMenu there).
    property bool omarchyAvailable: false
    property bool aetherAvailable: false
    property bool wallpaperAvailable: false

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

    onPlayingChanged: {
        pokeHint()
        if (showingVideo) {
            if (playing)
                player.play()
            else
                player.pause()
        }
    }

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
        img1.source = ""
        playing = (play !== false) // default to running when unspecified
        if (currentIsVideo) {
            img0.source = ""
            loadVideo()
        } else {
            showingVideo = false
            player.source = ""
            img0.source = currentSource
        }
        visibility = Window.FullScreen
        keyCatcher.forceActiveFocus()
    }

    function stop() {
        playing = false
        player.stop()
        player.source = ""
        visibility = Window.Hidden
    }

    // Swap in a new playlist without restarting; the shown item keeps showing.
    function setSources(list, i) {
        sources = list
        index = Math.max(0, Math.min(list.length - 1, i))
    }

    // Point the player at the current video and fade the video layer in. The
    // first frame shows even when opening paused.
    function loadVideo() {
        showingVideo = true
        player.source = currentSource
        player.play()
        if (!playing)
            player.pause()
    }

    // Advance/navigate to index i. Videos switch to the player; images load
    // into the back buffer and crossfade in once Ready.
    function go(i) {
        if (count === 0)
            return
        index = (i % count + count) % count
        if (currentIsVideo) {
            loadVideo()
            return
        }
        // Target is an image: silence any outgoing video right away, then load
        // into the back buffer. showingVideo stays true (if it was) until the
        // image is Ready, so the video→image switch crossfades cleanly.
        if (showingVideo)
            player.pause()
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
            showingVideo = false
            player.stop()
            player.source = ""
        } else {
            backImg.source = currentSource
        }
    }

    function next() { go(index + 1) }
    function prev() { go(index - 1) }

    // Manually advancing (arrow keys) resets the slide timer so the new photo
    // gets its full dwell time instead of whatever was left on the clock.
    function restartSlideTimer() {
        if (!slideTimer.running)
            return
        slideTimer.restart()
        // restart() assigns `running` imperatively, dropping its binding;
        // restore it so pause / videos / exit still stop the timer declaratively.
        slideTimer.running = Qt.binding(function () {
            return root.visible && root.playing && root.count > 1 && !root.showingVideo
        })
    }

    // When an image finishes loading into the back buffer, flip it to the front
    // and (if we were showing a video) crossfade the video layer out.
    function imageReady(buffer) {
        if (root.loadingInto !== buffer)
            return
        root.front = buffer
        root.loadingInto = -1
        if (root.showingVideo) {
            root.showingVideo = false
            player.stop()
            player.source = ""
        }
    }

    // Double-buffered crossfade stage for images.
    Item {
        anchors.fill: parent
        anchors.margins: 24
        opacity: root.showingVideo ? 0 : 1
        Behavior on opacity { NumberAnimation { duration: root.transitionMs; easing.type: Easing.InOutQuad } }

        Image {
            id: img0
            anchors.fill: parent
            fillMode: Image.PreserveAspectFit
            autoTransform: true // honour EXIF orientation, like the thumbnails
            asynchronous: true
            cache: true
            opacity: root.front === 0 ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: root.transitionMs; easing.type: Easing.InOutQuad } }
            onStatusChanged: if (status === Image.Ready) root.imageReady(0)
        }
        Image {
            id: img1
            anchors.fill: parent
            fillMode: Image.PreserveAspectFit
            autoTransform: true // honour EXIF orientation, like the thumbnails
            asynchronous: true
            cache: true
            opacity: root.front === 1 ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: root.transitionMs; easing.type: Easing.InOutQuad } }
            onStatusChanged: if (status === Image.Ready) root.imageReady(1)
        }
    }

    // Video layer. Fades in over the image stage while a video is current.
    VideoOutput {
        id: videoOut
        anchors.fill: parent
        anchors.margins: 24
        fillMode: VideoOutput.PreserveAspectFit
        opacity: root.showingVideo ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: root.transitionMs; easing.type: Easing.InOutQuad } }
    }

    MediaPlayer {
        id: player
        videoOutput: videoOut
        loops: root.loop ? MediaPlayer.Infinite : 1
        audioOutput: AudioOutput { muted: root.muted }
        onMediaStatusChanged: {
            // A non-looping video that reaches its end advances like the slide
            // timer would — but only when there's somewhere else to go.
            if (mediaStatus === MediaPlayer.EndOfMedia
                    && !root.loop && root.playing && root.count > 1)
                root.next()
        }
    }

    // The slide timer governs images only; videos drive their own advance.
    Timer {
        id: slideTimer
        interval: root.intervalMs
        repeat: true
        running: root.visible && root.playing && root.count > 1 && !root.showingVideo
        onTriggered: root.next()
    }

    // Transient toast for shuffle toggles and context-menu action feedback. (The
    // main window's toast would be hidden behind this fullscreen window, so the
    // slideshow shows its own.)
    property string toastText: ""
    function flashMessage(msg) {
        toastText = msg
        toast.flash()
    }

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
            text: root.toastText
        }

        function flash() { toastAnim.restart() }

        SequentialAnimation {
            id: toastAnim
            NumberAnimation { target: toast; property: "opacity"; to: 1; duration: 150 }
            PauseAnimation { duration: 900 }
            NumberAnimation { target: toast; property: "opacity"; to: 0; duration: 400 }
        }
    }

    onShuffleChanged: flashMessage(shuffle ? qsTr("Shuffle: On") : qsTr("Shuffle: Off"))
    onLoopChanged: if (currentIsVideo) flashMessage(loop ? qsTr("Loop: On") : qsTr("Loop: Off"))
    onMutedChanged: if (currentIsVideo) flashMessage(muted ? qsTr("Muted") : qsTr("Unmuted"))

    // Filename overlay (toggled with N).
    Rectangle {
        id: namePlate
        visible: root.showName && root.currentName !== ""
        anchors.left: parent.left
        anchors.top: parent.top
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
            text: (root.currentIsVideo
                   ? qsTr("Paused  ·  %1 / %2  ·  Space: play/pause   ←/→: prev/next   S: shuffle   L: loop   M: mute   N: filename   Esc: exit")
                   : qsTr("Paused  ·  %1 / %2  ·  Space: play/pause   ←/→: prev/next   S: shuffle   N: filename   Esc: exit"))
                  .arg(root.index + 1).arg(root.count)
        }
    }

    // Mouse movement counts as activity: bring the paused hint back. Right-click
    // opens the context menu; left clicks fall through (no left-click action here).
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.RightButton
        hoverEnabled: true
        onPositionChanged: root.pokeHint()
        onClicked: (mouse) => {
            if (mouse.button === Qt.RightButton) {
                root.pokeHint()
                contextMenu.popup()
            }
        }
    }

    Menu {
        id: contextMenu

        MenuItem {
            text: root.playing ? qsTr("Pause") : qsTr("Play")
            onTriggered: root.playing = !root.playing
        }
        MenuItem {
            text: root.shuffle ? qsTr("Turn off shuffle") : qsTr("Turn on shuffle")
            onTriggered: { root.toggleShuffleRequested(); root.pokeHint() }
        }
        MenuItem {
            text: root.showName ? qsTr("Hide filename") : qsTr("Show filename")
            onTriggered: { root.showName = !root.showName; root.pokeHint() }
        }

        // Video-only controls.
        MenuSeparator { visible: root.currentIsVideo; height: visible ? implicitHeight : 0 }
        MenuItem {
            visible: root.currentIsVideo
            height: visible ? implicitHeight : 0
            text: root.loop ? qsTr("Turn off loop") : qsTr("Loop video")
            onTriggered: { root.toggleLoopRequested(); root.pokeHint() }
        }
        MenuItem {
            visible: root.currentIsVideo
            height: visible ? implicitHeight : 0
            text: root.muted ? qsTr("Unmute") : qsTr("Mute")
            onTriggered: { root.muted = !root.muted; root.pokeHint() }
        }

        // Theme/wallpaper actions apply to still images only, and only when the
        // matching integration is present (mirrors the grid's photo menu).
        MenuSeparator {
            visible: !root.currentIsVideo
                     && (root.omarchyAvailable || root.aetherAvailable || root.wallpaperAvailable)
            height: visible ? implicitHeight : 0
        }
        MenuItem {
            visible: !root.currentIsVideo && root.omarchyAvailable
            height: visible ? implicitHeight : 0
            text: qsTr("Add to theme wallpapers")
            enabled: root.currentSource !== ""
            onTriggered: root.addWallpaperRequested(root.currentSource)
        }
        MenuItem {
            visible: !root.currentIsVideo && root.aetherAvailable
            height: visible ? implicitHeight : 0
            text: qsTr("Edit theme in Aether")
            enabled: root.currentSource !== ""
            onTriggered: root.editInAetherRequested(root.currentSource)
        }
        MenuItem {
            visible: !root.currentIsVideo && root.aetherAvailable
            height: visible ? implicitHeight : 0
            text: qsTr("Set as Aether theme")
            enabled: root.currentSource !== ""
            onTriggered: root.setAetherThemeRequested(root.currentSource)
        }
        MenuItem {
            // Generic wallpaper setter for non-Aether desktops.
            visible: !root.currentIsVideo && !root.aetherAvailable && root.wallpaperAvailable
            height: visible ? implicitHeight : 0
            text: qsTr("Set as wallpaper")
            enabled: root.currentSource !== ""
            onTriggered: root.setAsWallpaperRequested(root.currentSource)
        }
    }

    Item {
        id: keyCatcher
        anchors.fill: parent
        focus: true
        Keys.onEscapePressed: root.stop()
        Keys.onLeftPressed: { root.prev(); root.pokeHint(); root.restartSlideTimer() }
        Keys.onRightPressed: { root.next(); root.pokeHint(); root.restartSlideTimer() }
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
            } else if (event.key === Qt.Key_L) {
                root.toggleLoopRequested()
                root.pokeHint()
                event.accepted = true
            } else if (event.key === Qt.Key_M) {
                if (root.currentIsVideo)
                    root.muted = !root.muted
                root.pokeHint()
                event.accepted = true
            }
        }
    }

    onVisibleChanged: if (visible) keyCatcher.forceActiveFocus()
}
