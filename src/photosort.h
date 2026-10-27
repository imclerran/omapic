// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QCollator>
#include <QString>

// The gallery's photo ordering, as a pure comparison over the sort keys so the
// grid (PhotoFilterModel::lessThan) and the command-line `query` tool order
// photos identically. See PhotoFilterModel::SortMode for the modes' meaning.
namespace photosort {

enum class Mode { Name, Date, TagCount };

// The direction a mode reads naturally in: names A→Z, dates newest first, tag
// counts most-tagged first. The gallery resets to this when a mode is picked.
inline bool defaultAscending(Mode mode)
{
    return mode == Mode::Name;
}

// What one photo contributes to the comparison.
struct Keys {
    QString path;      // full path; the containing folder is derived from it
    QString fileName;
    qint64 takenAt = 0; // capture time, 0 = unknown
    int tagCount = 0;   // user-visible tags
};

// A natural, case-insensitive collator (digits compare by value), as the grid
// uses. Build one per sort, not per comparison.
QCollator collator();

// True when `a` orders before `b`. folderFirst groups by containing folder
// (always ascending) before the mode's key; the file name is always the
// ascending tiebreak so equal keys keep a stable order.
bool lessThan(const Keys &a, const Keys &b, Mode mode, bool ascending, bool folderFirst,
              const QCollator &collator);

} // namespace photosort
