// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>
#include <QQmlEngine>
#include <QThread>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include "database.h"
#include "photofiltermodel.h"
#include "photomodel.h"
#include "tagmodel.h"

class LibraryWorker;

// The single object QML talks to. Owns the database and the three models, and
// routes every mutation through the database so the in-memory views stay in sync.
class Library : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(PhotoModel *photos READ photos CONSTANT)
    Q_PROPERTY(PhotoFilterModel *filtered READ filtered CONSTANT)
    Q_PROPERTY(TagModel *tags READ tags CONSTANT)
    Q_PROPERTY(QVariantList allTags READ allTags NOTIFY tagsChanged)
    Q_PROPERTY(QVariantList folders READ folders NOTIFY foldersChanged)
    Q_PROPERTY(int orphanPhotoCount READ orphanPhotoCount NOTIFY foldersChanged)
    // A long-running background job (startup migration or folder import) is in
    // progress; progress is 0..1, or < 0 when the total isn't known yet.
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    // When busy: true shows the blocking overlay (import/migration/rescan), false
    // shows the non-blocking bottom status bar (bulk tagging).
    Q_PROPERTY(bool busyModal READ busyModal NOTIFY busyChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)

public:
    enum RenameResult {
        RenameOk,       // renamed (or only the casing changed)
        RenameEmpty,    // the new name was blank
        RenameConflict, // another tag already uses that name (case-insensitive)
    };
    Q_ENUM(RenameResult)

    explicit Library(QObject *parent = nullptr);
    ~Library() override;

    PhotoModel *photos() const { return m_photos; }
    PhotoFilterModel *filtered() const { return m_filtered; }
    TagModel *tags() const { return m_tags; }
    QVariantList allTags() const;
    QVariantList folders() const;
    int orphanPhotoCount() const;
    bool busy() const { return m_busy; }
    bool busyModal() const { return m_busyModal; }
    double progress() const { return m_progress; }
    QString statusText() const { return m_statusText; }

    Q_INVOKABLE void importDirectory(const QUrl &dir);
    Q_INVOKABLE void removeFolder(int folderId);
    Q_INVOKABLE void setFolderEnabled(int folderId, bool enabled);
    Q_INVOKABLE void removeOrphanPhotos();
    Q_INVOKABLE void rescan(); // reconcile the library with disk (adds, moves, deletions)
    // Tells the library whether the gallery filter is in "match all" mode, so the
    // sidebar can narrow its tags to those on the currently-matching photos.
    Q_INVOKABLE void setMatchAll(bool matchAll);
    Q_INVOKABLE void addTag(int photoId, const QString &name);
    Q_INVOKABLE void removeTag(int photoId, int tagId);
    Q_INVOKABLE void addTagToPhotos(const QVariantList &photoIds, const QString &name);
    Q_INVOKABLE void removeTagFromPhotos(const QVariantList &photoIds, int tagId);
    Q_INVOKABLE void deleteTag(int tagId);
    Q_INVOKABLE int renameTag(int tagId, const QString &newName); // returns a RenameResult
    Q_INVOKABLE QVariantMap photoInfo(int photoId) const;
    Q_INVOKABLE QVariantList commonTags(const QVariantList &photoIds) const;
    Q_INVOKABLE QStringList allTagNames() const;

signals:
    void photoChanged(int photoId);
    void foldersChanged();
    void tagsChanged();
    void busyChanged();
    void progressChanged();
    void statusChanged();

    // Internal: hands a job to the worker thread via a queued connection.
    void requestMigration();
    void requestImport(const QString &root);
    void requestRescan();
    void requestAddTagToPhotos(const QList<int> &photoIds, const QString &name);
    void requestRemoveTagFromPhotos(const QList<int> &photoIds, int tagId);

private:
    void applyTags(int photoId);
    void refreshTags();
    void beginBusy(const QString &status, bool modal = true);
    void endBusy();

    void onWorkerProgress(const QString &phase, int done, int total);
    void onMigrationFinished();
    void onImportFinished(const QString &root);
    void onRescanFinished();
    void onTagJobFinished();

    mutable Database m_db;
    PhotoModel *m_photos;
    PhotoFilterModel *m_filtered;
    TagModel *m_tags;

    QThread m_workerThread;
    LibraryWorker *m_worker = nullptr;

    bool m_matchAll = true; // mirrors the gallery's "match all tags" toggle
    bool m_busy = false;
    bool m_busyModal = true;
    double m_progress = -1.0;
    QString m_statusText;
};
