// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QtTest>

#include <QList>
#include <QSet>
#include <QSignalSpy>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

#include "photo.h"
#include "photofiltermodel.h"
#include "photomodel.h"
#include "triagesession.h"

// TriageSession is the bookkeeping behind "Triage" mode: which photos a run
// covers (built from the gallery's filtered view and selection), where the user
// is in it, the recent tags, and the undo stack. It holds state only — the
// gallery performs the actions — so it runs headless over a PhotoModel +
// PhotoFilterModel pair, exactly as the filter tests do.
class TestTriageSession : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    // Queue construction (one rule shared by the header button and the context
    // menu — both call buildQueue with the gallery selection).
    void queueNothingSelectedTakesUntaggedInViewOrder();
    void queueOneSelectedTakesWholeView();
    void queueTwoOrMoreSelectedTakesSelectionInViewOrder();
    void queueFollowsViewSortAndFilter();

    // History / queue navigation
    void beginMakesFirstPhotoCurrent();
    void nextPullsFromQueueThenFinishes();
    void previousAndNextRetraceHistoryBeforeQueue();
    void progressAndPositionTrackTheWalk();

    // Recent tags
    void recentTagsKeepStableOrderAndAppend();
    void recentTagsEvictLeastRecentlyUsedIntoItsSlot();

    // Undo stack
    void undoStackIsLifoAndEmptyPopIsEmptyMap();

    // Trash bookkeeping
    void removeCurrentAdvancesAndShrinksTotal();
    void removeCurrentAtEndPullsFromQueueOrFinishes();
    void insertCurrentRestoresAsCurrent();

    // Session lifecycle
    void galleryStateRoundTripsUntouched();
    void endClearsEverything();
    void beginResetsTrashConfirmation();

private:
    static Photo makePhoto(int id, const QString &name, const QStringList &tagNames = {});
    static QList<int> toInts(const QVariantList &list);
    static QVariantList toVariants(const QList<int> &ids);

    PhotoModel *m_source = nullptr;
    PhotoFilterModel *m_view = nullptr;
    TriageSession *m_session = nullptr;
};

Photo TestTriageSession::makePhoto(int id, const QString &name, const QStringList &tagNames)
{
    Photo p;
    p.id = id;
    p.path = QStringLiteral("/p/") + name;
    p.fileName = name;
    p.tagNames = tagNames;
    for (int i = 0; i < tagNames.size(); ++i)
        p.tagIds.insert(100 + i); // ids are irrelevant here; the count is what matters
    return p;
}

QList<int> TestTriageSession::toInts(const QVariantList &list)
{
    QList<int> out;
    for (const QVariant &v : list)
        out.append(v.toInt());
    return out;
}

QVariantList TestTriageSession::toVariants(const QList<int> &ids)
{
    QVariantList out;
    for (int id : ids)
        out.append(id);
    return out;
}

void TestTriageSession::init()
{
    m_source = new PhotoModel;
    m_view = new PhotoFilterModel;
    m_view->setSourceModel(m_source);
    m_view->sort(0); // activate lessThan ordering (name, ascending)
    m_session = new TriageSession;

    // A small library: b and d are tagged, a, c and e are not. Name order is
    // a, b, c, d, e; the ids are deliberately out of that order.
    m_source->setPhotos({
        makePhoto(30, QStringLiteral("c.jpg")),
        makePhoto(10, QStringLiteral("a.jpg")),
        makePhoto(50, QStringLiteral("e.jpg")),
        makePhoto(20, QStringLiteral("b.jpg"), {QStringLiteral("dog")}),
        makePhoto(40, QStringLiteral("d.jpg"), {QStringLiteral("dog"), QStringLiteral("park")}),
    });
}

void TestTriageSession::cleanup()
{
    delete m_session;
    m_session = nullptr;
    delete m_view;
    m_view = nullptr;
    delete m_source;
    m_source = nullptr;
}

// --- Queue construction ------------------------------------------------------

void TestTriageSession::queueNothingSelectedTakesUntaggedInViewOrder()
{
    const QVariantList q = m_session->buildQueue(m_view, {});
    QCOMPARE(toInts(q), (QList<int>{10, 30, 50})); // a, c, e — the untagged ones, by name
}

void TestTriageSession::queueOneSelectedTakesWholeView()
{
    const QVariantList q = m_session->buildQueue(m_view, {40});
    QCOMPARE(toInts(q), (QList<int>{10, 20, 30, 40, 50})); // every photo, view order
}

void TestTriageSession::queueTwoOrMoreSelectedTakesSelectionInViewOrder()
{
    // Selection order is irrelevant; the queue follows the view's sort.
    const QVariantList q = m_session->buildQueue(m_view, {50, 20, 30});
    QCOMPARE(toInts(q), (QList<int>{20, 30, 50}));
    // A selected id that isn't in the view (pruned elsewhere) is simply absent.
    const QVariantList q2 = m_session->buildQueue(m_view, {50, 999});
    QCOMPARE(toInts(q2), (QList<int>{50}));
}

void TestTriageSession::queueFollowsViewSortAndFilter()
{
    // Descending name order reverses the queue.
    m_view->setSortAscending(false);
    QCOMPARE(toInts(m_session->buildQueue(m_view, {})), (QList<int>{50, 30, 10}));
    QCOMPARE(toInts(m_session->buildQueue(m_view, {40})), (QList<int>{50, 40, 30, 20, 10}));

    // The current filter scopes the queue: a search for "a" leaves only a.jpg in
    // the view, so "one selected → the whole view" is just that photo.
    m_view->setSortAscending(true);
    m_view->setSearchText(QStringLiteral("a."));
    QCOMPARE(toInts(m_session->buildQueue(m_view, {10})), (QList<int>{10}));
    QCOMPARE(toInts(m_session->buildQueue(m_view, {})), (QList<int>{10}));
}

// --- Navigation ---------------------------------------------------------------

void TestTriageSession::beginMakesFirstPhotoCurrent()
{
    QVERIFY(!m_session->active());
    QCOMPARE(m_session->currentId(), -1);

    m_session->begin(toVariants({1, 2, 3}), QStringLiteral("untagged in Timeline"), {});
    QVERIFY(m_session->active());
    QCOMPARE(m_session->currentId(), 1);
    QCOMPARE(m_session->position(), 1);
    QCOMPARE(m_session->total(), 3);
    QVERIFY(!m_session->finished());
    QVERIFY(!m_session->hasPrevious());
    QVERIFY(m_session->hasNext());
    QCOMPARE(m_session->description(), QStringLiteral("untagged in Timeline"));

    // An empty queue starts (and stays) finished.
    m_session->begin({}, QString(), {});
    QVERIFY(m_session->active());
    QVERIFY(m_session->finished());
    QCOMPARE(m_session->currentId(), -1);
    QCOMPARE(m_session->total(), 0);
}

void TestTriageSession::nextPullsFromQueueThenFinishes()
{
    m_session->begin(toVariants({1, 2, 3}), QString(), {});
    QVERIFY(m_session->next());
    QCOMPARE(m_session->currentId(), 2);
    QVERIFY(m_session->next());
    QCOMPARE(m_session->currentId(), 3);
    QVERIFY(!m_session->hasNext());

    // One step past the last photo finishes the run; the photos stay in the
    // history (total unchanged) so the user can step back.
    QVERIFY(m_session->next());
    QVERIFY(m_session->finished());
    QCOMPARE(m_session->currentId(), -1);
    QCOMPARE(m_session->total(), 3);
    QCOMPARE(m_session->position(), 3);
    QVERIFY(!m_session->next()); // nowhere further to go
    QVERIFY(m_session->previous());
    QCOMPARE(m_session->currentId(), 3);
    QVERIFY(!m_session->finished());
}

void TestTriageSession::previousAndNextRetraceHistoryBeforeQueue()
{
    m_session->begin(toVariants({1, 2, 3, 4}), QString(), {});
    m_session->next(); // 2
    m_session->next(); // 3
    QVERIFY(m_session->previous());
    QCOMPARE(m_session->currentId(), 2);
    QVERIFY(m_session->previous());
    QCOMPARE(m_session->currentId(), 1);
    QVERIFY(!m_session->previous()); // at the first photo
    QCOMPARE(m_session->currentId(), 1);

    // Forward walks the history (2, 3) before pulling 4 from the queue.
    QVERIFY(m_session->next());
    QCOMPARE(m_session->currentId(), 2);
    QVERIFY(m_session->next());
    QCOMPARE(m_session->currentId(), 3);
    QVERIFY(m_session->next());
    QCOMPARE(m_session->currentId(), 4);
    QCOMPARE(m_session->total(), 4);
}

void TestTriageSession::progressAndPositionTrackTheWalk()
{
    m_session->begin(toVariants({1, 2, 3, 4}), QString(), {});
    QSignalSpy spy(m_session, &TriageSession::positionChanged);
    QCOMPARE(m_session->position(), 1);
    QCOMPARE(m_session->progress(), 0.0);
    m_session->next();
    QCOMPARE(m_session->position(), 2);
    QCOMPARE(m_session->progress(), 0.25);
    m_session->next();
    m_session->next();
    QCOMPARE(m_session->position(), 4);
    QCOMPARE(m_session->progress(), 0.75);
    m_session->next(); // finished
    QCOMPARE(m_session->position(), 4);
    QCOMPARE(m_session->progress(), 1.0);
    QCOMPARE(spy.count(), 4); // the header/footer bindings refresh on every move
}

// --- Recent tags --------------------------------------------------------------

void TestTriageSession::recentTagsKeepStableOrderAndAppend()
{
    m_session->begin(toVariants({1}), QString(), {});
    m_session->recordTag(QStringLiteral("beach"));
    m_session->recordTag(QStringLiteral("sunset"));
    m_session->recordTag(QStringLiteral("family"));
    // New tags append in the order first used — the Ctrl-numbers are 1=beach,
    // 2=sunset, 3=family and stay that way.
    QCOMPARE(m_session->recentTags(),
             (QStringList{QStringLiteral("beach"), QStringLiteral("sunset"), QStringLiteral("family")}));

    // Re-applying a tag (even with different casing) does NOT move it — its slot
    // and number hold still, so a queued Ctrl+1/2/3 keeps hitting the same tags.
    // Blanks are ignored.
    m_session->recordTag(QStringLiteral("Beach"));
    m_session->recordTag(QStringLiteral("   "));
    QCOMPARE(m_session->recentTags(),
             (QStringList{QStringLiteral("beach"), QStringLiteral("sunset"), QStringLiteral("family")}));

    // A genuinely new tag still appends at the end.
    m_session->recordTag(QStringLiteral("hike"));
    QCOMPARE(m_session->recentTags(),
             (QStringList{QStringLiteral("beach"), QStringLiteral("sunset"),
                          QStringLiteral("family"), QStringLiteral("hike")}));
}

void TestTriageSession::recentTagsEvictLeastRecentlyUsedIntoItsSlot()
{
    QCOMPARE(TriageSession::kRecentCap, 9); // Ctrl+1–9
    m_session->begin(toVariants({1}), QString(), {});
    for (int i = 1; i <= 9; ++i)
        m_session->recordTag(QStringLiteral("t%1").arg(i)); // fills slots 1..9 in order

    QStringList expected;
    for (int i = 1; i <= 9; ++i)
        expected << QStringLiteral("t%1").arg(i);
    QCOMPARE(m_session->recentTags(), expected);

    // Re-apply t2, so t1 is now the least-recently-used tag (t2's recency jumps
    // ahead of it) — without moving anything.
    m_session->recordTag(QStringLiteral("t2"));
    QCOMPARE(m_session->recentTags(), expected); // order unchanged

    // A 10th distinct tag evicts the LRU (t1) and takes its slot, so every other
    // tag keeps its number; only slot 1 changes meaning.
    m_session->recordTag(QStringLiteral("t10"));
    expected[0] = QStringLiteral("t10");
    QCOMPARE(m_session->recentTags(), expected);
    QCOMPARE(m_session->recentTags().size(), 9);

    // The next distinct tag evicts the new LRU. t3 is now oldest (t2 was bumped,
    // t1 is gone, t10 is newest), so t11 lands in t3's slot (index 2).
    m_session->recordTag(QStringLiteral("t11"));
    expected[2] = QStringLiteral("t11");
    QCOMPARE(m_session->recentTags(), expected);
}

// --- Undo stack ---------------------------------------------------------------

void TestTriageSession::undoStackIsLifoAndEmptyPopIsEmptyMap()
{
    m_session->begin(toVariants({1}), QString(), {});
    QCOMPARE(m_session->undoCount(), 0);
    QVERIFY(m_session->popUndo().isEmpty());

    m_session->pushUndo({{QStringLiteral("kind"), QStringLiteral("apply")}, {QStringLiteral("tag"), 7}});
    m_session->pushUndo({{QStringLiteral("kind"), QStringLiteral("favorite")}});
    QCOMPARE(m_session->undoCount(), 2);

    QVariantMap step = m_session->popUndo();
    QCOMPARE(step.value(QStringLiteral("kind")).toString(), QStringLiteral("favorite"));
    step = m_session->popUndo();
    QCOMPARE(step.value(QStringLiteral("kind")).toString(), QStringLiteral("apply"));
    QCOMPARE(step.value(QStringLiteral("tag")).toInt(), 7);
    QCOMPARE(m_session->undoCount(), 0);
    QVERIFY(m_session->popUndo().isEmpty());
}

// --- Trash bookkeeping --------------------------------------------------------

void TestTriageSession::removeCurrentAdvancesAndShrinksTotal()
{
    m_session->begin(toVariants({1, 2, 3}), QString(), {});
    QCOMPARE(m_session->removeCurrent(), 1);
    QCOMPARE(m_session->currentId(), 2); // what followed is now current
    QCOMPARE(m_session->total(), 2);
    QCOMPARE(m_session->position(), 1);
    QVERIFY(!m_session->hasPrevious()); // the trashed photo is gone from the history too
}

void TestTriageSession::removeCurrentAtEndPullsFromQueueOrFinishes()
{
    // Removing the last visited photo pulls the next queued one into its place …
    m_session->begin(toVariants({1, 2}), QString(), {});
    QCOMPARE(m_session->removeCurrent(), 1);
    QCOMPARE(m_session->currentId(), 2);
    QCOMPARE(m_session->total(), 1);
    // … and with nothing queued the session finishes.
    QCOMPARE(m_session->removeCurrent(), 2);
    QVERIFY(m_session->finished());
    QCOMPARE(m_session->currentId(), -1);
    QCOMPARE(m_session->total(), 0);
    QCOMPARE(m_session->removeCurrent(), -1); // nothing current
}

void TestTriageSession::insertCurrentRestoresAsCurrent()
{
    m_session->begin(toVariants({1, 2, 3}), QString(), {});
    m_session->next(); // on 2
    QCOMPARE(m_session->removeCurrent(), 2);
    QCOMPARE(m_session->currentId(), 3);

    // The restored row comes back under a new id, ahead of what's current now.
    m_session->insertCurrent(22);
    QCOMPARE(m_session->currentId(), 22);
    QCOMPARE(m_session->total(), 3);
    QCOMPARE(m_session->position(), 2);
    QVERIFY(m_session->next());
    QCOMPARE(m_session->currentId(), 3);
    QVERIFY(m_session->previous());
    QCOMPARE(m_session->currentId(), 22);
    QVERIFY(m_session->previous());
    QCOMPARE(m_session->currentId(), 1);

    // Restoring into a finished session makes it the current photo again.
    m_session->begin(toVariants({5}), QString(), {});
    QCOMPARE(m_session->removeCurrent(), 5);
    QVERIFY(m_session->finished());
    m_session->insertCurrent(55);
    QVERIFY(!m_session->finished());
    QCOMPARE(m_session->currentId(), 55);
}

// --- Lifecycle ----------------------------------------------------------------

void TestTriageSession::galleryStateRoundTripsUntouched()
{
    // The gallery stashes its selection and scroll position at entry and reads
    // them back at exit; nothing the session does in between may alter them.
    const QVariantMap state{
        {QStringLiteral("selectedIds"), QVariantList{10, 20}},
        {QStringLiteral("anchorIndex"), 1},
        {QStringLiteral("scrollY"), 1234.5},
    };
    m_session->begin(toVariants({1, 2, 3}), QString(), state);
    m_session->next();
    m_session->recordTag(QStringLiteral("x"));
    m_session->pushUndo({{QStringLiteral("kind"), QStringLiteral("apply")}});
    m_session->removeCurrent();
    m_session->insertCurrent(9);
    m_session->setTrashConfirmed(true);
    QCOMPARE(m_session->galleryState(), state);
    QCOMPARE(m_session->galleryState().value(QStringLiteral("selectedIds")).toList(), (QVariantList{10, 20}));
    QCOMPARE(m_session->galleryState().value(QStringLiteral("scrollY")).toDouble(), 1234.5);
}

void TestTriageSession::endClearsEverything()
{
    m_session->begin(toVariants({1, 2}), QStringLiteral("desc"),
                     {{QStringLiteral("scrollY"), 10}});
    m_session->recordTag(QStringLiteral("x"));
    m_session->pushUndo({{QStringLiteral("kind"), QStringLiteral("apply")}});
    QSignalSpy spy(m_session, &TriageSession::activeChanged);
    m_session->end();
    QCOMPARE(spy.count(), 1);
    QVERIFY(!m_session->active());
    QCOMPARE(m_session->currentId(), -1);
    QCOMPARE(m_session->total(), 0);
    QCOMPARE(m_session->position(), 0);
    QCOMPARE(m_session->progress(), 0.0);
    QVERIFY(m_session->recentTags().isEmpty());
    QCOMPARE(m_session->undoCount(), 0);
    QVERIFY(m_session->description().isEmpty());
    QVERIFY(m_session->galleryState().isEmpty());
    QVERIFY(!m_session->hasPrevious());
    QVERIFY(!m_session->hasNext());
    QVERIFY(!m_session->finished()); // inactive is not "finished"
}

void TestTriageSession::beginResetsTrashConfirmation()
{
    m_session->begin(toVariants({1}), QString(), {});
    QVERIFY(!m_session->trashConfirmed());
    m_session->setTrashConfirmed(true);
    QVERIFY(m_session->trashConfirmed());
    m_session->begin(toVariants({2}), QString(), {}); // a new session asks again
    QVERIFY(!m_session->trashConfirmed());
    // A fresh session also starts with no recent tags or undo steps.
    QVERIFY(m_session->recentTags().isEmpty());
    QCOMPARE(m_session->undoCount(), 0);
}

QTEST_GUILESS_MAIN(TestTriageSession)
#include "tst_triagesession.moc"
