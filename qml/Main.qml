// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Omapic

ApplicationWindow {
    id: win
    width: 1280
    height: 820
    visible: true
    title: qsTr("omapic")

    readonly property color accent: omarchyTheme.accent

    // Multi-selection state: ids of selected photos, plus the anchor row for
    // shift-range selection. Reassign selectedIds wholesale so bindings update.
    property var selectedIds: []
    property int anchorIndex: -1
    property int cursorIndex: -1

    // The unshuffled slideshow playlist, kept so shuffle can be toggled live.
    property var slideshowBase: []
    // Shuffle state for the CURRENT slideshow only; seeded from the saved
    // setting at start, toggled by 's' without changing the saved default.
    property bool slideshowShuffleActive: false

    function selectRange(a, b) {
        const lo = Math.min(a, b)
        const hi = Math.max(a, b)
        const ids = []
        for (let r = lo; r <= hi; ++r) {
            const rid = library.filtered.idAt(r)
            if (rid >= 0)
                ids.push(rid)
        }
        selectedIds = ids
    }

    // Select every photo in the current (filtered) gallery.
    function selectAll() {
        const n = library.filtered.count
        if (n === 0)
            return
        selectRange(0, n - 1)
        anchorIndex = 0
        cursorIndex = n - 1
    }

    // Re-anchor the gallery to the top. Deferred so it runs after the filter
    // model has updated, avoiding a GridView originY gap (blank space above the
    // results) when the filter changes while scrolled down.
    function scrollGalleryToTop() {
        Qt.callLater(function () { grid.positionViewAtBeginning() })
    }

    // Drop any selected ids whose photos have left the library's active set
    // (e.g. a folder was disabled or removed), so the detail pane doesn't keep
    // showing hidden photos.
    function pruneSelection() {
        if (selectedIds.length === 0)
            return
        const kept = selectedIds.filter(function (id) { return library.photos.contains(id) })
        if (kept.length !== selectedIds.length) {
            selectedIds = kept
            if (kept.length === 0) {
                anchorIndex = -1
                cursorIndex = -1
            }
        }
    }

    function handleClick(index, id, modifiers) {
        if ((modifiers & Qt.ShiftModifier) && anchorIndex >= 0) {
            cursorIndex = index
            selectRange(anchorIndex, cursorIndex)
        } else if (modifiers & Qt.ControlModifier) {
            const arr = selectedIds.slice()
            const pos = arr.indexOf(id)
            if (pos === -1)
                arr.push(id)
            else
                arr.splice(pos, 1)
            selectedIds = arr
            anchorIndex = index
            cursorIndex = index
        } else {
            selectedIds = [id]
            anchorIndex = index
            cursorIndex = index
        }
        grid.forceActiveFocus()
    }

    // Arrow-key navigation. Plain arrows move a single selection; Shift+arrows
    // extend a contiguous selection from the anchor to a moving cursor.
    function navigate(dir, extend) {
        const count = library.filtered.count
        if (count === 0)
            return
        const cols = Math.max(1, Math.floor(grid.width / grid.cellWidth))
        let base = (cursorIndex >= 0 && cursorIndex < count) ? cursorIndex
                 : (anchorIndex >= 0 && anchorIndex < count) ? anchorIndex : 0

        let target = base
        if (selectedIds.length > 0) {
            if (dir === "left")       target = base - 1
            else if (dir === "right") target = base + 1
            else if (dir === "up")    target = base - cols
            else if (dir === "down")  target = base + cols
        }
        target = Math.max(0, Math.min(count - 1, target))

        if (extend) {
            if (anchorIndex < 0 || anchorIndex >= count)
                anchorIndex = base
            cursorIndex = target
            selectRange(anchorIndex, cursorIndex)
        } else {
            const id = library.filtered.idAt(target)
            if (id < 0)
                return
            selectedIds = [id]
            anchorIndex = target
            cursorIndex = target
        }
        grid.positionViewAtIndex(target, GridView.Contain)
    }

    // Start a slideshow at the currently focused photo. play=false opens paused.
    function slideshowFromFocus(play) {
        const startId = (anchorIndex >= 0 && anchorIndex < library.filtered.count)
                        ? library.filtered.idAt(anchorIndex) : -1
        startSlideshow(startId, play)
    }

    // Build the slideshow set: the selection when more than one is selected,
    // otherwise the whole filtered set — in grid order, starting at startId.
    // play=false opens the slideshow paused.
    function startSlideshow(startId, play) {
        const selectedOnly = selectedIds.length > 1
        // Enter / double-click (play === false) on a specific photo start there,
        // ignoring random start. Play / Space (play === true) let random start win.
        const honorStart = (!play && startId >= 0)
        const srcs = []
        let startPos = 0
        const n = library.filtered.count
        for (let r = 0; r < n; ++r) {
            const id = library.filtered.idAt(r)
            if (selectedOnly && selectedIds.indexOf(id) === -1)
                continue
            if (id === startId)
                startPos = srcs.length
            srcs.push("" + library.filtered.sourceAt(r))
        }
        if (srcs.length === 0)
            return

        slideshowBase = srcs.slice()
        slideshowShuffleActive = appSettings.shuffle

        if (slideshowShuffleActive) {
            // Keep the chosen start photo first (if any), shuffle the rest.
            if (startPos > 0) {
                const s = srcs[0]; srcs[0] = srcs[startPos]; srcs[startPos] = s
            }
            const from = (startId >= 0) ? 1 : 0
            for (let i = srcs.length - 1; i > from; --i) {
                const j = from + Math.floor(Math.random() * (i - from + 1))
                const t = srcs[i]; srcs[i] = srcs[j]; srcs[j] = t
            }
            startPos = 0
        } else if (appSettings.randomStart && !honorStart) {
            // Random start applies to Play/Space (and whenever there's no specific
            // photo to honor) — within the selection if several are selected,
            // otherwise the whole set. Enter/double-click keep their photo.
            startPos = Math.floor(Math.random() * srcs.length)
        }

        slideshow.sources = srcs
        slideshow.startAt(startPos, play)
    }

    // Toggle shuffle while the slideshow is running: reorder the upcoming photos
    // but keep the current one on screen.
    function toggleSlideshowShuffle() {
        slideshowShuffleActive = !slideshowShuffleActive
        if (slideshowBase.length === 0)
            return

        const cur = slideshow.currentSource
        const list = slideshowBase.slice()

        if (slideshowShuffleActive) {
            const idx = list.indexOf(cur)
            if (idx > 0) {
                list.splice(idx, 1)
                list.unshift(cur)
            }
            // Shuffle indices 1..end, keeping the current photo at the front.
            for (let i = list.length - 1; i > 1; --i) {
                const j = 1 + Math.floor(Math.random() * i)
                const t = list[i]; list[i] = list[j]; list[j] = t
            }
            slideshow.setSources(list, 0)
        } else {
            const pos = Math.max(0, list.indexOf(cur))
            slideshow.setSources(list, pos)
        }
    }

    Library {
        id: library
    }

    Settings {
        id: appSettings
        category: "slideshow"
        property int intervalMs: 4000
        property bool shuffle: false
        property bool fade: true
        property bool randomStart: false
    }

    LibraryManager {
        id: libraryManager
        library: library
        accent: win.accent
    }

    TagManager {
        id: tagManager
        library: library
        accent: win.accent
    }

    // When the active photo set changes (folder enabled/disabled/removed, rescan,
    // orphan cleanup), drop selected photos that are no longer shown.
    Connections {
        target: library
        function onFoldersChanged() {
            win.pruneSelection()
            win.scrollGalleryToTop()
        }
    }

    // Changing the tag filter re-anchors the gallery to the top of the results.
    Connections {
        target: library.tags
        function onSelectionChanged() { win.scrollGalleryToTop() }
    }

    // Push the sidebar's tag selection and the toolbar's match mode into the filter.
    Binding {
        target: library.filtered
        property: "selectedTagIds"
        value: library.tags.selectedTagIds
    }
    Binding {
        target: library.filtered
        property: "matchAll"
        value: tagSidebar.matchAll
    }

    // Status bar: photo/selection count always, plus a progress bar for
    // non-blocking background jobs (bulk tagging). Blocking jobs use the overlay.
    footer: ToolBar {
        id: statusBar

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 8
            anchors.rightMargin: 8
            spacing: 12

            Label {
                text: win.selectedIds.length > 0
                      ? qsTr("%1 photos · %2 selected").arg(library.filtered.count).arg(win.selectedIds.length)
                      : qsTr("%1 photos").arg(library.filtered.count)
                color: palette.placeholderText
            }

            Item { Layout.fillWidth: true }

            Label {
                visible: library.busy && !library.busyModal
                text: library.statusText
                elide: Text.ElideRight
            }
            ProgressBar {
                visible: library.busy && !library.busyModal
                Layout.preferredWidth: 180
                indeterminate: library.progress < 0
                from: 0; to: 1
                value: library.progress < 0 ? 0 : library.progress
            }
            Label {
                visible: library.busy && !library.busyModal && library.progress >= 0
                text: Math.round(library.progress * 100) + "%"
                color: palette.placeholderText
            }
        }
    }

    header: ToolBar {
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 8
            anchors.rightMargin: 8

            ToolButton {
                text: qsTr("Manage ▾")
                onClicked: manageMenu.open()

                Menu {
                    id: manageMenu
                    y: parent.height
                    MenuItem {
                        text: qsTr("Library…")
                        onTriggered: libraryManager.open()
                    }
                    MenuItem {
                        text: qsTr("Tags…")
                        onTriggered: tagManager.open()
                    }
                }
            }

            Item { Layout.fillWidth: true }

            ToolButton {
                id: tagPresenceButton
                property int mode: 0 // 0 all, 1 tagged only, 2 untagged only
                text: mode === 0 ? qsTr("All photos")
                    : mode === 1 ? qsTr("Tagged only")
                    : qsTr("Untagged only")
                highlighted: mode !== 0
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Cycle: all photos → tagged only → untagged only")
                onClicked: {
                    mode = (mode + 1) % 3
                    library.filtered.tagPresence = mode
                    win.scrollGalleryToTop()
                }
            }

            TextField {
                id: gallerySearch
                Layout.preferredWidth: 220
                placeholderText: qsTr("Search name or tags…")
                // Combines (AND) with the tag filter from the sidebar.
                onTextChanged: {
                    library.filtered.searchText = text
                    win.scrollGalleryToTop()
                }
                Keys.onEscapePressed: text = ""
            }

            ToolButton {
                text: win.selectedIds.length > 1
                      ? qsTr("▶  Slideshow (%1)").arg(win.selectedIds.length)
                      : qsTr("▶  Slideshow")
                enabled: library.filtered.count > 0
                onClicked: win.slideshowFromFocus(true)
            }
            ToolButton {
                text: qsTr("⚙")
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Slideshow settings")
                onClicked: settingsDialog.open()
            }
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        SplitView {
            orientation: Qt.Vertical
            Layout.preferredWidth: 300
            Layout.fillHeight: true

            DetailPanel {
                SplitView.preferredHeight: 340
                SplitView.minimumHeight: 160
                library: library
                selectedIds: win.selectedIds
                accent: win.accent
            }

            TagSidebar {
                id: tagSidebar
                SplitView.fillHeight: true
                SplitView.minimumHeight: 120
                tagModel: library.tags
                accent: win.accent
                // Keep the library's filter mode in sync so the sidebar can
                // narrow its tags to the current match set when this is on.
                onMatchAllChanged: { library.setMatchAll(matchAll); win.scrollGalleryToTop() }
                Component.onCompleted: library.setMatchAll(matchAll)
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: palette.base

            GridView {
                id: grid
                anchors.fill: parent
                clip: true
                cellWidth: 184
                cellHeight: 184
                model: library.filtered
                boundsBehavior: Flickable.StopAtBounds

                // Arrow-key navigation (our own, not GridView's currentIndex).
                focus: true
                keyNavigationEnabled: false
                // Ctrl+A selects the whole gallery. Handled here (not window-wide)
                // so text fields keep their own Ctrl+A for selecting text.
                Keys.onPressed: (event) => {
                    if (event.key === Qt.Key_A && (event.modifiers & Qt.ControlModifier)) {
                        win.selectAll()
                        event.accepted = true
                    }
                }
                Keys.onLeftPressed: (event) => { win.navigate("left", (event.modifiers & Qt.ShiftModifier) !== 0); event.accepted = true }
                Keys.onRightPressed: (event) => { win.navigate("right", (event.modifiers & Qt.ShiftModifier) !== 0); event.accepted = true }
                Keys.onUpPressed: (event) => { win.navigate("up", (event.modifiers & Qt.ShiftModifier) !== 0); event.accepted = true }
                Keys.onDownPressed: (event) => { win.navigate("down", (event.modifiers & Qt.ShiftModifier) !== 0); event.accepted = true }
                Keys.onSpacePressed: (event) => { win.slideshowFromFocus(true); event.accepted = true }
                Keys.onReturnPressed: (event) => { win.slideshowFromFocus(false); event.accepted = true }
                Keys.onEnterPressed: (event) => { win.slideshowFromFocus(false); event.accepted = true }

                // Keep delegates alive and recycle them so fast scrolling
                // doesn't thrash delegate creation / image decoding.
                reuseItems: true
                cacheBuffer: 1200

                // Longer glide for touch flicks.
                flickDeceleration: 2500
                maximumFlickVelocity: 6000

                ScrollBar.vertical: ScrollBar {}

                delegate: PhotoDelegate {
                    width: grid.cellWidth
                    height: grid.cellHeight
                    accent: win.accent
                    selected: win.selectedIds.indexOf(model.id) !== -1
                    onClicked: (modifiers) => win.handleClick(index, model.id, modifiers)
                    onDoubleClicked: win.startSlideshow(model.id, false)
                }
            }

            // Momentum/fast wheel + trackpad scrolling. Sits above the grid and
            // consumes only wheel events; button clicks fall through to delegates.
            NumberAnimation {
                id: wheelAnim
                target: grid
                property: "contentY"
                duration: 220
                easing.type: Easing.OutCubic
            }

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.NoButton
                propagateComposedEvents: true
                onWheel: (wheel) => {
                    const maxY = Math.max(0, grid.contentHeight - grid.height)
                    if (wheel.pixelDelta.y !== 0) {
                        // High-resolution trackpad: amplified, immediate.
                        wheelAnim.stop()
                        grid.contentY = Math.max(0, Math.min(maxY, grid.contentY - wheel.pixelDelta.y * 4.0))
                    } else {
                        // Mouse-wheel notches: glide ~0.7 page with momentum.
                        const step = (wheel.angleDelta.y / 120) * grid.height * 0.7
                        const target = Math.max(0, Math.min(maxY, grid.contentY - step))
                        wheelAnim.stop()
                        wheelAnim.to = target
                        wheelAnim.start()
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                visible: grid.count === 0
                text: (library.tags.selectedTagIds.length === 0 && gallerySearch.text.trim() === ""
                       && tagPresenceButton.mode === 0)
                      ? qsTr("Import a folder to get started.")
                      : qsTr("No photos match the current filters.")
                color: palette.placeholderText
            }
        }
    }

    SlideshowView {
        id: slideshow
        intervalMs: appSettings.intervalMs
        shuffle: win.slideshowShuffleActive
        transitionMs: appSettings.fade ? 280 : 0
        onToggleShuffleRequested: win.toggleSlideshowShuffle()
    }

    // Progress overlay for background jobs (startup hashing / folder import).
    // A Popup in the window overlay layer, modal so it stacks above any open
    // dialog (e.g. the library manager) and dims everything beneath it.
    Popup {
        id: busyPopup
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        dim: true
        closePolicy: Popup.NoAutoClose
        visible: library.busy && library.busyModal
        padding: 24
        width: Math.min(420, (Overlay.overlay ? Overlay.overlay.width : width) - 48)

        Overlay.modal: Rectangle { color: Qt.rgba(0, 0, 0, 0.45) }

        background: Rectangle {
            color: palette.window
            radius: 10
            border.color: palette.mid
        }

        contentItem: ColumnLayout {
            spacing: 14
            Label {
                Layout.fillWidth: true
                text: library.statusText
                font.pixelSize: 15
                font.bold: true
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
            }
            ProgressBar {
                Layout.fillWidth: true
                indeterminate: library.progress < 0
                from: 0; to: 1
                value: library.progress < 0 ? 0 : library.progress
            }
            Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                visible: library.progress >= 0
                text: Math.round(library.progress * 100) + "%"
                color: palette.placeholderText
                font.pixelSize: 12
            }
        }
    }

    Dialog {
        id: settingsDialog
        title: qsTr("Slideshow settings")
        anchors.centerIn: parent
        modal: true
        standardButtons: Dialog.Close

        ColumnLayout {
            spacing: 16

            RowLayout {
                Layout.fillWidth: true
                spacing: 16
                Label {
                    text: qsTr("Seconds per photo")
                    Layout.fillWidth: true
                }
                SpinBox {
                    from: 1
                    to: 120
                    value: Math.round(appSettings.intervalMs / 1000)
                    onValueModified: appSettings.intervalMs = value * 1000
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 16
                Label {
                    text: qsTr("Shuffle order")
                    Layout.fillWidth: true
                }
                Switch {
                    checked: appSettings.shuffle
                    onToggled: appSettings.shuffle = checked
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 16
                Label {
                    text: qsTr("Crossfade between photos")
                    Layout.fillWidth: true
                }
                Switch {
                    checked: appSettings.fade
                    onToggled: appSettings.fade = checked
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 16
                Label {
                    text: qsTr("Start from a random photo")
                    Layout.fillWidth: true
                }
                Switch {
                    checked: appSettings.randomStart
                    onToggled: appSettings.randomStart = checked
                }
            }
        }
    }
}
