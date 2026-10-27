// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>
#include <QQmlEngine>
#include <QThread>
#include <QTimer>
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
    // Tag aliases: each entry {aliasName, tagId, tagName, colorSlot} of the
    // canonical tag, ordered by canonical name then alias, for the manager's list.
    Q_PROPERTY(QVariantList tagAliases READ tagAliases NOTIFY aliasesChanged)
    // Albums for the album view: each entry {id, name, favorite, count, thumbs},
    // Favorites first. Counts/covers honor the current tag filter, so an album
    // with no matching photos drops out — just like folders.
    Q_PROPERTY(QVariantList albums READ albums NOTIFY albumsChanged)
    // Every album as {id, name, favorite}, unfiltered — for the "Add to album"
    // menu, which must offer all albums regardless of the current tag/search filter.
    Q_PROPERTY(QVariantList allAlbums READ allAlbums NOTIFY albumsChanged)
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
    // Non-empty when the library could not be opened (e.g. it was written by a
    // newer Omapic). The UI shows it as a blocking message and loads nothing else.
    Q_PROPERTY(QString fatalError READ fatalError NOTIFY fatalErrorChanged)
    // Which desktop integrations are available, resolved once at startup. The
    // context menus use these to show only the actions that can actually work:
    // the theme-wallpaper and Aether items on Omarchy/Aether, a generic "Set as
    // wallpaper" elsewhere. All constant for the process lifetime.
    Q_PROPERTY(bool omarchyAvailable READ omarchyAvailable CONSTANT)
    Q_PROPERTY(bool aetherAvailable READ aetherAvailable CONSTANT)
    Q_PROPERTY(bool wallpaperAvailable READ wallpaperAvailable CONSTANT)
    // When true, tag/album/favorite changes are written back to XMP sidecars
    // automatically (on a short debounce). Bound from the gallery setting; off by
    // default. On-demand export works regardless.
    Q_PROPERTY(bool autoWriteSidecars READ autoWriteSidecars WRITE setAutoWriteSidecars
                   NOTIFY autoWriteSidecarsChanged)
    // How long to keep the archived tags of removed files before forgetting them,
    // in days (0 = forever, the default). Bound from the gallery setting; applied
    // at the end of each rescan.
    Q_PROPERTY(int tagRetentionDays READ tagRetentionDays WRITE setTagRetentionDays
                   NOTIFY tagRetentionDaysChanged)
    // How many removed files still have tags remembered (for the settings stats).
    Q_PROPERTY(int rememberedRemovedCount READ rememberedRemovedCount
                   NOTIFY rememberedRemovedCountChanged)
    // The free OQL query currently applied to the gallery (the builder's Apply),
    // in canonical text; empty when none. It narrows every view on top of the
    // sidebar selection and the search, and the album tiles honour it too.
    Q_PROPERTY(QString advancedQuery READ advancedQuery NOTIFY advancedQueryChanged)

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
    QVariantList tagAliases() const;
    QVariantList albums() const;
    QVariantList allAlbums() const;
    QVariantList folders() const;
    int orphanPhotoCount() const;
    bool busy() const { return m_busy; }
    bool busyModal() const { return m_busyModal; }
    double progress() const { return m_progress; }
    QString statusText() const { return m_statusText; }
    QString fatalError() const { return m_fatalError; }
    bool omarchyAvailable() const { return m_omarchyAvailable; }
    bool aetherAvailable() const { return m_aetherAvailable; }
    bool wallpaperAvailable() const { return m_wallpaperBackend != WallpaperBackend::None; }
    bool autoWriteSidecars() const { return m_autoWriteSidecars; }
    void setAutoWriteSidecars(bool on);
    int tagRetentionDays() const { return m_tagRetentionDays; }
    void setTagRetentionDays(int days);
    int rememberedRemovedCount() const { return m_db.rememberedRemovedCount(); }
    QString advancedQuery() const { return m_advancedText; }
    // Apply query text to the gallery; returns an empty string, or the parse
    // error. clearAdvancedQuery lifts it.
    Q_INVOKABLE QString setAdvancedQuery(const QString &text);
    Q_INVOKABLE void clearAdvancedQuery();

    Q_INVOKABLE void importDirectory(const QUrl &dir);
    Q_INVOKABLE void removeFolder(int folderId);
    Q_INVOKABLE void setFolderEnabled(int folderId, bool enabled);
    Q_INVOKABLE void removeOrphanPhotos();
    Q_INVOKABLE void rescan(); // reconcile the library with disk (adds, moves, deletions)
    // Write XMP sidecars now: for the given photos, or for the whole library.
    // Enqueues them and flushes on the worker; reports via sidecarsExported.
    Q_INVOKABLE void exportSidecars(const QVariantList &photoIds);
    Q_INVOKABLE void exportAllSidecars();
    // Forget the archived tags of every removed file remembered so far (ignores
    // the retention window). Irreversible unless the files are re-added with
    // byte-identical content.
    Q_INVOKABLE void forgetRemovedTags();
    // Tells the library whether the gallery filter is in "match all" mode, so the
    // sidebar can narrow its tags to those on the currently-matching photos.
    Q_INVOKABLE void setMatchAll(bool matchAll);
    // Scope the filter sidebar's tag chips to a folder subtree (folder view). Empty
    // string = the whole active library (flat / timeline views).
    Q_INVOKABLE void setTagFolderScope(const QString &path);
    Q_INVOKABLE void addTag(int photoId, const QString &name);
    Q_INVOKABLE void removeTag(int photoId, int tagId);
    // --- Triage (one-photo-at-a-time tagging with undo) ---
    // The same apply as addTag — aliases, implications, the content archive, the
    // sidecar queue — but it hands back what it inserted, as a map the triage
    // undo stack keeps: {photoId, tagId, tagName, hash, photoLinks: [[photoId,
    // tagId], …], contentLinks: [tagId, …], changed}. `tagName` is the canonical
    // name (an alias resolves to its tag). Empty map when nothing could be applied.
    Q_INVOKABLE QVariantMap addTagRecorded(int photoId, const QString &name);
    // Undo an addTagRecorded: remove exactly the links it inserted, leaving any
    // tag (implied or not) that was already on the photo alone.
    Q_INVOKABLE void revertTagApply(const QVariantMap &record);
    // Undo a removeTag: put the one tag back on the photo (every copy of its
    // content) and in the archive. Implications are not re-run — the remove took
    // only this link, so only this link comes back.
    Q_INVOKABLE void relinkTag(int photoId, int tagId);
    // Whether the photo is in the hidden Favorites album (the triage star button).
    Q_INVOKABLE bool isFavorite(int photoId) const;
    // Move one photo's file to the Trash and drop its row, as deleteFromDisk does,
    // returning what restoreTrashedPhoto needs to bring it back: {ok, id,
    // fileName, path, trashPath, hash, takenAt, mtime, size}. ok is false (and
    // the row kept) when the file couldn't be trashed.
    Q_INVOKABLE QVariantMap trashPhoto(int photoId);
    // Undo a trashPhoto: move the file back out of the Trash and re-insert its row
    // (tags reattach via the content archive). Returns the restored photo's new
    // id, or -1 when the file is no longer in the Trash (or can't be moved back).
    Q_INVOKABLE int restoreTrashedPhoto(const QVariantMap &record);
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
    // Slideshow source URLs for an album-view selection: every active photo in
    // each album in `albumIds` (newest first, as the album shows them, de-duped
    // across albums), followed by the photos named by `photoIds`. Mirrors
    // folderSlideshowSources so selected album tiles play like selected folders.
    Q_INVOKABLE QStringList albumSlideshowSources(const QVariantList &albumIds,
                                                  const QVariantList &photoIds) const;
    Q_INVOKABLE QVariantList commonTags(const QVariantList &photoIds) const;
    Q_INVOKABLE QStringList allTagNames() const;
    // Tags relevant to the active set as {name, colorSlot, categoryName}, for the
    // color-coded autocomplete.
    Q_INVOKABLE QVariantList tagSuggestions() const;
    // Up to `limit` file paths of photos carrying the tag `name` (an alias resolves
    // to its canonical tag), chosen to show the tag at its richest and most
    // varied: the most-tagged photo first (favorites get a head start), then
    // photos adding tags the earlier picks lack. See Database::tagSamplePaths.
    // Feeds the optional thumbnails on the tag-info popover.
    Q_INVOKABLE QStringList tagSamplePaths(const QString &name, int limit) const;

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
    // Create a standalone tag by name (or return an existing one's id),
    // case-insensitively; -1 on a blank name. Lets the Associations tab turn a
    // never-before-seen consequent typed into the "that tag" box into a real tag
    // on the spot, so a rule can be defined before the tag exists anywhere.
    Q_INVOKABLE int ensureTag(const QString &name);

    // --- Tag aliases ("this name means that tag") ---
    // Create an alias `name` for the canonical tag `canonicalTagId`. When `name` is
    // itself an existing tag it is merged into the canonical one on the worker
    // thread (the UI confirms first, since a merge can't be undone); otherwise the
    // alias row is written directly. Returns false when the request is invalid (a
    // blank name, the canonical tag's own name, or a name already used by an alias).
    Q_INVOKABLE bool createTagAlias(const QString &name, int canonicalTagId);
    Q_INVOKABLE void removeTagAlias(const QString &name);

    // --- Albums & favorites ---
    // Create an album and add the given photos to it in one step. Returns the
    // album id, or -1 when the name is blank or already used by a non-album tag.
    // Reuses an existing album of the same name (even a hidden/empty one).
    Q_INVOKABLE int createAlbumWithPhotos(const QString &name, const QVariantList &photoIds);
    Q_INVOKABLE void addPhotosToAlbum(const QVariantList &photoIds, int albumId);
    Q_INVOKABLE void removePhotosFromAlbum(const QVariantList &photoIds, int albumId);
    // Delete a regular album: drop its hidden tag and every membership link. The
    // photos and their other tags are untouched. Favorites is permanent (no-op).
    Q_INVOKABLE void deleteAlbum(int albumId);
    // Album ids that EVERY given photo already belongs to — so the "Add to album"
    // menu can show which albums the selection is already in (a check, or a filled
    // star for Favorites).
    Q_INVOKABLE QVariantList commonAlbumIds(const QVariantList &photoIds) const;
    // The hidden Favorites album's tag id (its album is always listed first).
    Q_INVOKABLE int favoriteAlbumId() const;
    // Toggle one photo's membership in the Favorites album (the thumbnail star).
    Q_INVOKABLE void toggleFavorite(int photoId);
    // Set the Favorites membership for many photos at once.
    Q_INVOKABLE void setFavorite(const QVariantList &photoIds, bool favorite);

    // --- Smart albums (saved OQL queries, shown alongside regular albums) ---
    // True when a UI-facing album id refers to a smart album (reserved high range)
    // rather than a regular album tag.
    Q_INVOKABLE bool isSmartAlbum(int albumId) const;
    // A smart album's definition for the edit dialog: {id, name, text, simple,
    // tags, matchAll}. `text` is the query in OQL; `simple` is true when it is a
    // plain tag set (then `tags` + `matchAll` describe it for the tag picker).
    Q_INVOKABLE QVariantMap smartAlbum(int albumId) const;
    // Create/update a smart album from a tag set (`a b c` or `a | b | c`). Returns
    // the new UI id (or true) on success, or -1/false when the name is
    // blank/duplicate or no tags are selected.
    Q_INVOKABLE int createSmartAlbum(const QString &name, const QVariantList &tagIds, bool matchAll);
    Q_INVOKABLE bool updateSmartAlbum(int albumId, const QString &name,
                                      const QVariantList &tagIds, bool matchAll);
    Q_INVOKABLE void deleteSmartAlbum(int albumId);

    // --- OQL (the query language; see oql.h) ---
    // Parse query text against the library's current names. Returns {ok, canonical
    // (the formatted reading), simple, tags, matchAll, notes: [string]} on success,
    // or {ok: false, message, start, length, suggestions: [string]} on an error.
    Q_INVOKABLE QVariantMap parseQuery(const QString &text) const;
    // The OQL text of a tag set, for seeding the text editor from the picker.
    Q_INVOKABLE QString formatTagQuery(const QVariantList &tagIds, bool matchAll) const;
    // Create (albumId 0) or update a smart album from query text. Returns an empty
    // string on success, else a message to show (a parse error, a duplicate name,
    // or a query that would make the album refer to itself).
    Q_INVOKABLE QString saveSmartAlbumQuery(int albumId, const QString &name, const QString &text);
    // The flat chip reading of query text (oql::tokenize), for the query builder:
    // a list of {type: tag|pred|open|close|or|bad, negated, name, id, kind
    // (album|smart|category for pred), value, colorSlot, categoryName, start,
    // length}. Unknown names come back as `bad` tokens with their raw text.
    Q_INVOKABLE QVariantList tokenizeQuery(const QString &text) const;
    // How many photos of the flat view (active, one per content) match the query
    // text; -1 when it doesn't parse. The builder's live count.
    Q_INVOKABLE int queryMatchCount(const QString &text) const;
    // The names the builder offers after album: and cat: — every album and smart
    // album as {name, smart}, every user category as {name, colorSlot}.
    Q_INVOKABLE QVariantMap queryVocabulary() const;

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
    // Set the photo as the desktop wallpaper via whichever backend is available
    // (Omarchy's own setter, the freedesktop wallpaper portal, or swww/swaybg) —
    // the non-Omarchy/Aether counterpart to the theme actions above. Returns
    // false if no backend is available or the set failed.
    Q_INVOKABLE bool setAsWallpaper(int photoId);
    // Open the photo's containing folder in the file manager (Omarchy's default
    // is Nautilus) with the file itself selected, falling back to just opening
    // the folder. Returns false only if nothing could be opened.
    Q_INVOKABLE bool showInFileManager(int photoId);

    // Same actions addressed by file URL instead of photo id, for the slideshow
    // (which tracks sources, not ids). `source` is a file:// URL.
    Q_INVOKABLE bool addToThemeWallpapersUrl(const QUrl &source);
    Q_INVOKABLE bool editThemeInAetherUrl(const QUrl &source);
    Q_INVOKABLE bool setAetherThemeUrl(const QUrl &source);
    Q_INVOKABLE bool setAsWallpaperUrl(const QUrl &source);

signals:
    void photoChanged(int photoId);
    void foldersChanged();
    void tagsChanged();
    void categoriesChanged();
    void implicationsChanged();
    void aliasesChanged();
    void albumsChanged();
    void busyChanged();
    void progressChanged();
    void statusChanged();
    void fatalErrorChanged();
    void autoWriteSidecarsChanged();
    void tagRetentionDaysChanged();
    void rememberedRemovedCountChanged();
    void advancedQueryChanged();
    // A sidecar export finished: counts of files written and failures, for a toast.
    void sidecarsExported(int written, int failed);

    // Internal: hands a job to the worker thread via a queued connection.
    void requestMigration();
    void requestImport(const QString &root);
    void requestRescan(int retentionDays);
    void requestImplicationMigration();
    void requestCreateAlias(const QString &name, int tagId);
    void requestAddTagToPhotos(const QList<int> &photoIds, const QString &name);
    void requestRemoveTagFromPhotos(const QList<int> &photoIds, int tagId);
    void requestSidecarFlush();
    void requestFacetTags(const QString &folderScope, const QList<int> &selected, bool matchAll,
                          const QList<int> &scopeIds, bool scoped);
    void requestResample();

private:
    // How to set the desktop wallpaper, resolved once at startup. Omarchy's own
    // setter is preferred where present (it keeps Omarchy's symlink + shell IPC);
    // otherwise the freedesktop portal (GNOME/KDE), otherwise a bare-Wayland
    // daemon (swww/swaybg). None means no "Set as wallpaper" action is offered.
    enum class WallpaperBackend { None, Omarchy, Portal, Swww, Swaybg };

    // Path-based cores shared by the id- and url-addressed theme actions above.
    bool addPathToThemeWallpapers(const QString &path);
    bool editThemeInAetherPath(const QString &path);
    bool setAetherThemePath(const QString &path);
    bool setPathAsWallpaper(const QString &path);
    bool showPathInFileManager(const QString &path);

    // One-time capability probes, called from the constructor.
    static bool detectOmarchy();
    static bool detectAether();
    static WallpaperBackend detectWallpaperBackend(bool omarchyAvailable);

    void applyTags(int photoId);
    // The shared core of addTag / addTagRecorded: resolve the name to a tag
    // (creating it unless it's an alias), apply it with its implications, queue
    // the sidecar, and refresh the models. Returns what was inserted.
    TagApplyRecord applyTagCore(int photoId, const QString &name);
    void refreshTags();          // synchronous: query + apply on the GUI thread
    void refreshTagsAsync();     // query on the worker, apply via onFacetTagsReady
    void applyFacetTags(const QVector<TagInfo> &tags); // set the model + notify (GUI)
    // Push each tag's alias names into the photo model, so a gallery search for an
    // alias finds photos carrying its canonical tag. Called at startup and whenever
    // the alias set changes.
    void updateSearchAliases();
    void onFacetTagsReady(const QVector<TagInfo> &tags);
    // Keep the filter model's list of registered folder roots in step with the
    // DB, so the folder view can tell orphan photos from foldered ones.
    void updateFolderRoots();
    // Keep the filter model's album tag id set in step, for the flattened album
    // search ("photos in any album"). Called when the album set changes.
    void updateAlbumTagIds();
    // --- OQL state ---
    // Rebuild what evaluation needs from the database — the EvalContext and every
    // smart album's expanded tree (album: references replaced by the referenced
    // album's current tree, cycles reported) — and hand them to the filter model.
    // Called whenever tags, categories, albums or smart albums change; the trees
    // are then reused for every photo of a filter pass, never re-resolved per photo.
    void rebuildQueryState();
    // The membership query for a UI-facing album id: an Album leaf for a regular
    // album, the expanded tree for a smart album, an empty (never-matching) AND
    // for anything else.
    oql::Node albumTree(int albumId) const;
    // A fresh vocabulary over the library's current names.
    oql::MapVocabulary vocabulary() const;
    // Ref-counted so overlapping worker jobs (e.g. a bulk tag started mid-import)
    // don't fight over one busy flag: the indicator stays up until every job has
    // called its matching endBusy. Pass the same `modal` to endBusy as to beginBusy.
    void beginBusy(const QString &status, bool modal = true);
    void endBusy(bool modal);
    void updateBusyState(); // recompute m_busy/m_busyModal from the counts, notify on change

    void onWorkerProgress(const QString &phase, int done, int total);
    void onMigrationFinished(bool changed);
    void onImportFinished(const QString &root);
    void onRescanFinished(bool changed);
    void onResampleFinished(bool changed);
    void onImplicationsExpanded();
    void onAliasCreated();
    void onTagJobFinished();
    void onSidecarFlushFinished(int written, int failed);
    // If auto-write is on, (re)start the debounce so a burst of edits coalesces
    // into one flush. No-op when auto-write is off.
    void scheduleSidecarFlush();
    // If auto-write is on and photos are queued, flush now with the progress bar
    // (but no toast). For user actions that can release a large backlog at once:
    // enabling auto-write, or re-enabling a folder whose photos were queued while
    // it was disabled. No-op when auto-write is off or nothing is pending.
    void catchUpSidecarsIfPending();

    mutable Database m_db;
    PhotoModel *m_photos;
    PhotoFilterModel *m_filtered;
    TagModel *m_tags;

    oql::EvalContext m_evalContext;
    QHash<int, oql::Node> m_smartTrees; // smart album id -> expanded query (see rebuildQueryState)
    oql::Node m_advancedRoot;           // the applied free query, unexpanded (re-expanded with the trees)
    oql::Node m_advancedTree;           // its expansion against the current smart-album trees
    QString m_advancedText;             // its canonical text; empty = none applied
    // The photos the applied query matches (every active photo, copies included,
    // as the sidebar counts them), for scoping the facet tags; empty when none.
    QList<int> advancedScopeIds() const;

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
    QString m_fatalError; // non-empty = the library could not be opened; UI blocks

    // Desktop-integration capabilities, probed once in the constructor.
    bool m_omarchyAvailable = false;
    bool m_aetherAvailable = false;
    WallpaperBackend m_wallpaperBackend = WallpaperBackend::None;

    bool m_autoWriteSidecars = false;
    int m_tagRetentionDays = 0; // 0 = keep removed files' tags forever
    QTimer *m_sidecarDebounce = nullptr; // coalesces auto-write flushes after edits
    int m_explicitFlushes = 0; // in-flight user-triggered exports (show busy + toast)
    int m_quietBarFlushes = 0; // in-flight catch-up flushes (show busy, no toast)
};
