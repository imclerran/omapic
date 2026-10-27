// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#include "library.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QProcess>
#include <QSet>
#include <QStringList>
#include <QUrl>
#include <QStandardPaths>

#include <algorithm>

#include "libraryworker.h"
#include "photometadata.h"

namespace {
// Above this many photos, bulk tag add/remove runs on the worker thread with a
// progress bar instead of blocking the UI; at or below it, apply in place.
constexpr int kBulkTagThreshold = 100;

// Smart albums are stored in their own table with their own (small) ids, which
// would collide with the tag ids that identify regular album tiles. So the
// UI-facing album id of a smart album is offset into this reserved high range,
// keeping all the int-based album plumbing (selection, currentAlbum, slideshow)
// kind-agnostic. The offset is decoded only inside Library.
constexpr int kSmartAlbumIdBase = 1'000'000'000;

// The active Omarchy theme's short name (the folder name under themes/), read
// from the state file the theme switcher keeps. Empty if it can't be read.
QString currentOmarchyTheme()
{
    QFile f(QDir::homePath() + QStringLiteral("/.local/state/omarchy/current/theme.name"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(f.readAll()).trimmed();
}

// Copy srcPath into destDir (created if needed), keeping the original filename.
// If a file with that name is already there, treat it as already installed
// rather than duplicating it. Returns true once the file is present in destDir.
bool copyIntoDir(const QString &srcPath, const QString &destDir)
{
    if (!QDir().mkpath(destDir))
        return false;
    const QString dest = destDir + QLatin1Char('/') + QFileInfo(srcPath).fileName();
    if (QFile::exists(dest))
        return true; // same name already present — nothing to do
    return QFile::copy(srcPath, dest);
}
}

Library::Library(QObject *parent)
    : QObject(parent)
    , m_photos(new PhotoModel(this))
    , m_filtered(new PhotoFilterModel(this))
    , m_tags(new TagModel(this))
{
    // Pinned to ~/.local/share/omapic regardless of organizationName (which is
    // set for QSettings' sake); AppDataLocation would otherwise nest this under a
    // second "omapic" once an org name exists.
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                        + QStringLiteral("/omapic");
    QDir().mkpath(dir);
    const QString dbPath = dir + QStringLiteral("/library.db");
    if (!m_db.open(dbPath)) {
        // Surface a blocking error to the UI and load nothing else — in
        // particular, never start the worker against a library we refused to open.
        m_fatalError = m_db.schemaTooNew()
            ? tr("This library was created by a newer version of Omapic. "
                 "Update Omapic or restore a backup.")
            : tr("The library database could not be opened.");
        return;
    }

    // Resolve desktop-integration capabilities once; the menus bind to these.
    m_omarchyAvailable = detectOmarchy();
    m_aetherAvailable = detectAether();
    m_wallpaperBackend = detectWallpaperBackend(m_omarchyAvailable);

    m_filtered->setSourceModel(m_photos);
    m_filtered->setDynamicSortFilter(true);
    // Ordering is defined by PhotoFilterModel::lessThan (natural, case-insensitive
    // sort of the file name); sort(0) activates it and keeps it maintained.
    m_filtered->sort(0);

    // Changing the tag selection re-narrows the sidebar (faceted drill-down) when
    // in "match all" mode.
    connect(m_tags, &TagModel::selectionChanged, this, &Library::refreshTags);

    QVector<Photo> loaded = m_db.loadPhotos();
    // Preset the timeline's date ordering (the first-launch default) before the
    // model is populated, so the initial sort is a cheap integer compare. Without
    // this the proxy's default name ordering runs a full QCollator sort on load
    // that the timeline view immediately re-sorts away. applyViewFilter() (QML,
    // on completion) still applies the actually-saved view; for a non-timeline
    // saved view it re-sorts once — the same cost as before, just not doubled.
    m_filtered->setSortMode(PhotoFilterModel::SortByDate);
    m_photos->setPhotos(std::move(loaded));
    // The album view's membership gate asks here for an album's query tree.
    m_filtered->setAlbumQueryResolver([this](int albumId) { return albumTree(albumId); });
    rebuildQueryState();
    // The tag sidebar is built asynchronously on the worker (see below), so the
    // window appears and stays responsive instead of stalling on its ~150ms query.
    updateFolderRoots();
    updateAlbumTagIds();
    // Keep the folder-view's orphan detection in step as folders come and go.
    connect(this, &Library::foldersChanged, this, &Library::updateFolderRoots);
    // The remembered-removed-files count changes whenever the photo set does
    // (removals archive a hash; re-adds clear one), which is exactly when
    // foldersChanged fires — so refresh the settings stat off the same signal.
    connect(this, &Library::foldersChanged, this, &Library::rememberedRemovedCountChanged);

    // Hashing runs on a worker thread so the window stays responsive. The worker
    // owns its own DB connection; we only touch the models here on the GUI thread.
    qRegisterMetaType<QVector<TagInfo>>(); // crosses the thread boundary below
    m_worker = new LibraryWorker(dbPath);
    m_worker->moveToThread(&m_workerThread);
    connect(&m_workerThread, &QThread::started, m_worker, &LibraryWorker::initialize);
    connect(&m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(this, &Library::requestMigration, m_worker, &LibraryWorker::runMigration);
    connect(this, &Library::requestImport, m_worker, &LibraryWorker::importDirectory);
    connect(this, &Library::requestRescan, m_worker, &LibraryWorker::rescan);
    connect(this, &Library::requestImplicationMigration, m_worker, &LibraryWorker::expandImplications);
    connect(this, &Library::requestCreateAlias, m_worker, &LibraryWorker::createAlias);
    connect(this, &Library::requestAddTagToPhotos, m_worker, &LibraryWorker::addTagToPhotos);
    connect(this, &Library::requestRemoveTagFromPhotos, m_worker, &LibraryWorker::removeTagFromPhotos);
    connect(m_worker, &LibraryWorker::progress, this, &Library::onWorkerProgress);
    connect(m_worker, &LibraryWorker::migrationFinished, this, &Library::onMigrationFinished);
    connect(m_worker, &LibraryWorker::importFinished, this, &Library::onImportFinished);
    connect(m_worker, &LibraryWorker::rescanFinished, this, &Library::onRescanFinished);
    connect(m_worker, &LibraryWorker::implicationsExpanded, this, &Library::onImplicationsExpanded);
    connect(m_worker, &LibraryWorker::aliasCreated, this, &Library::onAliasCreated);
    connect(m_worker, &LibraryWorker::tagJobFinished, this, &Library::onTagJobFinished);
    connect(this, &Library::requestSidecarFlush, m_worker, &LibraryWorker::flushSidecars);
    connect(m_worker, &LibraryWorker::sidecarFlushFinished, this, &Library::onSidecarFlushFinished);
    connect(this, &Library::requestFacetTags, m_worker, &LibraryWorker::loadFacetTags);
    connect(m_worker, &LibraryWorker::facetTagsReady, this, &Library::onFacetTagsReady);
    connect(this, &Library::requestResample, m_worker, &LibraryWorker::resampleHashes);
    connect(m_worker, &LibraryWorker::resampleFinished, this, &Library::onResampleFinished);
    m_workerThread.start();

    // Build the filter sidebar off-thread now that the worker is wired up, so the
    // first frame paints without waiting on it.
    refreshTagsAsync();
    updateSearchAliases(); // seed the photo model's alias-aware search blobs

    // Debounced auto-write: a burst of tag edits coalesces into one flush.
    m_sidecarDebounce = new QTimer(this);
    m_sidecarDebounce->setSingleShot(true);
    m_sidecarDebounce->setInterval(1500);
    connect(m_sidecarDebounce, &QTimer::timeout, this, [this] { emit requestSidecarFlush(); });

    // Kick off the one-time hash + capture-time backfill. Only show the progress
    // UI when there is actually a backlog to chew through.
    if (m_db.unhashedPhotoCount() > 0 || m_db.photosMissingTakenAtCount() > 0)
        beginBusy(tr("Preparing library…"));
    emit requestMigration();

    // Reconcile the library with disk on every launch, so files added, moved, or
    // deleted outside omapic since last time are picked up without a manual
    // rescan. It's queued behind the migration on the worker thread and reports
    // in the bottom status bar — non-blocking, exactly like a manual rescan. Skip
    // it when no folders are registered yet (nothing to scan).
    if (!m_db.loadFolders().isEmpty())
        rescan();

    // One full pass to re-fingerprint large files with the cheaper sampled hash.
    // Non-modal — it reports in the status bar and the library stays usable — and
    // queued behind the migration/rescan on the worker. Once every large file is
    // on the sampled scheme there are no candidates, so later launches skip it.
    if (m_db.hasResampleCandidates()) {
        beginBusy(tr("Optimizing media hashes…"), /*modal=*/false);
        emit requestResample();
    }
}

Library::~Library()
{
    m_workerThread.quit();
    m_workerThread.wait();
}

void Library::importDirectory(const QUrl &dir)
{
    const QString root = dir.toLocalFile();
    if (root.isEmpty())
        return;
    // Non-modal: the import runs on the worker thread, so report progress in the
    // status bar (like bulk tagging) and leave the library usable meanwhile,
    // rather than blocking behind the modal overlay.
    beginBusy(tr("Importing folder…"), /*modal=*/false);
    emit requestImport(root); // runs on the worker thread
}

void Library::beginBusy(const QString &status, bool modal)
{
    // Only the first job to start sets the initial label and an indeterminate
    // bar. If something is already running, leave its reporting untouched: the
    // newly queued job is serialized behind it on the worker, and its own
    // progress signals will take over the label once the worker reaches it —
    // which avoids briefly flashing the second job's label over the first.
    if (m_busyCount == 0) {
        m_statusText = status;
        emit statusChanged();
        m_progress = -1.0; // indeterminate until the first progress report
        emit progressChanged();
    }

    ++m_busyCount;
    if (modal)
        ++m_modalCount;
    updateBusyState();
}

void Library::endBusy(bool modal)
{
    if (m_busyCount == 0)
        return;
    --m_busyCount;
    if (modal && m_modalCount > 0)
        --m_modalCount;
    updateBusyState();
}

void Library::updateBusyState()
{
    const bool busy = m_busyCount > 0;
    const bool modal = m_modalCount > 0;
    if (busy == m_busy && modal == m_busyModal)
        return;
    m_busy = busy;
    m_busyModal = modal;
    emit busyChanged(); // also notifies busyModal
}

void Library::onWorkerProgress(const QString &phase, int done, int total)
{
    if (phase != m_statusText) {
        m_statusText = phase;
        emit statusChanged();
    }
    const double p = total > 0 ? double(done) / double(total) : -1.0;
    if (p != m_progress) {
        m_progress = p;
        emit progressChanged();
    }
}

void Library::onMigrationFinished(bool changed)
{
    // The backfill wrote hashes (affecting duplicate collapse) and capture times
    // (driving the timeline) on the worker's connection; reload so the in-memory
    // views reflect them. On a steady-state launch there is no backlog, so it
    // changed nothing — skip the reload, whose model reset would blink the
    // thumbnails that just painted.
    if (changed) {
        m_photos->setPhotos(m_db.loadPhotos());
        refreshTags();
    }
    endBusy(/*modal=*/true);
}

void Library::onImportFinished(const QString &root)
{
    Q_UNUSED(root)
    // The worker wrote the rows; refresh the in-memory views from the DB.
    m_photos->setPhotos(m_db.loadPhotos());
    refreshTags();
    endBusy(/*modal=*/false); // import reports in the status bar
    emit foldersChanged();
}

void Library::rescan()
{
    // Non-modal: runs on the worker thread, so report in the status bar and keep
    // the library usable (same as import), rather than blocking behind the overlay.
    beginBusy(tr("Scanning folders…"), /*modal=*/false);
    emit requestRescan(m_tagRetentionDays); // runs on the worker thread
}

void Library::onRescanFinished(bool changed)
{
    // Reload the in-memory models only when the rescan actually moved something
    // displayable (added/relocated/removed a row, re-keyed tags, imported an
    // external sidecar). A steady-state launch rescan changes nothing, and the
    // reload's model reset would tear down and recreate every delegate — blinking
    // all the thumbnails for no reason.
    if (changed) {
        m_photos->setPhotos(m_db.loadPhotos());
        refreshTags();
    }
    endBusy(/*modal=*/false); // rescan reports in the status bar
    emit foldersChanged(); // also refreshes the remembered-removed stat (see ctor)
    scheduleSidecarFlush(); // flush anything queued before the rescan, if auto-write is on
}

void Library::onResampleFinished(bool changed)
{
    // Re-fingerprinting only rewrote hash values and rekeyed the (hidden) tag
    // archive; a photo's live tags are untouched. It can shift duplicate grouping,
    // though, so reload the photo model when something actually changed (the
    // setPhotos guard no-ops it when the displayed set is identical).
    if (changed)
        m_photos->setPhotos(m_db.loadPhotos());
    endBusy(/*modal=*/false); // reported in the status bar
}

void Library::onImplicationsExpanded()
{
    // The worker materialized newly-implied tags across photo_tags/content_tags;
    // reload so the gallery, chips, and detail panel reflect them.
    m_photos->setPhotos(m_db.loadPhotos());
    refreshTags();
    endBusy(/*modal=*/false); // reported in the status bar
    emit implicationsChanged();
}

void Library::onAliasCreated()
{
    // The merge folded one tag into another across photo_tags/content_tags; reload
    // so the gallery, chips, and tag lists drop the merged-away tag.
    m_photos->setPhotos(m_db.loadPhotos());
    refreshTags();
    updateSearchAliases(); // the merged tag's alias is now searchable
    endBusy(/*modal=*/false); // reported in the status bar
    emit aliasesChanged();
    scheduleSidecarFlush(); // the merged photos' sidecars now use the canonical name
}

void Library::onTagJobFinished()
{
    // The worker changed photo_tags in bulk; reload the in-memory tags once.
    m_photos->setPhotos(m_db.loadPhotos());
    refreshTags(); // tagsChanged refreshes the detail panel in one pass
    endBusy(/*modal=*/false); // bulk tagging reports in the status bar
    scheduleSidecarFlush(); // the bulk tag change is committed; write sidecars if auto-write is on
}

void Library::setTagRetentionDays(int days)
{
    const int d = qMax(0, days);
    if (m_tagRetentionDays == d)
        return;
    m_tagRetentionDays = d;
    emit tagRetentionDaysChanged();
}

void Library::forgetRemovedTags()
{
    // Forget everything currently remembered (cutoff = now). One transaction, so
    // fast even for a large archive; safe on the GUI thread.
    m_db.pruneOrphanedTags(QDateTime::currentSecsSinceEpoch());
    emit rememberedRemovedCountChanged();
}

void Library::setAutoWriteSidecars(bool on)
{
    if (m_autoWriteSidecars == on)
        return;
    m_autoWriteSidecars = on;
    emit autoWriteSidecarsChanged();
    // Turning it on catches up: the dirty queue is fed on every edit regardless of
    // this setting, so it already holds everything changed (and unsynced) while it
    // was off. Flush that backlog now, with the progress bar — it can be large —
    // but only when something is actually queued, so enabling with nothing pending
    // (and the startup restore of a saved "on") stays quiet. Routine per-edit auto
    // writes still flush silently via scheduleSidecarFlush(). This shows the
    // progress bar but, unlike the on-demand export, reports no toast.
    if (on)
        catchUpSidecarsIfPending();
}

void Library::catchUpSidecarsIfPending()
{
    if (!m_autoWriteSidecars || m_db.sidecarDirtyCount() <= 0)
        return;
    ++m_quietBarFlushes;
    beginBusy(tr("Exporting tags…"), /*modal=*/false);
    emit requestSidecarFlush();
}

void Library::scheduleSidecarFlush()
{
    // Only the opt-in auto-write path debounces; explicit export flushes directly.
    if (m_autoWriteSidecars && m_sidecarDebounce)
        m_sidecarDebounce->start();
}

void Library::exportSidecars(const QVariantList &photoIds)
{
    QList<int> ids;
    ids.reserve(photoIds.size());
    for (const QVariant &v : photoIds) {
        const int pid = v.toInt();
        if (pid >= 0)
            ids.append(pid);
    }
    if (ids.isEmpty())
        return;
    m_db.enqueueSidecarDirty(ids);
    ++m_explicitFlushes; // user-triggered: show progress and report the result
    beginBusy(tr("Exporting tags…"), /*modal=*/false);
    emit requestSidecarFlush();
}

void Library::exportAllSidecars()
{
    // Only the active set (orphans + enabled folders): a disabled folder is left
    // untouched, matching what the gallery shows.
    const QList<int> ids = m_db.activePhotoIds();
    if (ids.isEmpty())
        return;
    m_db.enqueueSidecarDirty(ids);
    ++m_explicitFlushes;
    beginBusy(tr("Exporting tags…"), /*modal=*/false);
    emit requestSidecarFlush();
}

void Library::onSidecarFlushFinished(int written, int failed)
{
    // The import-first guard in the flush can add tags from an externally-changed
    // sidecar, so reload the models to reflect anything new.
    m_photos->setPhotos(m_db.loadPhotos());
    refreshTags();
    // An on-demand export shows the busy bar and reports a toast; the enable
    // catch-up shows the bar but stays quiet; the debounced per-edit auto-write
    // path shows nothing at all.
    if (m_explicitFlushes > 0) {
        --m_explicitFlushes;
        endBusy(/*modal=*/false);
        emit sidecarsExported(written, failed);
    } else if (m_quietBarFlushes > 0) {
        --m_quietBarFlushes;
        endBusy(/*modal=*/false);
    }
}

QVariantList Library::tagImplications() const
{
    QVariantList out;
    for (const ImplicationInfo &i : m_db.implications()) {
        out.append(QVariantMap{
            {QStringLiteral("antecedentId"), i.antecedentId},
            {QStringLiteral("antecedentName"), i.antecedentName},
            {QStringLiteral("antecedentColorSlot"), i.antecedentColorSlot},
            {QStringLiteral("antecedentCategoryName"), i.antecedentCategoryName},
            {QStringLiteral("consequentId"), i.consequentId},
            {QStringLiteral("consequentName"), i.consequentName},
            {QStringLiteral("consequentColorSlot"), i.consequentColorSlot},
            {QStringLiteral("consequentCategoryName"), i.consequentCategoryName},
        });
    }
    return out;
}

int Library::addTagImplication(int antecedentId, int consequentId)
{
    if (antecedentId < 0 || consequentId < 0 || antecedentId == consequentId)
        return ImplicationInvalid;
    if (m_db.implicationWouldCycle(antecedentId, consequentId))
        return ImplicationCycle;
    // Already defined? Don't kick off a pointless migration.
    for (const ImplicationInfo &i : m_db.implications()) {
        if (i.antecedentId == antecedentId && i.consequentId == consequentId)
            return ImplicationExists;
    }
    m_db.addImplication(antecedentId, consequentId);
    emit implicationsChanged();
    // Apply it to the existing library on the worker thread (status bar), so a
    // big library doesn't freeze the UI while the tags propagate.
    beginBusy(tr("Applying associations…"), /*modal=*/false);
    emit requestImplicationMigration();
    return ImplicationOk;
}

void Library::removeTagImplication(int antecedentId, int consequentId)
{
    // Only stops future propagation; tags it has already applied stay put.
    m_db.removeImplication(antecedentId, consequentId);
    emit implicationsChanged();
}

bool Library::implicationWouldCycle(int antecedentId, int consequentId) const
{
    if (antecedentId < 0 || consequentId < 0)
        return false;
    return m_db.implicationWouldCycle(antecedentId, consequentId);
}

int Library::ensureTag(const QString &name)
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty())
        return -1;
    // A tag with no memberships yet; the caller wires it into an association,
    // whose migration then applies it to photos (and refreshes the models).
    return m_db.ensureTag(trimmed);
}

bool Library::createTagAlias(const QString &name, int canonicalTagId)
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty() || canonicalTagId < 0)
        return false;
    // Mirror the database's cheap rejections so an invalid request never spins up a
    // worker job: a name already used by an alias, or the canonical tag's own name.
    if (m_db.aliasExists(trimmed))
        return false;
    const int existing = m_db.tagIdByName(trimmed); // raw: is the alias name itself a tag?
    if (existing == canonicalTagId)
        return false;
    if (existing >= 0) {
        // Folding an existing tag into the canonical one rewrites photo_tags and the
        // content archive — do it on the worker and reload the models when it's done.
        beginBusy(tr("Merging tags…"), /*modal=*/false);
        emit requestCreateAlias(trimmed, canonicalTagId);
        return true;
    }
    // No tag to merge: just record the alias row (no photo's tags change).
    const bool ok = m_db.createAlias(trimmed, canonicalTagId);
    if (ok) {
        emit aliasesChanged();
        updateSearchAliases(); // the new alias becomes searchable
    }
    return ok;
}

void Library::removeTagAlias(const QString &name)
{
    m_db.removeAlias(name.trimmed());
    emit aliasesChanged();
    updateSearchAliases(); // drop the removed alias from the search blobs
}

void Library::updateSearchAliases()
{
    QHash<int, QStringList> byTag;
    for (const AliasInfo &a : m_db.loadAliases())
        byTag[a.tagId].append(a.aliasName);
    m_photos->setTagAliases(byTag);
}

QVariantList Library::tagAliases() const
{
    QVariantList out;
    for (const AliasInfo &a : m_db.loadAliases()) {
        out.append(QVariantMap{
            {QStringLiteral("aliasName"), a.aliasName},
            {QStringLiteral("tagId"), a.tagId},
            {QStringLiteral("tagName"), a.tagName},
            {QStringLiteral("categoryName"), a.categoryName},
            {QStringLiteral("colorSlot"), a.colorSlot},
        });
    }
    return out;
}

QVariantList Library::folders() const
{
    QVariantList out;
    for (const FolderInfo &f : m_db.loadFolders()) {
        out.append(QVariantMap{
            {QStringLiteral("id"), f.id},
            {QStringLiteral("path"), f.path},
            {QStringLiteral("count"), f.count},
            {QStringLiteral("enabled"), f.enabled},
        });
    }
    return out;
}

void Library::updateFolderRoots()
{
    QStringList roots;
    for (const FolderInfo &f : m_db.loadFolders())
        roots.append(f.path);
    m_filtered->setFolderRoots(roots);
}

QVariantList Library::folderEntries(const QString &path) const
{
    QVariantList out;
    // When a tag filter is active, build the tiles from only the matching photos,
    // so the folder view shows only folders that contain the selected tags.
    const QList<int> selTags = m_tags->selectedTagIds();
    const bool filtering = !selTags.isEmpty();
    const QStringList paths = m_db.activePhotoPaths(selTags, m_matchAll);

    // A folder tile, with the count of active photos at or under `dir` and the
    // first (by path) up-to-4 of them for the cover mosaic.
    const auto makeFolder = [&](const QString &dir) -> QVariantMap {
        int count = 0;
        QStringList thumbs;
        const QString prefix = dir + QLatin1Char('/');
        for (const QString &p : paths) {
            if (p == dir || p.startsWith(prefix)) {
                ++count;
                if (thumbs.size() < 4)
                    thumbs.append(p);
            }
        }
        QString name = QFileInfo(dir).fileName();
        if (name.isEmpty())
            name = dir;
        return QVariantMap{
            {QStringLiteral("type"), QStringLiteral("folder")},
            {QStringLiteral("name"), name},
            {QStringLiteral("path"), dir},
            {QStringLiteral("count"), count},
            {QStringLiteral("thumbs"), thumbs},
        };
    };

    if (path.isEmpty()) {
        // Root level: one tile per enabled root library folder. (Orphan photos
        // show as photos in the grid here, not as tiles.) While filtering, hide a
        // root that holds no matching photo. (Subfolder tiles below are derived
        // from the matching paths, so they drop out on their own.)
        for (const FolderInfo &f : m_db.loadFolders()) {
            if (!f.enabled)
                continue;
            const QVariantMap tile = makeFolder(f.path);
            if (!filtering || tile.value(QStringLiteral("count")).toInt() > 0)
                out.append(tile);
        }
        return out;
    }

    // Inside a folder: the immediate subfolders that hold at least one active
    // photo. (Going up is handled by the folder view's title bar, not a cell.)
    const QString prefix = path + QLatin1Char('/');
    QStringList childDirs; // immediate subdirectories, in first-seen order
    QSet<QString> seen;
    for (const QString &p : paths) {
        if (!p.startsWith(prefix))
            continue;
        const QString rest = p.mid(prefix.size());
        const int slash = rest.indexOf(QLatin1Char('/'));
        if (slash < 0)
            continue; // a photo directly in `path`, not a subfolder
        const QString child = path + QLatin1Char('/') + rest.left(slash);
        if (!seen.contains(child)) {
            seen.insert(child);
            childDirs.append(child);
        }
    }
    childDirs.sort(Qt::CaseInsensitive);
    for (const QString &child : childDirs)
        out.append(makeFolder(child));

    return out;
}

QString Library::upFolder(const QString &path) const
{
    if (path.isEmpty())
        return {}; // already at the root listing
    for (const FolderInfo &f : m_db.loadFolders())
        if (f.path == path)
            return {}; // a registered root -> up returns to the root listing
    return path.left(path.lastIndexOf(QLatin1Char('/'))); // parent directory
}

QStringList Library::folderSlideshowSources(const QStringList &folderPaths,
                                            const QVariantList &photoIds) const
{
    QStringList out;
    const QStringList active = m_db.activePhotoPaths(); // sorted by path
    for (const QString &dir : folderPaths) {
        const QString prefix = dir + QLatin1Char('/');
        for (const QString &p : active) {
            if (p == dir || p.startsWith(prefix))
                out.append(QUrl::fromLocalFile(p).toString());
        }
    }
    for (const QVariant &v : photoIds) {
        const Photo ph = m_photos->photoById(v.toInt());
        if (ph.id >= 0)
            out.append(QUrl::fromLocalFile(ph.path).toString());
    }
    return out;
}

QStringList Library::albumSlideshowSources(const QVariantList &albumIds,
                                           const QVariantList &photoIds) const
{
    // Each selected album's membership query (a regular album is one Album leaf,
    // a smart album its expanded tree), resolved once up front.
    QVector<oql::Node> queries;
    for (const QVariant &v : albumIds)
        queries.append(albumTree(v.toInt()));

    // Active photos in any of the selected albums, newest first (the album view's
    // order), de-duped across albums by photo id.
    QVector<const Photo *> members;
    QSet<int> seen;
    for (const Photo &p : m_photos->photos()) {
        bool inAlbum = false;
        for (const oql::Node &q : queries)
            if (oql::matches(q, p.tagIds, m_evalContext)) { inAlbum = true; break; }
        if (inAlbum && !seen.contains(p.id)) {
            seen.insert(p.id);
            members.append(&p);
        }
    }
    std::sort(members.begin(), members.end(), [](const Photo *a, const Photo *b) {
        if (a->takenAt != b->takenAt)
            return a->takenAt > b->takenAt; // newest first
        return a->fileName.compare(b->fileName, Qt::CaseInsensitive) < 0;
    });

    QStringList out;
    for (const Photo *p : members)
        out.append(QUrl::fromLocalFile(p->path).toString());
    // Loose selected photos (mirrors the folder path; uncommon in album view).
    for (const QVariant &v : photoIds) {
        const Photo ph = m_photos->photoById(v.toInt());
        if (ph.id >= 0 && !seen.contains(ph.id))
            out.append(QUrl::fromLocalFile(ph.path).toString());
    }
    return out;
}

void Library::removeFolder(int folderId)
{
    const QString path = m_db.folderPath(folderId);
    if (path.isEmpty())
        return;

    m_db.deletePhotosUnderPath(path); // cascades photo_tags via foreign key
    m_db.removeFolder(folderId);

    m_photos->setPhotos(m_db.loadPhotos());
    refreshTags();
    emit foldersChanged();
}

void Library::setFolderEnabled(int folderId, bool enabled)
{
    m_db.setFolderEnabled(folderId, enabled);
    m_photos->setPhotos(m_db.loadPhotos()); // active set changed
    refreshTags();
    emit foldersChanged();
    // Re-enabling a folder makes its photos active again. Its sidecar writes that
    // were skipped (left queued) while it was disabled can now be written — so
    // just flush the dirty queue. We don't re-queue the whole folder: the queue is
    // the source of truth for what actually needs writing (seeded once at the v12
    // migration, kept current on every tag change), so nothing is written when
    // nothing changed.
    if (enabled)
        catchUpSidecarsIfPending();
}

int Library::orphanPhotoCount() const
{
    return m_db.orphanPhotoCount();
}

void Library::removeOrphanPhotos()
{
    m_db.deleteOrphanPhotos(); // content-tag archive is kept, so tags can return
    m_photos->setPhotos(m_db.loadPhotos());
    refreshTags();
    emit foldersChanged();
}

void Library::removeFromLibrary(const QVariantList &photoIds)
{
    bool any = false;
    m_db.beginTransaction();
    for (const QVariant &v : photoIds) {
        const int photoId = v.toInt();
        if (photoId < 0)
            continue;
        m_db.deletePhoto(photoId); // content_tags archive (keyed by hash) survives
        any = true;
    }
    m_db.commitTransaction();
    if (!any)
        return;
    m_photos->setPhotos(m_db.loadPhotos());
    refreshTags();
    emit foldersChanged(); // folder tallies / orphan count may change
}

int Library::deleteFromDisk(const QVariantList &photoIds)
{
    int failed = 0;
    bool any = false;
    m_db.beginTransaction();
    for (const QVariant &v : photoIds) {
        const int photoId = v.toInt();
        if (photoId < 0)
            continue;
        const Photo p = m_photos->photoById(photoId);
        if (p.id < 0)
            continue;

        // Trash rather than unlink: still "gone from disk" from the library's
        // view, but recoverable from the file manager. If trashing isn't
        // supported, the caller is told (we keep the row) instead of silently
        // hard-deleting.
        QFile f(p.path);
        if (f.exists() && !f.moveToTrash()) {
            ++failed;
            continue;
        }
        m_db.deletePhoto(photoId);
        any = true;
    }
    m_db.commitTransaction();

    if (any) {
        m_photos->setPhotos(m_db.loadPhotos());
        refreshTags();
        emit foldersChanged();
    }
    return failed;
}

bool Library::detectOmarchy()
{
    // Same state dir OmarchyTheme/currentOmarchyTheme() read from; its presence
    // marks an Omarchy session.
    return QFileInfo::exists(QDir::homePath()
                             + QStringLiteral("/.local/state/omarchy/current"));
}

bool Library::detectAether()
{
    return !QStandardPaths::findExecutable(QStringLiteral("aether")).isEmpty();
}

Library::WallpaperBackend Library::detectWallpaperBackend(bool omarchyAvailable)
{
    // Omarchy's own setter is the right integration where present — it keeps the
    // current/background symlink and notifies the shell, exactly as Omarchy does.
    if (omarchyAvailable
        && !QStandardPaths::findExecutable(QStringLiteral("omarchy-theme-bg-set")).isEmpty())
        return WallpaperBackend::Omarchy;

    // The freedesktop wallpaper portal (GNOME, KDE, some wlroots backends). A
    // readable `version` property means a backend is actually wired up.
    QDBusInterface portal(QStringLiteral("org.freedesktop.portal.Desktop"),
                          QStringLiteral("/org/freedesktop/portal/desktop"),
                          QStringLiteral("org.freedesktop.portal.Wallpaper"),
                          QDBusConnection::sessionBus());
    if (portal.isValid() && portal.property("version").isValid())
        return WallpaperBackend::Portal;

    // Bare Wayland compositors.
    if (!QStandardPaths::findExecutable(QStringLiteral("swww")).isEmpty())
        return WallpaperBackend::Swww;
    if (!QStandardPaths::findExecutable(QStringLiteral("swaybg")).isEmpty())
        return WallpaperBackend::Swaybg;

    return WallpaperBackend::None;
}

bool Library::addToThemeWallpapers(int photoId)
{
    const Photo p = m_photos->photoById(photoId);
    return p.id < 0 ? false : addPathToThemeWallpapers(p.path);
}

bool Library::addToThemeWallpapersUrl(const QUrl &source)
{
    const QString path = source.toLocalFile();
    return path.isEmpty() ? false : addPathToThemeWallpapers(path);
}

bool Library::addPathToThemeWallpapers(const QString &path)
{
    // Goes into the *user* backgrounds folder for the active theme, keyed by its
    // slug, so the wallpaper shows up only while that theme is active.
    const QString theme = currentOmarchyTheme();
    if (theme.isEmpty())
        return false;
    return copyIntoDir(path, QDir::homePath()
                                 + QStringLiteral("/.config/omarchy/backgrounds/") + theme);
}

bool Library::editThemeInAether(int photoId)
{
    const Photo p = m_photos->photoById(photoId);
    return p.id < 0 ? false : editThemeInAetherPath(p.path);
}

bool Library::editThemeInAetherUrl(const QUrl &source)
{
    const QString path = source.toLocalFile();
    return path.isEmpty() ? false : editThemeInAetherPath(path);
}

bool Library::editThemeInAetherPath(const QString &path)
{
    // The wrapper script below starts a detached shell, so it reports success
    // even when `aether` is missing. Fail honestly up front when it isn't here.
    if (!m_aetherAvailable)
        return false;
    // Load the photo into Aether's GUI editor so a new theme can be tweaked
    // before applying — deliberately not `--generate`, which would apply a theme
    // immediately (that's setAetherTheme). `aether extract` talks to a running
    // GUI over its control socket, so: start the GUI if it isn't up, then retry
    // `extract` until the socket is ready. Run in a detached shell so the wait
    // never blocks omapic's UI thread.
    static const QString script = QStringLiteral(
        "sock=\"$HOME/.config/aether/aether.sock\"\n"
        "if [ ! -S \"$sock\" ]; then setsid aether >/dev/null 2>&1 & fi\n"
        "for i in $(seq 1 100); do\n"
        "  aether extract \"$1\" >/dev/null 2>&1 && exit 0\n"
        "  sleep 0.2\n"
        "done\n"
        "exit 1\n");
    return QProcess::startDetached(QStringLiteral("sh"),
                                   {QStringLiteral("-c"), script,
                                    QStringLiteral("sh"), path});
}

bool Library::setAetherTheme(int photoId)
{
    const Photo p = m_photos->photoById(photoId);
    return p.id < 0 ? false : setAetherThemePath(p.path);
}

bool Library::setAetherThemeUrl(const QUrl &source)
{
    const QString path = source.toLocalFile();
    return path.isEmpty() ? false : setAetherThemePath(path);
}

bool Library::setAetherThemePath(const QString &path)
{
    // Extract colors from the photo and apply the generated theme right away.
    return QProcess::startDetached(QStringLiteral("aether"),
                                   {QStringLiteral("--generate"), path});
}

bool Library::setAsWallpaper(int photoId)
{
    const Photo p = m_photos->photoById(photoId);
    return p.id < 0 ? false : setPathAsWallpaper(p.path);
}

bool Library::setAsWallpaperUrl(const QUrl &source)
{
    const QString path = source.toLocalFile();
    return path.isEmpty() ? false : setPathAsWallpaper(path);
}

bool Library::setPathAsWallpaper(const QString &path)
{
    // Dispatch to the backend resolved at startup (see detectWallpaperBackend).
    switch (m_wallpaperBackend) {
    case WallpaperBackend::Omarchy:
        return QProcess::startDetached(QStringLiteral("omarchy-theme-bg-set"), {path});
    case WallpaperBackend::Portal: {
        // org.freedesktop.portal.Wallpaper.SetWallpaperURI(parent, uri, options).
        const QString uri = QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded);
        QDBusInterface portal(QStringLiteral("org.freedesktop.portal.Desktop"),
                              QStringLiteral("/org/freedesktop/portal/desktop"),
                              QStringLiteral("org.freedesktop.portal.Wallpaper"),
                              QDBusConnection::sessionBus());
        const QDBusMessage reply = portal.call(QStringLiteral("SetWallpaperURI"),
                                               QString(), uri, QVariantMap());
        return reply.type() != QDBusMessage::ErrorMessage;
    }
    case WallpaperBackend::Swww:
        return QProcess::startDetached(QStringLiteral("swww"),
                                       {QStringLiteral("img"), path});
    case WallpaperBackend::Swaybg:
        return QProcess::startDetached(QStringLiteral("swaybg"),
                                       {QStringLiteral("-i"), path});
    case WallpaperBackend::None:
        break;
    }
    return false;
}

bool Library::showInFileManager(int photoId)
{
    const Photo p = m_photos->photoById(photoId);
    return p.id < 0 ? false : showPathInFileManager(p.path);
}

bool Library::showPathInFileManager(const QString &path)
{
    // Prefer the freedesktop FileManager1 D-Bus interface: it opens the
    // containing folder with the file itself selected, which spawning a specific
    // manager doesn't reliably do. Nautilus (Omarchy's default), Dolphin, Thunar,
    // Nemo, Caja and PCManFM-Qt all implement it. The URL is fully percent-encoded
    // so spaces and other special characters survive as a single D-Bus argument.
    const QString uri = QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded);
    QDBusInterface fm(QStringLiteral("org.freedesktop.FileManager1"),
                      QStringLiteral("/org/freedesktop/FileManager1"),
                      QStringLiteral("org.freedesktop.FileManager1"),
                      QDBusConnection::sessionBus());
    fm.setTimeout(2000); // don't hang the UI thread if nothing answers
    const QDBusMessage reply = fm.call(QStringLiteral("ShowItems"),
                                       QStringList{uri}, QString());
    if (reply.type() != QDBusMessage::ErrorMessage)
        return true;

    // No FileManager1 provider (bare WM, TUI manager) or the call failed: fall
    // back to opening the containing folder with the desktop's default handler.
    // The file won't be selected, but something opens.
    const QString dir = QFileInfo(path).absolutePath();
    return QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

void Library::addTag(int photoId, const QString &name)
{
    applyTagCore(photoId, name);
}

TagApplyRecord Library::applyTagCore(int photoId, const QString &name)
{
    const QString trimmed = name.trimmed();
    if (photoId < 0 || trimmed.isEmpty())
        return {};
    const int tagId = m_db.ensureTag(trimmed);
    if (tagId < 0)
        return {};
    // Links every copy of this content (not just the visible one), archives the
    // tag, and applies any tags this one implies — recording what was new.
    const TagApplyRecord rec = m_db.applyTagRecorded(photoId, tagId);
    m_db.enqueueSidecarDirty({photoId});
    applyTags(photoId);
    refreshTags();
    emit photoChanged(photoId);
    scheduleSidecarFlush();
    return rec;
}

QVariantMap Library::addTagRecorded(int photoId, const QString &name)
{
    const TagApplyRecord rec = applyTagCore(photoId, name);
    if (rec.tagId < 0)
        return {};
    QVariantList links;
    for (const QPair<int, int> &l : rec.photoLinks)
        links.append(QVariantList{l.first, l.second});
    QVariantList content;
    for (int t : rec.contentLinks)
        content.append(t);
    return QVariantMap{
        {QStringLiteral("photoId"), rec.photoId},
        {QStringLiteral("tagId"), rec.tagId},
        {QStringLiteral("tagName"), m_db.tagName(rec.tagId)},
        {QStringLiteral("hash"), rec.hash},
        {QStringLiteral("photoLinks"), links},
        {QStringLiteral("contentLinks"), content},
        {QStringLiteral("changed"), rec.changedAnything()},
    };
}

void Library::revertTagApply(const QVariantMap &record)
{
    TagApplyRecord rec;
    rec.photoId = record.value(QStringLiteral("photoId"), -1).toInt();
    rec.tagId = record.value(QStringLiteral("tagId"), -1).toInt();
    rec.hash = record.value(QStringLiteral("hash")).toString();
    for (const QVariant &v : record.value(QStringLiteral("photoLinks")).toList()) {
        const QVariantList pair = v.toList();
        if (pair.size() == 2)
            rec.photoLinks.append(qMakePair(pair.at(0).toInt(), pair.at(1).toInt()));
    }
    for (const QVariant &v : record.value(QStringLiteral("contentLinks")).toList())
        rec.contentLinks.append(v.toInt());
    if (rec.photoId < 0 || !rec.changedAnything())
        return;

    m_db.revertTagApply(rec);
    // Every photo the apply touched (duplicates of the same content included)
    // gets its model row and sidecar refreshed.
    QList<int> touched{rec.photoId};
    for (const QPair<int, int> &l : rec.photoLinks)
        if (!touched.contains(l.first))
            touched.append(l.first);
    m_db.enqueueSidecarDirty(touched);
    for (int pid : touched) {
        applyTags(pid);
        emit photoChanged(pid);
    }
    refreshTags();
    scheduleSidecarFlush();
}

void Library::relinkTag(int photoId, int tagId)
{
    if (photoId < 0 || tagId < 0)
        return;
    m_db.linkTagToContent(photoId, tagId); // every copy of this content
    m_db.rememberContentTag(m_db.photoHash(photoId), tagId);
    m_db.enqueueSidecarDirty({photoId});
    applyTags(photoId);
    refreshTags();
    emit photoChanged(photoId);
    scheduleSidecarFlush();
}

bool Library::isFavorite(int photoId) const
{
    return m_photos->photoById(photoId).favorite;
}

QVariantMap Library::trashPhoto(int photoId)
{
    const Photo p = m_photos->photoById(photoId);
    if (p.id < 0)
        return {};
    // Snapshot the row before it goes, so the undo can re-insert it as it was.
    const PhotoRowSnapshot row = m_db.photoRow(photoId);

    // Trash rather than unlink, exactly as deleteFromDisk: recoverable from the
    // file manager — and from here. QFile reports where the file landed.
    QString trashPath;
    QFile f(p.path);
    if (f.exists()) {
        if (!f.moveToTrash())
            return QVariantMap{{QStringLiteral("ok"), false}};
        trashPath = f.fileName();
    }
    m_db.deletePhoto(photoId); // the content_tags archive (keyed by hash) survives
    m_photos->setPhotos(m_db.loadPhotos());
    refreshTags();
    emit foldersChanged();
    return QVariantMap{
        {QStringLiteral("ok"), true},
        {QStringLiteral("id"), photoId},
        {QStringLiteral("fileName"), p.fileName},
        {QStringLiteral("path"), row.path},
        {QStringLiteral("trashPath"), trashPath},
        {QStringLiteral("hash"), row.hash},
        {QStringLiteral("takenAt"), row.takenAt},
        {QStringLiteral("mtime"), row.mtime},
        {QStringLiteral("size"), row.size},
    };
}

int Library::restoreTrashedPhoto(const QVariantMap &record)
{
    PhotoRowSnapshot row;
    row.path = record.value(QStringLiteral("path")).toString();
    row.hash = record.value(QStringLiteral("hash")).toString();
    row.takenAt = record.value(QStringLiteral("takenAt")).toLongLong();
    row.mtime = record.value(QStringLiteral("mtime")).toLongLong();
    row.size = record.value(QStringLiteral("size")).toLongLong();
    const QString trashPath = record.value(QStringLiteral("trashPath")).toString();
    if (row.path.isEmpty())
        return -1;

    // Put the file back first; a row without its file would just vanish again on
    // the next rescan. (An empty trashPath means the file was already gone when
    // it was trashed — nothing to move, the row alone comes back.)
    if (!trashPath.isEmpty()) {
        if (!QFile::exists(trashPath) || QFile::exists(row.path))
            return -1; // emptied from the Trash, or something now sits at the old path
        if (!QFile::rename(trashPath, row.path))
            return -1;
        // Best effort: drop the freedesktop Trash's companion record
        // (<Trash>/info/<name>.trashinfo) so the file manager's Trash view doesn't
        // list a ghost. Layouts that don't match are simply left alone.
        const QFileInfo ti(trashPath);
        QDir filesDir = ti.dir();
        if (filesDir.dirName() == QLatin1String("files") && filesDir.cdUp())
            QFile::remove(filesDir.filePath(QStringLiteral("info/") + ti.fileName()
                                            + QStringLiteral(".trashinfo")));
    }

    const int id = m_db.restorePhotoRow(row); // new id; tags reattach via the archive
    if (id < 0)
        return -1;
    m_db.enqueueSidecarDirty({id});
    m_photos->setPhotos(m_db.loadPhotos());
    refreshTags();
    emit foldersChanged();
    return id;
}

void Library::removeTag(int photoId, int tagId)
{
    if (photoId < 0 || tagId < 0)
        return;
    m_db.unlinkTagFromContent(photoId, tagId); // every copy of this content
    m_db.forgetContentTag(m_db.photoHash(photoId), tagId);
    m_db.enqueueSidecarDirty({photoId});
    applyTags(photoId);
    refreshTags();
    emit photoChanged(photoId);
    scheduleSidecarFlush();
}

void Library::addTagToPhotos(const QVariantList &photoIds, const QString &name)
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty() || photoIds.isEmpty())
        return;

    QList<int> ids;
    ids.reserve(photoIds.size());
    for (const QVariant &v : photoIds) {
        const int pid = v.toInt();
        if (pid >= 0)
            ids.append(pid);
    }
    if (ids.isEmpty())
        return;

    m_db.enqueueSidecarDirty(ids); // queue the sidecar rewrite regardless of path

    // Large sets: do the writes on the worker thread with a progress bar so the
    // UI stays responsive. onTagJobFinished reloads the models once.
    if (ids.size() > kBulkTagThreshold) {
        beginBusy(tr("Applying tag…"), /*modal=*/false);
        emit requestAddTagToPhotos(ids, trimmed);
        return;
    }

    const int tagId = m_db.ensureTag(trimmed);
    if (tagId < 0)
        return;
    m_db.beginTransaction();
    for (int pid : ids) {
        const QString hash = m_db.photoHash(pid);
        m_db.linkTagToContent(pid, tagId); // every copy of this content
        m_db.rememberContentTag(hash, tagId);
        m_db.applyImplications(hash, tagId); // and any tags this one implies
        applyTags(pid); // in-place model update; detail refreshes once via refreshTags
    }
    m_db.commitTransaction();
    refreshTags();
    scheduleSidecarFlush();
}

void Library::removeTagFromPhotos(const QVariantList &photoIds, int tagId)
{
    if (tagId < 0 || photoIds.isEmpty())
        return;

    QList<int> ids;
    ids.reserve(photoIds.size());
    for (const QVariant &v : photoIds) {
        const int pid = v.toInt();
        if (pid >= 0)
            ids.append(pid);
    }
    if (ids.isEmpty())
        return;

    m_db.enqueueSidecarDirty(ids);

    if (ids.size() > kBulkTagThreshold) {
        beginBusy(tr("Removing tag…"), /*modal=*/false);
        emit requestRemoveTagFromPhotos(ids, tagId);
        return;
    }

    m_db.beginTransaction();
    for (int pid : ids) {
        m_db.unlinkTagFromContent(pid, tagId); // every copy of this content
        m_db.forgetContentTag(m_db.photoHash(pid), tagId);
        applyTags(pid);
    }
    m_db.commitTransaction();
    refreshTags();
    scheduleSidecarFlush();
}

QVariantList Library::albums() const
{
    const QVector<AlbumInfo> metas = m_db.albumTags();        // Favorites first, then name
    const QVector<SmartAlbumInfo> smart = m_db.smartAlbums(); // by name

    // Count members (and gather up-to-4 cover thumbs) per album over the active
    // photo set, honoring the current tag filter — so album tiles narrow exactly
    // like folder tiles. A regular album's membership is a plain tagIds test; a
    // smart album's is its expanded query tree. Both are keyed in `acc` by the
    // UI-facing album id (smart albums use the reserved high id range).
    const QList<int> sel = m_tags->selectedTagIds();
    const bool filtering = !sel.isEmpty();
    const oql::Node selQuery = oql::fromTagSet(sel, m_matchAll);

    struct Acc { int count = 0; QStringList thumbs; };
    QHash<int, Acc> acc;
    auto tally = [&acc](int uiId, const Photo &p) {
        Acc &x = acc[uiId];
        ++x.count;
        if (x.thumbs.size() < 4)
            x.thumbs.append(p.path);
    };
    for (const Photo &p : m_photos->photos()) {
        if (filtering && !oql::matches(selQuery, p.tagIds, m_evalContext))
            continue;
        for (const AlbumInfo &a : metas)
            if (p.tagIds.contains(a.id))
                tally(a.id, p);
        for (const SmartAlbumInfo &sa : smart) {
            const auto tree = m_smartTrees.constFind(sa.id);
            if (tree != m_smartTrees.constEnd() && oql::matches(tree.value(), p.tagIds, m_evalContext))
                tally(kSmartAlbumIdBase + sa.id, p);
        }
    }

    QVariantList out;
    for (const AlbumInfo &a : metas) {
        const Acc x = acc.value(a.id);
        // Hide empty albums, like empty tags/categories. Favorites stays visible
        // when not filtering (a permanent destination), even with no photos yet.
        if (x.count == 0 && !(a.favorite && !filtering))
            continue;
        out.append(QVariantMap{
            {QStringLiteral("type"), QStringLiteral("album")},
            {QStringLiteral("id"), a.id},
            {QStringLiteral("name"), a.name},
            {QStringLiteral("favorite"), a.favorite},
            {QStringLiteral("smart"), false},
            {QStringLiteral("count"), x.count},
            {QStringLiteral("thumbs"), x.thumbs},
        });
    }
    // Smart album tiles after the regular ones (by name). Hidden when empty, like
    // everything else. They carry their query text for the tile's tooltip (the
    // vocabulary it is formatted against is loaded only when a tile needs it).
    oql::MapVocabulary vocab;
    bool vocabLoaded = false;
    for (const SmartAlbumInfo &sa : smart) {
        const Acc x = acc.value(kSmartAlbumIdBase + sa.id);
        if (x.count == 0)
            continue;
        if (!vocabLoaded) {
            m_db.fillVocabulary(vocab);
            vocabLoaded = true;
        }
        out.append(QVariantMap{
            {QStringLiteral("type"), QStringLiteral("album")},
            {QStringLiteral("id"), kSmartAlbumIdBase + sa.id},
            {QStringLiteral("name"), sa.name},
            {QStringLiteral("favorite"), false},
            {QStringLiteral("smart"), true},
            {QStringLiteral("count"), x.count},
            {QStringLiteral("thumbs"), x.thumbs},
            {QStringLiteral("query"), oql::format(sa.query, vocab)},
        });
    }
    return out;
}

QVariantList Library::allAlbums() const
{
    QVariantList out;
    for (const AlbumInfo &a : m_db.albumTags()) {
        out.append(QVariantMap{
            {QStringLiteral("id"), a.id},
            {QStringLiteral("name"), a.name},
            {QStringLiteral("favorite"), a.favorite},
        });
    }
    return out;
}

int Library::favoriteAlbumId() const
{
    return m_db.favoritesTagId();
}

int Library::createAlbumWithPhotos(const QString &name, const QVariantList &photoIds)
{
    const int id = m_db.createAlbum(name);
    if (id < 0)
        return -1;
    updateAlbumTagIds();
    if (!photoIds.isEmpty())
        addPhotosToAlbum(photoIds, id); // adds + refreshes (emits albumsChanged)
    else
        emit albumsChanged();
    return id;
}

void Library::addPhotosToAlbum(const QVariantList &photoIds, int albumId)
{
    if (albumId < 0 || photoIds.isEmpty())
        return;
    const QString name = m_db.tagName(albumId);
    if (name.isEmpty())
        return;
    // Reuse the by-name tag-apply path: it handles the bulk-worker threshold,
    // content_tags archival (so album membership survives moves/re-imports), the
    // in-place model update, and the refresh. ensureTag re-resolves the same
    // existing album tag (names are unique), leaving its category intact.
    addTagToPhotos(photoIds, name);
}

void Library::removePhotosFromAlbum(const QVariantList &photoIds, int albumId)
{
    if (albumId < 0 || photoIds.isEmpty())
        return;
    removeTagFromPhotos(photoIds, albumId); // by id; handles bulk + refresh
}

QVariantList Library::commonAlbumIds(const QVariantList &photoIds) const
{
    QVariantList out;
    if (photoIds.isEmpty())
        return out;
    const QList<int> albumList = m_db.albumTagIds();
    const QSet<int> albumSet(albumList.begin(), albumList.end());

    QSet<int> common;
    bool first = true;
    for (const QVariant &v : photoIds) {
        const Photo p = m_photos->photoById(v.toInt());
        if (p.id < 0)
            continue;
        QSet<int> photoAlbums;
        for (int t : p.tagIds)
            if (albumSet.contains(t))
                photoAlbums.insert(t);
        if (first) { common = photoAlbums; first = false; }
        else common.intersect(photoAlbums);
        if (common.isEmpty())
            break;
    }
    for (int id : common)
        out.append(id);
    return out;
}

void Library::toggleFavorite(int photoId)
{
    if (photoId < 0)
        return;
    const Photo p = m_photos->photoById(photoId);
    const bool isFav = p.tagIds.contains(m_db.favoritesTagId());
    setFavorite(QVariantList{photoId}, !isFav);
}

void Library::setFavorite(const QVariantList &photoIds, bool favorite)
{
    const int favId = m_db.favoritesTagId();
    if (favId < 0 || photoIds.isEmpty())
        return;
    if (favorite)
        addPhotosToAlbum(photoIds, favId);
    else
        removeTagFromPhotos(photoIds, favId);
}

void Library::updateAlbumTagIds()
{
    m_filtered->setAlbumTagIds(m_db.albumTagIds());
}

bool Library::isSmartAlbum(int albumId) const
{
    return albumId >= kSmartAlbumIdBase;
}

oql::Node Library::albumTree(int albumId) const
{
    if (albumId >= kSmartAlbumIdBase) {
        const auto it = m_smartTrees.constFind(albumId - kSmartAlbumIdBase);
        return it == m_smartTrees.constEnd() ? oql::Node::andOf({}) : it.value();
    }
    if (albumId > 0)
        return oql::Node::album(albumId); // a regular album is a one-term query
    return oql::Node::andOf({});
}

oql::MapVocabulary Library::vocabulary() const
{
    oql::MapVocabulary vocab;
    m_db.fillVocabulary(vocab);
    return vocab;
}

void Library::rebuildQueryState()
{
    m_evalContext = m_db.evalContext();
    const QHash<int, oql::Node> stored = m_db.smartAlbumQueries();
    const oql::SmartAlbumLookup lookup = [&stored](int id) -> const oql::Node * {
        const auto it = stored.constFind(id);
        return it == stored.constEnd() ? nullptr : &it.value();
    };
    m_smartTrees.clear();
    for (auto it = stored.constBegin(); it != stored.constEnd(); ++it) {
        QStringList errors;
        m_smartTrees.insert(it.key(), oql::expand(it.value(), lookup, &errors, it.key()));
        for (const QString &e : errors)
            qWarning("omapic: %s", qPrintable(e));
    }
    m_filtered->setEvalContext(m_evalContext);
    m_filtered->refreshAlbumQuery();
}

QVariantMap Library::smartAlbum(int albumId) const
{
    const SmartAlbumInfo sa = m_db.smartAlbum(albumId - kSmartAlbumIdBase);
    QList<int> setIds;
    bool matchAll = true;
    const bool simple = oql::asTagSet(sa.query, setIds, matchAll);
    QVariantList tags;
    for (int t : setIds)
        tags.append(t);
    return QVariantMap{
        {QStringLiteral("id"), albumId},
        {QStringLiteral("name"), sa.name},
        {QStringLiteral("text"), oql::format(sa.query, vocabulary())},
        {QStringLiteral("simple"), simple},
        {QStringLiteral("tags"), tags},
        {QStringLiteral("matchAll"), matchAll},
    };
}

QVariantMap Library::parseQuery(const QString &text) const
{
    const oql::MapVocabulary vocab = vocabulary();
    const oql::ParseResult r = oql::parse(text, vocab);
    if (!r.ok()) {
        return QVariantMap{
            {QStringLiteral("ok"), false},
            {QStringLiteral("message"), r.error.message},
            {QStringLiteral("start"), r.error.span.start},
            {QStringLiteral("length"), r.error.span.length},
            {QStringLiteral("suggestions"), r.error.suggestions},
        };
    }
    QList<int> setIds;
    bool matchAll = true;
    const bool simple = oql::asTagSet(r.root, setIds, matchAll);
    QVariantList tags;
    for (int t : setIds)
        tags.append(t);
    QStringList notes;
    for (const oql::Note &n : r.notes)
        notes.append(n.message);
    return QVariantMap{
        {QStringLiteral("ok"), true},
        {QStringLiteral("canonical"), oql::format(r.root, vocab)},
        {QStringLiteral("simple"), simple},
        {QStringLiteral("tags"), tags},
        {QStringLiteral("matchAll"), matchAll},
        {QStringLiteral("notes"), notes},
    };
}

QString Library::formatTagQuery(const QVariantList &tagIds, bool matchAll) const
{
    QList<int> ids;
    for (const QVariant &v : tagIds)
        ids.append(v.toInt());
    if (ids.isEmpty())
        return {};
    const oql::MapVocabulary vocab = vocabulary();
    return oql::format(oql::fromTagSet(ids, matchAll, &vocab), vocab);
}

QString Library::saveSmartAlbumQuery(int albumId, const QString &name, const QString &text)
{
    const oql::ParseResult r = oql::parse(text, vocabulary());
    if (!r.ok())
        return r.error.message;
    if (name.trimmed().isEmpty())
        return tr("The smart album needs a name");
    if (albumId >= kSmartAlbumIdBase) {
        const int id = albumId - kSmartAlbumIdBase;
        if (m_db.smartAlbumWouldCycle(id, r.root))
            return tr("That query would make the album refer to itself through album: references");
        if (!m_db.updateSmartAlbum(id, name, r.root))
            return tr("Another smart album already uses that name");
    } else {
        if (m_db.createSmartAlbum(name, r.root) < 0)
            return tr("A smart album already uses that name");
    }
    rebuildQueryState();
    emit albumsChanged();
    return {};
}

int Library::createSmartAlbum(const QString &name, const QVariantList &tagIds, bool matchAll)
{
    QList<int> tags;
    for (const QVariant &v : tagIds)
        tags.append(v.toInt());
    const int id = m_db.createSmartAlbum(name, tags, matchAll);
    if (id < 0)
        return -1;
    rebuildQueryState();
    emit albumsChanged();
    return kSmartAlbumIdBase + id;
}

bool Library::updateSmartAlbum(int albumId, const QString &name,
                               const QVariantList &tagIds, bool matchAll)
{
    QList<int> tags;
    for (const QVariant &v : tagIds)
        tags.append(v.toInt());
    if (!m_db.updateSmartAlbum(albumId - kSmartAlbumIdBase, name, tags, matchAll))
        return false;
    rebuildQueryState();
    emit albumsChanged();
    return true;
}

void Library::deleteSmartAlbum(int albumId)
{
    m_db.deleteSmartAlbum(albumId - kSmartAlbumIdBase);
    rebuildQueryState();
    emit albumsChanged();
}

void Library::deleteAlbum(int albumId)
{
    if (albumId < 0 || albumId == m_db.favoritesTagId())
        return; // Favorites is a permanent, reserved album
    deleteTag(albumId);   // drops the hidden tag + memberships; refreshTags emits albumsChanged
    updateAlbumTagIds();  // keep the flattened-search album set in step
}

QVariantList Library::commonTags(const QVariantList &photoIds) const
{
    QHash<int, int> counts;
    QHash<int, TagInfo> infos;
    for (const QVariant &v : photoIds) {
        const int pid = v.toInt();
        if (pid < 0)
            continue;
        for (const TagInfo &t : m_db.photoTags(pid)) {
            counts[t.id] += 1;
            infos[t.id] = t;
        }
    }

    QList<int> ids = counts.keys();
    std::sort(ids.begin(), ids.end(), [&](int a, int b) {
        return infos.value(a).name.compare(infos.value(b).name, Qt::CaseInsensitive) < 0;
    });

    QVariantList out;
    for (int id : ids) {
        const TagInfo &t = infos[id];
        out.append(QVariantMap{
            {QStringLiteral("id"), id},
            {QStringLiteral("name"), t.name},
            {QStringLiteral("count"), counts.value(id)},
            {QStringLiteral("categoryId"), t.categoryId},
            {QStringLiteral("categoryName"), t.categoryName},
            {QStringLiteral("colorSlot"), t.colorSlot},
        });
    }
    return out;
}

QVariantMap Library::photoInfo(int photoId) const
{
    QVariantMap info;
    if (photoId < 0)
        return info;

    const Photo p = m_photos->photoById(photoId);
    if (p.id < 0)
        return info;

    QVariantList tags;
    for (const TagInfo &t : m_db.photoTags(photoId)) {
        tags.append(QVariantMap{
            {QStringLiteral("id"), t.id},
            {QStringLiteral("name"), t.name},
            {QStringLiteral("categoryId"), t.categoryId},
            {QStringLiteral("categoryName"), t.categoryName},
            {QStringLiteral("colorSlot"), t.colorSlot},
        });
    }

    info[QStringLiteral("id")] = p.id;
    info[QStringLiteral("fileName")] = p.fileName;
    info[QStringLiteral("path")] = p.path;
    info[QStringLiteral("url")] = QUrl::fromLocalFile(p.path).toString();
    info[QStringLiteral("isVideo")] = p.isVideo;
    info[QStringLiteral("favorite")] = p.favorite;
    info[QStringLiteral("tags")] = tags;

    // Dimensions and a few EXIF fields, read from the file on demand (empty
    // strings where unavailable, so the pane hides those rows).
    const PhotoMetadata meta = readPhotoMetadata(p.path, p.isVideo);
    info[QStringLiteral("dimensions")] = meta.dimensions;
    info[QStringLiteral("size")] = meta.size;
    info[QStringLiteral("taken")] = meta.taken;
    info[QStringLiteral("camera")] = meta.camera;
    info[QStringLiteral("duration")] = meta.duration;
    info[QStringLiteral("modified")] = meta.modified;
    return info;
}

QStringList Library::allTagNames() const
{
    // Only tags relevant to the active (shown) set, so autocomplete matches the
    // filter sidebar.
    return m_db.allTagNames();
}

QVariantList Library::allTags() const
{
    // Tags shown in the tag manager: those with at least one photo in an active
    // folder (hidden-folder-only and removed-folder-only tags are left out).
    QVariantList out;
    for (const TagInfo &t : m_db.loadTags()) {
        out.append(QVariantMap{
            {QStringLiteral("id"), t.id},
            {QStringLiteral("name"), t.name},
            {QStringLiteral("count"), t.count},
            {QStringLiteral("categoryId"), t.categoryId},
            {QStringLiteral("categoryName"), t.categoryName},
            {QStringLiteral("colorSlot"), t.colorSlot},
        });
    }
    return out;
}

QVariantList Library::tagSuggestions() const
{
    // Same set as allTagNames (tags on the active photos), with the category so
    // the autocomplete rows can show the tag's color.
    QVariantList out;
    for (const TagInfo &t : m_db.activeTags()) {
        out.append(QVariantMap{
            {QStringLiteral("name"), t.name},
            {QStringLiteral("colorSlot"), t.colorSlot},
            {QStringLiteral("categoryName"), t.categoryName},
        });
    }
    return out;
}

QStringList Library::tagSamplePaths(const QString &name, int limit) const
{
    const int tagId = m_db.resolveTagName(name);
    if (tagId < 0)
        return {};
    return m_db.tagSamplePaths(tagId, limit);
}

QVariantList Library::categories() const
{
    // Hide categories whose tags are all in hidden/removed folders, mirroring how
    // the tag list itself drops inactive tags (empty categories stay, so they can
    // still be populated).
    QVariantList out;
    for (const CategoryInfo &c : m_db.loadCategories(/*activeOnly=*/true)) {
        out.append(QVariantMap{
            {QStringLiteral("id"), c.id},
            {QStringLiteral("name"), c.name},
            {QStringLiteral("colorSlot"), c.colorSlot},
            {QStringLiteral("tagCount"), c.tagCount},
        });
    }
    return out;
}

int Library::createCategory(const QString &name, int paletteSize)
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty())
        return -1;
    // Resolve-or-create: if a category with this name already exists (including
    // one currently hidden because its tags are all in disabled folders), reuse
    // it rather than failing. The caller assigns the tag to it, which reveals it
    // again — so typing an existing name in "New category…" just adds to it.
    const int existing = m_db.categoryIdByName(trimmed);
    if (existing >= 0)
        return existing;

    // Pick the first slot no category uses; once every slot is taken, the one
    // used least (lowest index on ties), so colors spread as evenly as possible.
    const int slotCount = qMax(1, paletteSize); // ("slots" is a Qt keyword)
    QVector<int> uses(slotCount, 0);
    for (const CategoryInfo &c : m_db.loadCategories())
        uses[((c.colorSlot % slotCount) + slotCount) % slotCount] += 1;
    int slot = 0;
    for (int i = 1; i < slotCount; ++i) {
        if (uses[i] < uses[slot])
            slot = i;
    }

    const int id = m_db.insertCategory(trimmed, slot);
    if (id >= 0)
        emit categoriesChanged();
    return id;
}

int Library::renameCategory(int categoryId, const QString &newName)
{
    const QString trimmed = newName.trimmed();
    if (categoryId < 0 || trimmed.isEmpty())
        return RenameEmpty;
    if (m_db.categoryName(categoryId) == trimmed)
        return RenameOk;
    // Another category with this name is a conflict; this one is just a casing change.
    const int existing = m_db.categoryIdByName(trimmed);
    if (existing >= 0 && existing != categoryId)
        return RenameConflict;

    m_db.renameCategory(categoryId, trimmed);
    refreshTags(); // emits categoriesChanged + tagsChanged (tag payloads carry the name)
    return RenameOk;
}

void Library::setCategoryColor(int categoryId, int colorSlot)
{
    if (categoryId < 0 || colorSlot < 0)
        return;
    m_db.setCategoryColor(categoryId, colorSlot);
    refreshTags(); // emits categoriesChanged + tagsChanged (chips re-resolve their color)
}

void Library::deleteCategory(int categoryId)
{
    if (categoryId < 0)
        return;
    m_db.deleteCategory(categoryId); // tags are kept, now uncategorized
    refreshTags(); // emits categoriesChanged + tagsChanged
}

void Library::setTagCategory(int tagId, int categoryId)
{
    if (tagId < 0)
        return;
    m_db.setTagCategory(tagId, categoryId);
    refreshTags(); // emits categoriesChanged (per-category counts) + tagsChanged
}

void Library::deleteTag(int tagId)
{
    if (tagId < 0)
        return;
    m_db.enqueueSidecarDirtyForTag(tagId); // before the delete drops the links
    m_db.deleteTag(tagId); // removes the tag and all its photo links (cascade)
    m_photos->setPhotos(m_db.loadPhotos()); // refresh in-memory tag data
    refreshTags();
    emit aliasesChanged();  // the tag's aliases cascaded away with it
    updateSearchAliases();
    scheduleSidecarFlush();
}

int Library::renameTag(int tagId, const QString &newName)
{
    const QString trimmed = newName.trimmed();
    if (tagId < 0 || trimmed.isEmpty())
        return RenameEmpty;

    if (m_db.tagName(tagId) == trimmed)
        return RenameOk; // exact no-op

    // A match on another tag is a conflict; a match on this same tag is just a
    // change of casing, which is allowed (the id, links, and archive are intact).
    const int existing = m_db.tagIdByName(trimmed);
    if (existing >= 0 && existing != tagId)
        return RenameConflict;
    // A name already used by an alias would make resolution ambiguous (the alias
    // and a real tag sharing a name), so treat it as a conflict too.
    if (m_db.aliasExists(trimmed))
        return RenameConflict;

    m_db.renameTag(tagId, trimmed);
    m_db.enqueueSidecarDirtyForTag(tagId); // the keyword text changes for its carriers
    m_photos->setPhotos(m_db.loadPhotos()); // refresh in-memory tag names
    refreshTags();
    emit aliasesChanged(); // the canonical name shown in the alias list changed
    scheduleSidecarFlush();
    return RenameOk;
}

void Library::applyTags(int photoId)
{
    // tagIds carries every tag (including the hidden album/favorites ones the album
    // filter and the star need); the visible names come from photoTags(), which
    // already drops system tags.
    const QSet<int> ids = m_db.photoTagIds(photoId);
    QStringList names;
    for (const TagInfo &t : m_db.photoTags(photoId))
        names.append(t.name);
    const bool favorite = ids.contains(m_db.favoritesTagId());
    m_photos->setTagsForId(photoId, ids, names, favorite);
}

void Library::setMatchAll(bool matchAll)
{
    if (m_matchAll == matchAll)
        return;
    m_matchAll = matchAll;
    refreshTags(); // the sidebar's tag set depends on the filter mode
}

void Library::setTagFolderScope(const QString &path)
{
    if (m_tagFolderScope == path)
        return;
    m_tagFolderScope = path;
    refreshTags(); // the sidebar now lists only tags in this folder subtree
}

void Library::refreshTags()
{
    // The filter sidebar shows only tags relevant to the active (shown) set, with
    // counts over that set; tags touching only hidden folders drop out. In "match
    // all" mode with tags already selected, it narrows further to co-occurring
    // tags (faceted drill-down). Database::facetTags does all of that; apply it
    // here on the GUI thread.
    applyFacetTags(m_db.facetTags(m_tagFolderScope, m_tags->selectedTagIds(), m_matchAll));
}

void Library::refreshTagsAsync()
{
    // Same result as refreshTags(), but the ~150ms query runs on the worker so it
    // never freezes the window — used at launch, where a synchronous build would
    // stall the first interaction.
    emit requestFacetTags(m_tagFolderScope, m_tags->selectedTagIds(), m_matchAll);
}

void Library::onFacetTagsReady(const QVector<TagInfo> &tags)
{
    applyFacetTags(tags);
}

void Library::applyFacetTags(const QVector<TagInfo> &tags)
{
    // setTags drops any selected tag no longer in the list, so an incompatible
    // match-all pick is deselected once it leaves the active set.
    m_tags->setTags(tags);
    // Every tag/category/album mutation lands here, so this is where the query
    // state (categories, album ids, smart-album trees) is brought up to date
    // before the album tiles and the grid re-read it.
    rebuildQueryState();
    emit tagsChanged();
    // Category and implication visibility in the tag manager depend on the same
    // active set (a category is hidden once all its tags leave the active folders;
    // an association once either of its tags does), so refresh both in lockstep.
    emit categoriesChanged();
    emit implicationsChanged();
    // Album tiles count only active photos (and honor the tag filter), too.
    emit albumsChanged();
}
