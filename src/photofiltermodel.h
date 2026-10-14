// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QCollator>
#include <QQmlEngine>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

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
    // Timeline view: sort by capture time (newest first) instead of by name.
    Q_PROPERTY(bool sortByDate READ sortByDate WRITE setSortByDate NOTIFY sortByDateChanged)
    // Folder view: when on, restrict to one directory level (see folderPath).
    Q_PROPERTY(bool folderMode READ folderMode WRITE setFolderMode NOTIFY folderModeChanged)
    // The directory currently shown in folder view. Empty = the root level,
    // which shows orphan photos (those under no registered folder).
    Q_PROPERTY(QString folderPath READ folderPath WRITE setFolderPath NOTIFY folderPathChanged)
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

    bool sortByDate() const { return m_sortByDate; }
    void setSortByDate(bool on); // timeline: order by capture time, newest first

    bool folderMode() const { return m_folderMode; }
    void setFolderMode(bool on);

    QString folderPath() const { return m_folderPath; }
    void setFolderPath(const QString &path);

    // The registered folder roots, used to tell orphan photos (shown at the
    // folder-view root) from foldered ones. Set by Library; not a QML property.
    void setFolderRoots(const QStringList &roots);

    int count() const { return rowCount(); }

    // Map a proxy row to its image URL (used by the slideshow).
    Q_INVOKABLE QUrl sourceAt(int row) const;
    // Map a proxy row to its photo id (used for range selection).
    Q_INVOKABLE int idAt(int row) const;
    // The display data a timeline tile needs for one proxy row:
    // {id, filePath, fileName, isVideo, tagCount}. Empty map for a bad row.
    Q_INVOKABLE QVariantMap photoAt(int row) const;
    // The timeline's month sections over the current (date-sorted) order, newest
    // first: a list of {key, label, firstRow, count}. `label` is a localized
    // "Month Year" (or "Undated" for photos with no capture time).
    Q_INVOKABLE QVariantList monthGroups() const;
    // Whether the photo with this id currently passes the filter (is visible in
    // the gallery). Used to prune the selection when the filter changes.
    Q_INVOKABLE bool containsId(int id) const;

signals:
    void selectedTagIdsChanged();
    void matchAllChanged();
    void searchTextChanged();
    void tagPresenceChanged();
    void folderFirstChanged();
    void sortByDateChanged();
    void folderModeChanged();
    void folderPathChanged();
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

    // True when the photo passes the folder-view directory gate (always true when
    // folderMode is off). Factored out of filterAcceptsRow for clarity.
    bool passesFolderGate(const QString &path) const;

    QSet<int> m_selected;
    bool m_matchAll = true;
    QString m_search;
    TagPresence m_tagPresence = AnyTags;
    bool m_folderFirst = false;
    bool m_sortByDate = false;
    bool m_folderMode = false;
    QString m_folderPath;
    QStringList m_folderRoots;
    QCollator m_collator;
};
