// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

// Include exiv2 before the Qt headers: exiv2 pulls in a large tree, and keeping
// it first avoids any clash with Qt's `signals`/`slots`/`emit` keyword macros
// (this file uses none of them).
#include <exiv2/exiv2.hpp>

#include <utility>

#include <QDateTime>
#include <QFileInfo>
#include <QImageReader>
#include <QSize>

#include "photometadata.h"

namespace {

// Binary-prefix size, matching how file managers usually present it.
QString humanSize(qint64 bytes)
{
    if (bytes < 0)
        return {};
    if (bytes < 1024)
        return QStringLiteral("%1 B").arg(bytes);
    static const char *const units[] = {"KB", "MB", "GB", "TB", "PB"};
    double v = bytes / 1024.0;
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    return QStringLiteral("%1 %2").arg(v, 0, 'f', 1).arg(QLatin1String(units[u]));
}

} // namespace

PhotoMetadata readPhotoMetadata(const QString &path, bool isVideo)
{
    PhotoMetadata m;

    const QFileInfo fi(path);
    if (fi.exists()) {
        m.size = humanSize(fi.size());
        m.modified = fi.lastModified().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
    }

    // Dimensions and EXIF are image-only; videos get just the file facts above.
    if (isVideo)
        return m;

    // Pixel dimensions from the image header (cheap, covers every format Qt can
    // read). Swapped below if EXIF orientation rotates the image a quarter turn,
    // so the figure matches the photo as displayed.
    int w = -1, h = -1;
    QImageReader reader(path);
    const QSize sz = reader.size();
    if (sz.isValid()) {
        w = sz.width();
        h = sz.height();
    }

    int orientation = 0;
    try {
        Exiv2::Image::UniquePtr image = Exiv2::ImageFactory::open(path.toStdString());
        if (image.get()) {
            image->readMetadata();
            const Exiv2::ExifData &exif = image->exifData();
            if (!exif.empty()) {
                const auto getStr = [&exif](const char *key) -> QString {
                    const auto it = exif.findKey(Exiv2::ExifKey(key));
                    if (it != exif.end())
                        return QString::fromStdString(it->toString()).trimmed();
                    return {};
                };

                // Date taken: DateTimeOriginal is the capture time; fall back to
                // the generic DateTime tag. EXIF stores "YYYY:MM:DD HH:MM:SS".
                QString taken = getStr("Exif.Photo.DateTimeOriginal");
                if (taken.isEmpty())
                    taken = getStr("Exif.Image.DateTime");
                if (!taken.isEmpty()) {
                    const QDateTime dt =
                        QDateTime::fromString(taken, QStringLiteral("yyyy:MM:dd HH:mm:ss"));
                    m.taken = dt.isValid()
                                  ? dt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                                  : taken;
                }

                const QString make = getStr("Exif.Image.Make");
                const QString model = getStr("Exif.Image.Model");
                // Many cameras repeat the make inside the model ("NIKON D7000");
                // only prepend the make when it isn't already there.
                if (make.isEmpty() || model.startsWith(make, Qt::CaseInsensitive))
                    m.camera = model;
                else if (model.isEmpty())
                    m.camera = make;
                else
                    m.camera = make + QLatin1Char(' ') + model;

                const QString ori = getStr("Exif.Image.Orientation");
                if (!ori.isEmpty())
                    orientation = ori.toInt();
            }
        }
    } catch (...) {
        // Corrupt or unsupported metadata: leave the EXIF fields empty.
    }

    // Orientation values 5–8 rotate the image 90°, so width and height swap.
    if (w > 0 && h > 0) {
        if (orientation >= 5 && orientation <= 8)
            std::swap(w, h);
        m.dimensions = QStringLiteral("%1 × %2").arg(w).arg(h);
    }

    return m;
}
