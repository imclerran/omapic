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

PhotoFilterModel::PhotoFilterModel(QObject *parent)
    : QSortFilterProxyModel(parent)
{
    m_collator.setNumericMode(true);
    m_collator.setCaseSensitivity(Qt::CaseInsensitive);

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
    invalidateRowFilter();
    emit selectedTagIdsChanged();
    emit countChanged();
}

void PhotoFilterModel::setMatchAll(bool on)
{
    if (m_matchAll == on)
        return;
    m_matchAll = on;
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
    invalidateRowFilter();
    emit searchTextChanged();
    emit countChanged();
}

void PhotoFilterModel::setTagPresence(TagPresence mode)
{
    if (m_tagPresence == mode)
        return;
    m_tagPresence = mode;
    invalidateRowFilter();
    emit tagPresenceChanged();
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

void PhotoFilterModel::setSortByDate(bool on)
{
    if (m_sortByDate == on)
        return;
    m_sortByDate = on;
    invalidate(); // ordering only
    emit sortByDateChanged();
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

void PhotoFilterModel::applyView(bool dedupe, bool sortByDate, bool folderMode,
                                 const QString &folderPath)
{
    bool changed = false;
    if (m_dedupe != dedupe) { m_dedupe = dedupe; emit dedupeChanged(); changed = true; }
    if (m_sortByDate != sortByDate) { m_sortByDate = sortByDate; emit sortByDateChanged(); changed = true; }
    if (m_folderMode != folderMode) { m_folderMode = folderMode; emit folderModeChanged(); changed = true; }
    if (m_folderPath != folderPath) { m_folderPath = folderPath; emit folderPathChanged(); changed = true; }
    if (!changed)
        return;
    // One re-filter + re-sort against the final mode, rather than one per setter.
    invalidate();
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
    if (m_sortByDate) {
        // Timeline: newest first. Fall back to the name (ascending) within the
        // same instant so the order is stable and photos without a date (0) sort
        // together at the end.
        const qint64 lt = sourceModel()->data(left, PhotoModel::DateTakenRole).toLongLong();
        const qint64 rt = sourceModel()->data(right, PhotoModel::DateTakenRole).toLongLong();
        if (lt != rt)
            return lt > rt;
        const QString ln = sourceModel()->data(left, PhotoModel::FileNameRole).toString();
        const QString rn = sourceModel()->data(right, PhotoModel::FileNameRole).toString();
        return m_collator.compare(ln, rn) < 0;
    }

    if (m_folderFirst) {
        // Compare the containing folder first, so files group by folder; within a
        // folder (or when the paths lack a separator) fall through to the name.
        const QString lp = sourceModel()->data(left, PhotoModel::FilePathRole).toString();
        const QString rp = sourceModel()->data(right, PhotoModel::FilePathRole).toString();
        const QString ld = lp.left(lp.lastIndexOf(QLatin1Char('/')));
        const QString rd = rp.left(rp.lastIndexOf(QLatin1Char('/')));
        const int dirCmp = m_collator.compare(ld, rd);
        if (dirCmp != 0)
            return dirCmp < 0;
    }

    const QString ln = sourceModel()->data(left, PhotoModel::FileNameRole).toString();
    const QString rn = sourceModel()->data(right, PhotoModel::FileNameRole).toString();
    return m_collator.compare(ln, rn) < 0;
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

    // Tag filter.
    if (!m_selected.isEmpty()) {
        const QVariantList raw = sourceModel()->data(idx, PhotoModel::TagIdsRole).toList();
        QSet<int> tags;
        tags.reserve(raw.size());
        for (const QVariant &v : raw)
            tags.insert(v.toInt());

        if (m_matchAll) {
            for (int t : m_selected)
                if (!tags.contains(t))
                    return false;
        } else {
            bool any = false;
            for (int t : m_selected) {
                if (tags.contains(t)) {
                    any = true;
                    break;
                }
            }
            if (!any)
                return false;
        }
    }

    // Tagged / untagged restriction.
    if (m_tagPresence != AnyTags) {
        const bool hasTags = !sourceModel()->data(idx, PhotoModel::TagIdsRole).toList().isEmpty();
        if (m_tagPresence == TaggedOnly && !hasTags)
            return false;
        if (m_tagPresence == UntaggedOnly && hasTags)
            return false;
    }

    // Text search: match the file name or any tag name.
    if (!m_search.isEmpty()) {
        const QString name = sourceModel()->data(idx, PhotoModel::FileNameRole).toString();
        if (name.contains(m_search, Qt::CaseInsensitive))
            return true;
        const QStringList tagNames = sourceModel()->data(idx, PhotoModel::TagsRole).toStringList();
        for (const QString &tn : tagNames)
            if (tn.contains(m_search, Qt::CaseInsensitive))
                return true;
        return false;
    }

    return true;
}
