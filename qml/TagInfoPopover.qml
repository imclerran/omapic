// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Layouts

import Omapic // TextMatch singleton (accent-/case-insensitive folding)

// A small informational panel shown beside a tag autocomplete dropdown while a
// row is highlighted, surfacing detail that doesn't fit in the row itself: the
// full (un-elided) tag name, the aliases that resolve to it, and the tags that
// applying it will also add (its implication closure). Purely informational — it
// never takes focus or closes the dropdown.
//
// The host sets `tagName` to the highlighted entry, passes the cached alias and
// implication lists, and gates `active` on the dropdown being open with a valid
// highlight; it also reports `nameTruncated` for the highlighted row. The panel
// shows itself only when there is something extra to say.
Popup {
    id: root

    property string tagName: ""
    property var aliases: []            // [{aliasName, tagName, …}] — the full alias list
    property var implications: []       // [{antecedentName, consequentName, consequentColorSlot, …}]
    property var categoryColorFn: null  // function(colorSlot) -> color, for the implied-tag chips
    property bool active: false         // dropdown open with a valid highlight
    property bool nameTruncated: false  // the highlighted row's name is elided

    // The host's desired top (relative to `parent`, e.g. just below the dropdown).
    // The actual y is clamped so the panel isn't clipped by the window's bottom:
    // when it would overrun, it shifts up (never above the top).
    //
    // mapToItem() isn't reactive, so the binding can't see the parent move or the
    // layout settle on its own. `_repoTick` forces a recompute after the panel
    // opens (deferred, once layout has settled) and whenever its height changes —
    // otherwise a first open before layout settles reads stale coordinates and the
    // clamp is wrong until something reactive (e.g. a window resize) re-triggers it.
    property real preferredY: 0
    property int _repoTick: 0
    onOpenedChanged: if (opened) Qt.callLater(function() { root._repoTick++ })
    onHeightChanged: root._repoTick++
    y: {
        const _ = root._repoTick // dependency: re-run after open / content settles
        const ov = Overlay.overlay
        if (!root.parent || !ov)
            return root.preferredY
        const parentTop = root.parent.mapToItem(ov, 0, 0).y
        const margin = 8
        const maxY = ov.height - margin - root.height - parentTop
        const minY = margin - parentTop
        return Math.max(minY, Math.min(root.preferredY, maxY))
    }

    // Aliases that point at this tag (so the user sees its alternate names).
    readonly property var aliasNames: {
        const out = []
        const nf = TextMatch.fold(root.tagName)
        if (nf === "")
            return out
        for (let i = 0; i < root.aliases.length; ++i) {
            if (TextMatch.fold(root.aliases[i].tagName) === nf)
                out.push(root.aliases[i].aliasName)
        }
        return out
    }

    // The tags applying this one will also add: the transitive consequent closure
    // over the implication edges, as [{name, colorSlot}].
    readonly property var impliedTags: {
        const out = []
        const start = TextMatch.fold(root.tagName)
        if (start === "")
            return out
        const edges = root.implications
        const visited = ({})
        visited[start] = true
        let frontier = [start]
        while (frontier.length > 0) {
            const next = []
            for (let f = 0; f < frontier.length; ++f) {
                for (let i = 0; i < edges.length; ++i) {
                    if (TextMatch.fold(edges[i].antecedentName) !== frontier[f])
                        continue
                    const cf = TextMatch.fold(edges[i].consequentName)
                    if (visited[cf])
                        continue
                    visited[cf] = true
                    next.push(cf)
                    out.push({ name: edges[i].consequentName,
                               colorSlot: edges[i].consequentColorSlot,
                               categoryName: edges[i].consequentCategoryName || "" })
                }
            }
            frontier = next
        }
        return out
    }

    // The implied tags grouped by category, as [{category, tags: [...]}], with
    // categories alphabetical and the uncategorized group (if any) last.
    readonly property var impliedGroups: {
        const byCat = ({})
        const order = []
        for (let i = 0; i < root.impliedTags.length; ++i) {
            const t = root.impliedTags[i]
            const cat = t.categoryName || ""
            if (!(cat in byCat)) {
                byCat[cat] = []
                order.push(cat)
            }
            byCat[cat].push(t)
        }
        order.sort(function (a, b) {
            if (a === "") return 1   // uncategorized last
            if (b === "") return -1
            return a.localeCompare(b)
        })
        const out = []
        for (let i = 0; i < order.length; ++i)
            out.push({ category: order[i], tags: byCat[order[i]] })
        return out
    }

    readonly property bool hasExtra: root.aliasNames.length > 0 || root.impliedTags.length > 0

    // Show only when the dropdown is up, a row is highlighted, and there is extra
    // detail to surface (an elided name, aliases, or implications). Driven
    // imperatively with open()/close() — the sibling suggestion popup does the
    // same, and a bound `visible` doesn't reliably open a Popup here.
    readonly property bool wantShown: root.active && root.tagName !== ""
                                      && (root.nameTruncated || root.hasExtra)
    onWantShownChanged: wantShown ? open() : close()

    padding: 10
    // Informational: never grab focus or close on interaction (the dropdown owns
    // those); it simply tracks the highlight.
    focus: false
    closePolicy: Popup.NoAutoClose

    function swatch(slot) {
        return (root.categoryColorFn && slot !== undefined && slot >= 0)
               ? root.categoryColorFn(slot) : "transparent"
    }

    SystemPalette { id: sysp }

    // Size the popup to its content explicitly — a Popup does not reliably take
    // the implicit size of a Layout contentItem, and a zero-size popup shows
    // nothing (which is why the sibling suggestion popup sets its size too).
    contentWidth: infoColumn.implicitWidth
    contentHeight: infoColumn.implicitHeight

    background: Rectangle {
        color: sysp.window
        border.color: sysp.mid
        border.width: 1
        radius: 4
        // A drop shadow so the panel reads clearly over the photos behind it.
        layer.enabled: true
        layer.effect: MultiEffect {
            shadowEnabled: true
            shadowColor: "#aa000000"
            shadowBlur: 0.5
            shadowVerticalOffset: 3
            autoPaddingEnabled: true
        }
    }

    contentItem: ColumnLayout {
        id: infoColumn
        spacing: 8

        // Full tag name (resolves an elided row).
        Label {
            Layout.maximumWidth: 260
            text: root.tagName
            font.bold: true
            wrapMode: Text.WrapAtWordBoundaryOrAnywhere
            color: sysp.text
        }

        // Alternate names that resolve to this tag, inline; the "AKA:" label keeps
        // the dim heading color, the names stay readable.
        RowLayout {
            visible: root.aliasNames.length > 0
            Layout.maximumWidth: 260
            spacing: 4
            Label {
                text: qsTr("AKA:")
                Layout.alignment: Qt.AlignTop
                font.pixelSize: 12
                color: sysp.placeholderText
            }
            Label {
                Layout.fillWidth: true
                text: root.aliasNames.join(", ")
                wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                font.pixelSize: 12
                color: sysp.text
            }
        }

        // Full-width divider before the implications (negative margins cancel the
        // popup padding so it runs edge to edge, at the border's weight and color).
        Rectangle {
            visible: root.impliedTags.length > 0
            Layout.fillWidth: true
            Layout.leftMargin: -root.padding
            Layout.rightMargin: -root.padding
            implicitHeight: 1
            color: sysp.mid
        }

        // Tags that applying this one will also add (implication closure), grouped
        // by category under a tinted sub-heading.
        ColumnLayout {
            visible: root.impliedTags.length > 0
            spacing: 6
            Repeater {
                model: root.impliedGroups
                delegate: ColumnLayout {
                    id: grp
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: 3

                    // Category name, tinted with the category's color (neutral for
                    // the uncategorized group).
                    Label {
                        readonly property int slot: grp.modelData.tags[0].colorSlot
                        text: grp.modelData.category !== "" ? grp.modelData.category
                                                            : qsTr("Uncategorized")
                        font.pixelSize: 10
                        font.capitalization: Font.AllUppercase
                        color: slot >= 0 ? root.swatch(slot) : sysp.placeholderText
                    }
                    Flow {
                        Layout.maximumWidth: 260
                        Layout.preferredWidth: 260
                        spacing: 4
                        Repeater {
                            model: grp.modelData.tags
                            delegate: Rectangle {
                                required property var modelData
                                readonly property bool categorized: modelData.colorSlot >= 0
                                readonly property color tint: root.swatch(modelData.colorSlot)
                                implicitHeight: 22
                                implicitWidth: chipText.implicitWidth + 14
                                radius: height / 2
                                color: categorized ? Qt.alpha(tint, 0.18) : sysp.alternateBase
                                border.width: 1
                                border.color: categorized ? tint : sysp.mid
                                Label {
                                    id: chipText
                                    anchors.centerIn: parent
                                    text: parent.modelData.name
                                    font.pixelSize: 11
                                    color: sysp.text
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
