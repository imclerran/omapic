// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QtTest>

#include <QDir>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include "database.h"

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

private:
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

QTEST_GUILESS_MAIN(TestSchema)
#include "tst_schema.moc"
