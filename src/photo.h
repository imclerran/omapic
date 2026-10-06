#pragma once

#include <QSet>
#include <QString>
#include <QStringList>

struct Photo {
    int id = -1;
    QString path;
    QString fileName;
    QSet<int> tagIds;
    QStringList tagNames;
};

struct TagInfo {
    int id = -1;
    QString name;
    int count = 0;
};

struct FolderInfo {
    int id = -1;
    QString path;
    int count = 0;        // photos currently in the library under this folder
    bool enabled = true;  // whether its photos are shown in the gallery
};

// Minimal row view used by the rescan reconciliation (id, path, content hash).
struct PhotoRef {
    int id = -1;
    QString path;
    QString hash;
};
