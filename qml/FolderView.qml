// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls
import Omapic

// Folder browser: one photo-style grid holding the navigable folders (an Up
// entry plus subfolders, each a 2x2 cover mosaic) followed by the photos in the
// current folder. The folder entries come from Library.folderEntries(); the
// photos come from library.filtered (restricted to the folder) and are addressed
// by proxy row, exactly like the main grid — so selection/slideshow are shared.
Item {
    id: fv

    property var library
    property var entries: []
    property string currentFolder: ""
    property color accent: "#4a90d9"
    property bool crop: true
    property int cellSize: 184
    property var selectedIds: []

    // Folder cells occupy the first `folderCount` grid indices; the rest are
    // photos at proxy row (index - folderCount).
    readonly property int folderCount: entries.length

    signal navigate(string path) // a folder/Up cell was clicked
    signal photoClicked(int index, int id, int modifiers)
    signal photoDoubleClicked(int id)
    signal photoRightClicked(int index, int id, string fileName, bool isVideo)
    signal selectAllRequested()

    function toTop() { if (fgrid) fgrid.positionViewAtBeginning() }
    function focusView() { fgrid.forceActiveFocus() }

    Column {
        anchors.fill: parent
        spacing: 0

        // Breadcrumb for context (the folder cells handle navigation).
        Rectangle {
            width: parent.width
            height: crumb.implicitHeight + 12
            color: palette.alternateBase
            Label {
                id: crumb
                anchors.verticalCenter: parent.verticalCenter
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                text: fv.currentFolder === "" ? qsTr("Library") : fv.currentFolder
                elide: Text.ElideMiddle
                font.pixelSize: 12
                color: palette.placeholderText
            }
        }

        GridView {
            id: fgrid
            width: parent.width
            height: parent.height - y
            clip: true
            cellWidth: fv.cellSize
            cellHeight: fv.cellSize
            boundsBehavior: Flickable.StopAtBounds
            reuseItems: true
            cacheBuffer: 1200
            flickDeceleration: 2500
            maximumFlickVelocity: 6000
            ScrollBar.vertical: ScrollBar {}

            model: fv.folderCount + (fv.library ? fv.library.filtered.count : 0)

            focus: true
            keyNavigationEnabled: false
            Keys.onPressed: (event) => {
                if (event.key === Qt.Key_A && (event.modifiers & Qt.ControlModifier)) {
                    fv.selectAllRequested()
                    event.accepted = true
                }
            }

            delegate: Item {
                id: cell
                required property int index
                width: fgrid.cellWidth
                height: fgrid.cellHeight

                readonly property bool isFolder: index < fv.folderCount
                readonly property var entry: isFolder ? fv.entries[index] : null
                readonly property int proxyRow: index - fv.folderCount
                readonly property var photo: isFolder ? null
                                                      : (fv.library.filtered.photoAt(proxyRow) || null)

                Loader {
                    anchors.fill: parent
                    sourceComponent: cell.isFolder ? folderComp : photoComp
                }

                Component {
                    id: folderComp
                    FolderMosaicCell {
                        entry: cell.entry
                        accent: fv.accent
                        onClicked: { if (cell.entry) fv.navigate(cell.entry.path) }
                    }
                }
                Component {
                    id: photoComp
                    PhotoDelegate {
                        accent: fv.accent
                        crop: fv.crop
                        filePath: cell.photo ? cell.photo.filePath : ""
                        isVideo: cell.photo ? (cell.photo.isVideo === true) : false
                        tagCount: cell.photo ? cell.photo.tagCount : 0
                        selected: cell.photo ? (fv.selectedIds.indexOf(cell.photo.id) !== -1) : false
                        onClicked: (modifiers) => { if (cell.photo) fv.photoClicked(cell.proxyRow, cell.photo.id, modifiers) }
                        onDoubleClicked: { if (cell.photo) fv.photoDoubleClicked(cell.photo.id) }
                        onRightClicked: {
                            if (cell.photo)
                                fv.photoRightClicked(cell.proxyRow, cell.photo.id, cell.photo.fileName, cell.photo.isVideo === true)
                        }
                    }
                }
            }

            // Match the grid's wheel/trackpad feel.
            NumberAnimation {
                id: wheelAnim
                target: fgrid
                property: "contentY"
                duration: 220
                easing.type: Easing.OutCubic
            }
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.NoButton
                propagateComposedEvents: true
                onWheel: (wheel) => {
                    const minY = fgrid.originY
                    const maxY = Math.max(minY, fgrid.originY + fgrid.contentHeight - fgrid.height)
                    if (wheel.pixelDelta.y !== 0) {
                        wheelAnim.stop()
                        fgrid.contentY = Math.max(minY, Math.min(maxY, fgrid.contentY - wheel.pixelDelta.y * 4.0))
                    } else {
                        const step = (wheel.angleDelta.y / 120) * fgrid.cellHeight
                        const target = Math.max(minY, Math.min(maxY, fgrid.contentY - step))
                        wheelAnim.stop()
                        wheelAnim.to = target
                        wheelAnim.start()
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                visible: fgrid.count === 0
                text: qsTr("This folder has no photos.")
                color: palette.placeholderText
            }
        }
    }
}
