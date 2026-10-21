// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Omapic // TextMatch singleton (accent-/case-insensitive folding)

Rectangle {
    id: root

    property var tagModel
    property color accent: "#4a90d9"
    // Tag aliases ({aliasName, tagId, tagName, …}), so the filter matches a tag by
    // its aliases too — typing an alias reveals its canonical chip. Bound from Main.
    property var aliases: []

    // True when `foldedQuery` matches any alias of the tag named `tagName`.
    function aliasMatches(tagName, foldedQuery) {
        const al = root.aliases
        const tn = TextMatch.fold(tagName)
        for (let i = 0; i < al.length; ++i) {
            if (TextMatch.fold(al[i].tagName) === tn
                    && TextMatch.fold(al[i].aliasName).indexOf(foldedQuery) !== -1)
                return true
        }
        return false
    }
    // Exposed so Main can drive the filter / faceting from these controls.
    property alias matchAll: matchAllSwitch.checked
    property int tagPresence: 0 // 0 all, 1 tagged only, 2 untagged only

    // Tab focus cycle (routed by Main): forward from / back to the filter field.
    signal tabForward()
    signal tabBackward()
    // Focus the tag-filter field. Returns false when it's hidden (no tags), so
    // the cycle can skip this stop.
    function focusSearch() {
        if (!searchField.visible)
            return false
        searchField.forceActiveFocus()
        return true
    }

    // Color for a tag's category palette slot; the accent for uncategorized tags.
    // Reads the palette property (not just the invokable) so chips re-color when
    // the Omarchy theme changes.
    function categoryColor(slot) {
        const pal = omarchyTheme.categoryPalette
        if (slot < 0 || pal.length === 0)
            return root.accent
        return pal[slot % pal.length]
    }

    // How many chips match the current search text (all of them when empty).
    function matchCount(q) {
        const s = TextMatch.fold(q.trim())
        if (s === "")
            return rep.count
        let c = 0
        for (let i = 0; i < rep.count; ++i) {
            const it = rep.itemAt(i)
            if (it && (TextMatch.fold(it.name).indexOf(s) !== -1 || root.aliasMatches(it.name, s)))
                ++c
        }
        return c
    }

    SystemPalette { id: sys }

    color: sys.window

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 12

            Label {
                text: qsTr("Filter by tags")
                font.bold: true
                font.pixelSize: 15
                Layout.fillWidth: true
            }
            ToolButton {
                text: qsTr("Clear")
                enabled: root.tagModel.selectedTagIds.length > 0
                onClicked: root.tagModel.clearSelection()
            }
        }

        TextField {
            id: searchField
            Layout.fillWidth: true
            Layout.leftMargin: 12
            Layout.rightMargin: 12
            Layout.bottomMargin: 8
            visible: rep.count > 0
            placeholderText: qsTr("Search tags…")
            Keys.onEscapePressed: text = ""
            Keys.onTabPressed: (event) => { root.tabForward(); event.accepted = true }
            Keys.onBacktabPressed: (event) => { root.tabBackward(); event.accepted = true }
            ClearButton { field: searchField }
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
                    padding: 12
                    spacing: 4

                    Repeater {
                        id: rep
                        model: root.tagModel

                        delegate: Rectangle {
                            id: chip
                            required property int index
                            required property string name
                            required property int count
                            required property bool selected
                            required property int colorSlot
                            required property string categoryName

                            // Hide (and drop from the flow) chips that don't match
                            // the search text.
                            visible: {
                                const q = TextMatch.fold(searchField.text.trim())
                                return q === "" || TextMatch.fold(chip.name).indexOf(q) !== -1
                                       || root.aliasMatches(chip.name, q)
                            }

                            // Categorized tags take their category's color on the
                            // border, a light tint when idle and a solid fill when
                            // selected. Uncategorized tags keep a neutral grey border
                            // and, when selected, a matching grey fill — so selection
                            // never borrows one of the category palette colors.
                            readonly property bool categorized: chip.colorSlot >= 0
                            readonly property color tint: root.categoryColor(chip.colorSlot)
                            readonly property color selFill: chip.categorized ? chip.tint : sys.mid

                            implicitHeight: 24
                            implicitWidth: chipLabel.implicitWidth + 16
                            radius: height / 2
                            color: chip.selected ? chip.selFill
                                 : chip.categorized ? Qt.alpha(chip.tint, 0.18)
                                 : sys.alternateBase
                            border.width: 1
                            border.color: chip.categorized ? chip.tint : sys.mid

                            Label {
                                id: chipLabel
                                anchors.centerIn: parent
                                text: chip.name + "  " + chip.count
                                font.pixelSize: 12
                                color: chip.selected ? omarchyTheme.textOn(chip.selFill) : palette.text
                            }

                            MouseArea {
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.tagModel.toggle(chip.index)
                                // Color alone shouldn't carry the meaning; name the
                                // category on hover.
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
                      ? qsTr("No tags yet. Select a photo and add some above.")
                      : qsTr("No tags match “%1”.").arg(searchField.text.trim())
                color: palette.placeholderText
            }

            WheelScroller { flickable: scroll.contentItem }
        }

        MenuSeparator { Layout.fillWidth: true }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 8
            Layout.rightMargin: 12
            Layout.bottomMargin: 4
            spacing: 8

            Switch {
                id: matchAllSwitch
                text: qsTr("Match all tags")
                checked: true
            }
            Item { Layout.fillWidth: true }
            Button {
                text: root.tagPresence === 0 ? qsTr("All")
                    : root.tagPresence === 1 ? qsTr("Tagged")
                    : qsTr("Untagged")
                highlighted: root.tagPresence !== 0
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Cycle: all photos → tagged only → untagged only")
                onClicked: root.tagPresence = (root.tagPresence + 1) % 3
            }
        }
    }
}
