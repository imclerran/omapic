// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

class PhotoFilterModel;

// The state of one triage run — tagging photos one at a time from the keyboard.
// A single object shared by the triage header, sidebar, central view and footer,
// so they all read the same queue position, recent tags and undo stack. It holds
// state only: the gallery (Main.qml) performs each action through the Library and
// records an undo step here, and runs the inverse when a step is popped. Keeping
// the bookkeeping free of any Library/GUI dependency is what lets it run headless
// in the unit tests.
//
// Order model: `m_history` is every photo visited so far, in order, with
// `m_index` pointing at the current one; `m_queue` is what hasn't been shown yet.
// next() walks forward through the history first and only then pulls from the
// queue, so Ctrl+← / Ctrl+→ retrace the same path; stepping past the last photo
// leaves the session "finished" (currentId == -1) with everything still in the
// history for stepping back.
class TriageSession : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    // The photo being triaged, or -1 when inactive or every photo has been passed.
    Q_PROPERTY(int currentId READ currentId NOTIFY positionChanged)
    // 1-based position of the current photo ("N of M"); equals total once finished.
    Q_PROPERTY(int position READ position NOTIFY positionChanged)
    // Photos in the session: visited plus still queued. Shrinks when one is trashed.
    Q_PROPERTY(int total READ total NOTIFY positionChanged)
    // Share of the session completed: photos advanced past over the total, 0..1.
    Q_PROPERTY(double progress READ progress NOTIFY positionChanged)
    Q_PROPERTY(bool finished READ finished NOTIFY positionChanged)
    Q_PROPERTY(bool hasPrevious READ hasPrevious NOTIFY positionChanged)
    Q_PROPERTY(bool hasNext READ hasNext NOTIFY positionChanged)
    // The tags applied this session, in a stable slot order (index + 1 is the
    // Ctrl-number), at most kRecentCap. A tag keeps its slot for its whole life
    // in the list, so applying one never renumbers the others; new tags append,
    // and once the list is full the least-recently-used tag is evicted and the
    // newcomer takes its freed slot (so only that one number changes).
    Q_PROPERTY(QStringList recentTags READ recentTags NOTIFY recentTagsChanged)
    Q_PROPERTY(int undoCount READ undoCount NOTIFY undoStackChanged)
    // Caller-supplied text naming what the queue holds, for the header.
    Q_PROPERTY(QString description READ description NOTIFY activeChanged)
    // Whatever the gallery wants restored on exit (its selection, scroll position,
    // …). Stored opaquely at begin() and handed back unchanged — nothing in the
    // session touches it.
    Q_PROPERTY(QVariantMap galleryState READ galleryState NOTIFY activeChanged)
    // Trash asks for confirmation on first use per session; the gallery sets this
    // once the user has agreed. Reset by begin().
    Q_PROPERTY(bool trashConfirmed READ trashConfirmed WRITE setTrashConfirmed NOTIFY trashConfirmedChanged)

public:
    // How many recent tags are kept (and offered on Ctrl+1–9).
    static constexpr int kRecentCap = 9;

    explicit TriageSession(QObject *parent = nullptr);

    bool active() const { return m_active; }
    int currentId() const;
    int position() const;
    int total() const { return m_history.size() + m_queue.size(); }
    double progress() const;
    bool finished() const { return m_active && m_index >= m_history.size(); }
    bool hasPrevious() const { return m_active && m_index > 0; }
    bool hasNext() const { return m_active && (m_index + 1 < m_history.size() || !m_queue.isEmpty()); }
    QStringList recentTags() const { return m_recent; }
    int undoCount() const { return m_undo.size(); }
    QString description() const { return m_description; }
    QVariantMap galleryState() const { return m_galleryState; }
    bool trashConfirmed() const { return m_trashConfirmed; }
    void setTrashConfirmed(bool on);

    // The photos a triage run covers, in the view's current order — the one rule
    // every entry point shares:
    //   two or more selected   → the selected photos
    //   otherwise              → every photo in the view
    // (With exactly one selected the whole view is returned; the caller opens the
    // run on that photo via begin's startIndex.) `view` is the gallery's filtered
    // model (current filter, folder/album scope and sort); `selectedIds` the
    // gallery selection.
    Q_INVOKABLE QVariantList buildQueue(PhotoFilterModel *view, const QVariantList &selectedIds) const;

    // Start a run over `queue` (photo ids, in order), opening on the photo at
    // `startIndex` (clamped): the photos before it go into the history, reachable
    // with previous(); the rest are queued ahead. Clears the recent tags, the undo
    // stack and the trash confirmation.
    Q_INVOKABLE void begin(const QVariantList &queue, const QString &description,
                           const QVariantMap &galleryState, int startIndex = 0);
    // Finish the run: inactive, nothing current, every list cleared. The gallery
    // reads galleryState before calling this.
    Q_INVOKABLE void end();

    // Move on: forward through the history, then pull the next queued photo; one
    // step past the last photo finishes the session. Returns false when there was
    // nowhere to go (already finished).
    Q_INVOKABLE bool next();
    // Step back through the history. Returns false at the first photo.
    Q_INVOKABLE bool previous();

    // Note a tag applied by the user. A tag already in the list keeps its slot
    // (so numbering never shifts while tagging) and is just marked used, so it
    // won't be the one evicted; a new tag appends to the next free slot, or — once
    // the list is full — replaces the least-recently-used tag in its slot. Matched
    // case-insensitively.
    Q_INVOKABLE void recordTag(const QString &name);

    // The undo stack. Steps are opaque maps the gallery builds; popUndo returns an
    // empty map when the stack is empty.
    Q_INVOKABLE void pushUndo(const QVariantMap &step);
    Q_INVOKABLE QVariantMap popUndo();

    // Drop the current photo from the session (it was trashed). What followed it
    // becomes current — the next history entry, else the next queued photo, else
    // the session finishes. Returns the removed id, or -1 when nothing was current.
    Q_INVOKABLE int removeCurrent();
    // Put a photo back as the current one, ahead of whatever is current now (the
    // trash undo, with the restored row's new id).
    Q_INVOKABLE void insertCurrent(int photoId);

signals:
    void activeChanged();
    void positionChanged();
    void recentTagsChanged();
    void undoStackChanged();
    void trashConfirmedChanged();

private:
    // Pull the next queued photo into the history when the index has run past
    // the end of it (after a removal, or on next()). No-op with an empty queue.
    void pullFromQueue();

    bool m_active = false;
    QList<int> m_history;
    int m_index = 0;
    QList<int> m_queue;
    QStringList m_recent; // slot order; index + 1 is the Ctrl-number
    // Last-used sequence per recent tag (folded name), for least-recently-used
    // eviction only — it never affects the display order.
    QHash<QString, qint64> m_recentUsed;
    qint64 m_recentSeq = 0;
    QList<QVariantMap> m_undo;
    QString m_description;
    QVariantMap m_galleryState;
    bool m_trashConfirmed = false;
};
