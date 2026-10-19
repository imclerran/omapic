// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#include "libraryworker.h"

#include <QDateTime>
#include <QFileInfo>
#include <QHash>
#include <QSet>

#include "mediascan.h"
#include "photo.h"
#include "photometadata.h"
#include "sidecar.h"

namespace {

// Collect every media file under `root` (mapped to its mtime/size), .nomedia-
// aware. Thin alias over the shared scanner so the library and the standalone
// viewer stay in step.
void collectImages(const QString &root, QHash<QString, mediascan::FileEntry> &out)
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

    QHash<QString, mediascan::FileEntry> files;
    collectImages(root, files);

    const int total = files.size();
    emit progress(tr("Importing folder…"), 0, total);
    const int step = qMax(1, total / 100);
    int done = 0;

    for (auto it = files.constBegin(); it != files.constEnd(); ++it) {
        const QString &file = it.key();
        const mediascan::FileEntry &fe = it.value();
        const QString hash = Database::hashFile(file);
        const ImportMetadata meta = readImportMetadata(file, isVideoPath(file));
        const int id = m_db.insertPhoto(file, hash, meta.takenAt, fe.mtime, fe.size);
        if (id >= 0) {
            m_db.applyContentTags(id, hash); // reattach any archived tags for this content
            const qint64 sidecarMtime = applyImportedTags(id, file, hash, meta);
            if (sidecarMtime > 0)
                m_db.setSidecarWrittenAt(id, sidecarMtime);
        }

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

    // What's on disk now (non-excluded images, with mtime/size) under every
    // registered folder.
    QHash<QString, mediascan::FileEntry> onDisk;
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
    // .nomedia-excluded). Rows still on disk are kept for the second pass below
    // (detecting in-place edits and externally-changed sidecars). Orphan rows
    // (under no folder) are left untouched.
    const QVector<PhotoRef> rows = m_db.allPhotoRefs();
    QSet<QString> knownPaths;
    QVector<int> missingIds;
    QHash<QString, QList<int>> missingByHash;
    QVector<PhotoRef> knownOnDisk;
    for (const PhotoRef &r : rows) {
        knownPaths.insert(r.path);
        if (!underAnyFolder(r.path))
            continue;
        if (onDisk.contains(r.path))
            knownOnDisk.append(r);
        else {
            missingIds.append(r.id);
            if (!r.hash.isEmpty())
                missingByHash[r.hash].append(r.id);
        }
    }

    // Files on disk we don't yet have a row for: new imports or move targets.
    QStringList newPaths;
    for (auto it = onDisk.constBegin(); it != onDisk.constEnd(); ++it) {
        if (!knownPaths.contains(it.key()))
            newPaths.append(it.key());
    }

    const int total = newPaths.size();
    emit progress(tr("Rescanning…"), 0, total);
    const int step = qMax(1, total / 100);
    int done = 0;

    QSet<int> relocated;
    for (const QString &p : newPaths) {
        const mediascan::FileEntry fe = onDisk.value(p);
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
                    m_db.relocatePhoto(candidate, p, fe.mtime, fe.size);
                    relocated.insert(candidate);
                    didRelocate = true;
                }
            }
        }

        if (!didRelocate) {
            const ImportMetadata meta = readImportMetadata(p, isVideoPath(p));
            const int id = m_db.insertPhoto(p, hash, meta.takenAt, fe.mtime, fe.size);
            if (id >= 0) {
                m_db.applyContentTags(id, hash);
                const qint64 sidecarMtime = applyImportedTags(id, p, hash, meta);
                if (sidecarMtime > 0)
                    m_db.setSidecarWrittenAt(id, sidecarMtime);
            }
        }

        ++done;
        if (done % step == 0 || done == total)
            emit progress(tr("Rescanning…"), done, total);
    }

    // Second pass over files still in place: pick up in-place edits (the file's
    // mtime/size changed → rehash and rekey the archive) and sidecars another
    // app wrote (newer than our last sync → import their keywords).
    for (const PhotoRef &r : knownOnDisk) {
        const mediascan::FileEntry fe = onDisk.value(r.path);

        if (r.mtime < 0 || r.size < 0) {
            // Pre-v10 row with no stats yet: record them without rehashing, so a
            // real edit is detected next time (avoids a mass rehash on upgrade).
            m_db.setPhotoFileStats(r.id, fe.mtime, fe.size);
        } else if (fe.mtime != r.mtime || fe.size != r.size) {
            const QString newHash = Database::hashFile(r.path);
            if (!newHash.isEmpty() && newHash != r.hash) {
                m_db.supersedeHash(r.hash, newHash); // carry archived tags forward
                m_db.setPhotoHash(r.id, newHash);
                m_db.applyContentTags(r.id, newHash);
            }
            m_db.setPhotoFileStats(r.id, fe.mtime, fe.size);
        }

        // An externally-changed sidecar (or a first-seen one): import its tags.
        const QString side = sidecar::sidecarPath(r.path);
        if (!side.isEmpty()) {
            const qint64 sideMtime = QFileInfo(side).lastModified().toSecsSinceEpoch();
            const qint64 lastSync = r.sidecarWrittenAt < 0 ? 0 : r.sidecarWrittenAt;
            if (sideMtime > lastSync) {
                const QString hash = m_db.photoHash(r.id);
                applyImportedTags(r.id, r.path, hash, ImportMetadata{});
                m_db.setSidecarWrittenAt(r.id, sideMtime);
            }
        }
    }

    // Whatever stayed missing (not a move) is gone: drop the row. Tags remain in
    // the content archive, so they return if the file ever reappears.
    for (int id : missingIds) {
        if (!relocated.contains(id))
            m_db.deletePhoto(id);
    }

    emit rescanFinished();
}

qint64 LibraryWorker::applyImportedTags(int photoId, const QString &path,
                                        const QString &hash, const ImportMetadata &embedded)
{
    const std::optional<sidecar::ParsedTags> side = sidecar::readSidecar(path);
    const bool haveEmbedded = !embedded.flat.isEmpty() || !embedded.categorized.isEmpty();
    if (!side && !haveEmbedded)
        return 0;

    qint64 sidecarMtime = 0;
    if (side) {
        const QString sp = sidecar::sidecarPath(path);
        if (!sp.isEmpty())
            sidecarMtime = QFileInfo(sp).lastModified().toSecsSinceEpoch();
    }

    m_db.beginTransaction();

    // Hash link: a sidecar recording a different content hash means the file was
    // edited after omapic last tagged it; carry that content's archive forward to
    // the current hash so its tags aren't lost.
    if (side && !side->contentHash.isEmpty() && !hash.isEmpty()) {
        QString recorded = side->contentHash;
        const int colon = recorded.indexOf(QLatin1Char(':'));
        if (colon >= 0)
            recorded = recorded.mid(colon + 1); // drop the "sha256:" prefix
        if (!recorded.isEmpty() && recorded != hash) {
            m_db.supersedeHash(recorded, hash);
            m_db.applyContentTags(photoId, hash);
        }
    }

    const auto linkTag = [&](int tagId) {
        if (tagId < 0)
            return;
        m_db.linkTagToContent(photoId, tagId); // every copy of this content
        m_db.rememberContentTag(hash, tagId);
        m_db.applyImplications(hash, tagId);
    };
    const auto applyFlat = [&](const QStringList &names) {
        for (const QString &n : names) {
            const QString t = n.trimmed();
            if (!t.isEmpty())
                linkTag(m_db.ensureTag(t));
        }
    };
    const auto applyCategorized = [&](const QList<QPair<QString, QString>> &pairs) {
        for (const auto &pr : pairs) {
            const QString tagName = pr.second.trimmed();
            if (tagName.isEmpty())
                continue;
            // Only categorize a freshly-created tag — never move an existing one,
            // which the user may have filed deliberately. Imports stay additive.
            const int existing = m_db.tagIdByName(tagName);
            const int tagId = existing >= 0 ? existing : m_db.ensureTag(tagName);
            const QString cat = pr.first.trimmed();
            if (existing < 0 && !cat.isEmpty() && tagId >= 0) {
                const int catId = m_db.ensureCategory(cat);
                if (catId >= 0)
                    m_db.setTagCategory(tagId, catId);
            }
            linkTag(tagId);
        }
    };

    applyFlat(embedded.flat);
    applyCategorized(embedded.categorized);
    if (side) {
        applyFlat(side->flat);
        applyCategorized(side->categorized);
        // Trust omapic-private fields only in a sidecar that is recognisably ours
        // (its contentHash is present).
        if (side->hasOmapicData && !side->contentHash.isEmpty()) {
            for (const QString &album : side->albums) {
                const QString a = album.trimmed();
                if (!a.isEmpty())
                    linkTag(m_db.createAlbum(a));
            }
            if (side->favorite)
                linkTag(m_db.favoritesTagId());
        }
    }

    m_db.commitTransaction();
    return sidecarMtime;
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

void LibraryWorker::flushSidecars()
{
    const QList<int> ids = m_db.sidecarDirtyIds();
    const int total = ids.size();
    emit progress(tr("Exporting tags…"), 0, total);
    const int step = qMax(1, total / 100);

    int done = 0, written = 0, failed = 0;
    for (int id : ids) {
        const QString path = m_db.photoPath(id);
        if (path.isEmpty()) {
            m_db.dequeueSidecarDirty(id); // photo gone; drop the stale entry
            ++done;
            continue;
        }

        // If another app changed the sidecar since we last synced it, import those
        // keywords first so the rewrite below doesn't drop them.
        const QString side = sidecar::sidecarPath(path);
        if (!side.isEmpty()) {
            const qint64 sideMtime = QFileInfo(side).lastModified().toSecsSinceEpoch();
            const qint64 lastSync = m_db.sidecarWrittenAt(id);
            if (sideMtime > (lastSync < 0 ? 0 : lastSync))
                applyImportedTags(id, path, m_db.photoHash(id), ImportMetadata{});
        }

        sidecar::WriteTags tags;
        m_db.sidecarTagsForPhoto(id, tags.contentHash, tags.flat, tags.categorized,
                                 tags.albums, tags.favorite);
        const sidecar::WriteResult res = sidecar::writeSidecar(path, tags);

        if (res == sidecar::WriteResult::Failed) {
            ++failed; // leave the dirty row so a later flush retries
        } else {
            if (res == sidecar::WriteResult::Written || res == sidecar::WriteResult::Removed)
                ++written;
            // Record the sync marker from the file we just wrote (or now, if the
            // empty case removed it), so a later rescan won't re-import our own write.
            const QString after = sidecar::sidecarPath(path);
            const qint64 mtime = after.isEmpty()
                                     ? QDateTime::currentSecsSinceEpoch()
                                     : QFileInfo(after).lastModified().toSecsSinceEpoch();
            m_db.setSidecarWrittenAt(id, mtime);
            m_db.dequeueSidecarDirty(id);
        }

        ++done;
        if (done % step == 0 || done == total)
            emit progress(tr("Exporting tags…"), done, total);
    }

    emit sidecarFlushFinished(written, failed);
}
