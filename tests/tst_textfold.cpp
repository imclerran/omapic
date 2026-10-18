// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QtTest>

#include "textfold.h"

// foldText backs every accent-/case-insensitive search and autocomplete in the
// app (C++ filter and the QML TextMatch singleton), so a regression here would
// silently break "cafe" finding "Café".
class TestTextFold : public QObject {
    Q_OBJECT

private slots:
    void emptyStaysEmpty();
    void lowercases();
    void stripsAccents();
    void equivalenceClasses();
    void preservesNonDecomposable();
    void leavesInnerSpacingAndDigits();
    void preservesCompatibilityLigatures();
    void handlesSurrogatePairs();
    void textMatchContains();
};

void TestTextFold::emptyStaysEmpty()
{
    QCOMPARE(foldText(QString()), QString());
    QCOMPARE(foldText(QStringLiteral("")), QStringLiteral(""));
}

void TestTextFold::lowercases()
{
    QCOMPARE(foldText(QStringLiteral("HELLO")), QStringLiteral("hello"));
    QCOMPARE(foldText(QStringLiteral("MixedCase")), QStringLiteral("mixedcase"));
}

void TestTextFold::stripsAccents()
{
    QCOMPARE(foldText(QStringLiteral("café")), QStringLiteral("cafe"));
    QCOMPARE(foldText(QStringLiteral("naïve")), QStringLiteral("naive"));
    QCOMPARE(foldText(QStringLiteral("JALAPEÑO")), QStringLiteral("jalapeno"));
    QCOMPARE(foldText(QStringLiteral("Zürich")), QStringLiteral("zurich"));
}

void TestTextFold::equivalenceClasses()
{
    // The whole point: these three must fold to the same string so an unaccented
    // query matches an accented tag and vice versa.
    const QString a = foldText(QStringLiteral("café"));
    const QString b = foldText(QStringLiteral("Cafe"));
    const QString c = foldText(QStringLiteral("CAFÉ"));
    QCOMPARE(a, b);
    QCOMPARE(b, c);

    // A precomposed "é" (U+00E9) and a decomposed "e" + combining acute must also
    // fold alike, since EXIF/filesystems can hand us either normalization form.
    const QString precomposed = QStringLiteral("résumé");      // résumé
    const QString decomposed = QStringLiteral("résumé");     // re´sume´
    QCOMPARE(foldText(precomposed), QStringLiteral("resume"));
    QCOMPARE(foldText(precomposed), foldText(decomposed));
}

void TestTextFold::preservesNonDecomposable()
{
    // Letters with no canonical decomposition keep their base glyph (lower-cased),
    // per the documented behavior — they are not dropped or mangled.
    QCOMPARE(foldText(QStringLiteral("Smørrebrød")), QStringLiteral("smørrebrød"));
    QCOMPARE(foldText(QStringLiteral("Straße")), QStringLiteral("straße"));
}

void TestTextFold::leavesInnerSpacingAndDigits()
{
    // Folding must not collapse spacing or alter digits — only case and diacritics.
    QCOMPARE(foldText(QStringLiteral("São Paulo 2024")), QStringLiteral("sao paulo 2024"));
}

void TestTextFold::preservesCompatibilityLigatures()
{
    // foldText uses NFD (canonical), not NFKD (compatibility), so the "fi" ligature
    // is preserved rather than split into "f" + "i". This documents the deliberate
    // boundary: "ﬁle" does not fold to "file".
    const QString ligature = QStringLiteral("ﬁle"); // ﬁle
    QCOMPARE(foldText(ligature), QStringLiteral("ﬁle"));
    QVERIFY(foldText(ligature) != QStringLiteral("file"));
}

void TestTextFold::handlesSurrogatePairs()
{
    // A non-BMP code point (🎉, a surrogate pair in UTF-16) must pass through intact
    // — it is neither a combining mark nor an upper-case letter — while surrounding
    // text still folds normally.
    const QString party = QStringLiteral("\U0001F389"); // 🎉
    const QString input = QStringLiteral("Café ") + party; // "Café 🎉"
    const QString folded = foldText(input);
    QCOMPARE(folded, QStringLiteral("cafe ") + party);
    QVERIFY(folded.endsWith(party)); // the emoji's surrogate pair is unbroken
}

void TestTextFold::textMatchContains()
{
    TextMatch tm;
    QVERIFY(tm.contains(QStringLiteral("Café Society"), QStringLiteral("cafe")));
    QVERIFY(tm.contains(QStringLiteral("Zürich trip"), QStringLiteral("ZURICH")));
    QVERIFY(!tm.contains(QStringLiteral("beach"), QStringLiteral("mountain")));
    // An empty needle folds to empty, which every haystack contains.
    QVERIFY(tm.contains(QStringLiteral("anything"), QString()));
    QCOMPARE(tm.fold(QStringLiteral("NAÏVE")), QStringLiteral("naive"));
}

QTEST_GUILESS_MAIN(TestTextFold)
#include "tst_textfold.moc"
