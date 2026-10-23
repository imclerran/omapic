// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Omapic // TextMatch singleton (accent-/case-insensitive folding)

// A single-line tag picker with an autocomplete dropdown, matching the detail
// panel's "add a tag" box — far easier than a long combo box on a big library.
// Type to filter `tags` by substring; `text` holds the current value, which the
// caller resolves to a tag by name. `accepted()` fires on Enter when no
// suggestion is highlighted (i.e. the user committed the typed text).
Item {
    id: root

    property var tags: []                 // candidates: [{ name, colorSlot, categoryName }]
    // Tag aliases: [{ aliasName, tagName, colorSlot }]. Typing an alias surfaces
    // its canonical tag as a match (shown as the tag itself) and fills it on
    // selection, so an alias resolves here too. Empty disables alias matching.
    property var aliases: []
    // Tag implications: [{ antecedentName, consequentName, consequentColorSlot,
    // consequentCategoryName }]. Drives the highlighted-entry info panel. Empty
    // disables it.
    property var implications: []
    // Lower-cased names to leave out of the suggestions (tags already on the
    // photo, say). Empty excludes nothing.
    property var excludeNames: []
    // When on, picking a highlighted suggestion (Enter or click) commits at once:
    // the text is set to the tag and accepted() fires in the same step, instead of
    // leaving the picked name in the field for a second Enter. For hosts where
    // choosing a suggestion *is* the action (triage), not a form field to fill.
    property bool commitOnPick: false
    property string placeholderText: ""
    property var categoryColorFn: null    // function(colorSlot) -> color, optional
    property alias text: field.text
    signal accepted()
    // Every key press reaching the field that no dedicated handler (Up/Down,
    // Return, Escape) consumed, before the text input itself sees it. A host that
    // treats a chord such as Ctrl+→ as a command sets event.accepted = true to keep
    // it out of the text; anything left unaccepted goes on to the field as usual.
    signal commandKeyPressed(var event)
    // Escape was pressed (the dropdown is already closed). Hosts that use Escape
    // to leave a mode hook this; by default the key is simply swallowed here.
    signal escapePressed()

    implicitWidth: 180
    implicitHeight: field.implicitHeight

    SystemPalette { id: sys }

    // The dot shown beside a suggestion: the category's color, or the neutral grey
    // for an uncategorized tag (so every row carries a dot, aligned).
    function swatchColor(slot) {
        return (root.categoryColorFn && slot !== undefined && slot >= 0)
               ? root.categoryColorFn(slot) : sys.mid
    }
    function clear() { field.text = ""; field.suggestions = []; pop.close() }
    function focusInput() { field.forceActiveFocus() }

    TextField {
        id: field
        anchors.fill: parent
        placeholderText: root.placeholderText
        // '|' is the XMP sidecar hierarchy separator, so it can't be part of a
        // tag name; reject it on entry (the backend also strips it as a safety net).
        validator: RegularExpressionValidator { regularExpression: /[^|]*/ }

        property var suggestions: []
        property int highlighted: -1

        function refresh() {
            const q = TextMatch.fold(text.trim())
            highlighted = -1
            if (q === "") { suggestions = []; pop.close(); return }
            const out = []
            const seen = ({}) // folded canonical name -> index in `out`, deduped
            let exact = -1    // index of an exact (name or alias) match, to pre-highlight

            const excluded = root.excludeNames || []
            // Tags whose name contains the query (folded, so "cafe" finds "café").
            for (let i = 0; i < root.tags.length; ++i) {
                if (excluded.indexOf(root.tags[i].name.toLowerCase()) !== -1)
                    continue
                const ln = TextMatch.fold(root.tags[i].name)
                if (ln.indexOf(q) === -1)
                    continue
                if (!(ln in seen) && out.length < 8) {
                    seen[ln] = out.length
                    out.push(root.tags[i])
                }
                if (ln === q && (ln in seen))
                    exact = seen[ln]
            }
            // A typed alias resolves to its canonical tag. Show the canonical tag
            // itself (same name/color/category) — never a second row for a tag
            // already listed, and pre-highlight it when the alias is an exact match.
            for (let i = 0; i < root.aliases.length; ++i) {
                const a = root.aliases[i]
                if (excluded.indexOf((a.tagName || "").toLowerCase()) !== -1)
                    continue // canonical already excluded
                const af = TextMatch.fold(a.aliasName)
                if (af.indexOf(q) === -1)
                    continue
                const cn = TextMatch.fold(a.tagName)
                if (!(cn in seen) && out.length < 8) {
                    seen[cn] = out.length
                    out.push({ name: a.tagName, colorSlot: a.colorSlot, categoryName: a.categoryName })
                }
                if (af === q && (cn in seen))
                    exact = seen[cn]
            }

            // Surface the exact match at the top, pre-highlighted — as though the
            // user had arrow-keyed to it — so Enter accepts the canonical tag.
            if (exact >= 0) {
                out.unshift(out.splice(exact, 1)[0])
                highlighted = 0
            }
            suggestions = out
            if (out.length > 0) pop.open(); else pop.close()
        }
        function pick(name) {
            text = name; suggestions = []; pop.close()
            if (root.commitOnPick)
                root.accepted()
        }
        function commit(event) {
            if (pop.visible && highlighted >= 0 && highlighted < suggestions.length) {
                pick(suggestions[highlighted].name)
                event.accepted = true
            } else {
                pop.close()
                root.accepted()
            }
        }

        onTextEdited: refresh()
        onActiveFocusChanged: if (!activeFocus) pop.close()

        ClearButton { field: field }

        Keys.onDownPressed: if (suggestions.length > 0)
                                highlighted = Math.min(highlighted + 1, suggestions.length - 1)
        Keys.onUpPressed: highlighted = Math.max(highlighted - 1, -1)
        // Escape first dismisses the open suggestion dropdown; only a second
        // Escape (with the dropdown already closed) is forwarded as escapePressed,
        // so a host that uses it to leave a mode doesn't exit mid-autocomplete.
        Keys.onEscapePressed: {
            const wasOpen = pop.visible
            pop.close()
            if (!wasOpen)
                root.escapePressed()
        }
        Keys.onReturnPressed: (event) => field.commit(event)
        Keys.onEnterPressed: (event) => field.commit(event)
        // Runs before the text input handles the key (and only for keys the
        // handlers above didn't take), so a host can claim chords the input would
        // otherwise eat — Ctrl+←/→ (word movement), Ctrl+Z (undo), Ctrl+Delete.
        Keys.onPressed: (event) => root.commandKeyPressed(event)

        Popup {
            id: pop
            y: field.height
            x: 0
            width: field.width
            padding: 1
            closePolicy: Popup.CloseOnPressOutsideParent | Popup.CloseOnEscape

            contentItem: ListView {
                id: sugList
                implicitHeight: Math.min(field.suggestions.length, 8) * 32
                model: field.suggestions
                clip: true
                currentIndex: field.highlighted

                delegate: ItemDelegate {
                    id: sg
                    required property int index
                    required property var modelData
                    readonly property bool nameTruncated: sgName.truncated
                    width: ListView.view.width
                    height: 32
                    highlighted: index === field.highlighted
                    hoverEnabled: true
                    // A click picks the row; it must not pull focus off the field.
                    focusPolicy: Qt.NoFocus
                    onHoveredChanged: if (hovered) field.highlighted = index
                    onClicked: field.pick(sg.modelData.name)

                    contentItem: RowLayout {
                        spacing: 8
                        Rectangle {
                            width: 10
                            height: 10
                            radius: 5
                            color: root.swatchColor(sg.modelData.colorSlot)
                        }
                        Label {
                            id: sgName
                            Layout.fillWidth: true
                            text: sg.modelData.name
                            elide: Text.ElideRight
                            // Track the highlight so the name stays readable (see
                            // the suggestion-contrast fix on the detail panel).
                            color: sg.highlighted ? sg.palette.highlightedText : sg.palette.text
                        }
                        Label {
                            visible: (sg.modelData.categoryName || "") !== ""
                            text: sg.modelData.categoryName || ""
                            font.pixelSize: 11
                            color: sg.highlighted ? sg.palette.highlightedText : sg.palette.placeholderText
                        }
                    }
                }
            }
        }

        // Detail for the highlighted suggestion (full name, aliases, implications),
        // to the right of the dropdown — helps tell similar tag names apart.
        TagInfoPopover {
            parent: field
            preferredY: pop.y
            x: pop.width + 6
            aliases: root.aliases
            implications: root.implications
            categoryColorFn: root.categoryColorFn
            active: pop.visible && field.highlighted >= 0
                    && field.highlighted < field.suggestions.length
            tagName: active ? field.suggestions[field.highlighted].name : ""
            nameTruncated: sugList.currentItem ? sugList.currentItem.nameTruncated : false
        }
    }
}
