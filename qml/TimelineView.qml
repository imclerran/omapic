// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls
import Omapic

// Photos grouped into month sections by capture date (newest first). The model
// is library.filtered — sorted by date while this view is active — summarized
// into month groups by PhotoFilterModel::monthGroups(). Each group lays its
// photos out in a wrapping grid of the same PhotoDelegate the main grid uses.
Item {
    id: tv

    property var library
    property color accent: "#4a90d9"
    property bool crop: true
    property int cellSize: 184
    property var selectedIds: []
    // Mirror the window's selection cursor/anchor (proxy rows) so arrow-key
    // navigation starts from the same place the grid left off.
    property int cursorIndex: -1
    property int anchorIndex: -1
    // Whether no tag/search filter is active, for the empty-state wording.
    property bool emptyFiltered: true

    // Columns per month row — the same wrapping width every section uses, and the
    // step size for up/down navigation. Kept in one place so the layout and the
    // keyboard math never disagree.
    readonly property int columns: Math.max(1, Math.floor(width / cellSize))
    // Fixed header height, so a month's pixel height is exactly computable (used
    // to scroll the keyboard cursor into view without relying on realized items).
    readonly property int headerH: 30
    readonly property int sectionGap: 6  // gap between a header and its photos
    readonly property int monthGapTop: 16 // gap above each month's header
    // A header row carries the month gap, the label, and the gap to the photos.
    readonly property int headerRowH: monthGapTop + headerH + sectionGap

    // The current month groups ({key, label, firstRow, count}); kept so the
    // navigation math can read group bounds.
    property var groups: []
    // Flat index of each group's header row within the ListView's row model, so
    // scrollToRow can map a proxy row to the row that renders it.
    property var groupRowStart: []

    // Routed back to the window so selection/slideshow/menus stay consistent with
    // the grid. `index` is the proxy row (library.filtered), matching the grid.
    signal photoClicked(int index, int id, int modifiers)
    signal photoDoubleClicked(int id)
    signal photoRightClicked(int index, int id, string fileName, bool isVideo)
    signal selectAllRequested()
    // Move the selection cursor to a proxy row (extend = Shift-range). Handled by
    // the window, which owns the selection state.
    signal moveCursorRequested(int target, bool extend)
    // Start the slideshow from the focused photo (play = autoplay vs. open paused).
    signal slideshowFromFocusRequested(bool play)
    // Tab / Shift+Tab focus cycle (routed by the window).
    signal tabForward()
    signal tabBackward()

    function toTop() {
        if (list)
            list.positionViewAtBeginning()
    }
    // The vertical scroll position, saved by the window when triage replaces this
    // view and put back when it returns (see Main.qml's triage entry/exit).
    function scrollY() { return list ? list.contentY : 0 }
    function setScrollY(y) { if (list) list.contentY = y }

    // Give the scrolling list keyboard focus (arrow keys, Space, Enter).
    function focusView() {
        list.forceActiveFocus()
    }

    // Rebuild the month groups from the (date-sorted) filtered model. Cheap to
    // skip while hidden — recomputed when the view is shown or its contents change.
    function refresh() {
        if (!tv.visible || !tv.library)
            return
        // Force every cached photoAt() snapshot to re-fetch. A favorite (or tag)
        // changed in another view removes/alters rows without a dataChanged this
        // view sees, so re-fetch on every (re)show to avoid a stale star/badge.
        tv.dataRevision++
        tv.groups = tv.library.filtered.monthGroups()
        rebuildRows()
    }

    // Flatten the month groups into a virtualizable list of rows: one header row
    // per month, then ceil(count / columns) photo rows of up to `columns` tiles
    // each. The ListView then realizes only on-screen rows (± cacheBuffer) — so a
    // huge month no longer instantiates all its tiles at once. Recomputed when the
    // groups change and when the column count changes (window resize).
    function rebuildRows() {
        const rows = []
        const starts = []
        for (let k = 0; k < tv.groups.length; ++k) {
            starts.push(rows.length)
            const g = tv.groups[k]
            rows.push({ header: true, label: g.label })
            const n = Math.ceil(g.count / tv.columns)
            for (let r = 0; r < n; ++r) {
                const start = g.firstRow + r * tv.columns
                rows.push({ header: false, firstRow: start,
                            tiles: Math.min(tv.columns, g.count - r * tv.columns) })
            }
        }
        tv.groupRowStart = starts
        list.model = rows
    }

    // Column count changes on resize; re-flow the rows to match. Replacing the row
    // model resets the ListView to the top, so keep the photo at the top of the
    // viewport roughly in place across the re-flow.
    onColumnsChanged: {
        if (!tv.visible)
            return
        let anchorProxy = -1
        const topIdx = list.indexAt(2, list.contentY + 2)
        if (topIdx >= 0 && list.model[topIdx] && !list.model[topIdx].header)
            anchorProxy = list.model[topIdx].firstRow
        rebuildRows()
        if (anchorProxy >= 0) {
            const ap = anchorProxy
            Qt.callLater(function () {
                const k = groupIndexOfRow(ap)
                if (k >= 0 && k < tv.groupRowStart.length) {
                    const lr = Math.floor((ap - tv.groups[k].firstRow) / tv.columns)
                    list.positionViewAtIndex(tv.groupRowStart[k] + 1 + lr, ListView.Beginning)
                }
            })
        }
    }

    onVisibleChanged: {
        if (visible) {
            refresh()
            list.forceActiveFocus()
        }
    }
    Component.onCompleted: refresh()

    // Bumped on any model dataChanged so the cells (which read photoAt() once)
    // re-fetch — e.g. a tag edit updates the badge without a reload.
    property int dataRevision: 0
    Connections {
        target: tv.library ? tv.library.filtered : null
        // countChanged also fires on a re-sort (layoutChanged), so this covers
        // both filtering and the switch into date order when the view opens.
        // Coalesce a burst of count changes into one recompute: a filter change
        // (e.g. toggling de-dup when entering this view) re-inserts rows one at a
        // time, and monthGroups() is O(n) — recomputing it per row was O(n²) and
        // froze the view for seconds. Qt.callLater collapses repeats within a tick.
        function onCountChanged() { Qt.callLater(tv.refresh) }
        function onDataChanged() { tv.dataRevision++ }
    }

    // --- Keyboard navigation geometry ---------------------------------------
    // Months reset the grid flow, so up/down can't just step by `columns` across
    // the flat proxy; they must respect section boundaries.

    function groupIndexOfRow(row) {
        for (var k = 0; k < tv.groups.length; ++k) {
            var g = tv.groups[k]
            if (row >= g.firstRow && row < g.firstRow + g.count)
                return k
        }
        return -1
    }

    // The proxy row an arrow key moves to from `base`. left/right walk the flat
    // order; up/down move one visual row, crossing into the adjacent month (same
    // column, clamped) when leaving a section's first/last row.
    function navTarget(dir, base) {
        var k = groupIndexOfRow(base)
        if (k < 0)
            return base
        var g = tv.groups[k]
        var li = base - g.firstRow
        var c = li % tv.columns
        var r = Math.floor(li / tv.columns)

        if (dir === "left")
            return base - 1
        if (dir === "right")
            return base + 1
        if (dir === "down") {
            if (li + tv.columns < g.count)
                return base + tv.columns
            if (k + 1 < tv.groups.length) {
                var gn = tv.groups[k + 1]
                return gn.firstRow + Math.min(c, gn.count - 1)
            }
            return base
        }
        if (dir === "up") {
            if (r > 0)
                return base - tv.columns
            if (k - 1 >= 0) {
                var gp = tv.groups[k - 1]
                var lastRowStart = Math.floor((gp.count - 1) / tv.columns) * tv.columns
                return gp.firstRow + Math.min(lastRowStart + c, gp.count - 1)
            }
            return base
        }
        return base
    }

    // Scroll so the cell at proxy row `row` is visible. Maps the proxy row to the
    // flat row that renders it and lets the ListView bring that row into view (the
    // row is exactly one cell tall, so this makes the cell visible).
    function scrollToRow(row) {
        var k = groupIndexOfRow(row)
        if (k < 0 || k >= tv.groupRowStart.length)
            return
        var localRow = Math.floor((row - tv.groups[k].firstRow) / tv.columns)
        var flatIdx = tv.groupRowStart[k] + 1 + localRow // +1 past the header row
        list.positionViewAtIndex(flatIdx, ListView.Contain)
    }

    function navigate(dir, extend) {
        var count = tv.library ? tv.library.filtered.count : 0
        if (count === 0)
            return
        var base = (tv.cursorIndex >= 0 && tv.cursorIndex < count) ? tv.cursorIndex
                 : (tv.anchorIndex >= 0 && tv.anchorIndex < count) ? tv.anchorIndex : 0
        // With nothing selected, the first arrow just selects `base` (matches the
        // grid); otherwise it moves.
        var target = (tv.selectedIds.length > 0) ? navTarget(dir, base) : base
        target = Math.max(0, Math.min(count - 1, target))
        tv.moveCursorRequested(target, extend)
        tv.scrollToRow(target)
    }

    ListView {
        id: list
        anchors.fill: parent
        clip: true
        model: []
        spacing: 0 // month gaps are baked into the header rows (see rebuildRows)
        // Recycle row delegates instead of destroying and rebuilding them, the
        // same as the main grid (Main.qml). This is what keeps a resize cheap: a
        // width change re-flows the rows (rebuildRows replaces the model), and
        // without reuse every tile delegate would be torn down and recreated.
        reuseItems: true
        cacheBuffer: 1200
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}

        focus: true
        keyNavigationEnabled: false
        Keys.onPressed: (event) => {
            if (event.key === Qt.Key_A && (event.modifiers & Qt.ControlModifier)) {
                tv.selectAllRequested()
                event.accepted = true
            }
        }
        Keys.onLeftPressed: (event) => { tv.navigate("left", (event.modifiers & Qt.ShiftModifier) !== 0); event.accepted = true }
        Keys.onRightPressed: (event) => { tv.navigate("right", (event.modifiers & Qt.ShiftModifier) !== 0); event.accepted = true }
        Keys.onUpPressed: (event) => { tv.navigate("up", (event.modifiers & Qt.ShiftModifier) !== 0); event.accepted = true }
        Keys.onDownPressed: (event) => { tv.navigate("down", (event.modifiers & Qt.ShiftModifier) !== 0); event.accepted = true }
        Keys.onSpacePressed: (event) => { tv.slideshowFromFocusRequested(true); event.accepted = true }
        Keys.onReturnPressed: (event) => { tv.slideshowFromFocusRequested(false); event.accepted = true }
        Keys.onEnterPressed: (event) => { tv.slideshowFromFocusRequested(false); event.accepted = true }
        Keys.onTabPressed: (event) => { tv.tabForward(); event.accepted = true }
        Keys.onBacktabPressed: (event) => { tv.tabBackward(); event.accepted = true }

        // Each ListView row is either a month header or one grid row of up to
        // `columns` tiles — so only on-screen rows are realized, not whole months.
        delegate: Item {
            id: rowItem
            required property var modelData
            width: list.width
            height: rowItem.modelData.header ? tv.headerRowH : tv.cellSize

            // Month header: monthGapTop above it, sectionGap below (to the photos).
            Label {
                visible: rowItem.modelData.header
                anchors.left: parent.left
                anchors.leftMargin: 8
                anchors.bottom: parent.bottom
                anchors.bottomMargin: tv.sectionGap
                height: tv.headerH
                verticalAlignment: Text.AlignVCenter
                text: rowItem.modelData.header ? rowItem.modelData.label : ""
                font.pixelSize: 15
                font.bold: true
                color: palette.text
            }

            // One wrapping-grid row of tiles.
            Row {
                visible: !rowItem.modelData.header

                Repeater {
                    model: rowItem.modelData.header ? 0 : rowItem.modelData.tiles

                    delegate: PhotoDelegate {
                        id: cell
                        required property int index
                        // Proxy row for this tile within library.filtered.
                        readonly property int proxyRow: rowItem.modelData.firstRow + index
                        readonly property var photo: { tv.dataRevision; return tv.library.filtered.photoAt(proxyRow) }
                        // photoAt() returns an empty object for an out-of-range row
                        // (possible transiently while the model updates); id is set
                        // only for a real photo.
                        readonly property bool hasPhoto: photo && photo.id !== undefined

                        width: tv.cellSize
                        height: tv.cellSize
                        accent: tv.accent
                        crop: tv.crop
                        filePath: hasPhoto ? photo.filePath : ""
                        isVideo: hasPhoto ? (photo.isVideo === true) : false
                        tagCount: hasPhoto ? photo.tagCount : 0
                        favorite: hasPhoto ? (photo.favorite === true) : false
                        selected: hasPhoto ? (tv.selectedIds.indexOf(photo.id) !== -1) : false

                        onClicked: (modifiers) => {
                            if (hasPhoto) tv.photoClicked(proxyRow, photo.id, modifiers)
                        }
                        onDoubleClicked: { if (hasPhoto) tv.photoDoubleClicked(photo.id) }
                        onRightClicked: {
                            if (hasPhoto) tv.photoRightClicked(proxyRow, photo.id, photo.fileName, photo.isVideo === true)
                        }
                        onFavoriteToggled: { if (hasPhoto) tv.library.toggleFavorite(photo.id) }
                    }
                }
            }
        }
    }

    // Match the gallery grid's wheel/trackpad feel: amplified immediate trackpad
    // scrolling, one row per mouse-wheel notch with momentum. (The ListView's
    // native wheel is noticeably slower.)
    WheelScroller { flickable: list; step: tv.cellSize }

    Label {
        anchors.centerIn: parent
        visible: list.count === 0
        text: tv.emptyFiltered ? qsTr("Import a folder to get started.")
                               : qsTr("No photos match the current filters.")
        color: palette.placeholderText
    }
}
