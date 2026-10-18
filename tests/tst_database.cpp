// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QtTest>

#include <QHash>
#include <QSet>
#include <QSqlDatabase>
#include <QString>
#include <QTemporaryFile>

#include "database.h"

// The SQLite layer holds every persistence rule in the app — tag identity,
// content-addressed tag survival, implication closure, albums, and the
// active-folder set. Each test runs against its own isolated in-memory database.
class TestDatabase : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    // Tags
    void ensureTagIsIdempotentAndCaseInsensitive();
    void tagIdByNameMatchesCaseInsensitively();
    void renameTag();
    void linkAndUnlinkPhotoTag();
    void deleteTagCascadesLinks();

    // Albums (reserved tag category)
    void favoritesAlbumExistsAndSortsFirst();
    void createAlbumReusesByNameAndRejectsBlank();

    // Smart albums (saved queries)
    void createSmartAlbumValidatesAndRoundTrips();
    void updateAndDeleteSmartAlbum();

    // Implications
    void listedImplicationsNeedActiveTags();
    void implicationCycleDetection();
    void applyImplicationsMaterializesClosure();
    void expandAllImplicationsResolvesChains();

    // Content-addressed tags
    void linkTagToContentTagsEveryCopy();
    void contentTagsSurviveReimport();
    void forgetContentTagDropsOrphanedArchive();

    // Folders / active set
    void orphanPhotosAreActive();
    void disabledFolderHidesItsPhotos();
    void deletePhotosUnderPathRemovesSubtree();
    void activePhotoPathsFilteredByTags();

    // Aggregation queries (sidebar / manager reads)
    void loadTagsListsActiveTagsWithTotalCount();
    void activeTagsCountsOverActiveSetOnly();
    void activeTagsFolderScopeRestrictsToSubtree();
    void coOccurringTagsNarrowsBySelection();
    void loadFoldersCountsSubtreePhotos();
    void loadCategoriesActiveOnlyAndSystemExclusion();

    // Path prefix-boundary cases (a sibling path must not be treated as nested)
    void activeSetHonorsFolderPrefixBoundary();
    void deletePhotosUnderPathHonorsPrefixBoundary();
    void orphanCountHonorsPrefixBoundary();
    void deleteOrphanPhotosKeepsContentArchive();

    // Guard / rejection paths
    void renameTagNameCollisionIsNoop();
    void insertCategoryRejectsDuplicateName();
    void contentTagOpsNoopWithoutHash();

private:
    // Map a tag list to {name: count} for order-independent assertions.
    static QHash<QString, int> countsByName(const QVector<TagInfo> &tags);

    Database *m_db = nullptr;
    QString m_conn;
};

QHash<QString, int> TestDatabase::countsByName(const QVector<TagInfo> &tags)
{
    QHash<QString, int> m;
    for (const TagInfo &t : tags)
        m.insert(t.name, t.count);
    return m;
}

void TestDatabase::init()
{
    // A per-test connection name + ":memory:" gives each test a private, empty
    // database with the full schema created by open().
    m_conn = QStringLiteral("test_%1").arg(QString::fromLatin1(QTest::currentTestFunction()));
    m_db = new Database;
    QVERIFY(m_db->open(QStringLiteral(":memory:"), m_conn));
}

void TestDatabase::cleanup()
{
    // Destroy the Database (releasing its QSqlDatabase member) before removing the
    // connection, or Qt warns that the connection is still in use.
    delete m_db;
    m_db = nullptr;
    QSqlDatabase::removeDatabase(m_conn);
}

// --- Tags -------------------------------------------------------------------

void TestDatabase::ensureTagIsIdempotentAndCaseInsensitive()
{
    const int id = m_db->ensureTag(QStringLiteral("Beach"));
    QVERIFY(id > 0);
    // Same name and a case variant both resolve to the one row (name is NOCASE).
    QCOMPARE(m_db->ensureTag(QStringLiteral("Beach")), id);
    QCOMPARE(m_db->ensureTag(QStringLiteral("beach")), id);
    QCOMPARE(m_db->ensureTag(QStringLiteral("BEACH")), id);
    QCOMPARE(m_db->tagName(id), QStringLiteral("Beach"));
}

void TestDatabase::tagIdByNameMatchesCaseInsensitively()
{
    const int id = m_db->ensureTag(QStringLiteral("Sunset"));
    QCOMPARE(m_db->tagIdByName(QStringLiteral("sunset")), id);
    QCOMPARE(m_db->tagIdByName(QStringLiteral("SUNSET")), id);
    QCOMPARE(m_db->tagIdByName(QStringLiteral("nope")), -1);
}

void TestDatabase::renameTag()
{
    const int id = m_db->ensureTag(QStringLiteral("old"));
    m_db->renameTag(id, QStringLiteral("new"));
    QCOMPARE(m_db->tagName(id), QStringLiteral("new"));
    QCOMPARE(m_db->tagIdByName(QStringLiteral("old")), -1);
    QCOMPARE(m_db->tagIdByName(QStringLiteral("new")), id);
}

void TestDatabase::linkAndUnlinkPhotoTag()
{
    const int photo = m_db->insertPhoto(QStringLiteral("/p/1.jpg"), QString());
    const int tag = m_db->ensureTag(QStringLiteral("dog"));
    QVERIFY(photo > 0 && tag > 0);

    QVERIFY(m_db->linkPhotoTag(photo, tag));
    QVERIFY(m_db->photoTagIds(photo).contains(tag));
    // Linking twice is harmless (INSERT OR IGNORE) and keeps a single row.
    QVERIFY(m_db->linkPhotoTag(photo, tag));
    QCOMPARE(m_db->photoTagIds(photo).size(), 1);

    QVERIFY(m_db->unlinkPhotoTag(photo, tag));
    QVERIFY(!m_db->photoTagIds(photo).contains(tag));
}

void TestDatabase::deleteTagCascadesLinks()
{
    const int photo = m_db->insertPhoto(QStringLiteral("/p/1.jpg"), QString());
    const int tag = m_db->ensureTag(QStringLiteral("temp"));
    m_db->linkPhotoTag(photo, tag);
    QVERIFY(m_db->photoTagIds(photo).contains(tag));

    m_db->deleteTag(tag);
    QCOMPARE(m_db->tagIdByName(QStringLiteral("temp")), -1);
    QVERIFY(!m_db->photoTagIds(photo).contains(tag)); // photo_tags row cascaded away
}

// --- Albums -----------------------------------------------------------------

void TestDatabase::favoritesAlbumExistsAndSortsFirst()
{
    QVERIFY(m_db->favoritesTagId() > 0);
    QVERIFY(m_db->albumCategoryId() > 0);

    const QVector<AlbumInfo> albums = m_db->albumTags();
    QVERIFY(!albums.isEmpty());
    QVERIFY(albums.first().favorite);
    QCOMPARE(albums.first().name, QStringLiteral("Favorites"));
    QCOMPARE(albums.first().id, m_db->favoritesTagId());
}

void TestDatabase::createAlbumReusesByNameAndRejectsBlank()
{
    const int id = m_db->createAlbum(QStringLiteral("Vacation"));
    QVERIFY(id > 0);
    // Reuse an existing album of the same display name rather than duplicating it.
    QCOMPARE(m_db->createAlbum(QStringLiteral("Vacation")), id);
    // Blank names are rejected.
    QCOMPARE(m_db->createAlbum(QStringLiteral("   ")), -1);

    // The new album appears after Favorites, which always sorts first.
    const QVector<AlbumInfo> albums = m_db->albumTags();
    QCOMPARE(albums.size(), 2);
    QVERIFY(albums.first().favorite);
    QCOMPARE(albums.at(1).name, QStringLiteral("Vacation"));
    QVERIFY(!albums.at(1).favorite);

    // The album tag is hidden from the normal tag lists (reserved category), so
    // its display name does not leak into autocomplete as a user tag.
    QVERIFY(!m_db->allTagNames().contains(QStringLiteral("Vacation")));
}

// --- Smart albums -----------------------------------------------------------

void TestDatabase::createSmartAlbumValidatesAndRoundTrips()
{
    const int t1 = m_db->ensureTag(QStringLiteral("beach"));
    const int t2 = m_db->ensureTag(QStringLiteral("summer"));

    // Blank name or empty tag set is rejected.
    QCOMPARE(m_db->createSmartAlbum(QStringLiteral("  "), {t1}, true), -1);
    QCOMPARE(m_db->createSmartAlbum(QStringLiteral("Empty"), {}, true), -1);

    const int id = m_db->createSmartAlbum(QStringLiteral("Trips"), {t1, t2}, true);
    QVERIFY(id > 0);
    // A duplicate name (NOCASE) is rejected.
    QCOMPARE(m_db->createSmartAlbum(QStringLiteral("trips"), {t1}, false), -1);

    const SmartAlbumInfo sa = m_db->smartAlbum(id);
    QCOMPARE(sa.id, id);
    QCOMPARE(sa.name, QStringLiteral("Trips"));
    QVERIFY(sa.matchAll);
    QCOMPARE(QSet<int>(sa.tagIds.begin(), sa.tagIds.end()), (QSet<int>{t1, t2}));
}

void TestDatabase::updateAndDeleteSmartAlbum()
{
    const int t1 = m_db->ensureTag(QStringLiteral("a"));
    const int t2 = m_db->ensureTag(QStringLiteral("b"));
    const int id = m_db->createSmartAlbum(QStringLiteral("One"), {t1, t2}, true);
    QVERIFY(id > 0);

    QVERIFY(m_db->updateSmartAlbum(id, QStringLiteral("Renamed"), {t1}, false));
    const SmartAlbumInfo sa = m_db->smartAlbum(id);
    QCOMPARE(sa.name, QStringLiteral("Renamed"));
    QVERIFY(!sa.matchAll);
    QCOMPARE(sa.tagIds, QList<int>{t1}); // membership fully replaced

    // Invalid updates are refused and leave the row untouched.
    QVERIFY(!m_db->updateSmartAlbum(id, QStringLiteral(""), {t1}, false));
    QVERIFY(!m_db->updateSmartAlbum(id, QStringLiteral("Renamed"), {}, false));

    m_db->deleteSmartAlbum(id);
    QVERIFY(m_db->smartAlbums().isEmpty());
}

// --- Implications -----------------------------------------------------------

void TestDatabase::listedImplicationsNeedActiveTags()
{
    const int beach = m_db->ensureTag(QStringLiteral("beach"));
    const int vacation = m_db->ensureTag(QStringLiteral("vacation"));

    // An implication whose tags apply to no active photo is not listed.
    m_db->addImplication(beach, vacation);
    QVERIFY(m_db->implications().isEmpty());

    // Put both tags on an active (orphan) photo; now the rule is visible.
    const int photo = m_db->insertPhoto(QStringLiteral("/p/1.jpg"), QString());
    m_db->linkPhotoTag(photo, beach);
    m_db->linkPhotoTag(photo, vacation);

    const QVector<ImplicationInfo> imps = m_db->implications();
    QCOMPARE(imps.size(), 1);
    QCOMPARE(imps.first().antecedentName, QStringLiteral("beach"));
    QCOMPARE(imps.first().consequentName, QStringLiteral("vacation"));
}

void TestDatabase::implicationCycleDetection()
{
    const int a = m_db->ensureTag(QStringLiteral("a"));
    const int b = m_db->ensureTag(QStringLiteral("b"));
    const int c = m_db->ensureTag(QStringLiteral("c"));
    m_db->addImplication(a, b);
    m_db->addImplication(b, c);

    // a -> b -> c already exists, so adding c -> a would close a cycle.
    QVERIFY(m_db->implicationWouldCycle(c, a));
    QVERIFY(m_db->implicationWouldCycle(b, a));   // b -> ... -> a? a reaches b, so yes
    // Adding a -> c just shortcuts an existing chain; no cycle.
    QVERIFY(!m_db->implicationWouldCycle(a, c));
    // A self-implication is always a cycle.
    QVERIFY(m_db->implicationWouldCycle(a, a));
}

void TestDatabase::applyImplicationsMaterializesClosure()
{
    const int a = m_db->ensureTag(QStringLiteral("a"));
    const int b = m_db->ensureTag(QStringLiteral("b"));
    const int c = m_db->ensureTag(QStringLiteral("c"));
    m_db->addImplication(a, b);
    m_db->addImplication(b, c);

    const QString hash = QStringLiteral("HASH_APPLY");
    const int photo = m_db->insertPhoto(QStringLiteral("/p/1.jpg"), hash);
    m_db->linkPhotoTag(photo, a);

    // Applying the closure of 'a' onto the content pulls in b and c.
    m_db->applyImplications(hash, a);
    const QSet<int> ids = m_db->photoTagIds(photo);
    QVERIFY(ids.contains(a));
    QVERIFY(ids.contains(b));
    QVERIFY(ids.contains(c));

    // The closure is archived too, so a re-import of the same content carries it.
    const int reimport = m_db->insertPhoto(QStringLiteral("/p/copy.jpg"), hash);
    m_db->applyContentTags(reimport, hash);
    const QSet<int> copyIds = m_db->photoTagIds(reimport);
    QVERIFY(copyIds.contains(b));
    QVERIFY(copyIds.contains(c));
}

void TestDatabase::expandAllImplicationsResolvesChains()
{
    const int a = m_db->ensureTag(QStringLiteral("a"));
    const int b = m_db->ensureTag(QStringLiteral("b"));
    const int c = m_db->ensureTag(QStringLiteral("c"));

    const int photo = m_db->insertPhoto(QStringLiteral("/p/1.jpg"), QString());
    m_db->linkPhotoTag(photo, a); // only the antecedent of the chain

    // Add the chain after the photo was tagged, then run the full-library pass.
    m_db->addImplication(a, b);
    m_db->addImplication(b, c);
    const int added = m_db->expandAllImplications();
    QVERIFY(added >= 2); // at least b and c onto the photo

    const QSet<int> ids = m_db->photoTagIds(photo);
    QVERIFY(ids.contains(b));
    QVERIFY(ids.contains(c));

    // A second pass is a no-op once the closure is materialized.
    QCOMPARE(m_db->expandAllImplications(), 0);
}

// --- Content-addressed tags -------------------------------------------------

void TestDatabase::linkTagToContentTagsEveryCopy()
{
    const QString hash = QStringLiteral("HASH_DUP");
    const int p1 = m_db->insertPhoto(QStringLiteral("/a/1.jpg"), hash);
    const int p2 = m_db->insertPhoto(QStringLiteral("/b/1.jpg"), hash); // identical content
    const int tag = m_db->ensureTag(QStringLiteral("shared"));

    // Tagging one copy by content tags every live copy of that content.
    m_db->linkTagToContent(p1, tag);
    QVERIFY(m_db->photoTagIds(p1).contains(tag));
    QVERIFY(m_db->photoTagIds(p2).contains(tag));

    m_db->unlinkTagFromContent(p1, tag);
    QVERIFY(!m_db->photoTagIds(p1).contains(tag));
    QVERIFY(!m_db->photoTagIds(p2).contains(tag));
}

void TestDatabase::contentTagsSurviveReimport()
{
    const QString hash = QStringLiteral("HASH_SURVIVE");
    const int tag = m_db->ensureTag(QStringLiteral("keepme"));

    const int p1 = m_db->insertPhoto(QStringLiteral("/a/1.jpg"), hash);
    m_db->linkPhotoTag(p1, tag);
    m_db->rememberContentTag(hash, tag); // archive against the content

    // Delete the photo (e.g. the file was removed); the archive is kept.
    m_db->deletePhoto(p1);

    // Re-import identical content at a new path and replay the archive.
    const int p2 = m_db->insertPhoto(QStringLiteral("/elsewhere/1.jpg"), hash);
    m_db->applyContentTags(p2, hash);
    QVERIFY(m_db->photoTagIds(p2).contains(tag));
}

void TestDatabase::forgetContentTagDropsOrphanedArchive()
{
    const QString hash = QStringLiteral("HASH_FORGET");
    const int tag = m_db->ensureTag(QStringLiteral("gone"));

    const int p1 = m_db->insertPhoto(QStringLiteral("/a/1.jpg"), hash);
    m_db->linkPhotoTag(p1, tag);
    m_db->rememberContentTag(hash, tag);

    // While a live photo still carries the tag, forget is a no-op.
    m_db->forgetContentTag(hash, tag);
    const int probe = m_db->insertPhoto(QStringLiteral("/probe/1.jpg"), hash);
    m_db->applyContentTags(probe, hash);
    QVERIFY(m_db->photoTagIds(probe).contains(tag));
    m_db->deletePhoto(probe);

    // Untag the last live copy, then forget: the archive entry is dropped.
    m_db->unlinkPhotoTag(p1, tag);
    m_db->forgetContentTag(hash, tag);

    const int p2 = m_db->insertPhoto(QStringLiteral("/b/1.jpg"), hash);
    m_db->applyContentTags(p2, hash);
    QVERIFY(!m_db->photoTagIds(p2).contains(tag)); // archive no longer carries it
}

// --- Folders / active set ---------------------------------------------------

void TestDatabase::orphanPhotosAreActive()
{
    // A photo under no registered folder is an orphan and always active.
    const int photo = m_db->insertPhoto(QStringLiteral("/loose/1.jpg"), QString());
    QVERIFY(photo > 0);
    QCOMPARE(m_db->activePhotoPaths(), QStringList{QStringLiteral("/loose/1.jpg")});
    QCOMPARE(m_db->orphanPhotoCount(), 1);
}

void TestDatabase::disabledFolderHidesItsPhotos()
{
    const int folder = m_db->insertFolder(QStringLiteral("/lib"));
    QVERIFY(folder > 0);
    m_db->insertPhoto(QStringLiteral("/lib/a.jpg"), QString());
    m_db->insertPhoto(QStringLiteral("/lib/sub/b.jpg"), QString()); // nested under the folder

    // Enabled by default: both photos are active.
    QCOMPARE(m_db->activePhotoPaths().size(), 2);
    QCOMPARE(m_db->orphanPhotoCount(), 0); // both are under a registered folder

    // Disabling the folder hides its whole subtree.
    m_db->setFolderEnabled(folder, false);
    QVERIFY(m_db->activePhotoPaths().isEmpty());

    // Re-enabling restores them.
    m_db->setFolderEnabled(folder, true);
    QCOMPARE(m_db->activePhotoPaths().size(), 2);
}

void TestDatabase::deletePhotosUnderPathRemovesSubtree()
{
    m_db->insertPhoto(QStringLiteral("/lib/a.jpg"), QString());
    m_db->insertPhoto(QStringLiteral("/lib/sub/b.jpg"), QString());
    const int outside = m_db->insertPhoto(QStringLiteral("/other/c.jpg"), QString());

    m_db->deletePhotosUnderPath(QStringLiteral("/lib"));

    const QStringList remaining = m_db->activePhotoPaths();
    QCOMPARE(remaining, QStringList{QStringLiteral("/other/c.jpg")});
    QVERIFY(outside > 0);
}

void TestDatabase::activePhotoPathsFilteredByTags()
{
    const int p1 = m_db->insertPhoto(QStringLiteral("/p/1.jpg"), QString());
    const int p2 = m_db->insertPhoto(QStringLiteral("/p/2.jpg"), QString());
    const int p3 = m_db->insertPhoto(QStringLiteral("/p/3.jpg"), QString());
    const int dog = m_db->ensureTag(QStringLiteral("dog"));
    const int park = m_db->ensureTag(QStringLiteral("park"));

    m_db->linkPhotoTag(p1, dog);
    m_db->linkPhotoTag(p2, dog);
    m_db->linkPhotoTag(p2, park);
    m_db->linkPhotoTag(p3, park);

    // match-any: carrying either tag is enough -> all three.
    QCOMPARE(m_db->activePhotoPaths({dog, park}, false).size(), 3);
    // match-all: must carry both -> only p2.
    QCOMPARE(m_db->activePhotoPaths({dog, park}, true),
             QStringList{QStringLiteral("/p/2.jpg")});
    // A single-tag filter.
    QCOMPARE(m_db->activePhotoPaths({dog}, false).size(), 2);
    // Empty selection falls back to the whole active set.
    QCOMPARE(m_db->activePhotoPaths({}, true).size(), 3);
}

// --- Aggregation queries ----------------------------------------------------

void TestDatabase::loadTagsListsActiveTagsWithTotalCount()
{
    // loadTags lists tags that apply to at least one ACTIVE photo, but reports the
    // tag's TOTAL count across all photos (so the delete confirmation is honest).
    const int folder = m_db->insertFolder(QStringLiteral("/lib"));
    const int active = m_db->insertPhoto(QStringLiteral("/lib/a.jpg"), QString());
    const int hidden = m_db->insertPhoto(QStringLiteral("/lib/b.jpg"), QString());

    const int shared = m_db->ensureTag(QStringLiteral("shared"));
    const int only = m_db->ensureTag(QStringLiteral("hiddenonly"));
    m_db->linkPhotoTag(active, shared);
    m_db->linkPhotoTag(hidden, shared);   // same tag on both photos
    m_db->linkPhotoTag(hidden, only);     // this tag lives only on the hidden photo

    // With the folder enabled both photos are active: both tags listed.
    QHash<QString, int> all = countsByName(m_db->loadTags());
    QCOMPARE(all.value(QStringLiteral("shared")), 2);
    QCOMPARE(all.value(QStringLiteral("hiddenonly")), 1);

    // Disable the folder: now neither photo is active, so no tags are listed.
    m_db->setFolderEnabled(folder, false);
    QVERIFY(m_db->loadTags().isEmpty());

    // Make only the first photo active again (as an orphan would be): "shared"
    // reappears with its full total count of 2, while "hiddenonly" stays hidden.
    m_db->relocatePhoto(active, QStringLiteral("/orphan/a.jpg"));
    QHash<QString, int> some = countsByName(m_db->loadTags());
    QCOMPARE(some.value(QStringLiteral("shared")), 2); // total, not active-count
    QVERIFY(!some.contains(QStringLiteral("hiddenonly")));
}

void TestDatabase::activeTagsCountsOverActiveSetOnly()
{
    // Unlike loadTags, activeTags counts only photos in the active set, and never
    // lists reserved (Favorites/album) tags.
    const int folder = m_db->insertFolder(QStringLiteral("/lib"));
    const int p1 = m_db->insertPhoto(QStringLiteral("/lib/a.jpg"), QString());   // active
    const int p2 = m_db->insertPhoto(QStringLiteral("/orphan/b.jpg"), QString()); // active (orphan)
    const int tag = m_db->ensureTag(QStringLiteral("x"));
    m_db->linkPhotoTag(p1, tag);
    m_db->linkPhotoTag(p2, tag);
    // Favorites is a reserved tag; it must never surface in activeTags.
    m_db->linkPhotoTag(p1, m_db->favoritesTagId());

    QHash<QString, int> counts = countsByName(m_db->activeTags());
    QCOMPARE(counts.value(QStringLiteral("x")), 2);
    QVERIFY(!counts.contains(QStringLiteral("Favorites")));

    // Disabling the folder drops p1 from the active set, so the active count falls
    // to 1 (only the orphan) — whereas loadTags would still report the total 2.
    m_db->setFolderEnabled(folder, false);
    QCOMPARE(countsByName(m_db->activeTags()).value(QStringLiteral("x")), 1);
    QCOMPARE(countsByName(m_db->loadTags()).value(QStringLiteral("x")), 2);
}

void TestDatabase::activeTagsFolderScopeRestrictsToSubtree()
{
    m_db->insertFolder(QStringLiteral("/lib"));
    const int inScope = m_db->insertPhoto(QStringLiteral("/lib/sub/a.jpg"), QString());
    const int sibling = m_db->insertPhoto(QStringLiteral("/lib/subextra/b.jpg"), QString());
    const int other = m_db->insertPhoto(QStringLiteral("/lib/other/c.jpg"), QString());
    m_db->linkPhotoTag(inScope, m_db->ensureTag(QStringLiteral("in")));
    m_db->linkPhotoTag(sibling, m_db->ensureTag(QStringLiteral("sib")));
    m_db->linkPhotoTag(other, m_db->ensureTag(QStringLiteral("out")));

    const QHash<QString, int> scoped = countsByName(m_db->activeTags(QStringLiteral("/lib/sub")));
    QVERIFY(scoped.contains(QStringLiteral("in")));
    // "/lib/subextra" shares the "/lib/sub" prefix but is NOT under it: the trailing
    // separator in the scope match keeps the sibling folder out.
    QVERIFY(!scoped.contains(QStringLiteral("sib")));
    QVERIFY(!scoped.contains(QStringLiteral("out")));
}

void TestDatabase::coOccurringTagsNarrowsBySelection()
{
    const int a = m_db->ensureTag(QStringLiteral("a"));
    const int b = m_db->ensureTag(QStringLiteral("b"));
    const int c = m_db->ensureTag(QStringLiteral("c"));
    const int p1 = m_db->insertPhoto(QStringLiteral("/p/1.jpg"), QString());
    const int p2 = m_db->insertPhoto(QStringLiteral("/p/2.jpg"), QString());
    const int p3 = m_db->insertPhoto(QStringLiteral("/p/3.jpg"), QString());
    m_db->linkPhotoTag(p1, a); m_db->linkPhotoTag(p1, b);
    m_db->linkPhotoTag(p2, a); m_db->linkPhotoTag(p2, c);
    m_db->linkPhotoTag(p3, a);

    // Selecting {a}: every photo carries a, so all three tags co-occur, counted
    // over the matching set (a on 3, b on 1, c on 1).
    QHash<QString, int> one = countsByName(m_db->coOccurringTags({a}));
    QCOMPARE(one.value(QStringLiteral("a")), 3);
    QCOMPARE(one.value(QStringLiteral("b")), 1);
    QCOMPARE(one.value(QStringLiteral("c")), 1);

    // Selecting {a, b}: only p1 carries both, so the co-occurring set narrows to
    // a and b (each on that single photo); c drops out entirely.
    QHash<QString, int> two = countsByName(m_db->coOccurringTags({a, b}));
    QCOMPARE(two.value(QStringLiteral("a")), 1);
    QCOMPARE(two.value(QStringLiteral("b")), 1);
    QVERIFY(!two.contains(QStringLiteral("c")));

    // An empty selection falls back to activeTags (counts over the whole set).
    QCOMPARE(countsByName(m_db->coOccurringTags({})).value(QStringLiteral("a")), 3);
}

void TestDatabase::loadFoldersCountsSubtreePhotos()
{
    const int lib = m_db->insertFolder(QStringLiteral("/lib"));
    m_db->insertPhoto(QStringLiteral("/lib/a.jpg"), QString());
    m_db->insertPhoto(QStringLiteral("/lib/sub/b.jpg"), QString());   // nested, counts
    m_db->insertPhoto(QStringLiteral("/libraryextra/c.jpg"), QString()); // sibling, must NOT count
    m_db->setFolderEnabled(lib, false);

    const QVector<FolderInfo> folders = m_db->loadFolders();
    QCOMPARE(folders.size(), 1);
    QCOMPARE(folders.first().path, QStringLiteral("/lib"));
    QCOMPARE(folders.first().count, 2); // /lib/a.jpg and /lib/sub/b.jpg only
    QVERIFY(!folders.first().enabled); // reflects the enabled flag
}

void TestDatabase::loadCategoriesActiveOnlyAndSystemExclusion()
{
    const int people = m_db->insertCategory(QStringLiteral("People"), 0);
    const int ghosts = m_db->insertCategory(QStringLiteral("Ghosts"), 1);
    QVERIFY(people > 0 && ghosts > 0);

    const int alice = m_db->ensureTag(QStringLiteral("alice"));
    const int boo = m_db->ensureTag(QStringLiteral("boo"));
    m_db->setTagCategory(alice, people);
    m_db->setTagCategory(boo, ghosts);

    const int active = m_db->insertPhoto(QStringLiteral("/orphan/a.jpg"), QString());
    m_db->linkPhotoTag(active, alice); // People has a tag on an active photo
    const int folder = m_db->insertFolder(QStringLiteral("/hidden"));
    const int buried = m_db->insertPhoto(QStringLiteral("/hidden/b.jpg"), QString());
    m_db->linkPhotoTag(buried, boo);   // Ghosts' only tag lives on a hidden photo
    m_db->setFolderEnabled(folder, false);

    // activeOnly keeps only categories with a tag on an active photo; the reserved
    // "Albums" category is always excluded.
    QHash<QString, int> active_m;
    for (const CategoryInfo &c : m_db->loadCategories(/*activeOnly=*/true))
        active_m.insert(c.name, c.tagCount);
    QVERIFY(active_m.contains(QStringLiteral("People")));
    QVERIFY(!active_m.contains(QStringLiteral("Ghosts")));
    QVERIFY(!active_m.contains(QStringLiteral("Albums")));
    QCOMPARE(active_m.value(QStringLiteral("People")), 1);

    // Without activeOnly, every user category is listed (still no system ones).
    QHash<QString, int> all_m;
    for (const CategoryInfo &c : m_db->loadCategories(/*activeOnly=*/false))
        all_m.insert(c.name, c.tagCount);
    QVERIFY(all_m.contains(QStringLiteral("People")));
    QVERIFY(all_m.contains(QStringLiteral("Ghosts")));
    QVERIFY(!all_m.contains(QStringLiteral("Albums")));
}

// --- Path prefix-boundary cases ---------------------------------------------

void TestDatabase::activeSetHonorsFolderPrefixBoundary()
{
    // A photo at "/libraryextra/..." shares the "/lib" prefix but is NOT under the
    // folder "/lib" — it is an orphan, governed by no folder's enabled flag.
    const int folder = m_db->insertFolder(QStringLiteral("/lib"));
    m_db->insertPhoto(QStringLiteral("/lib/a.jpg"), QString());
    m_db->insertPhoto(QStringLiteral("/libraryextra/b.jpg"), QString());

    QCOMPARE(m_db->activePhotoPaths().size(), 2);

    // Disabling "/lib" hides only its own subtree; the sibling-prefix orphan stays.
    m_db->setFolderEnabled(folder, false);
    QCOMPARE(m_db->activePhotoPaths(), QStringList{QStringLiteral("/libraryextra/b.jpg")});
}

void TestDatabase::deletePhotosUnderPathHonorsPrefixBoundary()
{
    m_db->insertPhoto(QStringLiteral("/lib/a.jpg"), QString());
    m_db->insertPhoto(QStringLiteral("/lib/sub/b.jpg"), QString());
    m_db->insertPhoto(QStringLiteral("/libraryextra/c.jpg"), QString()); // sibling prefix

    m_db->deletePhotosUnderPath(QStringLiteral("/lib"));

    // Only the real "/lib" subtree is gone; "/libraryextra" is untouched.
    QCOMPARE(m_db->activePhotoPaths(), QStringList{QStringLiteral("/libraryextra/c.jpg")});
}

void TestDatabase::orphanCountHonorsPrefixBoundary()
{
    m_db->insertFolder(QStringLiteral("/lib"));
    m_db->insertPhoto(QStringLiteral("/lib/a.jpg"), QString());        // under the folder
    m_db->insertPhoto(QStringLiteral("/libraryextra/b.jpg"), QString()); // sibling -> orphan
    m_db->insertPhoto(QStringLiteral("/elsewhere/c.jpg"), QString());  // orphan

    // The sibling-prefix photo is an orphan, not "under /lib".
    QCOMPARE(m_db->orphanPhotoCount(), 2);
}

void TestDatabase::deleteOrphanPhotosKeepsContentArchive()
{
    const QString hash = QStringLiteral("HASH_ORPHAN");
    const int tag = m_db->ensureTag(QStringLiteral("archived"));
    const int orphan = m_db->insertPhoto(QStringLiteral("/loose/1.jpg"), hash);
    m_db->linkPhotoTag(orphan, tag);
    m_db->rememberContentTag(hash, tag);

    QCOMPARE(m_db->orphanPhotoCount(), 1);
    m_db->deleteOrphanPhotos();
    QCOMPARE(m_db->orphanPhotoCount(), 0);

    // The content archive is keyed by hash, so re-importing the same content
    // restores the tag even though the orphan row was purged.
    const int reimport = m_db->insertPhoto(QStringLiteral("/loose/again.jpg"), hash);
    m_db->applyContentTags(reimport, hash);
    QVERIFY(m_db->photoTagIds(reimport).contains(tag));
}

// --- Guard / rejection paths ------------------------------------------------

void TestDatabase::renameTagNameCollisionIsNoop()
{
    const int keep = m_db->ensureTag(QStringLiteral("keep"));
    const int other = m_db->ensureTag(QStringLiteral("other"));

    // Renaming onto an existing name (NOCASE) violates UNIQUE and is a no-op: both
    // tags keep their names and ids.
    m_db->renameTag(other, QStringLiteral("KEEP"));
    QCOMPARE(m_db->tagName(other), QStringLiteral("other"));
    QCOMPARE(m_db->tagName(keep), QStringLiteral("keep"));
    QCOMPARE(m_db->tagIdByName(QStringLiteral("keep")), keep);
    QCOMPARE(m_db->tagIdByName(QStringLiteral("other")), other);
}

void TestDatabase::insertCategoryRejectsDuplicateName()
{
    const int first = m_db->insertCategory(QStringLiteral("Places"), 0);
    QVERIFY(first > 0);
    // A duplicate name (NOCASE) is rejected with -1; the original is untouched.
    QCOMPARE(m_db->insertCategory(QStringLiteral("places"), 2), -1);
    QCOMPARE(m_db->categoryIdByName(QStringLiteral("Places")), first);
}

void TestDatabase::contentTagOpsNoopWithoutHash()
{
    const int tag = m_db->ensureTag(QStringLiteral("t"));

    // Empty-hash archive operations are no-ops and must not throw or create rows.
    m_db->rememberContentTag(QString(), tag);
    m_db->forgetContentTag(QString(), tag);

    // linkTagToContent falls back to the single row when the photo has no hash.
    const int noHash1 = m_db->insertPhoto(QStringLiteral("/a/1.jpg"), QString());
    const int noHash2 = m_db->insertPhoto(QStringLiteral("/b/1.jpg"), QString());
    m_db->linkTagToContent(noHash1, tag);
    QVERIFY(m_db->photoTagIds(noHash1).contains(tag));
    QVERIFY(!m_db->photoTagIds(noHash2).contains(tag)); // not spread by content

    // applyContentTags with an empty hash does nothing.
    const int target = m_db->insertPhoto(QStringLiteral("/c/1.jpg"), QString());
    m_db->applyContentTags(target, QString());
    QVERIFY(m_db->photoTagIds(target).isEmpty());
}

QTEST_GUILESS_MAIN(TestDatabase)
#include "tst_database.moc"
