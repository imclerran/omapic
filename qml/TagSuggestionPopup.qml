// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The suggestion dropdown shared by every tag entry field (TagAutoField, the
// detail panel's add-a-tag box, the query builder). The host computes `rows`
// and drives `highlighted`; this only renders them, reports hover and clicks,
// and shows the tag-info popover beside the highlighted row when it is a tag.
//
// A row is {kind, name, …}: kind is "tag" (the default; shows the category dot
// and name, with `via` as "alias → name" when the row came from an alias and
// `categoryName` on the right), "album" / "category" (a name of that kind),
// "predicate" (a key such as is:fav, with `detail` explaining it), "syntax"
// (`( )`, "NOT…"; `detail` explains) or "split" (a correction offer; `detail`
// lists the parts). Non-tag rows show a glyph instead of a dot.
Popup {
    id: pop

    property var rows: []
    property int highlighted: -1
    property var categoryColorFn: null  // function(colorSlot) -> color
    // For the info popover beside a highlighted tag row (see TagInfoPopover).
    property var aliases: []
    property var implications: []
    property var sampleFn: null
    property bool infoEnabled: true
    // Whether the highlighted row's name is cut off (for the popover's full name).
    readonly property bool currentTruncated: list.currentItem ? list.currentItem.nameTruncated : false
    readonly property var currentRow: (highlighted >= 0 && highlighted < rows.length) ? rows[highlighted] : null

    signal picked(var row)
    signal hovered(int index)

    y: parent ? parent.height : 0
    x: 0
    width: parent ? parent.width : 200
    padding: 1
    closePolicy: Popup.CloseOnPressOutsideParent | Popup.CloseOnEscape

    SystemPalette { id: sys }

    // The dot beside a tag row: the category's color, or the neutral grey for an
    // uncategorized tag (so every row carries a dot, aligned).
    function swatchColor(slot) {
        return (pop.categoryColorFn && slot !== undefined && slot !== null && slot >= 0)
               ? pop.categoryColorFn(slot) : sys.mid
    }
    function glyphFor(row) {
        switch (row.kind) {
        case "album": return row.smart ? "◈" : "▣"
        case "category": return "●"
        case "predicate": return "∷"
        case "syntax": return "⌘"
        case "split": return "⇢"
        default: return ""
        }
    }

    contentItem: ListView {
        id: list
        implicitHeight: Math.min(pop.rows.length, 8) * 32
        model: pop.rows
        clip: true
        currentIndex: pop.highlighted

        delegate: ItemDelegate {
            id: sg
            required property int index
            required property var modelData
            readonly property bool nameTruncated: sgName.truncated
            readonly property bool isTag: (modelData.kind || "tag") === "tag"
            width: ListView.view.width
            height: 32
            highlighted: index === pop.highlighted
            hoverEnabled: true
            // A click picks the row; it must not pull focus off the field.
            focusPolicy: Qt.NoFocus
            onHoveredChanged: if (hovered) pop.hovered(index)
            onClicked: pop.picked(sg.modelData)

            contentItem: RowLayout {
                spacing: 8
                Rectangle {
                    visible: sg.isTag || sg.modelData.kind === "category"
                    width: 10
                    height: 10
                    radius: 5
                    color: pop.swatchColor(sg.modelData.colorSlot)
                }
                Label {
                    visible: !sg.isTag && sg.modelData.kind !== "category"
                    text: pop.glyphFor(sg.modelData)
                    font.pixelSize: 12
                    color: sg.highlighted ? sg.palette.highlightedText : sg.palette.placeholderText
                }
                Label {
                    id: sgName
                    Layout.fillWidth: true
                    text: (sg.modelData.via ? sg.modelData.via + " → " : "") + sg.modelData.name
                    elide: Text.ElideRight
                    // Track the highlight so the name stays readable (see
                    // the suggestion-contrast fix on the detail panel).
                    color: sg.highlighted ? sg.palette.highlightedText : sg.palette.text
                }
                Label {
                    readonly property string side: sg.isTag ? (sg.modelData.categoryName || "")
                                                             : (sg.modelData.detail || "")
                    visible: side !== ""
                    text: side
                    elide: Text.ElideRight
                    Layout.maximumWidth: sg.width * 0.5
                    font.pixelSize: 11
                    color: sg.highlighted ? sg.palette.highlightedText : sg.palette.placeholderText
                }
            }
        }
    }

    // Detail for the highlighted tag row (full name, aliases, implications,
    // sample photos), to the right of the dropdown — helps tell similar tag
    // names apart. Only tag rows have anything to say.
    TagInfoPopover {
        parent: pop.parent
        preferredY: pop.y
        x: pop.width + 6
        aliases: pop.aliases
        implications: pop.implications
        categoryColorFn: pop.categoryColorFn
        sampleFn: pop.sampleFn
        active: pop.infoEnabled && pop.visible && pop.currentRow !== null
                && (pop.currentRow.kind || "tag") === "tag"
        tagName: active ? pop.currentRow.name : ""
        nameTruncated: pop.currentTruncated
    }
}
