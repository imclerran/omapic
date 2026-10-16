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
    readonly property int sectionGap: 6 // Column spacing between header and grid

    // The current month groups ({key, label, firstRow, count}); also the ListView
    // model. Kept as a property so the navigation math can read group bounds.
    property var groups: []

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

    // Give the scrolling list keyboard focus (arrow keys, Space, Enter).
    function focusView() {
        list.forceActiveFocus()
    }

    // Rebuild the month groups from the (date-sorted) filtered model. Cheap to
    // skip while hidden — recomputed when the view is shown or its contents change.
    function refresh() {
        if (!tv.visible || !tv.library)
            return
        tv.groups = tv.library.filtered.monthGroups()
        list.model = tv.groups
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

    function rowsIn(g) { return Math.ceil(g.count / tv.columns) }
    function monthHeight(g) { return tv.headerH + tv.sectionGap + rowsIn(g) * tv.cellSize }

    // Scroll so the cell at proxy row `row` is fully visible. Month heights are
    // fixed-size, so the cell's content-Y is computed directly rather than from
    // (possibly unrealized) delegate items.
    function scrollToRow(row) {
        var k = groupIndexOfRow(row)
        if (k < 0)
            return
        var top = 0
        for (var j = 0; j < k; ++j)
            top += monthHeight(tv.groups[j]) + list.spacing
        var li = row - tv.groups[k].firstRow
        var rr = Math.floor(li / tv.columns)
        var cellTop = top + tv.headerH + tv.sectionGap + rr * tv.cellSize
        var cellBottom = cellTop + tv.cellSize
        var maxY = Math.max(0, list.contentHeight - list.height)
        if (cellTop < list.contentY)
            list.contentY = Math.max(0, cellTop)
        else if (cellBottom > list.contentY + list.height)
            list.contentY = Math.min(maxY, cellBottom - list.height)
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
        spacing: 16
        cacheBuffer: 1600
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

        delegate: Column {
            id: monthSection
            required property var modelData
            width: list.width
            spacing: tv.sectionGap

            // Month header, fixed height so section pixel heights are exact.
            Label {
                height: tv.headerH
                verticalAlignment: Text.AlignVCenter
                text: monthSection.modelData.label
                font.pixelSize: 15
                font.bold: true
                color: palette.text
                leftPadding: 8
            }

            Grid {
                columns: tv.columns

                Repeater {
                    model: monthSection.modelData.count

                    delegate: PhotoDelegate {
                        id: cell
                        required property int index
                        // Proxy row for this tile within library.filtered.
                        readonly property int proxyRow: monthSection.modelData.firstRow + index
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
                        selected: hasPhoto ? (tv.selectedIds.indexOf(photo.id) !== -1) : false

                        onClicked: (modifiers) => {
                            if (hasPhoto) tv.photoClicked(proxyRow, photo.id, modifiers)
                        }
                        onDoubleClicked: { if (hasPhoto) tv.photoDoubleClicked(photo.id) }
                        onRightClicked: {
                            if (hasPhoto) tv.photoRightClicked(proxyRow, photo.id, photo.fileName, photo.isVideo === true)
                        }
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
