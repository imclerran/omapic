// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Omapic // TextMatch singleton (accent-/case-insensitive folding)

// A tag chooser for defining a smart album: a search box + a flow of tag chips +
// an AND/OR switch, modeled on TagSidebar but with its OWN selection. It reads
// the shared TagModel read-only (never calls toggle()), so it never disturbs the
// live sidebar filter. Owns `selectedIds` (tag ids) and `matchAll`.
ColumnLayout {
    id: root

    property var tagModel
    property color accent: "#4a90d9"
    // The chosen tag ids (array of int) and the AND/OR mode. The dialog reads
    // these on accept and seeds them via setSelection() when editing.
    property var selectedIds: []
    property alias matchAll: matchSwitch.checked

    function isSelected(id) { return root.selectedIds.indexOf(id) !== -1 }
    function toggleId(id) {
        const next = root.selectedIds.slice()
        const i = next.indexOf(id)
        if (i === -1)
            next.push(id)
        else
            next.splice(i, 1)
        root.selectedIds = next
    }
    function setSelection(ids, all) {
        root.selectedIds = (ids || []).slice()
        matchSwitch.checked = all
    }
    function clear() { root.selectedIds = []; matchSwitch.checked = true }

    // Color for a tag's category palette slot; the accent for uncategorized tags.
    function categoryColor(slot) {
        const pal = omarchyTheme.categoryPalette
        if (slot < 0 || pal.length === 0)
            return root.accent
        return pal[slot % pal.length]
    }

    function matchCount(q) {
        const s = TextMatch.fold(q.trim())
        if (s === "")
            return rep.count
        let c = 0
        for (let i = 0; i < rep.count; ++i) {
            const it = rep.itemAt(i)
            if (it && TextMatch.fold(it.name).indexOf(s) !== -1)
                ++c
        }
        return c
    }

    SystemPalette { id: sys }
    spacing: 8

    TextField {
        id: searchField
        Layout.fillWidth: true
        placeholderText: qsTr("Search tags…")
        Keys.onEscapePressed: text = ""
    }

    Item {
        Layout.fillWidth: true
        Layout.fillHeight: true

        ScrollView {
            id: scroll
            anchors.fill: parent
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            Flow {
                width: scroll.availableWidth
                padding: 4
                spacing: 4

                Repeater {
                    id: rep
                    model: root.tagModel

                    delegate: Rectangle {
                        id: chip
                        required property int index
                        required property string name
                        required property int count
                        required property int colorSlot
                        required property string categoryName
                        // The tag id for this row. The model's "id" role can't be
                        // read as model.id (`id` is reserved in QML), so it comes
                        // via idAt(index) — chips are hidden, not removed, so the
                        // delegate index maps straight to the model row.
                        property int tagId: root.tagModel ? root.tagModel.idAt(index) : -1

                        // Re-evaluates whenever the owner's selection array changes.
                        readonly property bool chosen: root.selectedIds.indexOf(chip.tagId) !== -1

                        visible: {
                            const q = TextMatch.fold(searchField.text.trim())
                            return q === "" || TextMatch.fold(chip.name).indexOf(q) !== -1
                        }

                        readonly property bool categorized: chip.colorSlot >= 0
                        readonly property color tint: root.categoryColor(chip.colorSlot)
                        readonly property color selFill: chip.categorized ? chip.tint : sys.mid

                        implicitHeight: 26
                        implicitWidth: chipLabel.implicitWidth + 18
                        radius: height / 2
                        color: chip.chosen ? chip.selFill
                             : chip.categorized ? Qt.alpha(chip.tint, 0.18)
                             : sys.alternateBase
                        border.width: 1
                        border.color: chip.categorized ? chip.tint : sys.mid

                        Label {
                            id: chipLabel
                            anchors.centerIn: parent
                            text: chip.name + "  " + chip.count
                            font.pixelSize: 12
                            color: chip.chosen ? omarchyTheme.textOn(chip.selFill) : palette.text
                        }

                        MouseArea {
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.toggleId(chip.tagId)
                            ToolTip.visible: containsMouse && chip.categorized
                            ToolTip.delay: 500
                            ToolTip.text: chip.categoryName
                        }
                    }
                }
            }
        }

        Label {
            anchors.centerIn: parent
            width: parent.width - 24
            visible: rep.count === 0 || root.matchCount(searchField.text) === 0
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: rep.count === 0
                  ? qsTr("No tags yet.")
                  : qsTr("No tags match “%1”.").arg(searchField.text.trim())
            color: palette.placeholderText
        }

        WheelScroller { flickable: scroll.contentItem }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 8

        Switch {
            id: matchSwitch
            text: qsTr("Match all tags")
            checked: true
        }
        Label {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignRight
            text: matchSwitch.checked ? qsTr("A photo must have every selected tag")
                                      : qsTr("A photo may have any selected tag")
            font.pixelSize: 11
            color: palette.placeholderText
            elide: Text.ElideRight
        }
    }
}
