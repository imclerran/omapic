// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Omapic

// Triage mode's sidebar, in place of the detail panel + tag filter: the add-tag
// field (always focused), the current photo's tags, the session's recent tags
// numbered for Ctrl+1–9, the Favorite / Trash / Undo buttons and a key legend.
// It only reads state and raises signals — the window performs every action
// through the Library and the TriageSession, so undo bookkeeping lives in one
// place. Nothing here takes keyboard focus but the field: every button is
// NoFocus, and the chords are handled on the field itself (before the text input
// sees them), so the keyboard never has to leave it.
Rectangle {
    id: root

    property var library
    property var session         // the TriageSession
    property color accent: "#4a90d9"
    // The photo being triaged (-1 once the queue is exhausted).
    property int photoId: -1

    signal applyTagRequested(string name)
    signal removeTagRequested(int tagId, string tagName)
    signal favoriteToggleRequested()
    signal trashRequested()
    signal undoRequested()
    signal nextRequested()
    signal previousRequested()
    signal exitRequested()

    // The current photo's tags ([{id, name, colorSlot, categoryName}]) and
    // favorite state, refreshed on every tag change.
    property var chips: []
    property bool favorite: false
    // Autocomplete candidates: the active set's tags, refreshed when tags change.
    property var suggestionTags: []
    readonly property var aliasList: root.library ? root.library.tagAliases : []
    readonly property var implicationList: root.library ? root.library.tagImplications : []
    readonly property var appliedLower: root.chips.map(function (t) { return t.name.toLowerCase() })
    readonly property var recent: root.session ? root.session.recentTags : []
    // Lower-cased names of the tags active for the enabled library roots (the same
    // set the autocomplete offers), so the recent list can be narrowed to them.
    readonly property var activeNamesLower: {
        const s = ({})
        const all = root.suggestionTags
        for (let i = 0; i < all.length; ++i)
            s[all[i].name.toLowerCase()] = true
        return s
    }
    // The recent tags actually shown: only those still active for the enabled
    // roots. Persisted tags whose photos have since left the active set (a folder
    // disabled or removed, the tag deleted or merged) drop out. Both the chips and
    // the Ctrl-number handler read this, so the numbering stays in step.
    readonly property var visibleRecent: root.recent.filter(function (name) {
        return root.activeNamesLower[name.toLowerCase()] === true
    })

    SystemPalette { id: sys }
    color: sys.window

    function focusTagInput() { tagField.focusInput() }

    // Color for a tag's category palette slot; the accent for uncategorized tags.
    function categoryColor(slot) {
        const pal = omarchyTheme.categoryPalette
        if (slot === undefined || slot < 0 || pal.length === 0)
            return root.accent
        return pal[slot % pal.length]
    }

    function refreshChips() {
        if (root.photoId < 0 || !root.library) {
            chips = []
            favorite = false
            return
        }
        chips = root.library.commonTags([root.photoId])
        favorite = root.library.isFavorite(root.photoId)
    }
    function refreshSuggestions() {
        suggestionTags = root.library ? root.library.tagSuggestions() : []
    }
    onPhotoIdChanged: refreshChips()
    Component.onCompleted: { refreshChips(); refreshSuggestions() }

    Connections {
        target: root.library
        function onPhotoChanged(id) { if (id === root.photoId) root.refreshChips() }
        // Favorites and bulk edits refresh the tag set without a per-photo signal;
        // deleted/renamed tags change the chips and the candidates alike.
        function onTagsChanged() { root.refreshChips(); root.refreshSuggestions() }
    }

    // The keyboard commands, on the field so they beat the text input's own
    // bindings (Ctrl+←/→ would otherwise move by word, Ctrl+Z undo typing). Plain
    // keys — arrows, letters, digits, symbols — are never commands and fall
    // through to the text.
    function handleCommandKey(event) {
        if (!(event.modifiers & Qt.ControlModifier))
            return
        switch (event.key) {
        case Qt.Key_Right:  root.nextRequested(); break
        case Qt.Key_Left:   root.previousRequested(); break
        case Qt.Key_Space:  root.favoriteToggleRequested(); break
        case Qt.Key_Delete: root.trashRequested(); break
        case Qt.Key_Z:      root.undoRequested(); break
        default:
            if (event.key >= Qt.Key_1 && event.key <= Qt.Key_9) {
                const n = event.key - Qt.Key_1
                // Act on the visible (active) list so the number matches the chip.
                // Skip a recent tag already on the photo (shown disabled), but still
                // swallow the chord so the digit never lands in the text field.
                if (n < root.visibleRecent.length
                        && root.appliedLower.indexOf(root.visibleRecent[n].toLowerCase()) === -1)
                    root.applyTagRequested(root.visibleRecent[n])
                break
            }
            return // not ours: let the text input have it
        }
        event.accepted = true
    }

    // A flat button that never takes focus from the field.
    component TriageButton: Button {
        focusPolicy: Qt.NoFocus
        Layout.fillWidth: true
    }
    // A chip for a tag, colored by its category like the detail panel's chips.
    component TagChip: Rectangle {
        id: chip
        property string name: ""
        property int colorSlot: -1
        property string categoryName: ""
        property alias content: chipRow.data
        readonly property bool categorized: colorSlot >= 0
        readonly property color tint: root.categoryColor(colorSlot)
        radius: height / 2
        height: 24
        width: chipRow.implicitWidth + 12
        // Categorized tags carry their category's color; uncategorized ones keep a
        // neutral grey border, matching the filter sidebar and the tag-info popover.
        color: categorized ? Qt.alpha(tint, 0.18) : sys.alternateBase
        border.color: categorized ? tint : sys.mid
        HoverHandler { id: chipHover }
        ToolTip.visible: chipHover.hovered && chip.categorized
        ToolTip.delay: 500
        ToolTip.text: chip.categoryName
        RowLayout {
            id: chipRow
            anchors.centerIn: parent
            spacing: 2
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 10

        // --- Add a tag -----------------------------------------------------------
        TagAutoField {
            id: tagField
            Layout.fillWidth: true
            placeholderText: root.photoId >= 0 ? qsTr("Add a tag…  (Enter on empty: next)")
                                               : qsTr("Enter or Esc to finish")
            tags: root.suggestionTags
            aliases: root.aliasList
            implications: root.implicationList
            excludeNames: root.appliedLower
            categoryColorFn: root.categoryColor
            commitOnPick: true
            // Enter: a highlighted suggestion was picked, or a name was typed →
            // apply and stay; an empty field → advance.
            onAccepted: {
                const name = text.trim()
                if (name.length > 0) {
                    root.applyTagRequested(name)
                    clear()
                } else {
                    root.nextRequested()
                }
            }
            onCommandKeyPressed: (event) => root.handleCommandKey(event)
            onEscapePressed: root.exitRequested()
        }

        // --- On this photo --------------------------------------------------------
        Label {
            text: qsTr("On this photo")
            font.pixelSize: 11
            color: sys.placeholderText
        }
        // Sized to its chips, up to a share of the sidebar, then scrolls — so the
        // recent tags and the buttons stay near the field rather than at the foot.
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.max(48, Math.min(chipFlow.implicitHeight + 4, root.height * 0.3))

            ScrollView {
                id: chipScroll
                anchors.fill: parent
                clip: true
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                Flow {
                    id: chipFlow
                    width: chipScroll.availableWidth
                    spacing: 4

                    Repeater {
                        model: root.chips
                        delegate: TagChip {
                            required property var modelData
                            name: modelData.name
                            colorSlot: modelData.colorSlot
                            categoryName: modelData.categoryName || ""
                            content: [
                                // Explicit foreground: these labels are reparented
                                // in via `content`, so they don't inherit the dark
                                // theme's text color the way an inline label would.
                                Label { text: modelData.name; font.pixelSize: 12; color: sys.text },
                                ToolButton {
                                    text: "✕"
                                    font.pixelSize: 11
                                    implicitWidth: 18
                                    implicitHeight: 18
                                    padding: 0
                                    focusPolicy: Qt.NoFocus
                                    ToolTip.visible: hovered
                                    ToolTip.text: qsTr("Remove “%1”").arg(modelData.name)
                                    onClicked: root.removeTagRequested(modelData.id, modelData.name)
                                }
                            ]
                        }
                    }

                    Label {
                        visible: root.chips.length === 0 && root.photoId >= 0
                        text: qsTr("No tags yet.")
                        color: sys.placeholderText
                    }
                }
            }
            WheelScroller { flickable: chipScroll.contentItem }
        }

        // --- Recent ---------------------------------------------------------------
        Label {
            text: qsTr("Recent  ·  Ctrl+1–9")
            font.pixelSize: 11
            color: sys.placeholderText
        }
        Flow {
            Layout.fillWidth: true
            spacing: 4

            Repeater {
                model: root.visibleRecent
                delegate: TagChip {
                    id: recentChip
                    required property int index
                    required property string modelData
                    // Look up the tag's category for the color (by name; recent
                    // tags are names, not ids).
                    readonly property var meta: {
                        const all = root.suggestionTags
                        const nf = TextMatch.fold(modelData)
                        for (let i = 0; i < all.length; ++i)
                            if (TextMatch.fold(all[i].name) === nf)
                                return all[i]
                        return null
                    }
                    // Already on the current photo → shown disabled: dimmed, no
                    // click, and its Ctrl+number does nothing (see handleCommandKey).
                    readonly property bool applied: root.appliedLower.indexOf(modelData.toLowerCase()) !== -1
                    name: modelData
                    colorSlot: meta ? meta.colorSlot : -1
                    categoryName: meta ? (meta.categoryName || "") : ""
                    opacity: applied ? 0.4 : 1.0
                    ToolTip.visible: recentHover.hovered
                    ToolTip.text: applied ? qsTr("“%1” is already on this photo").arg(modelData)
                                          : qsTr("Apply “%1”  (Ctrl+%2)").arg(modelData).arg(index + 1)
                    HoverHandler { id: recentHover }
                    content: [
                        Rectangle {
                            width: 16
                            height: 16
                            radius: 8
                            color: recentChip.categorized ? recentChip.tint : sys.mid
                            Label {
                                anchors.centerIn: parent
                                text: index + 1
                                font.pixelSize: 10
                                font.bold: true
                                color: omarchyTheme.textOn(parent.color)
                            }
                        },
                        Label { text: modelData; font.pixelSize: 12; rightPadding: 4; color: sys.text }
                    ]
                    TapHandler {
                        enabled: !recentChip.applied
                        onTapped: root.applyTagRequested(recentChip.modelData)
                    }
                }
            }
            Label {
                visible: root.visibleRecent.length === 0
                text: qsTr("Tags you apply show up here.")
                color: sys.placeholderText
                font.pixelSize: 12
            }
        }

        // --- Actions --------------------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            spacing: 6

            TriageButton {
                text: (root.favorite ? "★  " : "☆  ") + qsTr("Favorite")
                enabled: root.photoId >= 0
                ToolTip.visible: hovered
                ToolTip.text: (root.favorite ? qsTr("Remove from Favorites") : qsTr("Add to Favorites")) + "  (Ctrl+Space)"
                onClicked: root.favoriteToggleRequested()
            }
            TriageButton {
                text: qsTr("Trash")
                enabled: root.photoId >= 0
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Move to the Trash and drop from the library  (Ctrl+Delete)")
                onClicked: root.trashRequested()
            }
            TriageButton {
                text: qsTr("Undo")
                enabled: root.session ? root.session.undoCount > 0 : false
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Undo the last tag, favorite or trash  (Ctrl+Z)")
                onClicked: root.undoRequested()
            }
        }

        Item { Layout.fillHeight: true } // the legend sits at the foot

        MenuSeparator { Layout.fillWidth: true }

        // --- Key legend -----------------------------------------------------------
        GridLayout {
            Layout.fillWidth: true
            columns: 2
            columnSpacing: 12
            rowSpacing: 2

            component KeyLabel: Label {
                font.pixelSize: 11
                font.family: "monospace"
                color: sys.placeholderText
            }
            component KeyDesc: Label {
                Layout.fillWidth: true
                font.pixelSize: 11
                color: sys.placeholderText
                elide: Text.ElideRight
            }

            KeyLabel { text: "Enter" }       KeyDesc { text: qsTr("Apply tag · next when empty") }
            KeyLabel { text: "Ctrl+→ / ←" }  KeyDesc { text: qsTr("Next / previous") }
            KeyLabel { text: "Ctrl+Space" }  KeyDesc { text: qsTr("Toggle favorite") }
            KeyLabel { text: "Ctrl+Delete" } KeyDesc { text: qsTr("Move to Trash") }
            KeyLabel { text: "Ctrl+1–9" }    KeyDesc { text: qsTr("Apply a recent tag") }
            KeyLabel { text: "Ctrl+Z" }      KeyDesc { text: qsTr("Undo") }
            KeyLabel { text: "Esc" }         KeyDesc { text: qsTr("Exit triage") }
        }
    }
}
