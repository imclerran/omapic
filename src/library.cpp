// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#include "library.h"

#include <QDir>
#include <QHash>
#include <QStandardPaths>

#include <algorithm>

#include "libraryworker.h"

Library::Library(QObject *parent)
    : QObject(parent)
    , m_photos(new PhotoModel(this))
    , m_filtered(new PhotoFilterModel(this))
    , m_tags(new TagModel(this))
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    const QString dbPath = dir + QStringLiteral("/library.db");
    m_db.open(dbPath);

    m_filtered->setSourceModel(m_photos);
    m_filtered->setDynamicSortFilter(true);
    m_filtered->setSortRole(PhotoModel::FileNameRole);
    m_filtered->setSortCaseSensitivity(Qt::CaseInsensitive);
    m_filtered->sort(0);

    // Changing the tag selection re-narrows the sidebar (faceted drill-down) when
    // in "match all" mode.
    connect(m_tags, &TagModel::selectionChanged, this, &Library::refreshTags);

    m_photos->setPhotos(m_db.loadPhotos());
    refreshTags();

    // Hashing runs on a worker thread so the window stays responsive. The worker
    // owns its own DB connection; we only touch the models here on the GUI thread.
    m_worker = new LibraryWorker(dbPath);
    m_worker->moveToThread(&m_workerThread);
    connect(&m_workerThread, &QThread::started, m_worker, &LibraryWorker::initialize);
    connect(&m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(this, &Library::requestMigration, m_worker, &LibraryWorker::runMigration);
    connect(this, &Library::requestImport, m_worker, &LibraryWorker::importDirectory);
    connect(this, &Library::requestRescan, m_worker, &LibraryWorker::rescan);
    connect(m_worker, &LibraryWorker::progress, this, &Library::onWorkerProgress);
    connect(m_worker, &LibraryWorker::migrationFinished, this, &Library::onMigrationFinished);
    connect(m_worker, &LibraryWorker::importFinished, this, &Library::onImportFinished);
    connect(m_worker, &LibraryWorker::rescanFinished, this, &Library::onRescanFinished);
    m_workerThread.start();

    // Kick off the one-time hash backfill. Only show the progress UI when there
    // is actually a backlog to chew through.
    if (m_db.unhashedPhotoCount() > 0)
        beginBusy(tr("Preparing library…"));
    emit requestMigration();
}

Library::~Library()
{
    m_workerThread.quit();
    m_workerThread.wait();
}

void Library::importDirectory(const QUrl &dir)
{
    const QString root = dir.toLocalFile();
    if (root.isEmpty())
        return;
    beginBusy(tr("Importing folder…"));
    emit requestImport(root); // runs on the worker thread
}

void Library::beginBusy(const QString &status)
{
    m_statusText = status;
    emit statusChanged();
    m_progress = -1.0; // indeterminate until the first progress report
    emit progressChanged();
    if (!m_busy) {
        m_busy = true;
        emit busyChanged();
    }
}

void Library::endBusy()
{
    if (!m_busy)
        return;
    m_busy = false;
    emit busyChanged();
}

void Library::onWorkerProgress(const QString &phase, int done, int total)
{
    if (phase != m_statusText) {
        m_statusText = phase;
        emit statusChanged();
    }
    const double p = total > 0 ? double(done) / double(total) : -1.0;
    if (p != m_progress) {
        m_progress = p;
        emit progressChanged();
    }
}

void Library::onMigrationFinished()
{
    endBusy();
}

void Library::onImportFinished(const QString &root)
{
    Q_UNUSED(root)
    // The worker wrote the rows; refresh the in-memory views from the DB.
    m_photos->setPhotos(m_db.loadPhotos());
    refreshTags();
    endBusy();
    emit foldersChanged();
}

void Library::rescan()
{
    beginBusy(tr("Scanning folders…"));
    emit requestRescan(); // runs on the worker thread
}

void Library::onRescanFinished()
{
    // Rows were added/relocated/removed on the worker's connection; reload.
    m_photos->setPhotos(m_db.loadPhotos());
    refreshTags();
    endBusy();
    emit foldersChanged();
}

QVariantList Library::folders() const
{
    QVariantList out;
    for (const FolderInfo &f : m_db.loadFolders()) {
        out.append(QVariantMap{
            {QStringLiteral("id"), f.id},
            {QStringLiteral("path"), f.path},
            {QStringLiteral("count"), f.count},
            {QStringLiteral("enabled"), f.enabled},
        });
    }
    return out;
}

void Library::removeFolder(int folderId)
{
    const QString path = m_db.folderPath(folderId);
    if (path.isEmpty())
        return;

    m_db.deletePhotosUnderPath(path); // cascades photo_tags via foreign key
    m_db.removeFolder(folderId);

    m_photos->setPhotos(m_db.loadPhotos());
    refreshTags();
    emit foldersChanged();
}

void Library::setFolderEnabled(int folderId, bool enabled)
{
    m_db.setFolderEnabled(folderId, enabled);
    m_photos->setPhotos(m_db.loadPhotos()); // active set changed
    refreshTags();
    emit foldersChanged();
}

int Library::orphanPhotoCount() const
{
    return m_db.orphanPhotoCount();
}

void Library::removeOrphanPhotos()
{
    m_db.deleteOrphanPhotos(); // content-tag archive is kept, so tags can return
    m_photos->setPhotos(m_db.loadPhotos());
    refreshTags();
    emit foldersChanged();
}

void Library::addTag(int photoId, const QString &name)
{
    const QString trimmed = name.trimmed();
    if (photoId < 0 || trimmed.isEmpty())
        return;
    const int tagId = m_db.ensureTag(trimmed);
    if (tagId < 0)
        return;
    m_db.linkPhotoTag(photoId, tagId);
    m_db.rememberContentTag(m_db.photoHash(photoId), tagId);
    applyTags(photoId);
    refreshTags();
    emit photoChanged(photoId);
}

void Library::removeTag(int photoId, int tagId)
{
    if (photoId < 0 || tagId < 0)
        return;
    m_db.unlinkPhotoTag(photoId, tagId);
    m_db.forgetContentTag(m_db.photoHash(photoId), tagId);
    applyTags(photoId);
    refreshTags();
    emit photoChanged(photoId);
}

void Library::addTagToPhotos(const QVariantList &photoIds, const QString &name)
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty() || photoIds.isEmpty())
        return;
    const int tagId = m_db.ensureTag(trimmed);
    if (tagId < 0)
        return;
    for (const QVariant &v : photoIds) {
        const int pid = v.toInt();
        if (pid < 0)
            continue;
        m_db.linkPhotoTag(pid, tagId);
        m_db.rememberContentTag(m_db.photoHash(pid), tagId);
        applyTags(pid);
        emit photoChanged(pid);
    }
    refreshTags();
}

void Library::removeTagFromPhotos(const QVariantList &photoIds, int tagId)
{
    if (tagId < 0 || photoIds.isEmpty())
        return;
    for (const QVariant &v : photoIds) {
        const int pid = v.toInt();
        if (pid < 0)
            continue;
        m_db.unlinkPhotoTag(pid, tagId);
        m_db.forgetContentTag(m_db.photoHash(pid), tagId);
        applyTags(pid);
        emit photoChanged(pid);
    }
    refreshTags();
}

QVariantList Library::commonTags(const QVariantList &photoIds) const
{
    QHash<int, int> counts;
    QHash<int, QString> names;
    for (const QVariant &v : photoIds) {
        const int pid = v.toInt();
        if (pid < 0)
            continue;
        for (const auto &pr : m_db.photoTagPairs(pid)) {
            counts[pr.first] += 1;
            names[pr.first] = pr.second;
        }
    }

    QList<int> ids = counts.keys();
    std::sort(ids.begin(), ids.end(), [&](int a, int b) {
        return names.value(a).compare(names.value(b), Qt::CaseInsensitive) < 0;
    });

    QVariantList out;
    for (int id : ids) {
        out.append(QVariantMap{
            {QStringLiteral("id"), id},
            {QStringLiteral("name"), names.value(id)},
            {QStringLiteral("count"), counts.value(id)},
        });
    }
    return out;
}

QVariantMap Library::photoInfo(int photoId) const
{
    QVariantMap info;
    if (photoId < 0)
        return info;

    const Photo p = m_photos->photoById(photoId);
    if (p.id < 0)
        return info;

    QVariantList tags;
    const auto pairs = m_db.photoTagPairs(photoId);
    for (const auto &pr : pairs) {
        tags.append(QVariantMap{
            {QStringLiteral("id"), pr.first},
            {QStringLiteral("name"), pr.second},
        });
    }

    info[QStringLiteral("id")] = p.id;
    info[QStringLiteral("fileName")] = p.fileName;
    info[QStringLiteral("path")] = p.path;
    info[QStringLiteral("tags")] = tags;
    return info;
}

QStringList Library::allTagNames() const
{
    // Only tags relevant to the active (shown) set, so autocomplete matches the
    // filter sidebar.
    return m_db.allTagNames();
}

QVariantList Library::allTags() const
{
    // Tags shown in the tag manager: those with at least one photo in an active
    // folder (hidden-folder-only and removed-folder-only tags are left out).
    QVariantList out;
    for (const TagInfo &t : m_db.loadTags()) {
        out.append(QVariantMap{
            {QStringLiteral("id"), t.id},
            {QStringLiteral("name"), t.name},
            {QStringLiteral("count"), t.count},
        });
    }
    return out;
}

void Library::deleteTag(int tagId)
{
    if (tagId < 0)
        return;
    m_db.deleteTag(tagId); // removes the tag and all its photo links (cascade)
    m_photos->setPhotos(m_db.loadPhotos()); // refresh in-memory tag data
    refreshTags();
}

int Library::renameTag(int tagId, const QString &newName)
{
    const QString trimmed = newName.trimmed();
    if (tagId < 0 || trimmed.isEmpty())
        return RenameEmpty;

    if (m_db.tagName(tagId) == trimmed)
        return RenameOk; // exact no-op

    // A match on another tag is a conflict; a match on this same tag is just a
    // change of casing, which is allowed (the id, links, and archive are intact).
    const int existing = m_db.tagIdByName(trimmed);
    if (existing >= 0 && existing != tagId)
        return RenameConflict;

    m_db.renameTag(tagId, trimmed);
    m_photos->setPhotos(m_db.loadPhotos()); // refresh in-memory tag names
    refreshTags();
    return RenameOk;
}

void Library::applyTags(int photoId)
{
    const auto pairs = m_db.photoTagPairs(photoId);
    QSet<int> ids;
    QStringList names;
    for (const auto &pr : pairs) {
        ids.insert(pr.first);
        names.append(pr.second);
    }
    m_photos->setTagsForId(photoId, ids, names);
}

void Library::setMatchAll(bool matchAll)
{
    if (m_matchAll == matchAll)
        return;
    m_matchAll = matchAll;
    refreshTags(); // the sidebar's tag set depends on the filter mode
}

void Library::refreshTags()
{
    // The filter sidebar shows only tags relevant to the active (shown) set, with
    // counts over that set; tags touching only hidden folders drop out. Unused
    // tags still live in the DB and appear in the tag manager.
    //
    // In "match all" mode with tags already selected, narrow further to tags that
    // co-occur on the currently-matching photos (faceted drill-down), so chips
    // that could only ever yield an empty result disappear.
    const QList<int> sel = m_tags->selectedTagIds();
    QVector<TagInfo> tags =
        (m_matchAll && !sel.isEmpty()) ? m_db.coOccurringTags(sel) : m_db.activeTags();

    // Keep every selected tag present (even if the match set is momentarily empty,
    // e.g. after switching an incompatible selection into match-all) so it stays
    // visible and can be deselected.
    QSet<int> have;
    for (const TagInfo &t : tags)
        have.insert(t.id);
    for (int id : sel) {
        if (!have.contains(id)) {
            TagInfo t;
            t.id = id;
            t.name = m_db.tagName(id);
            t.count = 0;
            tags.append(t);
        }
    }
    std::sort(tags.begin(), tags.end(), [](const TagInfo &a, const TagInfo &b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });

    m_tags->setTags(tags);
    emit tagsChanged();
}
