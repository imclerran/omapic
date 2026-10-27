// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#include "photofiltermodel.h"

#include <QDate>
#include <QDateTime>
#include <QLocale>
#include <QString>
#include <QStringList>

#include "photomodel.h"
#include "photosort.h"
#include "textfold.h"

PhotoFilterModel::PhotoFilterModel(QObject *parent)
    : QSortFilterProxyModel(parent)
{
    m_collator = photosort::collator();

    connect(this, &QAbstractItemModel::rowsInserted, this, &PhotoFilterModel::countChanged);
    connect(this, &QAbstractItemModel::rowsRemoved, this, &PhotoFilterModel::countChanged);
    connect(this, &QAbstractItemModel::modelReset, this, &PhotoFilterModel::countChanged);
    connect(this, &QAbstractItemModel::layoutChanged, this, &PhotoFilterModel::countChanged);
}

void PhotoFilterModel::invalidateRowFilter()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 11, 0)
    beginFilterChange();
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
#else
    invalidateFilter();
#endif
}

QList<int> PhotoFilterModel::selectedTagIds() const
{
    return QList<int>(m_selected.begin(), m_selected.end());
}

void PhotoFilterModel::setSelectedTagIds(const QList<int> &ids)
{
    QSet<int> next(ids.begin(), ids.end());
    if (next == m_selected)
        return;
    m_selected = next;
    m_selectedQuery = oql::fromTagSet(ids, m_matchAll);
    invalidateRowFilter();
    emit selectedTagIdsChanged();
    emit countChanged();
}

void PhotoFilterModel::setMatchAll(bool on)
{
    if (m_matchAll == on)
        return;
    m_matchAll = on;
    m_selectedQuery = oql::fromTagSet(selectedTagIds(), m_matchAll);
    invalidateRowFilter();
    emit matchAllChanged();
    emit countChanged();
}

void PhotoFilterModel::setSearchText(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (m_search == trimmed)
        return;
    m_search = trimmed;
    // Pre-fold once so the per-row match is a plain folded substring test, and so
    // typing unaccented text ("cafe") matches accented names ("café").
    m_searchFolded = foldText(trimmed);
    invalidateRowFilter();
    emit searchTextChanged();
    emit countChanged();
}

void PhotoFilterModel::setFolderFirst(bool on)
{
    if (m_folderFirst == on)
        return;
    m_folderFirst = on;
    // Only the ordering changes, so re-sort without touching the row filter.
    invalidate();
    emit folderFirstChanged();
}

void PhotoFilterModel::setSortMode(SortMode mode)
{
    if (m_sortMode == mode)
        return;
    m_sortMode = mode;
    invalidate(); // ordering only
    emit sortModeChanged();
}

void PhotoFilterModel::setSortAscending(bool on)
{
    if (m_sortAscending == on)
        return;
    m_sortAscending = on;
    invalidate(); // ordering only
    emit sortAscendingChanged();
}

void PhotoFilterModel::setFolderMode(bool on)
{
    if (m_folderMode == on)
        return;
    m_folderMode = on;
    invalidateRowFilter();
    emit folderModeChanged();
    emit countChanged();
}

void PhotoFilterModel::setFolderPath(const QString &path)
{
    if (m_folderPath == path)
        return;
    m_folderPath = path;
    invalidateRowFilter();
    emit folderPathChanged();
    emit countChanged();
}

void PhotoFilterModel::setDedupe(bool on)
{
    if (m_dedupe == on)
        return;
    m_dedupe = on;
    invalidateRowFilter();
    emit dedupeChanged();
    emit countChanged();
}

void PhotoFilterModel::applyView(bool dedupe, int sortMode, bool sortAscending, bool folderMode,
                                 const QString &folderPath, bool albumMode, int albumId)
{
    const SortMode mode = static_cast<SortMode>(sortMode);
    bool changed = false;
    if (m_dedupe != dedupe) { m_dedupe = dedupe; emit dedupeChanged(); changed = true; }
    if (m_sortMode != mode) { m_sortMode = mode; emit sortModeChanged(); changed = true; }
    if (m_sortAscending != sortAscending) { m_sortAscending = sortAscending; emit sortAscendingChanged(); changed = true; }
    if (m_folderMode != folderMode) { m_folderMode = folderMode; emit folderModeChanged(); changed = true; }
    if (m_folderPath != folderPath) { m_folderPath = folderPath; emit folderPathChanged(); changed = true; }
    if (m_albumMode != albumMode) { m_albumMode = albumMode; changed = true; }
    if (!albumMode)
        albumId = 0;
    if (m_albumId != albumId) { m_albumId = albumId; changed = true; }
    if (albumMode && albumId > 0) {
        // Resolve the album's query once here; filterAcceptsRow only evaluates.
        const oql::Node query = m_albumResolver ? m_albumResolver(albumId) : oql::Node::andOf({});
        if (query != m_albumQuery) { m_albumQuery = query; changed = true; }
    }
    if (!changed)
        return;
    // One re-filter + re-sort against the final mode, rather than one per setter.
    invalidate();
    emit countChanged();
}

void PhotoFilterModel::setAlbumQueryResolver(std::function<oql::Node(int)> resolver)
{
    m_albumResolver = std::move(resolver);
    refreshAlbumQuery();
}

void PhotoFilterModel::setEvalContext(const oql::EvalContext &ctx)
{
    m_ctx = ctx;
    // Only the album gate can depend on these (cat:, is:fav, is:untagged); the
    // sidebar selection is a plain tag set.
    if (m_albumMode && m_albumId > 0) {
        invalidateRowFilter();
        emit countChanged();
    }
}

void PhotoFilterModel::refreshAlbumQuery()
{
    if (!m_albumMode || m_albumId <= 0 || !m_albumResolver)
        return;
    const oql::Node query = m_albumResolver(m_albumId);
    if (query == m_albumQuery)
        return;
    m_albumQuery = query;
    invalidateRowFilter();
    emit countChanged();
}

void PhotoFilterModel::setAdvancedQuery(const oql::Node &tree)
{
    if (m_advancedActive && tree == m_advanced)
        return;
    m_advanced = tree;
    m_advancedActive = true;
    invalidateRowFilter();
    emit countChanged();
}

void PhotoFilterModel::clearAdvancedQuery()
{
    if (!m_advancedActive)
        return;
    m_advancedActive = false;
    m_advanced = oql::Node();
    invalidateRowFilter();
    emit countChanged();
}

void PhotoFilterModel::setFolderRoots(const QStringList &roots)
{
    if (m_folderRoots == roots)
        return;
    m_folderRoots = roots;
    // Only matters at the folder-view root (orphan detection); re-filter there.
    if (m_folderMode && m_folderPath.isEmpty()) {
        invalidateRowFilter();
        emit countChanged();
    }
}

void PhotoFilterModel::setAlbumTagIds(const QList<int> &ids)
{
    const QSet<int> next(ids.begin(), ids.end());
    if (next == m_albumTagIds)
        return;
    m_albumTagIds = next;
    // Only affects the flattened album-list search; re-filter there.
    if (m_albumMode && m_albumId <= 0 && !m_search.isEmpty()) {
        invalidateRowFilter();
        emit countChanged();
    }
}

QUrl PhotoFilterModel::sourceAt(int row) const
{
    if (row < 0 || row >= rowCount())
        return {};
    return data(index(row, 0), PhotoModel::PathRole).toUrl();
}

int PhotoFilterModel::idAt(int row) const
{
    if (row < 0 || row >= rowCount())
        return -1;
    return data(index(row, 0), PhotoModel::IdRole).toInt();
}

QVariantMap PhotoFilterModel::photoAt(int row) const
{
    if (row < 0 || row >= rowCount())
        return {};
    const QModelIndex idx = index(row, 0);
    return QVariantMap{
        {QStringLiteral("id"), data(idx, PhotoModel::IdRole).toInt()},
        {QStringLiteral("filePath"), data(idx, PhotoModel::FilePathRole).toString()},
        {QStringLiteral("fileName"), data(idx, PhotoModel::FileNameRole).toString()},
        {QStringLiteral("isVideo"), data(idx, PhotoModel::IsVideoRole).toBool()},
        {QStringLiteral("tagCount"), data(idx, PhotoModel::TagsRole).toStringList().size()},
        {QStringLiteral("favorite"), data(idx, PhotoModel::FavoriteRole).toBool()},
    };
}

QVariantList PhotoFilterModel::monthGroups() const
{
    QVariantList groups;
    const int n = rowCount();
    int i = 0;
    while (i < n) {
        const QString key = data(index(i, 0), PhotoModel::MonthKeyRole).toString();
        const qint64 taken = data(index(i, 0), PhotoModel::DateTakenRole).toLongLong();
        int j = i + 1;
        while (j < n && data(index(j, 0), PhotoModel::MonthKeyRole).toString() == key)
            ++j;

        QString label;
        if (taken <= 0 || key == QStringLiteral("0000-00")) {
            label = tr("Undated");
        } else {
            const QDate d = QDateTime::fromSecsSinceEpoch(taken).date();
            // Localized "Month Year", e.g. "October 2026".
            label = QLocale().toString(d, QStringLiteral("MMMM yyyy"));
        }

        groups.append(QVariantMap{
            {QStringLiteral("key"), key},
            {QStringLiteral("label"), label},
            {QStringLiteral("firstRow"), i},
            {QStringLiteral("count"), j - i},
        });
        i = j;
    }
    return groups;
}

bool PhotoFilterModel::containsId(int id) const
{
    auto *photos = qobject_cast<PhotoModel *>(sourceModel());
    if (!photos)
        return false;
    const int srcRow = photos->rowForId(id);
    if (srcRow < 0)
        return false; // gone from the active set entirely
    // Visible only if the source row maps through to a proxy row.
    return mapFromSource(photos->index(srcRow, 0)).isValid();
}

bool PhotoFilterModel::lessThan(const QModelIndex &left, const QModelIndex &right) const
{
    // The comparison itself lives in photosort (shared with the CLI); this only
    // gathers each row's keys. Only the keys the mode needs are read.
    photosort::Keys a, b;
    a.fileName = sourceModel()->data(left, PhotoModel::FileNameRole).toString();
    b.fileName = sourceModel()->data(right, PhotoModel::FileNameRole).toString();
    if (m_folderFirst) {
        a.path = sourceModel()->data(left, PhotoModel::FilePathRole).toString();
        b.path = sourceModel()->data(right, PhotoModel::FilePathRole).toString();
    }
    photosort::Mode mode = photosort::Mode::Name;
    switch (m_sortMode) {
    case SortByDate:
        mode = photosort::Mode::Date;
        a.takenAt = sourceModel()->data(left, PhotoModel::DateTakenRole).toLongLong();
        b.takenAt = sourceModel()->data(right, PhotoModel::DateTakenRole).toLongLong();
        break;
    case SortByTagCount:
        mode = photosort::Mode::TagCount;
        a.tagCount = sourceModel()->data(left, PhotoModel::TagCountRole).toInt();
        b.tagCount = sourceModel()->data(right, PhotoModel::TagCountRole).toInt();
        break;
    case SortByName:
        break;
    }
    return photosort::lessThan(a, b, mode, m_sortAscending, m_folderFirst, m_collator);
}

bool PhotoFilterModel::passesFolderGate(const QString &path) const
{
    if (!m_folderMode)
        return true;

    // While a search is active, folder view flattens to results: match every photo
    // at or under the current folder (recursively), rather than one level at a
    // time. At the root that is the whole library (not just orphans).
    const bool searching = !m_search.isEmpty();

    if (m_folderPath.isEmpty()) {
        if (searching)
            return true; // searching the whole library from the root
        // Root level: show orphans only — photos under no registered folder.
        for (const QString &root : m_folderRoots) {
            if (path == root || path.startsWith(root + QLatin1Char('/')))
                return false;
        }
        return true;
    }

    if (searching) // the current folder's whole subtree
        return path.startsWith(m_folderPath + QLatin1Char('/'));

    // Inside a folder: immediate children only (photos directly in m_folderPath,
    // not nested deeper — nested ones are reached by descending into subfolders).
    const QString dir = path.left(path.lastIndexOf(QLatin1Char('/')));
    return dir == m_folderPath;
}

bool PhotoFilterModel::filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const
{
    const QModelIndex idx = sourceModel()->index(sourceRow, 0, sourceParent);

    // Duplicate gate: hide non-representative copies in the ungrouped view only.
    if (m_dedupe && sourceModel()->data(idx, PhotoModel::IsDuplicateRole).toBool())
        return false;

    // Folder-view directory gate (no-op outside folder mode).
    if (m_folderMode) {
        const QString path = sourceModel()->data(idx, PhotoModel::FilePathRole).toString();
        if (!passesFolderGate(path))
            return false;
    }

    // Album-view gate (no-op outside album mode). Inside an album, require the
    // photo to match the album's membership query (resolved in applyView: a
    // regular album is one Album leaf, a smart album its expanded tree). At the
    // album-list level the grid is hidden, so show nothing unless a search is
    // active — then flatten to every photo that belongs to some album (mirroring
    // folder view's flattened search).
    if (m_albumMode) {
        const QVariantList raw = sourceModel()->data(idx, PhotoModel::TagIdsRole).toList();
        if (m_albumId > 0) {
            QSet<int> tags;
            tags.reserve(raw.size());
            for (const QVariant &v : raw)
                tags.insert(v.toInt());
            if (!oql::matches(m_albumQuery, tags, m_ctx))
                return false;
        } else {
            if (m_search.isEmpty())
                return false; // album-list level shows tiles, not a photo grid
            bool inAny = false;
            for (const QVariant &v : raw)
                if (m_albumTagIds.contains(v.toInt())) { inAny = true; break; }
            if (!inAny)
                return false;
        }
    }

    // Tag filter: the sidebar selection as an AND/OR tree (see setSelectedTagIds),
    // and the free query from the builder when one is applied. Both must hold.
    if (!m_selected.isEmpty() || m_advancedActive) {
        const QVariantList raw = sourceModel()->data(idx, PhotoModel::TagIdsRole).toList();
        QSet<int> tags;
        tags.reserve(raw.size());
        for (const QVariant &v : raw)
            tags.insert(v.toInt());
        if (!m_selected.isEmpty() && !oql::matches(m_selectedQuery, tags, m_ctx))
            return false;
        if (m_advancedActive && !oql::matches(m_advanced, tags, m_ctx))
            return false;
    }

    // Text search: match the file name or any tag name. The model pre-folds both
    // into one blob (accent-/case-folded, so "cafe" finds "Café"), so this is a
    // plain substring test — no per-keystroke folding across the whole model.
    if (!m_searchFolded.isEmpty()) {
        const QString blob = sourceModel()->data(idx, PhotoModel::SearchFoldRole).toString();
        return blob.contains(m_searchFolded);
    }

    return true;
}
