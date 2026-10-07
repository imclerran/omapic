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
    connect(this, &QAbstractItemModel::rowsInserted, this, &PhotoFilterModel::countChanged);
    connect(this, &QAbstractItemModel::rowsRemoved, this, &PhotoFilterModel::countChanged);
    connect(this, &QAbstractItemModel::modelReset, this, &PhotoFilterModel::countChanged);
    connect(this, &QAbstractItemModel::layoutChanged, this, &PhotoFilterModel::countChanged);
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
    invalidateFilter();
    emit selectedTagIdsChanged();
    emit countChanged();
}

void PhotoFilterModel::setMatchAll(bool on)
{
    if (m_matchAll == on)
        return;
    m_matchAll = on;
    invalidateFilter();
    emit matchAllChanged();
    emit countChanged();
}

void PhotoFilterModel::setSearchText(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (m_search == trimmed)
        return;
    m_search = trimmed;
    invalidateFilter();
    emit searchTextChanged();
    emit countChanged();
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
