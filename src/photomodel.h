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
        TagsRole,
        TagIdsRole,
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
    void setTagsForId(int photoId, const QSet<int> &ids, const QStringList &names);

private:
    QVector<Photo> m_photos;
    QHash<int, int> m_idToRow;
};
