// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QtTest>

#include <QDir>
#include <QHash>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include "database.h"
#include "oql.h"
#include "photo.h"

// Exercises the schema-versioning / migration path in Database::open: an older or
// unversioned library is upgraded in place to the current version, a library from
// a newer build is refused, and a backup is written before the non-additive
// migration. Uses temp-file databases (not :memory:) so backups and reopening
// behave as they do in the real app.
class TestSchema : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void refusesNewerSchema();
    void upgradesFromReducedSchema();
    void upgradesLegacyUnversioned();
    void upgradesToV10SeedsOrphansAndAddsColumns();
    void upgradesToV11AddsSidecarDirty();
    void seedsSidecarDirtyForNeverSyncedTaggedPhotos();
    void convertsPipeNamesAndMergesCollisions();
    void clearsVideoDatesForCreationTimeReread();
    void upgradesToV15AddsTagAliases();
    void normalizesQuotesAndWhitespaceInNames();
    void upgradesToV17ConvertsSmartAlbumsToQueries();

private:
    // Count rows in orphaned_hashes matching `hash` (-1 if the table is absent).
    int orphanRowCount(const QString &path, const QString &hash);
    // Run one statement on a fresh connection to `path`, failing the test on error.
    void execRaw(const QString &path, const QString &sql);
    // Read PRAGMA user_version from `path` via its own short-lived connection.
    int readUserVersion(const QString &path);
    // Backup files sitting beside the library (library.db.bak-v*).
    QStringList backupFiles() const;
    QString dbPath() const { return m_dir->filePath(QStringLiteral("library.db")); }

    QTemporaryDir *m_dir = nullptr;
    int m_seq = 0; // unique connection-name counter
};

void TestSchema::init()
{
    m_dir = new QTemporaryDir;
    QVERIFY(m_dir->isValid());
}

void TestSchema::cleanup()
{
    delete m_dir;
    m_dir = nullptr;
}

void TestSchema::execRaw(const QString &path, const QString &sql)
{
    const QString conn = QStringLiteral("raw_%1").arg(++m_seq);
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(path);
        QVERIFY2(db.open(), qPrintable(db.lastError().text()));
        for (const QString &stmt : sql.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
            const QString trimmed = stmt.trimmed();
            if (trimmed.isEmpty())
                continue;
            QSqlQuery q(db);
            QVERIFY2(q.exec(trimmed), qPrintable(trimmed + QStringLiteral(" -> ") + q.lastError().text()));
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(conn);
}

int TestSchema::readUserVersion(const QString &path)
{
    int v = -1;
    const QString conn = QStringLiteral("probe_%1").arg(++m_seq);
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(path);
        if (db.open()) {
            QSqlQuery q(db);
            q.exec(QStringLiteral("PRAGMA user_version"));
            if (q.next())
                v = q.value(0).toInt();
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(conn);
    return v;
}

QStringList TestSchema::backupFiles() const
{
    return QDir(m_dir->path()).entryList({QStringLiteral("library.db.bak-v*")}, QDir::Files);
}

int TestSchema::orphanRowCount(const QString &path, const QString &hash)
{
    int n = -1;
    const QString conn = QStringLiteral("orphan_%1").arg(++m_seq);
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(path);
        if (db.open()) {
            QSqlQuery q(db);
            q.prepare(QStringLiteral("SELECT COUNT(*) FROM orphaned_hashes WHERE hash = ?"));
            q.addBindValue(hash);
            if (q.exec() && q.next())
                n = q.value(0).toInt();
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(conn);
    return n;
}

void TestSchema::refusesNewerSchema()
{
    // Build a current database, then stamp it with a version newer than this build.
    {
        Database db;
        QVERIFY(db.open(dbPath(), QStringLiteral("c_build")));
    }
    QSqlDatabase::removeDatabase(QStringLiteral("c_build"));
    execRaw(dbPath(), QStringLiteral("PRAGMA user_version = %1")
                          .arg(Database::currentSchemaVersion() + 1));

    {
        Database db;
        const bool opened = db.open(dbPath(), QStringLiteral("c_refuse"));
        QVERIFY(!opened);           // refused
        QVERIFY(db.schemaTooNew()); // and flagged as too-new, so the UI can explain
    } // destroy Database (releasing its connection) before removing it
    QSqlDatabase::removeDatabase(QStringLiteral("c_refuse"));

    // The refusal must not have upgraded or downgraded anything.
    QCOMPARE(readUserVersion(dbPath()), Database::currentSchemaVersion() + 1);
    QVERIFY(backupFiles().isEmpty());
}

void TestSchema::upgradesFromReducedSchema()
{
    // A v1 database: only the original base tables, with a seeded, tagged photo.
    execRaw(dbPath(), QStringLiteral(
        "CREATE TABLE photos (id INTEGER PRIMARY KEY, path TEXT UNIQUE NOT NULL, added_at INTEGER);"
        "CREATE TABLE tags (id INTEGER PRIMARY KEY, name TEXT UNIQUE NOT NULL COLLATE NOCASE);"
        "CREATE TABLE photo_tags ("
        "  photo_id INTEGER NOT NULL REFERENCES photos(id) ON DELETE CASCADE,"
        "  tag_id INTEGER NOT NULL REFERENCES tags(id) ON DELETE CASCADE,"
        "  PRIMARY KEY (photo_id, tag_id));"
        "CREATE TABLE folders (id INTEGER PRIMARY KEY, path TEXT UNIQUE NOT NULL, added_at INTEGER);"
        "INSERT INTO photos(id, path, added_at) VALUES (1, '/pics/one.jpg', 0);"
        "INSERT INTO tags(id, name) VALUES (1, 'beach');"
        "INSERT INTO photo_tags(photo_id, tag_id) VALUES (1, 1);"
        "PRAGMA user_version = 1;"));

    {
        Database db;
        QVERIFY(db.open(dbPath(), QStringLiteral("c_reduced")));

        // Lands on the current version, and the migration-added tables/columns are
        // now usable (loadPhotos reads hash/taken_at; smartAlbums reads smart_albums).
        QCOMPARE(readUserVersion(dbPath()), Database::currentSchemaVersion());
        QVERIFY(db.photoTagIds(1).contains(1)); // the photo kept its tag through the upgrade
        const QVector<Photo> photos = db.loadPhotos();
        QCOMPARE(photos.size(), 1);
        QVERIFY(photos.first().tagNames.contains(QStringLiteral("beach")));
        QVERIFY(db.smartAlbums().isEmpty()); // table exists and is queryable

        // Nothing to rename in a reduced schema, so no backup is written.
        QVERIFY(backupFiles().isEmpty());
    }
    QSqlDatabase::removeDatabase(QStringLiteral("c_reduced"));
}

void TestSchema::upgradesLegacyUnversioned()
{
    int photoId = -1, tagId = -1, favId = -1;

    // Start from a current database seeded with a photo, a user tag, and Favorites
    // membership, then rewind it to look like a pre-versioning library: rename the
    // reserved category back to the old bare "Albums" and clear user_version.
    {
        Database db;
        QVERIFY(db.open(dbPath(), QStringLiteral("c_seed")));
        photoId = db.insertPhoto(QStringLiteral("/pics/one.jpg"), QString());
        tagId = db.ensureTag(QStringLiteral("beach"));
        QVERIFY(db.linkPhotoTag(photoId, tagId));
        favId = db.favoritesTagId();
        QVERIFY(favId > 0);
        QVERIFY(db.linkPhotoTag(photoId, favId)); // favorite membership
    }
    QSqlDatabase::removeDatabase(QStringLiteral("c_seed"));

    execRaw(dbPath(), QStringLiteral(
        "UPDATE tag_categories SET name = 'Albums' WHERE name = '_system:Albums';"
        "PRAGMA user_version = 0;"));

    // Reopen through Database: it should migrate to the current version and rename
    // the category back, without losing the photo's tag or its favorite membership.
    {
        Database db;
        QVERIFY(db.open(dbPath(), QStringLiteral("c_upgrade")));

        QCOMPARE(readUserVersion(dbPath()), Database::currentSchemaVersion());
        const QSet<int> ids = db.photoTagIds(photoId);
        QVERIFY(ids.contains(tagId)); // user tag survived
        QVERIFY(ids.contains(favId)); // favorite membership survived

        QCOMPARE(db.favoritesTagId(), favId); // reserved tag re-resolved to the same row
        const QVector<AlbumInfo> albums = db.albumTags();
        QVERIFY(!albums.isEmpty());
        QVERIFY(albums.first().favorite);
        QCOMPARE(albums.first().name, QStringLiteral("Favorites"));

        // A legacy "Albums" row was present, so the non-additive step wrote a backup.
        QVERIFY(!backupFiles().isEmpty());
    }
    QSqlDatabase::removeDatabase(QStringLiteral("c_upgrade"));
}

void TestSchema::upgradesToV10SeedsOrphansAndAddsColumns()
{
    const QString orphanHash = QStringLiteral("ORPHAN_HASH");
    const QString liveHash = QStringLiteral("LIVE_HASH");

    // Build a current database, seed a live photo+archive and an archive for a
    // hash with no photo, then rewind to v9 to force the v10 migration to run.
    {
        Database db;
        QVERIFY(db.open(dbPath(), QStringLiteral("c_v10_seed")));
        const int p = db.insertPhoto(QStringLiteral("/pics/one.jpg"), liveHash);
        const int tag = db.ensureTag(QStringLiteral("beach"));
        QVERIFY(db.linkPhotoTag(p, tag));
        db.rememberContentTag(liveHash, tag);   // archived, still has a live photo
        db.rememberContentTag(orphanHash, tag); // archived, no photo → orphan
    }
    QSqlDatabase::removeDatabase(QStringLiteral("c_v10_seed"));

    // Rewind: drop the v10 state (so the migration genuinely re-runs) and the
    // version. SQLite can't drop columns on this version, but clearing the table
    // and the version is enough to re-trigger the additive step and the seed.
    execRaw(dbPath(), QStringLiteral(
        "DELETE FROM orphaned_hashes;"
        "PRAGMA user_version = 9;"));

    {
        Database db;
        QVERIFY(db.open(dbPath(), QStringLiteral("c_v10")));
        QCOMPARE(readUserVersion(dbPath()), Database::currentSchemaVersion());

        // The orphaned hash (archive, no photo) is seeded; the live one is not.
        QCOMPARE(orphanRowCount(dbPath(), orphanHash), 1);
        QCOMPARE(orphanRowCount(dbPath(), liveHash), 0);

        // The new per-photo stat columns exist and are readable.
        const QVector<PhotoRef> refs = db.allPhotoRefs();
        QCOMPARE(refs.size(), 1);
        QCOMPARE(refs.first().hash, liveHash);

        QVERIFY(backupFiles().isEmpty()); // additive step → no backup
    }
    QSqlDatabase::removeDatabase(QStringLiteral("c_v10"));
}

void TestSchema::upgradesToV11AddsSidecarDirty()
{
    // Build a current database, then rewind to v10: drop sidecar_dirty and the
    // version so the v11 migration genuinely re-runs.
    {
        Database db;
        QVERIFY(db.open(dbPath(), QStringLiteral("c_v11_seed")));
    }
    QSqlDatabase::removeDatabase(QStringLiteral("c_v11_seed"));
    execRaw(dbPath(), QStringLiteral(
        "DROP TABLE IF EXISTS sidecar_dirty;"
        "PRAGMA user_version = 10;"));

    {
        Database db;
        QVERIFY(db.open(dbPath(), QStringLiteral("c_v11")));
        QCOMPARE(readUserVersion(dbPath()), Database::currentSchemaVersion());
        // The queue table exists and is usable.
        const int p = db.insertPhoto(QStringLiteral("/pics/one.jpg"), QString());
        db.enqueueSidecarDirty({p});
        QCOMPARE(db.sidecarDirtyIds(), QList<int>{p});
        QVERIFY(backupFiles().isEmpty()); // additive step → no backup
    }
    QSqlDatabase::removeDatabase(QStringLiteral("c_v11"));
}

void TestSchema::seedsSidecarDirtyForNeverSyncedTaggedPhotos()
{
    int taggedUnsynced = -1, taggedSynced = -1, untagged = -1;
    {
        Database db;
        QVERIFY(db.open(dbPath(), QStringLiteral("c_seed12")));
        taggedUnsynced = db.insertPhoto(QStringLiteral("/p/a.jpg"), QString());
        taggedSynced = db.insertPhoto(QStringLiteral("/p/b.jpg"), QString());
        untagged = db.insertPhoto(QStringLiteral("/p/c.jpg"), QString());
        const int tag = db.ensureTag(QStringLiteral("x"));
        db.linkPhotoTag(taggedUnsynced, tag);
        db.linkPhotoTag(taggedSynced, tag);
        db.setSidecarWrittenAt(taggedSynced, 123); // already written once
    }
    QSqlDatabase::removeDatabase(QStringLiteral("c_seed12"));

    // Rewind: clear the queue and the version so the v12 seed genuinely re-runs.
    execRaw(dbPath(), QStringLiteral("DELETE FROM sidecar_dirty; PRAGMA user_version = 11;"));

    {
        Database db;
        QVERIFY(db.open(dbPath(), QStringLiteral("c_v12")));
        QCOMPARE(readUserVersion(dbPath()), Database::currentSchemaVersion());
        const QList<int> dirty = db.sidecarDirtyIds();
        QVERIFY(dirty.contains(taggedUnsynced)); // tagged, never synced → seeded
        QVERIFY(!dirty.contains(taggedSynced));  // tagged but already written → skipped
        QVERIFY(!dirty.contains(untagged));      // nothing to write → skipped
        QVERIFY(backupFiles().isEmpty());        // additive step → no backup
    }
    QSqlDatabase::removeDatabase(QStringLiteral("c_v12"));
}

void TestSchema::convertsPipeNamesAndMergesCollisions()
{
    // Create the current schema, then insert '|' names via raw SQL (the API would
    // sanitize them) and rewind to v12 so the v13 rename migration runs.
    {
        Database db;
        QVERIFY(db.open(dbPath(), QStringLiteral("c_seed13")));
    }
    QSqlDatabase::removeDatabase(QStringLiteral("c_seed13"));

    execRaw(dbPath(), QStringLiteral(
        "INSERT INTO photos(id, path) VALUES (1, '/a.jpg');"
        "INSERT INTO photos(id, path) VALUES (2, '/b.jpg');"
        // 'a|b' collides with the existing 'a/b' once converted; 'c|d' does not.
        "INSERT INTO tags(id, name) VALUES (10, 'a|b');"
        "INSERT INTO tags(id, name) VALUES (11, 'a/b');"
        "INSERT INTO tags(id, name) VALUES (12, 'c|d');"
        "INSERT INTO photo_tags(photo_id, tag_id) VALUES (1, 10);"
        "INSERT INTO photo_tags(photo_id, tag_id) VALUES (2, 11);"
        "INSERT INTO tag_categories(id, name, color_slot, system) VALUES (20, 'x|y', 0, 0);"
        "PRAGMA user_version = 12;"));

    {
        Database db;
        QVERIFY(db.open(dbPath(), QStringLiteral("c_v13")));
        QCOMPARE(readUserVersion(dbPath()), Database::currentSchemaVersion());

        // No '|' names survive.
        QCOMPARE(db.tagIdByName(QStringLiteral("c|d")), -1);
        QVERIFY(db.tagIdByName(QStringLiteral("c/d")) >= 0);   // plain rename
        QCOMPARE(db.tagIdByName(QStringLiteral("a|b")), -1);
        QVERIFY(db.tagName(10).isEmpty());                     // the colliding tag was merged away

        // The collision merged into the existing 'a/b', keeping both photos' links.
        const int merged = db.tagIdByName(QStringLiteral("a/b"));
        QVERIFY(merged >= 0);
        QVERIFY(db.photoTagIds(1).contains(merged));
        QVERIFY(db.photoTagIds(2).contains(merged));

        // Category converted too.
        QCOMPARE(db.categoryIdByName(QStringLiteral("x|y")), -1);
        QVERIFY(db.categoryIdByName(QStringLiteral("x/y")) >= 0);

        // '|' names were present, so the non-additive step wrote a backup.
        QVERIFY(!backupFiles().isEmpty());
    }
    QSqlDatabase::removeDatabase(QStringLiteral("c_v13"));
}

void TestSchema::clearsVideoDatesForCreationTimeReread()
{
    int video = -1, image = -1;
    {
        Database db;
        QVERIFY(db.open(dbPath(), QStringLiteral("c_seed14")));
        video = db.insertPhoto(QStringLiteral("/p/clip.mp4"), QString(), /*takenAt=*/1000);
        image = db.insertPhoto(QStringLiteral("/p/pic.jpg"), QString(), /*takenAt=*/2000);
    }
    QSqlDatabase::removeDatabase(QStringLiteral("c_seed14"));

    // Rewind so the v14 step re-runs on reopen.
    execRaw(dbPath(), QStringLiteral("PRAGMA user_version = 13;"));

    {
        Database db;
        QVERIFY(db.open(dbPath(), QStringLiteral("c_v14")));
        QCOMPARE(readUserVersion(dbPath()), Database::currentSchemaVersion());
        QHash<int, qint64> taken;
        for (const Photo &p : db.loadPhotos())
            taken.insert(p.id, p.takenAt);
        QCOMPARE(taken.value(video), qint64(0));    // video date cleared (NULL → 0), to be re-read
        QCOMPARE(taken.value(image), qint64(2000)); // image date left alone
        QVERIFY(backupFiles().isEmpty());           // additive step → no backup
    }
    QSqlDatabase::removeDatabase(QStringLiteral("c_v14"));
}

void TestSchema::upgradesToV15AddsTagAliases()
{
    // Build a current database, then rewind to v14: drop tag_aliases and the
    // version so the v15 migration genuinely re-runs.
    {
        Database db;
        QVERIFY(db.open(dbPath(), QStringLiteral("c_v15_seed")));
    }
    QSqlDatabase::removeDatabase(QStringLiteral("c_v15_seed"));
    execRaw(dbPath(), QStringLiteral(
        "DROP TABLE IF EXISTS tag_aliases;"
        "PRAGMA user_version = 14;"));

    {
        Database db;
        QVERIFY(db.open(dbPath(), QStringLiteral("c_v15")));
        QCOMPARE(readUserVersion(dbPath()), Database::currentSchemaVersion());
        // The alias table exists and resolves names through it.
        const int tag = db.ensureTag(QStringLiteral("forest"));
        QVERIFY(db.createAlias(QStringLiteral("forests"), tag));
        QCOMPARE(db.resolveTagName(QStringLiteral("forests")), tag);
        QVERIFY(backupFiles().isEmpty()); // additive step → no backup
    }
    QSqlDatabase::removeDatabase(QStringLiteral("c_v15"));
}

void TestSchema::normalizesQuotesAndWhitespaceInNames()
{
    // Seed names the API would now sanitize via raw SQL, then rewind to v15 so
    // the v16 normalization genuinely runs. Covers: a '"' becoming typographic
    // quotes, whitespace runs collapsing, a tag whose cleaned name collides with
    // an existing tag (merged), a category collision (tags reassigned), a smart
    // album collision (suffixed), an alias collision (dropped), and album tags
    // (which are tags under the "_album:" prefix).
    {
        Database db;
        QVERIFY(db.open(dbPath(), QStringLiteral("c_seed16")));
    }
    QSqlDatabase::removeDatabase(QStringLiteral("c_seed16"));

    execRaw(dbPath(), QStringLiteral(
        "INSERT INTO photos(id, path) VALUES (1, '/a.jpg');"
        "INSERT INTO photos(id, path) VALUES (2, '/b.jpg');"
        "INSERT INTO tags(id, name) VALUES (10, 'say \"hi\" (\"there\")');"
        "INSERT INTO tags(id, name) VALUES (11, 'dark   forest');"
        "INSERT INTO tags(id, name) VALUES (12, 'dark forest');"
        "INSERT INTO tags(id, name) VALUES (13, '_album:Trip  2019');"
        "INSERT INTO photo_tags(photo_id, tag_id) VALUES (1, 11);"
        "INSERT INTO photo_tags(photo_id, tag_id) VALUES (2, 12);"
        "INSERT INTO tag_categories(id, name, color_slot, system) VALUES (20, 'Place  ', 0, 0);"
        "INSERT INTO tag_categories(id, name, color_slot, system) VALUES (21, 'Place', 1, 0);"
        "INSERT INTO tags(id, name, category_id) VALUES (14, 'paris', 20);"
        "INSERT INTO smart_albums(id, name, match_all) VALUES (30, 'Best \"of\"', 1);"
        "INSERT INTO smart_albums(id, name, match_all) VALUES (31, 'Best “of”', 1);"
        "INSERT INTO tag_aliases(alias_name, tag_id) VALUES ('woods  ', 12);"
        "INSERT INTO tag_aliases(alias_name, tag_id) VALUES ('woods', 12);"
        "INSERT INTO tag_aliases(alias_name, tag_id) VALUES ('the  forest', 12);"
        "PRAGMA user_version = 15;"));

    {
        Database db;
        QVERIFY(db.open(dbPath(), QStringLiteral("c_v16")));
        QCOMPARE(readUserVersion(dbPath()), Database::currentSchemaVersion());

        // Quotes became typographic, opening after whitespace / '(' and closing
        // elsewhere; nothing else in the name changed.
        QCOMPARE(db.tagName(10), QString::fromUtf8("say \u201chi\u201d (\u201cthere\u201d)"));
        // The collapsed name collided with tag 12 and was merged into it.
        QVERIFY(db.tagName(11).isEmpty());
        const int forest = db.tagIdByName(QStringLiteral("dark forest"));
        QCOMPARE(forest, 12);
        QVERIFY(db.photoTagIds(1).contains(forest));
        QVERIFY(db.photoTagIds(2).contains(forest));
        // Album tags are normalized like any tag.
        QCOMPARE(db.tagName(13), QStringLiteral("_album:Trip 2019"));
        // The category collision reassigned its tags to the surviving row.
        QCOMPARE(db.categoryName(20), QString());
        QCOMPARE(db.tagCategoryId(14), 21);
        // The smart album collision got a distinct suffixed name.
        QCOMPARE(db.smartAlbum(30).name, QString::fromUtf8("Best \u201cof\u201d (2)"));
        QCOMPARE(db.smartAlbum(31).name, QString::fromUtf8("Best \u201cof\u201d"));
        // The duplicate alias was dropped, the other cleaned.
        const QVector<AliasInfo> aliases = db.loadAliases();
        QStringList names;
        for (const AliasInfo &a : aliases)
            names.append(a.aliasName);
        names.sort();
        QCOMPARE(names, QStringList({QStringLiteral("the forest"), QStringLiteral("woods")}));

        // Rows changed, so the non-additive step wrote a backup.
        QVERIFY(!backupFiles().isEmpty());
    }
    QSqlDatabase::removeDatabase(QStringLiteral("c_v16"));

    // A library whose names are already clean takes no backup for this step
    // (the smart albums go too, so the v17 conversion has nothing to back up).
    {
        execRaw(dbPath(), QStringLiteral("DELETE FROM smart_albums; PRAGMA user_version = 15;"));
        for (const QString &f : backupFiles())
            QFile::remove(m_dir->filePath(f));
        Database db;
        QVERIFY(db.open(dbPath(), QStringLiteral("c_v16b")));
        QVERIFY(backupFiles().isEmpty());
    }
    QSqlDatabase::removeDatabase(QStringLiteral("c_v16b"));
}

void TestSchema::upgradesToV17ConvertsSmartAlbumsToQueries()
{
    // Build a current database, then rewind to v16: drop the query column and
    // seed the old (tag set + match_all) form, so the conversion genuinely runs.
    int beach = -1, summer = -1, zoo = -1;
    {
        Database db;
        QVERIFY(db.open(dbPath(), QStringLiteral("c_v17_seed")));
        beach = db.ensureTag(QStringLiteral("beach"));
        summer = db.ensureTag(QStringLiteral("summer"));
        zoo = db.ensureTag(QStringLiteral("Zoo"));
    }
    QSqlDatabase::removeDatabase(QStringLiteral("c_v17_seed"));
    execRaw(dbPath(), QStringLiteral(
        "ALTER TABLE smart_albums DROP COLUMN query;"
        "INSERT INTO smart_albums(id, name, match_all) VALUES (1, 'All', 1);"
        "INSERT INTO smart_albums(id, name, match_all) VALUES (2, 'Any', 0);"
        "INSERT INTO smart_albums(id, name, match_all) VALUES (3, 'One', 1);"
        "INSERT INTO smart_albums(id, name, match_all) VALUES (4, 'None', 1);"
        "INSERT INTO smart_album_tags(smart_album_id, tag_id) VALUES (1, %1), (1, %2), (1, %3);"
        "INSERT INTO smart_album_tags(smart_album_id, tag_id) VALUES (2, %1), (2, %2);"
        "INSERT INTO smart_album_tags(smart_album_id, tag_id) VALUES (3, %2);"
        "PRAGMA user_version = 16;").arg(zoo).arg(beach).arg(summer));

    {
        Database db;
        QVERIFY(db.open(dbPath(), QStringLiteral("c_v17")));
        QCOMPARE(readUserVersion(dbPath()), Database::currentSchemaVersion());
        // all-tags -> AND, any-tag -> OR, ordered by name; one tag -> the tag.
        QCOMPARE(db.smartAlbum(1).query,
                 oql::Node::andOf({oql::Node::tag(beach, QStringLiteral("beach")),
                                   oql::Node::tag(summer, QStringLiteral("summer")),
                                   oql::Node::tag(zoo, QStringLiteral("Zoo"))}));
        QCOMPARE(db.smartAlbum(2).query,
                 oql::Node::orOf({oql::Node::tag(beach, QStringLiteral("beach")),
                                  oql::Node::tag(zoo, QStringLiteral("Zoo"))}));
        QCOMPARE(db.smartAlbum(3).query, oql::Node::tag(beach, QStringLiteral("beach")));
        // An album whose tags were all deleted converts to an empty AND, which
        // matches nothing — as it did before.
        QCOMPARE(db.smartAlbum(4).query, oql::Node::andOf({}));
        QVERIFY(!oql::matches(db.smartAlbum(4).query, {beach}, {}));
        // The index is untouched by the conversion.
        QCOMPARE(db.smartAlbum(1).tagIds.size(), 3);
        // Rows were converted, so the non-additive step wrote a backup.
        QVERIFY(!backupFiles().isEmpty());
    }
    QSqlDatabase::removeDatabase(QStringLiteral("c_v17"));
}

QTEST_GUILESS_MAIN(TestSchema)
#include "tst_schema.moc"
