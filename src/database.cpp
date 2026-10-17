// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#include "database.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSqlQuery>
#include <QVariant>

namespace {
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

    QSqlQuery q(m_db);
    q.exec(QStringLiteral("PRAGMA foreign_keys = ON"));
    // WAL lets the worker thread's connection write hashes while the UI thread
    // reads, without either blocking on the other's lock.
    q.exec(QStringLiteral("PRAGMA journal_mode = WAL"));
    q.exec(QStringLiteral("PRAGMA busy_timeout = 5000"));
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
    if (!hasColumn(QStringLiteral("folders"), QStringLiteral("enabled")))
        q.exec(QStringLiteral("ALTER TABLE folders ADD COLUMN enabled INTEGER NOT NULL DEFAULT 1"));

    // Content-addressed tags. A photo stores the SHA-256 of its file; content_tags
    // archives tags against that hash so they outlive any particular photo row,
    // path, or inode. Seed the archive from whatever tags already exist.
    if (!hasColumn(QStringLiteral("photos"), QStringLiteral("hash")))
        q.exec(QStringLiteral("ALTER TABLE photos ADD COLUMN hash TEXT"));
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_photos_hash ON photos(hash)"));
    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS content_tags ("
        "  hash TEXT NOT NULL,"
        "  tag_id INTEGER NOT NULL REFERENCES tags(id) ON DELETE CASCADE,"
        "  PRIMARY KEY (hash, tag_id))"));

    // Tag implications: a photo carrying the antecedent tag also gets the
    // consequent tag. Materialized — applying the antecedent writes the
    // consequent into photo_tags/content_tags too (see applyImplications and the
    // worker's expandImplications migration), so the rest of the app needs no
    // implication-aware queries. CHECK blocks a tag implying itself; cycles are
    // blocked in the UI.
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

    // Tag categories: a tag may belong to one category, which carries a palette
    // slot (an index into the theme-derived category palette, not a fixed color,
    // so chips keep following the active theme). Deleting a category leaves its
    // tags in place, uncategorized.
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

    // Albums reuse the tag system: a reserved "Albums" category (system = 1,
    // stored namespaced as "_system:Albums") whose tags are albums, hidden from
    // every normal tag list. The Favorites album is the one tag also marked
    // system = 1, so it sorts first and is permanent.
    if (!hasColumn(QStringLiteral("tag_categories"), QStringLiteral("system")))
        q.exec(QStringLiteral("ALTER TABLE tag_categories ADD COLUMN system INTEGER NOT NULL DEFAULT 0"));
    if (!hasColumn(QStringLiteral("tags"), QStringLiteral("system")))
        q.exec(QStringLiteral("ALTER TABLE tags ADD COLUMN system INTEGER NOT NULL DEFAULT 0"));

    // Capture time in Unix seconds for the timeline view (EXIF date taken, or the
    // file mtime when there's no EXIF date). NULL until backfilled: the one-time
    // worker migration fills it for pre-existing rows (see LibraryWorker).
    if (!hasColumn(QStringLiteral("photos"), QStringLiteral("taken_at")))
        q.exec(QStringLiteral("ALTER TABLE photos ADD COLUMN taken_at INTEGER"));
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_photos_taken_at ON photos(taken_at)"));

    // Resolve (creating if needed) the reserved Albums category + Favorites album.
    ensureAlbumsCategory();
    ensureFavoritesAlbum();

    // Hashing existing photos and seeding content_tags is slow on a large
    // library, so it is driven from a worker thread (see LibraryWorker), not here.
    return true;
}

void Database::ensureAlbumsCategory()
{
    const QString storedName = systemStoredName(QStringLiteral("Albums")); // "_system:Albums"

    // One-time migration: the category used to be stored under the bare name
    // "Albums". Move a row created that way (system = 1) to the namespaced name.
    // A user's own "Albums" category (system = 0) is left alone — the system one
    // is created fresh beside it below.
    QSqlQuery legacy(m_db);
    legacy.prepare(QStringLiteral(
        "SELECT id FROM tag_categories WHERE name = ? AND system = 1"));
    legacy.addBindValue(QStringLiteral("Albums"));
    legacy.exec();
    if (legacy.next()) {
        const int legacyId = legacy.value(0).toInt();
        QSqlQuery cur(m_db);
        cur.prepare(QStringLiteral("SELECT id FROM tag_categories WHERE name = ?"));
        cur.addBindValue(storedName);
        cur.exec();
        if (cur.next()) {
            // Both exist (an older build re-created the bare one): fold the
            // legacy row's albums into the namespaced one and drop it.
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

QVector<Photo> Database::loadPhotos()
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
    QSqlQuery q(QStringLiteral(
                    "SELECT p.id, p.path, p.taken_at, p.hash FROM photos p WHERE %1 "
                    "ORDER BY p.added_at, p.id")
                    .arg(activeClause(QStringLiteral("p"))),
                m_db);
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
        p.fileName = QFileInfo(p.path).fileName();
        p.isVideo = isVideoPath(p.path);
        indexOf.insert(p.id, photos.size());
        photos.append(p);
    }

    // tagIds carries every tag (including the hidden album/favorites tags, which
    // the album filter and the star need); tagNames carries only the user-visible
    // ones, so album membership never shows as a chip or inflates the tag badge.
    QSqlQuery t(QStringLiteral(
                    "SELECT pt.photo_id, t.id, t.name, "
                    "  (t.category_id IS NOT NULL AND t.category_id IN "
                    "   (SELECT id FROM tag_categories WHERE system = 1)) "
                    "FROM photo_tags pt JOIN tags t ON t.id = pt.tag_id "
                    "ORDER BY t.name"),
                m_db);
    while (t.next()) {
        const int pid = t.value(0).toInt();
        auto it = indexOf.constFind(pid);
        if (it == indexOf.constEnd())
            continue;
        Photo &p = photos[it.value()];
        const int tagId = t.value(1).toInt();
        p.tagIds.insert(tagId);
        if (!t.value(3).toBool())
            p.tagNames.append(t.value(2).toString());
        if (tagId == m_favoritesTagId)
            p.favorite = true;
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

QVector<TagInfo> Database::activeTags(const QString &folderScope)
{
    // Tags that apply to at least one photo in the active (shown) set, counted
    // over that set only. Drives the filter sidebar and tag autocomplete.
    // Active-photo set first (10.6k-row photos scan), then the tags on it — much
    // cheaper than joining photos and testing the substr() clause across all of
    // photo_tags. Counts are over the active set. Uses idx_photo_tags_tag.
    //
    // folderScope (folder view) additionally restricts the active photos to those
    // at or under that directory — the subtree prefix match mirrors activeClause.
    const QString scope = folderScope.isEmpty()
        ? QString()
        : QStringLiteral(" AND substr(p.path, 1, length(?) + 1) = ? || '/'");
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

QVector<TagInfo> Database::coOccurringTags(const QList<int> &selected, const QString &folderScope)
{
    QVector<TagInfo> tags;
    if (selected.isEmpty())
        return activeTags(folderScope);

    QStringList ph;
    for (int i = 0; i < selected.size(); ++i)
        ph.append(QStringLiteral("?"));
    const QString inList = ph.join(QLatin1Char(','));

    // The folder-view scope restricts the active photos to a directory subtree;
    // its bind lives in the active_photos CTE, which precedes the selected-tag
    // placeholders in the SQL, so it is bound first.
    const QString scope = folderScope.isEmpty()
        ? QString()
        : QStringLiteral(" AND substr(p.path, 1, length(?) + 1) = ? || '/'");

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
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "DELETE FROM photos WHERE path = ? OR substr(path, 1, ?) = ?"));
    q.addBindValue(folderPath);
    q.addBindValue(prefix.size());
    q.addBindValue(prefix);
    q.exec();
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

int Database::insertPhoto(const QString &path, const QString &hash, qint64 takenAt)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO photos(path, added_at, hash, taken_at) VALUES(?, ?, ?, ?)"));
    q.addBindValue(path);
    q.addBindValue(QDateTime::currentSecsSinceEpoch());
    q.addBindValue(hash.isEmpty() ? QVariant() : QVariant(hash));
    q.addBindValue(takenAt > 0 ? QVariant(takenAt) : QVariant());
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

    QSqlQuery sel(m_db);
    sel.prepare(QStringLiteral("SELECT id FROM photos WHERE path = ?"));
    sel.addBindValue(path);
    sel.exec();
    if (sel.next())
        return sel.value(0).toInt();
    return -1;
}

void Database::relocatePhoto(int photoId, const QString &newPath)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE photos SET path = ? WHERE id = ?"));
    q.addBindValue(newPath);
    q.addBindValue(photoId);
    q.exec();
}

void Database::deletePhoto(int photoId)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM photos WHERE id = ?")); // photo_tags cascade
    q.addBindValue(photoId);
    q.exec();
}

QVector<PhotoRef> Database::allPhotoRefs()
{
    QVector<PhotoRef> refs;
    QSqlQuery q(QStringLiteral("SELECT id, path, hash FROM photos"), m_db);
    while (q.next()) {
        PhotoRef r;
        r.id = q.value(0).toInt();
        r.path = q.value(1).toString();
        r.hash = q.value(2).toString();
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
    QSqlQuery q(m_db);
    q.exec(QStringLiteral(
        "DELETE FROM photos WHERE NOT EXISTS ("
        "  SELECT 1 FROM folders f WHERE photos.path = f.path"
        "    OR substr(photos.path, 1, length(f.path) + 1) = f.path || '/')"));
}

QString Database::hashFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash h(QCryptographicHash::Sha256);
    if (!h.addData(&f))
        return {};
    return QString::fromLatin1(h.result().toHex());
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

int Database::ensureTag(const QString &name)
{
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
    q.addBindValue(newName);
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
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM tags WHERE id = ?")); // photo_tags + implications cascade
    q.addBindValue(tagId);
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
        "SELECT a.id, a.name, ca.color_slot, c.id, c.name, cc.color_slot "
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
    const QString trimmed = name.trimmed();
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
    q.addBindValue(name);
    q.addBindValue(colorSlot);
    if (!q.exec())
        return -1; // UNIQUE (NOCASE) violation or other error
    return q.lastInsertId().toInt();
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
    q.addBindValue(newName);
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
