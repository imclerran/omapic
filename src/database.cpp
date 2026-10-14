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

    // Capture time in Unix seconds for the timeline view (EXIF date taken, or the
    // file mtime when there's no EXIF date). NULL until backfilled: the one-time
    // worker migration fills it for pre-existing rows (see LibraryWorker).
    if (!hasColumn(QStringLiteral("photos"), QStringLiteral("taken_at")))
        q.exec(QStringLiteral("ALTER TABLE photos ADD COLUMN taken_at INTEGER"));
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_photos_taken_at ON photos(taken_at)"));

    // Hashing existing photos and seeding content_tags is slow on a large
    // library, so it is driven from a worker thread (see LibraryWorker), not here.
    return true;
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

QVector<Photo> Database::loadPhotos()
{
    QHash<int, int> indexOf;
    QVector<Photo> photos;

    // Collapse duplicates: among the active photos, show at most one row per
    // content hash — the earliest added (by added_at, then id). Rows whose hash
    // isn't computed yet (NULL/empty) are always shown, so nothing disappears
    // before the background hashing catches up, and the representative is chosen
    // only from photos that are themselves active, so disabling the folder
    // holding the kept copy promotes a still-visible duplicate in its place.
    QSqlQuery q(QStringLiteral(
                    "SELECT p.id, p.path, p.taken_at FROM photos p WHERE %1 "
                    "AND (p.hash IS NULL OR p.hash = '' OR p.id = ("
                    "  SELECT p2.id FROM photos p2 WHERE p2.hash = p.hash AND %2 "
                    "  ORDER BY p2.added_at, p2.id LIMIT 1)) "
                    "ORDER BY p.added_at, p.id")
                    .arg(activeClause(QStringLiteral("p")), activeClause(QStringLiteral("p2"))),
                m_db);
    while (q.next()) {
        Photo p;
        p.id = q.value(0).toInt();
        p.path = q.value(1).toString();
        p.takenAt = q.value(2).isNull() ? 0 : q.value(2).toLongLong();
        p.fileName = QFileInfo(p.path).fileName();
        p.isVideo = isVideoPath(p.path);
        indexOf.insert(p.id, photos.size());
        photos.append(p);
    }

    QSqlQuery t(QStringLiteral(
                    "SELECT pt.photo_id, t.id, t.name "
                    "FROM photo_tags pt JOIN tags t ON t.id = pt.tag_id "
                    "ORDER BY t.name"),
                m_db);
    while (t.next()) {
        const int pid = t.value(0).toInt();
        auto it = indexOf.constFind(pid);
        if (it == indexOf.constEnd())
            continue;
        Photo &p = photos[it.value()];
        p.tagIds.insert(t.value(1).toInt());
        p.tagNames.append(t.value(2).toString());
    }

    return photos;
}

QVector<TagInfo> Database::loadTags()
{
    // Tags that apply to at least one photo in an active (enabled, non-removed)
    // folder, so the tag manager hides tags living only on hidden-folder or
    // removed-folder images. The count is still the tag's total across all
    // photos, so the delete confirmation reflects everything it would remove.
    QVector<TagInfo> tags;
    QSqlQuery q(QStringLiteral(
                    "SELECT t.id, t.name, COUNT(pt.photo_id), %2 "
                    "FROM tags t LEFT JOIN photo_tags pt ON pt.tag_id = t.id "
                    "%3"
                    "WHERE EXISTS (SELECT 1 FROM photo_tags pt2 "
                    "              JOIN photos p ON p.id = pt2.photo_id "
                    "              WHERE pt2.tag_id = t.id AND %1) "
                    "GROUP BY t.id, t.name ORDER BY t.name")
                    .arg(activeClause(QStringLiteral("p")),
                         QLatin1String(kTagCategoryColumns),
                         QLatin1String(kTagCategoryJoin)),
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

QVector<TagInfo> Database::activeTags()
{
    // Tags that apply to at least one photo in the active (shown) set, counted
    // over that set only. Drives the filter sidebar and tag autocomplete.
    QVector<TagInfo> tags;
    QSqlQuery q(QStringLiteral(
                    "SELECT t.id, t.name, COUNT(pt.photo_id), %2 "
                    "FROM tags t "
                    "JOIN photo_tags pt ON pt.tag_id = t.id "
                    "JOIN photos p ON p.id = pt.photo_id "
                    "%3"
                    "WHERE %1 "
                    "GROUP BY t.id, t.name HAVING COUNT(pt.photo_id) > 0 "
                    "ORDER BY t.name")
                    .arg(activeClause(QStringLiteral("p")),
                         QLatin1String(kTagCategoryColumns),
                         QLatin1String(kTagCategoryJoin)),
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

QVector<TagInfo> Database::coOccurringTags(const QList<int> &selected)
{
    QVector<TagInfo> tags;
    if (selected.isEmpty())
        return activeTags();

    QStringList ph;
    for (int i = 0; i < selected.size(); ++i)
        ph.append(QStringLiteral("?"));
    const QString inList = ph.join(QLatin1Char(','));

    // Matching photos: active photos that carry every selected tag. Then return
    // the tags present on those photos, counted over that matching set.
    const QString sql = QStringLiteral(
        "SELECT t.id, t.name, COUNT(DISTINCT pt.photo_id), %4 "
        "FROM tags t "
        "JOIN photo_tags pt ON pt.tag_id = t.id "
        "JOIN photos p ON p.id = pt.photo_id "
        "%5"
        "WHERE %1 AND p.id IN ("
        "  SELECT pt2.photo_id FROM photo_tags pt2 "
        "  WHERE pt2.tag_id IN (%2) "
        "  GROUP BY pt2.photo_id HAVING COUNT(DISTINCT pt2.tag_id) = %3) "
        "GROUP BY t.id, t.name HAVING COUNT(DISTINCT pt.photo_id) > 0 "
        "ORDER BY t.name")
        .arg(activeClause(QStringLiteral("p")), inList, QString::number(selected.size()),
             QLatin1String(kTagCategoryColumns), QLatin1String(kTagCategoryJoin));

    QSqlQuery q(m_db);
    q.prepare(sql);
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
                    "WHERE %1 ORDER BY t.name")
                    .arg(activeClause(QStringLiteral("p"))),
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
    q.prepare(QStringLiteral("DELETE FROM tags WHERE id = ?")); // photo_tags cascade
    q.addBindValue(tagId);
    q.exec();
}

QVector<TagInfo> Database::photoTags(int photoId)
{
    QVector<TagInfo> out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT t.id, t.name, %1 FROM photo_tags pt "
        "JOIN tags t ON t.id = pt.tag_id "
        "%2"
        "WHERE pt.photo_id = ? ORDER BY t.name")
        .arg(QLatin1String(kTagCategoryColumns), QLatin1String(kTagCategoryJoin)));
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

QVector<CategoryInfo> Database::loadCategories()
{
    QVector<CategoryInfo> out;
    QSqlQuery q(QStringLiteral(
                    "SELECT c.id, c.name, c.color_slot, COUNT(t.id) "
                    "FROM tag_categories c LEFT JOIN tags t ON t.category_id = c.id "
                    "GROUP BY c.id, c.name, c.color_slot ORDER BY c.name"),
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
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT id FROM tag_categories WHERE name = ?"));
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
        return q.value(0).toString();
    return {};
}

void Database::renameCategory(int categoryId, const QString &newName)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE tag_categories SET name = ? WHERE id = ?"));
    q.addBindValue(newName);
    q.addBindValue(categoryId);
    q.exec();
}

void Database::setCategoryColor(int categoryId, int colorSlot)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE tag_categories SET color_slot = ? WHERE id = ?"));
    q.addBindValue(colorSlot);
    q.addBindValue(categoryId);
    q.exec();
}

void Database::deleteCategory(int categoryId)
{
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
