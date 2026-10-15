// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Window
import Omapic

// Standalone viewer root for `omapic <file> […]`. No Library, no database — just
// the PhotoViewer over the launch files, with a SlideshowView it can hand off to.
// launchUrls / launchSinglePath are set as context properties by main.cpp.
PhotoViewer {
    id: viewer

    sources: launchUrls

    // Position the current photo within a (possibly reordered) playlist, then run.
    function startSlideshowAt(list, srcUrl) {
        const s = String(srcUrl)
        let pos = 0
        for (let i = 0; i < list.length; ++i) {
            if (String(list[i]) === s) { pos = i; break }
        }
        slideshow.sources = list
        slideshow.startAt(pos, true)
    }

    onSlideshowRequested: {
        // One launch path => slideshow the whole directory tree (enumerated
        // off-thread, .nomedia-aware); a list of paths => just those files.
        if (launchSinglePath && viewer.currentSource !== "")
            scanner.expandDirectoryTree(viewer.currentSource)
        else
            viewer.startSlideshowAt(viewer.sources, viewer.currentSource)
    }
    onCloseRequested: Qt.quit()

    MediaScanner {
        id: scanner
        onExpanded: (urls) => viewer.startSlideshowAt(urls, viewer.currentSource)
    }

    SlideshowView {
        id: slideshow
        // Theme actions need the Library, which the standalone viewer doesn't
        // have, so they stay unconnected here (the menu items are no-ops).
    }

    Component.onCompleted: {
        viewer.openAt(0)
        viewer.visibility = Window.Windowed
    }
}
