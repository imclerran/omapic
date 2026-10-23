// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Window
import QtQuick.Controls
import QtMultimedia

// Triage mode's central view: the current photo, fit to the area, on the window
// background. The image and video stages mirror PhotoViewer's (EXIF-aware fit,
// a hold layer so advancing never flashes blank, looping muted video, the
// missing-plugin notice) without its zoom/pan — triage only ever shows the whole
// photo. A small line in the bottom-left corner gives the dimensions, size and
// capture date. Shows a "done" message once the queue has been walked.
Rectangle {
    id: root

    property var library
    // The photo being triaged (-1 once the queue is exhausted).
    property int photoId: -1
    // How many photos the run covered, for the done message.
    property int total: 0

    // Display data for the current photo, read from the library once per photo
    // (the metadata comes from the file on disk, so not on every tag change).
    property var info: ({})
    readonly property string source: info.url || ""
    readonly property bool isVideo: info.isVideo === true

    SystemPalette { id: sys }
    color: sys.base

    function refresh() {
        info = (root.photoId >= 0 && root.library) ? root.library.photoInfo(root.photoId) : ({})
    }
    onPhotoIdChanged: {
        // Keep the outgoing photo on screen (it's cached) while the next decodes,
        // so advancing never flashes to the bare background.
        holdImg.source = (!root.isVideo && img.status === Image.Ready) ? root.source : ""
        refresh()
    }
    Component.onCompleted: refresh()

    // Drive the player imperatively, as PhotoViewer does: assigning the same URL
    // twice is a no-op for MediaPlayer, so clear it first to restart playback.
    onSourceChanged: {
        player.stop()
        player.source = ""
        if (root.isVideo && root.source !== "") {
            player.source = root.source
            player.play()
        }
    }
    // Pause when triage is hidden, so a video doesn't keep playing underneath.
    onVisibleChanged: if (!visible) player.stop()

    // --- Image stage ----------------------------------------------------------
    Item {
        id: stage
        anchors.fill: parent
        anchors.margins: 12
        visible: !root.isVideo && root.photoId >= 0

        // Hold layer: the previous photo (cached) stays up while the next decodes.
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
            anchors.fill: parent
            source: root.isVideo ? "" : root.source
            fillMode: Image.PreserveAspectFit
            autoTransform: true // honour EXIF orientation, like the thumbnails
            asynchronous: true
            cache: true
            smooth: true
            mipmap: true
            onStatusChanged: if (status === Image.Ready) holdImg.source = "" // live image is up
        }

        BusyIndicator {
            anchors.centerIn: parent
            running: img.status === Image.Loading && String(holdImg.source) === ""
        }

        // Error overlay: a corrupt file, or a HEIC/AVIF without its image plugin.
        Column {
            anchors.centerIn: parent
            spacing: 8
            width: Math.min(parent.width - 48, 420)
            visible: img.status === Image.Error
            Label {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("This image couldn't be displayed.")
                font.pixelSize: 16
            }
            Label {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                visible: (!heifSupported && /\.(heic|heif)$/i.test(root.source))
                         || (!avifSupported && /\.avif$/i.test(root.source))
                text: qsTr("This format needs a Qt image plugin "
                           + "(on Arch: kimageformats, plus libheif/libavif).")
                color: sys.placeholderText
                font.pixelSize: 13
            }
        }
    }

    // --- Video stage ----------------------------------------------------------
    VideoOutput {
        id: videoOut
        anchors.fill: parent
        anchors.margins: 12
        fillMode: VideoOutput.PreserveAspectFit
        visible: root.isVideo && root.photoId >= 0
    }
    MediaPlayer {
        id: player
        videoOutput: videoOut
        audioOutput: AudioOutput { muted: true }
        loops: MediaPlayer.Infinite
    }

    // File name, then dimensions · size · taken, bottom-left. Each part hides
    // when unknown; the box hides when there's nothing to show.
    Rectangle {
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.margins: 12
        visible: root.photoId >= 0 && (nameLabel.text !== "" || metaLabel.text !== "")
        radius: 4
        color: Qt.alpha(sys.window, 0.85)
        width: Math.max(nameLabel.implicitWidth, metaLabel.implicitWidth) + 16
        height: metaColumn.implicitHeight + 8
        Column {
            id: metaColumn
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left
            anchors.leftMargin: 8
            spacing: 1
            Label {
                id: nameLabel
                font.pixelSize: 12
                color: sys.text
                text: root.info.fileName || ""
                visible: text !== ""
            }
            Label {
                id: metaLabel
                font.pixelSize: 11
                color: sys.placeholderText
                text: [root.info.dimensions || "", root.info.size || "", root.info.taken || ""]
                          .filter(function (s) { return s !== "" }).join("  ·  ")
                visible: text !== ""
            }
        }
    }

    // Finished: every queued photo has been stepped past.
    Column {
        anchors.centerIn: parent
        spacing: 8
        visible: root.photoId < 0
        Label {
            anchors.horizontalCenter: parent.horizontalCenter
            text: qsTr("All done")
            font.pixelSize: 20
            font.bold: true
        }
        Label {
            anchors.horizontalCenter: parent.horizontalCenter
            text: root.total > 0
                  ? qsTr("You've been through all %n photo(s). Enter or Esc leaves triage; Ctrl+← steps back.", "", root.total)
                  : qsTr("Nothing to triage here. Enter or Esc leaves triage.")
            color: sys.placeholderText
        }
    }
}
