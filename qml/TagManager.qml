// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Omapic // TextMatch singleton (accent-/case-insensitive folding)

Dialog {
    id: root

    property var library
    property color accent: "#4a90d9"

    // Optional callback(fn) supplied by the parent that runs fn while keeping the
    // main picture gallery's scroll position — tag edits reset the photo model.
    property var galleryScrollGuard: null

    title: qsTr("Manage Tags")
    modal: true
    anchors.centerIn: parent
    width: Math.min(640, (parent ? parent.width : 640) - 80)
    standardButtons: Dialog.Close

    // Reopen with the full list: a leftover search or category filter from the
    // previous visit is easy to miss and makes tags look missing.
    onClosed: {
        searchField.text = ""
        root.categoryFilter = -2
    }

    SystemPalette { id: sys }

    // Tag awaiting delete confirmation; -1 when none is pending.
    property int pendingId: -1
    property string pendingName: ""
    property int pendingCount: 0

    // Category awaiting delete confirmation; -1 when none is pending.
    property int pendingCategoryId: -1
    property string pendingCategoryName: ""
    property int pendingCategoryTagCount: 0

    // Which tags the Tags tab shows: -2 every tag, -1 only uncategorized ones,
    // otherwise only the tags in that category.
    property int categoryFilter: -2

    // One cached read of library.allTags (an expensive query) per change. The many
    // consumers below — the filter chips' counts, the filtered list, the empty
    // state — read this instead of hitting the getter each time, which otherwise
    // re-ran the query ~once per category chip and froze the UI on a category edit.
    readonly property var allTags: root.library.allTags
    // Same single-read caching for the (also query-backed) category list, so the
    // two category list models and the visibility check below don't each re-run
    // the query every time categoriesChanged fires.
    readonly property var categories: root.library.categories
    // Tag implications ("if this tag, then that tag"), refreshed on any change.
    readonly property var implications: root.library.tagImplications
    // Tag aliases ("this name means that tag"), refreshed on any change.
    readonly property var aliases: root.library.tagAliases

    // Tag id for an exact (case-insensitive) name, or -1. Used by the Associations
    // tab's pickers, which only offer existing tag names.
    function tagIdByName(name) {
        const n = TextMatch.fold((name || "").trim())
        if (n === "")
            return -1
        const all = root.allTags
        // Folded compare so a typed unaccented name still resolves to its
        // accented tag ("cafe" -> the "café" tag's id).
        for (let i = 0; i < all.length; ++i) {
            if (TextMatch.fold(all[i].name) === n)
                return all[i].id
        }
        return -1
    }
    // Like tagIdByName, but a typed alias resolves to its canonical tag — so the
    // association pickers accept an alias anywhere a tag name is entered. (The
    // Aliases tab keeps using the raw tagIdByName to detect a name that is still a
    // real tag, which it then merges.)
    function resolveTagId(name) {
        const direct = root.tagIdByName(name)
        if (direct >= 0)
            return direct
        const n = TextMatch.fold((name || "").trim())
        if (n === "")
            return -1
        const al = root.aliases
        for (let i = 0; i < al.length; ++i) {
            if (TextMatch.fold(al[i].aliasName) === n)
                return al[i].tagId
        }
        return -1
    }
    // Palette color for a tag by id (for the association chips).
    function tagColorSlot(tagId) {
        const all = root.allTags
        for (let i = 0; i < all.length; ++i) {
            if (all[i].id === tagId)
                return all[i].colorSlot
        }
        return -1
    }

    readonly property int uncategorizedCount: {
        let n = 0
        const all = root.allTags
        for (let i = 0; i < all.length; ++i) {
            if (all[i].categoryId < 0)
                ++n
        }
        return n
    }

    // Active (shown) tags in a category — those in allTags, which already excludes
    // tags that only touch hidden/removed folders. Used for the Tags-tab category
    // pills so their counts match the "All" pill and the filtered tag list rather
    // than including hidden tags. (The Categories tab shows the total instead.)
    function activeCategoryCount(categoryId) {
        let n = 0
        const all = root.allTags
        for (let i = 0; i < all.length; ++i) {
            if (all[i].categoryId === categoryId)
                ++n
        }
        return n
    }

    // The tag list, filtered by the search box and the category chips (filter the
    // model, not visibility — a ListView leaves gaps for hidden delegates).
    readonly property var filteredTags: {
        const q = TextMatch.fold(searchField.text.trim())
        const cf = root.categoryFilter
        const all = root.allTags
        return all.filter(function (t) {
            // Accent-/case-folded so "cafe" lists the "café" tag.
            if (q !== "" && TextMatch.fold(t.name).indexOf(q) === -1)
                return false
            if (cf === -2)
                return true
            if (cf === -1)
                return t.categoryId < 0
            return t.categoryId === cf
        })
    }

    // Color for a category palette slot; the accent for "no category". Reads the
    // palette property so everything re-colors when the Omarchy theme changes.
    function categoryColor(slot) {
        const pal = omarchyTheme.categoryPalette
        if (slot === undefined || slot < 0 || pal.length === 0)
            return root.accent
        return pal[slot % pal.length]
    }

    // True when another category already uses this name (case-insensitive),
    // ignoring the category with id `exceptId`.
    function categoryNameTaken(name, exceptId) {
        const n = name.trim().toLowerCase()
        const cats = root.library.categories
        for (let i = 0; i < cats.length; ++i) {
            if (cats[i].id !== exceptId && cats[i].name.toLowerCase() === n)
                return true
        }
        return false
    }

    // Run a mutation that refreshes the tag list without the view jumping to the
    // top (the model is replaced wholesale, which otherwise resets the scroll).
    // The same mutation may reset the gallery's photo model, so route it through
    // the parent's guard too, keeping the picture gallery in place as well.
    function withPreservedScroll(fn) {
        const y = list.contentY
        if (root.galleryScrollGuard)
            root.galleryScrollGuard(fn)
        else
            fn()
        list.contentY = y
        Qt.callLater(function () { list.contentY = y })
    }

    // Drop a category filter whose category no longer exists.
    Connections {
        target: root.library
        function onCategoriesChanged() {
            if (root.categoryFilter < 0)
                return
            const cats = root.library.categories
            for (let i = 0; i < cats.length; ++i) {
                if (cats[i].id === root.categoryFilter)
                    return
            }
            root.categoryFilter = -2
        }
    }

    Dialog {
        id: confirmDialog
        title: qsTr("Delete tag?")
        modal: true
        anchors.centerIn: parent
        width: Math.min(460, root.width - 40)
        standardButtons: Dialog.Yes | Dialog.No

        onAccepted: {
            const id = root.pendingId
            root.withPreservedScroll(function () { root.library.deleteTag(id) })
            root.pendingId = -1
            root.pendingName = ""
            root.pendingCount = 0
        }
        onRejected: {
            root.pendingId = -1
            root.pendingName = ""
            root.pendingCount = 0
        }

        Label {
            width: parent.width
            wrapMode: Text.WordWrap
            text: root.pendingCount > 0
                  ? qsTr("Delete the tag “%1”? It will be removed from %2 photo(s). This cannot be undone.")
                        .arg(root.pendingName).arg(root.pendingCount)
                  : qsTr("Delete the unused tag “%1”?").arg(root.pendingName)
        }
    }

    Dialog {
        id: confirmCategoryDialog
        title: qsTr("Delete category?")
        modal: true
        anchors.centerIn: parent
        width: Math.min(460, root.width - 40)
        standardButtons: Dialog.Yes | Dialog.No

        onAccepted: {
            const id = root.pendingCategoryId
            root.withPreservedScroll(function () { root.library.deleteCategory(id) })
            root.pendingCategoryId = -1
            root.pendingCategoryName = ""
            root.pendingCategoryTagCount = 0
        }
        onRejected: {
            root.pendingCategoryId = -1
            root.pendingCategoryName = ""
            root.pendingCategoryTagCount = 0
        }

        Label {
            width: parent.width
            wrapMode: Text.WordWrap
            text: root.pendingCategoryTagCount > 0
                  ? qsTr("Delete the category “%1”? Its %2 tag(s) are kept, but become uncategorized.")
                        .arg(root.pendingCategoryName).arg(root.pendingCategoryTagCount)
                  : qsTr("Delete the empty category “%1”?").arg(root.pendingCategoryName)
        }
    }

    // Renames a tag or a category, depending on `kind`.
    Dialog {
        id: renameDialog
        title: kind === "tag" ? qsTr("Rename tag") : qsTr("Rename category")
        modal: true
        anchors.centerIn: parent
        width: Math.min(460, root.width - 40)
        closePolicy: Popup.CloseOnEscape

        property string kind: "tag" // "tag" or "category"
        property int targetId: -1
        property string originalName: ""

        function openFor(kind, id, name) {
            renameDialog.kind = kind
            targetId = id
            originalName = name
            field.text = name
            open()
            field.forceActiveFocus()
            field.selectAll()
        }

        // True when another tag already uses this name (case-insensitive). A name
        // that differs from the original only in casing is NOT a conflict.
        function tagNameTaken(name) {
            const n = name.trim().toLowerCase()
            const all = root.allTags
            for (let i = 0; i < all.length; ++i) {
                if (all[i].id !== renameDialog.targetId && all[i].name.toLowerCase() === n)
                    return true
            }
            return false
        }

        readonly property string trimmed: field.text.trim()
        readonly property bool conflict: trimmed.length > 0
                                         && (kind === "tag" ? tagNameTaken(trimmed)
                                                            : root.categoryNameTaken(trimmed, targetId))
        readonly property bool canApply: trimmed.length > 0 && !conflict
                                         && trimmed !== originalName

        function apply() {
            if (!canApply)
                return
            const id = targetId
            const name = trimmed
            const k = kind
            root.withPreservedScroll(function () {
                if (k === "tag")
                    root.library.renameTag(id, name)
                else
                    root.library.renameCategory(id, name)
            })
            close()
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 10

            TextField {
                id: field
                Layout.fillWidth: true
                selectByMouse: true
                // '|' is the sidecar hierarchy separator; not allowed in names.
                validator: RegularExpressionValidator { regularExpression: /[^|]*/ }
                onAccepted: renameDialog.apply()
                ClearButton { field: field }
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                visible: renameDialog.conflict
                text: renameDialog.kind === "tag"
                      ? qsTr("A tag named “%1” already exists.").arg(renameDialog.trimmed)
                      : qsTr("A category named “%1” already exists.").arg(renameDialog.trimmed)
                color: "#d64545"
                font.pixelSize: 12
            }

            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                Button {
                    text: qsTr("Cancel")
                    onClicked: renameDialog.close()
                }
                Button {
                    text: qsTr("Rename")
                    enabled: renameDialog.canApply
                    onClicked: renameDialog.apply()
                }
            }
        }
    }

    // Creates a category, optionally assigning a tag to it right away (reached
    // from a tag row's "New category…" so the common flow stays on the Tags tab).
    Dialog {
        id: newCategoryDialog
        title: qsTr("New category")
        modal: true
        anchors.centerIn: parent
        width: Math.min(460, root.width - 40)
        closePolicy: Popup.CloseOnEscape

        property int assignTagId: -1

        function openFor(tagId) {
            assignTagId = tagId
            newField.text = ""
            open()
            newField.forceActiveFocus()
        }

        readonly property string trimmed: newField.text.trim()
        readonly property bool conflict: trimmed.length > 0 && root.categoryNameTaken(trimmed, -1)
        readonly property bool canApply: trimmed.length > 0 && !conflict

        function apply() {
            if (!canApply)
                return
            const name = trimmed
            const tagId = assignTagId
            root.withPreservedScroll(function () {
                const id = root.library.createCategory(name, omarchyTheme.categoryPalette.length)
                if (id >= 0 && tagId >= 0)
                    root.library.setTagCategory(tagId, id)
            })
            close()
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 10

            TextField {
                id: newField
                Layout.fillWidth: true
                selectByMouse: true
                placeholderText: qsTr("Category name (e.g. Place, Event, Artist)")
                // '|' is the sidecar hierarchy separator; not allowed in names.
                validator: RegularExpressionValidator { regularExpression: /[^|]*/ }
                onAccepted: newCategoryDialog.apply()
                ClearButton { field: newField }
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                visible: newCategoryDialog.conflict
                text: qsTr("A category named “%1” already exists.").arg(newCategoryDialog.trimmed)
                color: "#d64545"
                font.pixelSize: 12
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("It gets the next free color automatically; change it later from the Categories tab.")
                color: palette.placeholderText
                font.pixelSize: 12
            }

            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                Button {
                    text: qsTr("Cancel")
                    onClicked: newCategoryDialog.close()
                }
                Button {
                    text: qsTr("Create")
                    enabled: newCategoryDialog.canApply
                    onClicked: newCategoryDialog.apply()
                }
            }
        }
    }

    // Swatch grid for a category's color. The current color is ringed; a small
    // dot marks colors some other category already uses (still selectable).
    Dialog {
        id: colorDialog
        title: qsTr("Category color")
        modal: true
        anchors.centerIn: parent
        width: Math.min(420, root.width - 40)
        standardButtons: Dialog.Cancel

        property int targetId: -1
        property string targetName: ""
        property int currentSlot: -1

        function openFor(id, name, slot) {
            targetId = id
            targetName = name
            currentSlot = slot
            open()
        }

        function usedByOther(slot) {
            const cats = root.library.categories
            const n = omarchyTheme.categoryPalette.length
            for (let i = 0; i < cats.length; ++i) {
                if (cats[i].id !== colorDialog.targetId && n > 0 && (cats[i].colorSlot % n) === slot)
                    return true
            }
            return false
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 12

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Choose a color for “%1”.").arg(colorDialog.targetName)
            }

            Flow {
                Layout.fillWidth: true
                spacing: 10

                Repeater {
                    model: omarchyTheme.categoryPalette

                    delegate: Rectangle {
                        id: swatch
                        required property int index
                        required property var modelData

                        readonly property bool current: index === colorDialog.currentSlot
                        readonly property bool taken: colorDialog.usedByOther(index)

                        width: 36
                        height: 36
                        radius: width / 2
                        color: swatch.modelData
                        border.width: swatch.current ? 3 : 0
                        border.color: palette.text

                        Rectangle {
                            anchors.centerIn: parent
                            width: 8
                            height: 8
                            radius: 4
                            visible: swatch.taken
                            color: omarchyTheme.textOn(swatch.modelData)
                            opacity: 0.8
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                const id = colorDialog.targetId
                                const slot = swatch.index
                                root.withPreservedScroll(function () {
                                    root.library.setCategoryColor(id, slot)
                                })
                                colorDialog.close()
                            }
                        }
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("A dot marks a color another category already uses.")
                color: palette.placeholderText
                font.pixelSize: 12
            }
        }
    }

    // One shared menu for the per-tag category pill: lists the categories (the
    // current one checked), None, and a shortcut to create a new category.
    Menu {
        id: categoryMenu

        property int targetTagId: -1
        property int currentCategoryId: -1
        // Snapshot of the categories taken when the menu opens. The list is NOT
        // bound live to library.categories: assigning a category changes the
        // per-category counts and re-emits categoriesChanged, which — if the
        // Instantiator tracked it live — would rebuild (and destroy) the very menu
        // item being clicked, so the pick wouldn't register. The menu is transient,
        // so a snapshot is fine; the next open refreshes it.
        property var menuCategories: []

        function openFor(tag, anchorItem) {
            targetTagId = tag.id
            currentCategoryId = tag.categoryId
            menuCategories = root.library.categories
            popup(anchorItem, 0, anchorItem.height)
        }

        function assign(categoryId) {
            const tagId = targetTagId
            root.withPreservedScroll(function () { root.library.setTagCategory(tagId, categoryId) })
        }

        // A menu row with a color swatch and our own check mark. Not `checkable`:
        // the style's check indicator would sit under the swatch and keep its own
        // box color when the row is highlighted.
        component CategoryMenuItem: MenuItem {
            id: cmi
            property color swatch: "transparent"
            property bool current: false

            readonly property color textColor: cmi.down || cmi.highlighted
                                               ? cmi.palette.highlightedText : cmi.palette.text

            contentItem: RowLayout {
                spacing: 8
                Rectangle {
                    width: 12
                    height: 12
                    radius: 6
                    color: cmi.swatch
                }
                Label {
                    Layout.fillWidth: true
                    text: cmi.text
                    elide: Text.ElideRight
                    color: cmi.textColor
                }
                Label {
                    text: "✓"
                    visible: cmi.current
                    color: cmi.textColor
                }
            }
        }

        Instantiator {
            model: categoryMenu.menuCategories
            delegate: CategoryMenuItem {
                required property var modelData
                text: modelData.name
                swatch: root.categoryColor(modelData.colorSlot)
                current: modelData.id === categoryMenu.currentCategoryId
                onTriggered: categoryMenu.assign(modelData.id)
            }
            onObjectAdded: (index, object) => categoryMenu.insertItem(index, object)
            onObjectRemoved: (index, object) => categoryMenu.removeItem(object)
        }

        CategoryMenuItem {
            text: qsTr("None")
            current: categoryMenu.currentCategoryId < 0
            onTriggered: categoryMenu.assign(-1)
        }
        MenuSeparator {}
        CategoryMenuItem {
            text: qsTr("New category…")
            onTriggered: newCategoryDialog.openFor(categoryMenu.targetTagId)
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 10

        TabBar {
            id: tabs
            Layout.fillWidth: true
            TabButton { text: qsTr("Tags") }
            TabButton { text: qsTr("Categories") }
            TabButton { text: qsTr("Associations") }
            TabButton { text: qsTr("Aliases") }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: tabs.currentIndex

            // ---------------------------------------------------------- Tags tab
            ColumnLayout {
                spacing: 10

                TextField {
                    id: searchField
                    Layout.fillWidth: true
                    placeholderText: qsTr("Search tags…")
                    Keys.onEscapePressed: text = ""
                    ClearButton { field: searchField }
                }

                // Category filter chips. Hidden until a category exists, since
                // "All" vs "Uncategorized" would then be the same list.
                Flow {
                    Layout.fillWidth: true
                    spacing: 4
                    visible: root.categories.length > 0

                    component FilterChip: Rectangle {
                        id: fchip
                        property int filterValue: -2
                        property string label: ""
                        property int colorSlot: -1

                        readonly property bool active: root.categoryFilter === fchip.filterValue
                        readonly property color tint: root.categoryColor(fchip.colorSlot)
                        // Active fill: the category color for a category chip, a
                        // neutral grey for the non-category chips (All / Uncategorized)
                        // so selection doesn't borrow the accent / a category color.
                        readonly property color selFill: fchip.colorSlot >= 0 ? fchip.tint : sys.mid

                        implicitHeight: 24
                        implicitWidth: fchipLabel.implicitWidth + 16
                        radius: height / 2
                        color: fchip.active ? fchip.selFill
                             : fchip.colorSlot >= 0 ? Qt.alpha(fchip.tint, 0.18)
                             : sys.alternateBase
                        border.width: 1
                        border.color: fchip.colorSlot >= 0 ? fchip.tint : sys.mid

                        Label {
                            id: fchipLabel
                            anchors.centerIn: parent
                            text: fchip.label
                            font.pixelSize: 12
                            color: fchip.active ? omarchyTheme.textOn(fchip.selFill) : palette.text
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.categoryFilter = fchip.filterValue
                        }
                    }

                    FilterChip {
                        filterValue: -2
                        label: qsTr("All") + "  " + root.allTags.length
                    }
                    FilterChip {
                        filterValue: -1
                        label: qsTr("Uncategorized") + "  " + root.uncategorizedCount
                    }
                    Repeater {
                        model: root.categories
                        delegate: FilterChip {
                            required property var modelData
                            filterValue: modelData.id
                            label: modelData.name + "  " + root.activeCategoryCount(modelData.id)
                            colorSlot: modelData.colorSlot
                        }
                    }
                }

                Frame {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumHeight: 340
                    padding: 0

                    ListView {
                        id: list
                        anchors.fill: parent
                        clip: true
                        model: root.filteredTags
                        boundsBehavior: Flickable.StopAtBounds
                        ScrollBar.vertical: ScrollBar {}

                        delegate: ItemDelegate {
                            id: row
                            width: ListView.view.width
                            hoverEnabled: true

                            required property var modelData

                            contentItem: RowLayout {
                                spacing: 12

                                Label {
                                    Layout.fillWidth: true
                                    text: row.modelData.name
                                    elide: Text.ElideRight
                                    font.pixelSize: 14
                                }
                                Label {
                                    text: row.modelData.count === 1
                                          ? qsTr("1 photo")
                                          : qsTr("%1 photos").arg(row.modelData.count)
                                    font.pixelSize: 12
                                    color: palette.placeholderText
                                }

                                // Category pill: shows the tag's category in its
                                // color; click to pick another, none, or a new one.
                                AbstractButton {
                                    id: catButton
                                    readonly property bool categorized: row.modelData.categoryId >= 0
                                    readonly property color tint: root.categoryColor(row.modelData.colorSlot)

                                    Layout.preferredWidth: Math.max(72, catLabel.implicitWidth + 22)
                                    Layout.maximumWidth: 150
                                    implicitHeight: 24
                                    hoverEnabled: true
                                    onClicked: categoryMenu.openFor(row.modelData, catButton)

                                    background: Rectangle {
                                        radius: height / 2
                                        color: catButton.categorized ? Qt.alpha(catButton.tint, 0.18)
                                             : catButton.hovered ? sys.alternateBase : "transparent"
                                        border.width: 1
                                        border.color: catButton.categorized ? catButton.tint : sys.mid
                                    }
                                    contentItem: Label {
                                        id: catLabel
                                        text: (catButton.categorized ? row.modelData.categoryName : qsTr("None")) + "  ▾"
                                        font.pixelSize: 12
                                        elide: Text.ElideRight
                                        horizontalAlignment: Text.AlignHCenter
                                        verticalAlignment: Text.AlignVCenter
                                        color: catButton.categorized ? palette.text : palette.placeholderText
                                    }
                                }

                                Button {
                                    text: qsTr("Rename")
                                    onClicked: renameDialog.openFor("tag", row.modelData.id, row.modelData.name)
                                }
                                Button {
                                    text: qsTr("Delete")
                                    onClicked: {
                                        root.pendingId = row.modelData.id
                                        root.pendingName = row.modelData.name
                                        root.pendingCount = row.modelData.count
                                        confirmDialog.open()
                                    }
                                }
                            }
                        }

                        Label {
                            anchors.centerIn: parent
                            width: parent.width - 32
                            visible: list.count === 0
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.WordWrap
                            text: {
                                const q = searchField.text.trim()
                                if (root.allTags.length === 0)
                                    return qsTr("No tags yet. Select a photo and add some.")
                                if (q !== "")
                                    return qsTr("No tags match “%1”.").arg(q)
                                if (root.categoryFilter === -1)
                                    return qsTr("Every tag has a category.")
                                return qsTr("No tags in this category yet. Use a tag's category menu to add some.")
                            }
                            color: palette.placeholderText
                        }

                        WheelScroller { flickable: list }
                    }
                }
            }

            // ---------------------------------------------------- Categories tab
            // View-and-manage only (rename, recolor, delete). Categories are
            // created from a tag's "New category…" menu on the Tags tab, so every
            // category is born attached to a tag and never ends up empty/invisible.
            ColumnLayout {
                spacing: 10

                Frame {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumHeight: 340
                    padding: 0

                    ListView {
                        id: categoryList
                        anchors.fill: parent
                        clip: true
                        model: root.categories
                        boundsBehavior: Flickable.StopAtBounds
                        ScrollBar.vertical: ScrollBar {}

                        delegate: ItemDelegate {
                            id: crow
                            width: ListView.view.width
                            hoverEnabled: true

                            required property var modelData
                            readonly property color tint: root.categoryColor(crow.modelData.colorSlot)

                            contentItem: RowLayout {
                                spacing: 12

                                // Swatch; click to recolor.
                                Rectangle {
                                    width: 18
                                    height: 18
                                    radius: 9
                                    color: crow.tint

                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: colorDialog.openFor(crow.modelData.id, crow.modelData.name,
                                                                       crow.modelData.colorSlot)
                                    }
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: crow.modelData.name
                                    elide: Text.ElideRight
                                    font.pixelSize: 14
                                }
                                Label {
                                    text: crow.modelData.tagCount === 1
                                          ? qsTr("1 tag")
                                          : qsTr("%1 tags").arg(crow.modelData.tagCount)
                                    font.pixelSize: 12
                                    color: palette.placeholderText
                                }
                                Button {
                                    text: qsTr("Color")
                                    onClicked: colorDialog.openFor(crow.modelData.id, crow.modelData.name,
                                                                   crow.modelData.colorSlot)
                                }
                                Button {
                                    text: qsTr("Rename")
                                    onClicked: renameDialog.openFor("category", crow.modelData.id, crow.modelData.name)
                                }
                                Button {
                                    text: qsTr("Delete")
                                    onClicked: {
                                        root.pendingCategoryId = crow.modelData.id
                                        root.pendingCategoryName = crow.modelData.name
                                        root.pendingCategoryTagCount = crow.modelData.tagCount
                                        confirmCategoryDialog.open()
                                    }
                                }
                            }
                        }

                        Label {
                            anchors.centerIn: parent
                            width: parent.width - 32
                            visible: categoryList.count === 0
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.WordWrap
                            text: qsTr("No categories yet. Create one from the Tags tab by opening a "
                                       + "tag's category menu and choosing “New category…”.")
                            color: palette.placeholderText
                        }

                        WheelScroller { flickable: categoryList }
                    }
                }
            }

            // -------------------------------------------------- Associations tab
            // "If a photo has <this> tag, also add <that> tag." Rules are applied
            // across the library when added (and to each tag as it's applied going
            // forward). They're materialized: removing a rule stops future
            // propagation but leaves already-added tags in place.
            ColumnLayout {
                spacing: 10

                // Rounded, category-colored chip for a rule's tags.
                component AssocChip: Rectangle {
                    id: chip
                    property string label: ""
                    property int colorSlot: -1
                    readonly property bool categorized: chip.colorSlot >= 0
                    readonly property color tint: root.categoryColor(chip.colorSlot)
                    implicitHeight: 24
                    implicitWidth: chipText.implicitWidth + 16
                    radius: height / 2
                    color: chip.categorized ? Qt.alpha(chip.tint, 0.18) : sys.alternateBase
                    border.width: 1
                    border.color: chip.categorized ? chip.tint : sys.mid
                    Label {
                        id: chipText
                        anchors.centerIn: parent
                        text: chip.label
                        font.pixelSize: 12
                        color: palette.text
                    }
                }

                // Validation state for the add row.
                QtObject {
                    id: assoc
                    // Folded antecedent text, for live-filtering the list below
                    // (accent-/case-insensitive, matching the pickers).
                    readonly property string query: TextMatch.fold(antField.text.trim())
                    // resolveTagId so a typed alias resolves to its canonical tag.
                    readonly property int antId: root.resolveTagId(antField.text)
                    readonly property int conId: root.resolveTagId(conField.text)
                    // The typed consequent, and whether it names a tag that doesn't
                    // exist yet — in which case Add creates it (see submit()).
                    readonly property string conName: conField.text.trim()
                    readonly property bool conIsNew: assoc.conName !== "" && assoc.conId < 0
                    readonly property bool isSelf: assoc.antId >= 0 && assoc.antId === assoc.conId
                    readonly property bool wouldCycle: assoc.antId >= 0 && assoc.conId >= 0 && !assoc.isSelf
                                                       && root.library.implicationWouldCycle(assoc.antId, assoc.conId)
                    readonly property bool exists: {
                        const im = root.implications
                        for (let i = 0; i < im.length; ++i) {
                            if (im[i].antecedentId === assoc.antId && im[i].consequentId === assoc.conId)
                                return true
                        }
                        return false
                    }
                    // A brand-new consequent can't be a self-loop, a cycle, or a
                    // duplicate, so those guards fall away when conIsNew.
                    readonly property bool canAdd: assoc.antId >= 0 && assoc.conName !== ""
                                                   && !assoc.isSelf && !assoc.wouldCycle && !assoc.exists
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    function submit() {
                        if (!assoc.canAdd)
                            return
                        // Create the consequent on the fly when it's a new name;
                        // the association's migration then applies it to photos.
                        let conId = assoc.conId
                        if (conId < 0) {
                            conId = root.library.ensureTag(assoc.conName)
                            if (conId < 0)
                                return
                        }
                        root.library.addTagImplication(assoc.antId, conId)
                        antField.clear()
                        conField.clear()
                        antField.focusInput()
                    }

                    Label { text: qsTr("If a photo has") }
                    TagAutoField {
                        id: antField
                        Layout.fillWidth: true
                        placeholderText: qsTr("this tag…")
                        tags: root.allTags
                        aliases: root.aliases
                        categoryColorFn: root.categoryColor
                        onAccepted: conField.focusInput()
                    }
                    Label { text: qsTr("also add") }
                    TagAutoField {
                        id: conField
                        Layout.fillWidth: true
                        placeholderText: qsTr("that tag…")
                        tags: root.allTags
                        aliases: root.aliases
                        categoryColorFn: root.categoryColor
                        onAccepted: parent.submit()
                    }
                    Button {
                        text: qsTr("Add")
                        enabled: assoc.canAdd
                        onClicked: parent.submit()
                    }
                }

                // Inline hint / validation message.
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.pixelSize: 12
                    color: palette.placeholderText
                    text: assoc.isSelf ? qsTr("A tag can't imply itself.")
                        : assoc.wouldCycle ? qsTr("That would create a loop of associations.")
                        : assoc.exists ? qsTr("That association already exists.")
                        : assoc.conIsNew ? qsTr("“%1” will be created as a new tag.").arg(assoc.conName)
                        : qsTr("Removing an association later won't remove tags it has already applied — "
                               + "those must be cleared by hand.")
                }

                // As the user types "this tag", the list narrows live to rules
                // whose antecedent matches, so they can see what already exists.
                Label {
                    Layout.fillWidth: true
                    font.pixelSize: 12
                    font.bold: true
                    text: assoc.query === "" ? qsTr("All associations")
                                             : qsTr("Associations matching “%1”").arg(antField.text.trim())
                }

                Frame {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumHeight: 280
                    padding: 0

                    ListView {
                        id: assocList
                        anchors.fill: parent
                        clip: true
                        // Live-filter to rules whose antecedent matches the text.
                        model: assoc.query === ""
                               ? root.implications
                               : root.implications.filter(im => TextMatch.fold(im.antecedentName).indexOf(assoc.query) !== -1)
                        boundsBehavior: Flickable.StopAtBounds
                        ScrollBar.vertical: ScrollBar {}

                        delegate: ItemDelegate {
                            id: arow
                            width: ListView.view.width
                            required property var modelData

                            contentItem: RowLayout {
                                spacing: 10
                                AssocChip {
                                    label: arow.modelData.antecedentName
                                    colorSlot: arow.modelData.antecedentColorSlot
                                }
                                Label { text: "→"; font.pixelSize: 16; color: palette.placeholderText }
                                AssocChip {
                                    label: arow.modelData.consequentName
                                    colorSlot: arow.modelData.consequentColorSlot
                                }
                                Item { Layout.fillWidth: true }
                                Button {
                                    text: qsTr("Remove")
                                    onClicked: root.library.removeTagImplication(arow.modelData.antecedentId,
                                                                                 arow.modelData.consequentId)
                                }
                            }
                        }

                        Label {
                            anchors.centerIn: parent
                            width: parent.width - 32
                            visible: assocList.count === 0
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.WordWrap
                            text: assoc.query === ""
                                  ? qsTr("No associations yet. Add one above to auto-apply a tag "
                                         + "whenever another is present.")
                                  : qsTr("No associations match “%1”.").arg(antField.text.trim())
                            color: palette.placeholderText
                        }

                        WheelScroller { flickable: assocList }
                    }
                }
            }

            // -------------------------------------------------------- Aliases tab
            // "This name means that tag." Typing or importing an alias applies the
            // canonical tag instead of making a new one; aliases never appear as
            // tags themselves. Aliasing a name that is already a tag MERGES that tag
            // into the canonical one (confirmed first — a merge can't be undone).
            ColumnLayout {
                id: aliasesTab
                spacing: 10

                // A merge awaiting confirmation; false while idle. Editing a field
                // clears it, so the confirmation always matches what's shown.
                property bool confirmPending: false

                // Rounded chip for a row's tags: category-colored for the canonical
                // tag, neutral (colorSlot -1) for the alias name, which isn't a tag.
                component CanonChip: Rectangle {
                    id: cchip
                    property string label: ""
                    property int colorSlot: -1
                    readonly property bool categorized: cchip.colorSlot >= 0
                    readonly property color tint: root.categoryColor(cchip.colorSlot)
                    implicitHeight: 24
                    implicitWidth: cchipText.implicitWidth + 16
                    radius: height / 2
                    color: cchip.categorized ? Qt.alpha(cchip.tint, 0.18) : sys.alternateBase
                    border.width: 1
                    border.color: cchip.categorized ? cchip.tint : sys.mid
                    Label {
                        id: cchipText
                        anchors.centerIn: parent
                        text: cchip.label
                        font.pixelSize: 12
                        color: palette.text
                    }
                }

                // Validation state for the add row.
                QtObject {
                    id: alias
                    readonly property string aliasName: aliasField.text.trim()
                    readonly property string query: TextMatch.fold(alias.aliasName)
                    // The alias points at an existing tag, chosen in the picker.
                    readonly property int canonId: root.tagIdByName(canonField.text)
                    readonly property string canonName: canonField.text.trim()
                    // Folded canonical text — the list below narrows to this tag's
                    // aliases as it's typed, so existing aliases surface up front.
                    readonly property string canonQuery: TextMatch.fold(alias.canonName)
                    // When the alias name is already a tag, Add merges that tag into
                    // the canonical one; mergeCount is that tag's photo count.
                    readonly property int matchTagId: root.tagIdByName(alias.aliasName)
                    readonly property bool isMerge: alias.matchTagId >= 0 && alias.matchTagId !== alias.canonId
                    readonly property int mergeCount: {
                        if (alias.matchTagId < 0)
                            return 0
                        const all = root.allTags
                        for (let i = 0; i < all.length; ++i) {
                            if (all[i].id === alias.matchTagId)
                                return all[i].count
                        }
                        return 0
                    }
                    // Aliasing a tag to its own name is meaningless; alias names are
                    // unique among aliases.
                    readonly property bool isSelf: alias.aliasName !== "" && alias.canonId >= 0
                                                   && TextMatch.fold(alias.aliasName) === TextMatch.fold(alias.canonName)
                    readonly property bool alreadyAlias: {
                        const al = root.aliases
                        for (let i = 0; i < al.length; ++i) {
                            if (TextMatch.fold(al[i].aliasName) === alias.query && alias.query !== "")
                                return true
                        }
                        return false
                    }
                    readonly property bool canAdd: alias.aliasName !== "" && alias.canonId >= 0
                                                   && !alias.isSelf && !alias.alreadyAlias
                }

                RowLayout {
                    id: aliasRow
                    Layout.fillWidth: true
                    spacing: 8

                    function submit() {
                        if (!alias.canAdd)
                            return
                        // A merge needs confirming first; a second Enter (or the
                        // Merge button) commits it.
                        if (alias.isMerge && !aliasesTab.confirmPending) {
                            aliasesTab.confirmPending = true
                            return
                        }
                        if (root.library.createTagAlias(alias.aliasName, alias.canonId)) {
                            aliasesTab.confirmPending = false
                            aliasField.text = ""
                            canonField.clear()
                            canonField.focusInput() // back to the tag box to start the next alias
                        }
                    }

                    TagAutoField {
                        id: canonField
                        Layout.fillWidth: true
                        placeholderText: qsTr("this tag…")
                        tags: root.allTags
                        categoryColorFn: root.categoryColor
                        onTextChanged: aliasesTab.confirmPending = false
                        onAccepted: aliasField.forceActiveFocus()
                    }
                    Label { text: qsTr("=") }
                    TextField {
                        id: aliasField
                        Layout.fillWidth: true
                        placeholderText: qsTr("alias…")
                        // '|' is the sidecar hierarchy separator, barred from names.
                        validator: RegularExpressionValidator { regularExpression: /[^|]*/ }
                        onTextEdited: aliasesTab.confirmPending = false
                        onAccepted: aliasRow.submit()
                    }
                    Button {
                        text: qsTr("Add")
                        enabled: alias.canAdd && !aliasesTab.confirmPending
                        onClicked: aliasRow.submit()
                    }
                }

                // Inline merge confirmation (Enter confirms, Esc cancels). It takes
                // focus when shown so the keys reach it rather than the dialog.
                FocusScope {
                    id: confirmBox
                    Layout.fillWidth: true
                    visible: aliasesTab.confirmPending
                    implicitHeight: visible ? confirmRow.implicitHeight : 0
                    onVisibleChanged: if (visible) forceActiveFocus()
                    Keys.onReturnPressed: aliasRow.submit()
                    Keys.onEnterPressed: aliasRow.submit()
                    Keys.onEscapePressed: (event) => {
                        aliasesTab.confirmPending = false
                        event.accepted = true
                    }

                    RowLayout {
                        id: confirmRow
                        width: parent.width
                        spacing: 8
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            font.pixelSize: 12
                            text: qsTr("Merge %n photo(s) from “%1” into “%2”? This can’t be undone.", "",
                                       alias.mergeCount).arg(alias.aliasName).arg(alias.canonName)
                        }
                        Button {
                            text: qsTr("Merge")
                            highlighted: true
                            onClicked: aliasRow.submit()
                        }
                        Button {
                            text: qsTr("Cancel")
                            onClicked: aliasesTab.confirmPending = false
                        }
                    }
                }

                // Hint / validation message (hidden while confirming a merge).
                Label {
                    Layout.fillWidth: true
                    visible: !aliasesTab.confirmPending
                    wrapMode: Text.WordWrap
                    font.pixelSize: 12
                    color: palette.placeholderText
                    text: alias.isSelf ? qsTr("A tag can’t be an alias of itself.")
                        : alias.alreadyAlias ? qsTr("“%1” is already an alias.").arg(alias.aliasName)
                        : (alias.aliasName !== "" && alias.canonId < 0 && alias.canonName !== "")
                            ? qsTr("Pick an existing tag for the alias to point to.")
                        : alias.isMerge && alias.canAdd
                            ? qsTr("“%1” is already a tag — adding this merges it into “%2”.")
                                  .arg(alias.aliasName).arg(alias.canonName)
                        : qsTr("An alias lets a second name resolve to a tag, e.g. “forests” → “forest”.")
                }

                Label {
                    Layout.fillWidth: true
                    font.pixelSize: 12
                    font.bold: true
                    text: alias.canonQuery === "" ? qsTr("All aliases")
                                                  : qsTr("Aliases for “%1”").arg(alias.canonName)
                }

                Frame {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumHeight: 280
                    padding: 0

                    ListView {
                        id: aliasList
                        anchors.fill: parent
                        clip: true
                        // Live-filter to the aliases of the canonical tag being typed.
                        model: alias.canonQuery === ""
                               ? root.aliases
                               : root.aliases.filter(a => TextMatch.fold(a.tagName).indexOf(alias.canonQuery) !== -1)
                        boundsBehavior: Flickable.StopAtBounds
                        ScrollBar.vertical: ScrollBar {}

                        // Ordered by canonical tag then alias, so a tag's aliases sit
                        // together — shown as "alias → canonical", like the rules list.
                        delegate: ItemDelegate {
                            id: alrow
                            width: ListView.view.width
                            required property var modelData

                            contentItem: RowLayout {
                                spacing: 10
                                CanonChip { label: alrow.modelData.aliasName; colorSlot: -1 }
                                Label { text: "→"; font.pixelSize: 16; color: palette.placeholderText }
                                CanonChip {
                                    label: alrow.modelData.tagName
                                    colorSlot: alrow.modelData.colorSlot
                                }
                                Item { Layout.fillWidth: true }
                                Button {
                                    text: qsTr("Remove")
                                    onClicked: root.library.removeTagAlias(alrow.modelData.aliasName)
                                }
                            }
                        }

                        Label {
                            anchors.centerIn: parent
                            width: parent.width - 32
                            visible: aliasList.count === 0
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.WordWrap
                            text: alias.canonQuery === ""
                                  ? qsTr("No aliases yet. Add one above so a second name resolves to a tag.")
                                  : qsTr("No aliases for “%1” yet.").arg(alias.canonName)
                            color: palette.placeholderText
                        }

                        WheelScroller { flickable: aliasList }
                    }
                }
            }
        }
    }
}
