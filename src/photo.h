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
    // True when this photo carries the hidden Favorites tag. Drives the thumbnail
    // star; favorites are a reserved album (see AlbumInfo / Database album APIs).
    bool favorite = false;
    // Every tag id on the photo, including the hidden album/favorites tags (the
    // album filter and the star need them); tagNames carries only the user-visible
    // (non-system) names, so album membership never shows as a chip or tag badge.
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

// A tag implication: every photo carrying the antecedent tag also gets the
// consequent tag ("if this tag, then that tag"). Carries both tags' names and
// palette slots so the manager can render them as color-coded chips.
struct ImplicationInfo {
    int antecedentId = -1;
    QString antecedentName;
    int antecedentColorSlot = -1;
    int consequentId = -1;
    QString consequentName;
    int consequentColorSlot = -1;
};

// An album: a tag in the reserved (hidden) "Albums" category. Favorites is the
// one album with favorite == true, always sorted first.
struct AlbumInfo {
    int id = -1;
    QString name;
    bool favorite = false;
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
