// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#include "photosort.h"

namespace photosort {

QCollator collator()
{
    QCollator c;
    c.setNumericMode(true);
    c.setCaseSensitivity(Qt::CaseInsensitive);
    return c;
}

bool lessThan(const Keys &a, const Keys &b, Mode mode, bool ascending, bool folderFirst,
              const QCollator &collator)
{
    if (folderFirst) {
        // "Group by folder": compare the containing folder first (always
        // ascending), then order within a folder by the chosen sort below.
        const QString ad = a.path.left(a.path.lastIndexOf(QLatin1Char('/')));
        const QString bd = b.path.left(b.path.lastIndexOf(QLatin1Char('/')));
        const int dirCmp = collator.compare(ad, bd);
        if (dirCmp != 0)
            return dirCmp < 0;
    }

    // Primary key for the active sort, computed in an ascending sense (negative =
    // a orders first). `ascending` flips it; the file name is always the
    // ascending tiebreak so equal keys keep a stable order.
    int cmp = 0;
    switch (mode) {
    case Mode::Date:
        // Ascending = oldest first. Photos without a date (0) share the smallest
        // key, so they group together (at the start ascending, the end descending).
        cmp = a.takenAt < b.takenAt ? -1 : a.takenAt > b.takenAt ? 1 : 0;
        break;
    case Mode::TagCount:
        // Ascending = fewest tags first.
        cmp = a.tagCount < b.tagCount ? -1 : a.tagCount > b.tagCount ? 1 : 0;
        break;
    case Mode::Name:
        break; // the name comparison below is the key
    }

    const int nameCmp = collator.compare(a.fileName, b.fileName);
    if (mode == Mode::Name)
        cmp = nameCmp;
    else if (cmp == 0)
        return nameCmp < 0; // stable tiebreak, always ascending

    return ascending ? cmp < 0 : cmp > 0;
}

} // namespace photosort
