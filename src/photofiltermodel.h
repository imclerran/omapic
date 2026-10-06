// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QQmlEngine>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QUrl>

// Filters the photo list down to those matching the selected tag set.
// matchAll == true  -> photo must carry every selected tag (intersection / narrow)
// matchAll == false -> photo must carry at least one selected tag (union)
class PhotoFilterModel : public QSortFilterProxyModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by Library")
    Q_PROPERTY(QList<int> selectedTagIds READ selectedTagIds WRITE setSelectedTagIds NOTIFY selectedTagIdsChanged)
    Q_PROPERTY(bool matchAll READ matchAll WRITE setMatchAll NOTIFY matchAllChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    explicit PhotoFilterModel(QObject *parent = nullptr);

    QList<int> selectedTagIds() const;
    void setSelectedTagIds(const QList<int> &ids);

    bool matchAll() const { return m_matchAll; }
    void setMatchAll(bool on);

    int count() const { return rowCount(); }

    // Map a proxy row to its image URL (used by the slideshow).
    Q_INVOKABLE QUrl sourceAt(int row) const;
    // Map a proxy row to its photo id (used for range selection).
    Q_INVOKABLE int idAt(int row) const;

signals:
    void selectedTagIdsChanged();
    void matchAllChanged();
    void countChanged();

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;

private:
    QSet<int> m_selected;
    bool m_matchAll = true;
};
