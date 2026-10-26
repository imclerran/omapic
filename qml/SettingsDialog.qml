// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

// The app's single settings dialog, tabbed so new sections can be added without
// spawning more dialogs. "General" holds gallery and tag preferences, "Library"
// manages the folder set, and "Slideshow" holds playback options.
Dialog {
    id: root

    property var library
    property color accent: "#4a90d9"
    // The two Settings objects (owned by Main), so toggles persist.
    property var gallerySettings
    property var appSettings
    // Which tab to show when opened (0 = General, 1 = Library, 2 = Slideshow).
    property int initialTab: 0

    // Display names for the thumbnail-size slider steps (matches Main's thumbSizes).
    readonly property var thumbNames: [qsTr("Extra small"), qsTr("Small"),
                                       qsTr("Medium"), qsTr("Large"), qsTr("Extra large")]

    title: qsTr("Settings")
    modal: true
    anchors.centerIn: parent
    width: Math.min(640, (parent ? parent.width : 640) - 80)
    height: Math.min(600, (parent ? parent.height : 600) - 80)
    standardButtons: Dialog.Close

    onAboutToShow: tabs.currentIndex = root.initialTab

    // Folder the user is about to remove; -1 when no confirmation is pending.
    property int pendingId: -1
    property string pendingPath: ""

    // Keep the folder list from jumping to the top when it refreshes.
    property real savedScrollY: 0
    property bool restoreScroll: false
    function rememberScroll() {
        savedScrollY = folderList.contentY
        restoreScroll = true
    }

    Connections {
        target: root.library
        function onFoldersChanged() {
            if (!root.restoreScroll)
                return
            root.restoreScroll = false
            folderList.contentY = root.savedScrollY
            Qt.callLater(function () { folderList.contentY = root.savedScrollY })
        }
    }

    FolderDialog {
        id: addDialog
        title: qsTr("Choose a folder to add")
        currentFolder: StandardPaths.writableLocation(StandardPaths.HomeLocation)
        onAccepted: { root.rememberScroll(); root.library.importDirectory(selectedFolder) }
    }

    Dialog {
        id: confirmDialog
        title: qsTr("Remove folder?")
        modal: true
        anchors.centerIn: parent
        width: Math.min(460, root.width - 40)
        standardButtons: Dialog.Yes | Dialog.No

        onAccepted: {
            root.rememberScroll()
            root.library.removeFolder(root.pendingId)
            root.pendingId = -1
            root.pendingPath = ""
        }
        onRejected: {
            root.pendingId = -1
            root.pendingPath = ""
        }

        Label {
            width: parent.width
            wrapMode: Text.WordWrap
            text: qsTr("Remove “%1” from the library?\n\nIts photos will be removed from the gallery. Their tags are remembered by content, so re-adding the folder restores them. The files on disk are not touched.")
                  .arg(root.pendingPath)
        }
    }

    Dialog {
        id: confirmOrphans
        title: qsTr("Remove loose photos?")
        modal: true
        anchors.centerIn: parent
        width: Math.min(460, root.width - 40)
        standardButtons: Dialog.Yes | Dialog.No

        onAccepted: root.library.removeOrphanPhotos()

        Label {
            width: parent.width
            wrapMode: Text.WordWrap
            text: qsTr("Remove %1 photo(s) that aren't in any folder?\n\nTheir tags are remembered by content and will be restored if you add a folder that contains them again. The files on disk are not touched.")
                  .arg(root.library.orphanPhotoCount)
        }
    }

    Dialog {
        id: forgetConfirm
        title: qsTr("Forget remembered tags?")
        modal: true
        anchors.centerIn: parent
        width: Math.min(460, root.width - 40)
        standardButtons: Dialog.Yes | Dialog.No

        onAccepted: root.library.forgetRemovedTags()

        Label {
            width: parent.width
            wrapMode: Text.WordWrap
            text: qsTr("Forget the tags remembered for %n removed file(s)?\n\nThis can't be undone unless those files are added again with byte-identical content.", "", root.library.rememberedRemovedCount)
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 12

        TabBar {
            id: tabs
            Layout.fillWidth: true
            TabButton { text: qsTr("General") }
            TabButton { text: qsTr("Library") }
            TabButton { text: qsTr("Slideshow") }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: tabs.currentIndex

            // ---- General tab ----
            ColumnLayout {
                spacing: 10

                Label {
                    text: qsTr("Gallery")
                    font.bold: true
                }

                Switch {
                    text: qsTr("Sort by folder in flat view")
                    checked: root.gallerySettings ? root.gallerySettings.folderFirst : false
                    onToggled: if (root.gallerySettings) root.gallerySettings.folderFirst = checked
                }

                Switch {
                    text: qsTr("Crop thumbnails to square")
                    checked: root.gallerySettings ? root.gallerySettings.cropThumbs : true
                    onToggled: if (root.gallerySettings) root.gallerySettings.cropThumbs = checked
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Off: fit the whole image, letterboxed to its aspect ratio")
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12
                    Label {
                        text: qsTr("Thumbnail size")
                        Layout.fillWidth: true
                    }
                    Label {
                        text: root.gallerySettings
                              ? root.thumbNames[Math.max(0, Math.min(root.thumbNames.length - 1,
                                                                     root.gallerySettings.thumbIndex))]
                              : ""
                        color: palette.placeholderText
                        font.pixelSize: 12
                    }
                    Slider {
                        Layout.preferredWidth: 160
                        from: 0
                        to: root.thumbNames.length - 1
                        stepSize: 1
                        snapMode: Slider.SnapAlways
                        value: root.gallerySettings ? root.gallerySettings.thumbIndex : 2
                        onMoved: if (root.gallerySettings) root.gallerySettings.thumbIndex = Math.round(value)
                    }
                }

                Label {
                    text: qsTr("Tags")
                    font.bold: true
                    Layout.topMargin: 8
                }

                Switch {
                    text: qsTr("Show sample photos in tag suggestions")
                    checked: root.gallerySettings ? root.gallerySettings.tagPopoverThumbs : false
                    onToggled: if (root.gallerySettings) root.gallerySettings.tagPopoverThumbs = checked
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Add two thumbnails of photos carrying the highlighted tag to its info panel: the most-tagged (favorites get a head start), then one adding tags the first lacks")
                }

                Switch {
                    text: qsTr("Write tags to XMP sidecars automatically")
                    checked: root.gallerySettings ? root.gallerySettings.writeSidecars : false
                    onToggled: if (root.gallerySettings) root.gallerySettings.writeSidecars = checked
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Keep a <photo>.xmp next to each file in sync so other apps can read its tags")
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Button {
                        text: qsTr("Export tags now")
                        enabled: root.library.folders.length > 0
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Write every photo's tags to an XMP sidecar now")
                        onClicked: root.library.exportAllSidecars()
                    }
                    Item { Layout.fillWidth: true }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12
                    Layout.topMargin: 4
                    Label {
                        text: qsTr("Remember tags for removed files")
                        Layout.fillWidth: true
                    }
                    ComboBox {
                        id: retentionCombo
                        textRole: "label"
                        // Each entry's `days`: 0 = forever.
                        model: [
                            { label: qsTr("Forever"), days: 0 },
                            { label: qsTr("1 year"), days: 365 },
                            { label: qsTr("6 months"), days: 182 },
                            { label: qsTr("30 days"), days: 30 }
                        ]
                        function indexForDays(d) {
                            for (let i = 0; i < model.length; ++i)
                                if (model[i].days === d)
                                    return i
                            return 0
                        }
                        Component.onCompleted: currentIndex =
                            indexForDays(root.gallerySettings ? root.gallerySettings.tagRetentionDays : 0)
                        onActivated: if (root.gallerySettings)
                            root.gallerySettings.tagRetentionDays = model[currentIndex].days
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        font.pixelSize: 12
                        color: palette.placeholderText
                        text: root.library.rememberedRemovedCount > 0
                              ? qsTr("Tags remembered for %n removed file(s). Recovery needs byte-identical files.", "", root.library.rememberedRemovedCount)
                              : qsTr("No removed files have remembered tags.")
                    }
                    Button {
                        text: qsTr("Forget now…")
                        enabled: root.library.rememberedRemovedCount > 0
                        onClicked: forgetConfirm.open()
                    }
                }

                // Keep everything top-aligned.
                Item { Layout.fillHeight: true }
            }

            // ---- Library tab ----
            ColumnLayout {
                spacing: 12

                Frame {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumHeight: 240
                    padding: 0

                    ListView {
                        id: folderList
                        anchors.fill: parent
                        clip: true
                        model: root.library.folders
                        boundsBehavior: Flickable.StopAtBounds
                        ScrollBar.vertical: ScrollBar {}

                        delegate: ItemDelegate {
                            id: row
                            width: ListView.view.width
                            hoverEnabled: true
                            highlighted: false

                            required property var modelData

                            contentItem: RowLayout {
                                spacing: 12

                                CheckBox {
                                    checked: row.modelData.enabled
                                    ToolTip.visible: hovered
                                    ToolTip.text: checked ? qsTr("Shown in library — click to hide")
                                                          : qsTr("Hidden from library — click to show")
                                    onToggled: { root.rememberScroll(); root.library.setFolderEnabled(row.modelData.id, checked) }
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 2
                                    opacity: row.modelData.enabled ? 1.0 : 0.5
                                    Label {
                                        Layout.fillWidth: true
                                        text: row.modelData.path
                                        elide: Text.ElideMiddle
                                        font.pixelSize: 14
                                    }
                                    Label {
                                        text: {
                                            const n = row.modelData.count
                                            const base = n === 1 ? qsTr("1 photo") : qsTr("%1 photos").arg(n)
                                            return row.modelData.enabled ? base : base + qsTr(" · hidden")
                                        }
                                        font.pixelSize: 12
                                        color: palette.placeholderText
                                    }
                                }

                                Button {
                                    text: qsTr("Remove")
                                    onClicked: {
                                        root.pendingId = row.modelData.id
                                        root.pendingPath = row.modelData.path
                                        confirmDialog.open()
                                    }
                                }
                            }
                        }

                        Label {
                            anchors.centerIn: parent
                            width: parent.width - 32
                            visible: folderList.count === 0
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.WordWrap
                            text: qsTr("No folders yet. Add one to build your library.")
                            color: palette.placeholderText
                        }

                        WheelScroller { flickable: folderList }
                    }
                }

                // Photos not under any registered folder.
                Frame {
                    Layout.fillWidth: true
                    visible: root.library.orphanPhotoCount > 0

                    RowLayout {
                        anchors.fill: parent
                        spacing: 12

                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            text: qsTr("%1 photo(s) aren't in any folder.")
                                  .arg(root.library.orphanPhotoCount)
                        }
                        Button {
                            text: qsTr("Remove")
                            onClicked: confirmOrphans.open()
                        }
                    }
                }

                MenuSeparator { Layout.fillWidth: true }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    Button {
                        text: qsTr("Add Folder…")
                        onClicked: addDialog.open()
                    }
                    Button {
                        text: qsTr("Rescan")
                        enabled: folderList.count > 0
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Pick up new, moved, and deleted files on disk")
                        onClicked: { root.rememberScroll(); root.library.rescan() }
                    }
                    Item { Layout.fillWidth: true }
                }
            }

            // ---- Slideshow tab ----
            ColumnLayout {
                spacing: 16

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 16
                    Label {
                        text: qsTr("Seconds per photo")
                        Layout.fillWidth: true
                    }
                    SpinBox {
                        id: intervalSpin
                        // Values are in tenths of a second, so the interval is
                        // adjustable down to 0.1 s (0.1 s … 120.0 s).
                        from: 1
                        to: 1200
                        editable: true
                        value: Math.round(root.appSettings.intervalMs / 100)
                        onValueModified: root.appSettings.intervalMs = value * 100

                        stepSize: intervalSpin.down.pressed ? (value > 10 ? 10 : 1)
                                                            : (value >= 10 ? 10 : 1)

                        textFromValue: function (value, locale) {
                            return Number(value / 10).toLocaleString(locale, 'f', 1)
                        }
                        valueFromText: function (text, locale) {
                            return Math.round(Number.fromLocaleString(locale, text) * 10)
                        }
                        validator: DoubleValidator {
                            bottom: intervalSpin.from / 10
                            top: intervalSpin.to / 10
                            decimals: 1
                            notation: DoubleValidator.StandardNotation
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 16
                    Label {
                        text: qsTr("Shuffle order")
                        Layout.fillWidth: true
                    }
                    Switch {
                        checked: root.appSettings.shuffle
                        onToggled: root.appSettings.shuffle = checked
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 16
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Label {
                            text: qsTr("Shuffle folders")
                        }
                        Label {
                            text: qsTr("Play enabled folders in random order, each folder's photos together")
                            font.pixelSize: 12
                            color: palette.placeholderText
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }
                    }
                    Switch {
                        checked: root.appSettings.shuffleFolders
                        onToggled: root.appSettings.shuffleFolders = checked
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 16
                    Label {
                        text: qsTr("Crossfade between photos")
                        Layout.fillWidth: true
                    }
                    Switch {
                        checked: root.appSettings.fade
                        onToggled: root.appSettings.fade = checked
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 16
                    Label {
                        text: qsTr("Start from a random photo")
                        Layout.fillWidth: true
                    }
                    Switch {
                        checked: root.appSettings.randomStart
                        onToggled: root.appSettings.randomStart = checked
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 16
                    Label {
                        text: qsTr("Loop videos")
                        Layout.fillWidth: true
                    }
                    Switch {
                        checked: root.appSettings.loop
                        onToggled: root.appSettings.loop = checked
                    }
                }

                // Push the options to the top; the tab is shorter than the others.
                Item { Layout.fillHeight: true }
            }
        }
    }
}
