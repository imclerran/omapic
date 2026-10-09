// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QFileInfo>
#include <QSet>
#include <QString>
#include <QStringList>

// True when `path` names a video file we treat as playable. Keep this list in
// sync with the scanner globs (libraryworker.cpp) and SlideshowView.qml.
inline bool isVideoPath(const QString &path)
{
    static const QSet<QString> kVideoSuffixes = {
        QStringLiteral("mp4"), QStringLiteral("m4v"), QStringLiteral("webm"),
        QStringLiteral("mkv"), QStringLiteral("mov"), QStringLiteral("avi"),
    };
    return kVideoSuffixes.contains(QFileInfo(path).suffix().toLower());
}

struct Photo {
    int id = -1;
    QString path;
    QString fileName;
    bool isVideo = false;
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
