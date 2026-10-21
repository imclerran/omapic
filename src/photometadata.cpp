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
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSize>

#include "photometadata.h"
#include "sidecar.h" // sidecar::splitHierarchical, shared with the sidecar reader

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

// The EXIF capture time as a QDateTime (invalid when absent/unparsable):
// DateTimeOriginal first, then the generic DateTime tag. EXIF stores
// "YYYY:MM:DD HH:MM:SS". Returns an invalid QDateTime on any error.
QDateTime exifCaptureDateTime(const QString &path)
{
    try {
        Exiv2::Image::UniquePtr image = Exiv2::ImageFactory::open(path.toStdString());
        if (!image.get())
            return {};
        image->readMetadata();
        const Exiv2::ExifData &exif = image->exifData();
        if (exif.empty())
            return {};

        const auto getStr = [&exif](const char *key) -> QString {
            const auto it = exif.findKey(Exiv2::ExifKey(key));
            if (it != exif.end())
                return QString::fromStdString(it->toString()).trimmed();
            return {};
        };

        QString taken = getStr("Exif.Photo.DateTimeOriginal");
        if (taken.isEmpty())
            taken = getStr("Exif.Image.DateTime");
        if (taken.isEmpty())
            return {};
        return QDateTime::fromString(taken, QStringLiteral("yyyy:MM:dd HH:mm:ss"));
    } catch (...) {
        return {};
    }
}

// Parse an ffprobe creation_time (ISO-8601 UTC, e.g. "2023-05-14T10:30:00.000000Z")
// into a QDateTime; invalid when empty/unparsable.
QDateTime parseVideoCreationTime(const QString &s)
{
    if (s.isEmpty())
        return {};
    QDateTime dt = QDateTime::fromString(s, Qt::ISODate); // honours the trailing Z
    if (!dt.isValid()) // ffmpeg sometimes emits microseconds, which ISODate rejects
        dt = QDateTime::fromString(s.left(19), QStringLiteral("yyyy-MM-ddTHH:mm:ss"));
    if (!dt.isValid())
        return {};
    if (dt.timeSpec() == Qt::LocalTime) // no zone in the parsed form → it's UTC
        dt.setTimeSpec(Qt::UTC);
    return dt;
}

// The video's container creation time as a QDateTime (invalid when absent or
// ffprobe is unavailable). exiv2 dropped video support in 0.28, so shell out to
// ffprobe — it ships with ffmpeg, as ffmpegthumbnailer already does.
QDateTime videoCaptureDateTime(const QString &path)
{
    QProcess proc;
    proc.start(QStringLiteral("ffprobe"),
               {QStringLiteral("-v"), QStringLiteral("quiet"),
                QStringLiteral("-show_entries"), QStringLiteral("format_tags=creation_time"),
                QStringLiteral("-of"), QStringLiteral("default=noprint_wrappers=1:nokey=1"),
                path});
    if (!proc.waitForStarted(3000))
        return {}; // ffprobe not installed / not on PATH
    if (!proc.waitForFinished(5000)) {
        proc.kill();
        return {};
    }
    return parseVideoCreationTime(QString::fromUtf8(proc.readAllStandardOutput()).trimmed());
}

// Seconds → "M:SS", or "H:MM:SS" past an hour. Empty for non-positive input.
QString formatDuration(double seconds)
{
    if (!(seconds > 0))
        return {};
    const qint64 total = qRound64(seconds);
    const qint64 h = total / 3600, mm = (total % 3600) / 60, ss = total % 60;
    if (h > 0)
        return QStringLiteral("%1:%2:%3").arg(h).arg(mm, 2, 10, QLatin1Char('0'))
                   .arg(ss, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(mm).arg(ss, 2, 10, QLatin1Char('0'));
}

} // namespace

qint64 capturedAtEpoch(const QString &path, bool isVideo)
{
    // Prefer the real capture time — EXIF for images, the container creation time
    // for videos — so both land on the timeline by when they were taken; fall back
    // to the file mtime when neither is available.
    const QDateTime dt = isVideo ? videoCaptureDateTime(path) : exifCaptureDateTime(path);
    if (dt.isValid())
        return dt.toSecsSinceEpoch();

    const QFileInfo fi(path);
    if (fi.exists())
        return fi.lastModified().toSecsSinceEpoch();
    return 0;
}

ImportMetadata readImportMetadata(const QString &path, bool isVideo)
{
    ImportMetadata m;

    // Videos skip exiv2 entirely (as capturedAtEpoch does); images read the
    // capture date and embedded keyword tags from one open.
    if (!isVideo) {
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
                    QString taken = getStr("Exif.Photo.DateTimeOriginal");
                    if (taken.isEmpty())
                        taken = getStr("Exif.Image.DateTime");
                    if (!taken.isEmpty()) {
                        const QDateTime dt = QDateTime::fromString(
                            taken, QStringLiteral("yyyy:MM:dd HH:mm:ss"));
                        if (dt.isValid())
                            m.takenAt = dt.toSecsSinceEpoch();
                    }
                }

                // Embedded XMP keywords: flat (dc:subject) + hierarchical.
                const Exiv2::XmpData &xmp = image->xmpData();
                for (const auto &v : xmp) {
                    if (v.key() == "Xmp.dc.subject") {
                        for (long i = 0; i < v.count(); ++i) {
                            const QString s = QString::fromStdString(v.toString(i)).trimmed();
                            if (!s.isEmpty())
                                m.flat << s;
                        }
                    } else if (v.key() == "Xmp.lr.hierarchicalSubject") {
                        for (long i = 0; i < v.count(); ++i) {
                            const QString s = QString::fromStdString(v.toString(i)).trimmed();
                            if (!s.isEmpty())
                                m.categorized.append(sidecar::splitHierarchical(s));
                        }
                    }
                }

                // Embedded IPTC keywords (repeatable, flat).
                const Exiv2::IptcData &iptc = image->iptcData();
                for (const auto &v : iptc) {
                    if (v.key() == "Iptc.Application2.Keywords") {
                        const QString s = QString::fromStdString(v.toString()).trimmed();
                        if (!s.isEmpty())
                            m.flat << s;
                    }
                }
            }
        } catch (...) {
            // Unreadable/metadata-less file: fall through to the mtime date.
        }
    } else {
        // Videos have no EXIF; take the container creation time via ffprobe.
        const QDateTime dt = videoCaptureDateTime(path);
        if (dt.isValid())
            m.takenAt = dt.toSecsSinceEpoch();
    }

    if (m.takenAt == 0) {
        const QFileInfo fi(path);
        if (fi.exists())
            m.takenAt = fi.lastModified().toSecsSinceEpoch();
    }
    return m;
}

PhotoMetadata readPhotoMetadata(const QString &path, bool isVideo)
{
    PhotoMetadata m;

    const QFileInfo fi(path);
    if (fi.exists()) {
        m.size = humanSize(fi.size());
        m.modified = fi.lastModified().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
    }

    // Videos: exiv2 doesn't cover them; pull resolution, duration, and creation
    // time from a single ffprobe call.
    if (isVideo) {
        QProcess proc;
        proc.start(QStringLiteral("ffprobe"),
                   {QStringLiteral("-v"), QStringLiteral("quiet"),
                    QStringLiteral("-select_streams"), QStringLiteral("v:0"),
                    QStringLiteral("-show_entries"),
                    QStringLiteral("stream=width,height:format=duration:format_tags=creation_time"),
                    QStringLiteral("-of"), QStringLiteral("json"), path});
        if (proc.waitForStarted(3000) && proc.waitForFinished(5000)) {
            const QJsonObject obj = QJsonDocument::fromJson(proc.readAllStandardOutput()).object();
            const QJsonArray streams = obj.value(QStringLiteral("streams")).toArray();
            if (!streams.isEmpty()) {
                const QJsonObject s = streams.first().toObject();
                const int w = s.value(QStringLiteral("width")).toInt();
                const int h = s.value(QStringLiteral("height")).toInt();
                if (w > 0 && h > 0)
                    m.dimensions = QStringLiteral("%1 × %2").arg(w).arg(h);
            }
            const QJsonObject fmt = obj.value(QStringLiteral("format")).toObject();
            m.duration = formatDuration(fmt.value(QStringLiteral("duration")).toString().toDouble());
            const QDateTime dt = parseVideoCreationTime(
                fmt.value(QStringLiteral("tags")).toObject()
                   .value(QStringLiteral("creation_time")).toString());
            if (dt.isValid())
                m.taken = dt.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        }
        return m;
    }

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
