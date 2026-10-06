#include "tagmodel.h"

TagModel::TagModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int TagModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_tags.size();
}

QVariant TagModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_tags.size())
        return {};

    const TagInfo &t = m_tags.at(index.row());
    switch (role) {
    case IdRole:
        return t.id;
    case NameRole:
        return t.name;
    case CountRole:
        return t.count;
    case SelectedRole:
        return m_selected.contains(t.id);
    default:
        return {};
    }
}

QHash<int, QByteArray> TagModel::roleNames() const
{
    return {
        {IdRole, "id"},
        {NameRole, "name"},
        {CountRole, "count"},
        {SelectedRole, "selected"},
    };
}

void TagModel::setTags(const QVector<TagInfo> &tags)
{
    beginResetModel();
    m_tags = tags;

    QSet<int> existing;
    existing.reserve(tags.size());
    for (const TagInfo &t : tags)
        existing.insert(t.id);

    const QSet<int> next = m_selected & existing;
    const bool selectionDropped = (next != m_selected);
    m_selected = next;
    endResetModel();

    if (selectionDropped)
        emit selectionChanged();
}

QStringList TagModel::names() const
{
    QStringList out;
    out.reserve(m_tags.size());
    for (const TagInfo &t : m_tags)
        out.append(t.name);
    return out;
}

QList<int> TagModel::selectedTagIds() const
{
    return QList<int>(m_selected.begin(), m_selected.end());
}

void TagModel::toggle(int row)
{
    if (row < 0 || row >= m_tags.size())
        return;
    const int id = m_tags.at(row).id;
    if (m_selected.contains(id))
        m_selected.remove(id);
    else
        m_selected.insert(id);

    const QModelIndex idx = index(row, 0);
    emit dataChanged(idx, idx, {SelectedRole});
    emit selectionChanged();
}

void TagModel::clearSelection()
{
    if (m_selected.isEmpty())
        return;
    m_selected.clear();
    emit dataChanged(index(0, 0), index(m_tags.size() - 1, 0), {SelectedRole});
    emit selectionChanged();
}
