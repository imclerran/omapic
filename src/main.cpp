// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImageReader>
#include <QList>
#include <QLoggingCategory>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStringList>
#include <QTextStream>
#include <QUrl>
#include <QVariantMap>

#include "cli.h"
#include "mediascan.h"
#include "omarchytheme.h"
#include "thumbnailprovider.h"

namespace {
// The process-wide names QSettings and the data dirs key on; the same for the
// gallery and the command-line tool so they read one config and one library.
void setApplicationNames()
{
    QCoreApplication::setApplicationName(QStringLiteral("omapic"));
    // So QSettings lands in ~/.config/omapic/ (not an "Unknown Organization"
    // folder) and groups values by category. The data and cache dirs are pinned
    // to Generic*Location + "/omapic" (see library.cpp / thumbnailprovider.cpp)
    // so setting this does not nest or move them.
    QCoreApplication::setOrganizationName(QStringLiteral("omapic"));
}
}

int main(int argc, char *argv[])
{
    // Dispatch on a subcommand before creating any application object: the
    // read-only commands (query, tags, albums, categories) run on a
    // QCoreApplication so they work without a display; view and slideshow fall
    // through to the GUI below with their query already recognised. Subcommands
    // win over file arguments — a file named "query" is opened with
    // `omapic ./query` or `omapic -- query`.
    QStringList rawArgs;
    for (int i = 1; i < argc; ++i)
        rawArgs.append(QString::fromLocal8Bit(argv[i]));
    const cli::Options cliOpts = cli::parse(rawArgs);
    if (cli::isHeadless(cliOpts.command)) {
        QCoreApplication app(argc, argv);
        setApplicationNames();
        QTextStream out(stdout), err(stderr);
        return cli::runHeadless(cliOpts, out, err);
    }
    const bool queryLaunch = cliOpts.command != cli::Command::None;

    QGuiApplication app(argc, argv);
    setApplicationNames();
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
    if (!queryLaunch) {
        QCommandLineParser parser;
        parser.setApplicationDescription(
            QStringLiteral("omapic — photo gallery, or a viewer when given files.\n\n")
            + cli::usage());
        parser.addHelpOption();
        parser.addVersionOption();
        parser.addPositionalArgument(QStringLiteral("files"),
                                     QStringLiteral("Image/video files to view."),
                                     QStringLiteral("[files...]"));
        parser.process(app);
        fileArgs = parser.positionalArguments();
    }

    // Resolve the arguments to media URLs. A query launch (`omapic view '<q>'`,
    // `omapic slideshow '<q>'`) resolves the query against the library; the
    // result set is the whole scope, so launchSinglePath stays false (S in the
    // viewer must not expand to a photo's directory tree). Otherwise a lone
    // directory argument opens that folder's tree, and each existing media file
    // is taken in order.
    QList<QUrl> launchUrls;
    bool singlePath = false;
    bool slideshowLaunch = false;
    QVariantMap slideshowOptions;
    if (queryLaunch) {
        QTextStream err(stderr);
        if (cliOpts.help) {
            QTextStream(stdout) << cli::usage(cliOpts.command);
            return cli::kExitMatches;
        }
        const int code = cli::resolveMedia(cliOpts, launchUrls, err);
        if (code != cli::kExitMatches)
            return code; // no window for an error or an empty result
        if (cliOpts.command == cli::Command::Slideshow) {
            slideshowLaunch = true;
            // From the flags alone: a query launch never reads the gallery's
            // saved settings.
            const cli::SlideshowSettings s = cli::slideshowSettings(cliOpts);
            const int start = cli::orderPlaylist(launchUrls, s);
            slideshowOptions = {
                {QStringLiteral("intervalMs"), s.intervalMs},
                {QStringLiteral("shuffle"), s.shuffle},
                {QStringLiteral("fade"), s.fade},
                {QStringLiteral("loop"), s.loop},
                {QStringLiteral("startIndex"), start},
            };
        }
    } else if (fileArgs.size() == 1 && QFileInfo(fileArgs.first()).isDir()) {
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

    // HEIC/HEIF and AVIF decoding lives in Qt image-format plugins that aren't
    // part of a stock Qt install (on Arch, kimageformats, which loads libheif for
    // HEIC/HEIF and libavif for AVIF). When a plugin is missing, those files are
    // scanned in but can't be rendered, so expose each capability to QML (for a
    // graceful placeholder/notice) and say so once on the console to point at the
    // fix.
    const QList<QByteArray> fmts = QImageReader::supportedImageFormats();
    const bool heifSupported = fmts.contains("heic") || fmts.contains("heif");
    const bool avifSupported = fmts.contains("avif");
    if (!heifSupported || !avifSupported) {
        QStringList missing;
        if (!heifSupported)
            missing << QStringLiteral("HEIC/HEIF");
        if (!avifSupported)
            missing << QStringLiteral("AVIF");
        const QString msg =
            QStringLiteral("omapic: no image plugin for %1 — those files will not "
                           "display. Install a Qt image plugin (on Arch: kimageformats, "
                           "plus libheif/libavif) to enable them.")
                .arg(missing.join(QLatin1Char('/')));
        qWarning("%s", msg.toUtf8().constData());
    }

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("omarchyTheme"), omarchyTheme);
    engine.rootContext()->setContextProperty(QStringLiteral("launchUrls"),
                                              QVariant::fromValue(launchUrls));
    engine.rootContext()->setContextProperty(QStringLiteral("launchSinglePath"), singlePath);
    engine.rootContext()->setContextProperty(QStringLiteral("launchSlideshow"), slideshowLaunch);
    engine.rootContext()->setContextProperty(QStringLiteral("launchSlideshowOptions"), slideshowOptions);
    engine.rootContext()->setContextProperty(QStringLiteral("launchFullscreen"), cliOpts.fullscreen);
    engine.rootContext()->setContextProperty(QStringLiteral("heifSupported"), heifSupported);
    engine.rootContext()->setContextProperty(QStringLiteral("avifSupported"), avifSupported);
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
