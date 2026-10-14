// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

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

    readonly property int uncategorizedCount: {
        let n = 0
        const all = root.library.allTags
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
        const all = root.library.allTags
        for (let i = 0; i < all.length; ++i) {
            if (all[i].categoryId === categoryId)
                ++n
        }
        return n
    }

    // The tag list, filtered by the search box and the category chips (filter the
    // model, not visibility — a ListView leaves gaps for hidden delegates).
    readonly property var filteredTags: {
        const q = searchField.text.trim().toLowerCase()
        const cf = root.categoryFilter
        const all = root.library.allTags
        return all.filter(function (t) {
            if (q !== "" && t.name.toLowerCase().indexOf(q) === -1)
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
            const all = root.library.allTags
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
                onAccepted: renameDialog.apply()
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
                onAccepted: newCategoryDialog.apply()
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

        function openFor(tag, anchorItem) {
            targetTagId = tag.id
            currentCategoryId = tag.categoryId
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
            model: root.library.categories
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
                }

                // Category filter chips. Hidden until a category exists, since
                // "All" vs "Uncategorized" would then be the same list.
                Flow {
                    Layout.fillWidth: true
                    spacing: 4
                    visible: root.library.categories.length > 0

                    component FilterChip: Rectangle {
                        id: fchip
                        property int filterValue: -2
                        property string label: ""
                        property int colorSlot: -1

                        readonly property bool active: root.categoryFilter === fchip.filterValue
                        readonly property color tint: root.categoryColor(fchip.colorSlot)

                        implicitHeight: 24
                        implicitWidth: fchipLabel.implicitWidth + 16
                        radius: height / 2
                        color: fchip.active ? fchip.tint
                             : fchip.colorSlot >= 0 ? Qt.alpha(fchip.tint, 0.18)
                             : sys.alternateBase
                        border.width: 1
                        border.color: fchip.active || fchip.colorSlot >= 0 ? fchip.tint : sys.mid

                        Label {
                            id: fchipLabel
                            anchors.centerIn: parent
                            text: fchip.label
                            font.pixelSize: 12
                            color: !fchip.active ? palette.text
                                 : fchip.colorSlot >= 0 ? omarchyTheme.textOn(fchip.tint)
                                 : palette.highlightedText
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.categoryFilter = fchip.filterValue
                        }
                    }

                    FilterChip {
                        filterValue: -2
                        label: qsTr("All") + "  " + root.library.allTags.length
                    }
                    FilterChip {
                        filterValue: -1
                        label: qsTr("Uncategorized") + "  " + root.uncategorizedCount
                    }
                    Repeater {
                        model: root.library.categories
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
                                if (root.library.allTags.length === 0)
                                    return qsTr("No tags yet. Select a photo and add some.")
                                if (q !== "")
                                    return qsTr("No tags match “%1”.").arg(q)
                                if (root.categoryFilter === -1)
                                    return qsTr("Every tag has a category.")
                                return qsTr("No tags in this category yet. Use a tag's category menu to add some.")
                            }
                            color: palette.placeholderText
                        }
                    }
                }
            }

            // ---------------------------------------------------- Categories tab
            ColumnLayout {
                spacing: 10

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    TextField {
                        id: newCategoryField
                        Layout.fillWidth: true
                        placeholderText: qsTr("New category (e.g. Place, Event, Artist)…")
                        selectByMouse: true

                        readonly property string trimmed: text.trim()
                        readonly property bool conflict: trimmed.length > 0 && root.categoryNameTaken(trimmed, -1)
                        readonly property bool canCreate: trimmed.length > 0 && !conflict

                        function create() {
                            if (!canCreate)
                                return
                            const name = trimmed
                            root.library.createCategory(name, omarchyTheme.categoryPalette.length)
                            text = ""
                        }

                        onAccepted: create()
                        Keys.onEscapePressed: text = ""
                    }
                    Button {
                        text: qsTr("Add")
                        enabled: newCategoryField.canCreate
                        onClicked: newCategoryField.create()
                    }
                }

                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    visible: newCategoryField.conflict
                    text: qsTr("A category named “%1” already exists.").arg(newCategoryField.trimmed)
                    color: "#d64545"
                    font.pixelSize: 12
                }

                Frame {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumHeight: 340
                    padding: 0

                    ListView {
                        id: categoryList
                        anchors.fill: parent
                        clip: true
                        model: root.library.categories
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
                            text: qsTr("No categories yet. Create one above to color-code your tags, "
                                       + "then assign tags to it from the Tags tab.")
                            color: palette.placeholderText
                        }
                    }
                }
            }
        }
    }
}
