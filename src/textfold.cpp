// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#include "textfold.h"

#include <QChar>

QString foldText(const QString &s)
{
    if (s.isEmpty())
        return s;
    // NFD splits an accented letter into its base plus a combining mark (é ->
    // e + U+0301); dropping the marks leaves the bare letters. Letters without a
    // canonical decomposition (ø, æ, ß, …) are left as-is — rare for tag names.
    const QString decomposed = s.normalized(QString::NormalizationForm_D);
    QString out;
    out.reserve(decomposed.size());
    for (const QChar c : decomposed) {
        switch (c.category()) {
        case QChar::Mark_NonSpacing:
        case QChar::Mark_SpacingCombining:
        case QChar::Mark_Enclosing:
            continue; // drop the diacritic
        default:
            out.append(c);
        }
    }
    return out.toLower();
}
