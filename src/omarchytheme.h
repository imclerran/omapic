#pragma once

#include <QColor>
#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QString>
#include <QTimer>

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
