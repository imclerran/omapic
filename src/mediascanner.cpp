// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mediascanner.h"

#include <QFileInfo>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>

#include "mediascan.h"

MediaScanner::MediaScanner(QObject *parent)
    : QObject(parent)
{
}

QList<QUrl> MediaScanner::toUrls(const QStringList &paths) const
{
    QList<QUrl> urls;
    urls.reserve(paths.size());
    for (const QString &p : paths) {
        const QFileInfo fi(p);
        if (fi.exists() && fi.isFile() && mediascan::isMediaFile(fi.absoluteFilePath()))
            urls.append(QUrl::fromLocalFile(fi.absoluteFilePath()));
    }
    return urls;
}

void MediaScanner::expandDirectoryTree(const QUrl &fileOrDir)
{
    const QString start = fileOrDir.toLocalFile();
    const QFileInfo fi(start);
    const QString dir = fi.isDir() ? fi.absoluteFilePath() : fi.absolutePath();

    auto *watcher = new QFutureWatcher<QStringList>(this);
    connect(watcher, &QFutureWatcher<QStringList>::finished, this, [this, watcher]() {
        const QStringList files = watcher->result();
        QList<QUrl> urls;
        urls.reserve(files.size());
        for (const QString &f : files)
            urls.append(QUrl::fromLocalFile(f));
        emit expanded(urls);
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([dir]() { return mediascan::collectSorted(dir); }));
}
