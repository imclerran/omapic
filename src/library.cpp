// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#include "library.h"

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
    m_db.open(dbPath);

    m_filtered->setSourceModel(m_photos);
    m_filtered->setDynamicSortFilter(true);
    // Ordering is defined by PhotoFilterModel::lessThan (natural, case-insensitive
    // sort of the file name); sort(0) activates it and keeps it maintained.
    m_filtered->sort(0);

    // Changing the tag selection re-narrows the sidebar (faceted drill-down) when
    // in "match all" mode.
    connect(m_tags, &TagModel::selectionChanged, this, &Library::refreshTags);

    m_photos->setPhotos(m_db.loadPhotos());
    refreshTags();
    updateFolderRoots();
    // Keep the folder-view's orphan detection in step as folders come and go.
    connect(this, &Library::foldersChanged, this, &Library::updateFolderRoots);

    // Hashing runs on a worker thread so the window stays responsive. The worker
    // owns its own DB connection; we only touch the models here on the GUI thread.
    m_worker = new LibraryWorker(dbPath);
    m_worker->moveToThread(&m_workerThread);
    connect(&m_workerThread, &QThread::started, m_worker, &LibraryWorker::initialize);
    connect(&m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(this, &Library::requestMigration, m_worker, &LibraryWorker::runMigration);
    connect(this, &Library::requestImport, m_worker, &LibraryWorker::importDirectory);
    connect(this, &Library::requestRescan, m_worker, &LibraryWorker::rescan);
    connect(this, &Library::requestAddTagToPhotos, m_worker, &LibraryWorker::addTagToPhotos);
    connect(this, &Library::requestRemoveTagFromPhotos, m_worker, &LibraryWorker::removeTagFromPhotos);
    connect(m_worker, &LibraryWorker::progress, this, &Library::onWorkerProgress);
    connect(m_worker, &LibraryWorker::migrationFinished, this, &Library::onMigrationFinished);
    connect(m_worker, &LibraryWorker::importFinished, this, &Library::onImportFinished);
    connect(m_worker, &LibraryWorker::rescanFinished, this, &Library::onRescanFinished);
    connect(m_worker, &LibraryWorker::tagJobFinished, this, &Library::onTagJobFinished);
    m_workerThread.start();

    // Kick off the one-time hash + capture-time backfill. Only show the progress
    // UI when there is actually a backlog to chew through.
    if (m_db.unhashedPhotoCount() > 0 || m_db.photosMissingTakenAtCount() > 0)
        beginBusy(tr("Preparing library…"));
    emit requestMigration();
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

void Library::onMigrationFinished()
{
    // The backfill wrote hashes (affecting duplicate collapse) and capture times
    // (driving the timeline) on the worker's connection; reload so the in-memory
    // views reflect them without needing a restart.
    m_photos->setPhotos(m_db.loadPhotos());
    refreshTags();
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
    emit requestRescan(); // runs on the worker thread
}

void Library::onRescanFinished()
{
    // Rows were added/relocated/removed on the worker's connection; reload.
    m_photos->setPhotos(m_db.loadPhotos());
    refreshTags();
    endBusy(/*modal=*/false); // rescan reports in the status bar
    emit foldersChanged();
}

void Library::onTagJobFinished()
{
    // The worker changed photo_tags in bulk; reload the in-memory tags once.
    m_photos->setPhotos(m_db.loadPhotos());
    refreshTags(); // tagsChanged refreshes the detail panel in one pass
    endBusy(/*modal=*/false); // bulk tagging reports in the status bar
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
    const QStringList paths = m_db.activePhotoPaths();

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
        // show as photos in the grid here, not as tiles.)
        for (const FolderInfo &f : m_db.loadFolders()) {
            if (f.enabled)
                out.append(makeFolder(f.path));
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

bool Library::showInFileManager(int photoId)
{
    const Photo p = m_photos->photoById(photoId);
    return p.id < 0 ? false : showPathInFileManager(p.path);
}

bool Library::showPathInFileManager(const QString &path)
{
    // Open the containing folder with the file highlighted, via the freedesktop
    // FileManager1 D-Bus interface rather than spawning a specific file manager.
    // Omarchy's default (Nautilus) implements it, as does any compliant manager,
    // and it selects the item — which spawning `nautilus <path>` doesn't reliably
    // do across versions. The URL is fully percent-encoded so spaces and other
    // special characters in the name survive as a single D-Bus argument.
    const QString uri = QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded);
    return QProcess::startDetached(
        QStringLiteral("dbus-send"),
        {QStringLiteral("--session"),
         QStringLiteral("--dest=org.freedesktop.FileManager1"),
         QStringLiteral("--type=method_call"),
         QStringLiteral("/org/freedesktop/FileManager1"),
         QStringLiteral("org.freedesktop.FileManager1.ShowItems"),
         QStringLiteral("array:string:") + uri,
         QStringLiteral("string:")});
}

void Library::addTag(int photoId, const QString &name)
{
    const QString trimmed = name.trimmed();
    if (photoId < 0 || trimmed.isEmpty())
        return;
    const int tagId = m_db.ensureTag(trimmed);
    if (tagId < 0)
        return;
    m_db.linkTagToContent(photoId, tagId); // every copy of this content, not just the visible one
    m_db.rememberContentTag(m_db.photoHash(photoId), tagId);
    applyTags(photoId);
    refreshTags();
    emit photoChanged(photoId);
}

void Library::removeTag(int photoId, int tagId)
{
    if (photoId < 0 || tagId < 0)
        return;
    m_db.unlinkTagFromContent(photoId, tagId); // every copy of this content
    m_db.forgetContentTag(m_db.photoHash(photoId), tagId);
    applyTags(photoId);
    refreshTags();
    emit photoChanged(photoId);
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
        m_db.linkTagToContent(pid, tagId); // every copy of this content
        m_db.rememberContentTag(m_db.photoHash(pid), tagId);
        applyTags(pid); // in-place model update; detail refreshes once via refreshTags
    }
    m_db.commitTransaction();
    refreshTags();
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
    info[QStringLiteral("tags")] = tags;

    // Dimensions and a few EXIF fields, read from the file on demand (empty
    // strings where unavailable, so the pane hides those rows).
    const PhotoMetadata meta = readPhotoMetadata(p.path, p.isVideo);
    info[QStringLiteral("dimensions")] = meta.dimensions;
    info[QStringLiteral("size")] = meta.size;
    info[QStringLiteral("taken")] = meta.taken;
    info[QStringLiteral("camera")] = meta.camera;
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
    m_db.deleteTag(tagId); // removes the tag and all its photo links (cascade)
    m_photos->setPhotos(m_db.loadPhotos()); // refresh in-memory tag data
    refreshTags();
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

    m_db.renameTag(tagId, trimmed);
    m_photos->setPhotos(m_db.loadPhotos()); // refresh in-memory tag names
    refreshTags();
    return RenameOk;
}

void Library::applyTags(int photoId)
{
    QSet<int> ids;
    QStringList names;
    for (const TagInfo &t : m_db.photoTags(photoId)) {
        ids.insert(t.id);
        names.append(t.name);
    }
    m_photos->setTagsForId(photoId, ids, names);
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
    // counts over that set; tags touching only hidden folders drop out. Unused
    // tags still live in the DB and appear in the tag manager.
    //
    // In "match all" mode with tags already selected, narrow further to tags that
    // co-occur on the currently-matching photos (faceted drill-down), so chips
    // that could only ever yield an empty result disappear.
    const QList<int> sel = m_tags->selectedTagIds();
    // In folder view the chips are scoped to the current folder subtree.
    const QVector<TagInfo> active = m_db.activeTags(m_tagFolderScope);
    QHash<int, TagInfo> activeById;
    for (const TagInfo &t : active)
        activeById.insert(t.id, t);
    // A selected tag whose photos are all outside the current folder scope won't
    // be in `active`; pull it from the unscoped active set so it stays visible
    // (and deselectable) rather than vanishing from under the selection.
    if (!m_tagFolderScope.isEmpty() && !sel.isEmpty()) {
        for (const TagInfo &t : m_db.activeTags())
            if (!activeById.contains(t.id))
                activeById.insert(t.id, t);
    }

    QVector<TagInfo> tags =
        (m_matchAll && !sel.isEmpty()) ? m_db.coOccurringTags(sel, m_tagFolderScope) : active;

    // Keep a selected tag visible only while it still applies to some active
    // photo. This keeps an incompatible match-all pick deselectable, but lets a
    // tag whose photos all left the active set (e.g. its folder was disabled) be
    // pruned by setTags, which drops it from the selection and the filter.
    QSet<int> have;
    for (const TagInfo &t : tags)
        have.insert(t.id);
    for (int id : sel) {
        if (!have.contains(id) && activeById.contains(id)) {
            TagInfo t = activeById.value(id); // keeps the category for coloring
            t.count = 0;
            tags.append(t);
        }
    }
    std::sort(tags.begin(), tags.end(), [](const TagInfo &a, const TagInfo &b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });

    m_tags->setTags(tags);
    emit tagsChanged();
    // Category visibility in the tag manager depends on the same active set (a
    // category is hidden once all its tags leave the active folders), so refresh
    // it in lockstep with the tag list.
    emit categoriesChanged();
}
