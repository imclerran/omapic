// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QSet>
#include <QString>
#include <QStringList>

// Shared, Library-free media enumeration. Both the library scanner
// (libraryworker.cpp) and the standalone viewer use this so the set of
// recognised extensions and the .nomedia exclusion behave identically.
namespace mediascan {

// Image and video globs the scanner picks up. Keep the video set in sync with
// Photo::isVideo (database.cpp), the thumbnail provider, and SlideshowView.qml.
const QStringList &globs();

// True when `path` (by extension) is one of the media types in globs().
bool isMediaFile(const QString &path);

// Collect every media file under `root`, skipping folders (and their subtrees)
// marked with a .nomedia file. Results are added to `out` (so several roots can
// be unioned). Recurses into subdirectories.
void collect(const QString &root, QSet<QString> &out);

// Convenience: every media file under `root` (.nomedia-aware), case-insensitively
// sorted by path — the order the viewer/slideshow plays them in.
QStringList collectSorted(const QString &root);

} // namespace mediascan
