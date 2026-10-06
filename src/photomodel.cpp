// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#include "photomodel.h"

#include <QUrl>

PhotoModel::PhotoModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int PhotoModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_photos.size();
}

QVariant PhotoModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_photos.size())
        return {};

    const Photo &p = m_photos.at(index.row());
    switch (role) {
    case IdRole:
        return p.id;
    case PathRole:
        return QUrl::fromLocalFile(p.path);
    case FilePathRole:
        return p.path;
    case FileNameRole:
        return p.fileName;
    case TagsRole:
        return p.tagNames;
    case TagIdsRole: {
        QVariantList ids;
        ids.reserve(p.tagIds.size());
        for (int id : p.tagIds)
            ids.append(id);
        return ids;
    }
    default:
        return {};
    }
}

QHash<int, QByteArray> PhotoModel::roleNames() const
{
    return {
        {IdRole, "id"},
        {PathRole, "path"},
        {FilePathRole, "filePath"},
        {FileNameRole, "fileName"},
        {TagsRole, "tags"},
        {TagIdsRole, "tagIds"},
    };
}

void PhotoModel::setPhotos(QVector<Photo> photos)
{
    beginResetModel();
    m_photos = std::move(photos);
    m_idToRow.clear();
    for (int i = 0; i < m_photos.size(); ++i)
        m_idToRow.insert(m_photos.at(i).id, i);
    endResetModel();
}

void PhotoModel::appendPhoto(const Photo &photo)
{
    const int row = m_photos.size();
    beginInsertRows({}, row, row);
    m_photos.append(photo);
    m_idToRow.insert(photo.id, row);
    endInsertRows();
}

int PhotoModel::rowForId(int id) const
{
    return m_idToRow.value(id, -1);
}

Photo PhotoModel::photoById(int id) const
{
    const int row = rowForId(id);
    if (row < 0)
        return {};
    return m_photos.at(row);
}

void PhotoModel::setTagsForId(int photoId, const QSet<int> &ids, const QStringList &names)
{
    const int row = rowForId(photoId);
    if (row < 0)
        return;
    Photo &p = m_photos[row];
    p.tagIds = ids;
    p.tagNames = names;
    const QModelIndex idx = index(row, 0);
    emit dataChanged(idx, idx, {TagsRole, TagIdsRole});
}
