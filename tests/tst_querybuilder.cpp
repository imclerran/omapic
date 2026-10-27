// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QtTest>

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSet>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

#include "clipboard.h"
#include "oql.h"
#include "textmatch.h"

// The query builder (qml/QueryBuilder.qml) driven as a user would: loaded from
// the source tree against a mock library that answers parseQuery /
// tokenizeQuery / queryMatchCount / queryVocabulary with the real engine over a
// fixed vocabulary, and typed into with real key events. Checks the token list,
// the validation verdict and the text that would be saved.
namespace {

// What Library offers the builder, over a hand-built vocabulary and photo set.
class MockLibrary : public QObject {
    Q_OBJECT
public:
    MockLibrary()
    {
        m_vocab.addTag(1, QStringLiteral("beach"));
        m_vocab.addTag(2, QStringLiteral("sunset"));
        m_vocab.addTag(3, QStringLiteral("people"));
        m_vocab.addTag(4, QStringLiteral("dinner (2019)"));
        m_vocab.addTag(5, QStringLiteral("(untitled)"));
        m_vocab.addAlias(QStringLiteral("seaside"), 1);
        m_vocab.addAlbum(10, QStringLiteral("Trip"));
        m_vocab.addSmartAlbum(20, QStringLiteral("Best"));
        m_vocab.addCategory(30, QStringLiteral("Place"));
        m_photos = {{1}, {1, 2, 11}, {1, 3}, {2}, {3, 10}, {}};
        m_ctx.favoritesTagId = 11;
        m_ctx.albumTagIds = {10, 11};
        m_ctx.tagCategory = {{1, 30}};
    }

    Q_INVOKABLE QVariantMap parseQuery(const QString &text) const
    {
        const oql::ParseResult r = oql::parse(text, m_vocab);
        if (!r.ok())
            return {{QStringLiteral("ok"), false}, {QStringLiteral("message"), r.error.message},
                    {QStringLiteral("start"), r.error.span.start}, {QStringLiteral("length"), r.error.span.length},
                    {QStringLiteral("suggestions"), r.error.suggestions}};
        return {{QStringLiteral("ok"), true}, {QStringLiteral("canonical"), oql::format(r.root, m_vocab)}};
    }
    Q_INVOKABLE QVariantList tokenizeQuery(const QString &text) const
    {
        QVariantList out;
        for (const oql::Token &t : oql::tokenize(text, m_vocab)) {
            QString type = QStringLiteral("bad");
            switch (t.kind) {
            case oql::Token::Tag: type = QStringLiteral("tag"); break;
            case oql::Token::Predicate: type = QStringLiteral("pred"); break;
            case oql::Token::Open: type = QStringLiteral("open"); break;
            case oql::Token::Close: type = QStringLiteral("close"); break;
            case oql::Token::Or: type = QStringLiteral("or"); break;
            case oql::Token::Unknown: break;
            }
            out.append(QVariantMap{{QStringLiteral("type"), type}, {QStringLiteral("negated"), t.negated},
                                   {QStringLiteral("name"), t.name}, {QStringLiteral("id"), t.id},
                                   {QStringLiteral("value"), t.value}, {QStringLiteral("colorSlot"), -1},
                                   {QStringLiteral("categoryName"), QString()}, {QStringLiteral("kind"), QString()},
                                   {QStringLiteral("start"), t.span.start}, {QStringLiteral("length"), t.span.length}});
        }
        return out;
    }
    Q_INVOKABLE int queryMatchCount(const QString &text) const
    {
        const oql::ParseResult r = oql::parse(text, m_vocab);
        if (!r.ok())
            return -1;
        int n = 0;
        for (const QSet<int> &p : m_photos)
            if (oql::matches(r.root, p, m_ctx))
                ++n;
        return n;
    }
    Q_INVOKABLE QVariantMap queryVocabulary() const
    {
        return {{QStringLiteral("albums"), QVariantList{QVariantMap{{QStringLiteral("name"), QStringLiteral("Trip")}, {QStringLiteral("smart"), false}},
                                                       QVariantMap{{QStringLiteral("name"), QStringLiteral("Best")}, {QStringLiteral("smart"), true}}}},
                {QStringLiteral("categories"), QVariantList{QVariantMap{{QStringLiteral("name"), QStringLiteral("Place")}, {QStringLiteral("colorSlot"), 0}}}}};
    }

private:
    oql::MapVocabulary m_vocab;
    QVector<QSet<int>> m_photos;
    oql::EvalContext m_ctx;
};

class MockTheme : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList categoryPalette READ categoryPalette CONSTANT)
public:
    QVariantList categoryPalette() const { return {QStringLiteral("#ff0000"), QStringLiteral("#00ff00")}; }
};

} // namespace

class TestQueryBuilder : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void loadsTextAsChips();
    void commitsTypedTagsWithTabAndEnter();
    void aliasAndPredicateRows();
    void groupsAndParentheses();
    void backspaceSelectsThenEdits();
    void negationAndOr();
    void clipboard();
    void badTokensAndErrors();
    void rawTextToggle();
    void enterOnUnresolvedTextOffersSplit();

private:
    QVariantList tokens() const { return m_builder->property("tokens").toList(); }
    QString tokenType(int i) const { return tokens().at(i).toMap().value(QStringLiteral("type")).toString(); }
    QString tokenName(int i) const { return tokens().at(i).toMap().value(QStringLiteral("name")).toString(); }
    bool tokenNegated(int i) const { return tokens().at(i).toMap().value(QStringLiteral("negated")).toBool(); }
    QString text() const { return m_builder->property("text").toString(); }
    bool valid() const { return m_builder->property("valid").toBool(); }
    int cursor() const { return m_builder->property("cursor").toInt(); }
    QString inputText() const { return m_input->property("text").toString(); }
    void load(const QString &query);
    void type(const QString &s)
    {
        for (const QChar c : s) // keyClicks has no QWindow overload
            QTest::keyClick(m_window, c.toLatin1());
    }
    void key(Qt::Key k, Qt::KeyboardModifiers mods = Qt::NoModifier) { QTest::keyClick(m_window, k, mods); }

    QQmlEngine *m_engine = nullptr;
    MockLibrary m_library;
    MockTheme m_theme;
    QQuickWindow *m_window = nullptr;
    QQuickItem *m_builder = nullptr;
    QQuickItem *m_input = nullptr;
};

void TestQueryBuilder::initTestCase()
{
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    qmlRegisterSingletonType<TextMatch>("Omapic", 1, 0, "TextMatch",
                                        [](QQmlEngine *, QJSEngine *) -> QObject * { return new TextMatch; });
    qmlRegisterSingletonType<Clipboard>("Omapic", 1, 0, "Clipboard",
                                        [](QQmlEngine *, QJSEngine *) -> QObject * { return new Clipboard; });
}

void TestQueryBuilder::init()
{
    m_engine = new QQmlEngine(this);
    m_engine->rootContext()->setContextProperty(QStringLiteral("mockLibrary"), &m_library);
    m_engine->rootContext()->setContextProperty(QStringLiteral("omarchyTheme"), &m_theme);
    const QString qmlDir = QStringLiteral(OMAPIC_QML_DIR);
    QQmlComponent component(m_engine);
    component.setData(QByteArrayLiteral(
        "import QtQuick\n"
        "import QtQuick.Controls\n"
        "import QtQuick.Window\n"
        "Window {\n"
        "    width: 900; height: 500; visible: true\n"
        "    QueryBuilder {\n"
        "        objectName: \"builder\"; width: 860; library: mockLibrary\n"
        "        tags: [{name: \"beach\", colorSlot: 0, categoryName: \"Place\"},"
        "               {name: \"sunset\", colorSlot: -1, categoryName: \"\"},"
        "               {name: \"people\", colorSlot: -1, categoryName: \"\"},"
        "               {name: \"dinner (2019)\", colorSlot: -1, categoryName: \"\"},"
        "               {name: \"(untitled)\", colorSlot: -1, categoryName: \"\"}]\n"
        "        aliases: [{aliasName: \"seaside\", tagName: \"beach\", colorSlot: 0, categoryName: \"Place\"}]\n"
        "    }\n"
        "}\n"), QUrl::fromLocalFile(qmlDir + QStringLiteral("/harness.qml")));
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    QObject *root = component.create();
    QVERIFY2(root, qPrintable(component.errorString()));
    m_window = qobject_cast<QQuickWindow *>(root);
    QVERIFY(m_window);
    m_builder = m_window->findChild<QQuickItem *>(QStringLiteral("builder"));
    QVERIFY(m_builder);
    QVERIFY(QTest::qWaitForWindowExposed(m_window));
    m_window->requestActivate();
    QVERIFY(QTest::qWaitForWindowActive(m_window));
    QMetaObject::invokeMethod(m_builder, "focusInput");
    QTRY_VERIFY(m_window->activeFocusItem() != nullptr);
    // The entry field is whatever took the focus: the TagAutoField's TextField.
    m_input = m_window->activeFocusItem();
    QVERIFY2(QString::fromLatin1(m_input->metaObject()->className()).contains(QStringLiteral("TextField")),
             m_input->metaObject()->className());
}

void TestQueryBuilder::cleanup()
{
    delete m_window;
    m_window = nullptr;
    delete m_engine;
    m_engine = nullptr;
}

void TestQueryBuilder::load(const QString &query)
{
    QMetaObject::invokeMethod(m_builder, "setQueryText", Q_ARG(QVariant, query));
}

void TestQueryBuilder::loadsTextAsChips()
{
    load(QStringLiteral("beach -people (sunset | album:Trip) cat:Place is:fav"));
    QCOMPARE(tokens().size(), 9);
    QCOMPARE(tokenType(0), QStringLiteral("tag"));
    QCOMPARE(tokenType(1), QStringLiteral("tag"));
    QVERIFY(tokenNegated(1));
    QCOMPARE(tokenType(2), QStringLiteral("open"));
    QCOMPARE(tokenType(4), QStringLiteral("or"));
    QCOMPARE(tokenType(5), QStringLiteral("pred"));
    QCOMPARE(tokens().at(5).toMap().value(QStringLiteral("value")).toString(), QStringLiteral("Trip"));
    QCOMPARE(tokenType(6), QStringLiteral("close"));
    QCOMPARE(tokenType(8), QStringLiteral("pred"));
    QVERIFY(valid());
    QCOMPARE(text(), QStringLiteral("beach -people (sunset | album:Trip) cat:Place is:fav"));
    QCOMPARE(cursor(), 9);
    QCOMPARE(m_builder->property("matchCount").toInt(), 1); // {1,2,11}
    QVERIFY(m_builder->property("unmatched").toList().isEmpty());
}

void TestQueryBuilder::commitsTypedTagsWithTabAndEnter()
{
    load(QStringLiteral("beach"));
    // Space never commits; Tab does, and the typed name becomes a chip.
    type(QStringLiteral("sun set"));
    QCOMPARE(tokens().size(), 1);
    QCOMPARE(inputText(), QStringLiteral("sun set"));
    key(Qt::Key_Backspace); key(Qt::Key_Backspace); key(Qt::Key_Backspace); key(Qt::Key_Backspace);
    type(QStringLiteral("set"));
    key(Qt::Key_Tab);
    QCOMPARE(tokens().size(), 2);
    QCOMPARE(tokenName(1), QStringLiteral("sunset"));
    QCOMPARE(inputText(), QString());
    // A multi-word name commits whole on Enter (the exact match is pre-highlighted).
    type(QStringLiteral("dinner (2019)"));
    key(Qt::Key_Return);
    QCOMPARE(tokens().size(), 3);
    QCOMPARE(tokenName(2), QStringLiteral("dinner (2019)"));
    QVERIFY(valid());
    QCOMPARE(text(), QStringLiteral("beach sunset dinner (2019)"));
    // Enter on an empty field submits.
    QSignalSpy submitted(m_builder, SIGNAL(submitted()));
    QVERIFY(submitted.isValid());
    key(Qt::Key_Return);
    QCOMPARE(submitted.count(), 1);
}

void TestQueryBuilder::aliasAndPredicateRows()
{
    load(QString());
    // An alias commits its canonical tag.
    type(QStringLiteral("seaside"));
    key(Qt::Key_Tab);
    QCOMPARE(tokens().size(), 1);
    QCOMPARE(tokenName(0), QStringLiteral("beach"));
    // Predicates: is:fav offered and committed as a chip; album:<name> from the
    // vocabulary's album names.
    type(QStringLiteral("is:fav"));
    key(Qt::Key_Return);
    QCOMPARE(tokens().size(), 2);
    QCOMPARE(tokenType(1), QStringLiteral("pred"));
    QCOMPARE(tokens().at(1).toMap().value(QStringLiteral("value")).toString(), QStringLiteral("fav"));
    type(QStringLiteral("album:trip"));
    key(Qt::Key_Return);
    QCOMPARE(tokens().size(), 3);
    QCOMPARE(tokens().at(2).toMap().value(QStringLiteral("value")).toString(), QStringLiteral("Trip"));
    type(QStringLiteral("cat:Place"));
    key(Qt::Key_Tab);
    QCOMPARE(tokens().size(), 4);
    QCOMPARE(text(), QStringLiteral("beach is:fav album:Trip cat:Place"));
    QVERIFY(valid());
}

void TestQueryBuilder::groupsAndParentheses()
{
    load(QStringLiteral("beach"));
    // "(" offers the "( )" row first; Enter inserts both with the cursor between.
    type(QStringLiteral("("));
    key(Qt::Key_Return);
    QCOMPARE(tokens().size(), 3);
    QCOMPARE(tokenType(1), QStringLiteral("open"));
    QCOMPARE(tokenType(2), QStringLiteral("close"));
    QCOMPARE(cursor(), 2);
    type(QStringLiteral("sunset"));
    key(Qt::Key_Tab);
    QCOMPARE(cursor(), 3);
    // ")" steps over the auto-inserted one rather than adding another.
    type(QStringLiteral(")"));
    QCOMPARE(tokens().size(), 4);
    QCOMPARE(cursor(), 4);
    QCOMPARE(inputText(), QString());
    QVERIFY(valid());
    QCOMPARE(text(), QStringLiteral("beach sunset"));
    // Backspace on an empty "()" removes both.
    type(QStringLiteral("("));
    key(Qt::Key_Return);
    QCOMPARE(tokens().size(), 6);
    key(Qt::Key_Backspace);
    QCOMPARE(tokens().size(), 4);
    // A ")" typed inside a name stays in the name; an unmatched one is flagged.
    type(QStringLiteral("dinner (2019)"));
    QCOMPARE(inputText(), QStringLiteral("dinner (2019)"));
    key(Qt::Key_Tab);
    QCOMPARE(tokenName(4), QStringLiteral("dinner (2019)"));
    type(QStringLiteral(")"));
    QCOMPARE(tokenType(5), QStringLiteral("close"));
    QCOMPARE(m_builder->property("unmatched").toList(), QVariantList{5});
    QVERIFY(!valid());
    // The quoted form is the tag, not a group, and the formatter quotes it back.
    load(QString());
    type(QStringLiteral("(untitled)"));
    key(Qt::Key_Return);
    QCOMPARE(tokens().size(), 1);
    QCOMPARE(tokenName(0), QStringLiteral("(untitled)"));
    QCOMPARE(text(), QStringLiteral("\"(untitled)\""));
}

void TestQueryBuilder::backspaceSelectsThenEdits()
{
    load(QStringLiteral("beach sunset"));
    key(Qt::Key_Backspace);
    QCOMPARE(m_builder->property("selStart").toInt(), 1);
    QCOMPARE(tokens().size(), 2);
    key(Qt::Key_Backspace);
    QCOMPARE(tokens().size(), 1);
    QCOMPARE(inputText(), QStringLiteral("sunset"));
    QCOMPARE(cursor(), 1);
    // ←/→ step across chips when the field is empty.
    key(Qt::Key_Backspace); key(Qt::Key_Backspace); key(Qt::Key_Backspace);
    key(Qt::Key_Backspace); key(Qt::Key_Backspace); key(Qt::Key_Backspace);
    QCOMPARE(inputText(), QString());
    key(Qt::Key_Left);
    QCOMPARE(cursor(), 0);
    key(Qt::Key_Right);
    QCOMPARE(cursor(), 1);
    // Shift+← selects a range; Delete removes it.
    load(QStringLiteral("beach sunset people"));
    key(Qt::Key_Left, Qt::ShiftModifier);
    key(Qt::Key_Left, Qt::ShiftModifier);
    QCOMPARE(m_builder->property("selStart").toInt(), 1);
    QCOMPARE(m_builder->property("selEnd").toInt(), 2);
    key(Qt::Key_Delete);
    QCOMPARE(tokens().size(), 1);
    QCOMPARE(text(), QStringLiteral("beach"));
}

void TestQueryBuilder::negationAndOr()
{
    load(QStringLiteral("beach sunset"));
    // "-" on a selected chip toggles its negation.
    key(Qt::Key_Left, Qt::ShiftModifier);
    type(QStringLiteral("-"));
    QVERIFY(tokenNegated(1));
    QCOMPARE(inputText(), QString());
    type(QStringLiteral("-"));
    QVERIFY(!tokenNegated(1));
    key(Qt::Key_Escape);
    // "-" with nothing selected offers NOT…; choosing it negates the next chip.
    load(QStringLiteral("beach"));
    type(QStringLiteral("-"));
    key(Qt::Key_Return);
    QVERIFY(m_builder->property("pendingNegate").toBool());
    type(QStringLiteral("people"));
    key(Qt::Key_Tab);
    QVERIFY(tokenNegated(1));
    QCOMPARE(text(), QStringLiteral("beach -people"));
    // "|" between chips makes an OR.
    type(QStringLiteral("|"));
    QCOMPARE(tokenType(2), QStringLiteral("or"));
    type(QStringLiteral("sunset"));
    key(Qt::Key_Tab);
    QCOMPARE(text(), QStringLiteral("beach (-people | sunset)"));
    QVERIFY(valid());
}

void TestQueryBuilder::clipboard()
{
    load(QStringLiteral("beach | sunset"));
    key(Qt::Key_A, Qt::ControlModifier);
    QCOMPARE(m_builder->property("selStart").toInt(), 0);
    QCOMPARE(m_builder->property("selEnd").toInt(), 2);
    key(Qt::Key_C, Qt::ControlModifier);
    Clipboard clip;
    QCOMPARE(clip.text(), QStringLiteral("beach | sunset"));
    load(QString());
    key(Qt::Key_V, Qt::ControlModifier);
    QCOMPARE(tokens().size(), 3);
    QCOMPARE(text(), QStringLiteral("beach | sunset"));
    // Pasting text with an unknown name keeps it as a highlighted bad chip.
    clip.setText(QStringLiteral("people zzz"));
    key(Qt::Key_V, Qt::ControlModifier);
    QCOMPARE(tokens().size(), 5);
    QCOMPARE(tokenType(4), QStringLiteral("bad"));
    QCOMPARE(m_builder->property("selStart").toInt(), 4);
}

void TestQueryBuilder::badTokensAndErrors()
{
    load(QStringLiteral("beach zzz"));
    QCOMPARE(tokenType(1), QStringLiteral("bad"));
    QVERIFY(!valid());
    QCOMPARE(m_builder->property("errorToken").toInt(), 1);
    QVERIFY(m_builder->property("errorMessage").toString().contains(QStringLiteral("Unknown tag")));
    QCOMPARE(m_builder->property("matchCount").toInt(), -1);
    // Removing it restores validity.
    key(Qt::Key_Backspace);
    key(Qt::Key_Backspace);
    QCOMPARE(inputText(), QStringLiteral("zzz"));
    key(Qt::Key_Backspace); key(Qt::Key_Backspace); key(Qt::Key_Backspace);
    QVERIFY(valid());
    QCOMPARE(text(), QStringLiteral("beach"));
    // A dangling OR is an error pointing at the bar.
    load(QStringLiteral("beach |"));
    QVERIFY(!valid());
    QCOMPARE(m_builder->property("errorToken").toInt(), 1);
}

void TestQueryBuilder::rawTextToggle()
{
    load(QStringLiteral("beach -people"));
    QMetaObject::invokeMethod(m_builder, "setRawMode", Q_ARG(QVariant, true));
    QCOMPARE(m_builder->property("rawText").toString(), QStringLiteral("beach -people"));
    m_builder->setProperty("rawText", QStringLiteral("sunset | people"));
    QVERIFY(valid());
    QCOMPARE(text(), QStringLiteral("sunset | people"));
    QCOMPARE(m_builder->property("matchCount").toInt(), 4);
    QMetaObject::invokeMethod(m_builder, "setRawMode", Q_ARG(QVariant, false));
    QCOMPARE(tokens().size(), 3);
    QCOMPARE(tokenType(1), QStringLiteral("or"));
    // Invalid raw text is reported, and comes back as chips with the bad span.
    QMetaObject::invokeMethod(m_builder, "setRawMode", Q_ARG(QVariant, true));
    m_builder->setProperty("rawText", QStringLiteral("sunset zzz"));
    QVERIFY(!valid());
    QCOMPARE(text(), QStringLiteral("sunset zzz"));
    QMetaObject::invokeMethod(m_builder, "setRawMode", Q_ARG(QVariant, false));
    QCOMPARE(tokenType(1), QStringLiteral("bad"));
}

void TestQueryBuilder::enterOnUnresolvedTextOffersSplit()
{
    load(QString());
    // "sunset people" is no tag: Enter keeps the text and offers to split it.
    type(QStringLiteral("sunset people"));
    key(Qt::Key_Return);
    QCOMPARE(tokens().size(), 0);
    QCOMPARE(inputText(), QStringLiteral("sunset people"));
    const QVariantList corrections = m_builder->property("corrections").toList();
    QVERIFY(!corrections.isEmpty());
    QCOMPARE(corrections.first().toMap().value(QStringLiteral("kind")).toString(), QStringLiteral("split"));
    // The split row is pre-highlighted; Enter takes it.
    key(Qt::Key_Return);
    QCOMPARE(tokens().size(), 2);
    QCOMPARE(text(), QStringLiteral("sunset people"));
    // A typo gets "did you mean" tag rows.
    type(QStringLiteral("sunsets"));
    key(Qt::Key_Return);
    QCOMPARE(tokens().size(), 2);
    const QVariantList fix = m_builder->property("corrections").toList();
    QVERIFY(!fix.isEmpty());
    QCOMPARE(fix.first().toMap().value(QStringLiteral("name")).toString(), QStringLiteral("sunset"));
    key(Qt::Key_Return);
    QCOMPARE(tokens().size(), 3);
    QCOMPARE(tokenName(2), QStringLiteral("sunset"));
}

QTEST_MAIN(TestQueryBuilder)
#include "tst_querybuilder.moc"
