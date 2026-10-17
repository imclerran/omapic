// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls
import Omapic

// Album browser, modeled on FolderView: album tiles (2x2 cover mosaics) at the
// list level, then one album's photos once opened. Albums are logical folders —
// the album entries come from Library.albums; the photos come from
// library.filtered (restricted to the album tag) and are addressed by proxy row,
// exactly like the main grid. Keyboard navigation, selection and slideshow mirror
// the folder view one-for-one (see the av* helpers in Main.qml). Favorites is
// always the first tile; tag filtering and search apply just as in folder view.
Item {
    id: av

    property var library
    property var entries: []
    property int currentAlbum: -1
    property string currentAlbumName: ""
    property color accent: "#4a90d9"
    property bool crop: true
    property int cellSize: 184
    property var selectedIds: []
    property var selectedAlbums: []
    // True while a search is active: the view flattens to matching photos.
    property bool searching: false

    // Album tiles occupy the first `albumCount` grid indices (only at the album
    // list level with no search); the rest are photos at proxy row index-albumCount.
    readonly property int albumCount: entries.length

    // Bumped on any model dataChanged so photo cells (which read photoAt() once)
    // re-fetch — e.g. a favorite toggle updates the star without a reload.
    property int dataRevision: 0
    Connections {
        target: av.library ? av.library.filtered : null
        function onDataChanged() { av.dataRevision++ }
    }
    // A favorite/tag change made in another view can remove or alter rows without
    // a dataChanged this view observes, leaving its cached photoAt() snapshots
    // stale. Re-fetch them whenever the view is (re)shown.
    onVisibleChanged: if (visible) av.dataRevision++

    signal goUp()                            // back to the album list (Alt+Up / Back)
    signal enterAlbum(int albumId)           // open an album (Enter on a selected tile)
    signal cellClicked(int gridIndex, int modifiers)
    signal cellDoubleClicked(int gridIndex)  // album -> open, photo -> view
    signal cellRightClicked(int gridIndex)
    signal photoFavoriteToggled(int id)
    signal selectAllRequested()
    signal slideshowRequested(bool play)
    signal cursorMove(string dir, bool extend)  // arrow-key nav over the combined grid
    signal tabForward()
    signal tabBackward()

    // Columns currently shown, so arrow Up/Down can step a whole row.
    readonly property int columns: agrid ? Math.max(1, Math.floor(agrid.width / agrid.cellWidth)) : 1

    function toTop() { if (agrid) agrid.positionViewAtBeginning() }
    function focusView() { if (agrid) agrid.forceActiveFocus() }
    function ensureVisible(index) { if (agrid) agrid.positionViewAtIndex(index, GridView.Contain) }

    // Enter/Return: open the selected album (descend into it) when exactly one
    // album tile and no photos are selected; otherwise start the slideshow.
    function activateSelection() {
        if (av.selectedAlbums.length === 1 && av.selectedIds.length === 0)
            av.enterAlbum(av.selectedAlbums[0])
        else
            av.slideshowRequested(false)
    }

    // "#rrggbb" for a color, for the Up button's SVG (matches FolderView).
    function hex(c) {
        function h(v) { const s = Math.round(v * 255).toString(16); return s.length < 2 ? "0" + s : s }
        return "#" + h(c.r) + h(c.g) + h(c.b)
    }

    Column {
        anchors.fill: parent
        spacing: 0

        // Title bar: a Back button (only inside an album) plus the album name.
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
                visible: av.currentAlbum > 0
                onClicked: av.goUp()
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Back to albums")
                contentItem: Image {
                    anchors.centerIn: parent
                    source: "data:image/svg+xml," + encodeURIComponent(
                        "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'><path fill='"
                        + av.hex(av.accent)
                        + "' d='M12 3 L20 11 L15 11 L15 20 L9 20 L9 11 L4 11 Z'/></svg>")
                    sourceSize.width: 16
                    sourceSize.height: 16
                    smooth: true
                }
            }
            Label {
                anchors.verticalCenter: parent.verticalCenter
                anchors.left: parent.left
                anchors.leftMargin: 10
                anchors.right: upButton.visible ? upButton.left : parent.right
                anchors.rightMargin: upButton.visible ? 6 : 10
                text: av.currentAlbum > 0 ? av.currentAlbumName : qsTr("Albums")
                elide: Text.ElideMiddle
                font.pixelSize: 12
                color: palette.placeholderText
            }
        }

        GridView {
            id: agrid
            width: parent.width
            height: parent.height - y
            clip: true
            cellWidth: av.cellSize
            cellHeight: av.cellSize
            boundsBehavior: Flickable.StopAtBounds
            reuseItems: true
            cacheBuffer: 1200
            flickDeceleration: 2500
            maximumFlickVelocity: 6000
            ScrollBar.vertical: ScrollBar {}

            model: av.albumCount + (av.library ? av.library.filtered.count : 0)

            focus: true
            keyNavigationEnabled: false
            Keys.onPressed: (event) => {
                if (event.key === Qt.Key_A && (event.modifiers & Qt.ControlModifier)) {
                    av.selectAllRequested()
                    event.accepted = true
                }
            }
            // Arrow keys move the selection cursor over the combined album+photo
            // grid. Alt+Up is "back to the album list", so let it fall through.
            Keys.onLeftPressed: (event) => { av.cursorMove("left", (event.modifiers & Qt.ShiftModifier) !== 0); event.accepted = true }
            Keys.onRightPressed: (event) => { av.cursorMove("right", (event.modifiers & Qt.ShiftModifier) !== 0); event.accepted = true }
            Keys.onUpPressed: (event) => {
                if (event.modifiers & Qt.AltModifier) { event.accepted = false; return }
                av.cursorMove("up", (event.modifiers & Qt.ShiftModifier) !== 0); event.accepted = true
            }
            Keys.onDownPressed: (event) => { av.cursorMove("down", (event.modifiers & Qt.ShiftModifier) !== 0); event.accepted = true }
            Keys.onSpacePressed: (event) => { av.slideshowRequested(true); event.accepted = true }
            Keys.onReturnPressed: (event) => { av.activateSelection(); event.accepted = true }
            Keys.onEnterPressed: (event) => { av.activateSelection(); event.accepted = true }
            Keys.onTabPressed: (event) => { av.tabForward(); event.accepted = true }
            Keys.onBacktabPressed: (event) => { av.tabBackward(); event.accepted = true }

            delegate: Item {
                id: cell
                required property int index
                width: agrid.cellWidth
                height: agrid.cellHeight

                readonly property bool isAlbum: index < av.albumCount
                readonly property var entry: isAlbum ? av.entries[index] : null
                readonly property int proxyRow: index - av.albumCount
                readonly property var photo: {
                    av.dataRevision
                    return isAlbum ? null : (av.library.filtered.photoAt(proxyRow) || null)
                }

                Loader {
                    anchors.fill: parent
                    sourceComponent: cell.isAlbum ? albumComp : photoComp
                }

                Component {
                    id: albumComp
                    FolderMosaicCell {
                        // Prefix a star on the Favorites tile so it reads as special.
                        entry: cell.entry ? ({
                            name: (cell.entry.favorite ? "★  " : "") + cell.entry.name,
                            count: cell.entry.count,
                            thumbs: cell.entry.thumbs
                        }) : ({})
                        accent: av.accent
                        selected: cell.entry && av.selectedAlbums.indexOf(cell.entry.id) !== -1
                        onClicked: (modifiers) => av.cellClicked(cell.index, modifiers)
                        onDoubleClicked: av.cellDoubleClicked(cell.index)
                    }
                }
                Component {
                    id: photoComp
                    PhotoDelegate {
                        accent: av.accent
                        crop: av.crop
                        filePath: cell.photo ? cell.photo.filePath : ""
                        isVideo: cell.photo ? (cell.photo.isVideo === true) : false
                        tagCount: cell.photo ? cell.photo.tagCount : 0
                        favorite: cell.photo ? (cell.photo.favorite === true) : false
                        selected: cell.photo ? (av.selectedIds.indexOf(cell.photo.id) !== -1) : false
                        onClicked: (modifiers) => { if (cell.photo) av.cellClicked(cell.index, modifiers) }
                        onDoubleClicked: { if (cell.photo) av.cellDoubleClicked(cell.index) }
                        onRightClicked: { if (cell.photo) av.cellRightClicked(cell.index) }
                        onFavoriteToggled: { if (cell.photo) av.photoFavoriteToggled(cell.photo.id) }
                    }
                }
            }

            WheelScroller { flickable: agrid; step: agrid.cellHeight }

            Label {
                anchors.centerIn: parent
                width: parent.width - 48
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                visible: agrid.count === 0
                text: av.searching ? qsTr("No photos match your search.")
                    : av.currentAlbum > 0 ? qsTr("This album is empty.")
                    : qsTr("No albums yet. Right-click a photo and choose “Add to album” to create one.")
                color: palette.placeholderText
            }
        }
    }
}
