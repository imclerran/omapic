// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QtTest>

#include <QRandomGenerator>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include "oql.h"

using namespace oql;

// The OQL engine: parsing (longest-match name resolution, syntax-wins-at-term-
// start, predicates, precedence, errors), the formatter's quoting decisions, the
// evaluator, and the parse(format(t)) == t round trip. Everything runs against a
// hand-built MapVocabulary, so no database is involved.
class TestOql : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();

    // One row per tricky input: does it parse, and to what canonical text.
    void trickyInputs_data();
    void trickyInputs();

    void darkForestDependsOnVocabulary();
    void quotesEndTerms();
    void parenAtTermStartAlwaysOpensGroup();
    void leadingDashIsAlwaysNot();
    void predicateIs();
    void predicateAlbumAndCat();
    void aliasesResolveToCanonicalTag();
    void precedence();
    void errorsCarrySpansAndSuggestions();
    void notesExplainVocabularyDependentReadings();
    void spansCoverTerms();
    void foldingMatchesAccentsAndCase();

    void formatterStructure();
    void formatterQuotesOnlyWhenNeeded();
    void formatterUsesCurrentNames();
    void formatterWritesMissingAsLiteral();
    void roundTripProperty();

    void evaluateLeaves();
    void evaluateOperators();

private:
    // The base vocabulary: tricky tag names, aliases, albums, categories.
    MapVocabulary m_vocab;
    // The same plus a tag literally named "dark forest".
    MapVocabulary m_vocabDF;

    static QString canon(const QString &text, const Vocabulary &v);
    static Node parsed(const QString &text, const Vocabulary &v);
    // A random tree over the vocabulary's names, for the round-trip property.
    Node randomTree(QRandomGenerator &rng, int depth, const Vocabulary &v) const;
};

enum TagId {
    Dark = 1, Forest, DarkForest, Untitled, MinusTwenty, Dinner2019, Paris, IsIt, TShirt,
    Beach, Sunset, Sunrise, StarWars, Cafe, Dinner, UntitledBare, X, Dark2
};
constexpr int kAlbumTrip = 100, kAlbumDark = 101, kFavorites = 102;
constexpr int kSmartBest = 200, kSmartTrip = 201;
constexpr int kCatPlace = 300, kCatMood = 301;

void TestOql::initTestCase()
{
    const struct { int id; const char *name; } tags[] = {
        {Dark, "dark"}, {Forest, "forest"}, {Untitled, "(untitled)"}, {MinusTwenty, "-20 °C"},
        {Dinner2019, "dinner (2019)"}, {Paris, "paris"}, {IsIt, "is:it"}, {TShirt, "t-shirt"},
        {Beach, "beach"}, {Sunset, "sunset"}, {Sunrise, "sunrise"},
        {StarWars, "Star Wars: Episode IV"}, {Cafe, "café"}, {Dinner, "dinner"},
        {UntitledBare, "untitled"}, {X, "x"}, {Dark2, "dark 2"},
    };
    for (MapVocabulary *v : {&m_vocab, &m_vocabDF}) {
        for (const auto &t : tags)
            v->addTag(t.id, QString::fromUtf8(t.name));
        v->addAlias(QStringLiteral("forests"), Forest);
        v->addAlias(QStringLiteral("the woods"), Forest);
        v->addAlbum(kAlbumTrip, QStringLiteral("Trip"));
        v->addAlbum(kAlbumDark, QStringLiteral("dark"));
        v->addAlbum(kFavorites, QStringLiteral("Favorites"));
        v->addSmartAlbum(kSmartBest, QStringLiteral("Best of 2019"));
        v->addSmartAlbum(kSmartTrip, QStringLiteral("Trip")); // same name as the manual album
        v->addCategory(kCatPlace, QStringLiteral("Place"));
        v->addCategory(kCatMood, QStringLiteral("Mood (old)"));
    }
    m_vocabDF.addTag(DarkForest, QStringLiteral("dark forest"));
}

QString TestOql::canon(const QString &text, const Vocabulary &v)
{
    const ParseResult r = parse(text, v);
    if (!r.ok())
        return QStringLiteral("ERROR: ") + r.error.message;
    return format(r.root, v);
}

Node TestOql::parsed(const QString &text, const Vocabulary &v)
{
    const ParseResult r = parse(text, v);
    if (!r.ok())
        qWarning("parse failed: %s", qPrintable(r.error.message));
    return r.root;
}

void TestOql::trickyInputs_data()
{
    QTest::addColumn<QString>("input");
    QTest::addColumn<bool>("withDarkForest");
    QTest::addColumn<QString>("expected"); // canonical text, or "ERROR: <message>"

    auto row = [](const char *name, const char *input, bool df, const char *expected) {
        QTest::newRow(name) << QString::fromUtf8(input) << df << QString::fromUtf8(expected);
    };
    row("two words, no such tag", "dark forest", false, "dark forest");
    row("two words, tag exists", "dark forest", true, "dark forest");
    row("quoted first word", "\"dark\" forest", true, "\"dark\" forest");
    row("quoted second word", "dark \"forest\"", true, "\"dark\" forest");
    row("paren at start is a group", "(untitled)", false, "untitled");
    row("quoted paren name is the tag", "\"(untitled)\"", false, "\"(untitled)\"");
    row("name with parens", "dinner (2019)", false, "dinner (2019)");
    row("group containing paren name", "(paris | dinner (2019))", false, "paris | dinner (2019)");
    row("nested group then tag", "((untitled) | x)", false, "untitled | x");
    row("quoted paren name in group", "(\"(untitled)\" | x)", false, "\"(untitled)\" | x");
    row("quoted leading dash", "\"-20 °C\"", false, "\"-20 °C\"");
    row("dash inside a word", "t-shirt", false, "t-shirt");
    row("negated dashed word", "-t-shirt", false, "-t-shirt");
    row("is:it predicate", "is:it", false, "ERROR: Unknown is: value \"is:it\"");
    row("is:it quoted tag", "\"is:it\"", false, "\"is:it\"");
    row("is:fav", "is:fav", false, "is:fav");
    row("is:untagged", "is:untagged", false, "is:untagged");
    row("colon in a tag", "Star Wars: Episode IV", false, "Star Wars: Episode IV");
    row("album predicate", "album:Trip", false, "album:Trip");
    row("album with spaces", "album:Best of 2019 beach", false, "album:Best of 2019 beach");
    row("cat predicate", "cat:Place", false, "cat:Place");
    row("cat with parens", "cat:Mood (old)", false, "cat:Mood (old)");
    row("unbalanced open", "(beach", false, "ERROR: Missing closing \")\"");
    row("unbalanced close", "beach)", false, "ERROR: Unmatched \")\"");
    row("empty group", "()", false, "ERROR: Empty group \"()\"");
    row("dangling bar", "beach |", false, "ERROR: Expected a term after \"|\"");
    row("leading bar", "| beach", false, "ERROR: Expected a term before \"|\"");
    row("double bar", "beach | | sunset", false, "ERROR: Expected a term after \"|\"");
    row("dangling dash", "beach -", false, "ERROR: Expected a term after \"-\"");
    row("unknown name", "zzz", false, "ERROR: Unknown tag \"zzz\"");
    row("unknown quoted", "\"zzz\"", false, "ERROR: Unknown tag \"zzz\"");
    row("unclosed quote", "\"beach", false, "ERROR: Missing closing quote");
    row("empty", "   ", false, "ERROR: Empty query");
    row("alias", "forests", false, "forest");
    row("multi-word alias", "the woods", false, "forest");
    row("and/or precedence", "beach sunset | sunrise", false, "beach (sunset | sunrise)");
    row("or then and", "beach | sunset sunrise", false, "(beach | sunset) sunrise");
    row("not binds tightest", "-beach | sunset", false, "-beach | sunset");
    row("not over group", "-(beach sunset)", false, "-(beach sunset)");
    row("double not", "--beach", false, "--beach");
    row("extra whitespace", "   beach    sunset  ", false, "beach sunset");
    row("bar without spaces", "beach|sunset", false, "beach | sunset");
    row("collapsed inner space", "dark    forest", true, "dark forest");
    row("case and accent folded", "CAFE", false, "café");
}

void TestOql::trickyInputs()
{
    QFETCH(QString, input);
    QFETCH(bool, withDarkForest);
    QFETCH(QString, expected);
    const Vocabulary &v = withDarkForest ? static_cast<const Vocabulary &>(m_vocabDF) : m_vocab;
    QCOMPARE(canon(input, v), expected);
}

void TestOql::darkForestDependsOnVocabulary()
{
    // Without the compound tag the two words are two AND-ed tags...
    QCOMPARE(parsed(QStringLiteral("dark forest"), m_vocab),
             Node::andOf({Node::tag(Dark, QStringLiteral("dark")), Node::tag(Forest, QStringLiteral("forest"))}));
    // ...and with it, longest match takes the whole thing as one tag.
    QCOMPARE(parsed(QStringLiteral("dark forest"), m_vocabDF),
             Node::tag(DarkForest, QStringLiteral("dark forest")));
    // The longest match must end at a boundary: "dark forestry" is not the tag.
    const ParseResult r = parse(QStringLiteral("dark forestry"), m_vocabDF);
    QVERIFY(!r.ok());
    QCOMPARE(r.error.message, QStringLiteral("Unknown tag \"forestry\""));
}

void TestOql::quotesEndTerms()
{
    // A quote always ends a term, so neither of these is the compound tag.
    const Node two = Node::andOf({Node::tag(Dark, QStringLiteral("dark")), Node::tag(Forest, QStringLiteral("forest"))});
    QCOMPARE(parsed(QStringLiteral("\"dark\" forest"), m_vocabDF), two);
    QCOMPARE(parsed(QStringLiteral("dark \"forest\""), m_vocabDF), two);
    QCOMPARE(parsed(QStringLiteral("\"dark\"\"forest\""), m_vocabDF), two);
    // Quotes resolve aliases like bare names do.
    QCOMPARE(parsed(QStringLiteral("\"forests\""), m_vocab), Node::tag(Forest, QStringLiteral("forest")));
}

void TestOql::parenAtTermStartAlwaysOpensGroup()
{
    // Even though a tag "(untitled)" exists, a bare "(untitled)" is a group
    // around the tag "untitled"; the quoted form is the parenthesised tag.
    QCOMPARE(parsed(QStringLiteral("(untitled)"), m_vocab), Node::tag(UntitledBare, QStringLiteral("untitled")));
    QCOMPARE(parsed(QStringLiteral("\"(untitled)\""), m_vocab), Node::tag(Untitled, QStringLiteral("(untitled)")));
    // Inside a longest match '(' is ordinary: "dinner (2019)" beats "dinner".
    QCOMPARE(parsed(QStringLiteral("dinner (2019)"), m_vocab), Node::tag(Dinner2019, QStringLiteral("dinner (2019)")));
    QCOMPARE(parsed(QStringLiteral("(paris | dinner (2019))"), m_vocab),
             Node::orOf({Node::tag(Paris, QStringLiteral("paris")), Node::tag(Dinner2019, QStringLiteral("dinner (2019)"))}));
    QCOMPARE(parsed(QStringLiteral("((untitled) | x)"), m_vocab),
             Node::orOf({Node::tag(UntitledBare, QStringLiteral("untitled")), Node::tag(X, QStringLiteral("x"))}));

    // With no tag "untitled", the group's content is an unknown name — and the
    // error suggests the quoted form of the tag the user probably meant.
    MapVocabulary v;
    v.addTag(Untitled, QStringLiteral("(untitled)"));
    const ParseResult r = parse(QStringLiteral("(untitled)"), v);
    QVERIFY(!r.ok());
    QCOMPARE(r.error.message, QStringLiteral("Unknown tag \"untitled\""));
    QVERIFY(r.error.suggestions.contains(QStringLiteral("\"(untitled)\"")));
}

void TestOql::leadingDashIsAlwaysNot()
{
    // "-20 °C" exists as a tag, but a bare leading '-' is NOT whatever exists.
    const ParseResult r = parse(QStringLiteral("-20 °C"), m_vocab);
    QVERIFY(!r.ok());
    QCOMPARE(r.error.message, QStringLiteral("Unknown tag \"20\""));
    QCOMPARE(r.error.suggestions.first(), QStringLiteral("\"-20 °C\""));
    QCOMPARE(parsed(QStringLiteral("\"-20 °C\""), m_vocab), Node::tag(MinusTwenty, QStringLiteral("-20 °C")));
    // A '-' not at a term start is an ordinary character.
    QCOMPARE(parsed(QStringLiteral("t-shirt"), m_vocab), Node::tag(TShirt, QStringLiteral("t-shirt")));
    QCOMPARE(parsed(QStringLiteral("-t-shirt"), m_vocab), Node::notOf(Node::tag(TShirt, QStringLiteral("t-shirt"))));
    QCOMPARE(parsed(QStringLiteral("beach -sunset"), m_vocab),
             Node::andOf({Node::tag(Beach, QStringLiteral("beach")), Node::notOf(Node::tag(Sunset, QStringLiteral("sunset")))}));
    // '-' followed by whitespace still negates the next term.
    QCOMPARE(parsed(QStringLiteral("- beach"), m_vocab), Node::notOf(Node::tag(Beach, QStringLiteral("beach"))));
}

void TestOql::predicateIs()
{
    QCOMPARE(parsed(QStringLiteral("is:fav"), m_vocab), Node::favorite());
    QCOMPARE(parsed(QStringLiteral("IS:Untagged"), m_vocab), Node::untagged());
    QCOMPARE(parsed(QStringLiteral("-is:fav"), m_vocab), Node::notOf(Node::favorite()));
    // A colon only starts a predicate after one of the closed set of keys, so
    // "is:it" is a predicate (and an error) while "is:it" quoted is the tag.
    ParseResult r = parse(QStringLiteral("is:it"), m_vocab);
    QVERIFY(!r.ok());
    QCOMPARE(r.error.span.start, 0);
    QCOMPARE(r.error.span.length, 5);
    QCOMPARE(r.error.suggestions,
             QStringList({QStringLiteral("\"is:it\""), QStringLiteral("is:fav"), QStringLiteral("is:untagged")}));
    QCOMPARE(parsed(QStringLiteral("\"is:it\""), m_vocab), Node::tag(IsIt, QStringLiteral("is:it")));
    // A bare "is:" with nothing after it.
    r = parse(QStringLiteral("is: beach"), m_vocab);
    QVERIFY(!r.ok());
    QCOMPARE(r.error.message, QStringLiteral("Expected fav or untagged after \"is:\""));
    // Other words ending in a colon are plain tags, not predicates.
    QCOMPARE(parsed(QStringLiteral("Star Wars: Episode IV"), m_vocab),
             Node::tag(StarWars, QStringLiteral("Star Wars: Episode IV")));
}

void TestOql::predicateAlbumAndCat()
{
    // album: resolves in the album namespace — "dark" here is the album, not the tag.
    QCOMPARE(parsed(QStringLiteral("album:dark"), m_vocab), Node::album(kAlbumDark, QStringLiteral("dark")));
    // A manual album wins over a smart album of the same name; otherwise the
    // smart album is found.
    QCOMPARE(parsed(QStringLiteral("album:Trip"), m_vocab), Node::album(kAlbumTrip, QStringLiteral("Trip")));
    QCOMPARE(parsed(QStringLiteral("album:best of 2019"), m_vocab),
             Node::smartAlbum(kSmartBest, QStringLiteral("Best of 2019")));
    QCOMPARE(parsed(QStringLiteral("album:\"Best of 2019\" beach"), m_vocab),
             Node::andOf({Node::smartAlbum(kSmartBest, QStringLiteral("Best of 2019")), Node::tag(Beach, QStringLiteral("beach"))}));
    QCOMPARE(parsed(QStringLiteral("-album:Trip"), m_vocab), Node::notOf(Node::album(kAlbumTrip, QStringLiteral("Trip"))));
    QCOMPARE(parsed(QStringLiteral("cat:place"), m_vocab), Node::category(kCatPlace, QStringLiteral("Place")));
    QCOMPARE(parsed(QStringLiteral("cat:Mood (old)"), m_vocab), Node::category(kCatMood, QStringLiteral("Mood (old)")));
    QCOMPARE(parsed(QStringLiteral("(cat:Place | album:Trip)"), m_vocab),
             Node::orOf({Node::category(kCatPlace, QStringLiteral("Place")), Node::album(kAlbumTrip, QStringLiteral("Trip"))}));

    ParseResult r = parse(QStringLiteral("album:Trips"), m_vocab);
    QVERIFY(!r.ok());
    QCOMPARE(r.error.message, QStringLiteral("Unknown album \"Trips\""));
    QVERIFY(r.error.suggestions.contains(QStringLiteral("Trip")));
    r = parse(QStringLiteral("cat:"), m_vocab);
    QVERIFY(!r.ok());
    QCOMPARE(r.error.message, QStringLiteral("Expected a category name after \"cat:\""));
    r = parse(QStringLiteral("cat:beach"), m_vocab);
    QVERIFY(!r.ok());
    QCOMPARE(r.error.message, QStringLiteral("Unknown category \"beach\""));
}

void TestOql::aliasesResolveToCanonicalTag()
{
    const Node forest = Node::tag(Forest, QStringLiteral("forest"));
    QCOMPARE(parsed(QStringLiteral("forests"), m_vocab), forest);
    QCOMPARE(parsed(QStringLiteral("the woods"), m_vocab), forest);
    // The canonical name is what comes back out.
    QCOMPARE(canon(QStringLiteral("dark | forests"), m_vocab), QStringLiteral("dark | forest"));
}

void TestOql::precedence()
{
    const Node beach = Node::tag(Beach, QStringLiteral("beach"));
    const Node sunset = Node::tag(Sunset, QStringLiteral("sunset"));
    const Node sunrise = Node::tag(Sunrise, QStringLiteral("sunrise"));
    QCOMPARE(parsed(QStringLiteral("beach sunset | sunrise"), m_vocab),
             Node::andOf({beach, Node::orOf({sunset, sunrise})}));
    QCOMPARE(parsed(QStringLiteral("beach | sunset sunrise"), m_vocab),
             Node::andOf({Node::orOf({beach, sunset}), sunrise}));
    QCOMPARE(parsed(QStringLiteral("-beach | sunset"), m_vocab),
             Node::orOf({Node::notOf(beach), sunset}));
    QCOMPARE(parsed(QStringLiteral("-(beach | sunset) sunrise"), m_vocab),
             Node::andOf({Node::notOf(Node::orOf({beach, sunset})), sunrise}));
    QCOMPARE(parsed(QStringLiteral("(beach sunset) sunrise"), m_vocab),
             Node::andOf({Node::andOf({beach, sunset}), sunrise}));
    QCOMPARE(parsed(QStringLiteral("beach sunset sunrise"), m_vocab),
             Node::andOf({beach, sunset, sunrise}));
    QCOMPARE(parsed(QStringLiteral("beach | sunset | sunrise"), m_vocab),
             Node::orOf({beach, sunset, sunrise}));
    QCOMPARE(parsed(QStringLiteral("--beach"), m_vocab), Node::notOf(Node::notOf(beach)));
}

void TestOql::errorsCarrySpansAndSuggestions()
{
    ParseResult r = parse(QStringLiteral("beach (sunset"), m_vocab);
    QVERIFY(!r.ok());
    QCOMPARE(r.error.span.start, 6); // the '(' that was never closed
    QCOMPARE(r.error.span.length, 1);

    r = parse(QStringLiteral("beach sunset)"), m_vocab);
    QVERIFY(!r.ok());
    QCOMPARE(r.error.span.start, 12);

    r = parse(QStringLiteral("beach sunsets"), m_vocab);
    QVERIFY(!r.ok());
    QCOMPARE(r.error.message, QStringLiteral("Unknown tag \"sunsets\""));
    QCOMPARE(r.error.span.start, 6);
    QCOMPARE(r.error.span.length, 7);
    QCOMPARE(r.error.suggestions.first(), QStringLiteral("sunset"));

    // Typo correction by edit distance; prefix matches rank first.
    r = parse(QStringLiteral("sunet"), m_vocab);
    QVERIFY(!r.ok());
    QVERIFY(r.error.suggestions.contains(QStringLiteral("sunset")));
    r = parse(QStringLiteral("sun"), m_vocab);
    QVERIFY(!r.ok());
    QCOMPARE(r.error.suggestions.mid(0, 2), QStringList({QStringLiteral("sunset"), QStringLiteral("sunrise")}));

    // Only the first error is reported, in text order.
    r = parse(QStringLiteral("zzz (yyy"), m_vocab);
    QVERIFY(!r.ok());
    QCOMPARE(r.error.message, QStringLiteral("Unknown tag \"zzz\""));

    r = parse(QString(), m_vocab);
    QVERIFY(!r.ok());
    QCOMPARE(r.error.message, QStringLiteral("Empty query"));
}

void TestOql::notesExplainVocabularyDependentReadings()
{
    // Two bare words read as separate tags: one note over the run.
    ParseResult r = parse(QStringLiteral("dark forest"), m_vocab);
    QVERIFY(r.ok());
    QCOMPARE(r.notes.size(), 1);
    QCOMPARE(r.notes.first().span.start, 0);
    QCOMPARE(r.notes.first().span.length, 11);
    QVERIFY(r.notes.first().message.contains(QStringLiteral("2 separate terms")));
    QVERIFY(r.notes.first().message.contains(QStringLiteral("\"dark\", \"forest\"")));

    // A multi-word name read as one tag.
    r = parse(QStringLiteral("dark forest"), m_vocabDF);
    QVERIFY(r.ok());
    QCOMPARE(r.notes.size(), 1);
    QCOMPARE(r.notes.first().message, QStringLiteral("\"dark forest\" is read as one tag"));

    // Quoting removes the ambiguity, and with it the note.
    r = parse(QStringLiteral("\"dark\" \"forest\""), m_vocabDF);
    QVERIFY(r.ok());
    QVERIFY(r.notes.isEmpty());

    // A negated bare term still counts towards a run (-dark forest could have
    // been NOT "dark forest").
    r = parse(QStringLiteral("-dark forest"), m_vocab);
    QVERIFY(r.ok());
    QCOMPARE(r.notes.size(), 1);
    QVERIFY(r.notes.first().message.startsWith(QStringLiteral("\"-dark forest\" is read as")));

    // Terms separated by syntax are unambiguous.
    r = parse(QStringLiteral("dark | forest"), m_vocab);
    QVERIFY(r.ok());
    QVERIFY(r.notes.isEmpty());
}

void TestOql::spansCoverTerms()
{
    const ParseResult r = parse(QStringLiteral("  beach  -(sunset | album:Trip) "), m_vocab);
    QVERIFY(r.ok());
    QCOMPARE(r.root.kind, NodeKind::And);
    QCOMPARE(r.root.children.at(0).span.start, 2);
    QCOMPARE(r.root.children.at(0).span.length, 5);
    const Node &neg = r.root.children.at(1);
    QCOMPARE(neg.span.start, 9);
    QCOMPARE(neg.span.length, 22); // "-(sunset | album:Trip)"
    const Node &group = neg.children.first();
    QCOMPARE(group.span.start, 10);
    QCOMPARE(group.span.length, 21); // the parenthesised OR, parens included
    QCOMPARE(group.children.at(1).span.start, 20);
    QCOMPARE(group.children.at(1).span.length, 10); // "album:Trip"
    QCOMPARE(r.root.span.start, 2);
    QCOMPARE(r.root.span.end(), 31);
}

void TestOql::foldingMatchesAccentsAndCase()
{
    const Node cafe = Node::tag(Cafe, QStringLiteral("café"));
    QCOMPARE(parsed(QStringLiteral("cafe"), m_vocab), cafe);
    QCOMPARE(parsed(QStringLiteral("CAFÉ"), m_vocab), cafe);
    QCOMPARE(parsed(QStringLiteral("\"Cafe\""), m_vocab), cafe);
    QCOMPARE(parsed(QStringLiteral("star wars: episode iv"), m_vocab),
             Node::tag(StarWars, QStringLiteral("Star Wars: Episode IV")));
    QCOMPARE(foldName(QStringLiteral("  Dark \t Forest ")), QStringLiteral("dark forest"));
}

void TestOql::formatterStructure()
{
    const Node a = Node::tag(Beach), b = Node::tag(Sunset), c = Node::tag(Sunrise);
    QCOMPARE(format(Node::andOf({a, b, c}), m_vocab), QStringLiteral("beach sunset sunrise"));
    QCOMPARE(format(Node::orOf({a, b, c}), m_vocab), QStringLiteral("beach | sunset | sunrise"));
    // Same-kind nesting keeps its parentheses so the structure survives.
    QCOMPARE(format(Node::andOf({Node::andOf({a, b}), c}), m_vocab), QStringLiteral("(beach sunset) sunrise"));
    QCOMPARE(format(Node::orOf({Node::orOf({a, b}), c}), m_vocab), QStringLiteral("(beach | sunset) | sunrise"));
    // AND under OR must be grouped; OR under AND is grouped for readability.
    QCOMPARE(format(Node::orOf({Node::andOf({a, b}), c}), m_vocab), QStringLiteral("(beach sunset) | sunrise"));
    QCOMPARE(format(Node::andOf({a, Node::orOf({b, c})}), m_vocab), QStringLiteral("beach (sunset | sunrise)"));
    // NOT over a leaf is bare; over an operator it is grouped.
    QCOMPARE(format(Node::notOf(a), m_vocab), QStringLiteral("-beach"));
    QCOMPARE(format(Node::notOf(Node::orOf({a, b})), m_vocab), QStringLiteral("-(beach | sunset)"));
    QCOMPARE(format(Node::notOf(Node::notOf(a)), m_vocab), QStringLiteral("--beach"));
    QCOMPARE(format(Node::andOf({Node::notOf(a), Node::favorite(), Node::untagged()}), m_vocab),
             QStringLiteral("-beach is:fav is:untagged"));
    QCOMPARE(format(Node::andOf({Node::album(kAlbumTrip), Node::smartAlbum(kSmartBest), Node::category(kCatPlace)}), m_vocab),
             QStringLiteral("album:Trip album:Best of 2019 cat:Place"));
}

void TestOql::formatterQuotesOnlyWhenNeeded()
{
    // Names that start with syntax, or look like a predicate, are quoted.
    QCOMPARE(format(Node::tag(Untitled), m_vocab), QStringLiteral("\"(untitled)\""));
    QCOMPARE(format(Node::tag(MinusTwenty), m_vocab), QStringLiteral("\"-20 °C\""));
    QCOMPARE(format(Node::tag(IsIt), m_vocab), QStringLiteral("\"is:it\""));
    // Interior parentheses, dashes and colons need nothing.
    QCOMPARE(format(Node::tag(Dinner2019), m_vocab), QStringLiteral("dinner (2019)"));
    QCOMPARE(format(Node::tag(TShirt), m_vocab), QStringLiteral("t-shirt"));
    QCOMPARE(format(Node::tag(StarWars), m_vocab), QStringLiteral("Star Wars: Episode IV"));
    QCOMPARE(format(Node::orOf({Node::tag(Paris), Node::tag(Dinner2019)}), m_vocab),
             QStringLiteral("paris | dinner (2019)"));
    // A name is quoted only when a longer known name would swallow what follows.
    const Node darkForest = Node::andOf({Node::tag(Dark), Node::tag(Forest)});
    QCOMPARE(format(darkForest, m_vocab), QStringLiteral("dark forest"));
    QCOMPARE(format(darkForest, m_vocabDF), QStringLiteral("\"dark\" forest"));
    QCOMPARE(format(Node::andOf({Node::tag(Dark), Node::tag(X)}), m_vocabDF), QStringLiteral("dark x"));
    QCOMPARE(format(Node::orOf({Node::tag(Dark), Node::tag(Forest)}), m_vocabDF), QStringLiteral("dark | forest"));
    // "dark 2" exists, so "dark" followed by a tag "2" would merge... but "dark"
    // followed by "dinner" does not.
    QCOMPARE(format(Node::andOf({Node::tag(Dark), Node::tag(Dinner)}), m_vocab), QStringLiteral("dark dinner"));
    // "dinner" followed by "(2019)"-ish text: a group starting with "(" after
    // "dinner" would be swallowed by the tag "dinner (2019)"... only if the group
    // text matched; "dinner (x)" does not, so no quote.
    QCOMPARE(format(Node::andOf({Node::tag(Dinner), Node::tag(X)}), m_vocab), QStringLiteral("dinner x"));
    // Album names after album: are quoted on the same terms.
    MapVocabulary v;
    v.addAlbum(1, QStringLiteral("-old"));
    v.addAlbum(2, QStringLiteral("a b"));
    v.addAlbum(3, QStringLiteral("a"));
    v.addTag(4, QStringLiteral("b"));
    QCOMPARE(format(Node::album(1), v), QStringLiteral("album:\"-old\""));
    QCOMPARE(format(Node::andOf({Node::album(3), Node::tag(4)}), v), QStringLiteral("album:\"a\" b"));
}

void TestOql::formatterUsesCurrentNames()
{
    // A node carries the name it was parsed with, but the text form always
    // shows the vocabulary's current name for the id (renames need no rewrite).
    QCOMPARE(format(Node::tag(Beach, QStringLiteral("old name")), m_vocab), QStringLiteral("beach"));
    QCOMPARE(format(Node::smartAlbum(kSmartBest, QStringLiteral("stale")), m_vocab),
             QStringLiteral("album:Best of 2019"));
    // An id the vocabulary doesn't know falls back to the recorded name.
    QCOMPARE(format(Node::tag(999, QStringLiteral("gone")), m_vocab), QStringLiteral("gone"));
}

void TestOql::formatterWritesMissingAsLiteral()
{
    QCOMPARE(format(Node::missing(QStringLiteral("old tag")), m_vocab), QStringLiteral("\"old tag\""));
    QCOMPARE(format(Node::missing(QStringLiteral("gone"), NameKind::Album), m_vocab), QStringLiteral("album:\"gone\""));
    QCOMPARE(format(Node::missing(QStringLiteral("gone"), NameKind::SmartAlbum), m_vocab), QStringLiteral("album:\"gone\""));
    QCOMPARE(format(Node::missing(QStringLiteral("gone"), NameKind::Category), m_vocab), QStringLiteral("cat:\"gone\""));
    QCOMPARE(format(Node::andOf({Node::tag(Beach), Node::notOf(Node::missing(QStringLiteral("gone")))}), m_vocab),
             QStringLiteral("beach -\"gone\""));
    // Reading it back is an error, not a silent drop.
    const ParseResult r = parse(QStringLiteral("beach -\"gone\""), m_vocab);
    QVERIFY(!r.ok());
    QCOMPARE(r.error.message, QStringLiteral("Unknown tag \"gone\""));
}

Node TestOql::randomTree(QRandomGenerator &rng, int depth, const Vocabulary &v) const
{
    const int leafIds[] = {Dark, Forest, DarkForest, Untitled, MinusTwenty, Dinner2019, Paris, IsIt,
                           TShirt, Beach, Sunset, Sunrise, StarWars, Cafe, Dinner, UntitledBare, X, Dark2};
    const int roll = depth <= 0 ? 0 : int(rng.bounded(10));
    if (roll < 5) {
        switch (rng.bounded(8)) {
        case 0: return Node::album(rng.bounded(2) ? kAlbumTrip : kAlbumDark);
        case 1: return Node::smartAlbum(kSmartBest);
        case 2: return Node::category(rng.bounded(2) ? kCatPlace : kCatMood);
        case 3: return rng.bounded(2) ? Node::favorite() : Node::untagged();
        default:
            for (;;) { // skip ids this vocabulary doesn't have ("dark forest" in the base one)
                const int id = leafIds[rng.bounded(int(sizeof(leafIds) / sizeof(leafIds[0])))];
                if (!v.name(NameKind::Tag, id).isEmpty())
                    return Node::tag(id);
            }
        }
    }
    if (roll < 7)
        return Node::notOf(randomTree(rng, depth - 1, v));
    QVector<Node> children;
    const int n = 2 + int(rng.bounded(3));
    for (int i = 0; i < n; ++i)
        children.append(randomTree(rng, depth - 1, v));
    return roll < 9 ? Node::andOf(children) : Node::orOf(children);
}

void TestOql::roundTripProperty()
{
    // parse(format(t)) == t for random trees over the vocabulary, including the
    // one where "dark forest" shadows "dark" + "forest". Names in generated
    // nodes are left empty; the parsed tree carries vocabulary names, so compare
    // against the formatted-and-reparsed form of a name-filled copy instead:
    // format is a pure function of (kind, id), so two formats agreeing means the
    // trees agree.
    QRandomGenerator rng(20260101);
    for (int i = 0; i < 400; ++i) {
        const Vocabulary &v = (i % 2) ? static_cast<const Vocabulary &>(m_vocabDF) : m_vocab;
        const Node t = randomTree(rng, 4, v);
        const QString text = format(t, v);
        const ParseResult r = parse(text, v);
        QVERIFY2(r.ok(), qPrintable(QStringLiteral("%1 -> %2").arg(text, r.error.message)));
        const QString again = format(r.root, v);
        QCOMPARE(again, text);
        // Structural comparison, ignoring the names the parse filled in.
        std::function<void(Node &)> strip = [&](Node &n) {
            n.name.clear();
            for (Node &c : n.children)
                strip(c);
        };
        Node parsedCopy = r.root;
        strip(parsedCopy);
        QCOMPARE(parsedCopy, t);
    }
}

void TestOql::evaluateLeaves()
{
    EvalContext ctx;
    ctx.favoritesTagId = kFavorites;
    ctx.albumTagIds = {kFavorites, kAlbumTrip, kAlbumDark};
    ctx.tagCategory = {{Paris, kCatPlace}, {Beach, kCatPlace}, {Dark, kCatMood}};

    QVERIFY(matches(Node::tag(Beach), {Beach, Sunset}, ctx));
    QVERIFY(!matches(Node::tag(Beach), {Sunset}, ctx));
    QVERIFY(matches(Node::album(kAlbumTrip), {kAlbumTrip, Beach}, ctx));
    QVERIFY(!matches(Node::album(kAlbumTrip), {Beach}, ctx));
    QVERIFY(matches(Node::category(kCatPlace), {Paris}, ctx));
    QVERIFY(matches(Node::category(kCatPlace), {Sunset, Beach}, ctx));
    QVERIFY(!matches(Node::category(kCatPlace), {Sunset, Dark}, ctx));
    QVERIFY(matches(Node::favorite(), {kFavorites}, ctx));
    QVERIFY(!matches(Node::favorite(), {kAlbumTrip}, ctx));
    // Untagged ignores album and Favorites membership.
    QVERIFY(matches(Node::untagged(), {}, ctx));
    QVERIFY(matches(Node::untagged(), {kFavorites, kAlbumTrip}, ctx));
    QVERIFY(!matches(Node::untagged(), {kFavorites, Beach}, ctx));
    // A missing name never matches; an unexpanded smart-album reference doesn't either.
    QVERIFY(!matches(Node::missing(QStringLiteral("gone")), {Beach}, ctx));
    QVERIFY(!matches(Node::smartAlbum(kSmartBest), {Beach}, ctx));
}

void TestOql::evaluateOperators()
{
    EvalContext ctx;
    const Node a = Node::tag(Beach), b = Node::tag(Sunset), c = Node::tag(Sunrise);
    QVERIFY(matches(Node::andOf({a, b}), {Beach, Sunset, Paris}, ctx));
    QVERIFY(!matches(Node::andOf({a, b}), {Beach, Paris}, ctx));
    QVERIFY(matches(Node::orOf({a, b}), {Sunset}, ctx));
    QVERIFY(!matches(Node::orOf({a, b}), {Sunrise}, ctx));
    QVERIFY(matches(Node::notOf(a), {Sunset}, ctx));
    QVERIFY(!matches(Node::notOf(a), {Beach}, ctx));
    QVERIFY(matches(Node::notOf(Node::missing(QStringLiteral("gone"))), {Beach}, ctx));
    // beach (sunset | sunrise)
    const Node q = Node::andOf({a, Node::orOf({b, c})});
    QVERIFY(matches(q, {Beach, Sunrise}, ctx));
    QVERIFY(matches(q, {Beach, Sunset}, ctx));
    QVERIFY(!matches(q, {Beach}, ctx));
    QVERIFY(!matches(q, {Sunset, Sunrise}, ctx));
    // (beach | sunset) -sunrise
    const Node q2 = Node::andOf({Node::orOf({a, b}), Node::notOf(c)});
    QVERIFY(matches(q2, {Sunset}, ctx));
    QVERIFY(!matches(q2, {Sunset, Sunrise}, ctx));
    // Degenerate operators: an empty AND/OR never matches.
    QVERIFY(!matches(Node::andOf({}), {Beach}, ctx));
    QVERIFY(!matches(Node::orOf({}), {Beach}, ctx));
}

QTEST_GUILESS_MAIN(TestOql)
#include "tst_oql.moc"
