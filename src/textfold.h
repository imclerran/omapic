// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>
#include <QQmlEngine>
#include <QString>

// Fold a string for accent- and case-insensitive matching: decompose to NFD and
// drop the combining marks, then lower-case. So "café", "Cafe" and "cafe" all
// fold to "cafe" and compare equal. Used by the C++ photo filter and by every
// QML search/autocomplete field (via the TextMatch singleton) so that typing
// unaccented text still finds tags stored with accents.
QString foldText(const QString &s);

// QML-facing singleton wrapping foldText so autocomplete/search fields fold the
// same way the C++ filter does. Usable from any QML file in the module as
// `TextMatch.fold(s)` / `TextMatch.contains(haystack, needle)`.
class TextMatch : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
public:
    explicit TextMatch(QObject *parent = nullptr) : QObject(parent) {}

    Q_INVOKABLE QString fold(const QString &s) const { return foldText(s); }
    Q_INVOKABLE bool contains(const QString &haystack, const QString &needle) const
    {
        return foldText(haystack).contains(foldText(needle));
    }
};
