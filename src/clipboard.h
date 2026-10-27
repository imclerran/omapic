// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QClipboard>
#include <QGuiApplication>
#include <QObject>
#include <QQmlEngine>
#include <QString>

// QML-facing access to the system clipboard's text, which Qt Quick only
// exposes through text editors. The query builder copies chips as OQL text
// and pastes text as chips through it: `Clipboard.text` / `Clipboard.setText(s)`.
class Clipboard : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
public:
    explicit Clipboard(QObject *parent = nullptr) : QObject(parent) {}

    Q_INVOKABLE QString text() const { return QGuiApplication::clipboard()->text(); }
    Q_INVOKABLE void setText(const QString &s) { QGuiApplication::clipboard()->setText(s); }
};
