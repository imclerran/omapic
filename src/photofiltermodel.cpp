#include "photofiltermodel.h"

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
    if (m_selected.isEmpty())
        return true;

    const QModelIndex idx = sourceModel()->index(sourceRow, 0, sourceParent);
    const QVariantList raw = sourceModel()->data(idx, PhotoModel::TagIdsRole).toList();

    QSet<int> tags;
    tags.reserve(raw.size());
    for (const QVariant &v : raw)
        tags.insert(v.toInt());

    if (m_matchAll) {
        for (int t : m_selected)
            if (!tags.contains(t))
                return false;
        return true;
    }

    for (int t : m_selected)
        if (tags.contains(t))
            return true;
    return false;
}
