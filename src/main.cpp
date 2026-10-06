// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>

#include "omarchytheme.h"
#include "thumbnailprovider.h"

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("omapic"));
    QGuiApplication::setOrganizationName(QStringLiteral("omapic"));
    // Ties the window to omapic.desktop so Wayland compositors show the right
    // icon and name in docks, alt-tab, etc. (the app_id becomes "omapic").
    QGuiApplication::setDesktopFileName(QStringLiteral("omapic"));

    // Fusion renders fully from the QPalette, which the Omarchy theme bridge
    // below fills from the active theme's colors.
    if (qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE"))
        QQuickStyle::setStyle(QStringLiteral("Fusion"));

    // Apply the Omarchy palette before any QML/controls are created.
    auto *omarchyTheme = new OmarchyTheme(&app);

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("omarchyTheme"), omarchyTheme);
    engine.addImageProvider(QStringLiteral("thumbs"), new ThumbnailProvider);
    engine.loadFromModule("Omapic", "Main");
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
