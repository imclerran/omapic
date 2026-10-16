// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#include "libraryworker.h"

#include <QFileInfo>
#include <QSet>

#include "mediascan.h"
#include "photo.h"
#include "photometadata.h"

namespace {

// Collect every media file under `root`, .nomedia-aware, into `out`. Thin alias
// over the shared scanner so the library and the standalone viewer stay in step.
void collectImages(const QString &root, QSet<QString> &out)
{
    mediascan::collect(root, out);
}

} // namespace

LibraryWorker::LibraryWorker(const QString &dbPath, QObject *parent)
    : QObject(parent)
    , m_dbPath(dbPath)
{
}

void LibraryWorker::initialize()
{
    // Must run in the worker's own thread: QSqlDatabase connections belong to
    // the thread that created them.
    m_db.open(m_dbPath, QStringLiteral("omapic_worker"));
}

void LibraryWorker::runMigration()
{
    const QVector<QPair<int, QString>> todo = m_db.photosMissingHash();
    const int total = todo.size();

    if (total > 0) {
        emit progress(tr("Preparing library…"), 0, total);
        const int step = qMax(1, total / 100);
        int done = 0;
        for (const auto &pr : todo) {
            const QString hash = Database::hashFile(pr.second);
            if (!hash.isEmpty())
                m_db.setPhotoHash(pr.first, hash);
            ++done;
            if (done % step == 0 || done == total)
                emit progress(tr("Preparing library…"), done, total);
        }
    }

    // Seed the content-tag archive from whatever tags already exist (now that
    // the photos carrying them have hashes). Idempotent on later runs.
    m_db.backfillContentTags();

    // Backfill the capture time (timeline view) for rows that predate the
    // taken_at column — reads EXIF/mtime once per legacy photo, here on the
    // worker thread. Idempotent: only NULL rows are revisited.
    const QVector<QPair<int, QString>> takenTodo = m_db.photosMissingTakenAt();
    const int takenTotal = takenTodo.size();
    if (takenTotal > 0) {
        emit progress(tr("Preparing library…"), 0, takenTotal);
        const int takenStep = qMax(1, takenTotal / 100);
        int done = 0;
        for (const auto &pr : takenTodo) {
            m_db.setPhotoTakenAt(pr.first, capturedAtEpoch(pr.second, isVideoPath(pr.second)));
            ++done;
            if (done % takenStep == 0 || done == takenTotal)
                emit progress(tr("Preparing library…"), done, takenTotal);
        }
    }

    emit migrationFinished();
}

void LibraryWorker::importDirectory(const QString &root)
{
    if (root.isEmpty()) {
        emit importFinished(root);
        return;
    }

    QSet<QString> files;
    collectImages(root, files);

    const int total = files.size();
    emit progress(tr("Importing folder…"), 0, total);
    const int step = qMax(1, total / 100);
    int done = 0;

    for (const QString &file : files) {
        const QString hash = Database::hashFile(file);
        const qint64 takenAt = capturedAtEpoch(file, isVideoPath(file));
        const int id = m_db.insertPhoto(file, hash, takenAt);
        if (id >= 0)
            m_db.applyContentTags(id, hash); // reattach any archived tags for this content

        ++done;
        if (done % step == 0 || done == total)
            emit progress(tr("Importing folder…"), done, total);
    }

    m_db.insertFolder(root);
    emit importFinished(root);
}

void LibraryWorker::rescan()
{
    emit progress(tr("Scanning folders…"), 0, 0); // indeterminate during the walk

    const QVector<FolderInfo> folders = m_db.loadFolders();

    // What's on disk now (non-excluded images under every registered folder).
    QSet<QString> onDisk;
    for (const FolderInfo &f : folders)
        collectImages(f.path, onDisk);

    auto underAnyFolder = [&](const QString &p) -> bool {
        for (const FolderInfo &f : folders) {
            if (p == f.path || p.startsWith(f.path + QLatin1Char('/')))
                return true;
        }
        return false;
    };

    // Classify existing rows. A row under a registered folder whose file is no
    // longer part of the on-disk set is "missing" (moved, deleted, or newly
    // .nomedia-excluded). Orphan rows (under no folder) are left untouched.
    const QVector<PhotoRef> rows = m_db.allPhotoRefs();
    QSet<QString> knownPaths;
    QVector<int> missingIds;
    QHash<QString, QList<int>> missingByHash;
    for (const PhotoRef &r : rows) {
        knownPaths.insert(r.path);
        if (underAnyFolder(r.path) && !onDisk.contains(r.path)) {
            missingIds.append(r.id);
            if (!r.hash.isEmpty())
                missingByHash[r.hash].append(r.id);
        }
    }

    // Files on disk we don't yet have a row for: new imports or move targets.
    QStringList newPaths;
    for (const QString &p : onDisk) {
        if (!knownPaths.contains(p))
            newPaths.append(p);
    }

    const int total = newPaths.size();
    emit progress(tr("Rescanning…"), 0, total);
    const int step = qMax(1, total / 100);
    int done = 0;

    QSet<int> relocated;
    for (const QString &p : newPaths) {
        const QString hash = Database::hashFile(p);

        // A move: exactly one missing row has this content. Relocate it in place
        // so its id and live tags are preserved — no duplicate, no re-tagging.
        bool didRelocate = false;
        if (!hash.isEmpty()) {
            auto it = missingByHash.constFind(hash);
            if (it != missingByHash.constEnd()) {
                int candidate = -1;
                int availCount = 0;
                for (int id : it.value()) {
                    if (!relocated.contains(id)) {
                        candidate = id;
                        ++availCount;
                    }
                }
                if (availCount == 1) {
                    m_db.relocatePhoto(candidate, p);
                    relocated.insert(candidate);
                    didRelocate = true;
                }
            }
        }

        if (!didRelocate) {
            const qint64 takenAt = capturedAtEpoch(p, isVideoPath(p));
            const int id = m_db.insertPhoto(p, hash, takenAt);
            if (id >= 0)
                m_db.applyContentTags(id, hash);
        }

        ++done;
        if (done % step == 0 || done == total)
            emit progress(tr("Rescanning…"), done, total);
    }

    // Whatever stayed missing (not a move) is gone: drop the row. Tags remain in
    // the content archive, so they return if the file ever reappears.
    for (int id : missingIds) {
        if (!relocated.contains(id))
            m_db.deletePhoto(id);
    }

    emit rescanFinished();
}

void LibraryWorker::addTagToPhotos(const QList<int> &photoIds, const QString &name)
{
    const QString trimmed = name.trimmed();
    const int tagId = trimmed.isEmpty() ? -1 : m_db.ensureTag(trimmed);
    if (tagId < 0 || photoIds.isEmpty()) {
        emit tagJobFinished();
        return;
    }

    const int total = photoIds.size();
    emit progress(tr("Applying tag…"), 0, total);
    const int step = qMax(1, total / 100);

    m_db.beginTransaction();
    int done = 0;
    for (int pid : photoIds) {
        if (pid >= 0) {
            const QString hash = m_db.photoHash(pid);
            m_db.linkTagToContent(pid, tagId); // every copy of this content
            m_db.rememberContentTag(hash, tagId);
            m_db.applyImplications(hash, tagId); // and any tags this one implies
        }
        ++done;
        if (done % step == 0 || done == total)
            emit progress(tr("Applying tag…"), done, total);
    }
    m_db.commitTransaction();

    emit tagJobFinished();
}

void LibraryWorker::expandImplications()
{
    // Materialize every implication across the whole library in one transaction
    // (closure pass over content_tags + photo_tags). Indeterminate progress — the
    // pass is a handful of set-based statements, not a per-row loop.
    emit progress(tr("Applying associations…"), 0, 0);
    m_db.beginTransaction();
    m_db.expandAllImplications();
    m_db.commitTransaction();
    emit implicationsExpanded();
}

void LibraryWorker::removeTagFromPhotos(const QList<int> &photoIds, int tagId)
{
    if (tagId < 0 || photoIds.isEmpty()) {
        emit tagJobFinished();
        return;
    }

    const int total = photoIds.size();
    emit progress(tr("Removing tag…"), 0, total);
    const int step = qMax(1, total / 100);

    m_db.beginTransaction();
    int done = 0;
    for (int pid : photoIds) {
        if (pid >= 0) {
            m_db.unlinkTagFromContent(pid, tagId); // every copy of this content
            m_db.forgetContentTag(m_db.photoHash(pid), tagId);
        }
        ++done;
        if (done % step == 0 || done == total)
            emit progress(tr("Removing tag…"), done, total);
    }
    m_db.commitTransaction();

    emit tagJobFinished();
}
