// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Omapic // TextMatch singleton (accent-/case-insensitive folding)

Rectangle {
    id: root

    property var library
    property var selectedIds: []
    property color accent: "#4a90d9"
    // Optional wrapper that preserves the gallery scroll across a tag edit (a
    // large edit reloads the photo model, which would otherwise jump the grid).
    property var tagScrollGuard: null

    readonly property int count: selectedIds.length

    // single-mode photo info; `chips` unifies the tag list for both modes.
    property var info: ({})
    property var chips: []
    property var appliedLower: []
    // Tag aliases, cached per change, so the add-a-tag autocomplete can offer
    // "alias → tag" hint rows (and the typed alias resolves to its canonical tag).
    readonly property var aliasList: root.library ? root.library.tagAliases : []
    // Tag implications, cached per change, for the highlighted-entry info panel
    // ("applying this also applies …").
    readonly property var implicationList: root.library ? root.library.tagImplications : []

    // Tab focus cycle (routed by Main): forward from / back to the add-a-tag field.
    signal tabForward()
    signal tabBackward()
    // Focus the add-a-tag field. Returns false when no photo is selected (the
    // field is hidden), so the cycle can skip this stop.
    function focusTagInput() {
        if (root.count < 1)
            return false
        tagInput.forceActiveFocus()
        return true
    }

    SystemPalette { id: sys }

    // Color for a tag's category palette slot; the accent for uncategorized tags.
    // Reads the palette property so chips re-color when the theme changes.
    function categoryColor(slot) {
        const pal = omarchyTheme.categoryPalette
        if (slot === undefined || slot < 0 || pal.length === 0)
            return root.accent
        return pal[slot % pal.length]
    }

    // Remembers whether the collapsible file/EXIF detail is expanded. Collapsed
    // by default so the tag list isn't crowded.
    Settings {
        id: ui
        category: "details"
        property bool infoExpanded: false
    }

    color: sys.window

    // Full refresh: the file metadata (dimensions/EXIF/camera — read from disk)
    // plus the tag chips. Only needed when the selection changes; a tag edit
    // leaves the file untouched, so it uses refreshChips() to skip the disk read.
    function refresh() {
        info = (selectedIds.length === 1) ? library.photoInfo(selectedIds[0]) : ({})
        refreshChips()
    }

    // Rebuild just the tag chips / applied set from the DB (no file I/O). Safe to
    // run on every tag or category change, where the metadata is unchanged.
    function refreshChips() {
        const n = selectedIds.length
        if (n === 0) {
            chips = []
            appliedLower = []
            return
        }
        const ct = library.commonTags(selectedIds)
        if (n === 1) {
            chips = ct.map(function (t) {
                return { id: t.id, name: t.name, label: t.name, partial: false,
                         colorSlot: t.colorSlot, categoryName: t.categoryName }
            })
            appliedLower = ct.map(function (t) { return t.name.toLowerCase() })
        } else {
            chips = ct.map(function (t) {
                return { id: t.id, name: t.name,
                         label: t.name + "  (" + t.count + "/" + n + ")",
                         partial: t.count < n, // on some but not all selected
                         colorSlot: t.colorSlot, categoryName: t.categoryName }
            })
            // Only hide tags already on every selected photo.
            appliedLower = ct.filter(function (t) { return t.count === n })
                             .map(function (t) { return t.name.toLowerCase() })
        }
    }

    // Run a tag mutation through the scroll guard when one is set, so the gallery
    // stays put even if the edit reloads the photo model on a worker thread.
    function runGuarded(fn) {
        if (root.tagScrollGuard)
            root.tagScrollGuard(fn)
        else
            fn()
    }

    function commitTag(name) {
        if (!name || name.trim().length === 0)
            return
        runGuarded(function () {
            if (selectedIds.length === 1)
                library.addTag(selectedIds[0], name)
            else if (selectedIds.length > 1)
                library.addTagToPhotos(selectedIds, name)
        })
    }

    function removeTag(tagId) {
        runGuarded(function () {
            if (selectedIds.length === 1)
                library.removeTag(selectedIds[0], tagId)
            else if (selectedIds.length > 1)
                library.removeTagFromPhotos(selectedIds, tagId)
        })
    }

    onSelectedIdsChanged: refresh()

    Connections {
        target: root.library
        function onPhotoChanged(id) {
            if (root.selectedIds.indexOf(id) !== -1)
                root.refreshChips() // tags changed, not the file — skip the disk read
        }
        // A tag deleted from the manager can affect any selected photo's chips.
        function onTagsChanged() { root.refreshChips() }
    }

    // Empty state.
    Label {
        anchors.centerIn: parent
        width: parent.width - 24
        visible: root.count === 0
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        text: qsTr("Select a photo to see its details.")
        color: palette.placeholderText
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 10
        visible: root.count >= 1

        Label {
            text: root.count === 1 ? qsTr("Details")
                                   : qsTr("%1 photos selected").arg(root.count)
            font.bold: true
            font.pixelSize: 15
        }


        // File name (single selection). Shrinks to fit rather than clipping when
        // a long name would overflow two lines.
        FontMetrics { id: nameMetrics; font.bold: true; font.pixelSize: 15 }
        Label {
            visible: root.count === 1
            Layout.fillWidth: true
            Layout.preferredHeight: Math.ceil(nameMetrics.lineSpacing * 2)
            text: root.info.fileName || ""
            font.bold: true
            font.pixelSize: 15
            minimumPixelSize: 9
            fontSizeMode: Text.Fit
            wrapMode: Text.WrapAnywhere
            maximumLineCount: 2
            elide: Text.ElideMiddle
            verticalAlignment: Text.AlignVCenter
        }

        // Collapsible detail (path + dimensions/EXIF), folded away by default so
        // it never crowds the tags. A quiet caret-label, matching the other
        // section labels rather than a heavy header bar.
        Label {
            visible: root.count === 1
            Layout.fillWidth: true
            text: (ui.infoExpanded ? "▾  " : "▸  ") + qsTr("Info")
            font.pixelSize: 11
            color: palette.placeholderText

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: ui.infoExpanded = !ui.infoExpanded
            }
        }

        ColumnLayout {
            visible: root.count === 1 && ui.infoExpanded
            Layout.fillWidth: true
            Layout.leftMargin: 8
            spacing: 4

            Label {
                Layout.fillWidth: true
                text: root.info.path || ""
                font.pixelSize: 11
                color: palette.placeholderText
                wrapMode: Text.WrapAnywhere
                maximumLineCount: 3
                elide: Text.ElideMiddle
            }

            // Dimensions and EXIF. Each row hides itself when its value is empty
            // (missing tag, a video, an unreadable file); invisible layout items
            // take no cell, so label/value pairs stay aligned as rows drop out.
            GridLayout {
                Layout.fillWidth: true
                Layout.topMargin: 2
                columns: 2
                columnSpacing: 12
                rowSpacing: 4

                component FieldLabel: Label {
                    font.pixelSize: 11
                    color: palette.placeholderText
                }

                FieldLabel { visible: (root.info.dimensions || "") !== ""; text: qsTr("Dimensions") }
                Label {
                    visible: (root.info.dimensions || "") !== ""
                    Layout.fillWidth: true
                    text: root.info.dimensions || ""
                    font.pixelSize: 12
                }

                FieldLabel { visible: (root.info.size || "") !== ""; text: qsTr("Size") }
                Label {
                    visible: (root.info.size || "") !== ""
                    Layout.fillWidth: true
                    text: root.info.size || ""
                    font.pixelSize: 12
                }

                FieldLabel { visible: (root.info.taken || "") !== ""; text: qsTr("Taken") }
                Label {
                    visible: (root.info.taken || "") !== ""
                    Layout.fillWidth: true
                    text: root.info.taken || ""
                    font.pixelSize: 12
                }

                FieldLabel { visible: (root.info.camera || "") !== ""; text: qsTr("Camera") }
                Label {
                    visible: (root.info.camera || "") !== ""
                    Layout.fillWidth: true
                    text: root.info.camera || ""
                    font.pixelSize: 12
                    wrapMode: Text.WrapAnywhere
                }

                FieldLabel { visible: (root.info.duration || "") !== ""; text: qsTr("Duration") }
                Label {
                    visible: (root.info.duration || "") !== ""
                    Layout.fillWidth: true
                    text: root.info.duration || ""
                    font.pixelSize: 12
                }

                FieldLabel { visible: (root.info.modified || "") !== ""; text: qsTr("Modified") }
                Label {
                    visible: (root.info.modified || "") !== ""
                    Layout.fillWidth: true
                    text: root.info.modified || ""
                    font.pixelSize: 12
                }
            }
        }

        MenuSeparator { Layout.fillWidth: true }

        Label {
            text: root.count === 1 ? qsTr("Tags") : qsTr("Tags across selection")
            font.pixelSize: 11
            color: palette.placeholderText
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ScrollView {
            id: tagScroll
            anchors.fill: parent
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            Flow {
            width: tagScroll.availableWidth
            spacing: 4

            Repeater {
                model: root.chips
                delegate: Rectangle {
                    id: chip
                    required property var modelData
                    readonly property bool categorized: modelData.colorSlot >= 0
                    readonly property color tint: root.categoryColor(modelData.colorSlot)
                    radius: height / 2
                    height: 24
                    width: chipRow.implicitWidth + 12
                    // Partial tags (on only some of the selection) read as
                    // "unfilled" to hint they can be completed with a click.
                    // Categorized tags carry their category's color on the border
                    // and as a light tint; uncategorized ones keep a neutral grey
                    // border, matching the filter sidebar and the tag-info popover.
                    color: modelData.partial ? "transparent"
                         : chip.categorized ? Qt.alpha(chip.tint, 0.18)
                         : sys.alternateBase
                    border.color: chip.categorized ? chip.tint : sys.mid

                    // Click a partial chip to apply the tag to every selected photo.
                    // Sits below the row so the ✕ button still gets its own clicks.
                    MouseArea {
                        anchors.fill: parent
                        enabled: modelData.partial === true
                        cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                        onClicked: root.commitTag(modelData.name)
                    }

                    // One tooltip for both hints: the apply-to-all nudge on a
                    // partial chip, else the category name (color alone shouldn't
                    // carry the meaning).
                    HoverHandler { id: chipHover }
                    ToolTip.visible: chipHover.hovered && (modelData.partial === true || chip.categorized)
                    ToolTip.delay: modelData.partial === true ? 0 : 500
                    ToolTip.text: modelData.partial === true
                                  ? qsTr("Apply to all %1 selected").arg(root.count)
                                  : modelData.categoryName

                    RowLayout {
                        id: chipRow
                        anchors.centerIn: parent
                        spacing: 2
                        Label {
                            text: modelData.label
                            font.pixelSize: 12
                        }
                        ToolButton {
                            text: "✕"
                            font.pixelSize: 11
                            implicitWidth: 18
                            implicitHeight: 18
                            padding: 0
                            onClicked: root.removeTag(modelData.id)
                        }
                    }
                }
            }

            Label {
                visible: root.chips.length === 0
                text: qsTr("No tags yet.")
                color: palette.placeholderText
            }
            }
            }

            WheelScroller { flickable: tagScroll.contentItem }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            TextField {
                id: tagInput
                Layout.fillWidth: true
                placeholderText: root.count > 1 ? qsTr("Add a tag to all…") : qsTr("Add a tag…")

                property var suggestions: []
                property int highlighted: -1

                function updateSuggestions() {
                    const q = TextMatch.fold(text.trim())
                    highlighted = -1
                    if (q.length === 0) {
                        suggestions = []
                        suggestionPopup.close()
                        return
                    }
                    const applied = root.appliedLower
                    // {name, colorSlot, categoryName} so rows can show the color.
                    const all = root.library.tagSuggestions()
                    const matches = []
                    const seen = ({}) // folded canonical name -> index in `matches`
                    var exact = -1    // index of an exact (name or alias) match, to pre-highlight

                    for (var i = 0; i < all.length; ++i) {
                        // Exclude tags already applied (exact lower-cased name), but
                        // match the typed query accent-/case-folded so "cafe" finds
                        // "café".
                        if (applied.indexOf(all[i].name.toLowerCase()) !== -1)
                            continue
                        const ln = TextMatch.fold(all[i].name)
                        if (ln.indexOf(q) === -1)
                            continue
                        if (!(ln in seen) && matches.length < 8) {
                            seen[ln] = matches.length
                            matches.push(all[i])
                        }
                        if (ln === q && (ln in seen))
                            exact = seen[ln]
                    }
                    // A typed alias resolves to its canonical tag: show the canonical
                    // tag itself (same name/color/category), never a second row for a
                    // tag already listed, and pre-highlight it on an exact alias.
                    const aliases = root.aliasList
                    for (var j = 0; j < aliases.length; ++j) {
                        const cn = aliases[j].tagName || ""
                        if (applied.indexOf(cn.toLowerCase()) !== -1)
                            continue // canonical already on the photo
                        const af = TextMatch.fold(aliases[j].aliasName)
                        if (af.indexOf(q) === -1)
                            continue
                        const cf = TextMatch.fold(cn)
                        if (!(cf in seen) && matches.length < 8) {
                            seen[cf] = matches.length
                            matches.push({ name: cn, colorSlot: aliases[j].colorSlot,
                                           categoryName: aliases[j].categoryName || "" })
                        }
                        if (af === q && (cf in seen))
                            exact = seen[cf]
                    }
                    // Surface the exact match at the top, pre-highlighted — as though
                    // the user had arrow-keyed to it — so Enter accepts the tag.
                    if (exact >= 0) {
                        matches.unshift(matches.splice(exact, 1)[0])
                        highlighted = 0
                    }
                    suggestions = matches
                    if (matches.length > 0)
                        suggestionPopup.open()
                    else
                        suggestionPopup.close()
                }

                function commit(name) {
                    root.commitTag(name)
                    text = ""
                    suggestions = []
                    suggestionPopup.close()
                }

                onTextEdited: updateSuggestions()
                onAccepted: {
                    if (suggestionPopup.visible && highlighted >= 0 && highlighted < suggestions.length)
                        commit(suggestions[highlighted].name)
                    else
                        commit(text)
                }

                ClearButton { field: tagInput }
                Keys.onDownPressed: if (suggestions.length > 0)
                                        highlighted = Math.min(highlighted + 1, suggestions.length - 1)
                Keys.onUpPressed: highlighted = Math.max(highlighted - 1, -1)
                Keys.onEscapePressed: suggestionPopup.close()
                Keys.onTabPressed: (event) => { suggestionPopup.close(); root.tabForward(); event.accepted = true }
                Keys.onBacktabPressed: (event) => { suggestionPopup.close(); root.tabBackward(); event.accepted = true }

                Popup {
                    id: suggestionPopup
                    y: tagInput.height
                    x: 0
                    width: tagInput.width
                    padding: 1
                    closePolicy: Popup.CloseOnPressOutsideParent | Popup.CloseOnEscape

                    contentItem: ListView {
                        id: suggestionList
                        implicitHeight: Math.min(tagInput.suggestions.length, 8) * 32
                        model: tagInput.suggestions
                        clip: true
                        currentIndex: tagInput.highlighted

                        delegate: ItemDelegate {
                            id: suggestion
                            required property int index
                            required property var modelData
                            // Whether this row's name is cut off, so the info panel
                            // knows to surface the full name.
                            readonly property bool nameTruncated: nameLabel.truncated
                            width: ListView.view.width
                            height: 32
                            highlighted: index === tagInput.highlighted
                            hoverEnabled: true
                            // Hovering a row highlights it too, so the mouse drives
                            // the selection and its info panel, not just the keyboard.
                            onHoveredChanged: if (hovered) tagInput.highlighted = index
                            onClicked: tagInput.commit(modelData.name)

                            contentItem: RowLayout {
                                spacing: 8
                                // Category swatch; kept (transparent) for
                                // uncategorized tags so names stay aligned.
                                Rectangle {
                                    width: 10
                                    height: 10
                                    radius: 5
                                    color: suggestion.modelData.colorSlot >= 0
                                           ? root.categoryColor(suggestion.modelData.colorSlot)
                                           : "transparent"
                                }
                                Label {
                                    id: nameLabel
                                    Layout.fillWidth: true
                                    text: suggestion.modelData.name
                                    elide: Text.ElideRight
                                    // Custom contentItem loses the delegate's
                                    // automatic highlight recoloring, so track it
                                    // explicitly — otherwise the name is unreadable
                                    // on the highlight background in many themes.
                                    color: suggestion.highlighted ? suggestion.palette.highlightedText
                                                                   : suggestion.palette.text
                                }
                                Label {
                                    visible: suggestion.modelData.categoryName !== ""
                                    text: suggestion.modelData.categoryName
                                    font.pixelSize: 11
                                    color: suggestion.highlighted ? suggestion.palette.highlightedText
                                                                  : suggestion.palette.placeholderText
                                }
                            }
                        }
                    }
                }

                // Detail for the highlighted suggestion — the full name, aliases,
                // and the tags it will also apply — shown to the LEFT of the
                // dropdown (the detail panel hugs the window's right edge). Appears
                // only when there's something the row can't show.
                TagInfoPopover {
                    id: tagInfoPopover
                    parent: tagInput
                    preferredY: suggestionPopup.y
                    // To the right of the dropdown (the detail panel is docked left,
                    // so the gallery — and the room — is on that side).
                    x: suggestionPopup.width + 6
                    categoryColorFn: root.categoryColor
                    aliases: root.aliasList
                    implications: root.implicationList
                    active: suggestionPopup.visible && tagInput.highlighted >= 0
                            && tagInput.highlighted < tagInput.suggestions.length
                    tagName: active ? tagInput.suggestions[tagInput.highlighted].name : ""
                    nameTruncated: suggestionList.currentItem
                                   ? suggestionList.currentItem.nameTruncated : false
                }
            }
            Button {
                text: qsTr("Add")
                enabled: tagInput.text.trim().length > 0
                onClicked: tagInput.commit(tagInput.text)
            }
        }

    }
}
