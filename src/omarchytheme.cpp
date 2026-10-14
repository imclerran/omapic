// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#include "omarchytheme.h"

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QPalette>
#include <QTextStream>

OmarchyTheme::OmarchyTheme(QObject *parent)
    : QObject(parent)
{
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(150);
    connect(&m_debounce, &QTimer::timeout, this, &OmarchyTheme::reload);

    // Watch both the colors file and the "current" dir, since `omarchy theme set`
    // swaps the `theme` symlink (a directory change) and rewrites the file.
    const QString file = colorsPath();
    const QString currentDir = QDir::homePath() + QStringLiteral("/.local/state/omarchy/current");
    m_watcher.addPath(currentDir);
    if (QFile::exists(file))
        m_watcher.addPath(file);

    const auto schedule = [this] { m_debounce.start(); };
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, schedule);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, schedule);

    reload();
}

QString OmarchyTheme::colorsPath()
{
    return QDir::homePath()
           + QStringLiteral("/.local/state/omarchy/current/theme/colors.toml");
}

void OmarchyTheme::reload()
{
    m_colors.clear();
    m_mode.clear();

    QFile f(colorsPath());
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&f);
        while (!in.atEnd()) {
            const QString line = in.readLine().trimmed();
            if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
                continue;
            const int eq = line.indexOf(QLatin1Char('='));
            if (eq < 0)
                continue;
            const QString key = line.left(eq).trimmed();
            QString val = line.mid(eq + 1).trimmed();
            val.remove(QLatin1Char('"'));
            val.remove(QLatin1Char('\''));
            if (key == QStringLiteral("mode"))
                m_mode = val;
            else if (val.startsWith(QLatin1Char('#')) && QColor::isValidColorName(val))
                m_colors.insert(key, QColor(val));
        }
    }

    // Re-arm the file watch (editors/omarchy replace the file, dropping the path).
    const QString file = colorsPath();
    if (QFile::exists(file) && !m_watcher.files().contains(file))
        m_watcher.addPath(file);

    applyToApplication();
    emit changed();
}

void OmarchyTheme::reapplyForced()
{
    if (m_colors.isEmpty())
        return;
    // Reset to the default palette first so the subsequent apply is always seen
    // as a change (no frame renders between the two calls, so no visible flash).
    QGuiApplication::setPalette(QPalette());
    applyToApplication();
}

QColor OmarchyTheme::color(const QString &key, const QColor &fallback) const
{
    return m_colors.value(key, fallback);
}

QColor OmarchyTheme::accent() const
{
    if (m_colors.contains(QStringLiteral("accent")))
        return m_colors.value(QStringLiteral("accent"));
    return QGuiApplication::palette().highlight().color();
}

QColor OmarchyTheme::background() const
{
    return color(QStringLiteral("background"), QGuiApplication::palette().window().color());
}

QColor OmarchyTheme::foreground() const
{
    return color(QStringLiteral("foreground"), QGuiApplication::palette().windowText().color());
}

QColor OmarchyTheme::surface() const
{
    return color(QStringLiteral("lighter_background"),
                 QGuiApplication::palette().alternateBase().color());
}

QColor OmarchyTheme::dimText() const
{
    return color(QStringLiteral("dark_foreground"),
                 QGuiApplication::palette().placeholderText().color());
}

QVariantList OmarchyTheme::categoryPalette() const
{
    // Omarchy themes name their terminal colors; use those so category colors
    // match the rest of the desktop. The order is fixed so slot i stays roughly
    // the same hue family across themes (red, orange, yellow, green, ...).
    static const char *const kThemeKeys[] = {
        "red", "orange", "yellow", "green", "cyan", "blue", "magenta", "brown",
    };
    // Fallback for non-Omarchy systems: mid-saturation hues that read on both
    // light and dark surfaces, in the same order as the theme keys.
    static const char *const kFallback[] = {
        "#e06c75", "#e59350", "#d4b04a", "#7fb66a", "#45b3ad", "#5c9ded", "#a98bea", "#b07d5e",
    };

    QVariantList out;
    int fromTheme = 0;
    for (size_t i = 0; i < sizeof(kThemeKeys) / sizeof(kThemeKeys[0]); ++i) {
        const QString key = QLatin1String(kThemeKeys[i]);
        if (m_colors.contains(key)) {
            out.append(m_colors.value(key));
            ++fromTheme;
        } else {
            out.append(QColor(QLatin1String(kFallback[i])));
        }
    }
    // A theme with only one or two named colors would mix two palettes; only
    // trust it when it is reasonably complete.
    if (fromTheme < 4) {
        out.clear();
        for (const char *hex : kFallback)
            out.append(QColor(QLatin1String(hex)));
    }
    return out;
}

QColor OmarchyTheme::categoryColor(int slot, const QColor &fallback) const
{
    if (slot < 0)
        return fallback;
    const QVariantList pal = categoryPalette();
    if (pal.isEmpty())
        return fallback;
    return pal.at(slot % pal.size()).value<QColor>();
}

QColor OmarchyTheme::textOn(const QColor &background) const
{
    // Perceived luminance (Rec. 601 weights) on the sRGB components.
    const qreal lum = 0.299 * background.redF() + 0.587 * background.greenF()
                      + 0.114 * background.blueF();
    return lum > 0.55 ? QColor(QStringLiteral("#1b1b1f")) : QColor(QStringLiteral("#f7f7f8"));
}

void OmarchyTheme::applyToApplication()
{
    if (m_colors.isEmpty())
        return; // not an Omarchy system — leave the platform theme in place

    const QColor bg = color(QStringLiteral("background"), QColor(QStringLiteral("#1e1e2e")));
    const QColor fg = color(QStringLiteral("foreground"), QColor(QStringLiteral("#cdd6f4")));
    const QColor base = color(QStringLiteral("dark_background"), bg.darker(110));
    const QColor alt = color(QStringLiteral("lighter_background"), bg.lighter(120));
    const QColor acc = color(QStringLiteral("accent"), QColor(QStringLiteral("#7aa2f7")));
    const QColor dim = color(QStringLiteral("dark_foreground"), fg.darker(150));
    const QColor sel = color(QStringLiteral("selection"), alt);
    const QColor mutedColor = color(QStringLiteral("muted"), dim);
    const QColor darker = color(QStringLiteral("darker_background"), base.darker(120));
    const QColor bright = color(QStringLiteral("bright_foreground"), fg);

    QPalette p;
    p.setColor(QPalette::Window, bg);
    p.setColor(QPalette::WindowText, fg);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, alt);
    p.setColor(QPalette::Text, fg);
    p.setColor(QPalette::Button, alt);
    p.setColor(QPalette::ButtonText, fg);
    p.setColor(QPalette::BrightText, bright);
    p.setColor(QPalette::ToolTipBase, alt);
    p.setColor(QPalette::ToolTipText, fg);
    p.setColor(QPalette::Highlight, acc);
    p.setColor(QPalette::HighlightedText, bg);
    p.setColor(QPalette::PlaceholderText, dim);
    p.setColor(QPalette::Light, alt);
    p.setColor(QPalette::Midlight, alt);
    p.setColor(QPalette::Mid, mutedColor);
    p.setColor(QPalette::Dark, darker);
    p.setColor(QPalette::Shadow, darker);

    p.setColor(QPalette::Disabled, QPalette::Text, dim);
    p.setColor(QPalette::Disabled, QPalette::WindowText, dim);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, dim);
    p.setColor(QPalette::Disabled, QPalette::Highlight, sel);
    p.setColor(QPalette::Disabled, QPalette::HighlightedText, dim);

    QGuiApplication::setPalette(p);
}
