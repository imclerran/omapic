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

    // Hashing existing photos and seeding content_tags is slow on a large
    // library, so it is driven from a worker thread (see LibraryWorker), not here.
    return true;
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

    QSqlQuery q(QStringLiteral("SELECT id, path FROM photos WHERE %1 ORDER BY added_at, id")
                    .arg(activeClause(QStringLiteral("photos"))),
                m_db);
    while (q.next()) {
        Photo p;
        p.id = q.value(0).toInt();
        p.path = q.value(1).toString();
        p.fileName = QFileInfo(p.path).fileName();
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
    QVector<TagInfo> tags;
    QSqlQuery q(QStringLiteral(
                    "SELECT t.id, t.name, COUNT(pt.photo_id) "
                    "FROM tags t LEFT JOIN photo_tags pt ON pt.tag_id = t.id "
                    "GROUP BY t.id, t.name ORDER BY t.name"),
                m_db);
    while (q.next()) {
        TagInfo info;
        info.id = q.value(0).toInt();
        info.name = q.value(1).toString();
        info.count = q.value(2).toInt();
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
                    "SELECT t.id, t.name, COUNT(pt.photo_id) "
                    "FROM tags t "
                    "JOIN photo_tags pt ON pt.tag_id = t.id "
                    "JOIN photos p ON p.id = pt.photo_id "
                    "WHERE %1 "
                    "GROUP BY t.id, t.name HAVING COUNT(pt.photo_id) > 0 "
                    "ORDER BY t.name")
                    .arg(activeClause(QStringLiteral("p"))),
                m_db);
    while (q.next()) {
        TagInfo info;
        info.id = q.value(0).toInt();
        info.name = q.value(1).toString();
        info.count = q.value(2).toInt();
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
        "SELECT t.id, t.name, COUNT(DISTINCT pt.photo_id) "
        "FROM tags t "
        "JOIN photo_tags pt ON pt.tag_id = t.id "
        "JOIN photos p ON p.id = pt.photo_id "
        "WHERE %1 AND p.id IN ("
        "  SELECT pt2.photo_id FROM photo_tags pt2 "
        "  WHERE pt2.tag_id IN (%2) "
        "  GROUP BY pt2.photo_id HAVING COUNT(DISTINCT pt2.tag_id) = %3) "
        "GROUP BY t.id, t.name HAVING COUNT(DISTINCT pt.photo_id) > 0 "
        "ORDER BY t.name")
        .arg(activeClause(QStringLiteral("p")), inList, QString::number(selected.size()));

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

int Database::insertPhoto(const QString &path, const QString &hash)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT OR IGNORE INTO photos(path, added_at, hash) VALUES(?, ?, ?)"));
    q.addBindValue(path);
    q.addBindValue(QDateTime::currentSecsSinceEpoch());
    q.addBindValue(hash.isEmpty() ? QVariant() : QVariant(hash));
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

void Database::deleteTag(int tagId)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM tags WHERE id = ?")); // photo_tags cascade
    q.addBindValue(tagId);
    q.exec();
}

QVector<QPair<int, QString>> Database::photoTagPairs(int photoId)
{
    QVector<QPair<int, QString>> out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT t.id, t.name FROM photo_tags pt "
        "JOIN tags t ON t.id = pt.tag_id "
        "WHERE pt.photo_id = ? ORDER BY t.name"));
    q.addBindValue(photoId);
    q.exec();
    while (q.next())
        out.append({q.value(0).toInt(), q.value(1).toString()});
    return out;
}
