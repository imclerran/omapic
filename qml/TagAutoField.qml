// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A single-line tag picker with an autocomplete dropdown, matching the detail
// panel's "add a tag" box — far easier than a long combo box on a big library.
// Type to filter `tags` by substring; `text` holds the current value, which the
// caller resolves to a tag by name. `accepted()` fires on Enter when no
// suggestion is highlighted (i.e. the user committed the typed text).
Item {
    id: root

    property var tags: []                 // candidates: [{ name, colorSlot, categoryName }]
    property string placeholderText: ""
    property var categoryColorFn: null    // function(colorSlot) -> color, optional
    property alias text: field.text
    signal accepted()

    implicitWidth: 180
    implicitHeight: field.implicitHeight

    function swatchColor(slot) {
        return (root.categoryColorFn && slot !== undefined && slot >= 0)
               ? root.categoryColorFn(slot) : "transparent"
    }
    function clear() { field.text = ""; field.suggestions = []; pop.close() }
    function focusInput() { field.forceActiveFocus() }

    TextField {
        id: field
        anchors.fill: parent
        placeholderText: root.placeholderText

        property var suggestions: []
        property int highlighted: -1

        function refresh() {
            const q = text.trim().toLowerCase()
            highlighted = -1
            if (q === "") { suggestions = []; pop.close(); return }
            const out = []
            let exact = null
            for (let i = 0; i < root.tags.length; ++i) {
                const ln = root.tags[i].name.toLowerCase()
                if (ln === q)
                    exact = root.tags[i] // keep the exact match, surfaced below
                else if (ln.indexOf(q) !== -1 && out.length < 8)
                    out.push(root.tags[i])
            }
            // When the text is an exact tag name, keep that tag at the top and
            // pre-highlight it — as though the user had arrow-keyed to it — so
            // Enter accepts the real (canonically-cased) tag. Matches the detail
            // panel's add-a-tag box 1:1.
            if (exact) {
                out.unshift(exact)
                if (out.length > 8) out.pop()
                highlighted = 0
            }
            suggestions = out
            if (out.length > 0) pop.open(); else pop.close()
        }
        function pick(name) { text = name; suggestions = []; pop.close() }
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

        Keys.onDownPressed: if (suggestions.length > 0)
                                highlighted = Math.min(highlighted + 1, suggestions.length - 1)
        Keys.onUpPressed: highlighted = Math.max(highlighted - 1, -1)
        Keys.onEscapePressed: pop.close()
        Keys.onReturnPressed: (event) => field.commit(event)
        Keys.onEnterPressed: (event) => field.commit(event)

        Popup {
            id: pop
            y: field.height
            x: 0
            width: field.width
            padding: 1
            closePolicy: Popup.CloseOnPressOutsideParent | Popup.CloseOnEscape

            contentItem: ListView {
                implicitHeight: Math.min(field.suggestions.length, 8) * 32
                model: field.suggestions
                clip: true
                currentIndex: field.highlighted

                delegate: ItemDelegate {
                    id: sg
                    required property int index
                    required property var modelData
                    width: ListView.view.width
                    height: 32
                    highlighted: index === field.highlighted
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
    }
}
