// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QtTest>

#include <QDateTime>
#include <QHash>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QFile>
#include <QString>
#include <QTemporaryDir>
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
    void supersedeHashForwardsArchiveAndPrunesWhenUnreferenced();
    void hashFileSamplesLargeFilesAndFullHashesSmall();
    void photosNeedingResampleSelectsLargeLegacyHashes();
    void orphanHashLifecycleAcrossRemovalAndReimport();
    void sidecarDirtyQueueEnqueueDequeue();
    void sidecarTagsForPhotoClassifies();
    void activePhotoIdsExcludeDisabledFolders();
    void retentionPrunesOrphansOlderThanCutoff();
    void nameInputStripsPipe();

    // Folders / active set
    void orphanPhotosAreActive();
    void disabledFolderHidesItsPhotos();
    void deletePhotosUnderPathRemovesSubtree();
    void activePhotoPathsFilteredByTags();

    // Aggregation queries (sidebar / manager reads)
    void loadTagsListsActiveTagsWithTotalCount();
    void activeTagsCountsOverActiveSetOnly();
    void activeTagsFolderScopeRestrictsToSubtree();
    void tagSamplePathsScoreCandidatesOverActiveSet();
    void pickTagSamplesGreedyCoverageWithFavoriteBonus();
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
    void favoritesTagCannotBeDeleted();
    void addImplicationRejectsSelfAndNegative();
    void expandAllImplicationsResolvesDiamond();
    void removeImplicationStopsFuturePropagationOnly();
    void deleteTagCascadesImplications();

    // Tag aliases
    void resolveTagNameAndEnsureTagFollowAliases();
    void createAliasWithoutExistingTagAddsRedirectOnly();
    void createAliasMergesExistingTagAcrossAllTables();
    void createAliasRejectsSelfNameAndDuplicate();
    void removeAliasKeepsCanonicalTag();
    void deletingCanonicalTagCascadesAliases();
    void renameTagOntoAliasNameIsRejectedByDatabase();

    void linkPhotoTagFailsOnMissingForeignKey();
    void coOccurringTagsEmptyWhenNoCoOccurrence();

    // Triage undo primitives
    void applyTagRecordedReportsOnlyNewRows();
    void revertTagApplyKeepsPreexistingImpliedTags();
    void revertTagApplyCoversEveryContentCopy();
    void relinkAfterRemoveRestoresTheOneLink();
    void photoRowRestoreReattachesArchivedTags();

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

void TestDatabase::supersedeHashForwardsArchiveAndPrunesWhenUnreferenced()
{
    const QString oldHash = QStringLiteral("HASH_OLD");
    const QString newHash = QStringLiteral("HASH_NEW");
    const int tag = m_db->ensureTag(QStringLiteral("kept"));

    // An edited file: its old content had an archived tag, now gone from disk.
    m_db->rememberContentTag(oldHash, tag);
    m_db->supersedeHash(oldHash, newHash);

    // The tag carried forward to the new hash...
    const int pNew = m_db->insertPhoto(QStringLiteral("/n/1.jpg"), newHash);
    m_db->applyContentTags(pNew, newHash);
    QVERIFY(m_db->photoTagIds(pNew).contains(tag));
    // ...and the old archive is gone (nothing referenced it).
    const int pOld = m_db->insertPhoto(QStringLiteral("/o/1.jpg"), oldHash);
    m_db->applyContentTags(pOld, oldHash);
    QVERIFY(!m_db->photoTagIds(pOld).contains(tag));
    m_db->deletePhoto(pOld);
    m_db->deletePhoto(pNew);

    // Duplicate-twin case: a still-present copy keeps the old archive alive.
    const QString twinHash = QStringLiteral("HASH_TWIN");
    const QString twinNew = QStringLiteral("HASH_TWIN_EDIT");
    const int tag2 = m_db->ensureTag(QStringLiteral("twinTag"));
    const int twinA = m_db->insertPhoto(QStringLiteral("/t/a.jpg"), twinHash);
    m_db->insertPhoto(QStringLiteral("/t/b.jpg"), twinHash); // untouched twin
    m_db->rememberContentTag(twinHash, tag2);
    m_db->supersedeHash(twinHash, twinNew); // one copy edited

    const int probe = m_db->insertPhoto(QStringLiteral("/t/probe.jpg"), twinHash);
    m_db->applyContentTags(probe, twinHash);
    QVERIFY2(m_db->photoTagIds(probe).contains(tag2),
             "the untouched twin must keep the old content archive");
    Q_UNUSED(twinA);
}

void TestDatabase::orphanHashLifecycleAcrossRemovalAndReimport()
{
    const auto orphanCount = [this](const QString &hash) {
        QSqlQuery q(QSqlDatabase::database(m_conn));
        q.prepare(QStringLiteral("SELECT COUNT(*) FROM orphaned_hashes WHERE hash = ?"));
        q.addBindValue(hash);
        q.exec();
        return q.next() ? q.value(0).toInt() : -1;
    };

    const QString hash = QStringLiteral("HASH_ORPHAN");
    const int tag = m_db->ensureTag(QStringLiteral("remembered"));
    const int p = m_db->insertPhoto(QStringLiteral("/x/1.jpg"), hash);
    m_db->linkTagToContent(p, tag);
    m_db->rememberContentTag(hash, tag);
    QCOMPARE(orphanCount(hash), 0); // a live photo references it — not orphaned

    // Removing the last live photo leaves the archive but marks it orphaned.
    m_db->deletePhoto(p);
    QCOMPARE(orphanCount(hash), 1);

    // Re-importing a file with that content clears the orphan and reattaches tags.
    const int p2 = m_db->insertPhoto(QStringLiteral("/x/moved.jpg"), hash);
    m_db->applyContentTags(p2, hash);
    QCOMPARE(orphanCount(hash), 0);
    QVERIFY(m_db->photoTagIds(p2).contains(tag));
}

void TestDatabase::retentionPrunesOrphansOlderThanCutoff()
{
    const QString oldHash = QStringLiteral("OLD");
    const QString newHash = QStringLiteral("NEW");
    const int tagA = m_db->ensureTag(QStringLiteral("a"));
    const int tagB = m_db->ensureTag(QStringLiteral("b"));

    // Two removed files: archive a tag for each, then delete the photos so each
    // hash is orphaned.
    const int p1 = m_db->insertPhoto(QStringLiteral("/p/old.jpg"), oldHash);
    m_db->linkTagToContent(p1, tagA);
    m_db->rememberContentTag(oldHash, tagA);
    m_db->deletePhoto(p1);
    const int p2 = m_db->insertPhoto(QStringLiteral("/p/new.jpg"), newHash);
    m_db->linkTagToContent(p2, tagB);
    m_db->rememberContentTag(newHash, tagB);
    m_db->deletePhoto(p2);
    QCOMPARE(m_db->rememberedRemovedCount(), 2);

    // Backdate the "old" one to 100 days ago.
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    {
        QSqlQuery u(QSqlDatabase::database(m_conn));
        u.prepare(QStringLiteral("UPDATE orphaned_hashes SET orphaned_at = ? WHERE hash = ?"));
        u.addBindValue(now - qint64(100) * 24 * 60 * 60);
        u.addBindValue(oldHash);
        QVERIFY(u.exec());
    }

    // Prune everything orphaned more than 30 days ago.
    m_db->pruneOrphanedTags(now - qint64(30) * 24 * 60 * 60);

    QCOMPARE(m_db->rememberedRemovedCount(), 1); // only the recent one remains
    // The old archive is gone...
    const int pa = m_db->insertPhoto(QStringLiteral("/p/reold.jpg"), oldHash);
    m_db->applyContentTags(pa, oldHash);
    QVERIFY(!m_db->photoTagIds(pa).contains(tagA));
    // ...the recent one survives.
    const int pb = m_db->insertPhoto(QStringLiteral("/p/renew.jpg"), newHash);
    m_db->applyContentTags(pb, newHash);
    QVERIFY(m_db->photoTagIds(pb).contains(tagB));
}

void TestDatabase::nameInputStripsPipe()
{
    // '|' is forbidden in tag/category names (it's the sidecar hierarchy
    // separator); the backend converts it to '/' as a safety net behind the editor.
    const int tag = m_db->ensureTag(QStringLiteral("a|b"));
    QCOMPARE(m_db->tagName(tag), QStringLiteral("a/b"));
    m_db->renameTag(tag, QStringLiteral("c|d|e"));
    QCOMPARE(m_db->tagName(tag), QStringLiteral("c/d/e"));

    const int cat = m_db->insertCategory(QStringLiteral("p|q"), 0);
    QVERIFY(cat > 0);
    QCOMPARE(m_db->categoryName(cat), QStringLiteral("p/q"));
    m_db->renameCategory(cat, QStringLiteral("r|s"));
    QCOMPARE(m_db->categoryName(cat), QStringLiteral("r/s"));
}

void TestDatabase::sidecarDirtyQueueEnqueueDequeue()
{
    const int p1 = m_db->insertPhoto(QStringLiteral("/p/1.jpg"), QString());
    const int p2 = m_db->insertPhoto(QStringLiteral("/p/2.jpg"), QString());
    const int tag = m_db->ensureTag(QStringLiteral("beach"));
    QVERIFY(m_db->linkPhotoTag(p1, tag));
    QVERIFY(m_db->linkPhotoTag(p2, tag));

    QVERIFY(m_db->sidecarDirtyIds().isEmpty());
    QCOMPARE(m_db->sidecarDirtyCount(), 0);

    m_db->enqueueSidecarDirty({p1, p1}); // INSERT OR IGNORE coalesces the repeat
    QCOMPARE(m_db->sidecarDirtyIds(), QList<int>{p1});
    QCOMPARE(m_db->sidecarDirtyCount(), 1);

    // Enqueue every photo carrying a tag, for the library-wide rename/delete paths.
    m_db->enqueueSidecarDirtyForTag(tag);
    QCOMPARE(m_db->sidecarDirtyIds(), (QList<int>{p1, p2}));

    m_db->dequeueSidecarDirty(p1);
    QCOMPARE(m_db->sidecarDirtyIds(), QList<int>{p2});

    // Deleting a photo cascades its dirty row away.
    m_db->deletePhoto(p2);
    QVERIFY(m_db->sidecarDirtyIds().isEmpty());
}

void TestDatabase::sidecarTagsForPhotoClassifies()
{
    const int photo = m_db->insertPhoto(QStringLiteral("/p/x.jpg"), QStringLiteral("HASHX"));

    // A categorised tag, an uncategorised tag, an album, and Favorites.
    const int placesCat = m_db->insertCategory(QStringLiteral("Places"), 0);
    const int spain = m_db->ensureTag(QStringLiteral("Spain"));
    m_db->setTagCategory(spain, placesCat);
    const int loose = m_db->ensureTag(QStringLiteral("candid"));
    const int album = m_db->createAlbum(QStringLiteral("Trip"));
    for (int t : {spain, loose, album, m_db->favoritesTagId()})
        QVERIFY(m_db->linkPhotoTag(photo, t));

    QString hash;
    QStringList flat, albums;
    QList<QPair<QString, QString>> categorized;
    bool favorite = false;
    m_db->sidecarTagsForPhoto(photo, hash, flat, categorized, albums, favorite);

    QCOMPARE(hash, QStringLiteral("HASHX"));
    QVERIFY(favorite);
    QCOMPARE(albums, QStringList{QStringLiteral("Trip")}); // album display name, prefix stripped
    // Flat carries the user tags only (no album, no Favorites).
    QVERIFY(flat.contains(QStringLiteral("Spain")));
    QVERIFY(flat.contains(QStringLiteral("candid")));
    QCOMPARE(flat.size(), 2);
    QVERIFY(categorized.contains(
        QPair<QString, QString>(QStringLiteral("Places"), QStringLiteral("Spain"))));
    QVERIFY(categorized.contains(
        QPair<QString, QString>(QString(), QStringLiteral("candid"))));
}

void TestDatabase::activePhotoIdsExcludeDisabledFolders()
{
    // One enabled folder, one disabled, plus an orphan under no folder.
    const int enabledFolder = m_db->insertFolder(QStringLiteral("/lib/on"));
    const int disabledFolder = m_db->insertFolder(QStringLiteral("/lib/off"));
    QVERIFY(enabledFolder > 0 && disabledFolder > 0);
    m_db->setFolderEnabled(disabledFolder, false);

    const int shown = m_db->insertPhoto(QStringLiteral("/lib/on/a.jpg"), QString());
    const int hidden = m_db->insertPhoto(QStringLiteral("/lib/off/b.jpg"), QString());
    const int orphan = m_db->insertPhoto(QStringLiteral("/elsewhere/c.jpg"), QString());

    const QList<int> active = m_db->activePhotoIds();
    QVERIFY(active.contains(shown));
    QVERIFY(active.contains(orphan));
    QVERIFY(!active.contains(hidden)); // a disabled folder's photo is not active

    QVERIFY(m_db->isPhotoActive(shown));
    QVERIFY(m_db->isPhotoActive(orphan));
    QVERIFY(!m_db->isPhotoActive(hidden));

    // Re-enabling the folder brings its photo back into the active set.
    m_db->setFolderEnabled(disabledFolder, true);
    QVERIFY(m_db->isPhotoActive(hidden));
    QVERIFY(m_db->activePhotoIds().contains(hidden));
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

void TestDatabase::tagSamplePathsScoreCandidatesOverActiveSet()
{
    // The query half: candidates come from the active set only, carry the right
    // favorite flag and non-system tag set (the sampled tag itself left out), and
    // rank by tag count + the favorite bonus, newest first on ties.
    const int dog = m_db->ensureTag(QStringLiteral("dog"));
    const int park = m_db->ensureTag(QStringLiteral("park"));
    const int ball = m_db->ensureTag(QStringLiteral("ball"));
    const int album = m_db->createAlbum(QStringLiteral("Trip"));
    QVERIFY(album > 0);

    // A favorite with only the sampled tag: 0 tags + bonus 3.
    const int fav = m_db->insertPhoto(QStringLiteral("/p/fav.jpg"), QString(), 10);
    m_db->linkPhotoTag(fav, dog);
    m_db->linkPhotoTag(fav, m_db->favoritesTagId());
    // Two other tags (score 2), plus a byte-identical copy tagged the same way.
    const int rich = m_db->insertPhoto(QStringLiteral("/p/rich.jpg"), QStringLiteral("h-rich"), 20);
    const int dup = m_db->insertPhoto(QStringLiteral("/p/rich-copy.jpg"), QStringLiteral("h-rich"), 20);
    for (int id : {rich, dup}) {
        m_db->linkPhotoTag(id, dog);
        m_db->linkPhotoTag(id, park);
        m_db->linkPhotoTag(id, ball);
    }
    // One other tag each (score 1): the newer capture ranks first.
    const int newer = m_db->insertPhoto(QStringLiteral("/p/newer.jpg"), QString(), 300);
    const int older = m_db->insertPhoto(QStringLiteral("/p/older.jpg"), QString(), 200);
    for (int id : {newer, older}) {
        m_db->linkPhotoTag(id, dog);
        m_db->linkPhotoTag(id, park);
    }
    // Album membership is a hidden system tag: it counts for nothing (score 0),
    // so the newest photo of all still ranks last.
    const int inAlbum = m_db->insertPhoto(QStringLiteral("/p/album.jpg"), QString(), 900);
    m_db->linkPhotoTag(inAlbum, dog);
    m_db->linkPhotoTag(inAlbum, album);
    // Not a dog photo (even as a favorite): never a candidate for "dog".
    const int other = m_db->insertPhoto(QStringLiteral("/p/other.jpg"), QString(), 999);
    m_db->linkPhotoTag(other, park);
    m_db->linkPhotoTag(other, m_db->favoritesTagId());

    const QVector<TagSampleCandidate> pool = m_db->tagSampleCandidates(dog, 10);
    QStringList order;
    for (const TagSampleCandidate &c : pool)
        order.append(c.path);
    QCOMPARE(order, (QStringList{QStringLiteral("/p/fav.jpg"), QStringLiteral("/p/rich.jpg"),
                                 QStringLiteral("/p/rich-copy.jpg"), QStringLiteral("/p/newer.jpg"),
                                 QStringLiteral("/p/older.jpg"), QStringLiteral("/p/album.jpg")}));
    QVERIFY(pool[0].favorite);
    QVERIFY(pool[0].tagIds.isEmpty());          // "dog" itself and Favorites are left out
    QCOMPARE(pool[1].tagIds, (QSet<int>{park, ball}));
    QCOMPARE(pool[1].hash, QStringLiteral("h-rich"));
    QVERIFY(!pool[5].favorite);
    QVERIFY(pool[5].tagIds.isEmpty());          // the album tag doesn't count
    // The pool size caps the ranking, keeping the top.
    QCOMPARE(m_db->tagSampleCandidates(dog, 2).size(), 2);

    // End to end: the favorite's head start wins the first slot, the two-tag photo
    // adds the most new tags for the second, and the duplicate copy never shows.
    QCOMPARE(m_db->tagSamplePaths(dog, 2),
             (QStringList{QStringLiteral("/p/fav.jpg"), QStringLiteral("/p/rich.jpg")}));
    QCOMPARE(m_db->tagSamplePaths(dog, 10).size(), 5);
    QVERIFY(!m_db->tagSamplePaths(dog, 10).contains(QStringLiteral("/p/rich-copy.jpg")));
    // Degenerate requests.
    QVERIFY(m_db->tagSamplePaths(dog, 0).isEmpty());
    QVERIFY(m_db->tagSamplePaths(-1, 2).isEmpty());

    // Only active photos qualify: hide the folder and the samples go with it.
    const int folder = m_db->insertFolder(QStringLiteral("/p"));
    QCOMPARE(m_db->tagSamplePaths(dog, 2).size(), 2);
    m_db->setFolderEnabled(folder, false);
    QVERIFY(m_db->tagSamplePaths(dog, 2).isEmpty());
}

void TestDatabase::pickTagSamplesGreedyCoverageWithFavoriteBonus()
{
    // The selection half, on a hand-built pool already in standalone-score order
    // (tag count + 3 for a favorite; ties newest first).
    auto cand = [](int id, const char *path, bool favorite, QSet<int> tags,
                   const char *hash = "") {
        TagSampleCandidate c;
        c.id = id;
        c.path = QString::fromLatin1(path);
        c.hash = QString::fromLatin1(hash);
        c.favorite = favorite;
        c.tagIds = std::move(tags);
        return c;
    };
    const QVector<TagSampleCandidate> pool{
        cand(1, "rich", false, {1, 2, 3, 4, 5}, "h"),      // 5: beats the favorite's 0 + 3
        cand(2, "rich-copy", false, {1, 2, 3, 4, 5}, "h"), // same content as rich: skipped
        cand(3, "varied", false, {6, 7, 8, 9}),            // 4, all new after rich
        cand(4, "fav", true, {1}),                         // 1 + 3 = 4, nothing new after rich
        cand(5, "similar", false, {1, 2, 3, 6}),           // 4, mostly repeats rich
        cand(6, "small", false, {7, 8}),                   // 2
    };

    // Favorites weigh in rather than winning outright: the five-tag photo takes
    // the first slot over the one-tag favorite.
    QCOMPARE(Database::pickTagSamples(pool, 1), QStringList{QStringLiteral("rich")});
    // Second slot: "varied" adds four tags rich lacks (4) and beats the favorite
    // (0 new + 3) and the look-alike (1 new), despite equal standalone scores.
    QCOMPARE(Database::pickTagSamples(pool, 2),
             (QStringList{QStringLiteral("rich"), QStringLiteral("varied")}));
    // With 1-9 covered, the favorite's bonus is all that's left to score; the two
    // remaining non-favorites tie at zero and keep pool order.
    QCOMPARE(Database::pickTagSamples(pool, 10),
             (QStringList{QStringLiteral("rich"), QStringLiteral("varied"),
                          QStringLiteral("fav"), QStringLiteral("similar"),
                          QStringLiteral("small")}));
    QVERIFY(Database::pickTagSamples(pool, 0).isEmpty());
    QVERIFY(Database::pickTagSamples({}, 2).isEmpty());
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

void TestDatabase::favoritesTagCannotBeDeleted()
{
    const int fav = m_db->favoritesTagId();
    QVERIFY(fav > 0);

    // The reserved Favorites tag is permanent; deleteTag must not remove it.
    m_db->deleteTag(fav);
    QCOMPARE(m_db->tagName(fav), QStringLiteral("_album:Favorites")); // row still present
    const QVector<AlbumInfo> albums = m_db->albumTags();
    QVERIFY(!albums.isEmpty());
    QVERIFY(albums.first().favorite);
    QCOMPARE(albums.first().id, fav);

    // A normal user tag is still deletable.
    const int user = m_db->ensureTag(QStringLiteral("deleteme"));
    m_db->deleteTag(user);
    QCOMPARE(m_db->tagIdByName(QStringLiteral("deleteme")), -1);
}

void TestDatabase::addImplicationRejectsSelfAndNegative()
{
    const int a = m_db->ensureTag(QStringLiteral("a"));
    const int b = m_db->ensureTag(QStringLiteral("b"));
    const int photo = m_db->insertPhoto(QStringLiteral("/p/1.jpg"), QString());
    m_db->linkPhotoTag(photo, a);
    m_db->linkPhotoTag(photo, b);

    // Self-implication and negative ids are rejected outright.
    m_db->addImplication(a, a);
    m_db->addImplication(-1, b);
    m_db->addImplication(a, -1);
    QVERIFY(m_db->implications().isEmpty());

    // A valid rule is still accepted, confirming the guards didn't over-reject.
    m_db->addImplication(a, b);
    QCOMPARE(m_db->implications().size(), 1);
}

void TestDatabase::expandAllImplicationsResolvesDiamond()
{
    // Diamond: a implies both b and c, which both imply d. Every photo tagged a
    // should end up with b, c, and d, and d must not be double-counted or loop.
    const int a = m_db->ensureTag(QStringLiteral("a"));
    const int b = m_db->ensureTag(QStringLiteral("b"));
    const int c = m_db->ensureTag(QStringLiteral("c"));
    const int d = m_db->ensureTag(QStringLiteral("d"));
    m_db->addImplication(a, b);
    m_db->addImplication(a, c);
    m_db->addImplication(b, d);
    m_db->addImplication(c, d);

    const int photo = m_db->insertPhoto(QStringLiteral("/p/1.jpg"), QString());
    m_db->linkPhotoTag(photo, a);

    m_db->expandAllImplications();
    const QSet<int> ids = m_db->photoTagIds(photo);
    QCOMPARE(ids, (QSet<int>{a, b, c, d}));

    // Fixed point reached: a second pass adds nothing.
    QCOMPARE(m_db->expandAllImplications(), 0);
}

void TestDatabase::removeImplicationStopsFuturePropagationOnly()
{
    const int a = m_db->ensureTag(QStringLiteral("a"));
    const int b = m_db->ensureTag(QStringLiteral("b"));
    m_db->addImplication(a, b);

    // Apply the rule to an existing photo: p1 gets both a and b.
    const int p1 = m_db->insertPhoto(QStringLiteral("/p/1.jpg"), QStringLiteral("H1"));
    m_db->linkPhotoTag(p1, a);
    m_db->applyImplications(QStringLiteral("H1"), a);
    QVERIFY(m_db->photoTagIds(p1).contains(b));

    // Removing the implication drops the rule but must NOT un-apply what it already
    // produced: the rule is gone, yet p1 keeps b.
    m_db->removeImplication(a, b);
    QVERIFY(m_db->implications().isEmpty());
    QVERIFY(m_db->photoTagIds(p1).contains(b));

    // Future propagation stops: a freshly tagged photo no longer gains b.
    const int p2 = m_db->insertPhoto(QStringLiteral("/p/2.jpg"), QStringLiteral("H2"));
    m_db->linkPhotoTag(p2, a);
    m_db->applyImplications(QStringLiteral("H2"), a);
    const QSet<int> ids = m_db->photoTagIds(p2);
    QVERIFY(ids.contains(a));
    QVERIFY(!ids.contains(b));
}

void TestDatabase::deleteTagCascadesImplications()
{
    // a -> b -> c. Deleting the middle tag must cascade both implication rows away,
    // breaking the chain (observed through implicationWouldCycle's graph walk).
    const int a = m_db->ensureTag(QStringLiteral("a"));
    const int b = m_db->ensureTag(QStringLiteral("b"));
    const int c = m_db->ensureTag(QStringLiteral("c"));
    m_db->addImplication(a, b);
    m_db->addImplication(b, c);

    // Before: a reaches c through b, so adding c -> a would close a cycle.
    QVERIFY(m_db->implicationWouldCycle(c, a));

    m_db->deleteTag(b);

    // After: with b (and its two implication rows) gone, a no longer reaches c.
    QVERIFY(!m_db->implicationWouldCycle(c, a));
}

void TestDatabase::linkPhotoTagFailsOnMissingForeignKey()
{
    const int photo = m_db->insertPhoto(QStringLiteral("/p/1.jpg"), QString());
    const int tag = m_db->ensureTag(QStringLiteral("x"));

    // With foreign_keys ON, linking a non-existent photo or tag is a foreign-key
    // violation (not caught by INSERT OR IGNORE, which only absorbs the duplicate
    // PK case): the call reports failure and writes no row.
    QVERIFY(!m_db->linkPhotoTag(99999, tag));
    QVERIFY(!m_db->linkPhotoTag(photo, 99999));
    QVERIFY(m_db->photoTagIds(photo).isEmpty());

    // The valid link still succeeds.
    QVERIFY(m_db->linkPhotoTag(photo, tag));
    QVERIFY(m_db->photoTagIds(photo).contains(tag));
}

void TestDatabase::coOccurringTagsEmptyWhenNoCoOccurrence()
{
    const int a = m_db->ensureTag(QStringLiteral("a"));
    const int b = m_db->ensureTag(QStringLiteral("b"));
    const int p1 = m_db->insertPhoto(QStringLiteral("/p/1.jpg"), QString());
    const int p2 = m_db->insertPhoto(QStringLiteral("/p/2.jpg"), QString());
    m_db->linkPhotoTag(p1, a);
    m_db->linkPhotoTag(p2, b); // a and b never appear on the same photo

    // Faceted drill-down on {a, b}: no photo carries both, so the co-occurring set
    // is empty — it must NOT fall back to the full active tag list.
    QVERIFY(m_db->coOccurringTags({a, b}).isEmpty());
}

void TestDatabase::hashFileSamplesLargeFilesAndFullHashesSmall()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto write = [&](const QString &name, const QByteArray &bytes) {
        const QString p = dir.filePath(name);
        QFile f(p);
        return (f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size()) ? p : QString();
    };
    // Tiny thresholds so a few hundred bytes exercises sampling.
    const qint64 kFull = 1024, kChunk = 256;

    // At/under the threshold: whole-file hash, bare hex (unchanged scheme).
    const QString smallHash = Database::hashFile(write("small.bin", QByteArray(500, '\1')),
                                                 kFull, kChunk);
    QVERIFY(!smallHash.isEmpty());
    QVERIFY(!smallHash.startsWith(QLatin1String("s1:")));

    // Build a > threshold buffer with a distinct head and tail.
    QByteArray big(2000, 'x');
    for (int i = 0; i < 256; ++i) {
        big[i] = char('A' + (i % 26));
        big[big.size() - 1 - i] = char('a' + (i % 26));
    }
    const QString bigHash = Database::hashFile(write("big.bin", big), kFull, kChunk);
    QVERIFY(bigHash.startsWith(QLatin1String("s1:")));

    // Byte-identical copy → identical fingerprint (content matching preserved).
    QCOMPARE(Database::hashFile(write("big-copy.bin", big), kFull, kChunk), bigHash);

    // A changed byte in the sampled head or tail → different fingerprint.
    QByteArray tailEdit = big;
    tailEdit[big.size() - 1] = tailEdit[big.size() - 1] + 1;
    QVERIFY(Database::hashFile(write("tail.bin", tailEdit), kFull, kChunk) != bigHash);

    // A changed byte strictly between the head and tail windows is NOT detected —
    // the documented trade-off for big files.
    QByteArray midEdit = big;
    midEdit[1000] = midEdit[1000] + 1;
    QCOMPARE(Database::hashFile(write("mid.bin", midEdit), kFull, kChunk), bigHash);

    // A different length (same head, extended) → different fingerprint (size is mixed in).
    QVERIFY(Database::hashFile(write("longer.bin", big + QByteArray(64, 'x')), kFull, kChunk)
            != bigHash);
}

void TestDatabase::photosNeedingResampleSelectsLargeLegacyHashes()
{
    const qint64 T = 1000;
    // Large files on a whole-file hash are candidates regardless of folder state,
    // including one in a disabled folder (a full pass migrates the whole library).
    const int big = m_db->insertPhoto(QStringLiteral("/p/big.mp4"),
                                      QStringLiteral("deadbeef"), 0, 0, /*size=*/2000);
    const int disabledFolder = m_db->insertFolder(QStringLiteral("/lib/off"));
    m_db->setFolderEnabled(disabledFolder, false);
    const int hidden = m_db->insertPhoto(QStringLiteral("/lib/off/big.mp4"),
                                         QStringLiteral("deadbee2"), 0, 0, 2000);

    m_db->insertPhoto(QStringLiteral("/p/already.mp4"),
                      QStringLiteral("s1:cafef00d"), 0, 0, 3000); // already sampled
    m_db->insertPhoto(QStringLiteral("/p/small.jpg"),
                      QStringLiteral("abc123"), 0, 0, 500);       // under the threshold
    m_db->insertPhoto(QStringLiteral("/p/new.mp4"), QString(), 0, 0, 5000); // not hashed yet

    QList<int> ids;
    for (const auto &pr : m_db->photosNeedingResample(T))
        ids << pr.first;
    QVERIFY(ids.contains(big));
    QVERIFY(ids.contains(hidden)); // disabled folder included
    QCOMPARE(ids.size(), 2);       // not the sampled, small, or un-hashed rows
    QVERIFY(m_db->hasResampleCandidates(T));
}

// --- Tag aliases ------------------------------------------------------------

void TestDatabase::resolveTagNameAndEnsureTagFollowAliases()
{
    const int forest = m_db->ensureTag(QStringLiteral("forest"));
    QVERIFY(m_db->createAlias(QStringLiteral("forests"), forest));

    // resolveTagName maps the alias (any case) to the canonical id, a real tag to
    // its own id, and an unknown name to -1.
    QCOMPARE(m_db->resolveTagName(QStringLiteral("forests")), forest);
    QCOMPARE(m_db->resolveTagName(QStringLiteral("FORESTS")), forest);
    QCOMPARE(m_db->resolveTagName(QStringLiteral("forest")), forest);
    QCOMPARE(m_db->resolveTagName(QStringLiteral("nope")), -1);

    // ensureTag resolves the alias to the canonical tag and never creates a row for
    // the alias name.
    QCOMPARE(m_db->ensureTag(QStringLiteral("forests")), forest);
    QCOMPARE(m_db->tagIdByName(QStringLiteral("forests")), -1); // raw: no "forests" tag exists
}

void TestDatabase::createAliasWithoutExistingTagAddsRedirectOnly()
{
    const int forest = m_db->ensureTag(QStringLiteral("forest"));
    QVERIFY(m_db->createAlias(QStringLiteral("woods"), forest));

    // No tag named "woods" is created; it only redirects.
    QCOMPARE(m_db->tagIdByName(QStringLiteral("woods")), -1);
    QVERIFY(m_db->aliasExists(QStringLiteral("woods")));
    QCOMPARE(m_db->resolveTagName(QStringLiteral("woods")), forest);

    const QVector<AliasInfo> aliases = m_db->loadAliases();
    QCOMPARE(aliases.size(), 1);
    QCOMPARE(aliases.first().aliasName, QStringLiteral("woods"));
    QCOMPARE(aliases.first().tagId, forest);
    QCOMPARE(aliases.first().tagName, QStringLiteral("forest"));
}

void TestDatabase::createAliasMergesExistingTagAcrossAllTables()
{
    // A is the canonical tag; B becomes an alias of A, so B's links fold into A.
    const int a = m_db->ensureTag(QStringLiteral("forest"));
    const int b = m_db->ensureTag(QStringLiteral("forests"));
    const int x = m_db->ensureTag(QStringLiteral("x"));
    const int y = m_db->ensureTag(QStringLiteral("y"));
    const int z = m_db->ensureTag(QStringLiteral("z"));

    // photo_tags: a photo only on B, and one on both (to exercise the dedupe).
    const int pB = m_db->insertPhoto(QStringLiteral("/p/b.jpg"), QString());
    const int pBoth = m_db->insertPhoto(QStringLiteral("/p/both.jpg"), QString());
    m_db->linkPhotoTag(pB, b);
    m_db->linkPhotoTag(pBoth, a);
    m_db->linkPhotoTag(pBoth, b);

    // content_tags: B archived under a hash; a shared hash archived under both.
    m_db->rememberContentTag(QStringLiteral("HB"), b);
    m_db->rememberContentTag(QStringLiteral("HS"), a);
    m_db->rememberContentTag(QStringLiteral("HS"), b);

    // smart_album_tags: an album referencing both A and B.
    const int sa = m_db->createSmartAlbum(QStringLiteral("sa"), {a, b}, true);
    QVERIFY(sa > 0);

    // tag_implications on both sides, including two that become self-loops (A<->B)
    // and one that becomes a duplicate (z->a and z->b).
    m_db->addImplication(x, b);
    m_db->addImplication(b, y);
    m_db->addImplication(a, b);
    m_db->addImplication(b, a);
    m_db->addImplication(z, a);
    m_db->addImplication(z, b);

    QVERIFY(m_db->createAlias(QStringLiteral("forests"), a));

    // B is gone; "forests" now resolves to A via the alias.
    QCOMPARE(m_db->tagIdByName(QStringLiteral("forests")), -1);
    QCOMPARE(m_db->resolveTagName(QStringLiteral("forests")), a);
    QVERIFY(m_db->aliasExists(QStringLiteral("forests")));

    // photo_tags moved to A (and deduped on the photo that had both).
    QVERIFY(m_db->photoTagIds(pB).contains(a));
    QVERIFY(m_db->photoTagIds(pBoth).contains(a));
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.exec(QStringLiteral("SELECT COUNT(*) FROM photo_tags WHERE photo_id = %1 AND tag_id = %2")
               .arg(pBoth).arg(a));
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toInt(), 1); // not duplicated

    // content_tags moved to A: a fresh copy of HB now receives A, and HS carries A once.
    const int pHB = m_db->insertPhoto(QStringLiteral("/p/hb.jpg"), QStringLiteral("HB"));
    m_db->applyContentTags(pHB, QStringLiteral("HB"));
    QVERIFY(m_db->photoTagIds(pHB).contains(a));
    q.exec(QStringLiteral("SELECT COUNT(*) FROM content_tags WHERE hash = 'HS' AND tag_id = %1").arg(a));
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toInt(), 1);
    q.exec(QStringLiteral("SELECT COUNT(*) FROM content_tags WHERE tag_id = %1").arg(b));
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toInt(), 0); // nothing left pointing at B

    // smart_album_tags: the album now lists A once, not A+B.
    QCOMPARE(m_db->smartAlbum(sa).tagIds, QList<int>{a});

    // tag_implications: rewired to x->a, a->y, z->a; the A<->B self-loops dropped,
    // z->b folded into the existing z->a.
    q.exec(QStringLiteral("SELECT COUNT(*) FROM tag_implications WHERE antecedent_id = consequent_id"));
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toInt(), 0);
    q.exec(QStringLiteral("SELECT antecedent_id, consequent_id FROM tag_implications "
                          "ORDER BY antecedent_id, consequent_id"));
    QList<QPair<int, int>> rules;
    while (q.next())
        rules.append({q.value(0).toInt(), q.value(1).toInt()});
    QCOMPARE(rules.size(), 3);
    QVERIFY(rules.contains(qMakePair(x, a)));
    QVERIFY(rules.contains(qMakePair(a, y)));
    QVERIFY(rules.contains(qMakePair(z, a)));

    // The merged photos are queued for sidecar rewrite (now under the canonical name).
    const QList<int> dirty = m_db->sidecarDirtyIds();
    QVERIFY(dirty.contains(pB));
    QVERIFY(dirty.contains(pBoth));
}

void TestDatabase::createAliasRejectsSelfNameAndDuplicate()
{
    const int forest = m_db->ensureTag(QStringLiteral("forest"));
    // Aliasing a tag to its own name (any case) is refused.
    QVERIFY(!m_db->createAlias(QStringLiteral("forest"), forest));
    QVERIFY(!m_db->createAlias(QStringLiteral("FOREST"), forest));
    QVERIFY(!m_db->aliasExists(QStringLiteral("forest")));

    // A second alias with the same name is refused.
    const int tree = m_db->ensureTag(QStringLiteral("tree"));
    QVERIFY(m_db->createAlias(QStringLiteral("woods"), forest));
    QVERIFY(!m_db->createAlias(QStringLiteral("woods"), tree));
    QVERIFY(!m_db->createAlias(QStringLiteral("WOODS"), tree));
    QCOMPARE(m_db->resolveTagName(QStringLiteral("woods")), forest); // still the first target
}

void TestDatabase::removeAliasKeepsCanonicalTag()
{
    const int forest = m_db->ensureTag(QStringLiteral("forest"));
    const int p = m_db->insertPhoto(QStringLiteral("/p/1.jpg"), QString());
    m_db->linkPhotoTag(p, forest);
    QVERIFY(m_db->createAlias(QStringLiteral("woods"), forest));

    m_db->removeAlias(QStringLiteral("woods"));
    QVERIFY(!m_db->aliasExists(QStringLiteral("woods")));
    QCOMPARE(m_db->resolveTagName(QStringLiteral("woods")), -1); // no longer resolves
    // The canonical tag and its photo link are untouched.
    QCOMPARE(m_db->tagIdByName(QStringLiteral("forest")), forest);
    QVERIFY(m_db->photoTagIds(p).contains(forest));
}

void TestDatabase::deletingCanonicalTagCascadesAliases()
{
    const int forest = m_db->ensureTag(QStringLiteral("forest"));
    QVERIFY(m_db->createAlias(QStringLiteral("woods"), forest));
    QVERIFY(m_db->createAlias(QStringLiteral("forests"), forest));
    QCOMPARE(m_db->loadAliases().size(), 2);

    m_db->deleteTag(forest); // user tag (system = 0) → deletes
    QCOMPARE(m_db->loadAliases().size(), 0); // ON DELETE CASCADE dropped both aliases
    QVERIFY(!m_db->aliasExists(QStringLiteral("woods")));
}

void TestDatabase::renameTagOntoAliasNameIsRejectedByDatabase()
{
    // The Library guards this, but assert the DB primitives it relies on: an alias
    // name is detectable so a tag can't be renamed onto it.
    const int forest = m_db->ensureTag(QStringLiteral("forest"));
    const int tree = m_db->ensureTag(QStringLiteral("tree"));
    QVERIFY(m_db->createAlias(QStringLiteral("woods"), forest));
    QVERIFY(m_db->aliasExists(QStringLiteral("woods")));
    QCOMPARE(m_db->tagIdByName(QStringLiteral("woods")), -1); // not a tag, only an alias
    Q_UNUSED(tree);
}

// --- Triage undo primitives -------------------------------------------------

void TestDatabase::applyTagRecordedReportsOnlyNewRows()
{
    const int a = m_db->ensureTag(QStringLiteral("a"));
    const int b = m_db->ensureTag(QStringLiteral("b"));
    m_db->addImplication(a, b);
    const QString hash = QStringLiteral("HASH_REC");
    const int photo = m_db->insertPhoto(QStringLiteral("/p/1.jpg"), hash);

    // A fresh apply of 'a' adds a and (by implication) b, both live and archived.
    TagApplyRecord rec = m_db->applyTagRecorded(photo, a);
    QCOMPARE(rec.photoId, photo);
    QCOMPARE(rec.tagId, a);
    QCOMPARE(rec.hash, hash);
    QVERIFY(rec.changedAnything());
    QCOMPARE(rec.photoLinks, (QList<QPair<int, int>>{qMakePair(photo, a), qMakePair(photo, b)}));
    QCOMPARE(rec.contentLinks, (QList<int>{a, b}));

    // Applying it again changes nothing, and the record says so.
    rec = m_db->applyTagRecorded(photo, a);
    QVERIFY(!rec.changedAnything());
    QVERIFY(rec.photoLinks.isEmpty());
    QVERIFY(rec.contentLinks.isEmpty());

    // Bad input is a harmless empty record.
    QVERIFY(!m_db->applyTagRecorded(-1, a).changedAnything());
    QVERIFY(!m_db->applyTagRecorded(photo, -1).changedAnything());
}

void TestDatabase::revertTagApplyKeepsPreexistingImpliedTags()
{
    // Two rules share a consequent: a → shared, c → shared.
    const int a = m_db->ensureTag(QStringLiteral("a"));
    const int c = m_db->ensureTag(QStringLiteral("c"));
    const int shared = m_db->ensureTag(QStringLiteral("shared"));
    m_db->addImplication(a, shared);
    m_db->addImplication(c, shared);
    const QString hash = QStringLiteral("HASH_UNDO");
    const int photo = m_db->insertPhoto(QStringLiteral("/p/1.jpg"), hash);

    // 'c' is applied first, so 'shared' is already on the photo (and archived)
    // before 'a' arrives.
    m_db->applyTagRecorded(photo, c);
    QVERIFY(m_db->photoTagIds(photo).contains(shared));

    const TagApplyRecord rec = m_db->applyTagRecorded(photo, a);
    QCOMPARE(rec.photoLinks, (QList<QPair<int, int>>{qMakePair(photo, a)})); // only 'a' was new
    QCOMPARE(rec.contentLinks, (QList<int>{a}));

    // Undoing 'a' takes back just 'a': the implied 'shared' stays, as does 'c'.
    m_db->revertTagApply(rec);
    const QSet<int> ids = m_db->photoTagIds(photo);
    QVERIFY(!ids.contains(a));
    QVERIFY(ids.contains(c));
    QVERIFY(ids.contains(shared));

    // The archive is untouched for the kept tags too: a re-import still gets them.
    const int copy = m_db->insertPhoto(QStringLiteral("/p/copy.jpg"), hash);
    m_db->applyContentTags(copy, hash);
    const QSet<int> copyIds = m_db->photoTagIds(copy);
    QVERIFY(!copyIds.contains(a));
    QVERIFY(copyIds.contains(c));
    QVERIFY(copyIds.contains(shared));

    // Contrast: on a photo that didn't have the implied tag yet, undoing the apply
    // removes the implied tag along with it.
    const QString hash2 = QStringLiteral("HASH_UNDO2");
    const int photo2 = m_db->insertPhoto(QStringLiteral("/p/2.jpg"), hash2);
    const TagApplyRecord rec2 = m_db->applyTagRecorded(photo2, a);
    QVERIFY(m_db->photoTagIds(photo2).contains(shared));
    m_db->revertTagApply(rec2);
    QVERIFY(m_db->photoTagIds(photo2).isEmpty());
    QVERIFY(!m_db->photoTagIds(photo).isEmpty()); // the other photo is unaffected
}

void TestDatabase::revertTagApplyCoversEveryContentCopy()
{
    // Tagging by content reaches every live copy of the hash; so does the undo.
    const QString hash = QStringLiteral("HASH_COPIES");
    const int p1 = m_db->insertPhoto(QStringLiteral("/a/1.jpg"), hash);
    const int p2 = m_db->insertPhoto(QStringLiteral("/b/1.jpg"), hash);
    const int tag = m_db->ensureTag(QStringLiteral("shared"));

    const TagApplyRecord rec = m_db->applyTagRecorded(p1, tag);
    QCOMPARE(rec.photoLinks.size(), 2);
    QVERIFY(m_db->photoTagIds(p2).contains(tag));

    m_db->revertTagApply(rec);
    QVERIFY(!m_db->photoTagIds(p1).contains(tag));
    QVERIFY(!m_db->photoTagIds(p2).contains(tag));
    // And the archive row is gone, so a re-import doesn't resurrect it.
    const int p3 = m_db->insertPhoto(QStringLiteral("/c/1.jpg"), hash);
    m_db->applyContentTags(p3, hash);
    QVERIFY(!m_db->photoTagIds(p3).contains(tag));

    // Content without a hash degrades to the single row, like linkTagToContent.
    const int loose = m_db->insertPhoto(QStringLiteral("/d/loose.jpg"), QString());
    const TagApplyRecord lr = m_db->applyTagRecorded(loose, tag);
    QCOMPARE(lr.photoLinks, (QList<QPair<int, int>>{qMakePair(loose, tag)}));
    QVERIFY(lr.contentLinks.isEmpty());
    m_db->revertTagApply(lr);
    QVERIFY(!m_db->photoTagIds(loose).contains(tag));
}

void TestDatabase::relinkAfterRemoveRestoresTheOneLink()
{
    // Removing a tag in triage drops one link (content-wide) and its archive row;
    // undoing it puts exactly that back — without re-running implications.
    const int a = m_db->ensureTag(QStringLiteral("a"));
    const int b = m_db->ensureTag(QStringLiteral("b"));
    m_db->addImplication(a, b);
    const QString hash = QStringLiteral("HASH_RELINK");
    const int photo = m_db->insertPhoto(QStringLiteral("/p/1.jpg"), hash);
    m_db->applyTagRecorded(photo, a); // a + b

    // The remove, as Library::removeTag does it.
    m_db->unlinkTagFromContent(photo, b);
    m_db->forgetContentTag(hash, b);
    QVERIFY(!m_db->photoTagIds(photo).contains(b));

    // The undo, as Library::relinkTag does it.
    m_db->linkTagToContent(photo, b);
    m_db->rememberContentTag(hash, b);
    QVERIFY(m_db->photoTagIds(photo).contains(b));
    const int copy = m_db->insertPhoto(QStringLiteral("/p/copy.jpg"), hash);
    m_db->applyContentTags(copy, hash);
    QVERIFY(m_db->photoTagIds(copy).contains(b)); // archived again
}

void TestDatabase::photoRowRestoreReattachesArchivedTags()
{
    const QString hash = QStringLiteral("HASH_RESTORE");
    const int tag = m_db->ensureTag(QStringLiteral("keep"));
    const int photo = m_db->insertPhoto(QStringLiteral("/p/1.jpg"), hash, 1700000000, 1699999000, 4096);
    m_db->applyTagRecorded(photo, tag);

    // Snapshot, then delete (the triage trash): the row goes, the archive stays
    // and the hash is remembered as orphaned.
    const PhotoRowSnapshot row = m_db->photoRow(photo);
    QCOMPARE(row.id, photo);
    QCOMPARE(row.path, QStringLiteral("/p/1.jpg"));
    QCOMPARE(row.hash, hash);
    QCOMPARE(row.takenAt, qint64(1700000000));
    QCOMPARE(row.mtime, qint64(1699999000));
    QCOMPARE(row.size, qint64(4096));
    m_db->deletePhoto(photo);
    QCOMPARE(m_db->rememberedRemovedCount(), 1);
    QVERIFY(m_db->photoPath(photo).isEmpty());

    // Restore: a new row at the same path, with the tag back from the archive and
    // the orphan marker cleared.
    const int restored = m_db->restorePhotoRow(row);
    QVERIFY(restored >= 0);
    QCOMPARE(m_db->photoPath(restored), QStringLiteral("/p/1.jpg"));
    QVERIFY(m_db->photoTagIds(restored).contains(tag));
    QCOMPARE(m_db->photoHash(restored), hash);
    QCOMPARE(m_db->rememberedRemovedCount(), 0);
    const QVector<Photo> photos = m_db->loadPhotos();
    QCOMPARE(photos.size(), 1);
    QCOMPARE(photos.first().takenAt, qint64(1700000000));

    // Unknown rows snapshot as empty and refuse to restore.
    QCOMPARE(m_db->photoRow(9999).id, -1);
    QCOMPARE(m_db->restorePhotoRow(PhotoRowSnapshot{}), -1);
}

QTEST_GUILESS_MAIN(TestDatabase)
#include "tst_database.moc"
