// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

#include <optional>

// Reads tags written by other apps (darktable, digiKam, Lightroom, Microsoft,
// …) from XMP sidecar files, so a library can pick up tagging done elsewhere.
// Pure and Library-free; the worker calls it on import/rescan. Writing sidecars
// is a later phase.
namespace sidecar {

// A (category, tag) pair. An empty category means an uncategorised tag.
using CategorizedTag = QPair<QString, QString>;

// Tags parsed out of a sidecar. `flat` are plain keywords (dc:subject);
// `categorized` are hierarchical keywords split into {category, tag}. The
// omapic:* fields are only present (and only trustworthy) in sidecars omapic
// wrote; callers should trust `albums`/`favorite` only when `contentHash` is set.
struct ParsedTags {
    QStringList flat;
    QList<CategorizedTag> categorized;
    QString contentHash;      // omapic:contentHash, e.g. "sha256:<hex>"; empty if absent
    QStringList albums;       // omapic:albums (album display names)
    bool favorite = false;    // omapic:favorite
    bool hasOmapicData = false; // any omapic:* field was present
};

// Split a hierarchical keyword into {category, tag}. The separator is '|'
// (Lightroom's hierarchicalSubject) or '/' (digiKam / Microsoft). The first
// component is the category; the remainder, rejoined with '/', is the tag. A
// keyword with no separator becomes an uncategorised tag ({"", keyword}).
CategorizedTag splitHierarchical(const QString &keyword);

// Path of the XMP sidecar omapic would read for `photoPath`: "<photoPath>.xmp"
// (preferred, the darktable/digiKam convention) or "<base>.xmp" (the Adobe
// convention) as a fallback. Empty when neither file exists.
QString sidecarPath(const QString &photoPath);

// Read the sidecar for `photoPath`. Returns nullopt when no sidecar exists or it
// can't be parsed.
std::optional<ParsedTags> readSidecar(const QString &photoPath);

// What omapic writes into a photo's sidecar. flat = plain keyword names;
// categorized = {category, tag} hierarchical keywords (empty category = bare);
// albums = album display names; favorite = in Favorites. contentHash is the bare
// hex SHA-256 of the photo (the "sha256:" prefix is added on write).
struct WriteTags {
    QStringList flat;
    QList<CategorizedTag> categorized;
    QString contentHash;
    QStringList albums;
    bool favorite = false;

    bool isEmpty() const { return flat.isEmpty() && categorized.isEmpty()
                                  && albums.isEmpty() && !favorite; }
};

enum class WriteResult {
    Written,   // the sidecar was created or updated
    Removed,   // omapic's only content was cleared, so the file was deleted
    Unchanged, // nothing to write and no omapic-created file to remove
    Failed,    // an I/O or parse error; the existing file (if any) is untouched
};

// Write `tags` into the XMP sidecar for `photoPath`, replacing only omapic's own
// keys and preserving everything else another app put in the packet. Uses the
// existing sidecar (full-name or Adobe fallback) or creates "<photoPath>.xmp".
// Writes atomically (temp file + rename). When `tags` is empty, omapic's keys are
// removed and an otherwise-empty, omapic-created file is deleted.
WriteResult writeSidecar(const QString &photoPath, const WriteTags &tags);

} // namespace sidecar
