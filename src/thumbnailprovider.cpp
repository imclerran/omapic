// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#include "thumbnailprovider.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QProcess>
#include <QQuickImageResponse>
#include <QQuickTextureFactory>
#include <QRunnable>
#include <QStandardPaths>
#include <QThread>
#include <QUrl>

#include "photo.h"

namespace {

// Box size used when a request carries no usable size, and the ceiling we'll
// ever generate (guards against an absurd request blowing up the cache / memory;
// 1024 covers the largest cell on a 3x display).
constexpr int kDefaultBox = 320;
constexpr int kMaxBox = 1024;

// Render a representative frame of a video to `cachePath` using
// ffmpegthumbnailer (runs on this pool worker thread, so blocking is fine).
QImage makeVideoThumbnail(const QString &path, const QString &cachePath, int box)
{
    QProcess proc;
    proc.start(QStringLiteral("ffmpegthumbnailer"),
               {QStringLiteral("-i"), path,
                QStringLiteral("-o"), cachePath,
                QStringLiteral("-s"), QString::number(box),
                QStringLiteral("-q"), QStringLiteral("8")});
    // Generous ceiling; a frame grab is normally well under a second.
    if (!proc.waitForStarted(5000))
        return {}; // ffmpegthumbnailer not installed / not on PATH
    if (!proc.waitForFinished(30000)) {
        proc.kill();
        return {};
    }
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0)
        return {};

    QImage img;
    if (img.load(cachePath))
        return img;
    return {};
}

// `box` is the longest-edge target in physical pixels (the display already
// folds in the device pixel ratio, see requestImageResponse). Each size is
// cached separately and generated lazily — the grid only ever asks for the
// cell sizes it actually shows, so caches for other sizes never get built.
QImage makeThumbnail(const QString &rawId, const QString &cacheDir, int box)
{
    // The provider id is the local file path; fall back to percent-decoding.
    QString path = rawId;
    if (!QFileInfo::exists(path)) {
        const QString decoded = QUrl::fromPercentEncoding(rawId.toUtf8());
        if (QFileInfo::exists(decoded))
            path = decoded;
    }

    const QFileInfo fi(path);
    if (!fi.exists())
        return {};

    // Key on path + mtime + box size, so edited files regenerate automatically
    // and each requested size gets its own cache entry.
    const QByteArray keySrc = (path + QLatin1Char('|')
                               + QString::number(fi.lastModified().toSecsSinceEpoch())
                               + QLatin1Char('|') + QString::number(box))
                                  .toUtf8();
    const QString key = QString::fromLatin1(
        QCryptographicHash::hash(keySrc, QCryptographicHash::Sha1).toHex());
    const QString cachePath = cacheDir + QLatin1Char('/') + key + QStringLiteral(".jpg");

    QImage img;
    if (QFileInfo::exists(cachePath) && img.load(cachePath))
        return img;

    if (isVideoPath(path))
        return makeVideoThumbnail(path, cachePath, box);

    QImageReader reader(path);
    reader.setAutoTransform(true); // honour EXIF orientation
    const QSize orig = reader.size();
    if (orig.isValid()) {
        const QSize target = orig.scaled(box, box, Qt::KeepAspectRatio);
        if (!target.isEmpty() && target.width() <= orig.width())
            reader.setScaledSize(target); // fast downscale during decode
    }
    img = reader.read();
    if (img.isNull())
        return {};

    img.save(cachePath, "JPEG", 85);
    return img;
}

class ThumbnailResponse : public QQuickImageResponse, public QRunnable {
public:
    ThumbnailResponse(const QString &id, const QString &cacheDir, int box)
        : m_id(id)
        , m_cacheDir(cacheDir)
        , m_box(box)
    {
        setAutoDelete(false); // the engine owns and deletes the response
    }

    void run() override
    {
        m_image = makeThumbnail(m_id, m_cacheDir, m_box);
        emit finished();
    }

    QQuickTextureFactory *textureFactory() const override
    {
        return QQuickTextureFactory::textureFactoryForImage(m_image);
    }

    // A null result (undecodable file — a corrupt image, or a HEIC/HEIF with no
    // HEIF plugin installed) reports as an error so the delegate can swap in a
    // placeholder rather than silently showing a blank card.
    QString errorString() const override
    {
        return m_image.isNull() ? QStringLiteral("thumbnail unavailable") : QString();
    }

private:
    QString m_id;
    QString m_cacheDir;
    int m_box;
    QImage m_image;
};

} // namespace

ThumbnailProvider::ThumbnailProvider()
{
    // Pinned to ~/.cache/omapic regardless of organizationName (set for
    // QSettings), which would otherwise nest CacheLocation under a second
    // "omapic". Thumbnails are regenerable, so no migration is needed either way.
    m_cacheDir = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
                 + QStringLiteral("/omapic/thumbnails");
    QDir().mkpath(m_cacheDir);
    m_pool.setMaxThreadCount(qMax(2, QThread::idealThreadCount()));
}

QQuickImageResponse *ThumbnailProvider::requestImageResponse(const QString &id,
                                                             const QSize &requestedSize)
{
    // The QML Image's sourceSize arrives here already multiplied by the window's
    // device pixel ratio, so honouring it gives crisp thumbnails on HiDPI grids
    // without any dpr math of our own. Fall back to a sane box when a caller sets
    // no sourceSize, and cap it so an outlandish request can't bloat the cache.
    const int longest = qMax(requestedSize.width(), requestedSize.height());
    const int box = longest > 0 ? qMin(longest, kMaxBox) : kDefaultBox;
    auto *response = new ThumbnailResponse(id, m_cacheDir, box);
    m_pool.start(response);
    return response;
}
