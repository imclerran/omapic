// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QChar>
#include <QString>

// The lexical facts of OQL shared by the parser (oqlparser.cpp) and the
// formatter (oql.cpp): what ends a term, and the closed set of predicate keys.
// Internal to the engine; consumers use oql.h.
namespace oql::syntax {

// A character that ends a bare name: whitespace, '|', ')' or '"'. A longest
// match may only end right before one of these (or at the end of the input).
inline bool isBoundary(QChar c)
{
    return c.isSpace() || c == QLatin1Char('|') || c == QLatin1Char(')') || c == QLatin1Char('"');
}

// The predicate keys. A colon starts a predicate only when the text before it
// is one of these (case-insensitively), so `Star Wars: Episode IV` is a plain
// tag. The table is the one place to extend when type:/date:/name: arrive.
enum class Predicate { Is, Album, Cat };
struct PredicateSpec {
    const char *key;
    Predicate pred;
};
inline const PredicateSpec kPredicates[] = {
    {"is", Predicate::Is},
    {"album", Predicate::Album},
    {"cat", Predicate::Cat},
};
inline const QString kIsKey = QStringLiteral("is");
inline const QString kAlbumKey = QStringLiteral("album");
inline const QString kCatKey = QStringLiteral("cat");
inline const QString kFavValue = QStringLiteral("fav");
inline const QString kUntaggedValue = QStringLiteral("untagged");

// If `text` at `pos` starts with "<key>:" for one of the predicate keys, the
// index of that key in kPredicates; else -1.
inline int predicateKeyAt(const QString &text, int pos)
{
    for (int i = 0; i < int(sizeof(kPredicates) / sizeof(kPredicates[0])); ++i) {
        const QLatin1String key(kPredicates[i].key);
        const int n = key.size();
        if (pos + n < text.size() && text.at(pos + n) == QLatin1Char(':')
            && text.mid(pos, n).compare(key, Qt::CaseInsensitive) == 0)
            return i;
    }
    return -1;
}

} // namespace oql::syntax
