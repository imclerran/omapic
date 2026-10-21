// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QQmlEngine>
#include <QVector>

#include "photo.h"

class PhotoModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by Library")

public:
    enum Roles {
        IdRole = Qt::UserRole + 1,
        PathRole,
        FilePathRole,
        FileNameRole,
        IsVideoRole,
        TagsRole,
        TagIdsRole,
        DateTakenRole, // capture time in Unix seconds (0 = unknown)
        MonthKeyRole,  // "yyyy-MM" of the capture time, the timeline section key
        IsDuplicateRole, // true for a non-representative duplicate (see Photo)
        FavoriteRole,  // true when the photo is in the hidden Favorites album
        SearchFoldRole, // pre-folded name+tags blob for the gallery search filter
        TagCountRole,   // number of user-visible tags, for the tag-count sort
    };

    explicit PhotoModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setPhotos(QVector<Photo> photos);
    void appendPhoto(const Photo &photo);
    int rowForId(int id) const;
    Q_INVOKABLE bool contains(int id) const { return rowForId(id) >= 0; }
    Photo photoById(int id) const;
    const QVector<Photo> &photos() const { return m_photos; }
    void setTagsForId(int photoId, const QSet<int> &ids, const QStringList &names, bool favorite);
    // Alias names per canonical tag id, folded into each photo's search blob so a
    // gallery search for an alias finds photos carrying its canonical tag. Rebuilds
    // the blobs when it changes.
    void setTagAliases(const QHash<int, QStringList> &aliasesByTagId);

private:
    QString buildSearchFold(const Photo &p) const;

    QVector<Photo> m_photos;
    QHash<int, int> m_idToRow;
    QHash<int, QStringList> m_aliasesByTagId; // canonical tag id -> its alias names
};
