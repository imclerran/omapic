// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QtTest>

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

private:
    Database *m_db = nullptr;
    QString m_conn;
};

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

QTEST_GUILESS_MAIN(TestDatabase)
#include "tst_database.moc"
