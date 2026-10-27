// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QCollator>
#include <QQmlEngine>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <functional>

#include "oql.h"

// Filters the photo list down to those matching the selected tag set.
// matchAll == true  -> photo must carry every selected tag (intersection / narrow)
// matchAll == false -> photo must carry at least one selected tag (union)
// Both that selection and the album-view gate are evaluated as OQL trees
// (oql::matches), so a smart album and the sidebar read the same way.
class PhotoFilterModel : public QSortFilterProxyModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by Library")
    Q_PROPERTY(QList<int> selectedTagIds READ selectedTagIds WRITE setSelectedTagIds NOTIFY selectedTagIdsChanged)
    Q_PROPERTY(bool matchAll READ matchAll WRITE setMatchAll NOTIFY matchAllChanged)
    Q_PROPERTY(QString searchText READ searchText WRITE setSearchText NOTIFY searchTextChanged)
    Q_PROPERTY(bool folderFirst READ folderFirst WRITE setFolderFirst NOTIFY folderFirstChanged)
    // How the flat / folder / album grids order photos (the timeline always sorts
    // by date). SortByName is natural filename order; SortByDate is newest first;
    // SortByTagCount is most-tagged first. Each falls back to the file name to keep
    // ties stable.
    Q_PROPERTY(SortMode sortMode READ sortMode WRITE setSortMode NOTIFY sortModeChanged)
    // Sort direction. Ascending means A→Z / oldest first / fewest tags first;
    // descending reverses the primary key (the file-name tiebreak stays ascending
    // so ties keep a stable order). The timeline always uses descending date.
    Q_PROPERTY(bool sortAscending READ sortAscending WRITE setSortAscending NOTIFY sortAscendingChanged)
    // Folder view: when on, restrict to one directory level (see folderPath).
    Q_PROPERTY(bool folderMode READ folderMode WRITE setFolderMode NOTIFY folderModeChanged)
    // The directory currently shown in folder view. Empty = the root level,
    // which shows orphan photos (those under no registered folder).
    Q_PROPERTY(QString folderPath READ folderPath WRITE setFolderPath NOTIFY folderPathChanged)
    // Hide non-representative duplicates (same content hash). On for the ungrouped
    // view; off for timeline and folder views, which show every copy.
    Q_PROPERTY(bool dedupe READ dedupe WRITE setDedupe NOTIFY dedupeChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    // Photo ordering for the flat / folder / album grids (the timeline forces
    // SortByDate). Each mode breaks ties on the file name for a stable order.
    enum SortMode { SortByName, SortByDate, SortByTagCount };
    Q_ENUM(SortMode)

    explicit PhotoFilterModel(QObject *parent = nullptr);

    QList<int> selectedTagIds() const;
    void setSelectedTagIds(const QList<int> &ids);

    bool matchAll() const { return m_matchAll; }
    void setMatchAll(bool on);

    QString searchText() const { return m_search; }
    void setSearchText(const QString &text); // matches file name or any tag name

    bool folderFirst() const { return m_folderFirst; }
    void setFolderFirst(bool on); // group by containing folder before file name

    SortMode sortMode() const { return m_sortMode; }
    void setSortMode(SortMode mode); // re-sorts the grid (ordering only)

    bool sortAscending() const { return m_sortAscending; }
    void setSortAscending(bool on); // re-sorts the grid (ordering only)

    bool folderMode() const { return m_folderMode; }
    void setFolderMode(bool on);

    QString folderPath() const { return m_folderPath; }
    void setFolderPath(const QString &path);

    bool dedupe() const { return m_dedupe; }
    void setDedupe(bool on); // hide duplicates (ungrouped view only)

    // Apply a whole view mode at once (dedup, sort order, folder restriction) with
    // a single re-filter + re-sort. Switching views changes several of these
    // together; setting them one at a time made the proxy re-sort once per change
    // — e.g. a wasted name-sort before the date-sort when opening the timeline.
    // albumId is the current album (a UI-facing id: a regular album's tag id or
    // a smart album's offset id); 0 means the album-list level (tiles, not a photo
    // grid). Its membership query comes from the resolver below.
    Q_INVOKABLE void applyView(bool dedupe, int sortMode, bool sortAscending, bool folderMode,
                               const QString &folderPath, bool albumMode = false, int albumId = 0);

    // How an album id becomes its membership query: an expanded OQL tree (a
    // regular album is an Album leaf; a smart album its saved query with every
    // album: reference replaced by that album's current tree). Set by Library,
    // which owns the definitions; resolved once per applyView / refreshAlbumQuery,
    // never per photo.
    void setAlbumQueryResolver(std::function<oql::Node(int albumId)> resolver);
    // The library facts the evaluator needs (Favorites id, album tag ids, the
    // tag -> category map). Re-filters in album mode, where they can matter.
    void setEvalContext(const oql::EvalContext &ctx);
    // Re-resolve the current album's query (its definition, or one it refers
    // to, changed) and re-filter if the tree differs.
    void refreshAlbumQuery();

    // The registered folder roots, used to tell orphan photos (shown at the
    // folder-view root) from foldered ones. Set by Library; not a QML property.
    void setFolderRoots(const QStringList &roots);

    // Every album tag id (Library keeps this in step), so the album view's
    // flattened search ("photos in any album") can be answered from a row's tags.
    void setAlbumTagIds(const QList<int> &ids);

    int count() const { return rowCount(); }

    // Map a proxy row to its image URL (used by the slideshow).
    Q_INVOKABLE QUrl sourceAt(int row) const;
    // Map a proxy row to its photo id (used for range selection).
    Q_INVOKABLE int idAt(int row) const;
    // The display data a timeline tile needs for one proxy row:
    // {id, filePath, fileName, isVideo, tagCount}. Empty map for a bad row.
    Q_INVOKABLE QVariantMap photoAt(int row) const;
    // The timeline's month sections over the current (date-sorted) order, newest
    // first: a list of {key, label, firstRow, count}. `label` is a localized
    // "Month Year" (or "Undated" for photos with no capture time).
    Q_INVOKABLE QVariantList monthGroups() const;
    // Whether the photo with this id currently passes the filter (is visible in
    // the gallery). Used to prune the selection when the filter changes.
    Q_INVOKABLE bool containsId(int id) const;

signals:
    void selectedTagIdsChanged();
    void matchAllChanged();
    void searchTextChanged();
    void folderFirstChanged();
    void sortModeChanged();
    void sortAscendingChanged();
    void folderModeChanged();
    void folderPathChanged();
    void dedupeChanged();
    void countChanged();

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;
    // Natural, case-insensitive ordering of file names: runs of digits compare by
    // value (3 < 22 < 111), so numbered files sort as a person would expect, and
    // the rest of the name compares alphabetically. When folderFirst is on, photos
    // are grouped by containing folder (compared the same way) before file name.
    // The comparison is photosort::lessThan, shared with the CLI.
    bool lessThan(const QModelIndex &left, const QModelIndex &right) const override;

private:
    // Re-run the row filter. Wraps the begin/endFilterChange API on Qt that has it
    // (invalidateFilter() is deprecated there), falling back for older Qt.
    void invalidateRowFilter();

    // True when the photo passes the folder-view directory gate (always true when
    // folderMode is off). Factored out of filterAcceptsRow for clarity.
    bool passesFolderGate(const QString &path) const;

    QSet<int> m_selected;
    bool m_matchAll = true;
    QString m_search;
    QString m_searchFolded; // m_search, accent-/case-folded for matching
    bool m_folderFirst = false;
    SortMode m_sortMode = SortByName;
    bool m_sortAscending = true;
    bool m_folderMode = false;
    QString m_folderPath;
    bool m_dedupe = true; // ungrouped is the default view, which dedupes
    bool m_albumMode = false;   // album view: restrict to one album (or the list level)
    int m_albumId = 0;          // the viewed album's UI id; 0 = the list level
    oql::Node m_albumQuery;     // its expanded membership query (valid when m_albumId > 0)
    std::function<oql::Node(int)> m_albumResolver;
    oql::Node m_selectedQuery;  // m_selected + m_matchAll as a tree
    oql::EvalContext m_ctx;
    QStringList m_folderRoots;
    QSet<int> m_albumTagIds;    // every album tag id (for the flattened album search)
    QCollator m_collator;
};
