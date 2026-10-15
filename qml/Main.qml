// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

import QtCore
import QtQuick
import QtQuick.Window
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import Omapic

ApplicationWindow {
    id: win
    width: 1280
    height: 820
    visible: true
    title: qsTr("Omapic")

    readonly property color accent: omarchyTheme.accent

    // Grid thumbnail sizes (square cell edge, px): xs, sm, md, lg, xl. "md" is
    // the original default. The chosen index is persisted in gallerySettings.
    readonly property var thumbSizes: [120, 152, 184, 240, 320]
    readonly property int thumbIndex: Math.max(0, Math.min(thumbSizes.length - 1, gallerySettings.thumbIndex))
    readonly property int cellSize: thumbSizes[thumbIndex]

    // Multi-selection state: ids of selected photos, plus the anchor row for
    // shift-range selection. Reassign selectedIds wholesale so bindings update.
    property var selectedIds: []
    property int anchorIndex: -1
    property int cursorIndex: -1

    // Folder-view selection: paths of selected folders (photos reuse selectedIds),
    // with a grid-index anchor/cursor spanning folders and photos for Shift-range.
    property var selectedFolders: []
    property int fvAnchor: -1
    property int fvCursor: -1

    // The photo the right-click context menu currently targets. Set when the
    // menu opens and read by the menu items and the remove/delete confirm dialogs.
    property int menuPhotoId: -1
    property string menuPhotoName: ""
    property bool menuPhotoIsVideo: false

    // Central view mode: "ungrouped", "timeline", or "folder". Restored from and
    // persisted to gallerySettings (see below). The header button group sets it.
    property string viewMode: "ungrouped"

    // Folder view: the directory currently shown. "" = the root level (library
    // root folders as tiles, plus any orphan photos in the grid).
    property string currentFolder: ""
    // The navigable entries (up tile + subfolders, or the roots) for currentFolder.
    // Refreshed explicitly since folderEntries() isn't a notifying property.
    property var folderEntries: []
    function refreshFolderEntries() {
        win.folderEntries = (win.viewMode === "folder") ? library.folderEntries(win.currentFolder) : []
    }

    onViewModeChanged: {
        if (win.viewMode === "folder") {
            win.currentFolder = ""
            win.refreshFolderEntries()
        }
        win.scrollGalleryToTop()
    }
    onCurrentFolderChanged: {
        // Entering a new folder is a fresh context; drop the previous selection.
        win.selectedIds = []
        win.selectedFolders = []
        win.fvAnchor = -1
        win.fvCursor = -1
        win.refreshFolderEntries()
        win.scrollGalleryToTop()
    }

    // Alt+Up climbs one folder level while browsing folders, mirroring the Up
    // button. Disabled at the root (nothing to climb to).
    Shortcut {
        sequence: "Alt+Up"
        enabled: win.viewMode === "folder" && win.currentFolder !== ""
        onActivated: win.fvGoUp()
    }

    // --- Tab focus cycle: add-a-tag field → tag filter → gallery → main search ---
    // Four stops. focusStop(i) returns false when stop i is currently unavailable
    // (the add-tag field with no photo selected, or the tag filter with no tags),
    // so tabStepTo skips it and keeps going the same direction. The gallery stop
    // routes to whichever of the three views is active.
    function focusGalleryView() {
        // Deferred: moving focus to the view synchronously from the text field's
        // own Tab key event doesn't stick (the GridView/ListView ends up handing
        // focus to an internal item). Run it after the key event settles.
        Qt.callLater(function () {
            if (win.viewMode === "timeline") timelineView.focusView()
            else if (win.viewMode === "folder") folderView.focusView()
            else grid.forceActiveFocus()
        })
    }
    function focusStop(i) {
        if (i === 0) return detailPanel.focusTagInput()   // false: no photo selected
        if (i === 1) return tagSidebar.focusSearch()      // false: no tags to filter
        if (i === 2) { win.focusGalleryView(); return true }
        if (i === 3) { gallerySearch.forceActiveFocus(); return true }
        return false
    }
    function tabStepTo(index, forward) {
        for (let k = 0; k < 4; ++k) {
            const i = ((index % 4) + 4) % 4
            if (win.focusStop(i))
                return
            index += forward ? 1 : -1
        }
    }

    // Brief, self-dismissing status message (wallpaper added, Aether failed, …).
    function toast(message) {
        toastLabel.text = message
        toastPopup.open()
        toastTimer.restart()
    }

    // The unshuffled slideshow playlist (grid order), kept so shuffle can be
    // toggled live.
    property var slideshowBase: []
    // Image-shuffle state for the CURRENT slideshow only; seeded from the saved
    // setting at start, toggled by 's' without changing the saved default.
    property bool slideshowShuffleActive: false
    // Folder-shuffle state for the CURRENT slideshow: play each folder's photos
    // together but visit the folders in random order. Seeded from the saved
    // setting at start; folder order stays fixed while 's' toggles image shuffle.
    property bool slideshowFolderShuffleActive: false

    // The folder (directory) portion of a source URL/path.
    function folderOf(src) {
        const s = "" + src
        const slash = s.lastIndexOf('/')
        return slash >= 0 ? s.substring(0, slash) : ""
    }

    // Fisher-Yates shuffle of arr[from+1 .. end] in place, leaving arr[0..from]
    // pinned. from = -1 (default) shuffles the whole array.
    function shuffleTail(arr, from) {
        if (from === undefined)
            from = -1
        for (let i = arr.length - 1; i > from + 1; --i) {
            const j = from + 1 + Math.floor(Math.random() * (i - from))
            const t = arr[i]; arr[i] = arr[j]; arr[j] = t
        }
    }

    // The order in which folders first appear in a source list.
    function folderSequence(list) {
        const seen = ({})
        const seq = []
        for (let i = 0; i < list.length; ++i) {
            const d = folderOf(list[i])
            if (!seen[d]) { seen[d] = true; seq.push(d) }
        }
        return seq
    }

    // Reorder `base` (grid order) into folder-grouped play order: folders are
    // visited in random order (or `fixedFolderOrder` when given, so live image-
    // shuffle toggles don't re-scramble the folders), and photos within a folder
    // keep grid order unless `imageShuffle` is on. When `pinnedSrc` is given (and
    // no fixed order), its folder goes first and begins on that photo, so the
    // result opens on it.
    function orderByFolderShuffle(base, imageShuffle, pinnedSrc, fixedFolderOrder) {
        pinnedSrc = pinnedSrc || ""
        const groups = ({})
        const natural = []
        for (let k = 0; k < base.length; ++k) {
            const d = folderOf(base[k])
            if (groups[d] === undefined) { groups[d] = []; natural.push(d) }
            groups[d].push(base[k])
        }

        const pinnedDir = pinnedSrc !== "" ? folderOf(pinnedSrc) : null
        const usingFixed = fixedFolderOrder !== undefined && fixedFolderOrder.length > 0
        let dirs
        if (usingFixed) {
            dirs = []
            for (let i = 0; i < fixedFolderOrder.length; ++i)
                if (groups[fixedFolderOrder[i]] !== undefined && dirs.indexOf(fixedFolderOrder[i]) === -1)
                    dirs.push(fixedFolderOrder[i])
            // Append any folders the fixed order didn't mention (robustness).
            for (let i = 0; i < natural.length; ++i)
                if (dirs.indexOf(natural[i]) === -1) dirs.push(natural[i])
        } else {
            dirs = natural.slice()
            let pinnedCount = 0
            if (pinnedDir !== null && groups[pinnedDir] !== undefined) {
                dirs.splice(dirs.indexOf(pinnedDir), 1); dirs.unshift(pinnedDir); pinnedCount = 1
            }
            shuffleTail(dirs, pinnedCount - 1)
        }

        const out = []
        for (let di = 0; di < dirs.length; ++di) {
            let items = groups[dirs[di]].slice()
            const isPinnedFolder = !usingFixed && pinnedDir !== null && di === 0 && dirs[di] === pinnedDir
            if (isPinnedFolder) {
                const p = items.indexOf(pinnedSrc)
                if (imageShuffle) {
                    if (p > 0) { items.splice(p, 1); items.unshift(pinnedSrc) }
                    shuffleTail(items, 0) // keep the pinned photo first, shuffle the rest
                } else if (p > 0) {
                    items = items.slice(p).concat(items.slice(0, p)) // rotate to start on it
                }
            } else if (imageShuffle) {
                shuffleTail(items, -1)
            }
            for (let m = 0; m < items.length; ++m) out.push(items[m])
        }
        return out
    }

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

    // Clear the gallery selection (e.g. clicking empty space in the grid).
    function clearSelection() {
        selectedIds = []
        selectedFolders = []
        anchorIndex = -1
        cursorIndex = -1
        fvAnchor = -1
        fvCursor = -1
        grid.forceActiveFocus()
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
        Qt.callLater(function () {
            grid.positionViewAtBeginning()
            timelineView.toTop()
            folderView.toTop()
        })
    }

    // Set while a mutation that must keep the gallery in place is running, so the
    // foldersChanged handler skips its scroll-to-top (see withPreservedGalleryScroll).
    property bool preservingGalleryScroll: false

    // Run a mutation that refreshes the photo model (tag rename/delete, image
    // delete/remove) without the gallery jumping to the top. Those paths call
    // setPhotos(), a wholesale model reset that otherwise drops the scroll; capture
    // and restore contentY around it, deferred once more for the view's relayout.
    function withPreservedGalleryScroll(fn) {
        const y = grid.contentY
        preservingGalleryScroll = true
        try {
            fn()
        } finally {
            grid.contentY = y
            Qt.callLater(function () {
                grid.contentY = y
                win.preservingGalleryScroll = false
            })
        }
    }

    // Keep the gallery in place when applying/removing tags. Small edits update
    // the model in place and don't move the grid, but a large selection runs on
    // the worker thread and reloads the whole model (setPhotos) on completion,
    // which snaps the grid to the top. Capture contentY around the operation and
    // restore it — immediately for the synchronous (small) case, and again once
    // the background job finishes (busy returns to false) for the large case.
    property real galleryTagScrollY: 0
    property bool galleryTagScrollPending: false
    function withPreservedGalleryScrollTagging(fn) {
        const y = grid.contentY
        galleryTagScrollY = y
        fn()
        if (library.busy) {
            // The edit was handed to the worker; restore after it reloads the model.
            galleryTagScrollPending = true
        } else {
            grid.contentY = y
            Qt.callLater(function () { grid.contentY = y })
        }
    }

    Connections {
        target: library
        function onBusyChanged() {
            if (!library.busy && win.galleryTagScrollPending) {
                win.galleryTagScrollPending = false
                grid.contentY = win.galleryTagScrollY
                Qt.callLater(function () { grid.contentY = win.galleryTagScrollY })
            }
        }
    }

    // Drop any selected ids whose photos are no longer visible in the gallery —
    // either gone from the active set (a folder was disabled or removed) or hidden
    // by the current tag/search filter. This keeps context-menu actions and the
    // detail pane from touching photos the user can't actually see.
    function pruneSelection() {
        if (selectedIds.length === 0)
            return
        const kept = selectedIds.filter(function (id) { return library.filtered.containsId(id) })
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
        // Return keyboard focus to whichever view is showing, so arrow keys and
        // Space keep working right after a click.
        if (win.viewMode === "timeline")
            timelineView.focusView()
        else if (win.viewMode === "folder")
            folderView.focusView()
        else
            grid.forceActiveFocus()
    }

    // Open the right-click context menu for a photo, matching the grid's behavior:
    // a right-click on a photo outside the current multi-selection selects just
    // that one first; a right-click inside it keeps the whole selection. Shared by
    // the grid and timeline so both menus behave identically.
    function openPhotoMenu(index, id, fileName, isVideo) {
        if (win.selectedIds.length <= 1 || win.selectedIds.indexOf(id) === -1)
            win.handleClick(index, id, 0)
        win.menuPhotoId = id
        win.menuPhotoName = fileName
        win.menuPhotoIsVideo = isVideo === true
        if (win.selectedIds.length > 1)
            multiPhotoMenu.popup()
        else
            photoMenu.popup()
    }

    // --- Folder-view selection ----------------------------------------------
    // The folder grid indexes an optional Up cell, then folders, then photos.
    // Map a grid index to its item, or null for Up / an empty row.
    function fvItemAt(gridIndex) {
        const fc = folderView.folderCount
        if (gridIndex < fc) {
            const e = folderView.entries[gridIndex]
            return e ? { type: "folder", path: e.path } : null
        }
        const id = library.filtered.idAt(gridIndex - fc)
        return id >= 0 ? { type: "photo", id: id } : null
    }

    // Select the contiguous grid range [a, b] into selectedFolders/selectedIds.
    function fvApplyRange(a, b) {
        const lo = Math.min(a, b), hi = Math.max(a, b)
        const folders = [], ids = []
        for (let i = lo; i <= hi; ++i) {
            const it = fvItemAt(i)
            if (!it) continue
            if (it.type === "folder") folders.push(it.path)
            else ids.push(it.id)
        }
        selectedFolders = folders
        selectedIds = ids
    }

    // Click on a folder or photo cell: plain = replace, Ctrl = toggle, Shift =
    // range from the anchor. (The Up cell navigates instead; see FolderView.)
    function fvClick(gridIndex, modifiers) {
        const it = fvItemAt(gridIndex)
        if (!it)
            return
        if ((modifiers & Qt.ShiftModifier) && fvAnchor >= 0) {
            fvCursor = gridIndex
            fvApplyRange(fvAnchor, fvCursor)
        } else if (modifiers & Qt.ControlModifier) {
            if (it.type === "folder") {
                const arr = selectedFolders.slice()
                const p = arr.indexOf(it.path)
                if (p === -1) arr.push(it.path); else arr.splice(p, 1)
                selectedFolders = arr
            } else {
                const arr = selectedIds.slice()
                const p = arr.indexOf(it.id)
                if (p === -1) arr.push(it.id); else arr.splice(p, 1)
                selectedIds = arr
            }
            fvAnchor = gridIndex; fvCursor = gridIndex
        } else {
            if (it.type === "folder") { selectedFolders = [it.path]; selectedIds = [] }
            else { selectedIds = [it.id]; selectedFolders = [] }
            fvAnchor = gridIndex; fvCursor = gridIndex
        }
        folderView.focusView()
    }

    // Double-click: enter a folder, or view a photo.
    function fvDoubleClick(gridIndex) {
        const fc = folderView.folderCount
        if (gridIndex < fc) {
            const e = folderView.entries[gridIndex]
            if (e)
                win.fvEnterFolder(e.path)
        } else {
            const id = library.filtered.idAt(gridIndex - fc)
            if (id >= 0) win.openViewer(id)
        }
    }

    function fvRightClick(gridIndex) {
        const fc = folderView.folderCount
        if (gridIndex < fc)
            return // no context menu for folders
        const row = gridIndex - fc
        const id = library.filtered.idAt(row)
        if (id < 0)
            return
        const ph = library.filtered.photoAt(row)
        win.openPhotoMenu(row, id, ph ? ph.fileName : "", ph ? ph.isVideo === true : false)
    }

    // Ctrl+A in the folder grid selects every folder and photo shown.
    function fvSelectAll() {
        const total = folderView.folderCount + library.filtered.count
        if (total === 0) return
        fvApplyRange(0, total - 1)
        fvAnchor = 0; fvCursor = total - 1
    }

    // Arrow-key navigation over the folder grid (folders then photos). Plain
    // arrows move a single selection; Shift+arrows extend a range from the
    // anchor. Mirrors the gallery's navigate(), but spans both item kinds.
    function fvNavigate(dir, extend) {
        const total = folderView.folderCount + library.filtered.count
        if (total === 0)
            return
        const cols = Math.max(1, folderView.columns)
        let base = (fvCursor >= 0 && fvCursor < total) ? fvCursor
                 : (fvAnchor >= 0 && fvAnchor < total) ? fvAnchor : 0
        let target = base
        if (selectedFolders.length > 0 || selectedIds.length > 0) {
            if (dir === "left")       target = base - 1
            else if (dir === "right") target = base + 1
            else if (dir === "up")    target = base - cols
            else if (dir === "down")  target = base + cols
        }
        target = Math.max(0, Math.min(total - 1, target))
        if (extend) {
            if (!fvItemAt(target)) return
            if (fvAnchor < 0 || fvAnchor >= total) fvAnchor = target
            fvCursor = target
            fvApplyRange(fvAnchor, fvCursor)
            folderView.ensureVisible(target)
        } else {
            win.fvSelectGridIndex(target)
        }
    }

    // Select just the item at grid index `i` (a folder or a photo), move the
    // cursor/anchor there, and scroll it into view.
    function fvSelectGridIndex(i) {
        const it = fvItemAt(i)
        if (!it)
            return
        if (it.type === "folder") { selectedFolders = [it.path]; selectedIds = [] }
        else { selectedIds = [it.id]; selectedFolders = [] }
        fvAnchor = i; fvCursor = i
        folderView.ensureVisible(i)
    }

    // Grid index of the folder entry with this path, or -1.
    function fvFolderIndexOf(path) {
        for (let i = 0; i < folderView.folderCount; ++i) {
            const e = folderView.entries[i]
            if (e && e.path === path)
                return i
        }
        return -1
    }

    // Descend into a folder, landing with its first item selected and the grid
    // focused so arrow keys work immediately.
    function fvEnterFolder(path) {
        win.currentFolder = path        // onCurrentFolderChanged clears selection + refreshes entries
        win.fvSelectGridIndex(0)
        win.focusGalleryView()
    }

    // Climb one level, landing with the folder just left selected and the grid
    // focused. No-op at the root.
    function fvGoUp() {
        if (win.currentFolder === "")
            return
        const from = win.currentFolder
        win.currentFolder = library.upFolder(from)
        const idx = win.fvFolderIndexOf(from)
        win.fvSelectGridIndex(idx >= 0 ? idx : 0)
        win.focusGalleryView()
    }

    // Commit the selection cursor to a proxy row. Plain move replaces the
    // selection; extend (Shift) grows a contiguous range from the anchor. View-
    // agnostic: the grid and the timeline both drive selection through this, each
    // handling its own scrolling. Returns true when the move was applied.
    function moveCursorTo(target, extend) {
        const count = library.filtered.count
        if (count === 0)
            return false
        target = Math.max(0, Math.min(count - 1, target))
        if (extend) {
            if (anchorIndex < 0 || anchorIndex >= count)
                anchorIndex = target
            cursorIndex = target
            selectRange(anchorIndex, cursorIndex)
        } else {
            const id = library.filtered.idAt(target)
            if (id < 0)
                return false
            selectedIds = [id]
            anchorIndex = target
            cursorIndex = target
        }
        return true
    }

    // Arrow-key navigation for the grid. Plain arrows move a single selection;
    // Shift+arrows extend a contiguous selection from the anchor to a moving
    // cursor. (The timeline has its own month-aware navigation; both commit
    // through moveCursorTo.)
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

        if (moveCursorTo(target, extend))
            grid.positionViewAtIndex(target, GridView.Contain)
    }

    // Start a slideshow at the currently focused photo. play=false opens paused.
    function slideshowFromFocus(play) {
        if (win.viewMode === "folder") {
            win.startFolderSlideshow(-1, play)
            return
        }
        const startId = (anchorIndex >= 0 && anchorIndex < library.filtered.count)
                        ? library.filtered.idAt(anchorIndex) : -1
        startSlideshow(startId, play)
    }

    // Open the single-item viewer (fit/pan/zoom, left/right nav) over the same set
    // a slideshow would use — the selection when several are selected, otherwise
    // the whole view — in grid order (unshuffled), positioned at startId. This is
    // the Enter / double-click "view" action; the viewer offers its own slideshow.
    function openViewer(startId) {
        let srcs = []
        let startSrc = ""
        if (win.viewMode === "folder") {
            let folderPaths = []
            let photoIds = []
            const selectedCount = win.selectedFolders.length + win.selectedIds.length
            if (selectedCount > 1) {
                folderPaths = win.selectedFolders.slice()
                photoIds = win.selectedIds.slice()
            } else {
                for (let i = 0; i < folderView.entries.length; ++i) {
                    const e = folderView.entries[i]
                    if (e && e.type === "folder")
                        folderPaths.push(e.path)
                }
                for (let r = 0; r < library.filtered.count; ++r)
                    photoIds.push(library.filtered.idAt(r))
            }
            srcs = library.folderSlideshowSources(folderPaths, photoIds)
            if (startId >= 0) {
                for (let r = 0; r < library.filtered.count; ++r) {
                    if (library.filtered.idAt(r) === startId) {
                        startSrc = "" + library.filtered.sourceAt(r)
                        break
                    }
                }
            }
        } else {
            const selectedOnly = selectedIds.length > 1
            const n = library.filtered.count
            for (let r = 0; r < n; ++r) {
                const id = library.filtered.idAt(r)
                if (selectedOnly && selectedIds.indexOf(id) === -1)
                    continue
                const s = "" + library.filtered.sourceAt(r)
                if (id === startId)
                    startSrc = s
                srcs.push(s)
            }
        }
        if (srcs.length === 0)
            return
        photoViewer.sources = srcs
        photoViewer.openAt(Math.max(0, srcs.indexOf(startSrc)))
    }

    // Enter in the grid/timeline: view the focused photo. (Folder view routes its
    // own Enter — a selected folder is entered, a photo opens here.)
    function openViewerFromFocus() {
        if (win.viewMode === "folder") {
            // Open at the focused photo when a single one is selected, else the set.
            win.openViewer(win.selectedIds.length === 1 ? win.selectedIds[0] : -1)
            return
        }
        const startId = (anchorIndex >= 0 && anchorIndex < library.filtered.count)
                        ? library.filtered.idAt(anchorIndex) : -1
        win.openViewer(startId)
    }

    // Build the slideshow set for the grid/timeline: the selection when more than
    // one is selected, otherwise the whole filtered set — in grid order, starting
    // at startId. Delegates the shuffle/start handling to playSlideshow.
    function startSlideshow(startId, play, forceStart) {
        const selectedOnly = selectedIds.length > 1
        const srcs = []
        let startSrc = ""
        const n = library.filtered.count
        for (let r = 0; r < n; ++r) {
            const id = library.filtered.idAt(r)
            if (selectedOnly && selectedIds.indexOf(id) === -1)
                continue
            const s = "" + library.filtered.sourceAt(r)
            if (id === startId)
                startSrc = s
            srcs.push(s)
        }
        playSlideshow(srcs, startSrc, play, forceStart)
    }

    // Build the slideshow set for the folder view: the contents of every selected
    // folder (recursively) plus every selected photo when more than one item is
    // selected; otherwise everything in the current folder (all subfolders'
    // contents plus the loose photos) — the folder-view "whole set". startId, when
    // >= 0, is a loose photo to open on (double-clicked).
    function startFolderSlideshow(startId, play) {
        let folderPaths = []
        let photoIds = []
        const selectedCount = win.selectedFolders.length + win.selectedIds.length
        if (selectedCount > 1) {
            folderPaths = win.selectedFolders.slice()
            photoIds = win.selectedIds.slice()
        } else {
            // All siblings: every folder shown here plus every loose photo.
            for (let i = 0; i < folderView.entries.length; ++i) {
                const e = folderView.entries[i]
                if (e && e.type === "folder")
                    folderPaths.push(e.path)
            }
            for (let r = 0; r < library.filtered.count; ++r)
                photoIds.push(library.filtered.idAt(r))
        }
        const srcs = library.folderSlideshowSources(folderPaths, photoIds)
        // Open on the double-clicked loose photo when there is one.
        let startSrc = ""
        if (startId >= 0) {
            for (let r = 0; r < library.filtered.count; ++r) {
                if (library.filtered.idAt(r) === startId) {
                    startSrc = "" + library.filtered.sourceAt(r)
                    break
                }
            }
        }
        playSlideshow(srcs, startSrc, play, false)
    }

    // Shared slideshow runner: given the source list and an optional source to
    // open on, apply the shuffle / folder-shuffle / random-start settings and
    // start the show. play=false opens paused; forceStart honors startSrc even
    // when playing (Enter / double-click / "Play from here").
    function playSlideshow(srcs, startSrc, play, forceStart) {
        if (!srcs || srcs.length === 0)
            return

        // Open on a specific photo (and ignore random start) for paused opens and
        // "Play from here"; Play/Space otherwise let random start win.
        const honorStart = (startSrc !== "") && (!play || forceStart === true)
        let startPos = 0
        if (startSrc !== "") {
            const p = srcs.indexOf(startSrc)
            if (p >= 0)
                startPos = p
        }

        slideshowBase = srcs.slice()
        slideshowShuffleActive = appSettings.shuffle
        slideshowFolderShuffleActive = appSettings.shuffleFolders

        const randomize = appSettings.randomStart && !honorStart
        const pin = (startSrc !== "") && !randomize

        if (slideshowFolderShuffleActive) {
            // Group by folder and visit the folders in random order; image shuffle
            // (if on) reorders photos within each folder. A honored start photo
            // opens the show from its folder.
            const pinnedSrc = (pin && startSrc !== "") ? startSrc : ""
            srcs.splice(0, srcs.length)
            const ordered = orderByFolderShuffle(slideshowBase, slideshowShuffleActive, pinnedSrc)
            for (let i = 0; i < ordered.length; ++i) srcs.push(ordered[i])
            startPos = 0
        } else if (slideshowShuffleActive) {
            // Pin the chosen start photo first and shuffle the rest — unless
            // random start applies, in which case shuffle everything so the
            // slideshow opens on a random photo.
            if (pin && startPos > 0) {
                const s = srcs[0]; srcs[0] = srcs[startPos]; srcs[startPos] = s
            }
            shuffleTail(srcs, pin ? 0 : -1)
            startPos = 0
        } else if (randomize) {
            // Random start applies to Play/Space (and whenever there's no specific
            // photo to honor) — within the selection if several are selected,
            // otherwise the whole set. Enter/double-click keep their photo.
            startPos = Math.floor(Math.random() * srcs.length)
        }

        slideshow.sources = srcs
        slideshow.startAt(startPos, play)
    }

    // Toggle image shuffle while the slideshow is running: reorder the upcoming
    // photos but keep the current one on screen.
    function toggleSlideshowShuffle() {
        slideshowShuffleActive = !slideshowShuffleActive
        if (slideshowBase.length === 0)
            return

        const cur = slideshow.currentSource

        if (slideshowFolderShuffleActive) {
            // Keep the folders in their current order (don't re-scramble them on an
            // image-shuffle toggle); only the within-folder order changes. The
            // current photo stays on screen at its new position.
            const order = orderByFolderShuffle(slideshowBase, slideshowShuffleActive,
                                                "", folderSequence(slideshow.sources))
            slideshow.setSources(order, Math.max(0, order.indexOf(cur)))
            return
        }

        const list = slideshowBase.slice()
        if (slideshowShuffleActive) {
            const idx = list.indexOf(cur)
            if (idx > 0) {
                list.splice(idx, 1)
                list.unshift(cur)
            }
            shuffleTail(list, 0) // keep the current photo at the front
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
        // Visit enabled folders in random order (photos within a folder stay
        // together); independent of the per-photo `shuffle` above.
        property bool shuffleFolders: false
        property bool fade: true
        property bool randomStart: false
        property bool loop: false
    }

    Settings {
        id: gallerySettings
        category: "gallery"
        // Index into win.thumbSizes; 2 == "md", the original thumbnail size.
        property int thumbIndex: 2
        // true: crop thumbnails to a square (original behavior). false: fit the
        // whole image, letterboxed to its true aspect ratio.
        property bool cropThumbs: true
        // true: group photos by containing folder before sorting by file name.
        // false (original): sort by file name across all folders.
        property bool folderFirst: false
        // Central view mode: "ungrouped" (the flat grid), "timeline" (grouped by
        // capture month), or "folder" (navigable library folders).
        property string viewMode: "ungrouped"
    }

    // Keep the gallery sort order in step with the saved setting (also applies it
    // once the models exist at startup).
    Binding {
        target: library.filtered
        property: "folderFirst"
        value: gallerySettings.folderFirst
    }

    // Restore the saved view mode at startup, then persist any later change.
    Component.onCompleted: win.viewMode = gallerySettings.viewMode
    Binding {
        target: gallerySettings
        property: "viewMode"
        value: win.viewMode
    }

    // Drive the filter model's mode from the current view: timeline sorts by
    // capture date; folder view restricts to one directory level (currentFolder).
    Binding {
        target: library.filtered
        property: "sortByDate"
        value: win.viewMode === "timeline"
    }
    Binding {
        target: library.filtered
        property: "folderMode"
        value: win.viewMode === "folder"
    }
    Binding {
        target: library.filtered
        property: "folderPath"
        value: win.currentFolder
    }
    // Duplicates are collapsed only in the ungrouped view; timeline and folder
    // views show every copy.
    Binding {
        target: library.filtered
        property: "dedupe"
        value: win.viewMode === "ungrouped"
    }

    // The modal dialogs are lazy-loaded: their (sizable) content is only built
    // the first time each is opened, instead of instantiating and laying out at
    // startup where it dominated launch time. openX() activates the Loader (a
    // synchronous Loader instantiates immediately) and opens the dialog; the
    // built instance is kept for subsequent opens.
    Loader {
        id: libraryManagerLoader
        // Fill the window so the dialog's `anchors.centerIn: parent` still centers
        // on the window (the Loader is this dialog's parent). Empty while inactive.
        anchors.fill: parent
        active: false
        sourceComponent: LibraryManager {
            library: library
            accent: win.accent
            gallerySettings: gallerySettings
        }
    }
    function openLibraryManager() {
        libraryManagerLoader.active = true
        libraryManagerLoader.item.open()
    }

    // Folder picker for the header's Import button (the Library dialog has its
    // own, so this one lets you import without opening that dialog first).
    FolderDialog {
        id: importDialog
        title: qsTr("Choose a folder to add")
        currentFolder: StandardPaths.writableLocation(StandardPaths.HomeLocation)
        onAccepted: library.importDirectory(selectedFolder)
    }

    Loader {
        id: tagManagerLoader
        anchors.fill: parent
        active: false
        sourceComponent: TagManager {
            library: library
            accent: win.accent
            // Renaming or deleting a tag refreshes the photo model, which would
            // drop the gallery's scroll; keep it in place across the mutation.
            galleryScrollGuard: win.withPreservedGalleryScroll
        }
    }
    function openTagManager() {
        tagManagerLoader.active = true
        tagManagerLoader.item.open()
    }

    // When the active photo set changes (folder enabled/disabled/removed, rescan,
    // orphan cleanup), drop selected photos that are no longer shown.
    Connections {
        target: library
        function onFoldersChanged() {
            win.pruneSelection()
            // The folder set or active photos changed; rebuild the folder-view
            // navigation (handles a now-removed currentFolder too — its photos and
            // subfolder tiles simply vanish, and Up still walks back by string).
            win.refreshFolderEntries()
            // A single image delete/remove keeps its scroll position; only broader
            // changes (folders added/removed, rescan, orphan cleanup) re-anchor.
            if (!win.preservingGalleryScroll)
                win.scrollGalleryToTop()
        }
    }

    // When the tag/search filter changes, drop selected photos the new filter
    // hides. The filter setters invalidate the proxy before emitting these, so
    // containsId() already reflects the new result set here.
    Connections {
        target: library.filtered
        function onSelectedTagIdsChanged() { win.pruneSelection() }
        function onMatchAllChanged() { win.pruneSelection() }
        function onSearchTextChanged() { win.pruneSelection() }
        function onTagPresenceChanged() { win.pruneSelection() }
    }

    // Changing the tag filter re-anchors the gallery to the top of the results.
    Connections {
        target: library.tags
        function onSelectionChanged() { win.scrollGalleryToTop() }
    }

    // Push the sidebar's tag selection, match mode, and tagged/untagged filter
    // into the gallery filter model.
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
    Binding {
        target: library.filtered
        property: "tagPresence"
        value: tagSidebar.tagPresence
    }

    // Status bar: photo/selection count always, plus a progress bar for
    // non-blocking background jobs (bulk tagging). Blocking jobs use the overlay.
    footer: ToolBar {
        id: statusBar

        // Keep the status bar slim: the ToolButton's default padding would
        // otherwise dictate a much taller bar.
        topPadding: 2
        bottomPadding: 2

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
                // Compact fixed width so it reads as a status element rather than
                // stretching across the bar.
                Layout.preferredWidth: 100
                indeterminate: library.progress < 0
                from: 0; to: 1
                value: library.progress < 0 ? 0 : library.progress
            }

            // Toggle thumbnails between cropped-to-square and whole-image (fit).
            ToolButton {
                id: cropFitButton
                readonly property string cropLabel: qsTr("◼ Crop")
                readonly property string fitLabel: qsTr("▭  Fit")
                text: gallerySettings.cropThumbs ? cropLabel : fitLabel
                // Plain toggle: flip the mode on click. Not `checkable`, so it
                // never sticks in a highlighted state — same look in both modes.
                onClicked: gallerySettings.cropThumbs = !gallerySettings.cropThumbs
                // Compact: trim the default padding so it doesn't inflate the bar.
                font.pixelSize: 12
                topPadding: 3
                bottomPadding: 3
                leftPadding: 10
                rightPadding: 10
                // Size to the wider of the two labels so the button width stays
                // constant when the text swaps — measured by pixel width (not
                // character count: the ◼/▭ glyphs render at different widths), so
                // it never clips and tracks the font/theme, not a magic number.
                TextMetrics {
                    id: cropMetrics
                    font: cropFitButton.font
                    text: cropFitButton.cropLabel
                }
                TextMetrics {
                    id: fitMetrics
                    font: cropFitButton.font
                    text: cropFitButton.fitLabel
                }
                Layout.preferredWidth: Math.ceil(Math.max(cropMetrics.advanceWidth,
                                                          fitMetrics.advanceWidth))
                                       + leftPadding + rightPadding
                Layout.preferredHeight: 24
                ToolTip.visible: hovered
                ToolTip.text: gallerySettings.cropThumbs
                              ? qsTr("Crop thumbnails to squares")
                              : qsTr("Fit whole image in thumbnail")
            }

            // Thumbnail size control: drag to pick xs / sm / md / lg / xl. Flanked
            // by small and large glyphs so the direction reads at a glance.
            Label {
                text: "▪"
                color: palette.placeholderText
                font.pixelSize: 11
            }
            Slider {
                id: thumbSizeSlider
                Layout.preferredWidth: 90
                Layout.preferredHeight: 24
                from: 0
                to: win.thumbSizes.length - 1
                stepSize: 1
                snapMode: Slider.SnapAlways
                value: gallerySettings.thumbIndex
                onMoved: gallerySettings.thumbIndex = Math.round(value)

                ToolTip.visible: hovered || pressed
                ToolTip.text: ["Extra small", "Small", "Medium", "Large", "Extra large"][win.thumbIndex]
            }
            Label {
                text: "◼"
                color: palette.placeholderText
                font.pixelSize: 16
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
                        onTriggered: win.openLibraryManager()
                    }
                    MenuItem {
                        text: qsTr("Tags…")
                        onTriggered: win.openTagManager()
                    }
                }
            }

            ToolButton {
                text: qsTr("Import…")
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Add a folder of photos to the library")
                onClicked: importDialog.open()
            }

            // Equal flex spacers on either side center the view-mode switch in the
            // gap between the Import button and the search box.
            Item { Layout.fillWidth: true }

            // View mode: switch the central area between the flat grid, a
            // capture-month timeline, and a navigable folder tree. Exclusive, so
            // exactly one is active; the choice is persisted in gallerySettings.
            ButtonGroup { id: viewModeGroup; exclusive: true }
            Row {
                spacing: 0
                ToolButton {
                    text: qsTr("Ungrouped")
                    checkable: true
                    ButtonGroup.group: viewModeGroup
                    checked: win.viewMode === "ungrouped"
                    onClicked: win.viewMode = "ungrouped"
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("All photos in one grid")
                }
                ToolButton {
                    text: qsTr("Timeline")
                    checkable: true
                    ButtonGroup.group: viewModeGroup
                    checked: win.viewMode === "timeline"
                    onClicked: win.viewMode = "timeline"
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Grouped by the month each photo was taken")
                }
                ToolButton {
                    text: qsTr("Folders")
                    checkable: true
                    ButtonGroup.group: viewModeGroup
                    checked: win.viewMode === "folder"
                    onClicked: win.viewMode = "folder"
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Browse your library folders")
                }
            }

            Item { Layout.fillWidth: true }

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
                // Tab cycle: main search → add-a-tag (wrap); Shift+Tab → gallery.
                Keys.onTabPressed: (event) => { win.tabStepTo(0, true); event.accepted = true }
                Keys.onBacktabPressed: (event) => { win.tabStepTo(2, false); event.accepted = true }
            }

            ToolButton {
                // In folder view the selection may include folders as well as photos.
                readonly property int selCount: win.selectedIds.length
                    + (win.viewMode === "folder" ? win.selectedFolders.length : 0)
                text: selCount > 1 ? qsTr("▶  Slideshow (%1)").arg(selCount)
                                   : qsTr("▶  Slideshow")
                enabled: library.filtered.count > 0
                         || (win.viewMode === "folder" && win.folderEntries.length > 0)
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
                id: detailPanel
                SplitView.preferredHeight: 340
                SplitView.minimumHeight: 160
                library: library
                selectedIds: win.selectedIds
                accent: win.accent
                // Keep the gallery from jumping when a tag edit reloads the model.
                tagScrollGuard: win.withPreservedGalleryScrollTagging
                // Tab cycle: add-a-tag → tag filter; Shift+Tab → main search.
                onTabForward: win.tabStepTo(1, true)
                onTabBackward: win.tabStepTo(3, false)
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
                // The tagged/untagged filter also re-anchors the gallery.
                onTagPresenceChanged: win.scrollGalleryToTop()
                // Tab cycle: tag filter → gallery; Shift+Tab → add-a-tag.
                onTabForward: win.tabStepTo(2, true)
                onTabBackward: win.tabStepTo(0, false)
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: palette.base

            GridView {
                id: grid
                anchors.fill: parent
                visible: win.viewMode === "ungrouped"
                clip: true
                cellWidth: win.cellSize
                cellHeight: win.cellSize
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
                Keys.onReturnPressed: (event) => { win.openViewerFromFocus(); event.accepted = true }
                Keys.onEnterPressed: (event) => { win.openViewerFromFocus(); event.accepted = true }
                // Tab cycle: gallery → main search; Shift+Tab → tag filter.
                Keys.onTabPressed: (event) => { win.tabStepTo(3, true); event.accepted = true }
                Keys.onBacktabPressed: (event) => { win.tabStepTo(1, false); event.accepted = true }

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
                    crop: gallerySettings.cropThumbs
                    filePath: model.filePath
                    isVideo: model.isVideo === true
                    tagCount: model.tags.length
                    selected: win.selectedIds.indexOf(model.id) !== -1
                    onClicked: (modifiers) => win.handleClick(index, model.id, modifiers)
                    onDoubleClicked: win.openViewer(model.id)
                    onRightClicked: win.openPhotoMenu(index, model.id, model.fileName, model.isVideo === true)
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
                visible: grid.visible
                enabled: grid.visible
                acceptedButtons: Qt.NoButton
                propagateComposedEvents: true
                onWheel: (wheel) => {
                    // A GridView's top extent is originY, not 0 — it drifts as
                    // delegates above the viewport are recycled. Clamping to 0
                    // would let the wheel push the content above the first row.
                    const minY = grid.originY
                    const maxY = Math.max(minY, grid.originY + grid.contentHeight - grid.height)
                    if (wheel.pixelDelta.y !== 0) {
                        // High-resolution trackpad: amplified, immediate.
                        wheelAnim.stop()
                        grid.contentY = Math.max(minY, Math.min(maxY, grid.contentY - wheel.pixelDelta.y * 4.0))
                    } else {
                        // Mouse-wheel notches: glide one row per notch with
                        // momentum (tied to cell height, not the viewport, so one
                        // notch is a single row rather than most of a page).
                        const step = (wheel.angleDelta.y / 120) * grid.cellHeight
                        const target = Math.max(minY, Math.min(maxY, grid.contentY - step))
                        wheelAnim.stop()
                        wheelAnim.to = target
                        wheelAnim.start()
                    }
                }
            }

            // Clicking empty gallery space (the right remainder left by column
            // wrapping, or below the last row) clears the selection. Sits above
            // the grid and hit-tests each press: presses on a photo cell fall
            // through to its delegate (so selection and drag-flicking still
            // work), only empty-space presses are handled here.
            MouseArea {
                anchors.fill: parent
                visible: grid.visible
                enabled: grid.visible
                acceptedButtons: Qt.LeftButton
                onPressed: (mouse) => {
                    const idx = grid.indexAt(grid.contentX + mouse.x, grid.contentY + mouse.y)
                    // Keep the press only when it missed every photo; otherwise
                    // let it reach the GridView and the delegate beneath.
                    mouse.accepted = (idx === -1)
                }
                onClicked: win.clearSelection()
            }

            Label {
                anchors.centerIn: parent
                visible: grid.visible && grid.count === 0
                text: (library.tags.selectedTagIds.length === 0 && gallerySearch.text.trim() === ""
                       && tagSidebar.tagPresence === 0)
                      ? qsTr("Import a folder to get started.")
                      : qsTr("No photos match the current filters.")
                color: palette.placeholderText
            }

            // Folder browser: folders (2x2 cover mosaics) and the current folder's
            // photos in one photo-style grid. Photos share library.filtered (the
            // folderMode/folderPath bindings restrict it to the current folder), so
            // selection/slideshow route through the same win helpers.
            FolderView {
                id: folderView
                anchors.fill: parent
                visible: win.viewMode === "folder"
                library: library
                entries: win.folderEntries
                currentFolder: win.currentFolder
                accent: win.accent
                crop: gallerySettings.cropThumbs
                cellSize: win.cellSize
                selectedIds: win.selectedIds
                selectedFolders: win.selectedFolders
                onGoUp: win.fvGoUp()
                onEnterFolder: (path) => win.fvEnterFolder(path)
                onCellClicked: (gridIndex, modifiers) => win.fvClick(gridIndex, modifiers)
                onCellDoubleClicked: (gridIndex) => win.fvDoubleClick(gridIndex)
                onCellRightClicked: (gridIndex) => win.fvRightClick(gridIndex)
                onSelectAllRequested: win.fvSelectAll()
                onCursorMove: (dir, extend) => win.fvNavigate(dir, extend)
                onSlideshowRequested: (play) => play ? win.slideshowFromFocus(true) : win.openViewerFromFocus()
                // Tab cycle: gallery → main search; Shift+Tab → tag filter.
                onTabForward: win.tabStepTo(3, true)
                onTabBackward: win.tabStepTo(1, false)
            }

            // Capture-month timeline. Shares library.filtered (sorted by date in
            // this mode) and routes selection/slideshow back through the same win
            // helpers the grid uses, so selection is consistent across views.
            TimelineView {
                id: timelineView
                anchors.fill: parent
                visible: win.viewMode === "timeline"
                library: library
                accent: win.accent
                crop: gallerySettings.cropThumbs
                cellSize: win.cellSize
                selectedIds: win.selectedIds
                cursorIndex: win.cursorIndex
                anchorIndex: win.anchorIndex
                emptyFiltered: library.tags.selectedTagIds.length === 0
                               && gallerySearch.text.trim() === "" && tagSidebar.tagPresence === 0
                onPhotoClicked: (index, id, modifiers) => win.handleClick(index, id, modifiers)
                onPhotoDoubleClicked: (id) => win.openViewer(id)
                onPhotoRightClicked: (index, id, fileName, isVideo) => win.openPhotoMenu(index, id, fileName, isVideo)
                onSelectAllRequested: win.selectAll()
                onMoveCursorRequested: (target, extend) => win.moveCursorTo(target, extend)
                onSlideshowFromFocusRequested: (play) => play ? win.slideshowFromFocus(true) : win.openViewerFromFocus()
                // Tab cycle: gallery → main search; Shift+Tab → tag filter.
                onTabForward: win.tabStepTo(3, true)
                onTabBackward: win.tabStepTo(1, false)
            }
        }
    }

    SlideshowView {
        id: slideshow
        intervalMs: appSettings.intervalMs
        shuffle: win.slideshowShuffleActive
        transitionMs: appSettings.fade ? 280 : 0
        loop: appSettings.loop
        onToggleShuffleRequested: win.toggleSlideshowShuffle()
        onToggleLoopRequested: appSettings.loop = !appSettings.loop

        // Theme actions from the slideshow's context menu. The Library lives here,
        // and feedback goes to the slideshow's own toast (its window is on top).
        onAddWallpaperRequested: (source) => slideshow.flashMessage(
            library.addToThemeWallpapersUrl(source)
                ? qsTr("Added to theme wallpapers")
                : qsTr("Couldn't add to theme wallpapers"))
        onEditInAetherRequested: (source) => {
            // Editing opens Aether's GUI to switch to, so leave the fullscreen
            // slideshow on success; stay and report if it couldn't launch.
            if (library.editThemeInAetherUrl(source))
                slideshow.stop()
            else
                slideshow.flashMessage(qsTr("Couldn't launch Aether"))
        }
        onSetAetherThemeRequested: (source) => {
            if (!library.setAetherThemeUrl(source))
                slideshow.flashMessage(qsTr("Couldn't launch Aether"))
        }
    }

    // Single-item viewer (fit/pan/zoom), the Enter / double-click "view" action.
    // Full screen here so it reads like the slideshow it replaces; its own "start
    // slideshow" hands the current set to the SlideshowView above.
    PhotoViewer {
        id: photoViewer
        preferredVisibility: Window.FullScreen
        onSlideshowRequested: {
            photoViewer.visibility = Window.Hidden
            win.playSlideshow(photoViewer.sources.slice(), photoViewer.currentSource, true, true)
        }
        onCloseRequested: {
            photoViewer.visibility = Window.Hidden
            win.focusGalleryView()
        }
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
                    id: intervalSpin
                    // Values are in tenths of a second, so the interval is
                    // adjustable down to 0.1 s (0.1 s … 120.0 s).
                    from: 1
                    to: 1200
                    editable: true
                    value: Math.round(appSettings.intervalMs / 100)
                    onValueModified: appSettings.intervalMs = value * 100

                    // Step by a whole second (10 tenths) at or above 1 s, and by a
                    // tenth once below it. Keyed off which indicator is pressed so
                    // the 1 s boundary is handled per direction: from 1.0 s, down
                    // goes to 0.9 s but up goes to 2.0 s.
                    stepSize: intervalSpin.down.pressed ? (value > 10 ? 10 : 1)
                                                        : (value >= 10 ? 10 : 1)

                    textFromValue: function (value, locale) {
                        return Number(value / 10).toLocaleString(locale, 'f', 1)
                    }
                    valueFromText: function (text, locale) {
                        return Math.round(Number.fromLocaleString(locale, text) * 10)
                    }
                    validator: DoubleValidator {
                        bottom: intervalSpin.from / 10
                        top: intervalSpin.to / 10
                        decimals: 1
                        notation: DoubleValidator.StandardNotation
                    }
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
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Label {
                        text: qsTr("Shuffle folders")
                    }
                    Label {
                        text: qsTr("Play enabled folders in random order, each folder's photos together")
                        font.pixelSize: 12
                        color: palette.placeholderText
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                }
                Switch {
                    checked: appSettings.shuffleFolders
                    onToggled: appSettings.shuffleFolders = checked
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

            RowLayout {
                Layout.fillWidth: true
                spacing: 16
                Label {
                    text: qsTr("Loop videos")
                    Layout.fillWidth: true
                }
                Switch {
                    checked: appSettings.loop
                    onToggled: appSettings.loop = checked
                }
            }
        }
    }

    // Right-click context menu for a single photo (win.menuPhotoId). A
    // multi-selection gets its own menu below.
    Menu {
        id: photoMenu

        // Slideshow first so the top item is the primary action.
        MenuItem {
            text: qsTr("Play slideshow")
            onTriggered: win.startSlideshow(win.menuPhotoId, true, true)
        }
        MenuItem {
            text: win.menuPhotoIsVideo ? qsTr("Play video") : qsTr("View photo")
            onTriggered: win.openViewer(win.menuPhotoId)
        }

        // Theme actions apply to still images only.
        MenuSeparator { visible: !win.menuPhotoIsVideo; height: visible ? implicitHeight : 0 }
        MenuItem {
            visible: !win.menuPhotoIsVideo
            height: visible ? implicitHeight : 0
            text: qsTr("Add to theme wallpapers")
            onTriggered: win.toast(library.addToThemeWallpapers(win.menuPhotoId)
                                   ? qsTr("Added to theme wallpapers")
                                   : qsTr("Couldn't add to theme wallpapers"))
        }
        MenuItem {
            visible: !win.menuPhotoIsVideo
            height: visible ? implicitHeight : 0
            text: qsTr("Edit theme in Aether")
            onTriggered: {
                if (!library.editThemeInAether(win.menuPhotoId))
                    win.toast(qsTr("Couldn't launch Aether"))
            }
        }
        MenuItem {
            visible: !win.menuPhotoIsVideo
            height: visible ? implicitHeight : 0
            text: qsTr("Set as Aether theme")
            onTriggered: {
                if (!library.setAetherTheme(win.menuPhotoId))
                    win.toast(qsTr("Couldn't launch Aether"))
            }
        }

        MenuSeparator {}

        MenuItem {
            text: qsTr("Show in file manager")
            onTriggered: {
                if (!library.showInFileManager(win.menuPhotoId))
                    win.toast(qsTr("Couldn't open the file manager"))
            }
        }

        MenuSeparator {}

        MenuItem {
            text: qsTr("Remove from library")
            onTriggered: removeConfirm.open()
        }
        MenuItem {
            text: qsTr("Delete from disk")
            onTriggered: deleteConfirm.open()
        }
    }

    // Right-click context menu for a multi-selection: only the actions that make
    // sense for many photos at once, each operating on the whole selection.
    Menu {
        id: multiPhotoMenu

        MenuItem {
            text: qsTr("Play slideshow")
            onTriggered: win.startSlideshow(win.menuPhotoId, true, true)
        }

        MenuSeparator {}

        MenuItem {
            text: qsTr("Remove from library")
            onTriggered: removeConfirm.open()
        }
        MenuItem {
            text: qsTr("Delete from disk")
            onTriggered: deleteConfirm.open()
        }
    }

    // "Remove from library": drops the entry but leaves the file on disk.
    Dialog {
        id: removeConfirm
        title: qsTr("Remove from library")
        anchors.centerIn: parent
        modal: true
        width: 420
        standardButtons: Dialog.Yes | Dialog.No
        onAccepted: win.withPreservedGalleryScroll(function () {
            library.removeFromLibrary(win.selectedIds)
        })

        Label {
            width: parent.width
            wrapMode: Text.WordWrap
            text: win.selectedIds.length > 1
                  ? qsTr("Remove these %1 photos from the library?\n\nThe files stay on disk — only their library entries and tags are removed.")
                        .arg(win.selectedIds.length)
                  : qsTr("Remove “%1” from the library?\n\nThe file stays on disk — only its library entry and tags are removed.")
                        .arg(win.menuPhotoName)
        }
    }

    // "Delete from disk": moves the file to the Trash, then drops the entry.
    Dialog {
        id: deleteConfirm
        title: qsTr("Delete from disk")
        anchors.centerIn: parent
        modal: true
        width: 420
        standardButtons: Dialog.Yes | Dialog.No
        onAccepted: win.withPreservedGalleryScroll(function () {
            const failed = library.deleteFromDisk(win.selectedIds)
            if (failed === 1)
                win.toast(qsTr("Couldn't delete the file"))
            else if (failed > 1)
                win.toast(qsTr("Couldn't delete %1 files").arg(failed))
        })

        Label {
            width: parent.width
            wrapMode: Text.WordWrap
            text: win.selectedIds.length > 1
                  ? qsTr("Delete these %1 photos?\n\nThe files are moved to the Trash and removed from the library.")
                        .arg(win.selectedIds.length)
                  : qsTr("Delete “%1”?\n\nThe file is moved to the Trash and removed from the library.")
                        .arg(win.menuPhotoName)
        }
    }

    // Transient status toast (see win.toast). Floats near the bottom, fades out.
    Popup {
        id: toastPopup
        parent: Overlay.overlay
        x: Math.round(((parent ? parent.width : width) - width) / 2)
        y: (parent ? parent.height : height) - height - 48
        modal: false
        closePolicy: Popup.NoAutoClose
        padding: 12

        background: Rectangle {
            color: palette.window
            radius: 8
            border.color: palette.mid
        }

        Label {
            id: toastLabel
            color: palette.windowText
            font.pixelSize: 13
        }

        Timer {
            id: toastTimer
            interval: 2200
            onTriggered: toastPopup.close()
        }
    }
}
