#pragma once

#include <QAbstractListModel>
#include <QQmlEngine>
#include <QSet>
#include <QVector>

#include "photo.h"

class TagModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by Library")
    Q_PROPERTY(QList<int> selectedTagIds READ selectedTagIds NOTIFY selectionChanged)

public:
    enum Roles {
        IdRole = Qt::UserRole + 1,
        NameRole,
        CountRole,
        SelectedRole,
    };

    explicit TagModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setTags(const QVector<TagInfo> &tags);
    QStringList names() const;

    QList<int> selectedTagIds() const;
    Q_INVOKABLE void toggle(int row);
    Q_INVOKABLE void clearSelection();

signals:
    void selectionChanged();

private:
    QVector<TagInfo> m_tags;
    QSet<int> m_selected;
};
