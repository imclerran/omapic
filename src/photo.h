// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QFileInfo>
#include <QSet>
#include <QString>
#include <QStringList>

// True when `path` names a video file we treat as playable. Keep this list in
// sync with the scanner globs (libraryworker.cpp) and SlideshowView.qml.
inline bool isVideoPath(const QString &path)
{
    static const QSet<QString> kVideoSuffixes = {
        QStringLiteral("mp4"), QStringLiteral("m4v"), QStringLiteral("webm"),
        QStringLiteral("mkv"), QStringLiteral("mov"), QStringLiteral("avi"),
    };
    return kVideoSuffixes.contains(QFileInfo(path).suffix().toLower());
}

struct Photo {
    int id = -1;
    QString path;
    QString fileName;
    bool isVideo = false;
    // Capture time in Unix seconds: EXIF date taken when available, else the file
    // mtime. 0 when unknown (not yet backfilled). Drives the timeline grouping.
    qint64 takenAt = 0;
    // True when this is a non-representative duplicate: another active photo with
    // the same content hash was added earlier. Hidden only in the ungrouped view
    // (timeline and folder views show every copy). See Database::loadPhotos.
    bool isDuplicate = false;
    QSet<int> tagIds;
    QStringList tagNames;
};

struct TagInfo {
    int id = -1;
    QString name;
    int count = 0;
    // The category this tag belongs to, if any (-1 / empty when uncategorized).
    // colorSlot indexes the category palette (see OmarchyTheme::categoryPalette).
    int categoryId = -1;
    QString categoryName;
    int colorSlot = -1;
};

// A user-defined grouping of tags (place, event, artist, ...). Tags inherit the
// category's palette slot for color coding in the chips.
struct CategoryInfo {
    int id = -1;
    QString name;
    int colorSlot = 0;
    int tagCount = 0; // tags assigned to this category
};

struct FolderInfo {
    int id = -1;
    QString path;
    int count = 0;        // photos currently in the library under this folder
    bool enabled = true;  // whether its photos are shown in the gallery
};

// Minimal row view used by the rescan reconciliation (id, path, content hash).
struct PhotoRef {
    int id = -1;
    QString path;
    QString hash;
};
