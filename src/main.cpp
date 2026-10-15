// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QCommandLineParser>
#include <QFileInfo>
#include <QGuiApplication>
#include <QList>
#include <QLoggingCategory>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QUrl>

#include "mediascan.h"
#include "omarchytheme.h"
#include "thumbnailprovider.h"

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("omapic"));
    // So QSettings lands in ~/.config/omapic/ (not an "Unknown Organization"
    // folder) and groups values by category. The data and cache dirs are pinned
    // to Generic*Location + "/omapic" (see library.cpp / thumbnailprovider.cpp)
    // so setting this does not nest or move them.
    QGuiApplication::setOrganizationName(QStringLiteral("omapic"));
    // Ties the window to omapic.desktop so Wayland compositors show the right
    // icon and name in docks, alt-tab, etc. (the app_id becomes "omapic").
    QGuiApplication::setDesktopFileName(QStringLiteral("omapic"));

    // Quiet two harmless-but-noisy Qt categories so launching omapic — especially
    // as a terminal/file-manager image viewer — doesn't print stderr spam: the
    // multimedia backend's FFmpeg-version banner, and the Wayland portal's benign
    // "could not register app ID" warning. An explicit QT_LOGGING_RULES wins.
    if (qEnvironmentVariableIsEmpty("QT_LOGGING_RULES"))
        QLoggingCategory::setFilterRules(QStringLiteral(
            "qt.multimedia.ffmpeg.info=false\n"
            "qt.qpa.services.warning=false"));

    // Fusion renders fully from the QPalette, which the Omarchy theme bridge
    // below fills from the active theme's colors.
    if (qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE"))
        QQuickStyle::setStyle(QStringLiteral("Fusion"));

    // Apply the Omarchy palette before any QML/controls are created.
    auto *omarchyTheme = new OmarchyTheme(&app);

    // Positional file arguments turn omapic into a lightweight image viewer
    // (`omapic photo.jpg …`), so it can be the system's default image handler
    // without paying for the full gallery (no database, no scan, no worker).
    QStringList fileArgs;
    {
        QCommandLineParser parser;
        parser.setApplicationDescription(
            QStringLiteral("omapic — photo gallery, or a viewer when given files"));
        parser.addHelpOption();
        parser.addVersionOption();
        parser.addPositionalArgument(QStringLiteral("files"),
                                     QStringLiteral("Image/video files to view."),
                                     QStringLiteral("[files...]"));
        parser.process(app);
        fileArgs = parser.positionalArguments();
    }

    // Resolve the arguments to media URLs. A lone directory argument opens that
    // folder's tree; otherwise each existing media file is taken in order.
    QList<QUrl> launchUrls;
    bool singlePath = false;
    if (fileArgs.size() == 1 && QFileInfo(fileArgs.first()).isDir()) {
        const QString dir = QFileInfo(fileArgs.first()).absoluteFilePath();
        for (const QString &f : mediascan::collectSorted(dir))
            launchUrls.append(QUrl::fromLocalFile(f));
        singlePath = true; // slideshow scope is already the whole tree
    } else {
        for (const QString &p : fileArgs) {
            const QFileInfo fi(p);
            if (fi.exists() && fi.isFile() && mediascan::isMediaFile(fi.absoluteFilePath()))
                launchUrls.append(QUrl::fromLocalFile(fi.absoluteFilePath()));
        }
        singlePath = (fileArgs.size() == 1); // one path => slideshow the folder
    }
    const bool viewerMode = !launchUrls.isEmpty();

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("omarchyTheme"), omarchyTheme);
    engine.rootContext()->setContextProperty(QStringLiteral("launchUrls"),
                                              QVariant::fromValue(launchUrls));
    engine.rootContext()->setContextProperty(QStringLiteral("launchSinglePath"), singlePath);
    engine.addImageProvider(QStringLiteral("thumbs"), new ThumbnailProvider);
    engine.loadFromModule("Omapic", viewerMode ? "Viewer" : "Main");
    if (engine.rootObjects().isEmpty())
        return -1;

    // The gtk3 platform theme stamps its own palette on the window at creation,
    // which the toolbar inherits. After the first frame (platform palette now
    // settled), force our palette to win — the same effect as a live swap.
    if (auto *win = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst())) {
        QObject::connect(win, &QQuickWindow::frameSwapped, omarchyTheme,
                         [omarchyTheme] { omarchyTheme->reapplyForced(); },
                         Qt::SingleShotConnection);
    }

    return app.exec();
}
