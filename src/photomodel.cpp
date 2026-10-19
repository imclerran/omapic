// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#include "photomodel.h"

#include <QDateTime>
#include <QUrl>

#include "textfold.h"

namespace {
// Fold the file name and every tag name into one blob so the gallery search can
// substring-match it directly, instead of re-folding each field per keystroke.
// Pieces are joined by '\n' (never in a search term) so a match can't straddle
// the file-name/tag boundary.
QString buildSearchFold(const Photo &p)
{
    QString blob = foldText(p.fileName);
    for (const QString &tag : p.tagNames) {
        blob += QLatin1Char('\n');
        blob += foldText(tag);
    }
    return blob;
}
}

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
    case IsVideoRole:
        return p.isVideo;
    case TagsRole:
        return p.tagNames;
    case TagIdsRole: {
        QVariantList ids;
        ids.reserve(p.tagIds.size());
        for (int id : p.tagIds)
            ids.append(id);
        return ids;
    }
    case DateTakenRole:
        return static_cast<qlonglong>(p.takenAt);
    case MonthKeyRole: {
        if (p.takenAt <= 0)
            return QStringLiteral("0000-00"); // unknown dates group together, last
        const QDateTime dt = QDateTime::fromSecsSinceEpoch(p.takenAt);
        return dt.toString(QStringLiteral("yyyy-MM"));
    }
    case IsDuplicateRole:
        return p.isDuplicate;
    case FavoriteRole:
        return p.favorite;
    case SearchFoldRole:
        return p.searchFold;
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
        {IsVideoRole, "isVideo"},
        {TagsRole, "tags"},
        {TagIdsRole, "tagIds"},
        {DateTakenRole, "takenAt"},
        {MonthKeyRole, "monthKey"},
        {IsDuplicateRole, "isDuplicate"},
        {FavoriteRole, "favorite"},
    };
}

void PhotoModel::setPhotos(QVector<Photo> photos)
{
    // Reloads after a worker job (launch migration/rescan, sidecar flush, …) often
    // re-fetch an identical set. A reset would still tear down and recreate every
    // delegate — blanking all the thumbnails — so skip it when nothing changed.
    if (photos == m_photos)
        return;
    beginResetModel();
    m_photos = std::move(photos);
    m_idToRow.clear();
    for (int i = 0; i < m_photos.size(); ++i) {
        m_idToRow.insert(m_photos.at(i).id, i);
        m_photos[i].searchFold = buildSearchFold(m_photos.at(i));
    }
    endResetModel();
}

void PhotoModel::appendPhoto(const Photo &photo)
{
    const int row = m_photos.size();
    beginInsertRows({}, row, row);
    m_photos.append(photo);
    m_photos[row].searchFold = buildSearchFold(m_photos.at(row));
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

void PhotoModel::setTagsForId(int photoId, const QSet<int> &ids, const QStringList &names, bool favorite)
{
    const int row = rowForId(photoId);
    if (row < 0)
        return;
    Photo &p = m_photos[row];
    p.tagIds = ids;
    p.tagNames = names;
    p.favorite = favorite;
    p.searchFold = buildSearchFold(p); // tag names changed; refresh the search blob
    const QModelIndex idx = index(row, 0);
    emit dataChanged(idx, idx, {TagsRole, TagIdsRole, FavoriteRole, SearchFoldRole});
}
