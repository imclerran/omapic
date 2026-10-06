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
#include <QQuickImageResponse>
#include <QQuickTextureFactory>
#include <QRunnable>
#include <QStandardPaths>
#include <QThread>
#include <QUrl>

namespace {

constexpr int kThumbBox = 320;

QImage makeThumbnail(const QString &rawId, const QString &cacheDir)
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

    // Key on path + mtime + box size, so edited files regenerate automatically.
    const QByteArray keySrc = (path + QLatin1Char('|')
                               + QString::number(fi.lastModified().toSecsSinceEpoch())
                               + QLatin1Char('|') + QString::number(kThumbBox))
                                  .toUtf8();
    const QString key = QString::fromLatin1(
        QCryptographicHash::hash(keySrc, QCryptographicHash::Sha1).toHex());
    const QString cachePath = cacheDir + QLatin1Char('/') + key + QStringLiteral(".jpg");

    QImage img;
    if (QFileInfo::exists(cachePath) && img.load(cachePath))
        return img;

    QImageReader reader(path);
    reader.setAutoTransform(true); // honour EXIF orientation
    const QSize orig = reader.size();
    if (orig.isValid()) {
        const QSize target = orig.scaled(kThumbBox, kThumbBox, Qt::KeepAspectRatio);
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
    ThumbnailResponse(const QString &id, const QString &cacheDir)
        : m_id(id)
        , m_cacheDir(cacheDir)
    {
        setAutoDelete(false); // the engine owns and deletes the response
    }

    void run() override
    {
        m_image = makeThumbnail(m_id, m_cacheDir);
        emit finished();
    }

    QQuickTextureFactory *textureFactory() const override
    {
        return QQuickTextureFactory::textureFactoryForImage(m_image);
    }

private:
    QString m_id;
    QString m_cacheDir;
    QImage m_image;
};

} // namespace

ThumbnailProvider::ThumbnailProvider()
{
    m_cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
                 + QStringLiteral("/thumbnails");
    QDir().mkpath(m_cacheDir);
    m_pool.setMaxThreadCount(qMax(2, QThread::idealThreadCount()));
}

QQuickImageResponse *ThumbnailProvider::requestImageResponse(const QString &id,
                                                             const QSize &requestedSize)
{
    Q_UNUSED(requestedSize)
    auto *response = new ThumbnailResponse(id, m_cacheDir);
    m_pool.start(response);
    return response;
}
