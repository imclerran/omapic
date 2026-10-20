// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QFileInfo>
#include <QList>
#include <QMetaType>
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
    // Extract the suffix directly instead of constructing a QFileInfo per call —
    // this runs for every photo on load. A dot at or before the last '/' (or
    // none) means no extension, matching QFileInfo::suffix for dotfiles.
    const int dot = path.lastIndexOf(QLatin1Char('.'));
    if (dot <= path.lastIndexOf(QLatin1Char('/')) + 1)
        return false;
    return kVideoSuffixes.contains(path.mid(dot + 1).toLower());
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
    // Accent-/case-folded concatenation of the file name and tag names, built by
    // PhotoModel for the gallery search so filtering doesn't re-fold every row on
    // every keystroke. Not populated by the database layer.
    QString searchFold;

    // Every field the gallery/model reads, so PhotoModel::setPhotos can detect a
    // reload that wouldn't change anything on screen and skip the model reset.
    // searchFold is intentionally excluded: it's derived from fileName + tagNames
    // (already compared) and isn't populated on the incoming database rows, so
    // comparing it would always differ and defeat the guard.
    bool operator==(const Photo &o) const
    {
        return id == o.id && path == o.path && fileName == o.fileName
            && isVideo == o.isVideo && takenAt == o.takenAt
            && isDuplicate == o.isDuplicate && favorite == o.favorite
            && tagIds == o.tagIds && tagNames == o.tagNames;
    }
    bool operator!=(const Photo &o) const { return !(*this == o); }
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

// A smart album: a saved tag query (a set of tags + AND/OR mode) that
// auto-populates in the Albums view. Unlike a regular album it has no stored
// membership — a photo belongs iff its tags currently match the query.
struct SmartAlbumInfo {
    int id = -1;
    QString name;
    bool matchAll = true;   // true = AND (carry every tag); false = OR (any tag)
    QList<int> tagIds;
};

struct FolderInfo {
    int id = -1;
    QString path;
    int count = 0;        // photos currently in the library under this folder
    bool enabled = true;  // whether its photos are shown in the gallery
};

// Minimal row view used by the rescan reconciliation (id, path, content hash),
// plus the file stats and sidecar sync marker rescan needs to detect in-place
// edits and externally-changed sidecars. mtime/size/sidecarWrittenAt are -1 when
// the stored column is NULL (e.g. a row from before schema v10).
struct PhotoRef {
    int id = -1;
    QString path;
    QString hash;
    qint64 mtime = -1;
    qint64 size = -1;
    qint64 sidecarWrittenAt = -1;
};

// Passed across the worker↔GUI thread boundary by the async tag-sidebar refresh.
Q_DECLARE_METATYPE(TagInfo)
