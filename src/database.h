// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QList>
#include <QPair>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>
#include <QVector>

#include "photo.h"

// Thin SQLite wrapper. Holds the persistent library; all persistence lives here
// so the models can stay pure in-memory views.
class Database {
public:
    // connectionName lets a second thread open its own connection to the same
    // file (QSqlDatabase connections are per-thread).
    bool open(const QString &path, const QString &connectionName = {});

    bool beginTransaction();  // wrap many writes in one commit (much faster)
    bool commitTransaction();

    QVector<Photo> loadPhotos();         // only photos in the active (shown) set
    QVector<TagInfo> loadTags();         // tags with >=1 active-folder photo, total count (tag manager)
    // Tags applying to >=1 active photo, with active counts. When folderScope is
    // non-empty, restricts to photos at or under that directory (the folder-view
    // tag filter, scoped to the current folder and its subfolders).
    QVector<TagInfo> activeTags(const QString &folderScope = {});
    // Tags on the active photos that carry ALL of `selected` (faceted drill-down),
    // counted over that matching set. Used for the "match all" filter sidebar.
    QVector<TagInfo> coOccurringTags(const QList<int> &selected, const QString &folderScope = {});
    QStringList allTagNames();           // tag names relevant to the active set, for autocomplete
    QVector<FolderInfo> loadFolders(); // registered library folders, with photo counts + enabled

    int insertFolder(const QString &path);         // returns id (existing or new), -1 on error
    QString folderPath(int folderId);              // path for a folder id, empty if unknown
    void removeFolder(int folderId);               // drop the folder row (not its photos)
    void setFolderEnabled(int folderId, bool enabled); // show/hide its photos
    void deletePhotosUnderPath(const QString &folderPath); // delete photos in/under a folder

    int orphanPhotoCount();   // photos not under any registered folder
    void deleteOrphanPhotos(); // remove them (content-tag archive is kept)

    // id (existing or new), -1 on error. takenAt is the capture time in Unix
    // seconds (0 = unknown; stored as NULL), used by the timeline view.
    int insertPhoto(const QString &path, const QString &hash, qint64 takenAt = 0);
    void relocatePhoto(int photoId, const QString &newPath);   // move a row to a new path (rescan)
    void deletePhoto(int photoId);                 // delete one photo row (cascades photo_tags)
    QVector<PhotoRef> allPhotoRefs();              // id/path/hash of every row, for rescan
    int ensureTag(const QString &name);            // returns tag id, creating if needed
    int tagIdByName(const QString &name);          // case-insensitive lookup, -1 if none
    QString tagName(int tagId);                    // current name, empty if unknown
    void renameTag(int tagId, const QString &newName);
    bool linkPhotoTag(int photoId, int tagId);
    bool unlinkPhotoTag(int photoId, int tagId);
    // Link/unlink a tag on every photo row sharing this photo's content hash, so
    // duplicates collapsed out of the gallery stay in lockstep with the visible
    // copy. Falls back to the single row when the hash isn't computed yet.
    void linkTagToContent(int photoId, int tagId);
    void unlinkTagFromContent(int photoId, int tagId);
    void deleteTag(int tagId); // delete a tag and all its photo links

    // Tags on one photo (with category info; count is unused), sorted by name.
    // Excludes reserved/system tags (album membership, favorites), so the detail
    // panel never shows them as chips.
    QVector<TagInfo> photoTags(int photoId);
    // Every tag id on a photo, including reserved/system ones — used to refresh the
    // in-memory model (the album filter and the favorite star need them).
    QSet<int> photoTagIds(int photoId);

    // --- Albums (reserved hidden "Albums" tag-category; Favorites is one album) ---
    // The reserved ids, resolved/created in open(). favoritesTagId's album is
    // always listed first and can't be renamed or deleted.
    int albumCategoryId() const { return m_albumCategoryId; }
    int favoritesTagId() const { return m_favoritesTagId; }
    // Album tags as {id, name, favorite}, Favorites first then by name. Metadata
    // only — per-album counts/covers are computed from the active photo set in
    // Library, so they honor the current tag filter (exactly like folders).
    QVector<AlbumInfo> albumTags();
    QList<int> albumTagIds();          // ids of every album tag (incl. Favorites)
    int tagCategoryId(int tagId);      // a tag's category id, -1 if none/unknown
    // Create an album (a tag in the Albums category). Returns its id; reuses an
    // existing album of the same name (even a hidden/empty one). Returns -1 when
    // the name is blank or already belongs to a non-album tag.
    int createAlbum(const QString &name);

    // --- Smart albums (saved tag queries) ---
    // A smart album is a stored (tag set + AND/OR) query, kept in its own tables
    // (smart_albums / smart_album_tags), not in the tag system. Membership is
    // evaluated live against a photo's tags, so there is nothing in photo_tags.
    QVector<SmartAlbumInfo> smartAlbums();        // all, each with its tag ids, by name
    SmartAlbumInfo smartAlbum(int id);            // one row (for the edit dialog)
    // Create/update return the id (or true) on success, -1/false when the name is
    // blank, the tag set is empty, or the name duplicates another smart album.
    int createSmartAlbum(const QString &name, const QList<int> &tagIds, bool matchAll);
    bool updateSmartAlbum(int id, const QString &name, const QList<int> &tagIds, bool matchAll);
    void deleteSmartAlbum(int id);

    // --- Tag implications ("if this tag, then that tag") ---
    // Every implication with both tags' names/colors, for the manager. Sorted by
    // antecedent then consequent name.
    QVector<ImplicationInfo> implications();
    void addImplication(int antecedentId, int consequentId);    // INSERT OR IGNORE
    void removeImplication(int antecedentId, int consequentId); // stops future propagation only
    // True when antecedent already belongs to consequent's downstream closure, so
    // adding antecedent -> consequent would close a cycle (blocked in the UI).
    bool implicationWouldCycle(int antecedentId, int consequentId);
    // Materialize the consequent closure of `tagId` onto one content hash: write
    // every downstream-implied tag into content_tags and onto every live photo of
    // that hash. Called right after a tag is applied, so implications hold going
    // forward without a full re-scan.
    void applyImplications(const QString &hash, int tagId);
    // Full-library closure pass used when an implication is added: propagate every
    // antecedent's consequents across content_tags and photo_tags until stable
    // (handles chains; terminates on cycles). Returns rows added.
    int expandAllImplications();

    // --- Tag categories ---
    // All categories sorted by name. With activeOnly, drop categories whose tags
    // are all in hidden/removed folders (no tag applies to any active photo),
    // keeping still-empty categories visible so they can be populated. tagCount
    // is always the category's total tag count.
    QVector<CategoryInfo> loadCategories(bool activeOnly = false);
    int insertCategory(const QString &name, int colorSlot); // new id, -1 on error/duplicate
    int categoryIdByName(const QString &name);         // case-insensitive lookup of a user category, -1 if none
    QString categoryName(int categoryId);              // current display name, empty if unknown
    // The three below never touch reserved (system) categories.
    void renameCategory(int categoryId, const QString &newName);
    void setCategoryColor(int categoryId, int colorSlot);
    void deleteCategory(int categoryId);               // its tags become uncategorized
    void setTagCategory(int tagId, int categoryId);    // categoryId < 0 clears it

    // Content-addressed tags: tags are archived against a file's content hash so
    // they survive re-import, restore, move, or an identical copy.
    static QString hashFile(const QString &path);  // SHA-256 hex of contents, empty on error
    QString photoHash(int photoId);                // stored hash for a photo, empty if none
    void applyContentTags(int photoId, const QString &hash); // link archived tags onto a photo
    void rememberContentTag(const QString &hash, int tagId); // archive a tag for this content
    void forgetContentTag(const QString &hash, int tagId);   // drop from archive if no live photo uses it

    // Migration helpers. Hashing is slow, so it runs on a worker thread in
    // bite-sized steps instead of all at once inside open().
    int unhashedPhotoCount();                             // photos still missing a hash
    QVector<QPair<int, QString>> photosMissingHash();     // (id, path) of those photos
    void setPhotoHash(int photoId, const QString &hash);
    void backfillContentTags(); // seed the archive from current photo tags

    // Capture-time backfill (timeline view), same bite-sized worker pattern as
    // the hash backfill: fill taken_at from EXIF/mtime for rows that lack it.
    int photosMissingTakenAtCount();
    QVector<QPair<int, QString>> photosMissingTakenAt(); // (id, path) of rows with NULL taken_at
    void setPhotoTakenAt(int photoId, qint64 takenAt);

    // Paths of every photo in the active (shown) set — orphans plus photos under
    // an enabled folder. Used to build the folder-view navigation tree.
    QStringList activePhotoPaths();
    // Same, restricted to photos matching a tag selection (matchAll = must carry
    // every selected tag; otherwise any). Empty selection returns all active
    // paths. Lets the folder view show only folders holding the filtered photos.
    QStringList activePhotoPaths(const QList<int> &selectedTags, bool matchAll);

private:
    bool hasColumn(const QString &table, const QString &column);
    // SQL boolean (for the given photos-table alias) that is true when a photo is
    // in the active set: an orphan, or under at least one enabled folder.
    QString activeClause(const QString &alias) const;
    // SQL boolean (for the given tags-table alias) that is true for a user-visible
    // tag: one not in a reserved (system) category. Used to keep album/favorites
    // tags out of every tag list, autocomplete, and the detail panel.
    QString nonSystemTagClause(const QString &alias) const;
    // Resolve (creating if needed) the reserved Albums category and Favorites tag,
    // caching their ids. Idempotent; called at the end of open().
    void ensureAlbumsCategory();
    void ensureFavoritesAlbum();

    QSqlDatabase m_db;
    QString m_connectionName;
    int m_albumCategoryId = -1; // reserved "Albums" category (system = 1, stored "_system:Albums")
    int m_favoritesTagId = -1;  // reserved "Favorites" album tag (system = 1)
};
