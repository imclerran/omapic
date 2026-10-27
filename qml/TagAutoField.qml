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
//
// Hosts with more to offer than tags (the query builder) can narrow what part
// of the text is matched (filterFn), add rows of their own (extraRowsFn),
// switch tag rows off (suggestTags), relax the validator, and hear about every
// picked row (rowPicked). The defaults keep the plain tag-entry behaviour.
Item {
    id: root

    property var tags: []                 // candidates: [{ name, colorSlot, categoryName }]
    // Tag aliases: [{ aliasName, tagName, colorSlot }]. Typing an alias surfaces
    // its canonical tag as a match (shown as "alias → tag") and fills it on
    // selection, so an alias resolves here too. Empty disables alias matching.
    property var aliases: []
    // Tag implications: [{ antecedentName, consequentName, consequentColorSlot,
    // consequentCategoryName }]. Drives the highlighted-entry info panel. Empty
    // disables it.
    property var implications: []
    // function(tagName) -> [filePath, …] for the info panel's sample thumbnails
    // (see TagInfoPopover.sampleFn); null (the default) shows none.
    property var sampleFn: null
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
    property alias cursorPosition: field.cursorPosition
    property alias textField: field
    // function(text) -> the part of the text to match tags against; null matches
    // the whole (trimmed) text. The builder uses it to match only what follows
    // "album:" / "cat:".
    property var filterFn: null
    // function(text, query) -> rows to show before the tag rows ({kind, name, …}
    // as TagSuggestionPopup describes); null adds none.
    property var extraRowsFn: null
    // Whether tag (and alias) rows are offered at all.
    property bool suggestTags: true
    // The entry rule. The default is the tag-name rule shared by every name
    // editor; a host that accepts syntax (the query builder) passes its own.
    property var validatorRegex: /([^|"\s]+( [^|"\s]+)* ?)?/
    signal accepted()
    // A suggestion row was picked (by Enter or a click), of any kind. A "tag" row
    // also fills the text (and, with commitOnPick, fires accepted()); the host
    // handles every other kind itself.
    signal rowPicked(var row)
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

    readonly property bool popupOpen: pop.visible
    readonly property var highlightedRow: pop.currentRow

    function clear() { field.text = ""; field.suggestions = []; pop.close() }
    function focusInput() { field.forceActiveFocus() }
    function refreshSuggestions() { field.refresh() }
    function closeSuggestions() { pop.close() }

    TextField {
        id: field
        anchors.fill: parent
        placeholderText: root.placeholderText
        // Name rules (the backend sanitizes as a safety net): no '|' (the XMP
        // sidecar hierarchy separator), no '"' (the query language's quote, which
        // has no escape), no leading or doubled whitespace.
        validator: RegularExpressionValidator { regularExpression: root.validatorRegex }

        property var suggestions: []
        property int highlighted: -1

        function refresh() {
            const raw = root.filterFn ? root.filterFn(text) : text
            const q = TextMatch.fold((raw || "").trim())
            highlighted = -1
            const out = []
            const seen = ({}) // folded canonical name -> index in `out`, deduped
            let exact = -1    // index of an exact (name or alias) match, to pre-highlight

            if (root.extraRowsFn) {
                const extra = root.extraRowsFn(text, q) || []
                for (let e = 0; e < extra.length; ++e) {
                    out.push(extra[e])
                    if (extra[e].exact && exact < 0)
                        exact = out.length - 1
                }
            }
            if (q === "" && out.length === 0) { suggestions = []; pop.close(); return }

            const excluded = root.excludeNames || []
            if (root.suggestTags && q !== "") {
                const limit = out.length + 8
                // Tags whose name contains the query (folded, so "cafe" finds "café").
                for (let i = 0; i < root.tags.length; ++i) {
                    if (excluded.indexOf(root.tags[i].name.toLowerCase()) !== -1)
                        continue
                    const ln = TextMatch.fold(root.tags[i].name)
                    if (ln.indexOf(q) === -1)
                        continue
                    if (!(ln in seen) && out.length < limit) {
                        seen[ln] = out.length
                        out.push({ kind: "tag", name: root.tags[i].name, colorSlot: root.tags[i].colorSlot,
                                   categoryName: root.tags[i].categoryName })
                    }
                    if (ln === q && (ln in seen))
                        exact = seen[ln]
                }
                // A typed alias resolves to its canonical tag. Show the canonical tag
                // (as "alias → tag", same color/category) — never a second row for a
                // tag already listed — and pre-highlight it on an exact alias.
                for (let i = 0; i < root.aliases.length; ++i) {
                    const a = root.aliases[i]
                    if (excluded.indexOf((a.tagName || "").toLowerCase()) !== -1)
                        continue // canonical already excluded
                    const af = TextMatch.fold(a.aliasName)
                    if (af.indexOf(q) === -1)
                        continue
                    const cn = TextMatch.fold(a.tagName)
                    if (!(cn in seen) && out.length < limit) {
                        seen[cn] = out.length
                        out.push({ kind: "tag", name: a.tagName, via: a.aliasName, colorSlot: a.colorSlot,
                                   categoryName: a.categoryName })
                    }
                    if (af === q && (cn in seen))
                        exact = seen[cn]
                }
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
        function pick(row) {
            if ((row.kind || "tag") !== "tag") {
                root.rowPicked(row)
                return
            }
            text = row.name; suggestions = []; pop.close()
            root.rowPicked(row) // a host may replace the text here (the builder clears it)
            if (root.commitOnPick)
                root.accepted()
        }
        function commit(event) {
            if (pop.visible && highlighted >= 0 && highlighted < suggestions.length) {
                pick(suggestions[highlighted])
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

        TagSuggestionPopup {
            id: pop
            parent: field
            rows: field.suggestions
            highlighted: field.highlighted
            categoryColorFn: root.categoryColorFn
            aliases: root.aliases
            implications: root.implications
            sampleFn: root.sampleFn
            onHovered: (index) => field.highlighted = index
            onPicked: (row) => field.pick(row)
        }
    }
}
