// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>

// Basic, display-ready metadata for the details pane: image dimensions, file
// size, and a few EXIF fields. Every field is a formatted string, left empty
// when unavailable (missing tag, a video, an unreadable file), so the pane can
// simply hide the empty rows. Reads straight from the file on disk, so call it
// off the UI hot path (it's used only for the single-selection details).
struct PhotoMetadata {
    QString dimensions; // e.g. "4032 × 3024"
    QString size;       // e.g. "3.8 MB"
    QString taken;      // EXIF date taken, e.g. "2026-05-12 14:22:07"
    QString camera;     // EXIF make + model, e.g. "FUJIFILM X-T30"
    QString modified;   // file mtime, e.g. "2026-05-12 14:22"
};

PhotoMetadata readPhotoMetadata(const QString &path, bool isVideo);

// The photo's capture time in Unix seconds, for grouping in the timeline view:
// EXIF DateTimeOriginal, then the generic DateTime tag, then the file's mtime.
// Videos and EXIF-less images fall back to mtime. Returns 0 only when the file
// can't be stat'd at all. Reads straight from disk; call it off the UI thread.
qint64 capturedAtEpoch(const QString &path, bool isVideo);
