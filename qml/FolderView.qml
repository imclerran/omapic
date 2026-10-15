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
    property var selectedFolders: []

    // Folder cells occupy the first `folderCount` grid indices; the rest are
    // photos at proxy row (index - folderCount).
    readonly property int folderCount: entries.length

    // Bumped on any model dataChanged so photo cells (which read photoAt() once)
    // re-fetch — e.g. a tag edit updates the badge without a reload.
    property int dataRevision: 0
    Connections {
        target: fv.library ? fv.library.filtered : null
        function onDataChanged() { fv.dataRevision++ }
    }

    signal navigate(string path)             // go to a folder path (Up button / Alt+Up, or Enter on a folder)
    signal cellClicked(int gridIndex, int modifiers)
    signal cellDoubleClicked(int gridIndex)  // folder -> enter, photo -> slideshow
    signal cellRightClicked(int gridIndex)
    signal selectAllRequested()
    signal slideshowRequested(bool play)
    signal cursorMove(string dir, bool extend)  // arrow-key navigation over the combined grid

    // Columns currently shown, so arrow Up/Down can step a whole row.
    readonly property int columns: fgrid ? Math.max(1, Math.floor(fgrid.width / fgrid.cellWidth)) : 1

    function toTop() { if (fgrid) fgrid.positionViewAtBeginning() }
    function focusView() { fgrid.forceActiveFocus() }
    // Scroll so the given grid index (used by arrow navigation) stays on screen.
    function ensureVisible(index) { if (fgrid) fgrid.positionViewAtIndex(index, GridView.Contain) }

    // Enter/Return: open the selected folder (descend into it) when exactly one
    // folder and no photos are selected; otherwise start the slideshow as before.
    function activateSelection() {
        if (fv.selectedFolders.length === 1 && fv.selectedIds.length === 0)
            fv.navigate(fv.selectedFolders[0])
        else
            fv.slideshowRequested(false)
    }

    // "#rrggbb" for a color, for embedding in the Up button's SVG.
    function hex(c) {
        function h(v) { const s = Math.round(v * 255).toString(16); return s.length < 2 ? "0" + s : s }
        return "#" + h(c.r) + h(c.g) + h(c.b)
    }

    Column {
        anchors.fill: parent
        spacing: 0

        // Title bar: an Up/back button (hidden at the root) plus the current path.
        Rectangle {
            width: parent.width
            height: 36
            color: palette.alternateBase

            ToolButton {
                id: upButton
                anchors.right: parent.right
                anchors.rightMargin: 4
                anchors.verticalCenter: parent.verticalCenter
                width: 30
                height: 30
                visible: fv.currentFolder !== ""
                onClicked: { if (fv.library) fv.navigate(fv.library.upFolder(fv.currentFolder)) }
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Up")
                contentItem: Image {
                    anchors.centerIn: parent
                    source: "data:image/svg+xml," + encodeURIComponent(
                        "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'><path fill='"
                        + fv.hex(fv.accent)
                        + "' d='M12 3 L20 11 L15 11 L15 20 L9 20 L9 11 L4 11 Z'/></svg>")
                    sourceSize.width: 16
                    sourceSize.height: 16
                    smooth: true
                }
            }
            Label {
                id: crumb
                anchors.verticalCenter: parent.verticalCenter
                anchors.left: parent.left
                anchors.leftMargin: 10
                anchors.right: upButton.visible ? upButton.left : parent.right
                anchors.rightMargin: upButton.visible ? 6 : 10
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
            // Arrow keys move the selection cursor over the combined folder+photo
            // grid. Alt+Up is the "go up a folder" shortcut, so let it fall through.
            Keys.onLeftPressed: (event) => { fv.cursorMove("left", (event.modifiers & Qt.ShiftModifier) !== 0); event.accepted = true }
            Keys.onRightPressed: (event) => { fv.cursorMove("right", (event.modifiers & Qt.ShiftModifier) !== 0); event.accepted = true }
            Keys.onUpPressed: (event) => {
                if (event.modifiers & Qt.AltModifier) { event.accepted = false; return }
                fv.cursorMove("up", (event.modifiers & Qt.ShiftModifier) !== 0); event.accepted = true
            }
            Keys.onDownPressed: (event) => { fv.cursorMove("down", (event.modifiers & Qt.ShiftModifier) !== 0); event.accepted = true }
            Keys.onSpacePressed: (event) => { fv.slideshowRequested(true); event.accepted = true }
            Keys.onReturnPressed: (event) => { fv.activateSelection(); event.accepted = true }
            Keys.onEnterPressed: (event) => { fv.activateSelection(); event.accepted = true }

            delegate: Item {
                id: cell
                required property int index
                width: fgrid.cellWidth
                height: fgrid.cellHeight

                readonly property bool isFolder: index < fv.folderCount
                readonly property var entry: isFolder ? fv.entries[index] : null
                readonly property int proxyRow: index - fv.folderCount
                readonly property var photo: {
                    fv.dataRevision
                    return isFolder ? null : (fv.library.filtered.photoAt(proxyRow) || null)
                }

                Loader {
                    anchors.fill: parent
                    sourceComponent: cell.isFolder ? folderComp : photoComp
                }

                Component {
                    id: folderComp
                    FolderMosaicCell {
                        entry: cell.entry
                        accent: fv.accent
                        selected: cell.entry && fv.selectedFolders.indexOf(cell.entry.path) !== -1
                        // Click selects (Ctrl/Shift extend); double-click enters.
                        onClicked: (modifiers) => fv.cellClicked(cell.index, modifiers)
                        onDoubleClicked: fv.cellDoubleClicked(cell.index)
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
                        onClicked: (modifiers) => { if (cell.photo) fv.cellClicked(cell.index, modifiers) }
                        onDoubleClicked: { if (cell.photo) fv.cellDoubleClicked(cell.index) }
                        onRightClicked: { if (cell.photo) fv.cellRightClicked(cell.index) }
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
