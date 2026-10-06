// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root

    property var tagModel
    property color accent: "#4a90d9"

    // How many chips match the current search text (all of them when empty).
    function matchCount(q) {
        const s = q.trim().toLowerCase()
        if (s === "")
            return rep.count
        let c = 0
        for (let i = 0; i < rep.count; ++i) {
            const it = rep.itemAt(i)
            if (it && it.name.toLowerCase().indexOf(s) !== -1)
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

                            // Hide (and drop from the flow) chips that don't match
                            // the search text.
                            visible: {
                                const q = searchField.text.trim().toLowerCase()
                                return q === "" || chip.name.toLowerCase().indexOf(q) !== -1
                            }

                            implicitHeight: 24
                            implicitWidth: chipLabel.implicitWidth + 16
                            radius: height / 2
                            color: chip.selected ? root.accent : sys.alternateBase
                            border.width: 1
                            border.color: chip.selected ? root.accent : sys.mid

                            Label {
                                id: chipLabel
                                anchors.centerIn: parent
                                text: chip.name + "  " + chip.count
                                font.pixelSize: 12
                                color: chip.selected ? palette.highlightedText : palette.text
                            }

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.tagModel.toggle(chip.index)
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
                      ? qsTr("No tags yet. Select a photo and add some below.")
                      : qsTr("No tags match “%1”.").arg(searchField.text.trim())
                color: palette.placeholderText
            }
        }
    }
}
