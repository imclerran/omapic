// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QList>
#include <QObject>
#include <QString>

#include "database.h"
#include "photometadata.h"

// Runs the slow, file-hashing-heavy jobs (startup migration and folder import)
// on a background thread so the GUI stays responsive, reporting progress as it
// goes. It owns its own database connection; the models it never touches — the
// Library reloads them from the DB once a job finishes.
class LibraryWorker : public QObject {
    Q_OBJECT

public:
    explicit LibraryWorker(const QString &dbPath, QObject *parent = nullptr);

public slots:
    void initialize();                        // open the worker's DB connection (in its thread)
    void runMigration();                      // hash un-hashed photos, then seed the tag archive
    void importDirectory(const QString &root); // walk, hash, insert, reattach archived tags
    void rescan();                            // reconcile the DB with disk: add/relocate/remove
    void expandImplications();                // apply tag implications across the whole library
    void addTagToPhotos(const QList<int> &photoIds, const QString &name);
    void removeTagFromPhotos(const QList<int> &photoIds, int tagId);
    void flushSidecars();                     // write queued photos' XMP sidecars
    void flushSidecarsForFolder(int folderId); // queue a folder's photos, then flush

signals:
    void progress(const QString &phase, int done, int total); // total <= 0 means indeterminate
    void migrationFinished();
    void importFinished(const QString &root);
    void rescanFinished();
    void implicationsExpanded();
    void tagJobFinished();
    void sidecarFlushFinished(int written, int failed);

private:
    // Apply a photo's sidecar + embedded tags to its row (and archive them
    // against the content hash), additively. `embedded` carries keywords already
    // read from the file itself (empty on the sidecar-changed rescan path, where
    // only the sidecar is new). If the sidecar records a different content hash,
    // that old hash is superseded so its archived tags carry forward. Returns the
    // sidecar's mtime to record as the sync marker, or 0 when there was no sidecar.
    qint64 applyImportedTags(int photoId, const QString &path, const QString &hash,
                             const ImportMetadata &embedded);

    QString m_dbPath;
    Database m_db;
};
