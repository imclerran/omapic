#pragma once

#include <QQuickImageProvider>
#include <QString>
#include <QThreadPool>

// Serves grid thumbnails via image://thumbs/<file-path>. Thumbnails are generated
// on a worker thread and cached to disk so later launches load them instantly.
class ThumbnailProvider : public QQuickAsyncImageProvider {
public:
    ThumbnailProvider();

    QQuickImageResponse *requestImageResponse(const QString &id,
                                              const QSize &requestedSize) override;

private:
    QThreadPool m_pool;
    QString m_cacheDir;
};
