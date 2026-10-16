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
    // Tag categories: {id, name, colorSlot, tagCount}, sorted by name.
    Q_PROPERTY(QVariantList categories READ categories NOTIFY categoriesChanged)
    // Tag implications ("if this tag, then that tag"): each entry carries both
    // tags' {id, name, colorSlot} for the manager's chips.
    Q_PROPERTY(QVariantList tagImplications READ tagImplications NOTIFY implicationsChanged)
    Q_PROPERTY(QVariantList folders READ folders NOTIFY foldersChanged)
    Q_PROPERTY(int orphanPhotoCount READ orphanPhotoCount NOTIFY foldersChanged)
    // A long-running background job (startup migration or folder import) is in
    // progress; progress is 0..1, or < 0 when the total isn't known yet.
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    // When busy: true shows the blocking overlay (the one-time startup
    // migration), false shows the non-blocking bottom status bar (import,
    // rescan, bulk tagging).
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

    enum ImplicationResult {
        ImplicationOk,      // added; a background pass applies it to existing photos
        ImplicationInvalid, // missing/equal tags
        ImplicationCycle,   // would close a loop (this -> that -> ... -> this)
        ImplicationExists,  // that rule is already defined
    };
    Q_ENUM(ImplicationResult)

    explicit Library(QObject *parent = nullptr);
    ~Library() override;

    PhotoModel *photos() const { return m_photos; }
    PhotoFilterModel *filtered() const { return m_filtered; }
    TagModel *tags() const { return m_tags; }
    QVariantList allTags() const;
    QVariantList categories() const;
    QVariantList tagImplications() const;
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
    // Scope the filter sidebar's tag chips to a folder subtree (folder view). Empty
    // string = the whole active library (flat / timeline views).
    Q_INVOKABLE void setTagFolderScope(const QString &path);
    Q_INVOKABLE void addTag(int photoId, const QString &name);
    Q_INVOKABLE void removeTag(int photoId, int tagId);
    Q_INVOKABLE void addTagToPhotos(const QVariantList &photoIds, const QString &name);
    Q_INVOKABLE void removeTagFromPhotos(const QVariantList &photoIds, int tagId);
    Q_INVOKABLE void deleteTag(int tagId);
    Q_INVOKABLE int renameTag(int tagId, const QString &newName); // returns a RenameResult
    Q_INVOKABLE QVariantMap photoInfo(int photoId) const;

    // Navigable entries for the folder view at `path`. When `path` is empty
    // (the root), returns one {type:"folder", name, path, count} per enabled
    // root library folder. Otherwise returns a leading {type:"up", name, path}
    // entry (path = the parent dir, or "" to return to the root listing) followed
    // by the immediate subfolders of `path` that hold active photos. Orphan
    // photos (under no folder) aren't listed here — they appear as photos in the
    // grid at the root level.
    Q_INVOKABLE QVariantList folderEntries(const QString &path) const;
    // The folder to navigate to when going "up" from `path`: empty for the root
    // listing or when `path` is itself a registered root, otherwise the parent dir.
    Q_INVOKABLE QString upFolder(const QString &path) const;
    // Slideshow source URLs for a folder-view selection: every active photo under
    // each folder in `folderPaths` (recursively, in path order), followed by the
    // photos named by `photoIds`. Used to play selected folders (all their
    // contents) together with selected loose photos.
    Q_INVOKABLE QStringList folderSlideshowSources(const QStringList &folderPaths,
                                                   const QVariantList &photoIds) const;
    Q_INVOKABLE QVariantList commonTags(const QVariantList &photoIds) const;
    Q_INVOKABLE QStringList allTagNames() const;
    // Tags relevant to the active set as {name, colorSlot, categoryName}, for the
    // color-coded autocomplete.
    Q_INVOKABLE QVariantList tagSuggestions() const;

    // --- Tag categories ---
    // Create a category, auto-picking the first palette slot no other category
    // uses (the least-used slot once all are taken). `paletteSize` is the number
    // of slots available (OmarchyTheme::categoryPalette). Returns the new id, or
    // -1 when the name is blank or already taken (case-insensitive).
    Q_INVOKABLE int createCategory(const QString &name, int paletteSize);
    Q_INVOKABLE int renameCategory(int categoryId, const QString &newName); // RenameResult
    Q_INVOKABLE void setCategoryColor(int categoryId, int colorSlot);
    Q_INVOKABLE void deleteCategory(int categoryId); // its tags become uncategorized
    Q_INVOKABLE void setTagCategory(int tagId, int categoryId); // categoryId < 0 clears

    // --- Tag implications ("if this tag, then that tag") ---
    // Define antecedent -> consequent, then apply it to existing photos on the
    // worker thread. Returns an ImplicationResult. Adding one never removes tags;
    // deleting a rule (removeTagImplication) only stops future propagation and
    // leaves already-applied tags in place.
    Q_INVOKABLE int addTagImplication(int antecedentId, int consequentId);
    Q_INVOKABLE void removeTagImplication(int antecedentId, int consequentId);
    // Whether antecedent -> consequent would close a loop, for live UI validation.
    Q_INVOKABLE bool implicationWouldCycle(int antecedentId, int consequentId) const;

    // --- Context-menu actions (operate on one or many selected photos) ---
    // Drop the photos from the library only; the files on disk are left untouched.
    Q_INVOKABLE void removeFromLibrary(const QVariantList &photoIds);
    // Move the files to the Trash, then drop them from the library. Returns the
    // number that couldn't be trashed (those keep their library rows), so a photo
    // is never silently lost from the library while still present on disk.
    Q_INVOKABLE int deleteFromDisk(const QVariantList &photoIds);
    // Copy the photo into the active theme's user backgrounds folder
    // (~/.config/omarchy/backgrounds/<theme>/), so it joins that theme's wallpaper
    // rotation — and only that theme's. Returns false on failure.
    Q_INVOKABLE bool addToThemeWallpapers(int photoId);
    // Load the photo into Aether's GUI editor to craft a new theme from it,
    // without applying anything yet. Returns false if the helper couldn't start.
    Q_INVOKABLE bool editThemeInAether(int photoId);
    // Generate a desktop theme from the photo and apply it immediately via Aether.
    // Returns false if the process couldn't be launched.
    Q_INVOKABLE bool setAetherTheme(int photoId);
    // Open the photo's containing folder in the file manager (Omarchy's default
    // is Nautilus) with the file itself selected. Returns false if the helper
    // couldn't start.
    Q_INVOKABLE bool showInFileManager(int photoId);

    // Same three actions addressed by file URL instead of photo id, for the
    // slideshow (which tracks sources, not ids). `source` is a file:// URL.
    Q_INVOKABLE bool addToThemeWallpapersUrl(const QUrl &source);
    Q_INVOKABLE bool editThemeInAetherUrl(const QUrl &source);
    Q_INVOKABLE bool setAetherThemeUrl(const QUrl &source);

signals:
    void photoChanged(int photoId);
    void foldersChanged();
    void tagsChanged();
    void categoriesChanged();
    void implicationsChanged();
    void busyChanged();
    void progressChanged();
    void statusChanged();

    // Internal: hands a job to the worker thread via a queued connection.
    void requestMigration();
    void requestImport(const QString &root);
    void requestRescan();
    void requestImplicationMigration();
    void requestAddTagToPhotos(const QList<int> &photoIds, const QString &name);
    void requestRemoveTagFromPhotos(const QList<int> &photoIds, int tagId);

private:
    // Path-based cores shared by the id- and url-addressed theme actions above.
    bool addPathToThemeWallpapers(const QString &path);
    bool editThemeInAetherPath(const QString &path);
    bool setAetherThemePath(const QString &path);
    bool showPathInFileManager(const QString &path);

    void applyTags(int photoId);
    void refreshTags();
    // Keep the filter model's list of registered folder roots in step with the
    // DB, so the folder view can tell orphan photos from foldered ones.
    void updateFolderRoots();
    // Ref-counted so overlapping worker jobs (e.g. a bulk tag started mid-import)
    // don't fight over one busy flag: the indicator stays up until every job has
    // called its matching endBusy. Pass the same `modal` to endBusy as to beginBusy.
    void beginBusy(const QString &status, bool modal = true);
    void endBusy(bool modal);
    void updateBusyState(); // recompute m_busy/m_busyModal from the counts, notify on change

    void onWorkerProgress(const QString &phase, int done, int total);
    void onMigrationFinished();
    void onImportFinished(const QString &root);
    void onRescanFinished();
    void onImplicationsExpanded();
    void onTagJobFinished();

    mutable Database m_db;
    PhotoModel *m_photos;
    PhotoFilterModel *m_filtered;
    TagModel *m_tags;

    QThread m_workerThread;
    LibraryWorker *m_worker = nullptr;

    bool m_matchAll = true; // mirrors the gallery's "match all tags" toggle
    QString m_tagFolderScope; // folder-view subtree the tag chips are scoped to ("" = all)
    int m_busyCount = 0;    // active jobs; m_busy mirrors (m_busyCount > 0)
    int m_modalCount = 0;   // active jobs that want the blocking overlay
    bool m_busy = false;
    bool m_busyModal = false;
    double m_progress = -1.0;
    QString m_statusText;
};
