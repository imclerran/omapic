// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#include "database.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QSqlQuery>
#include <QVariant>

#include <algorithm>

namespace {
// The schema version this build writes, stored in PRAGMA user_version. Bump this
// and add a matching migrate_(N)_to_(N+1) step for every schema change. A library
// with a higher version than this is refused by open() (it was written by a newer
// Omapic). See runMigrations.
constexpr int kSchemaVersion = 17;

// Album tags are stored under a reserved, namespaced prefix so an album named
// "Destiny" can't collide with a user tag "Destiny" (tag names are globally
// unique). The prefix is internal — the UI always shows the display name.
const QString kAlbumPrefix = QStringLiteral("_album:");
QString albumStoredName(const QString &display) { return kAlbumPrefix + display.trimmed(); }
QString albumDisplayName(const QString &stored)
{
    return stored.startsWith(kAlbumPrefix) ? stored.mid(kAlbumPrefix.size()) : stored;
}
// Reserved (system) categories get the same treatment, so the hidden Albums
// category can't be confused with — or block — a user category named "Albums"
// (category names are unique too).
const QString kSystemPrefix = QStringLiteral("_system:");
QString systemStoredName(const QString &display) { return kSystemPrefix + display.trimmed(); }
QString systemDisplayName(const QString &stored)
{
    return stored.startsWith(kSystemPrefix) ? stored.mid(kSystemPrefix.size()) : stored;
}

// The characters and shapes a tag, category or album name may not have, fixed
// on the way in (the editors reject them too, and migrations convert any
// pre-existing names):
//  - '|' is the hierarchy separator in the XMP sidecar (Category|Tag), so it
//    would corrupt the exported hierarchy: replaced with '/'.
//  - '"' is the one character the OQL query language can't quote (a quoted
//    name has no escape), so it would make a name unwritable in a query:
//    replaced with the typographic quotes “ ” (opening after whitespace, '('
//    or at the start; closing elsewhere).
//  - Runs of whitespace collapse to a single space and the ends are trimmed,
//    so a query's longest-match name resolution (which folds whitespace the
//    same way) can never tell two names apart by spacing alone.
// Everything else stays legal — spaces, parentheses, colons, a leading '-' or
// '(' — because the query language quotes around them and a ban would break
// sidecar round-trips with other tools' keywords.
QString sanitizeName(const QString &name)
{
    QString s = name;
    s.replace(QLatin1Char('|'), QLatin1Char('/'));
    for (int i = 0; i < s.size(); ++i) {
        if (s.at(i) != QLatin1Char('"'))
            continue;
        const bool opening = i == 0 || s.at(i - 1).isSpace() || s.at(i - 1) == QLatin1Char('(');
        s[i] = opening ? QChar(0x201C) : QChar(0x201D); // “ or ”
    }
    return s.simplified();
}
}

bool Database::open(const QString &path, const QString &connectionName)
{
    m_connectionName = connectionName.isEmpty()
        ? QLatin1String(QSqlDatabase::defaultConnection)
        : connectionName;
    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_db.setDatabaseName(path);
    if (!m_db.open())
        return false;

    // Scoped so the statement is finalized before migrations run — VACUUM INTO
    // (the pre-migration backup) refuses to run with a statement still active.
    {
        QSqlQuery q(m_db);
        q.exec(QStringLiteral("PRAGMA foreign_keys = ON"));
        // WAL lets the worker thread's connection write hashes while the UI thread
        // reads, without either blocking on the other's lock.
        q.exec(QStringLiteral("PRAGMA journal_mode = WAL"));
        q.exec(QStringLiteral("PRAGMA busy_timeout = 5000"));
    }

    // Schema versioning (PRAGMA user_version). Refuse a library written by a newer
    // Omapic rather than risk corrupting it with an older schema; otherwise bring
    // an older or brand-new (version 0) library up to the current schema.
    const int version = userVersion();
    if (version > kSchemaVersion) {
        m_schemaTooNew = true;
        m_db.close(); // do not leave a too-new library partially open
        return false;
    }
    if (version < kSchemaVersion && !runMigrations(version))
        return false;

    // Resolve/cache the reserved Albums category + Favorites album. These ids are
    // per-instance (in-memory), so this runs on every open regardless of version.
    ensureAlbumsCategory();
    ensureFavoritesAlbum();

    // Hashing existing photos and seeding content_tags is slow on a large
    // library, so it is driven from a worker thread (see LibraryWorker), not here.
    return true;
}

Database::OpenStatus Database::openReadOnly(const QString &path, const QString &connectionName)
{
    // SQLite would create an empty file for a missing path; a library that
    // doesn't exist yet is simply not openable here.
    if (path != QStringLiteral(":memory:") && !QFile::exists(path))
        return OpenStatus::CannotOpen;
    m_connectionName = connectionName.isEmpty()
        ? QLatin1String(QSqlDatabase::defaultConnection)
        : connectionName;
    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_db.setDatabaseName(path);
    if (!m_db.open())
        return OpenStatus::CannotOpen;
    {
        QSqlQuery q(m_db);
        q.exec(QStringLiteral("PRAGMA query_only = 1"));
        q.exec(QStringLiteral("PRAGMA busy_timeout = 5000"));
    }
    const int version = userVersion();
    if (version > kSchemaVersion) {
        m_schemaTooNew = true;
        m_db.close();
        return OpenStatus::SchemaNewer;
    }
    if (version < kSchemaVersion) {
        m_db.close();
        return OpenStatus::SchemaOlder;
    }
    resolveReservedIds();
    return OpenStatus::Ok;
}

int Database::currentSchemaVersion()
{
    return kSchemaVersion;
}

int Database::userVersion()
{
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("PRAGMA user_version"));
    return q.next() ? q.value(0).toInt() : 0;
}

void Database::setUserVersion(int version)
{
    // PRAGMA takes no bind parameters, so the (integer) version is interpolated.
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("PRAGMA user_version = %1").arg(version));
}

bool Database::runMigrations(int fromVersion)
{
    struct Step {
        int target;
        void (Database::*apply)();
        bool (Database::*needsBackup)(); // nullptr for a purely additive step
    };
    static const Step steps[] = {
        {1, &Database::migrate_0_to_1, nullptr},
        {2, &Database::migrate_1_to_2, nullptr},
        {3, &Database::migrate_2_to_3, nullptr},
        {4, &Database::migrate_3_to_4, nullptr},
        {5, &Database::migrate_4_to_5, nullptr},
        {6, &Database::migrate_5_to_6, nullptr},
        {7, &Database::migrate_6_to_7, nullptr},
        {8, &Database::migrate_7_to_8, nullptr},
        {9, &Database::migrate_8_to_9, &Database::legacyAlbumsNeedRename},
        {10, &Database::migrate_9_to_10, nullptr},
        {11, &Database::migrate_10_to_11, nullptr},
        {12, &Database::migrate_11_to_12, nullptr},
        {13, &Database::migrate_12_to_13, &Database::namesContainPipe},
        {14, &Database::migrate_13_to_14, nullptr},
        {15, &Database::migrate_14_to_15, nullptr},
        {16, &Database::migrate_15_to_16, &Database::namesNeedNormalizing},
        {17, &Database::migrate_16_to_17, &Database::smartAlbumsNeedConversion},
    };
    for (const Step &s : steps) {
        if (s.target <= fromVersion)
            continue;
        // Back up before a non-additive step that will actually move rows (VACUUM
        // INTO can't run inside a transaction, so it happens before BEGIN).
        if (s.needsBackup && (this->*s.needsBackup)())
            backupDatabase(s.target - 1);
        if (!m_db.transaction())
            return false;
        (this->*s.apply)();
        setUserVersion(s.target); // inside the step's transaction, so it resumes cleanly
        if (!m_db.commit()) {
            m_db.rollback();
            return false;
        }
    }
    return true;
}

// The migration steps below mirror the historical, additive schema ladder; each
// keeps its hasColumn / IF NOT EXISTS guard so re-running it (including running
// the whole chain on an already-current pre-versioning library) is harmless.

void Database::migrate_0_to_1()
{
    QSqlQuery q(m_db);
    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS photos ("
        "  id INTEGER PRIMARY KEY,"
        "  path TEXT UNIQUE NOT NULL,"
        "  added_at INTEGER)"));
    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS tags ("
        "  id INTEGER PRIMARY KEY,"
        "  name TEXT UNIQUE NOT NULL COLLATE NOCASE)"));
    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS photo_tags ("
        "  photo_id INTEGER NOT NULL REFERENCES photos(id) ON DELETE CASCADE,"
        "  tag_id INTEGER NOT NULL REFERENCES tags(id) ON DELETE CASCADE,"
        "  PRIMARY KEY (photo_id, tag_id))"));
    // The PK indexes (photo_id, tag_id); add the reverse so tag->photo lookups
    // (every tag/category query joins photo_tags by tag_id) don't scan the table.
    q.exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_photo_tags_tag ON photo_tags(tag_id)"));
    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS folders ("
        "  id INTEGER PRIMARY KEY,"
        "  path TEXT UNIQUE NOT NULL,"
        "  added_at INTEGER)"));
}

void Database::migrate_1_to_2()
{
    if (!hasColumn(QStringLiteral("folders"), QStringLiteral("enabled"))) {
        QSqlQuery q(m_db);
        q.exec(QStringLiteral("ALTER TABLE folders ADD COLUMN enabled INTEGER NOT NULL DEFAULT 1"));
    }
}

void Database::migrate_2_to_3()
{
    // Content-addressed tags. A photo stores the SHA-256 of its file; content_tags
    // archives tags against that hash so they outlive any particular photo row,
    // path, or inode.
    QSqlQuery q(m_db);
    if (!hasColumn(QStringLiteral("photos"), QStringLiteral("hash")))
        q.exec(QStringLiteral("ALTER TABLE photos ADD COLUMN hash TEXT"));
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_photos_hash ON photos(hash)"));
    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS content_tags ("
        "  hash TEXT NOT NULL,"
        "  tag_id INTEGER NOT NULL REFERENCES tags(id) ON DELETE CASCADE,"
        "  PRIMARY KEY (hash, tag_id))"));
}

void Database::migrate_3_to_4()
{
    // Tag implications: a photo carrying the antecedent tag also gets the
    // consequent tag. Materialized — applying the antecedent writes the
    // consequent into photo_tags/content_tags too. CHECK blocks a tag implying
    // itself; cycles are blocked in the UI.
    QSqlQuery q(m_db);
    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS tag_implications ("
        "  antecedent_id INTEGER NOT NULL REFERENCES tags(id) ON DELETE CASCADE,"
        "  consequent_id INTEGER NOT NULL REFERENCES tags(id) ON DELETE CASCADE,"
        "  PRIMARY KEY (antecedent_id, consequent_id),"
        "  CHECK (antecedent_id <> consequent_id))"));
    // Reverse lookups (closure walks the consequent side too).
    q.exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_tag_implications_consequent "
        "ON tag_implications(consequent_id)"));
}

void Database::migrate_4_to_5()
{
    // Tag categories: a tag may belong to one category, which carries a palette
    // slot. Deleting a category leaves its tags in place, uncategorized.
    QSqlQuery q(m_db);
    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS tag_categories ("
        "  id INTEGER PRIMARY KEY,"
        "  name TEXT UNIQUE NOT NULL COLLATE NOCASE,"
        "  color_slot INTEGER NOT NULL DEFAULT 0)"));
    if (!hasColumn(QStringLiteral("tags"), QStringLiteral("category_id"))) {
        q.exec(QStringLiteral(
            "ALTER TABLE tags ADD COLUMN category_id INTEGER "
            "REFERENCES tag_categories(id) ON DELETE SET NULL"));
    }
}

void Database::migrate_5_to_6()
{
    // Albums reuse the tag system: a reserved "Albums" category (system = 1) whose
    // tags are albums, hidden from every normal tag list. The Favorites album is
    // the one tag also marked system = 1, so it sorts first and is permanent.
    QSqlQuery q(m_db);
    if (!hasColumn(QStringLiteral("tag_categories"), QStringLiteral("system")))
        q.exec(QStringLiteral("ALTER TABLE tag_categories ADD COLUMN system INTEGER NOT NULL DEFAULT 0"));
    if (!hasColumn(QStringLiteral("tags"), QStringLiteral("system")))
        q.exec(QStringLiteral("ALTER TABLE tags ADD COLUMN system INTEGER NOT NULL DEFAULT 0"));
}

void Database::migrate_6_to_7()
{
    // Smart albums: a saved tag query (a set of tags + AND/OR mode). Unlike a
    // regular album they have no stored membership, so they live in their own
    // tables rather than the tag system.
    QSqlQuery q(m_db);
    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS smart_albums ("
        "  id INTEGER PRIMARY KEY,"
        "  name TEXT NOT NULL UNIQUE COLLATE NOCASE,"
        "  match_all INTEGER NOT NULL DEFAULT 1)"));
    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS smart_album_tags ("
        "  smart_album_id INTEGER NOT NULL REFERENCES smart_albums(id) ON DELETE CASCADE,"
        "  tag_id INTEGER NOT NULL REFERENCES tags(id) ON DELETE CASCADE,"
        "  PRIMARY KEY (smart_album_id, tag_id))"));
}

void Database::migrate_7_to_8()
{
    // Capture time in Unix seconds for the timeline view. NULL until backfilled by
    // the worker migration for pre-existing rows (see LibraryWorker).
    QSqlQuery q(m_db);
    if (!hasColumn(QStringLiteral("photos"), QStringLiteral("taken_at")))
        q.exec(QStringLiteral("ALTER TABLE photos ADD COLUMN taken_at INTEGER"));
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_photos_taken_at ON photos(taken_at)"));
}

void Database::migrate_8_to_9()
{
    // Non-additive: the reserved Albums category used to be stored under the bare
    // name "Albums". Move a row created that way (system = 1) to the namespaced
    // "_system:Albums". A user's own "Albums" category (system = 0) is left alone.
    const QString storedName = systemStoredName(QStringLiteral("Albums")); // "_system:Albums"
    QSqlQuery legacy(m_db);
    legacy.prepare(QStringLiteral(
        "SELECT id FROM tag_categories WHERE name = ? AND system = 1"));
    legacy.addBindValue(QStringLiteral("Albums"));
    legacy.exec();
    if (!legacy.next())
        return;
    const int legacyId = legacy.value(0).toInt();
    QSqlQuery cur(m_db);
    cur.prepare(QStringLiteral("SELECT id FROM tag_categories WHERE name = ?"));
    cur.addBindValue(storedName);
    cur.exec();
    if (cur.next()) {
        // Both exist (an older build re-created the bare one): fold the legacy
        // row's albums into the namespaced one and drop it.
        QSqlQuery mv(m_db);
        mv.prepare(QStringLiteral("UPDATE tags SET category_id = ? WHERE category_id = ?"));
        mv.addBindValue(cur.value(0).toInt());
        mv.addBindValue(legacyId);
        mv.exec();
        QSqlQuery del(m_db);
        del.prepare(QStringLiteral("DELETE FROM tag_categories WHERE id = ?"));
        del.addBindValue(legacyId);
        del.exec();
    } else {
        QSqlQuery ren(m_db);
        ren.prepare(QStringLiteral("UPDATE tag_categories SET name = ? WHERE id = ?"));
        ren.addBindValue(storedName);
        ren.addBindValue(legacyId);
        ren.exec();
    }
}

void Database::migrate_9_to_10()
{
    // Additive groundwork for tag export/import: per-photo file stats (so rescan
    // can detect an in-place edit) and the orphaned-hash table (so a later
    // retention policy can prune archive rows with no live photo).
    QSqlQuery q(m_db);
    if (!hasColumn(QStringLiteral("photos"), QStringLiteral("mtime")))
        q.exec(QStringLiteral("ALTER TABLE photos ADD COLUMN mtime INTEGER"));
    if (!hasColumn(QStringLiteral("photos"), QStringLiteral("size")))
        q.exec(QStringLiteral("ALTER TABLE photos ADD COLUMN size INTEGER"));
    if (!hasColumn(QStringLiteral("photos"), QStringLiteral("sidecar_written_at")))
        q.exec(QStringLiteral("ALTER TABLE photos ADD COLUMN sidecar_written_at INTEGER"));
    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS orphaned_hashes ("
        "  hash TEXT PRIMARY KEY,"
        "  orphaned_at INTEGER NOT NULL)"));
    // Seed from hashes already archived but belonging to no live photo. The clock
    // starts now (conservative); idempotent via the primary key.
    QSqlQuery seed(m_db);
    seed.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO orphaned_hashes(hash, orphaned_at) "
        "SELECT DISTINCT hash, ? FROM content_tags "
        "WHERE hash NOT IN (SELECT hash FROM photos WHERE hash IS NOT NULL)"));
    seed.addBindValue(QDateTime::currentSecsSinceEpoch());
    seed.exec();
}

void Database::migrate_10_to_11()
{
    // The sidecar write queue: photos whose tags changed and whose XMP sidecar is
    // due for a rewrite. Drained by the worker's flush. ON DELETE CASCADE drops a
    // photo's pending entry when the row goes away.
    QSqlQuery q(m_db);
    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS sidecar_dirty ("
        "  photo_id INTEGER PRIMARY KEY REFERENCES photos(id) ON DELETE CASCADE)"));
}

void Database::migrate_11_to_12()
{
    // One-time seed of the sidecar write queue: mark every tagged photo that has
    // never been synced to a sidecar (sidecar_written_at IS NULL) as dirty, so a
    // library tagged before sidecar-writing existed gets written once. After this,
    // the queue is the complete record of what needs writing — fed on every tag
    // change — so nothing re-queues a whole folder just because it was re-enabled.
    QSqlQuery q(m_db);
    q.exec(QStringLiteral(
        "INSERT OR IGNORE INTO sidecar_dirty(photo_id) "
        "SELECT DISTINCT pt.photo_id FROM photo_tags pt "
        "JOIN photos p ON p.id = pt.photo_id "
        "WHERE p.sidecar_written_at IS NULL"));
}

bool Database::namesContainPipe()
{
    QSqlQuery q(QStringLiteral(
                    "SELECT 1 FROM tags WHERE name LIKE '%|%' "
                    "UNION ALL SELECT 1 FROM tag_categories WHERE name LIKE '%|%' LIMIT 1"),
                m_db);
    return q.next();
}

void Database::migrate_12_to_13()
{
    // Non-additive: '|' is the hierarchy separator in the XMP sidecar, so it can't
    // appear in a tag or category name. Replace it with '/', merging any name that
    // would collide (case-insensitively, since the name columns are COLLATE NOCASE)
    // into the existing row rather than hitting the UNIQUE constraint.
    QVector<QPair<int, QString>> tags, cats;
    {
        QSqlQuery q(QStringLiteral("SELECT id, name FROM tags WHERE name LIKE '%|%'"), m_db);
        while (q.next())
            tags.append({q.value(0).toInt(), q.value(1).toString()});
    }
    {
        QSqlQuery q(QStringLiteral(
            "SELECT id, name FROM tag_categories WHERE name LIKE '%|%'"), m_db);
        while (q.next())
            cats.append({q.value(0).toInt(), q.value(1).toString()});
    }
    normalizeNameRows(tags, cats);
}

void Database::normalizeNameRows(const QVector<QPair<int, QString>> &tags,
                                 const QVector<QPair<int, QString>> &cats)
{
    // Tags: rename each to its sanitized form, or fold it into the tag that
    // already has that name.
    for (const auto &t : tags) {
        const int oldId = t.first;
        const QString target = sanitizeName(t.second);

        int existing = -1;
        QSqlQuery sel(m_db);
        sel.prepare(QStringLiteral("SELECT id FROM tags WHERE name = ? AND id <> ?"));
        sel.addBindValue(target);
        sel.addBindValue(oldId);
        sel.exec();
        if (sel.next())
            existing = sel.value(0).toInt();

        if (existing < 0) {
            QSqlQuery ren(m_db);
            ren.prepare(QStringLiteral("UPDATE tags SET name = ? WHERE id = ?"));
            ren.addBindValue(target);
            ren.addBindValue(oldId);
            ren.exec();
            continue;
        }

        // The sanitized name collides with another tag; fold this one into it.
        mergeTagInto(oldId, existing);
    }

    // Categories. Only tags.category_id references a category, so the merge is
    // just reassigning that column.
    for (const auto &c : cats) {
        const int oldId = c.first;
        const QString target = sanitizeName(c.second);

        int existing = -1;
        QSqlQuery sel(m_db);
        sel.prepare(QStringLiteral("SELECT id FROM tag_categories WHERE name = ? AND id <> ?"));
        sel.addBindValue(target);
        sel.addBindValue(oldId);
        sel.exec();
        if (sel.next())
            existing = sel.value(0).toInt();

        if (existing < 0) {
            QSqlQuery ren(m_db);
            ren.prepare(QStringLiteral("UPDATE tag_categories SET name = ? WHERE id = ?"));
            ren.addBindValue(target);
            ren.addBindValue(oldId);
            ren.exec();
            continue;
        }
        QSqlQuery mv(m_db);
        mv.prepare(QStringLiteral("UPDATE tags SET category_id = ? WHERE category_id = ?"));
        mv.addBindValue(existing);
        mv.addBindValue(oldId);
        mv.exec();
        QSqlQuery drop(m_db);
        drop.prepare(QStringLiteral("DELETE FROM tag_categories WHERE id = ?"));
        drop.addBindValue(oldId);
        drop.exec();
    }
}

void Database::migrate_13_to_14()
{
    // Videos now take their timeline date from the container creation time (read
    // via ffprobe on the capture-time backfill) instead of the file mtime. Clear
    // the mtime-based dates already stored for video rows so that backfill re-reads
    // them. Non-destructive: the value is re-derivable (mtime stays the fallback),
    // so no backup is needed.
    QSqlQuery q(m_db);
    q.exec(QStringLiteral(
        "UPDATE photos SET taken_at = NULL WHERE "
        "lower(path) LIKE '%.mp4' OR lower(path) LIKE '%.m4v' OR lower(path) LIKE '%.webm' "
        "OR lower(path) LIKE '%.mkv' OR lower(path) LIKE '%.mov' OR lower(path) LIKE '%.avi'"));
}

void Database::migrate_14_to_15()
{
    // Tag aliases: alternate names that resolve to a canonical tag. The name is the
    // primary key (COLLATE NOCASE, like tag names), and ON DELETE CASCADE drops an
    // alias when its canonical tag is deleted. Additive — no backup needed.
    QSqlQuery q(m_db);
    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS tag_aliases ("
        "  alias_name TEXT PRIMARY KEY COLLATE NOCASE,"
        "  tag_id INTEGER NOT NULL REFERENCES tags(id) ON DELETE CASCADE)"));
    q.exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_tag_aliases_tag ON tag_aliases(tag_id)"));
}

QVector<QPair<int, QString>> Database::rowsNeedingNormalizing(const QString &table,
                                                              const QString &idColumn,
                                                              const QString &nameColumn)
{
    QVector<QPair<int, QString>> out;
    QSqlQuery q(QStringLiteral("SELECT %1, %2 FROM %3").arg(idColumn, nameColumn, table), m_db);
    while (q.next()) {
        const QString name = q.value(1).toString();
        if (sanitizeName(name) != name)
            out.append({q.value(0).toInt(), name});
    }
    return out;
}

bool Database::namesNeedNormalizing()
{
    return !rowsNeedingNormalizing(QStringLiteral("tags")).isEmpty()
        || !rowsNeedingNormalizing(QStringLiteral("tag_categories")).isEmpty()
        || !rowsNeedingNormalizing(QStringLiteral("smart_albums")).isEmpty()
        || !rowsNeedingNormalizing(QStringLiteral("tag_aliases"), QStringLiteral("rowid"),
                                   QStringLiteral("alias_name")).isEmpty();
}

void Database::migrate_15_to_16()
{
    // Non-additive: the OQL query language (oql.h) resolves names by longest
    // match with whitespace folded, and quotes a name with '"' when it must, so a
    // name may no longer contain '"' or runs of whitespace (see sanitizeName).
    // Rewrite every tag (albums included — they are tags), category and smart
    // album name to its sanitized form, merging tags and categories whose new
    // name collides with an existing row exactly as the '|' migration did.
    normalizeNameRows(rowsNeedingNormalizing(QStringLiteral("tags")),
                      rowsNeedingNormalizing(QStringLiteral("tag_categories")));
    // Smart albums reference nothing by name, so a collision just needs a
    // distinct name: append " (2)", " (3)", … until it is unique.
    for (const auto &sa : rowsNeedingNormalizing(QStringLiteral("smart_albums"))) {
        const QString base = sanitizeName(sa.second);
        QString target = base;
        for (int n = 2;; ++n) {
            QSqlQuery sel(m_db);
            sel.prepare(QStringLiteral("SELECT 1 FROM smart_albums WHERE name = ? AND id <> ?"));
            sel.addBindValue(target);
            sel.addBindValue(sa.first);
            sel.exec();
            if (!sel.next())
                break;
            target = QStringLiteral("%1 (%2)").arg(base).arg(n);
        }
        QSqlQuery ren(m_db);
        ren.prepare(QStringLiteral("UPDATE smart_albums SET name = ? WHERE id = ?"));
        ren.addBindValue(target);
        ren.addBindValue(sa.first);
        ren.exec();
    }
    // Aliases are plain redirects, so one whose sanitized name is already taken
    // (by another alias or by a tag) is simply dropped.
    for (const auto &a : rowsNeedingNormalizing(QStringLiteral("tag_aliases"), QStringLiteral("rowid"),
                                                QStringLiteral("alias_name"))) {
        const QString target = sanitizeName(a.second);
        const bool taken = aliasExists(target) || tagIdByName(target) >= 0;
        QSqlQuery q(m_db);
        q.prepare(taken ? QStringLiteral("DELETE FROM tag_aliases WHERE rowid = ?")
                        : QStringLiteral("UPDATE tag_aliases SET alias_name = ? WHERE rowid = ?"));
        if (!taken)
            q.addBindValue(target);
        q.addBindValue(a.first);
        q.exec();
    }
}

bool Database::smartAlbumsNeedConversion()
{
    QSqlQuery q(QStringLiteral("SELECT 1 FROM smart_albums LIMIT 1"), m_db);
    return q.next();
}

void Database::migrate_16_to_17()
{
    // Smart albums become OQL queries: a JSON tree of ids in the new `query`
    // column (see oql::toJson) replaces the (tag set + match_all) pair. Each
    // existing album converts to `a b c` or `a | b | c` over its indexed tags,
    // ordered by name so the text reads naturally; smart_album_tags stays as the
    // index of referenced tags. match_all is left in place but no longer read.
    // Non-additive in meaning (the old columns stop being authoritative), so the
    // backup gate above runs when there are rows to convert.
    if (!hasColumn(QStringLiteral("smart_albums"), QStringLiteral("query"))) {
        QSqlQuery add(m_db);
        add.exec(QStringLiteral("ALTER TABLE smart_albums ADD COLUMN query TEXT"));
    }
    QVector<QPair<int, bool>> rows;
    {
        QSqlQuery q(QStringLiteral("SELECT id, match_all FROM smart_albums WHERE query IS NULL"), m_db);
        while (q.next())
            rows.append({q.value(0).toInt(), q.value(1).toInt() != 0});
    }
    for (const auto &row : rows) {
        QVector<oql::Node> leaves;
        QSqlQuery t(m_db);
        t.prepare(QStringLiteral(
            "SELECT t.id, t.name FROM smart_album_tags s JOIN tags t ON t.id = s.tag_id "
            "WHERE s.smart_album_id = ? ORDER BY t.name COLLATE NOCASE"));
        t.addBindValue(row.first);
        t.exec();
        while (t.next())
            leaves.append(oql::Node::tag(t.value(0).toInt(), t.value(1).toString()));
        oql::Node query;
        if (leaves.size() == 1)
            query = leaves.first();
        else
            query = row.second ? oql::Node::andOf(leaves) : oql::Node::orOf(leaves);
        QSqlQuery up(m_db);
        up.prepare(QStringLiteral("UPDATE smart_albums SET query = ? WHERE id = ?"));
        up.addBindValue(QString::fromUtf8(oql::toJsonText(query)));
        up.addBindValue(row.first);
        up.exec();
    }
}

bool Database::legacyAlbumsNeedRename()
{
    if (!hasColumn(QStringLiteral("tag_categories"), QStringLiteral("system")))
        return false;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT 1 FROM tag_categories WHERE name = ? AND system = 1 LIMIT 1"));
    q.addBindValue(QStringLiteral("Albums"));
    q.exec();
    return q.next();
}

void Database::backupDatabase(int fromVersion)
{
    const QString path = m_db.databaseName();
    if (path.isEmpty() || path == QStringLiteral(":memory:"))
        return; // nothing to copy for an in-memory database
    const QString target = QStringLiteral("%1.bak-v%2").arg(path).arg(fromVersion);
    QFile::remove(target); // VACUUM INTO refuses to overwrite an existing file
    QString escaped = target;
    escaped.replace(QLatin1Char('\''), QStringLiteral("''"));
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("VACUUM INTO '%1'").arg(escaped));
    pruneBackups(path);
}

void Database::pruneBackups(const QString &dbPath)
{
    const QFileInfo fi(dbPath);
    QDir dir(fi.absolutePath());
    // QDir::Time sorts newest first; keep the two most recent, delete the rest.
    const QStringList backups = dir.entryList(
        {fi.fileName() + QStringLiteral(".bak-v*")}, QDir::Files, QDir::Time);
    for (int i = 2; i < backups.size(); ++i)
        QFile::remove(dir.absoluteFilePath(backups.at(i)));
}

void Database::ensureAlbumsCategory()
{
    // Resolve/create + cache the reserved Albums category. The one-time rename of
    // the old bare "Albums" name lives in migrate_8_to_9; this only ensures the
    // namespaced row exists and caches its id, so it is safe to run every open.
    const QString storedName = systemStoredName(QStringLiteral("Albums")); // "_system:Albums"
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO tag_categories(name, color_slot, system) VALUES(?, 0, 1)"));
    q.addBindValue(storedName);
    q.exec();
    // Keep the reserved category marked system, whatever created the row.
    QSqlQuery u(m_db);
    u.prepare(QStringLiteral("UPDATE tag_categories SET system = 1 WHERE name = ?"));
    u.addBindValue(storedName);
    u.exec();
    QSqlQuery sel(m_db);
    sel.prepare(QStringLiteral("SELECT id FROM tag_categories WHERE name = ?"));
    sel.addBindValue(storedName);
    sel.exec();
    if (sel.next())
        m_albumCategoryId = sel.value(0).toInt();
}

void Database::resolveReservedIds()
{
    QSqlQuery c(m_db);
    c.prepare(QStringLiteral("SELECT id FROM tag_categories WHERE name = ?"));
    c.addBindValue(systemStoredName(QStringLiteral("Albums")));
    c.exec();
    if (c.next())
        m_albumCategoryId = c.value(0).toInt();
    QSqlQuery f(m_db);
    f.prepare(QStringLiteral("SELECT id FROM tags WHERE name = ?"));
    f.addBindValue(albumStoredName(QStringLiteral("Favorites")));
    f.exec();
    if (f.next())
        m_favoritesTagId = f.value(0).toInt();
}

void Database::ensureFavoritesAlbum()
{
    if (m_albumCategoryId < 0)
        return;
    const QString favName = albumStoredName(QStringLiteral("Favorites")); // "_album:Favorites"
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO tags(name, category_id, system) VALUES(?, ?, 1)"));
    q.addBindValue(favName);
    q.addBindValue(m_albumCategoryId);
    q.exec();
    // Keep the reserved favorites tag in the Albums category and marked system.
    QSqlQuery u(m_db);
    u.prepare(QStringLiteral(
        "UPDATE tags SET category_id = ?, system = 1 WHERE name = ?"));
    u.addBindValue(m_albumCategoryId);
    u.addBindValue(favName);
    u.exec();
    QSqlQuery sel(m_db);
    sel.prepare(QStringLiteral("SELECT id FROM tags WHERE name = ?"));
    sel.addBindValue(favName);
    sel.exec();
    if (sel.next())
        m_favoritesTagId = sel.value(0).toInt();
}

namespace {
// Column list + join shared by every tag query, so each TagInfo carries its
// category. The tags table must be aliased `t`.
const char *const kTagCategoryColumns = "c.id, c.name, c.color_slot";
const char *const kTagCategoryJoin = "LEFT JOIN tag_categories c ON c.id = t.category_id ";

// Fills the category fields from a row laid out as (..., c.id, c.name, c.color_slot)
// starting at column `first`.
void readCategory(const QSqlQuery &q, int first, TagInfo &info)
{
    if (q.value(first).isNull())
        return;
    info.categoryId = q.value(first).toInt();
    info.categoryName = q.value(first + 1).toString();
    info.colorSlot = q.value(first + 2).toInt();
}
}

bool Database::beginTransaction()
{
    return m_db.transaction();
}

bool Database::commitTransaction()
{
    return m_db.commit();
}

bool Database::hasColumn(const QString &table, const QString &column)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("PRAGMA table_info(%1)").arg(table));
    q.exec();
    while (q.next()) {
        if (q.value(1).toString().compare(column, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

int Database::unhashedPhotoCount()
{
    QSqlQuery q(QStringLiteral("SELECT COUNT(*) FROM photos WHERE hash IS NULL OR hash = ''"), m_db);
    if (q.next())
        return q.value(0).toInt();
    return 0;
}

QVector<QPair<int, QString>> Database::photosMissingHash()
{
    QVector<QPair<int, QString>> todo;
    QSqlQuery q(QStringLiteral("SELECT id, path FROM photos WHERE hash IS NULL OR hash = ''"), m_db);
    while (q.next())
        todo.append({q.value(0).toInt(), q.value(1).toString()});
    return todo;
}

void Database::setPhotoHash(int photoId, const QString &hash)
{
    if (hash.isEmpty())
        return;
    QSqlQuery u(m_db);
    u.prepare(QStringLiteral("UPDATE photos SET hash = ? WHERE id = ?"));
    u.addBindValue(hash);
    u.addBindValue(photoId);
    u.exec();
}

void Database::setSidecarWrittenAt(int photoId, qint64 whenSecs)
{
    QSqlQuery u(m_db);
    u.prepare(QStringLiteral("UPDATE photos SET sidecar_written_at = ? WHERE id = ?"));
    u.addBindValue(whenSecs > 0 ? QVariant(whenSecs) : QVariant());
    u.addBindValue(photoId);
    u.exec();
}

qint64 Database::sidecarWrittenAt(int photoId)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT sidecar_written_at FROM photos WHERE id = ?"));
    q.addBindValue(photoId);
    q.exec();
    if (q.next() && !q.value(0).isNull())
        return q.value(0).toLongLong();
    return -1;
}

QString Database::photoPath(int photoId)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT path FROM photos WHERE id = ?"));
    q.addBindValue(photoId);
    q.exec();
    if (q.next())
        return q.value(0).toString();
    return {};
}

void Database::enqueueSidecarDirty(const QList<int> &photoIds)
{
    if (photoIds.isEmpty())
        return;
    // One transaction, not one commit (fsync) per row — a whole-folder enqueue can
    // be thousands of ids. Guarded so it's harmless if a caller already has a
    // transaction open.
    const bool txn = m_db.transaction();
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT OR IGNORE INTO sidecar_dirty(photo_id) VALUES(?)"));
    for (int id : photoIds) {
        if (id < 0)
            continue;
        q.addBindValue(id);
        q.exec();
    }
    if (txn)
        m_db.commit();
}

void Database::enqueueSidecarDirtyForTag(int tagId)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO sidecar_dirty(photo_id) "
        "SELECT photo_id FROM photo_tags WHERE tag_id = ?"));
    q.addBindValue(tagId);
    q.exec();
}

QList<int> Database::sidecarDirtyIds()
{
    QList<int> ids;
    QSqlQuery q(QStringLiteral("SELECT photo_id FROM sidecar_dirty ORDER BY photo_id"), m_db);
    while (q.next())
        ids.append(q.value(0).toInt());
    return ids;
}

int Database::sidecarDirtyCount()
{
    QSqlQuery q(QStringLiteral("SELECT COUNT(*) FROM sidecar_dirty"), m_db);
    return q.next() ? q.value(0).toInt() : 0;
}

int Database::rememberedRemovedCount()
{
    QSqlQuery q(QStringLiteral("SELECT COUNT(*) FROM orphaned_hashes"), m_db);
    return q.next() ? q.value(0).toInt() : 0;
}

void Database::pruneOrphanedTags(qint64 cutoffEpoch)
{
    const bool txn = m_db.transaction();
    QSqlQuery del(m_db);
    del.prepare(QStringLiteral(
        "DELETE FROM content_tags WHERE hash IN "
        "(SELECT hash FROM orphaned_hashes WHERE orphaned_at < ?)"));
    del.addBindValue(cutoffEpoch);
    del.exec();

    QSqlQuery delOrphans(m_db);
    delOrphans.prepare(QStringLiteral("DELETE FROM orphaned_hashes WHERE orphaned_at < ?"));
    delOrphans.addBindValue(cutoffEpoch);
    delOrphans.exec();
    if (txn)
        m_db.commit();
}

void Database::dequeueSidecarDirty(int photoId)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM sidecar_dirty WHERE photo_id = ?"));
    q.addBindValue(photoId);
    q.exec();
}

void Database::sidecarTagsForPhoto(int photoId, QString &hash, QStringList &flat,
                                   QList<QPair<QString, QString>> &categorized,
                                   QStringList &albums, bool &favorite)
{
    hash = photoHash(photoId);
    flat.clear();
    categorized.clear();
    albums.clear();
    favorite = false;

    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT t.id, t.name, t.category_id, c.name "
        "FROM photo_tags pt JOIN tags t ON t.id = pt.tag_id "
        "LEFT JOIN tag_categories c ON c.id = t.category_id "
        "WHERE pt.photo_id = ? ORDER BY t.name"));
    q.addBindValue(photoId);
    q.exec();
    while (q.next()) {
        const int tagId = q.value(0).toInt();
        const QString name = q.value(1).toString();
        const int categoryId = q.value(2).isNull() ? -1 : q.value(2).toInt();
        const QString categoryName = q.value(3).toString();

        if (tagId == m_favoritesTagId) {
            favorite = true;
        } else if (categoryId >= 0 && categoryId == m_albumCategoryId) {
            albums << albumDisplayName(name); // strip the "_album:" prefix
        } else {
            flat << name;
            if (categoryId >= 0)
                categorized.append({categoryName, name});
            else
                categorized.append({QString(), name});
        }
    }
}

int Database::photosMissingTakenAtCount()
{
    QSqlQuery q(QStringLiteral("SELECT COUNT(*) FROM photos WHERE taken_at IS NULL"), m_db);
    if (q.next())
        return q.value(0).toInt();
    return 0;
}

QVector<QPair<int, QString>> Database::photosMissingTakenAt()
{
    QVector<QPair<int, QString>> todo;
    QSqlQuery q(QStringLiteral("SELECT id, path FROM photos WHERE taken_at IS NULL"), m_db);
    while (q.next())
        todo.append({q.value(0).toInt(), q.value(1).toString()});
    return todo;
}

void Database::setPhotoTakenAt(int photoId, qint64 takenAt)
{
    // Write the value even when 0 (file unreadable): the row is then no longer
    // NULL, so the backfill doesn't keep retrying it on every launch.
    QSqlQuery u(m_db);
    u.prepare(QStringLiteral("UPDATE photos SET taken_at = ? WHERE id = ?"));
    u.addBindValue(takenAt);
    u.addBindValue(photoId);
    u.exec();
}

QStringList Database::activePhotoPaths()
{
    QStringList paths;
    QSqlQuery q(QStringLiteral("SELECT p.path FROM photos p WHERE %1 ORDER BY p.path")
                    .arg(activeClause(QStringLiteral("p"))),
                m_db);
    while (q.next())
        paths.append(q.value(0).toString());
    return paths;
}

QStringList Database::activePhotoPaths(const QList<int> &selectedTags, bool matchAll)
{
    if (selectedTags.isEmpty())
        return activePhotoPaths();

    QStringList ph;
    for (int i = 0; i < selectedTags.size(); ++i)
        ph.append(QStringLiteral("?"));
    const QString inList = ph.join(QLatin1Char(','));
    // match-all: the photo must carry every selected tag (count of distinct
    // matched tags equals the selection size). match-any: carrying one is enough.
    const QString having = matchAll
        ? QStringLiteral(" GROUP BY pt.photo_id HAVING COUNT(DISTINCT pt.tag_id) = %1")
              .arg(selectedTags.size())
        : QString();

    QStringList paths;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
                  "SELECT p.path FROM photos p WHERE %1 AND p.id IN ("
                  "  SELECT pt.photo_id FROM photo_tags pt WHERE pt.tag_id IN (%2)%3) "
                  "ORDER BY p.path")
                  .arg(activeClause(QStringLiteral("p")), inList, having));
    for (int id : selectedTags)
        q.addBindValue(id);
    q.exec();
    while (q.next())
        paths.append(q.value(0).toString());
    return paths;
}

QVector<TagSampleCandidate> Database::tagSampleCandidates(int tagId, int poolSize)
{
    QVector<TagSampleCandidate> pool;
    if (tagId < 0 || poolSize <= 0)
        return pool;

    // Every active photo carrying the tag, with its favorite flag and its other
    // user-visible tags (as a packed id list), ranked by standalone score: the
    // count of those tags plus the favorite bonus, then newest capture, then id
    // for a stable order. A richly tagged photo is likelier to be a well-chosen,
    // representative one. Only the top of that ranking is handed back — the
    // greedy picker (pickTagSamples) can't do better than a candidate's own score,
    // so a small pool loses nothing worth showing.
    QSqlQuery q(m_db);
    q.setForwardOnly(true);
    q.prepare(QStringLiteral(
                  "WITH cand AS ("
                  "  SELECT p.id AS id, p.path AS path, p.hash AS hash, "
                  "    COALESCE(p.taken_at, 0) AS taken, "
                  "    EXISTS (SELECT 1 FROM photo_tags fv "
                  "      WHERE fv.photo_id = p.id AND fv.tag_id = ?) AS fav, "
                  "    (SELECT COUNT(*) FROM photo_tags c JOIN tags t ON t.id = c.tag_id "
                  "      WHERE c.photo_id = p.id AND c.tag_id <> ? AND %2) AS n, "
                  "    (SELECT group_concat(c.tag_id) FROM photo_tags c JOIN tags t ON t.id = c.tag_id "
                  "      WHERE c.photo_id = p.id AND c.tag_id <> ? AND %2) AS tag_ids "
                  "  FROM photos p "
                  "  WHERE %1 AND EXISTS (SELECT 1 FROM photo_tags pt "
                  "    WHERE pt.photo_id = p.id AND pt.tag_id = ?)) "
                  "SELECT id, path, hash, taken, fav, tag_ids FROM cand "
                  "ORDER BY n + fav * ? DESC, taken DESC, id LIMIT ?")
                  .arg(activeClause(QStringLiteral("p")),
                       nonSystemTagClause(QStringLiteral("t"))));
    q.addBindValue(m_favoritesTagId);
    q.addBindValue(tagId);
    q.addBindValue(tagId);
    q.addBindValue(tagId);
    q.addBindValue(kFavoriteSampleBonus);
    q.addBindValue(poolSize);
    q.exec();

    while (q.next()) {
        TagSampleCandidate c;
        c.id = q.value(0).toInt();
        c.path = q.value(1).toString();
        c.hash = q.value(2).toString();
        c.takenAt = q.value(3).toLongLong();
        c.favorite = q.value(4).toInt() != 0;
        const QString packed = q.value(5).toString();
        if (!packed.isEmpty()) {
            for (QStringView part : QStringView(packed).split(QLatin1Char(',')))
                c.tagIds.insert(part.toInt());
        }
        pool.append(c);
    }
    return pool;
}

QStringList Database::pickTagSamples(const QVector<TagSampleCandidate> &pool, int limit)
{
    // Duplicate copies of one image (same content hash) carry the same tags, so
    // only the first by pool order competes; the rest would merely repeat it.
    QVector<const TagSampleCandidate *> cands;
    QSet<QString> seenHashes;
    for (const TagSampleCandidate &c : pool) {
        if (!c.hash.isEmpty()) {
            if (seenHashes.contains(c.hash))
                continue;
            seenHashes.insert(c.hash);
        }
        cands.append(&c);
    }

    // Greedy coverage: each round takes the candidate that adds the most tags
    // not yet covered by the picks so far (plus the favorite bonus), so the set
    // as a whole shows as many of the tag's companions as it can. The pool is
    // already in standalone-score order (then newest, then id), and only a
    // strictly better score displaces the earlier candidate, so ties fall back
    // to exactly that order.
    QStringList out;
    QSet<int> covered;
    QVector<bool> used(cands.size(), false);
    while (out.size() < limit) {
        int best = -1;
        int bestScore = -1;
        for (int i = 0; i < cands.size(); ++i) {
            if (used[i])
                continue;
            int score = cands[i]->favorite ? kFavoriteSampleBonus : 0;
            for (int t : cands[i]->tagIds) {
                if (!covered.contains(t))
                    ++score;
            }
            if (score > bestScore) {
                bestScore = score;
                best = i;
            }
        }
        if (best < 0)
            break;
        used[best] = true;
        out.append(cands[best]->path);
        covered.unite(cands[best]->tagIds);
    }
    return out;
}

QStringList Database::tagSamplePaths(int tagId, int limit)
{
    if (tagId < 0 || limit <= 0)
        return {};
    // A pool a good deal larger than the request gives the diversity pass real
    // alternatives to the top-scoring photos (and spare rows for duplicates).
    constexpr int kPoolSize = 24;
    return pickTagSamples(tagSampleCandidates(tagId, qMax(kPoolSize, limit)), limit);
}

QList<int> Database::activePhotoIds()
{
    QList<int> ids;
    QSqlQuery q(QStringLiteral("SELECT p.id FROM photos p WHERE %1 ORDER BY p.id")
                    .arg(activeClause(QStringLiteral("p"))),
                m_db);
    while (q.next())
        ids.append(q.value(0).toInt());
    return ids;
}

bool Database::isPhotoActive(int photoId)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT 1 FROM photos p WHERE p.id = ? AND %1")
                  .arg(activeClause(QStringLiteral("p"))));
    q.addBindValue(photoId);
    q.exec();
    return q.next();
}

void Database::backfillContentTags()
{
    QSqlQuery q(m_db);
    q.exec(QStringLiteral(
        "INSERT OR IGNORE INTO content_tags(hash, tag_id) "
        "SELECT p.hash, pt.tag_id FROM photo_tags pt "
        "JOIN photos p ON p.id = pt.photo_id "
        "WHERE p.hash IS NOT NULL AND p.hash <> ''"));
}

QString Database::activeClause(const QString &alias) const
{
    // Active = not contained by any folder (an orphan), or contained by at least
    // one enabled folder. Prefix match so nested subfolders count.
    return QStringLiteral(
        "(NOT EXISTS (SELECT 1 FROM folders f WHERE %1.path = f.path"
        "   OR substr(%1.path, 1, length(f.path) + 1) = f.path || '/')"
        " OR EXISTS (SELECT 1 FROM folders f WHERE f.enabled = 1 AND (%1.path = f.path"
        "   OR substr(%1.path, 1, length(f.path) + 1) = f.path || '/')))")
        .arg(alias);
}

QString Database::nonSystemTagClause(const QString &alias) const
{
    // A tag is user-visible unless it lives in a reserved (system) category —
    // which is how album tags and the Favorites tag are hidden from every normal
    // tag list, autocomplete, and the detail panel.
    return QStringLiteral(
        "(%1.category_id IS NULL OR %1.category_id NOT IN "
        "(SELECT id FROM tag_categories WHERE system = 1))")
        .arg(alias);
}

QVector<Photo> Database::loadPhotos(bool activeOnly)
{
    QHash<int, int> indexOf;
    QVector<Photo> photos;

    // Load every active photo, ordered by (added_at, id). Duplicates are not
    // dropped here: each photo is tagged isDuplicate when an earlier active photo
    // shares its content hash, and only the ungrouped view hides those — timeline
    // and folder views show every copy. Walking in added order, the first photo
    // seen for a hash is the representative and the rest are duplicates; this
    // mirrors the old SQL (earliest by added_at, then id) while keeping the copies
    // in the model. Photos whose hash isn't computed yet (NULL/empty) are never
    // duplicates, so nothing hides before background hashing catches up. Because
    // only active photos are loaded, disabling the folder holding the kept copy
    // promotes a still-visible duplicate to representative on the next reload.
    QSet<QString> seenHashes;
    QSqlQuery q(m_db);
    q.setForwardOnly(true); // stream rows, don't cache — this is a hot launch query
    q.exec(QStringLiteral(
               "SELECT p.id, p.path, p.taken_at, p.hash FROM photos p WHERE %1 "
               "ORDER BY p.added_at, p.id")
               .arg(activeOnly ? activeClause(QStringLiteral("p")) : QStringLiteral("1")));
    while (q.next()) {
        Photo p;
        p.id = q.value(0).toInt();
        p.path = q.value(1).toString();
        p.takenAt = q.value(2).isNull() ? 0 : q.value(2).toLongLong();
        const QString hash = q.value(3).toString();
        if (!hash.isEmpty()) {
            p.isDuplicate = seenHashes.contains(hash); // an earlier copy was seen
            seenHashes.insert(hash);
        }
        p.fileName = p.path.mid(p.path.lastIndexOf(QLatin1Char('/')) + 1);
        p.isVideo = isVideoPath(p.path);
        indexOf.insert(p.id, photos.size());
        photos.append(p);
    }

    // The hidden album/favorites tags live in a "system" category; they belong in
    // tagIds (the album filter and the star need them) but not in tagNames (never
    // a visible chip or badge). Resolve that set once here instead of testing a
    // correlated subquery for every photo_tags row below.
    QSet<int> systemTagIds;
    {
        QSqlQuery s(QStringLiteral(
                        "SELECT id FROM tags WHERE category_id IN "
                        "(SELECT id FROM tag_categories WHERE system = 1)"),
                    m_db);
        while (s.next())
            systemTagIds.insert(s.value(0).toInt());
    }

    // Distribute tags onto photos. Aggregate into one row per photo — each tag
    // packed as "id<RS>name" joined by <US> — so a ~10k-row result crosses the Qt
    // SQL boundary instead of the ~64k-row raw join (the per-row value() marshalling
    // was this query's cost). The pairing is self-contained per element, so the
    // group's arbitrary order is harmless; the chip order is restored in memory.
    constexpr QChar kUnit(u'\x1f');   // between tags
    constexpr QChar kRecord(u'\x1e'); // between a tag's id and name
    QSqlQuery t(m_db);
    t.setForwardOnly(true);
    t.exec(QStringLiteral(
               "SELECT pt.photo_id, "
               "  group_concat(t.id || char(30) || t.name, char(31)) "
               "FROM photo_tags pt JOIN tags t ON t.id = pt.tag_id "
               "GROUP BY pt.photo_id"));
    while (t.next()) {
        const int pid = t.value(0).toInt();
        auto it = indexOf.constFind(pid);
        if (it == indexOf.constEnd())
            continue;
        Photo &p = photos[it.value()];
        const QString packed = t.value(1).toString();
        for (QStringView entry : QStringView(packed).split(kUnit)) {
            const qsizetype sep = entry.indexOf(kRecord);
            if (sep < 0)
                continue;
            const int tagId = entry.left(sep).toInt();
            p.tagIds.insert(tagId);
            if (!systemTagIds.contains(tagId))
                p.tagNames.append(entry.mid(sep + 1).toString());
            if (tagId == m_favoritesTagId)
                p.favorite = true;
        }
    }
    for (Photo &p : photos) {
        if (p.tagNames.size() > 1)
            std::sort(p.tagNames.begin(), p.tagNames.end(),
                      [](const QString &a, const QString &b) {
                          return a.compare(b, Qt::CaseInsensitive) < 0;
                      });
    }

    return photos;
}

QVector<TagInfo> Database::loadTags()
{
    // Tags that apply to at least one photo in an active (enabled, non-removed)
    // folder, so the tag manager hides tags living only on hidden-folder or
    // removed-folder images. The count is still the tag's total across all
    // photos, so the delete confirmation reflects everything it would remove.
    // Resolve the active-photo set once (over the photos table), then the active
    // tags from it, rather than testing the active-folder clause per tag with a
    // correlated EXISTS: that clause uses substr() (not index-friendly), so the
    // old form re-scanned photo_tags for every tag and was very slow on large
    // libraries. The count is still the tag's total across all photos (outer LEFT
    // JOIN). Relies on idx_photo_tags_tag for the tag->photo lookups.
    QVector<TagInfo> tags;
    QSqlQuery q(QStringLiteral(
                    "WITH active_photos AS (SELECT p.id FROM photos p WHERE %1), "
                    "active_tags AS (SELECT DISTINCT pt.tag_id FROM photo_tags pt "
                    "  WHERE pt.photo_id IN (SELECT id FROM active_photos)) "
                    "SELECT t.id, t.name, COUNT(pt.photo_id), %2 "
                    "FROM tags t LEFT JOIN photo_tags pt ON pt.tag_id = t.id "
                    "%3"
                    "WHERE t.id IN (SELECT tag_id FROM active_tags) AND %4 "
                    "GROUP BY t.id, t.name ORDER BY t.name")
                    .arg(activeClause(QStringLiteral("p")),
                         QLatin1String(kTagCategoryColumns),
                         QLatin1String(kTagCategoryJoin),
                         nonSystemTagClause(QStringLiteral("t"))),
                m_db);
    while (q.next()) {
        TagInfo info;
        info.id = q.value(0).toInt();
        info.name = q.value(1).toString();
        info.count = q.value(2).toInt();
        readCategory(q, 3, info);
        tags.append(info);
    }
    return tags;
}

QString Database::photoScopeClause(const QList<int> &scopeIds, bool scoped)
{
    if (!scoped)
        return {};
    if (scopeIds.isEmpty())
        return QStringLiteral(" AND 0");
    QStringList ids;
    ids.reserve(scopeIds.size());
    for (int id : scopeIds)
        ids.append(QString::number(id));
    return QStringLiteral(" AND p.id IN (%1)").arg(ids.join(QLatin1Char(',')));
}

QVector<TagInfo> Database::activeTags(const QString &folderScope, const QList<int> &scopeIds,
                                      bool scoped)
{
    // Tags that apply to at least one photo in the active (shown) set, counted
    // over that set only. Drives the filter sidebar and tag autocomplete.
    // Active-photo set first (10.6k-row photos scan), then the tags on it — much
    // cheaper than joining photos and testing the substr() clause across all of
    // photo_tags. Counts are over the active set. Uses idx_photo_tags_tag.
    //
    // folderScope (folder view) additionally restricts the active photos to those
    // at or under that directory — the subtree prefix match mirrors activeClause.
    const QString scope = (folderScope.isEmpty()
        ? QString()
        : QStringLiteral(" AND substr(p.path, 1, length(?) + 1) = ? || '/'"))
        + photoScopeClause(scopeIds, scoped);
    QVector<TagInfo> tags;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
                  "WITH active_photos AS (SELECT p.id FROM photos p WHERE %1%4) "
                  "SELECT t.id, t.name, COUNT(pt.photo_id), %2 "
                  "FROM tags t "
                  "JOIN photo_tags pt ON pt.tag_id = t.id "
                  "%3"
                  "WHERE pt.photo_id IN (SELECT id FROM active_photos) AND %5 "
                  "GROUP BY t.id, t.name HAVING COUNT(pt.photo_id) > 0 "
                  "ORDER BY t.name")
                  .arg(activeClause(QStringLiteral("p")),
                       QLatin1String(kTagCategoryColumns),
                       QLatin1String(kTagCategoryJoin), scope,
                       nonSystemTagClause(QStringLiteral("t"))));
    if (!folderScope.isEmpty()) {
        q.addBindValue(folderScope);
        q.addBindValue(folderScope);
    }
    q.exec();
    while (q.next()) {
        TagInfo info;
        info.id = q.value(0).toInt();
        info.name = q.value(1).toString();
        info.count = q.value(2).toInt();
        readCategory(q, 3, info);
        tags.append(info);
    }
    return tags;
}

QVector<TagInfo> Database::coOccurringTags(const QList<int> &selected, const QString &folderScope,
                                           const QList<int> &scopeIds, bool scoped)
{
    QVector<TagInfo> tags;
    if (selected.isEmpty())
        return activeTags(folderScope, scopeIds, scoped);

    QStringList ph;
    for (int i = 0; i < selected.size(); ++i)
        ph.append(QStringLiteral("?"));
    const QString inList = ph.join(QLatin1Char(','));

    // The folder-view scope restricts the active photos to a directory subtree;
    // its bind lives in the active_photos CTE, which precedes the selected-tag
    // placeholders in the SQL, so it is bound first.
    const QString scope = (folderScope.isEmpty()
        ? QString()
        : QStringLiteral(" AND substr(p.path, 1, length(?) + 1) = ? || '/'"))
        + photoScopeClause(scopeIds, scoped);

    // Matching photos: active photos that carry every selected tag. Then return
    // the tags present on those photos, counted over that matching set.
    const QString sql = QStringLiteral(
        "WITH active_photos AS (SELECT p.id FROM photos p WHERE %1%6) "
        "SELECT t.id, t.name, COUNT(DISTINCT pt.photo_id), %4 "
        "FROM tags t "
        "JOIN photo_tags pt ON pt.tag_id = t.id "
        "%5"
        "WHERE pt.photo_id IN (SELECT id FROM active_photos) AND pt.photo_id IN ("
        "  SELECT pt2.photo_id FROM photo_tags pt2 "
        "  WHERE pt2.tag_id IN (%2) "
        "  GROUP BY pt2.photo_id HAVING COUNT(DISTINCT pt2.tag_id) = %3) "
        "  AND %7 "
        "GROUP BY t.id, t.name HAVING COUNT(DISTINCT pt.photo_id) > 0 "
        "ORDER BY t.name")
        .arg(activeClause(QStringLiteral("p")), inList, QString::number(selected.size()),
             QLatin1String(kTagCategoryColumns), QLatin1String(kTagCategoryJoin), scope,
             nonSystemTagClause(QStringLiteral("t")));

    QSqlQuery q(m_db);
    q.prepare(sql);
    if (!folderScope.isEmpty()) {
        q.addBindValue(folderScope);
        q.addBindValue(folderScope);
    }
    for (int id : selected)
        q.addBindValue(id);
    q.exec();
    while (q.next()) {
        TagInfo info;
        info.id = q.value(0).toInt();
        info.name = q.value(1).toString();
        info.count = q.value(2).toInt();
        readCategory(q, 3, info);
        tags.append(info);
    }
    return tags;
}

QVector<TagInfo> Database::facetTags(const QString &folderScope, const QList<int> &selected,
                                     bool matchAll, const QList<int> &scopeIds, bool scoped)
{
    const QVector<TagInfo> active = activeTags(folderScope, scopeIds, scoped);
    QHash<int, TagInfo> activeById;
    for (const TagInfo &t : active)
        activeById.insert(t.id, t);
    // A selected tag whose photos are all outside the current folder or query
    // scope won't be in `active`; pull it from the unscoped active set so it
    // stays deselectable.
    if ((!folderScope.isEmpty() || scoped) && !selected.isEmpty()) {
        for (const TagInfo &t : activeTags())
            if (!activeById.contains(t.id))
                activeById.insert(t.id, t);
    }

    QVector<TagInfo> tags = (matchAll && !selected.isEmpty())
        ? coOccurringTags(selected, folderScope, scopeIds, scoped) : active;

    // Keep a selected tag visible even once it no longer applies to any active
    // photo, so an incompatible match-all pick stays deselectable (count 0).
    QSet<int> have;
    for (const TagInfo &t : tags)
        have.insert(t.id);
    for (int id : selected) {
        if (!have.contains(id) && activeById.contains(id)) {
            TagInfo t = activeById.value(id); // keeps the category for coloring
            t.count = 0;
            tags.append(t);
        }
    }
    std::sort(tags.begin(), tags.end(), [](const TagInfo &a, const TagInfo &b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    return tags;
}

QVector<FolderInfo> Database::loadFolders()
{
    QVector<FolderInfo> folders;
    QSqlQuery q(QStringLiteral("SELECT id, path, enabled FROM folders ORDER BY path"), m_db);
    while (q.next()) {
        FolderInfo info;
        info.id = q.value(0).toInt();
        info.path = q.value(1).toString();
        info.enabled = q.value(2).toInt() != 0;

        // Count photos in the library that live in or under this folder.
        const QString prefix = info.path + QStringLiteral("/");
        QSqlQuery c(m_db);
        c.prepare(QStringLiteral(
            "SELECT COUNT(*) FROM photos WHERE path = ? OR substr(path, 1, ?) = ?"));
        c.addBindValue(info.path);
        c.addBindValue(prefix.size());
        c.addBindValue(prefix);
        c.exec();
        if (c.next())
            info.count = c.value(0).toInt();

        folders.append(info);
    }
    return folders;
}

int Database::insertFolder(const QString &path)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT OR IGNORE INTO folders(path, added_at) VALUES(?, ?)"));
    q.addBindValue(path);
    q.addBindValue(QDateTime::currentSecsSinceEpoch());
    q.exec();

    QSqlQuery sel(m_db);
    sel.prepare(QStringLiteral("SELECT id FROM folders WHERE path = ?"));
    sel.addBindValue(path);
    sel.exec();
    if (sel.next())
        return sel.value(0).toInt();
    return -1;
}

QString Database::folderPath(int folderId)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT path FROM folders WHERE id = ?"));
    q.addBindValue(folderId);
    q.exec();
    if (q.next())
        return q.value(0).toString();
    return {};
}

void Database::removeFolder(int folderId)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM folders WHERE id = ?"));
    q.addBindValue(folderId);
    q.exec();
}

void Database::setFolderEnabled(int folderId, bool enabled)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE folders SET enabled = ? WHERE id = ?"));
    q.addBindValue(enabled ? 1 : 0);
    q.addBindValue(folderId);
    q.exec();
}

void Database::deletePhotosUnderPath(const QString &folderPath)
{
    // Prefix match on the path so nested subfolders go too; photo_tags rows
    // cascade away via the foreign key.
    const QString prefix = folderPath + QStringLiteral("/");

    // Capture the affected content hashes first so we can mark any that become
    // orphaned (archive kept) after the rows go.
    QStringList hashes;
    QSqlQuery sel(m_db);
    sel.prepare(QStringLiteral(
        "SELECT DISTINCT hash FROM photos "
        "WHERE hash IS NOT NULL AND (path = ? OR substr(path, 1, ?) = ?)"));
    sel.addBindValue(folderPath);
    sel.addBindValue(prefix.size());
    sel.addBindValue(prefix);
    sel.exec();
    while (sel.next())
        hashes.append(sel.value(0).toString());

    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "DELETE FROM photos WHERE path = ? OR substr(path, 1, ?) = ?"));
    q.addBindValue(folderPath);
    q.addBindValue(prefix.size());
    q.addBindValue(prefix);
    q.exec();

    for (const QString &h : hashes)
        recordOrphanIfUnreferenced(h);
}

QStringList Database::allTagNames()
{
    // Only tags relevant to the active set, so autocomplete can't suggest a tag
    // that applies to nothing currently shown.
    QStringList out;
    QSqlQuery q(QStringLiteral(
                    "SELECT DISTINCT t.name FROM tags t "
                    "JOIN photo_tags pt ON pt.tag_id = t.id "
                    "JOIN photos p ON p.id = pt.photo_id "
                    "WHERE %1 AND %2 ORDER BY t.name")
                    .arg(activeClause(QStringLiteral("p")),
                         nonSystemTagClause(QStringLiteral("t"))),
                m_db);
    while (q.next())
        out.append(q.value(0).toString());
    return out;
}

int Database::insertPhoto(const QString &path, const QString &hash, qint64 takenAt,
                          qint64 mtime, qint64 size)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO photos(path, added_at, hash, taken_at, mtime, size) "
        "VALUES(?, ?, ?, ?, ?, ?)"));
    q.addBindValue(path);
    q.addBindValue(QDateTime::currentSecsSinceEpoch());
    q.addBindValue(hash.isEmpty() ? QVariant() : QVariant(hash));
    q.addBindValue(takenAt > 0 ? QVariant(takenAt) : QVariant());
    q.addBindValue(mtime > 0 ? QVariant(mtime) : QVariant());
    q.addBindValue(size > 0 ? QVariant(size) : QVariant());
    q.exec();

    // Fill in the hash if the row predated hashing (e.g. legacy import).
    if (!hash.isEmpty()) {
        QSqlQuery u(m_db);
        u.prepare(QStringLiteral(
            "UPDATE photos SET hash = ? WHERE path = ? AND (hash IS NULL OR hash = '')"));
        u.addBindValue(hash);
        u.addBindValue(path);
        u.exec();
    }

    // This content now has a live photo again, so it's no longer orphaned.
    if (!hash.isEmpty())
        clearOrphanedHash(hash);

    QSqlQuery sel(m_db);
    sel.prepare(QStringLiteral("SELECT id FROM photos WHERE path = ?"));
    sel.addBindValue(path);
    sel.exec();
    if (sel.next())
        return sel.value(0).toInt();
    return -1;
}

void Database::relocatePhoto(int photoId, const QString &newPath, qint64 mtime, qint64 size)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "UPDATE photos SET path = ?, mtime = ?, size = ? WHERE id = ?"));
    q.addBindValue(newPath);
    q.addBindValue(mtime > 0 ? QVariant(mtime) : QVariant());
    q.addBindValue(size > 0 ? QVariant(size) : QVariant());
    q.addBindValue(photoId);
    q.exec();

    const QString hash = photoHash(photoId);
    if (!hash.isEmpty())
        clearOrphanedHash(hash);
}

void Database::setPhotoFileStats(int photoId, qint64 mtime, qint64 size)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE photos SET mtime = ?, size = ? WHERE id = ?"));
    q.addBindValue(mtime > 0 ? QVariant(mtime) : QVariant());
    q.addBindValue(size > 0 ? QVariant(size) : QVariant());
    q.addBindValue(photoId);
    q.exec();
}

void Database::deletePhoto(int photoId)
{
    const QString hash = photoHash(photoId);
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM photos WHERE id = ?")); // photo_tags cascade
    q.addBindValue(photoId);
    q.exec();
    recordOrphanIfUnreferenced(hash); // keep the archive, mark it orphaned if now unused
}

QVector<PhotoRef> Database::allPhotoRefs()
{
    QVector<PhotoRef> refs;
    QSqlQuery q(QStringLiteral(
        "SELECT id, path, hash, mtime, size, sidecar_written_at FROM photos"), m_db);
    while (q.next()) {
        PhotoRef r;
        r.id = q.value(0).toInt();
        r.path = q.value(1).toString();
        r.hash = q.value(2).toString();
        r.mtime = q.value(3).isNull() ? -1 : q.value(3).toLongLong();
        r.size = q.value(4).isNull() ? -1 : q.value(4).toLongLong();
        r.sidecarWrittenAt = q.value(5).isNull() ? -1 : q.value(5).toLongLong();
        refs.append(r);
    }
    return refs;
}

int Database::orphanPhotoCount()
{
    QSqlQuery q(QStringLiteral(
        "SELECT COUNT(*) FROM photos WHERE NOT EXISTS ("
        "  SELECT 1 FROM folders f WHERE photos.path = f.path"
        "    OR substr(photos.path, 1, length(f.path) + 1) = f.path || '/')"),
        m_db);
    if (q.next())
        return q.value(0).toInt();
    return 0;
}

void Database::deleteOrphanPhotos()
{
    // content_tags is keyed by hash, not photo id, so the archive survives this.
    const QString orphanPred = QStringLiteral(
        "NOT EXISTS (SELECT 1 FROM folders f WHERE photos.path = f.path"
        "  OR substr(photos.path, 1, length(f.path) + 1) = f.path || '/')");

    // Capture the hashes first so any left with no live photo are marked orphaned.
    QStringList hashes;
    QSqlQuery sel(
        QStringLiteral("SELECT DISTINCT hash FROM photos WHERE hash IS NOT NULL AND ")
            + orphanPred,
        m_db);
    while (sel.next())
        hashes.append(sel.value(0).toString());

    QSqlQuery q(m_db);
    q.exec(QStringLiteral("DELETE FROM photos WHERE ") + orphanPred);

    for (const QString &h : hashes)
        recordOrphanIfUnreferenced(h);
}

QString Database::hashFile(const QString &path, qint64 fullHashMaxBytes, qint64 sampleChunk)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const qint64 size = f.size();

    QCryptographicHash h(QCryptographicHash::Sha256);
    // Small enough to read whole (also the fallback when the two sample windows
    // would overlap). Bare hex — byte-for-byte the original whole-file scheme, so
    // these hashes are unchanged by this feature and need no migration.
    if (size <= fullHashMaxBytes || size < 2 * sampleChunk) {
        if (!h.addData(&f))
            return {};
        return QString::fromLatin1(h.result().toHex());
    }

    // Large file: fingerprint the first and last sampleChunk bytes plus the exact
    // size — ~constant-time regardless of length. The tail disambiguates files
    // that share a long common header (same camera/encoder); the size guards
    // against head+tail collisions.
    const QByteArray head = f.read(sampleChunk);
    if (head.size() != sampleChunk)
        return {};
    if (!f.seek(size - sampleChunk))
        return {};
    const QByteArray tail = f.read(sampleChunk);
    if (tail.size() != sampleChunk)
        return {};
    h.addData(head);
    h.addData(tail);
    h.addData(QByteArray::number(size));
    return QStringLiteral("s1:") + QString::fromLatin1(h.result().toHex());
}

QVector<QPair<int, QString>> Database::photosNeedingResample(qint64 fullHashMaxBytes)
{
    // Every large file still on a whole-file hash, regardless of folder state —
    // one full pass converts the library so the scheme stays consistent across
    // enabled, disabled, and orphaned rows alike.
    QVector<QPair<int, QString>> todo;
    QSqlQuery q(m_db);
    q.setForwardOnly(true);
    q.prepare(QStringLiteral(
        "SELECT id, path FROM photos "
        "WHERE size > ? AND hash IS NOT NULL AND hash != '' AND hash NOT LIKE 's1:%'"));
    q.addBindValue(fullHashMaxBytes);
    q.exec();
    while (q.next())
        todo.append({q.value(0).toInt(), q.value(1).toString()});
    return todo;
}

bool Database::hasResampleCandidates(qint64 fullHashMaxBytes)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT 1 FROM photos "
        "WHERE size > ? AND hash IS NOT NULL AND hash != '' AND hash NOT LIKE 's1:%' LIMIT 1"));
    q.addBindValue(fullHashMaxBytes);
    q.exec();
    return q.next();
}

QString Database::photoHash(int photoId)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT hash FROM photos WHERE id = ?"));
    q.addBindValue(photoId);
    q.exec();
    if (q.next())
        return q.value(0).toString();
    return {};
}

void Database::applyContentTags(int photoId, const QString &hash)
{
    if (hash.isEmpty())
        return;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO photo_tags(photo_id, tag_id) "
        "SELECT ?, tag_id FROM content_tags WHERE hash = ?"));
    q.addBindValue(photoId);
    q.addBindValue(hash);
    q.exec();
}

void Database::rememberContentTag(const QString &hash, int tagId)
{
    if (hash.isEmpty())
        return;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT OR IGNORE INTO content_tags(hash, tag_id) VALUES(?, ?)"));
    q.addBindValue(hash);
    q.addBindValue(tagId);
    q.exec();
}

void Database::forgetContentTag(const QString &hash, int tagId)
{
    if (hash.isEmpty())
        return;
    // Only forget the content→tag link once no live photo of this content still
    // carries the tag; un-importing a photo (which keeps the archive) must not
    // trip this, but explicitly untagging the last copy should.
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "DELETE FROM content_tags WHERE hash = ? AND tag_id = ? AND NOT EXISTS ("
        "  SELECT 1 FROM photo_tags pt JOIN photos p ON p.id = pt.photo_id "
        "  WHERE p.hash = ? AND pt.tag_id = ?)"));
    q.addBindValue(hash);
    q.addBindValue(tagId);
    q.addBindValue(hash);
    q.addBindValue(tagId);
    q.exec();
}

void Database::supersedeHash(const QString &oldHash, const QString &newHash)
{
    if (oldHash.isEmpty() || newHash.isEmpty() || oldHash == newHash)
        return;

    // Carry the archived tags forward to the new content hash.
    QSqlQuery fwd(m_db);
    fwd.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO content_tags(hash, tag_id) "
        "SELECT ?, tag_id FROM content_tags WHERE hash = ?"));
    fwd.addBindValue(newHash);
    fwd.addBindValue(oldHash);
    fwd.exec();

    // Drop the old archive only if nothing live still carries that content (an
    // edited duplicate leaves its untouched twin referencing the old hash).
    QSqlQuery stillUsed(m_db);
    stillUsed.prepare(QStringLiteral("SELECT 1 FROM photos WHERE hash = ? LIMIT 1"));
    stillUsed.addBindValue(oldHash);
    stillUsed.exec();
    if (!stillUsed.next()) {
        QSqlQuery del(m_db);
        del.prepare(QStringLiteral("DELETE FROM content_tags WHERE hash = ?"));
        del.addBindValue(oldHash);
        del.exec();
        clearOrphanedHash(oldHash); // superseded, not retained — never a retention candidate
    }
}

void Database::recordOrphanIfUnreferenced(const QString &hash)
{
    if (hash.isEmpty())
        return;
    // A hash is orphaned only while it still has archive rows but no live photo.
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO orphaned_hashes(hash, orphaned_at) "
        "SELECT ?, ? WHERE NOT EXISTS (SELECT 1 FROM photos WHERE hash = ?) "
        "AND EXISTS (SELECT 1 FROM content_tags WHERE hash = ?)"));
    q.addBindValue(hash);
    q.addBindValue(QDateTime::currentSecsSinceEpoch());
    q.addBindValue(hash);
    q.addBindValue(hash);
    q.exec();
}

void Database::clearOrphanedHash(const QString &hash)
{
    if (hash.isEmpty())
        return;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM orphaned_hashes WHERE hash = ?"));
    q.addBindValue(hash);
    q.exec();
}

int Database::ensureTag(const QString &rawName)
{
    const QString name = sanitizeName(rawName);
    // An alias resolves to its canonical tag — never materialize a tag for it.
    const int aliased = aliasTarget(name);
    if (aliased >= 0)
        return aliased;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT OR IGNORE INTO tags(name) VALUES(?)"));
    q.addBindValue(name);
    q.exec();

    QSqlQuery sel(m_db);
    sel.prepare(QStringLiteral("SELECT id FROM tags WHERE name = ?"));
    sel.addBindValue(name);
    sel.exec();
    if (sel.next())
        return sel.value(0).toInt();
    return -1;
}

int Database::tagIdByName(const QString &name)
{
    // The name column is COLLATE NOCASE, so this matches regardless of case.
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT id FROM tags WHERE name = ?"));
    q.addBindValue(name);
    q.exec();
    if (q.next())
        return q.value(0).toInt();
    return -1;
}

QString Database::tagName(int tagId)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT name FROM tags WHERE id = ?"));
    q.addBindValue(tagId);
    q.exec();
    if (q.next())
        return q.value(0).toString();
    return {};
}

void Database::renameTag(int tagId, const QString &newName)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE tags SET name = ? WHERE id = ?"));
    q.addBindValue(sanitizeName(newName));
    q.addBindValue(tagId);
    q.exec();
}

bool Database::linkPhotoTag(int photoId, int tagId)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT OR IGNORE INTO photo_tags(photo_id, tag_id) VALUES(?, ?)"));
    q.addBindValue(photoId);
    q.addBindValue(tagId);
    return q.exec();
}

bool Database::unlinkPhotoTag(int photoId, int tagId)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM photo_tags WHERE photo_id = ? AND tag_id = ?"));
    q.addBindValue(photoId);
    q.addBindValue(tagId);
    return q.exec();
}

void Database::linkTagToContent(int photoId, int tagId)
{
    const QString hash = photoHash(photoId);
    if (hash.isEmpty()) {
        linkPhotoTag(photoId, tagId);
        return;
    }
    // One statement tags every copy of this content at once (cheap via idx_photos_hash).
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO photo_tags(photo_id, tag_id) "
        "SELECT id, ? FROM photos WHERE hash = ?"));
    q.addBindValue(tagId);
    q.addBindValue(hash);
    q.exec();
}

void Database::unlinkTagFromContent(int photoId, int tagId)
{
    const QString hash = photoHash(photoId);
    if (hash.isEmpty()) {
        unlinkPhotoTag(photoId, tagId);
        return;
    }
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "DELETE FROM photo_tags WHERE tag_id = ? "
        "AND photo_id IN (SELECT id FROM photos WHERE hash = ?)"));
    q.addBindValue(tagId);
    q.addBindValue(hash);
    q.exec();
}

void Database::deleteTag(int tagId)
{
    // Reserved (system) tags are permanent: deleting the Favorites tag would leave
    // favoritesTagId() dangling and drop it from the albums list. User tags and
    // regular albums are system = 0, so only Favorites is protected here.
    {
        QSqlQuery sys(m_db);
        sys.prepare(QStringLiteral("SELECT system, name FROM tags WHERE id = ?"));
        sys.addBindValue(tagId);
        sys.exec();
        if (!sys.next() || sys.value(0).toInt() != 0)
            return;
        // Smart albums that mention the tag (as a tag or as an album) keep a
        // "missing" term with its last known name rather than silently losing it.
        const QString stored = sys.value(1).toString();
        const bool isAlbum = stored.startsWith(kAlbumPrefix);
        const QString display = isAlbum ? albumDisplayName(stored) : stored;
        rewriteSmartAlbumQueries([&](oql::Node &n) {
            const bool a = oql::markMissing(n, oql::NodeKind::Tag, tagId, display);
            const bool b = oql::markMissing(n, oql::NodeKind::Album, tagId, display);
            return a || b;
        });
    }
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM tags WHERE id = ? AND system = 0")); // photo_tags + implications cascade
    q.addBindValue(tagId);
    q.exec();
}

void Database::mergeTagInto(int fromTagId, int intoTagId)
{
    if (fromTagId < 0 || intoTagId < 0 || fromTagId == intoTagId)
        return;
    // Move fromTagId's rows to intoTagId across every table that references a tag
    // id. UPDATE OR IGNORE skips rows that would duplicate a primary key (the
    // target already has that link), and the follow-up DELETE clears whatever was
    // left behind on the losing id.
    const char *const refs[][2] = {
        {"photo_tags", "tag_id"},
        {"content_tags", "tag_id"},
        {"smart_album_tags", "tag_id"},
        {"tag_implications", "antecedent_id"},
        {"tag_implications", "consequent_id"},
    };
    for (const auto &r : refs) {
        QSqlQuery up(m_db);
        up.prepare(QStringLiteral("UPDATE OR IGNORE %1 SET %2 = ? WHERE %2 = ?")
                       .arg(QLatin1String(r[0]), QLatin1String(r[1])));
        up.addBindValue(intoTagId);
        up.addBindValue(fromTagId);
        up.exec();
        QSqlQuery del(m_db);
        del.prepare(QStringLiteral("DELETE FROM %1 WHERE %2 = ?")
                        .arg(QLatin1String(r[0]), QLatin1String(r[1])));
        del.addBindValue(fromTagId);
        del.exec();
    }
    // The merge can create a self-implication (a -> a); drop any.
    QSqlQuery loop(m_db);
    loop.exec(QStringLiteral("DELETE FROM tag_implications WHERE antecedent_id = consequent_id"));
    // Smart-album queries name the tag by id too: point them at the survivor.
    rewriteSmartAlbumQueries([&](oql::Node &n) { return oql::replaceTagId(n, fromTagId, intoTagId); });
    QSqlQuery drop(m_db);
    drop.prepare(QStringLiteral("DELETE FROM tags WHERE id = ?"));
    drop.addBindValue(fromTagId);
    drop.exec();
}

int Database::aliasTarget(const QString &name)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT tag_id FROM tag_aliases WHERE alias_name = ?"));
    q.addBindValue(name.trimmed()); // alias_name is COLLATE NOCASE, so case is ignored
    q.exec();
    if (q.next())
        return q.value(0).toInt();
    return -1;
}

int Database::resolveTagName(const QString &name)
{
    const int aliased = aliasTarget(name);
    if (aliased >= 0)
        return aliased;
    return tagIdByName(name.trimmed());
}

bool Database::aliasExists(const QString &name)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT 1 FROM tag_aliases WHERE alias_name = ? LIMIT 1"));
    q.addBindValue(name.trimmed());
    q.exec();
    return q.next();
}

QVector<AliasInfo> Database::loadAliases()
{
    QVector<AliasInfo> out;
    QSqlQuery q(QStringLiteral(
                    "SELECT a.alias_name, t.id, t.name, c.color_slot, c.name "
                    "FROM tag_aliases a "
                    "JOIN tags t ON t.id = a.tag_id "
                    "LEFT JOIN tag_categories c ON c.id = t.category_id "
                    "ORDER BY t.name COLLATE NOCASE, a.alias_name COLLATE NOCASE"),
                m_db);
    while (q.next()) {
        AliasInfo info;
        info.aliasName = q.value(0).toString();
        info.tagId = q.value(1).toInt();
        info.tagName = q.value(2).toString();
        info.colorSlot = q.value(3).isNull() ? -1 : q.value(3).toInt();
        info.categoryName = q.value(4).toString(); // empty when uncategorized
        out.append(info);
    }
    return out;
}

bool Database::createAlias(const QString &rawName, int tagId)
{
    const QString name = sanitizeName(rawName).trimmed();
    if (name.isEmpty() || tagId < 0)
        return false;
    // Can't alias a tag to its own name, and alias names are unique among aliases.
    if (name.compare(tagName(tagId), Qt::CaseInsensitive) == 0)
        return false;
    if (aliasExists(name))
        return false;
    // When the alias name is itself an existing tag, fold that tag into the
    // canonical one so the alias is a true merge, not a dangling redirect, and
    // queue the now-canonical photos for sidecar rewrite.
    const int existing = tagIdByName(name);
    if (existing >= 0 && existing != tagId) {
        mergeTagInto(existing, tagId);
        enqueueSidecarDirtyForTag(tagId);
    }
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT OR IGNORE INTO tag_aliases(alias_name, tag_id) VALUES(?, ?)"));
    q.addBindValue(name);
    q.addBindValue(tagId);
    return q.exec();
}

void Database::removeAlias(const QString &name)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM tag_aliases WHERE alias_name = ?"));
    q.addBindValue(name.trimmed());
    q.exec();
}

QVector<ImplicationInfo> Database::implications()
{
    // Only rules whose BOTH tags are active (apply to a photo in an enabled,
    // non-removed folder), so the manager hides associations that reference tags
    // living only on hidden/removed images — mirroring how the tag and category
    // lists drop inactive tags. Same active-photos/active-tags CTE as loadTags().
    QVector<ImplicationInfo> out;
    QSqlQuery q(QStringLiteral(
        "WITH active_photos AS (SELECT p.id FROM photos p WHERE %1), "
        "active_tags AS (SELECT DISTINCT pt.tag_id FROM photo_tags pt "
        "  WHERE pt.photo_id IN (SELECT id FROM active_photos)) "
        "SELECT a.id, a.name, ca.color_slot, c.id, c.name, cc.color_slot, ca.name, cc.name "
        "FROM tag_implications i "
        "JOIN tags a ON a.id = i.antecedent_id "
        "JOIN tags c ON c.id = i.consequent_id "
        "LEFT JOIN tag_categories ca ON ca.id = a.category_id "
        "LEFT JOIN tag_categories cc ON cc.id = c.category_id "
        "WHERE i.antecedent_id IN (SELECT tag_id FROM active_tags) "
        "  AND i.consequent_id IN (SELECT tag_id FROM active_tags) "
        "  AND %2 AND %3 "
        "ORDER BY a.name, c.name")
        .arg(activeClause(QStringLiteral("p")),
             nonSystemTagClause(QStringLiteral("a")),
             nonSystemTagClause(QStringLiteral("c"))),
        m_db);
    while (q.next()) {
        ImplicationInfo info;
        info.antecedentId = q.value(0).toInt();
        info.antecedentName = q.value(1).toString();
        info.antecedentColorSlot = q.value(2).isNull() ? -1 : q.value(2).toInt();
        info.consequentId = q.value(3).toInt();
        info.consequentName = q.value(4).toString();
        info.consequentColorSlot = q.value(5).isNull() ? -1 : q.value(5).toInt();
        info.antecedentCategoryName = q.value(6).toString(); // empty when uncategorized
        info.consequentCategoryName = q.value(7).toString();
        out.append(info);
    }
    return out;
}

void Database::addImplication(int antecedentId, int consequentId)
{
    if (antecedentId < 0 || consequentId < 0 || antecedentId == consequentId)
        return;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO tag_implications(antecedent_id, consequent_id) VALUES(?, ?)"));
    q.addBindValue(antecedentId);
    q.addBindValue(consequentId);
    q.exec();
}

void Database::removeImplication(int antecedentId, int consequentId)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "DELETE FROM tag_implications WHERE antecedent_id = ? AND consequent_id = ?"));
    q.addBindValue(antecedentId);
    q.addBindValue(consequentId);
    q.exec();
}

bool Database::implicationWouldCycle(int antecedentId, int consequentId)
{
    if (antecedentId == consequentId)
        return true;
    // Adding antecedent -> consequent closes a cycle iff consequent can already
    // reach antecedent: walk the graph forward from consequent and look for it.
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "WITH RECURSIVE reach(tag_id) AS ("
        "  SELECT consequent_id FROM tag_implications WHERE antecedent_id = ?"
        "  UNION "
        "  SELECT i.consequent_id FROM tag_implications i JOIN reach r ON i.antecedent_id = r.tag_id"
        ") SELECT 1 FROM reach WHERE tag_id = ? LIMIT 1"));
    q.addBindValue(consequentId);
    q.addBindValue(antecedentId);
    q.exec();
    return q.next();
}

void Database::applyImplications(const QString &hash, int tagId)
{
    if (hash.isEmpty() || tagId < 0)
        return;
    // The downstream closure of tagId; UNION dedups, so cycles terminate.
    const QString closure = QStringLiteral(
        "WITH RECURSIVE closure(tag_id) AS ("
        "  SELECT consequent_id FROM tag_implications WHERE antecedent_id = ?"
        "  UNION "
        "  SELECT i.consequent_id FROM tag_implications i JOIN closure c ON i.antecedent_id = c.tag_id"
        ") ");

    // Keep the archive closed under implications so re-imports carry them too.
    QSqlQuery a(m_db);
    a.prepare(closure + QStringLiteral(
        "INSERT OR IGNORE INTO content_tags(hash, tag_id) SELECT ?, tag_id FROM closure"));
    a.addBindValue(tagId);
    a.addBindValue(hash);
    a.exec();

    // Apply the closure to every live photo of this content.
    QSqlQuery p(m_db);
    p.prepare(closure + QStringLiteral(
        "INSERT OR IGNORE INTO photo_tags(photo_id, tag_id) "
        "SELECT ph.id, closure.tag_id FROM closure JOIN photos ph ON ph.hash = ?"));
    p.addBindValue(tagId);
    p.addBindValue(hash);
    p.exec();
}

int Database::expandAllImplications()
{
    // Propagate every antecedent's consequents across the archive and the live
    // links, one hop per pass, until a pass adds nothing. UNION / OR IGNORE reach
    // a fixed point, so chains fully materialize and cycles terminate; the pass
    // cap is a belt-and-braces guard against a driver that misreports row counts.
    int total = 0;
    for (int guard = 0; guard < 64; ++guard) {
        QSqlQuery archive(m_db);
        archive.exec(QStringLiteral(
            "INSERT OR IGNORE INTO content_tags(hash, tag_id) "
            "SELECT ct.hash, i.consequent_id FROM content_tags ct "
            "JOIN tag_implications i ON i.antecedent_id = ct.tag_id"));
        QSqlQuery live(m_db);
        live.exec(QStringLiteral(
            "INSERT OR IGNORE INTO photo_tags(photo_id, tag_id) "
            "SELECT pt.photo_id, i.consequent_id FROM photo_tags pt "
            "JOIN tag_implications i ON i.antecedent_id = pt.tag_id"));
        const int added = qMax(0, archive.numRowsAffected()) + qMax(0, live.numRowsAffected());
        total += added;
        if (added == 0)
            break;
    }
    return total;
}

namespace {
// The (photo_id, tag_id) rows on every live photo sharing `hash` — or just on
// `photoId` when the content isn't hashed yet. The before/after sets that
// applyTagRecorded diffs to learn what it inserted.
QSet<QPair<int, int>> contentPhotoLinks(QSqlDatabase &db, int photoId, const QString &hash)
{
    QSet<QPair<int, int>> out;
    QSqlQuery q(db);
    if (hash.isEmpty()) {
        q.prepare(QStringLiteral("SELECT photo_id, tag_id FROM photo_tags WHERE photo_id = ?"));
        q.addBindValue(photoId);
    } else {
        q.prepare(QStringLiteral(
            "SELECT pt.photo_id, pt.tag_id FROM photo_tags pt "
            "JOIN photos p ON p.id = pt.photo_id WHERE p.hash = ?"));
        q.addBindValue(hash);
    }
    q.exec();
    while (q.next())
        out.insert(qMakePair(q.value(0).toInt(), q.value(1).toInt()));
    return out;
}

QSet<int> contentArchiveTags(QSqlDatabase &db, const QString &hash)
{
    QSet<int> out;
    if (hash.isEmpty())
        return out;
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT tag_id FROM content_tags WHERE hash = ?"));
    q.addBindValue(hash);
    q.exec();
    while (q.next())
        out.insert(q.value(0).toInt());
    return out;
}
}

TagApplyRecord Database::applyTagRecorded(int photoId, int tagId)
{
    TagApplyRecord rec;
    rec.photoId = photoId;
    rec.tagId = tagId;
    if (photoId < 0 || tagId < 0)
        return rec;
    rec.hash = photoHash(photoId);

    // Diff the rows around the same three steps Library::addTag has always run,
    // so what triage applies is byte-for-byte what the detail panel applies.
    const QSet<QPair<int, int>> linksBefore = contentPhotoLinks(m_db, photoId, rec.hash);
    const QSet<int> archiveBefore = contentArchiveTags(m_db, rec.hash);

    linkTagToContent(photoId, tagId);
    rememberContentTag(rec.hash, tagId);
    applyImplications(rec.hash, tagId);

    QList<QPair<int, int>> added = (contentPhotoLinks(m_db, photoId, rec.hash) - linksBefore).values();
    std::sort(added.begin(), added.end()); // deterministic order (sets are unordered)
    rec.photoLinks = added;
    QList<int> archived = (contentArchiveTags(m_db, rec.hash) - archiveBefore).values();
    std::sort(archived.begin(), archived.end());
    rec.contentLinks = archived;
    return rec;
}

void Database::revertTagApply(const TagApplyRecord &record)
{
    // Remove exactly the rows the apply inserted — nothing more. The content rows
    // are dropped unconditionally (not via forgetContentTag's "still carried by a
    // live photo" guard): every live carrier the apply created is unlinked here
    // too, and any carrier that predates the apply means the row did as well, in
    // which case it isn't in the record.
    for (const QPair<int, int> &link : record.photoLinks)
        unlinkPhotoTag(link.first, link.second);
    if (record.hash.isEmpty())
        return;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM content_tags WHERE hash = ? AND tag_id = ?"));
    for (int tagId : record.contentLinks) {
        q.addBindValue(record.hash);
        q.addBindValue(tagId);
        q.exec();
    }
}

PhotoRowSnapshot Database::photoRow(int photoId)
{
    PhotoRowSnapshot row;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT id, path, hash, taken_at, mtime, size FROM photos WHERE id = ?"));
    q.addBindValue(photoId);
    q.exec();
    if (!q.next())
        return row;
    row.id = q.value(0).toInt();
    row.path = q.value(1).toString();
    row.hash = q.value(2).toString();
    row.takenAt = q.value(3).toLongLong();
    row.mtime = q.value(4).toLongLong();
    row.size = q.value(5).toLongLong();
    return row;
}

int Database::restorePhotoRow(const PhotoRowSnapshot &row)
{
    if (row.path.isEmpty())
        return -1;
    // insertPhoto clears the orphaned-hash marker the delete left, so the archive
    // is no longer a retention candidate; replaying it reattaches the tags.
    const int id = insertPhoto(row.path, row.hash, row.takenAt, row.mtime, row.size);
    if (id >= 0)
        applyContentTags(id, row.hash);
    return id;
}

QVector<TagInfo> Database::photoTags(int photoId)
{
    QVector<TagInfo> out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT t.id, t.name, %1 FROM photo_tags pt "
        "JOIN tags t ON t.id = pt.tag_id "
        "%2"
        "WHERE pt.photo_id = ? AND %3 ORDER BY t.name")
        .arg(QLatin1String(kTagCategoryColumns), QLatin1String(kTagCategoryJoin),
             nonSystemTagClause(QStringLiteral("t"))));
    q.addBindValue(photoId);
    q.exec();
    while (q.next()) {
        TagInfo info;
        info.id = q.value(0).toInt();
        info.name = q.value(1).toString();
        readCategory(q, 2, info);
        out.append(info);
    }
    return out;
}

QSet<int> Database::photoTagIds(int photoId)
{
    QSet<int> ids;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT tag_id FROM photo_tags WHERE photo_id = ?"));
    q.addBindValue(photoId);
    q.exec();
    while (q.next())
        ids.insert(q.value(0).toInt());
    return ids;
}

QVector<AlbumInfo> Database::albumTags()
{
    QVector<AlbumInfo> out;
    if (m_albumCategoryId < 0)
        return out;
    QSqlQuery q(m_db);
    // Favorites (system = 1) first, then user albums by name.
    q.prepare(QStringLiteral(
        "SELECT id, name, system FROM tags WHERE category_id = ? "
        "ORDER BY system DESC, name"));
    q.addBindValue(m_albumCategoryId);
    q.exec();
    while (q.next()) {
        AlbumInfo a;
        a.id = q.value(0).toInt();
        a.name = albumDisplayName(q.value(1).toString()); // strip the "_album:" prefix
        a.favorite = q.value(2).toInt() != 0;
        out.append(a);
    }
    return out;
}

QList<int> Database::albumTagIds()
{
    QList<int> ids;
    if (m_albumCategoryId < 0)
        return ids;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT id FROM tags WHERE category_id = ?"));
    q.addBindValue(m_albumCategoryId);
    q.exec();
    while (q.next())
        ids.append(q.value(0).toInt());
    return ids;
}

int Database::tagCategoryId(int tagId)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT category_id FROM tags WHERE id = ?"));
    q.addBindValue(tagId);
    q.exec();
    if (q.next() && !q.value(0).isNull())
        return q.value(0).toInt();
    return -1;
}

int Database::createAlbum(const QString &name)
{
    const QString trimmed = sanitizeName(name);
    if (trimmed.isEmpty() || m_albumCategoryId < 0)
        return -1;
    // Albums are stored under the namespaced "_album:" prefix, so they never
    // collide with user tags of the same display name. Reuse an existing album of
    // this name (even a hidden/empty one), mirroring tag creation.
    const QString stored = albumStoredName(trimmed);
    const int existing = tagIdByName(stored);
    if (existing >= 0)
        return tagCategoryId(existing) == m_albumCategoryId ? existing : -1;
    const int id = ensureTag(stored);
    if (id < 0)
        return -1;
    setTagCategory(id, m_albumCategoryId);
    return id;
}

QVector<SmartAlbumInfo> Database::smartAlbums()
{
    QVector<SmartAlbumInfo> out;
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("SELECT id, name, query FROM smart_albums ORDER BY name"));
    QHash<int, int> idx; // smart album id -> position in out
    while (q.next()) {
        SmartAlbumInfo sa;
        sa.id = q.value(0).toInt();
        sa.name = q.value(1).toString();
        if (!oql::fromJsonText(q.value(2).toString().toUtf8(), sa.query))
            sa.query = oql::Node::andOf({}); // unreadable row: matches nothing
        idx.insert(sa.id, out.size());
        out.append(sa);
    }
    if (out.isEmpty())
        return out;
    QSqlQuery t(m_db);
    t.exec(QStringLiteral("SELECT smart_album_id, tag_id FROM smart_album_tags"));
    while (t.next()) {
        const int sid = t.value(0).toInt();
        auto it = idx.constFind(sid);
        if (it != idx.constEnd())
            out[it.value()].tagIds.append(t.value(1).toInt());
    }
    return out;
}

SmartAlbumInfo Database::smartAlbum(int id)
{
    SmartAlbumInfo sa;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT id, name, query FROM smart_albums WHERE id = ?"));
    q.addBindValue(id);
    q.exec();
    if (!q.next())
        return sa;
    sa.id = q.value(0).toInt();
    sa.name = q.value(1).toString();
    if (!oql::fromJsonText(q.value(2).toString().toUtf8(), sa.query))
        sa.query = oql::Node::andOf({});
    QSqlQuery t(m_db);
    t.prepare(QStringLiteral("SELECT tag_id FROM smart_album_tags WHERE smart_album_id = ?"));
    t.addBindValue(id);
    t.exec();
    while (t.next())
        sa.tagIds.append(t.value(0).toInt());
    return sa;
}

QHash<int, oql::Node> Database::smartAlbumQueries()
{
    QHash<int, oql::Node> out;
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("SELECT id, query FROM smart_albums"));
    while (q.next()) {
        oql::Node n;
        if (!oql::fromJsonText(q.value(1).toString().toUtf8(), n))
            n = oql::Node::andOf({});
        out.insert(q.value(0).toInt(), n);
    }
    return out;
}

bool Database::smartAlbumWouldCycle(int id, const oql::Node &query)
{
    const QHash<int, oql::Node> all = smartAlbumQueries();
    return oql::wouldCycle(id, query, [&all](int sid) -> const oql::Node * {
        auto it = all.constFind(sid);
        return it == all.constEnd() ? nullptr : &it.value();
    });
}

namespace {
// An AND/OR with nothing in it is the one tree with no meaning; refuse to save it.
bool emptyQuery(const oql::Node &q)
{
    return (q.kind == oql::NodeKind::And || q.kind == oql::NodeKind::Or) && q.children.isEmpty();
}
}

void Database::writeSmartAlbumQuery(int id, const oql::Node &query)
{
    QSqlQuery up(m_db);
    up.prepare(QStringLiteral("UPDATE smart_albums SET query = ? WHERE id = ?"));
    up.addBindValue(QString::fromUtf8(oql::toJsonText(query)));
    up.addBindValue(id);
    up.exec();
    // The index: every tag id the tree mentions, as a tag or as an album.
    QSqlQuery del(m_db);
    del.prepare(QStringLiteral("DELETE FROM smart_album_tags WHERE smart_album_id = ?"));
    del.addBindValue(id);
    del.exec();
    QSqlQuery link(m_db);
    link.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO smart_album_tags (smart_album_id, tag_id) VALUES (?, ?)"));
    for (int t : oql::referencedIds(query, {oql::NodeKind::Tag, oql::NodeKind::Album})) {
        link.addBindValue(id);
        link.addBindValue(t);
        link.exec();
    }
}

void Database::rewriteSmartAlbumQueries(const std::function<bool(oql::Node &)> &mutate)
{
    if (!hasColumn(QStringLiteral("smart_albums"), QStringLiteral("query")))
        return; // pre-v17 library mid-migration: nothing stored as a tree yet
    QHash<int, oql::Node> all = smartAlbumQueries();
    for (auto it = all.begin(); it != all.end(); ++it)
        if (mutate(it.value()))
            writeSmartAlbumQuery(it.key(), it.value());
}

int Database::createSmartAlbum(const QString &name, const oql::Node &query)
{
    const QString trimmed = sanitizeName(name);
    if (trimmed.isEmpty() || emptyQuery(query))
        return -1;
    beginTransaction();
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT INTO smart_albums (name, match_all, query) VALUES (?, 1, '')"));
    q.addBindValue(trimmed);
    if (!q.exec()) { // UNIQUE name collision, etc.
        m_db.rollback();
        return -1;
    }
    const int id = q.lastInsertId().toInt();
    writeSmartAlbumQuery(id, query);
    commitTransaction();
    return id;
}

bool Database::updateSmartAlbum(int id, const QString &name, const oql::Node &query)
{
    const QString trimmed = sanitizeName(name);
    if (trimmed.isEmpty() || emptyQuery(query) || smartAlbumWouldCycle(id, query))
        return false;
    beginTransaction();
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE smart_albums SET name = ? WHERE id = ?"));
    q.addBindValue(trimmed);
    q.addBindValue(id);
    if (!q.exec() || q.numRowsAffected() == 0) { // UNIQUE name collision, or no such row
        m_db.rollback();
        return false;
    }
    writeSmartAlbumQuery(id, query);
    commitTransaction();
    return true;
}

int Database::createSmartAlbum(const QString &name, const QList<int> &tagIds, bool matchAll)
{
    if (tagIds.isEmpty())
        return -1;
    oql::MapVocabulary vocab;
    fillVocabulary(vocab);
    return createSmartAlbum(name, oql::fromTagSet(tagIds, matchAll, &vocab));
}

bool Database::updateSmartAlbum(int id, const QString &name, const QList<int> &tagIds, bool matchAll)
{
    if (tagIds.isEmpty())
        return false;
    oql::MapVocabulary vocab;
    fillVocabulary(vocab);
    return updateSmartAlbum(id, name, oql::fromTagSet(tagIds, matchAll, &vocab));
}

void Database::deleteSmartAlbum(int id)
{
    const QString name = smartAlbum(id).name;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM smart_albums WHERE id = ?")); // tags cascade
    q.addBindValue(id);
    q.exec();
    if (!name.isEmpty())
        rewriteSmartAlbumQueries([&](oql::Node &n) {
            return oql::markMissing(n, oql::NodeKind::SmartAlbum, id, name);
        });
}

void Database::fillVocabulary(oql::MapVocabulary &vocab)
{
    {
        // Every tag by its stored name. Albums are tags too, under the "_album:"
        // prefix: they go into the album table by display name instead.
        QSqlQuery q(QStringLiteral("SELECT id, name FROM tags"), m_db);
        while (q.next()) {
            const int id = q.value(0).toInt();
            const QString name = q.value(1).toString();
            if (name.startsWith(kAlbumPrefix))
                vocab.addAlbum(id, albumDisplayName(name));
            else
                vocab.addTag(id, name);
        }
    }
    {
        QSqlQuery q(QStringLiteral("SELECT alias_name, tag_id FROM tag_aliases"), m_db);
        while (q.next())
            vocab.addAlias(q.value(0).toString(), q.value(1).toInt());
    }
    {
        QSqlQuery q(QStringLiteral("SELECT id, name FROM smart_albums"), m_db);
        while (q.next())
            vocab.addSmartAlbum(q.value(0).toInt(), q.value(1).toString());
    }
    {
        QSqlQuery q(QStringLiteral("SELECT id, name FROM tag_categories WHERE system = 0"), m_db);
        while (q.next())
            vocab.addCategory(q.value(0).toInt(), q.value(1).toString());
    }
}

oql::EvalContext Database::evalContext()
{
    oql::EvalContext ctx;
    ctx.favoritesTagId = m_favoritesTagId;
    for (int id : albumTagIds())
        ctx.albumTagIds.insert(id);
    QSqlQuery q(QStringLiteral("SELECT id, category_id FROM tags WHERE category_id IS NOT NULL"), m_db);
    while (q.next())
        ctx.tagCategory.insert(q.value(0).toInt(), q.value(1).toInt());
    return ctx;
}

QVector<CategoryInfo> Database::loadCategories(bool activeOnly)
{
    QVector<CategoryInfo> out;
    // When activeOnly, keep a category only if at least one of its tags applies
    // to a photo in an active folder — mirroring the active-folder filter
    // loadTags() uses for individual tags. Categories are always created attached
    // to a tag (from the Tags tab), so there are no empty categories to special-case.
    // When activeOnly, restrict to categories that have a tag on an active photo.
    // Resolve the active categories up front (one photos-table scan + a tag_id
    // join via idx_photo_tags_tag) instead of a correlated EXISTS per category.
    const QString cte = activeOnly
        ? QStringLiteral(
              "WITH active_photos AS (SELECT p.id FROM photos p WHERE %1), "
              "active_categories AS ("
              "  SELECT DISTINCT t2.category_id FROM tags t2 "
              "  JOIN photo_tags pt ON pt.tag_id = t2.id "
              "  WHERE pt.photo_id IN (SELECT id FROM active_photos) "
              "    AND t2.category_id IS NOT NULL) ")
              .arg(activeClause(QStringLiteral("p")))
        : QString();
    const QString where = activeOnly
        ? QStringLiteral("WHERE c.id IN (SELECT category_id FROM active_categories) AND c.system = 0 ")
        : QStringLiteral("WHERE c.system = 0 ");
    QSqlQuery q(QStringLiteral(
                    "%1"
                    "SELECT c.id, c.name, c.color_slot, COUNT(t.id) "
                    "FROM tag_categories c LEFT JOIN tags t ON t.category_id = c.id "
                    "%2"
                    "GROUP BY c.id, c.name, c.color_slot "
                    "ORDER BY c.name").arg(cte, where),
                m_db);
    while (q.next()) {
        CategoryInfo info;
        info.id = q.value(0).toInt();
        info.name = q.value(1).toString();
        info.colorSlot = q.value(2).toInt();
        info.tagCount = q.value(3).toInt();
        out.append(info);
    }
    return out;
}

int Database::insertCategory(const QString &name, int colorSlot)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT INTO tag_categories(name, color_slot) VALUES(?, ?)"));
    q.addBindValue(sanitizeName(name));
    q.addBindValue(colorSlot);
    if (!q.exec())
        return -1; // UNIQUE (NOCASE) violation or other error
    return q.lastInsertId().toInt();
}

int Database::ensureCategory(const QString &name)
{
    const int existing = categoryIdByName(name);
    if (existing >= 0)
        return existing;
    // No palette size is available here (that's a UI concept), and color slots
    // wrap modulo the active theme's palette at display time — so a sequential
    // slot (the current user-category count) just spreads imported categories out.
    int slot = 0;
    QSqlQuery c(QStringLiteral("SELECT COUNT(*) FROM tag_categories WHERE system = 0"), m_db);
    if (c.next())
        slot = c.value(0).toInt();
    return insertCategory(name, slot);
}

int Database::categoryIdByName(const QString &name)
{
    // User categories only: the reserved ones are never a name conflict and
    // never a resolve-or-create target.
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT id FROM tag_categories WHERE name = ? AND system = 0"));
    q.addBindValue(name);
    q.exec();
    if (q.next())
        return q.value(0).toInt();
    return -1;
}

QString Database::categoryName(int categoryId)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT name FROM tag_categories WHERE id = ?"));
    q.addBindValue(categoryId);
    q.exec();
    if (q.next())
        return systemDisplayName(q.value(0).toString()); // strip the "_system:" prefix
    return {};
}

void Database::renameCategory(int categoryId, const QString &newName)
{
    // Reserved (system) categories are not renamable.
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE tag_categories SET name = ? WHERE id = ? AND system = 0"));
    q.addBindValue(sanitizeName(newName));
    q.addBindValue(categoryId);
    q.exec();
}

void Database::setCategoryColor(int categoryId, int colorSlot)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE tag_categories SET color_slot = ? WHERE id = ? AND system = 0"));
    q.addBindValue(colorSlot);
    q.addBindValue(categoryId);
    q.exec();
}

void Database::deleteCategory(int categoryId)
{
    // Reserved (system) categories are permanent; deleting one would turn its
    // hidden album tags into visible user tags.
    QSqlQuery guard(m_db);
    guard.prepare(QStringLiteral("SELECT system FROM tag_categories WHERE id = ?"));
    guard.addBindValue(categoryId);
    guard.exec();
    if (!guard.next() || guard.value(0).toInt() != 0)
        return;
    // Smart albums with a cat: term for it keep a "missing" term by name.
    const QString name = categoryName(categoryId);
    rewriteSmartAlbumQueries([&](oql::Node &n) {
        return oql::markMissing(n, oql::NodeKind::Category, categoryId, name);
    });
    // The FK is ON DELETE SET NULL, but clear explicitly too so the tags are
    // uncategorized even on a connection without foreign_keys enabled.
    QSqlQuery u(m_db);
    u.prepare(QStringLiteral("UPDATE tags SET category_id = NULL WHERE category_id = ?"));
    u.addBindValue(categoryId);
    u.exec();
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM tag_categories WHERE id = ?"));
    q.addBindValue(categoryId);
    q.exec();
}

void Database::setTagCategory(int tagId, int categoryId)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE tags SET category_id = ? WHERE id = ?"));
    q.addBindValue(categoryId < 0 ? QVariant(QMetaType(QMetaType::Int)) : QVariant(categoryId));
    q.addBindValue(tagId);
    q.exec();
}
