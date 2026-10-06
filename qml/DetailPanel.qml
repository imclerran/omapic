// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root

    property var library
    property var selectedIds: []
    property color accent: "#4a90d9"

    readonly property int count: selectedIds.length

    // single-mode photo info; `chips` unifies the tag list for both modes.
    property var info: ({})
    property var chips: []
    property var appliedLower: []

    SystemPalette { id: sys }

    color: sys.window

    function refresh() {
        const n = selectedIds.length
        if (n === 1) {
            info = library.photoInfo(selectedIds[0])
            const ts = info.tags || []
            chips = ts.map(function (t) { return { id: t.id, name: t.name, label: t.name } })
            appliedLower = ts.map(function (t) { return t.name.toLowerCase() })
        } else if (n > 1) {
            info = ({})
            const ct = library.commonTags(selectedIds)
            chips = ct.map(function (t) {
                return { id: t.id, name: t.name, label: t.name + "  (" + t.count + "/" + n + ")" }
            })
            // Only hide tags already on every selected photo.
            appliedLower = ct.filter(function (t) { return t.count === n })
                             .map(function (t) { return t.name.toLowerCase() })
        } else {
            info = ({})
            chips = []
            appliedLower = []
        }
    }

    function commitTag(name) {
        if (!name || name.trim().length === 0)
            return
        if (selectedIds.length === 1)
            library.addTag(selectedIds[0], name)
        else if (selectedIds.length > 1)
            library.addTagToPhotos(selectedIds, name)
    }

    function removeTag(tagId) {
        if (selectedIds.length === 1)
            library.removeTag(selectedIds[0], tagId)
        else if (selectedIds.length > 1)
            library.removeTagFromPhotos(selectedIds, tagId)
    }

    onSelectedIdsChanged: refresh()

    Connections {
        target: root.library
        function onPhotoChanged(id) {
            if (root.selectedIds.indexOf(id) !== -1)
                root.refresh()
        }
        // A tag deleted from the manager can affect any selected photo's chips.
        function onTagsChanged() { root.refresh() }
    }

    // Empty state.
    Label {
        anchors.centerIn: parent
        width: parent.width - 24
        visible: root.count === 0
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        text: qsTr("Select a photo to see its details.\nCtrl+click and Shift+click to select several.")
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

        // File details (single selection only).
        Label {
            visible: root.count === 1
            text: qsTr("File")
            font.pixelSize: 11
            color: palette.placeholderText
        }
        Label {
            visible: root.count === 1
            Layout.fillWidth: true
            text: root.info.fileName || ""
            font.bold: true
            wrapMode: Text.WrapAnywhere
            maximumLineCount: 2
            elide: Text.ElideMiddle
        }
        Label {
            visible: root.count === 1
            Layout.fillWidth: true
            text: root.info.path || ""
            font.pixelSize: 11
            color: palette.placeholderText
            wrapMode: Text.WrapAnywhere
            maximumLineCount: 2
            elide: Text.ElideMiddle
        }

        MenuSeparator { Layout.fillWidth: true }

        Label {
            text: root.count === 1 ? qsTr("Tags") : qsTr("Tags across selection")
            font.pixelSize: 11
            color: palette.placeholderText
        }

        Flow {
            Layout.fillWidth: true
            spacing: 4

            Repeater {
                model: root.chips
                delegate: Rectangle {
                    required property var modelData
                    radius: height / 2
                    height: 24
                    width: chipRow.implicitWidth + 12
                    color: sys.alternateBase
                    border.color: root.accent

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

        Item { Layout.fillHeight: true }

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
                    const q = text.trim().toLowerCase()
                    highlighted = -1
                    if (q.length === 0) {
                        suggestions = []
                        suggestionPopup.close()
                        return
                    }
                    const applied = root.appliedLower
                    const all = root.library.allTagNames()
                    const matches = []
                    for (var i = 0; i < all.length && matches.length < 8; ++i) {
                        const ln = all[i].toLowerCase()
                        if (ln.indexOf(q) !== -1 && ln !== q && applied.indexOf(ln) === -1)
                            matches.push(all[i])
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
                        commit(suggestions[highlighted])
                    else
                        commit(text)
                }
                Keys.onDownPressed: if (suggestions.length > 0)
                                        highlighted = Math.min(highlighted + 1, suggestions.length - 1)
                Keys.onUpPressed: highlighted = Math.max(highlighted - 1, -1)
                Keys.onEscapePressed: suggestionPopup.close()

                Popup {
                    id: suggestionPopup
                    y: tagInput.height
                    x: 0
                    width: tagInput.width
                    padding: 1
                    closePolicy: Popup.CloseOnPressOutsideParent | Popup.CloseOnEscape

                    contentItem: ListView {
                        implicitHeight: Math.min(tagInput.suggestions.length, 8) * 32
                        model: tagInput.suggestions
                        clip: true
                        currentIndex: tagInput.highlighted

                        delegate: ItemDelegate {
                            required property int index
                            required property var modelData
                            width: ListView.view.width
                            height: 32
                            text: modelData
                            highlighted: index === tagInput.highlighted
                            onClicked: tagInput.commit(modelData)
                        }
                    }
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
