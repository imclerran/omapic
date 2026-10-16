// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mediascan.h"

#include <QDirIterator>
#include <QFileInfo>
#include <QHash>

#include <algorithm>
#include <functional>

namespace mediascan {

const QStringList &globs()
{
    static const QStringList kMediaGlobs = {
        QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"), QStringLiteral("*.png"),
        QStringLiteral("*.gif"), QStringLiteral("*.bmp"), QStringLiteral("*.webp"),
        QStringLiteral("*.tif"), QStringLiteral("*.tiff"),
        // HEIC/HEIF and AVIF decode through Qt's image plugins, which need extra
        // plugins installed (on Arch: kimageformats, with libheif for HEIC/HEIF
        // and libavif for AVIF); see the missing-plugin handling in main.cpp and
        // the thumbnail provider.
        QStringLiteral("*.heic"), QStringLiteral("*.heif"), QStringLiteral("*.avif"),
        QStringLiteral("*.mp4"), QStringLiteral("*.m4v"), QStringLiteral("*.webm"),
        QStringLiteral("*.mkv"), QStringLiteral("*.mov"), QStringLiteral("*.avi"),
    };
    return kMediaGlobs;
}

bool isMediaFile(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix.isEmpty())
        return false;
    const QString dotGlob = QStringLiteral("*.") + suffix;
    return globs().contains(dotGlob);
}

void collect(const QString &root, QSet<QString> &out)
{
    if (root.isEmpty())
        return;

    // A directory is excluded when it (or any ancestor up to `root`) holds a
    // .nomedia file. Cache each directory's verdict so the parent walk is cheap.
    QHash<QString, bool> excluded;
    const std::function<bool(const QString &)> isExcluded = [&](const QString &dirPath) -> bool {
        auto cached = excluded.constFind(dirPath);
        if (cached != excluded.constEnd())
            return cached.value();
        bool ex = QFileInfo::exists(dirPath + QStringLiteral("/.nomedia"));
        if (!ex && QFileInfo(dirPath).canonicalFilePath() != QFileInfo(root).canonicalFilePath()) {
            const QString parent = QFileInfo(dirPath).path();
            if (!parent.isEmpty() && parent != dirPath)
                ex = isExcluded(parent);
        }
        excluded.insert(dirPath, ex);
        return ex;
    };

    QDirIterator it(root, globs(), QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString file = it.next();
        if (!isExcluded(QFileInfo(file).path()))
            out.insert(file);
    }
}

QStringList collectSorted(const QString &root)
{
    QSet<QString> set;
    collect(root, set);
    QStringList list(set.begin(), set.end());
    std::sort(list.begin(), list.end(), [](const QString &a, const QString &b) {
        return a.compare(b, Qt::CaseInsensitive) < 0;
    });
    return list;
}

} // namespace mediascan
