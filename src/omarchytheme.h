// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QColor>
#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>

// Reads the active Omarchy theme's colors.toml, applies it as the application
// QPalette (so Qt Quick Controls + SystemPalette follow it), and exposes the
// theme accent to QML. Watches for `omarchy theme set` and reloads live.
class OmarchyTheme : public QObject {
    Q_OBJECT
    Q_PROPERTY(QColor accent READ accent NOTIFY changed)
    Q_PROPERTY(QColor background READ background NOTIFY changed)
    Q_PROPERTY(QColor foreground READ foreground NOTIFY changed)
    Q_PROPERTY(QColor surface READ surface NOTIFY changed)
    Q_PROPERTY(QColor dimText READ dimText NOTIFY changed)
    Q_PROPERTY(bool dark READ dark NOTIFY changed)
    // Colors available to tag categories, as a list of QColor. A category stores
    // a slot index into this list (not a color), so the chips follow the theme.
    // Built from the theme's named colors when present, else a built-in set.
    Q_PROPERTY(QVariantList categoryPalette READ categoryPalette NOTIFY changed)

public:
    explicit OmarchyTheme(QObject *parent = nullptr);

    // Builds a QPalette from the loaded colors and sets it on QGuiApplication.
    // No-op (keeps the platform theme) when no Omarchy colors are available.
    void applyToApplication();

    // Force a palette-change notification even if the palette is unchanged, so
    // controls that inherited the platform (gtk3) window palette at creation
    // re-resolve against ours — mirrors what a live theme swap does.
    Q_INVOKABLE void reapplyForced();

    QColor accent() const;
    QColor background() const;
    QColor foreground() const;
    QColor surface() const;
    QColor dimText() const;
    bool dark() const { return m_mode != QStringLiteral("light"); }
    QVariantList categoryPalette() const;
    // Color for a category palette slot (wraps when the slot exceeds the palette).
    // Returns `fallback` for a negative slot (an uncategorized tag).
    Q_INVOKABLE QColor categoryColor(int slot, const QColor &fallback) const;
    // Near-black or near-white, whichever reads better on `background`.
    Q_INVOKABLE QColor textOn(const QColor &background) const;

signals:
    void changed();

private:
    static QString colorsPath();
    void reload();
    QColor color(const QString &key, const QColor &fallback) const;

    QHash<QString, QColor> m_colors;
    QString m_mode;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
};
