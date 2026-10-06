// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>
#include <QString>

#include "database.h"

// Runs the slow, file-hashing-heavy jobs (startup migration and folder import)
// on a background thread so the GUI stays responsive, reporting progress as it
// goes. It owns its own database connection; the models it never touches — the
// Library reloads them from the DB once a job finishes.
class LibraryWorker : public QObject {
    Q_OBJECT

public:
    explicit LibraryWorker(const QString &dbPath, QObject *parent = nullptr);

public slots:
    void initialize();                        // open the worker's DB connection (in its thread)
    void runMigration();                      // hash un-hashed photos, then seed the tag archive
    void importDirectory(const QString &root); // walk, hash, insert, reattach archived tags
    void rescan();                            // reconcile the DB with disk: add/relocate/remove

signals:
    void progress(const QString &phase, int done, int total); // total <= 0 means indeterminate
    void migrationFinished();
    void importFinished(const QString &root);
    void rescanFinished();

private:
    QString m_dbPath;
    Database m_db;
};
