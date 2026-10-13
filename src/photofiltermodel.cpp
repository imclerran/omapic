// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#include "photofiltermodel.h"

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

bool PhotoFilterModel::filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const
{
    const QModelIndex idx = sourceModel()->index(sourceRow, 0, sourceParent);

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
