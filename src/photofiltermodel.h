// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QCollator>
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
    Q_PROPERTY(QString searchText READ searchText WRITE setSearchText NOTIFY searchTextChanged)
    Q_PROPERTY(TagPresence tagPresence READ tagPresence WRITE setTagPresence NOTIFY tagPresenceChanged)
    Q_PROPERTY(bool folderFirst READ folderFirst WRITE setFolderFirst NOTIFY folderFirstChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    // Whether to additionally restrict to tagged or untagged photos.
    enum TagPresence { AnyTags, TaggedOnly, UntaggedOnly };
    Q_ENUM(TagPresence)

    explicit PhotoFilterModel(QObject *parent = nullptr);

    QList<int> selectedTagIds() const;
    void setSelectedTagIds(const QList<int> &ids);

    bool matchAll() const { return m_matchAll; }
    void setMatchAll(bool on);

    QString searchText() const { return m_search; }
    void setSearchText(const QString &text); // matches file name or any tag name

    TagPresence tagPresence() const { return m_tagPresence; }
    void setTagPresence(TagPresence mode);

    bool folderFirst() const { return m_folderFirst; }
    void setFolderFirst(bool on); // group by containing folder before file name

    int count() const { return rowCount(); }

    // Map a proxy row to its image URL (used by the slideshow).
    Q_INVOKABLE QUrl sourceAt(int row) const;
    // Map a proxy row to its photo id (used for range selection).
    Q_INVOKABLE int idAt(int row) const;
    // Whether the photo with this id currently passes the filter (is visible in
    // the gallery). Used to prune the selection when the filter changes.
    Q_INVOKABLE bool containsId(int id) const;

signals:
    void selectedTagIdsChanged();
    void matchAllChanged();
    void searchTextChanged();
    void tagPresenceChanged();
    void folderFirstChanged();
    void countChanged();

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;
    // Natural, case-insensitive ordering of file names: runs of digits compare by
    // value (3 < 22 < 111), so numbered files sort as a person would expect, and
    // the rest of the name compares alphabetically. When folderFirst is on, photos
    // are grouped by containing folder (compared the same way) before file name.
    bool lessThan(const QModelIndex &left, const QModelIndex &right) const override;

private:
    // Re-run the row filter. Wraps the begin/endFilterChange API on Qt that has it
    // (invalidateFilter() is deprecated there), falling back for older Qt.
    void invalidateRowFilter();

    QSet<int> m_selected;
    bool m_matchAll = true;
    QString m_search;
    TagPresence m_tagPresence = AnyTags;
    bool m_folderFirst = false;
    QCollator m_collator;
};
