// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Omapic // TextMatch, Clipboard

// The OQL query builder: a row of chips — tags, predicates, parentheses, OR
// bars, each optionally negated — with a tag-entry field at the cursor, backed
// by a flat token list. The parser (Library.parseQuery) validates the list on
// every change, the live match count is shown beneath, and a raw-text toggle
// exposes the query as OQL for power users. `text` is the query to save.
//
// Keys in the field: Tab/Enter or a click commits a chip (Space never does —
// names contain spaces); ←/→ step across chips when the caret is at an end;
// Backspace on an empty field selects the previous chip, and again turns it
// back into text; Shift+arrows select a range; - on a selection toggles NOT;
// | inserts an OR; ) steps over an auto-inserted ); Ctrl+C / Ctrl+V copy the
// selection as text and paste text as chips; Ctrl+A selects every chip.
Item {
    id: root

    property var library
    property color accent: "#4a90d9"
    property var categoryColorFn: null
    property var tagSampleFn: null
    // Suggestion sources, bound by the host.
    property var tags: []         // [{name, colorSlot, categoryName}]
    property var aliases: []
    property var implications: []

    // --- the token list and everything derived from it ---
    // Each token: {type: tag|pred|open|close|or|bad, negated, name, id, kind,
    // value, colorSlot, categoryName}. `name` is the tag name, the predicate key
    // (is/album/cat) or a bad token's raw text; `value` the predicate's value.
    property var tokens: []
    property int cursor: 0          // insertion index into `tokens`
    property int selStart: -1       // selected chip range, inclusive; -1 = none
    property int selEnd: -1
    property int anchor: -1         // the fixed end of a Shift-selection
    property bool pendingNegate: false // the next committed chip is negated
    property bool rawMode: false
    property string rawText: ""
    property var corrections: []    // rows offered after Enter on unresolvable text
    readonly property bool hasSelection: selStart >= 0

    property string serialized: ""  // the tokens as OQL (every name quoted)
    property var tokenSpans: []     // [{start, length}] of each token in `serialized`
    property var parsed: ({ ok: false, message: "" })
    property var unmatched: []      // indices of parentheses without a partner
    property int errorToken: -1     // the token the parse error points into
    property int matchCount: -1     // live result count, -1 when invalid
    readonly property bool valid: parsed.ok === true
    readonly property bool empty: rawMode ? rawText.trim() === "" : tokens.length === 0
    // The query text to save: the canonical reading when it parses, else what is there.
    readonly property string text: rawMode ? rawText : (valid ? parsed.canonical : serialized)
    readonly property string errorMessage: valid || empty ? "" : (parsed.message || "")

    // Enter on an empty field with nothing selected: the host's "accept".
    signal submitted()

    SystemPalette { id: sys }
    // Color for a tag's category palette slot: the host's function, else the
    // Omarchy palette (the accent for uncategorized tags), like the other hosts.
    function colorFor(slot) {
        if (root.categoryColorFn)
            return root.categoryColorFn(slot)
        const pal = omarchyTheme.categoryPalette
        if (slot === undefined || slot === null || slot < 0 || pal.length === 0)
            return root.accent
        return pal[slot % pal.length]
    }
    function focusInput() { if (root.rawMode) rawField.forceActiveFocus(); else input.focusInput() }
    readonly property var vocab: root.library ? root.library.queryVocabulary() : ({ albums: [], categories: [] })

    // --- loading / reading the query ---
    function setQueryText(t) {
        const toks = root.library ? root.library.tokenizeQuery(t) : []
        const list = []
        for (let i = 0; i < toks.length; ++i)
            list.push(fromToken(toks[i]))
        root.rawText = t
        root.tokens = list
        root.cursor = list.length
        clearSelection()
        root.pendingNegate = false
    }
    function clear() { setQueryText(""); root.rawMode = false; input.clear() }
    function fromToken(t) {
        return { type: t.type, negated: t.negated === true, name: t.name, id: t.id, kind: t.kind || "",
                 value: t.value || "", colorSlot: t.colorSlot === undefined ? -1 : t.colorSlot,
                 categoryName: t.categoryName || "" }
    }

    // --- serialization (always-quoted names, so the text is unambiguous) ---
    function serializeToken(t) {
        const neg = t.negated ? "-" : ""
        switch (t.type) {
        case "tag": return neg + "\"" + t.name + "\""
        case "pred": return neg + t.name + ":" + (t.name === "is" ? t.value : "\"" + t.value + "\"")
        case "open": return neg + "("
        case "close": return ")"
        case "or": return "|"
        default: return neg + "\"" + t.name + "\""
        }
    }
    function chipLabel(t) {
        switch (t.type) {
        case "tag": return t.name
        case "pred": return t.name + ":" + t.value
        case "open": return "("
        case "close": return ")"
        case "or": return "|"
        default: return t.name
        }
    }
    function revalidate() {
        let s = ""
        const spans = []
        for (let i = 0; i < root.tokens.length; ++i) {
            if (i > 0) s += " "
            const piece = serializeToken(root.tokens[i])
            spans.push({ start: s.length, length: piece.length })
            s += piece
        }
        root.serialized = s
        root.tokenSpans = spans
        // Unmatched parentheses, for the underline.
        const stack = [], bad = []
        for (let i = 0; i < root.tokens.length; ++i) {
            const ty = root.tokens[i].type
            if (ty === "open") stack.push(i)
            else if (ty === "close") { if (stack.length) stack.pop(); else bad.push(i) }
        }
        root.unmatched = bad.concat(stack)
        const q = root.rawMode ? root.rawText : s
        if (q.trim() === "" || !root.library) {
            root.parsed = { ok: false, message: "" }
            root.errorToken = -1
            root.matchCount = -1
            return
        }
        const p = root.library.parseQuery(q)
        root.parsed = p
        root.matchCount = p.ok ? root.library.queryMatchCount(q) : -1
        let et = -1
        if (!p.ok && !root.rawMode)
            for (let i = 0; i < spans.length; ++i)
                if (p.start >= spans[i].start && p.start < spans[i].start + Math.max(1, spans[i].length)) { et = i; break }
        root.errorToken = et
    }
    onTokensChanged: revalidate()
    onRawTextChanged: if (rawMode) revalidate()
    function setRawMode(on) {
        if (on === root.rawMode)
            return
        if (on) {
            root.rawText = root.valid ? root.parsed.canonical : root.serialized
            root.rawMode = true
            revalidate()
            rawField.forceActiveFocus()
        } else {
            const t = root.rawText
            root.rawMode = false
            setQueryText(t)
            input.focusInput()
        }
    }

    // --- editing the token list ---
    function insertTokens(list) {
        const next = root.tokens.slice()
        for (let i = 0; i < list.length; ++i)
            next.splice(root.cursor + i, 0, list[i])
        root.tokens = next
        root.cursor += list.length
        clearSelection()
    }
    function removeRange(a, b) { // inclusive
        const next = root.tokens.slice()
        next.splice(a, b - a + 1)
        root.tokens = next
        if (root.cursor > b) root.cursor -= (b - a + 1)
        else if (root.cursor > a) root.cursor = a
        clearSelection()
    }
    function commitChip(t) {
        if (root.pendingNegate && (t.type === "tag" || t.type === "pred" || t.type === "open" || t.type === "bad")) {
            t.negated = !t.negated
            root.pendingNegate = false
        }
        insertTokens([t])
        root.corrections = []
        input.clear()
    }
    function insertGroup() {
        insertTokens([{ type: "open", negated: root.pendingNegate }, { type: "close" }])
        root.pendingNegate = false
        root.cursor -= 1 // between the parentheses
        root.corrections = []
        input.clear()
    }
    function closeParen() {
        // Step over an auto-inserted ')' rather than adding a second one.
        if (root.cursor < root.tokens.length && root.tokens[root.cursor].type === "close")
            root.cursor += 1
        else
            insertTokens([{ type: "close" }])
        input.clear()
    }
    function insertOr() { insertTokens([{ type: "or" }]); input.clear() }

    // --- selection ---
    function clearSelection() { root.selStart = -1; root.selEnd = -1; root.anchor = -1 }
    function selectRange(a, b) {
        if (a > b) { const t = a; a = b; b = t }
        root.selStart = Math.max(0, a)
        root.selEnd = Math.min(root.tokens.length - 1, b)
        if (root.selEnd < root.selStart) clearSelection()
    }
    function isSelected(i) { return root.selStart >= 0 && i >= root.selStart && i <= root.selEnd }
    function deleteSelection() {
        if (!root.hasSelection) return
        removeRange(root.selStart, root.selEnd)
    }
    function toggleNegateSelection() {
        const next = root.tokens.slice()
        for (let i = root.selStart; i <= root.selEnd; ++i) {
            const t = next[i]
            if (t.type === "tag" || t.type === "pred" || t.type === "open" || t.type === "bad")
                next[i] = Object.assign({}, t, { negated: !t.negated })
        }
        root.tokens = next
    }
    // Turn one chip back into editable text in the field.
    function editToken(i) {
        const t = root.tokens[i]
        root.pendingNegate = t.negated === true
        const txt = t.type === "pred" ? t.name + ":" + t.value : (t.type === "tag" || t.type === "bad" ? t.name : chipLabel(t))
        removeRange(i, i)
        root.cursor = i
        input.text = txt
        input.cursorPosition = txt.length
        input.focusInput()
        input.refreshSuggestions()
    }
    function selectedText() {
        if (!root.hasSelection) return ""
        let s = ""
        for (let i = root.selStart; i <= root.selEnd; ++i)
            s += (i > root.selStart ? " " : "") + serializeToken(root.tokens[i])
        // The canonical reading when the selection parses on its own.
        const p = root.library ? root.library.parseQuery(s) : { ok: false }
        return p.ok ? p.canonical : s
    }
    function copySelection() {
        const s = selectedText()
        if (s !== "") Clipboard.setText(s)
    }
    function pasteText(t) {
        const toks = root.library ? root.library.tokenizeQuery(t) : []
        const list = []
        for (let i = 0; i < toks.length; ++i)
            list.push(fromToken(toks[i]))
        if (list.length === 0) return
        insertTokens(list)
        // Highlight what couldn't be resolved by selecting the first bad chip.
        for (let i = root.cursor - list.length; i < root.cursor; ++i)
            if (root.tokens[i].type === "bad") { selectRange(i, i); break }
    }

    // --- committing typed text ---
    function commitText(t) {
        t = t.trim()
        if (t === "") {
            if (root.hasSelection) { clearSelection(); return }
            if (root.pendingNegate) { root.pendingNegate = false; return }
            root.submitted()
            return
        }
        if (t === "(") { insertGroup(); return }
        if (t === ")") { closeParen(); return }
        if (t === "|") { insertOr(); return }
        if (t === "-") { root.pendingNegate = !root.pendingNegate; input.clear(); return }
        const toks = root.library.tokenizeQuery(t)
        if (toks.length === 1 && toks[0].type !== "bad") {
            commitChip(fromToken(toks[0]))
            return
        }
        // No exact match: offer corrections, or a split into the terms it does
        // resolve to, and keep the text for the user to decide.
        const rows = []
        let allKnown = toks.length > 1
        for (let i = 0; i < toks.length; ++i)
            if (toks[i].type === "bad") allKnown = false
        if (allKnown) {
            const parts = []
            for (let i = 0; i < toks.length; ++i)
                parts.push(chipLabel(fromToken(toks[i])))
            rows.push({ kind: "split", name: qsTr("Split into %1 terms").arg(toks.length),
                        detail: parts.join("  "), tokens: toks, exact: true })
        }
        const p = root.library.parseQuery(t)
        const sugg = p.suggestions || []
        for (let i = 0; i < sugg.length; ++i) {
            const name = sugg[i].replace(/^"(.*)"$/, "$1")
            const info = tagInfo(name)
            if (info)
                rows.push({ kind: "tag", name: info.name, colorSlot: info.colorSlot, categoryName: info.categoryName,
                            detail: qsTr("did you mean") })
            else if (sugg[i].indexOf(":") > 0)
                rows.push({ kind: "predicate", name: sugg[i], detail: qsTr("did you mean") })
        }
        // The best offer is pre-highlighted, so a second Enter takes it.
        if (rows.length > 0 && !rows[0].exact)
            rows[0] = Object.assign({ exact: true }, rows[0])
        root.corrections = rows
        input.refreshSuggestions()
    }
    function tagInfo(name) {
        const f = TextMatch.fold(name)
        for (let i = 0; i < root.tags.length; ++i)
            if (TextMatch.fold(root.tags[i].name) === f)
                return root.tags[i]
        for (let i = 0; i < root.aliases.length; ++i)
            if (TextMatch.fold(root.aliases[i].tagName) === f)
                return { name: root.aliases[i].tagName, colorSlot: root.aliases[i].colorSlot,
                         categoryName: root.aliases[i].categoryName || "" }
        return null
    }

    // --- suggestions ---
    readonly property var predicateRows: [
        { kind: "predicate", name: "is:fav", detail: qsTr("in Favorites") },
        { kind: "predicate", name: "is:untagged", detail: qsTr("has no tags") },
        { kind: "predicate", name: "album:", detail: qsTr("in an album…") },
        { kind: "predicate", name: "cat:", detail: qsTr("has a tag of a category…") },
    ]
    function predicatePrefix(t) {
        const m = t.trim().match(/^(album|cat):(.*)$/i)
        return m ? { key: m[1].toLowerCase(), rest: m[2] } : null
    }
    function extraRows(text, q) {
        const rows = []
        const t = text.trim()
        const pre = predicatePrefix(t)
        if (pre) {
            const rest = TextMatch.fold(pre.rest.trim())
            const list = pre.key === "album" ? root.vocab.albums : root.vocab.categories
            for (let i = 0; i < list.length && rows.length < 8; ++i) {
                const f = TextMatch.fold(list[i].name)
                if (rest !== "" && f.indexOf(rest) === -1)
                    continue
                rows.push(pre.key === "album"
                          ? { kind: "album", name: list[i].name, smart: list[i].smart === true,
                              detail: list[i].smart ? qsTr("smart album") : qsTr("album"), exact: f === rest }
                          : { kind: "category", name: list[i].name, colorSlot: list[i].colorSlot,
                              detail: qsTr("category"), exact: f === rest })
            }
            return rows
        }
        for (let i = 0; i < root.corrections.length; ++i)
            rows.push(root.corrections[i])
        if (t === "(")
            rows.push({ kind: "syntax", name: "( )", detail: qsTr("group"), action: "group", exact: true })
        if (t === "-")
            rows.push({ kind: "syntax", name: qsTr("NOT…"), detail: qsTr("negate the next term"), action: "not", exact: true })
        if (t === "|")
            rows.push({ kind: "syntax", name: "|", detail: qsTr("OR"), action: "or", exact: true })
        const f = TextMatch.fold(t)
        if (f !== "")
            for (let i = 0; i < root.predicateRows.length; ++i) {
                const p = root.predicateRows[i]
                if (p.name.indexOf(f) === 0)
                    rows.push(Object.assign({ exact: f === p.name }, p))
            }
        return rows
    }
    function onRowPicked(row) {
        switch (row.kind) {
        case "tag":
            commitChip({ type: "tag", name: row.name, colorSlot: row.colorSlot === undefined ? -1 : row.colorSlot,
                         categoryName: row.categoryName || "" })
            break
        case "syntax":
            if (row.action === "group") insertGroup()
            else if (row.action === "or") insertOr()
            else { root.pendingNegate = !root.pendingNegate; input.clear() }
            break
        case "predicate":
            if (row.name.endsWith(":")) {
                input.text = row.name
                input.cursorPosition = row.name.length
                input.refreshSuggestions()
            } else {
                const parts = row.name.split(":")
                commitChip({ type: "pred", name: parts[0], value: parts[1] })
            }
            break
        case "album":
            commitChip({ type: "pred", name: "album", value: row.name, kind: row.smart ? "smart" : "album" })
            break
        case "category":
            commitChip({ type: "pred", name: "cat", value: row.name, kind: "category", colorSlot: row.colorSlot })
            break
        case "split": {
            const list = []
            for (let i = 0; i < row.tokens.length; ++i)
                list.push(fromToken(row.tokens[i]))
            if (root.pendingNegate && list.length) { list[0].negated = !list[0].negated; root.pendingNegate = false }
            insertTokens(list)
            root.corrections = []
            input.clear()
            break
        }
        }
    }

    // --- keys the field doesn't handle itself ---
    function handleKey(event) {
        const empty = input.text.length === 0
        const atStart = input.cursorPosition === 0
        const atEnd = input.cursorPosition === input.text.length
        const shift = event.modifiers & Qt.ShiftModifier
        const ctrl = event.modifiers & Qt.ControlModifier
        if (ctrl && event.key === Qt.Key_C && root.hasSelection) { copySelection(); event.accepted = true; return }
        if (ctrl && event.key === Qt.Key_V && empty) { pasteText(Clipboard.text()); event.accepted = true; return }
        if (ctrl && event.key === Qt.Key_A && empty && root.tokens.length) { selectRange(0, root.tokens.length - 1); event.accepted = true; return }
        if (event.key === Qt.Key_Tab) {
            if (input.popupOpen && input.highlightedRow) onRowPicked(input.highlightedRow)
            else commitText(input.text)
            event.accepted = true
            return
        }
        if (event.key === Qt.Key_Left && (empty || atStart)) {
            if (root.cursor > 0) {
                if (shift) { const a = root.anchor >= 0 ? root.anchor : root.cursor - 1; root.cursor -= 1; root.anchor = a; selectRange(a, root.cursor) }
                else { clearSelection(); root.cursor -= 1 }
            }
            event.accepted = true
            return
        }
        if (event.key === Qt.Key_Right && (empty || atEnd)) {
            if (root.cursor < root.tokens.length) {
                if (shift) { const a = root.anchor >= 0 ? root.anchor : root.cursor; root.cursor += 1; root.anchor = a; selectRange(a, root.cursor - 1) }
                else { clearSelection(); root.cursor += 1 }
            }
            event.accepted = true
            return
        }
        if (event.key === Qt.Key_Backspace && empty) {
            event.accepted = true
            if (root.hasSelection) {
                if (root.selStart === root.selEnd) editToken(root.selStart)
                else deleteSelection()
                return
            }
            if (root.pendingNegate) { root.pendingNegate = false; return }
            if (root.cursor === 0) return
            const prev = root.tokens[root.cursor - 1]
            const next = root.cursor < root.tokens.length ? root.tokens[root.cursor] : null
            // Backspace on an empty "()" removes both.
            if (prev.type === "open" && next && next.type === "close") { removeRange(root.cursor - 1, root.cursor); return }
            selectRange(root.cursor - 1, root.cursor - 1)
            return
        }
        if (event.key === Qt.Key_Delete && empty) {
            event.accepted = true
            if (root.hasSelection) { deleteSelection(); return }
            if (root.cursor < root.tokens.length) removeRange(root.cursor, root.cursor)
            return
        }
        if (event.text === "-" && root.hasSelection) { toggleNegateSelection(); event.accepted = true; return }
        if (event.text === "|") {
            if (!empty) commitText(input.text)
            if (input.text.length === 0) insertOr()
            event.accepted = true
            return
        }
        if (event.text === ")") {
            // Only outside a name being typed: "dinner (2019" keeps its ')'.
            const opens = (input.text.match(/\(/g) || []).length
            const closes = (input.text.match(/\)/g) || []).length
            if (empty || opens <= closes) {
                if (!empty) commitText(input.text)
                if (input.text.length === 0) closeParen()
                event.accepted = true
            }
            return
        }
        if (event.key === Qt.Key_Escape) { clearSelection(); root.pendingNegate = false }
    }

    implicitHeight: column.implicitHeight

    ColumnLayout {
        id: column
        anchors.fill: parent
        spacing: 6

        // The chip row (or the raw text field), framed like a text field.
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: Math.max(36, root.rawMode ? rawField.implicitHeight + 8 : flow.implicitHeight + 10)
            radius: 4
            color: sys.base
            border.width: 1
            border.color: (root.rawMode ? rawField.activeFocus : input.textField.activeFocus) ? root.accent : sys.mid

            TextField {
                id: rawField
                visible: root.rawMode
                anchors.fill: parent
                anchors.margins: 2
                background: null
                font.family: "monospace"
                placeholderText: qsTr("e.g. beach (sunset | sunrise) -people")
                text: root.rawText
                onTextEdited: root.rawText = text
                onAccepted: if (root.valid) root.submitted()
            }

            MouseArea {
                anchors.fill: parent
                visible: !root.rawMode
                onClicked: { root.clearSelection(); root.cursor = root.tokens.length; input.focusInput() }
            }

            Flow {
                id: flow
                visible: !root.rawMode
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 5
                spacing: 4

                component Chip: Rectangle {
                    id: chip
                    required property var tok
                    required property int tokenIndex
                    readonly property bool selected: root.isSelected(tokenIndex)
                    readonly property bool invalid: root.errorToken === tokenIndex || tok.type === "bad"
                    readonly property bool bracket: tok.type === "open" || tok.type === "close"
                    readonly property bool unmatchedParen: bracket && root.unmatched.indexOf(tokenIndex) !== -1
                    readonly property bool isTag: tok.type === "tag"
                    readonly property bool categorized: isTag && tok.colorSlot >= 0
                    readonly property color tint: tok.colorSlot >= 0 ? root.colorFor(tok.colorSlot) : sys.mid
                    height: 24
                    width: label.implicitWidth + (bracket || tok.type === "or" ? 10 : 14)
                    radius: bracket || tok.type === "or" ? 3 : height / 2
                    color: selected ? sys.highlight
                         : bracket || tok.type === "or" ? "transparent"
                         : categorized ? Qt.alpha(tint, tok.negated ? 0.06 : 0.18)
                         : sys.alternateBase
                    border.width: bracket || tok.type === "or" ? 0 : 1
                    border.color: invalid ? "#e06c75" : categorized ? tint : sys.mid

                    Label {
                        id: label
                        anchors.centerIn: parent
                        text: (chip.tok.negated ? "−" : "") + root.chipLabel(chip.tok)
                        font.pixelSize: chip.bracket || chip.tok.type === "or" ? 16 : 12
                        font.bold: chip.bracket || chip.tok.type === "or"
                        font.family: chip.tok.type === "pred" ? "monospace" : Qt.application.font.family
                        font.strikeout: false
                        color: chip.selected ? sys.highlightedText
                             : chip.invalid ? "#e06c75"
                             : chip.bracket || chip.tok.type === "or" ? root.accent
                             : sys.text
                    }
                    // Underline for an unmatched parenthesis (and a bad token).
                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        anchors.margins: 3
                        height: 2
                        visible: chip.unmatchedParen
                        color: "#e06c75"
                    }
                    HoverHandler { id: chipHover }
                    ToolTip.visible: chipHover.hovered && (chip.invalid || chip.unmatchedParen || (chip.isTag && chip.tok.categoryName !== ""))
                    ToolTip.delay: 400
                    ToolTip.text: chip.tok.type === "bad" ? qsTr("Unknown name")
                                : chip.unmatchedParen ? qsTr("Unmatched parenthesis")
                                : chip.invalid ? (root.parsed.message || "") : (chip.tok.categoryName || "")
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: (mouse) => {
                            if (mouse.modifiers & Qt.ShiftModifier) {
                                const a = root.anchor >= 0 ? root.anchor : (root.selStart >= 0 ? root.selStart : chip.tokenIndex)
                                root.anchor = a
                                root.selectRange(a, chip.tokenIndex)
                            } else {
                                root.clearSelection()
                                root.selectRange(chip.tokenIndex, chip.tokenIndex)
                                root.anchor = chip.tokenIndex
                            }
                            root.cursor = chip.tokenIndex + 1
                            input.focusInput()
                        }
                        onDoubleClicked: root.editToken(chip.tokenIndex)
                    }
                }

                Repeater {
                    model: root.tokens.slice(0, root.cursor)
                    delegate: Chip {
                        required property int index
                        required property var modelData
                        tok: modelData
                        tokenIndex: index
                    }
                }

                // The pending NOT marker, then the entry field at the cursor.
                Label {
                    visible: root.pendingNegate
                    height: 24
                    verticalAlignment: Text.AlignVCenter
                    text: "−"
                    font.pixelSize: 16
                    font.bold: true
                    color: root.accent
                }
                TagAutoField {
                    id: input
                    height: 24
                    width: Math.max(root.tokens.length === 0 && text.length === 0 ? 220 : 80,
                                    inputMetrics.advanceWidth + 28)
                    tags: root.tags
                    aliases: root.aliases
                    implications: root.implications
                    categoryColorFn: root.colorFor
                    sampleFn: root.tagSampleFn
                    suggestionMinWidth: 320 // the entry field is only as wide as its text
                    placeholderText: root.tokens.length === 0 ? qsTr("Type a tag, is:, album:, cat:, ( or -") : ""
                    // Syntax is typed here, so only '"' is barred (it can't be quoted).
                    validatorRegex: /[^"]*/
                    filterFn: (t) => { const p = root.predicatePrefix(t); return p ? p.rest : t }
                    extraRowsFn: root.extraRows
                    suggestTags: root.predicatePrefix(text) === null
                    onRowPicked: (row) => root.onRowPicked(row)
                    onAccepted: root.commitText(text)
                    onCommandKeyPressed: (event) => root.handleKey(event)
                    onEscapePressed: { root.clearSelection(); root.pendingNegate = false }
                    onTextChanged: { if (text.length > 0) root.clearSelection(); if (root.corrections.length && text.length === 0) root.corrections = [] }
                    Component.onCompleted: {
                        textField.background = null
                        textField.topPadding = 0
                        textField.bottomPadding = 0
                        textField.leftPadding = 6
                    }
                    TextMetrics { id: inputMetrics; font: input.textField.font; text: input.text }
                }

                Repeater {
                    model: root.tokens.slice(root.cursor)
                    delegate: Chip {
                        required property int index
                        required property var modelData
                        tok: modelData
                        tokenIndex: root.cursor + index
                    }
                }
            }
        }

        // Verdict: the live count when the query parses, else the error (with
        // suggestions); and the raw-text toggle.
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                font.pixelSize: 12
                color: root.valid || root.empty ? sys.placeholderText : "#e06c75"
                text: {
                    if (root.empty)
                        return qsTr("Build a query from tags, predicates and groups.")
                    if (root.valid)
                        return (root.matchCount === 1 ? qsTr("%1 photo matches") : qsTr("%1 photos match")).arg(root.matchCount)
                               + (root.rawMode ? "" : "  ·  " + root.parsed.canonical)
                    let msg = root.errorMessage
                    const sugg = root.parsed.suggestions || []
                    if (sugg.length > 0)
                        msg += "  ·  " + qsTr("Did you mean: %1").arg(sugg.join(", "))
                    return msg
                }
            }
            ToolButton {
                text: root.rawMode ? qsTr("Chips") : qsTr("Text")
                font.pixelSize: 12
                ToolTip.visible: hovered
                ToolTip.delay: 500
                ToolTip.text: root.rawMode ? qsTr("Back to chips") : qsTr("Edit the query as text")
                onClicked: root.setRawMode(!root.rawMode)
            }
        }
    }
}
