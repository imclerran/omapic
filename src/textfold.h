// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>

// Fold a string for accent- and case-insensitive matching: decompose to NFD and
// drop the combining marks, then lower-case. So "café", "Cafe" and "cafe" all
// fold to "cafe" and compare equal. Used by the C++ photo filter, the OQL query
// parser, and by every QML search/autocomplete field (via the TextMatch
// singleton in textmatch.h) so that typing unaccented text still finds tags
// stored with accents.
//
// This header is part of the core library (no GUI/QML dependency); the QML
// singleton that wraps it lives in textmatch.h, in the app module.
QString foldText(const QString &s);
