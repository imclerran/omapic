// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

// Shared, Library-free media enumeration. Both the library scanner
// (libraryworker.cpp) and the standalone viewer use this so the set of
// recognised extensions and the .nomedia exclusion behave identically.
namespace mediascan {

// File stats captured during the directory walk. The walk already stats every
// entry to apply the QDir::Files filter, so carrying mtime/size out costs no
// extra stat — and lets rescan detect an in-place edit without a second pass.
// Unix seconds for mtime; bytes for size.
struct FileEntry {
    qint64 mtime = 0;
    qint64 size = 0;
};

// Image and video globs the scanner picks up. Keep the video set in sync with
// Photo::isVideo (database.cpp), the thumbnail provider, and SlideshowView.qml.
const QStringList &globs();

// True when `path` (by extension) is one of the media types in globs().
bool isMediaFile(const QString &path);

// Collect every media file under `root` (mapped to its mtime/size), skipping
// folders (and their subtrees) marked with a .nomedia file. Entries are added to
// `out` (so several roots can be unioned). Recurses into subdirectories.
void collect(const QString &root, QHash<QString, FileEntry> &out);

// Convenience: every media file under `root` (.nomedia-aware), case-insensitively
// sorted by path — the order the viewer/slideshow plays them in.
QStringList collectSorted(const QString &root);

} // namespace mediascan
