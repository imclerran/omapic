// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#include "photomodel.h"

#include <QDateTime>
#include <QUrl>

#include "textfold.h"

PhotoModel::PhotoModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

// Fold the file name and every tag name — plus the aliases of those tags — into
// one blob so the gallery search can substring-match it directly, instead of
// re-folding each field per keystroke. Folding the aliases in means a search for
// an alias finds photos carrying its canonical tag. Pieces are joined by '\n'
// (never in a search term) so a match can't straddle a field boundary.
QString PhotoModel::buildSearchFold(const Photo &p) const
{
    QString blob = foldText(p.fileName);
    for (const QString &tag : p.tagNames) {
        blob += QLatin1Char('\n');
        blob += foldText(tag);
    }
    if (!m_aliasesByTagId.isEmpty()) {
        for (int tagId : p.tagIds) {
            const auto it = m_aliasesByTagId.constFind(tagId);
            if (it == m_aliasesByTagId.cend())
                continue;
            for (const QString &a : it.value()) {
                blob += QLatin1Char('\n');
                blob += foldText(a);
            }
        }
    }
    return blob;
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
    case TagCountRole:
        return p.tagNames.size();
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
        {TagCountRole, "tagCount"},
    };
}

namespace {
// Beyond this many separate removed/inserted runs, a reload is a different set,
// not an edit: one reset is cheaper than that many proxy re-mappings, and a view
// has nothing to hold its place against anyway.
constexpr int kMaxSplices = 64;
}

void PhotoModel::setPhotos(QVector<Photo> photos)
{
    // Reloads after a worker job (launch migration/rescan, sidecar flush, …) often
    // re-fetch an identical set. Equality ignores the derived search blob, so a
    // re-fetch compares equal and nothing is signalled.
    if (photos == m_photos)
        return;

    // Plan the splice. Survivors are the old rows whose id is still present;
    // they must appear in the same relative order in the new set, which holds
    // whenever the load order (added_at, id) is stable across the reload.
    struct Run { int first; int count; };
    QVector<Run> removals; // in old-row terms, ascending
    QVector<Run> inserts;  // in new-row terms, ascending
    bool splice = !m_photos.isEmpty() && !photos.isEmpty();
    if (splice) {
        QSet<int> newIds;
        newIds.reserve(photos.size());
        for (const Photo &p : photos)
            newIds.insert(p.id);
        QVector<int> survivors; // old rows kept, in old order
        for (int i = 0; i < m_photos.size(); ++i) {
            if (newIds.contains(m_photos.at(i).id)) {
                survivors.append(i);
            } else if (!removals.isEmpty()
                       && removals.last().first + removals.last().count == i) {
                ++removals.last().count;
            } else {
                removals.append({i, 1});
            }
        }
        int k = 0; // next survivor to match
        for (int j = 0; j < photos.size() && splice; ++j) {
            if (k < survivors.size() && m_photos.at(survivors.at(k)).id == photos.at(j).id) {
                ++k;
            } else if (m_idToRow.contains(photos.at(j).id)) {
                splice = false; // a survivor out of order: the load order changed
            } else if (!inserts.isEmpty() && inserts.last().first + inserts.last().count == j) {
                ++inserts.last().count;
            } else {
                inserts.append({j, 1});
            }
        }
        if (k != survivors.size() || removals.size() + inserts.size() > kMaxSplices)
            splice = false;
    }

    if (!splice) {
        beginResetModel();
        m_photos = std::move(photos);
        m_idToRow.clear();
        for (int i = 0; i < m_photos.size(); ++i) {
            m_idToRow.insert(m_photos.at(i).id, i);
            m_photos[i].searchFold = buildSearchFold(m_photos.at(i));
        }
        endResetModel();
        return;
    }

    // Removals from the end, so earlier runs keep their row numbers.
    for (int r = removals.size() - 1; r >= 0; --r) {
        const Run run = removals.at(r);
        beginRemoveRows({}, run.first, run.first + run.count - 1);
        m_photos.remove(run.first, run.count);
        endRemoveRows();
    }
    // Then a forward walk over the new set: insert each new run where it
    // belongs, and refresh a survivor in place when its fields changed (tags,
    // favorite, duplicate flag, capture time, path …).
    int ins = 0;
    for (int j = 0; j < photos.size(); ++j) {
        if (ins < inserts.size() && inserts.at(ins).first == j) {
            const Run run = inserts.at(ins++);
            beginInsertRows({}, j, j + run.count - 1);
            for (int n = 0; n < run.count; ++n) {
                m_photos.insert(j + n, photos.at(j + n));
                m_photos[j + n].searchFold = buildSearchFold(m_photos.at(j + n));
            }
            endInsertRows();
            j += run.count - 1;
            continue;
        }
        Photo &old = m_photos[j];
        if (old != photos.at(j)) {
            old = photos.at(j);
            old.searchFold = buildSearchFold(old);
            const QModelIndex idx = index(j, 0);
            emit dataChanged(idx, idx);
        }
    }
    m_idToRow.clear();
    for (int i = 0; i < m_photos.size(); ++i)
        m_idToRow.insert(m_photos.at(i).id, i);
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

void PhotoModel::setTagAliases(const QHash<int, QStringList> &aliasesByTagId)
{
    if (m_aliasesByTagId == aliasesByTagId)
        return;
    m_aliasesByTagId = aliasesByTagId;
    if (m_photos.isEmpty())
        return;
    for (int i = 0; i < m_photos.size(); ++i)
        m_photos[i].searchFold = buildSearchFold(m_photos.at(i));
    emit dataChanged(index(0, 0), index(m_photos.size() - 1, 0), {SearchFoldRole});
}
