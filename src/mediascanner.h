// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QList>
#include <QObject>
#include <QQmlEngine>
#include <QStringList>
#include <QUrl>

// QML-facing media enumeration for the standalone viewer. Library-free: it only
// touches the filesystem, so the viewer can run without opening the database.
class MediaScanner : public QObject
{
    Q_OBJECT
    QML_ELEMENT

public:
    explicit MediaScanner(QObject *parent = nullptr);

    // Turn launch paths into file:// URLs, in order, keeping only existing media
    // files (so a stray argument can't crash the viewer).
    Q_INVOKABLE QList<QUrl> toUrls(const QStringList &paths) const;

    // Recursively enumerate every media file in the directory containing
    // `fileOrDir` (or the directory itself), .nomedia-aware and filename-sorted,
    // off the UI thread. Emits expanded() when done. Used for the "slideshow the
    // whole folder" action when the viewer was opened on a single path.
    Q_INVOKABLE void expandDirectoryTree(const QUrl &fileOrDir);

signals:
    void expanded(const QList<QUrl> &urls);
};
