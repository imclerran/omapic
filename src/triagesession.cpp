// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#include "triagesession.h"

#include <QSet>

#include "photofiltermodel.h"
#include "photomodel.h"

TriageSession::TriageSession(QObject *parent)
    : QObject(parent)
{
}

int TriageSession::currentId() const
{
    if (!m_active || m_index < 0 || m_index >= m_history.size())
        return -1;
    return m_history.at(m_index);
}

int TriageSession::position() const
{
    if (!m_active)
        return 0;
    return qMin(m_index + 1, total());
}

double TriageSession::progress() const
{
    const int n = total();
    if (!m_active || n == 0)
        return 0.0;
    // Photos advanced past, over everything in the run: 0 on the first photo, 1
    // once the last has been stepped past.
    return qBound(0.0, double(m_index) / double(n), 1.0);
}

void TriageSession::setTrashConfirmed(bool on)
{
    if (m_trashConfirmed == on)
        return;
    m_trashConfirmed = on;
    emit trashConfirmedChanged();
}

QVariantList TriageSession::buildQueue(PhotoFilterModel *view, const QVariantList &selectedIds) const
{
    QVariantList out;
    if (!view)
        return out;
    QSet<int> selected;
    for (const QVariant &v : selectedIds)
        selected.insert(v.toInt());

    const int rows = view->rowCount();
    for (int r = 0; r < rows; ++r) {
        const QModelIndex idx = view->index(r, 0);
        const int id = view->data(idx, PhotoModel::IdRole).toInt();
        bool take;
        if (selected.size() >= 2)
            take = selected.contains(id);
        else if (selected.size() == 1)
            take = true;
        else
            take = view->data(idx, PhotoModel::TagCountRole).toInt() == 0;
        if (take)
            out.append(id);
    }
    return out;
}

void TriageSession::begin(const QVariantList &queue, const QString &description,
                          const QVariantMap &galleryState)
{
    m_history.clear();
    m_queue.clear();
    for (const QVariant &v : queue) {
        const int id = v.toInt();
        if (id >= 0)
            m_queue.append(id);
    }
    m_index = 0;
    pullFromQueue(); // the first photo is current right away
    m_recent.clear();
    m_undo.clear();
    m_description = description;
    m_galleryState = galleryState;
    m_trashConfirmed = false;
    m_active = true;
    emit activeChanged();
    emit positionChanged();
    emit recentTagsChanged();
    emit undoStackChanged();
    emit trashConfirmedChanged();
}

void TriageSession::end()
{
    const bool wasActive = m_active;
    m_active = false;
    m_history.clear();
    m_queue.clear();
    m_index = 0;
    m_recent.clear();
    m_undo.clear();
    m_description.clear();
    m_galleryState.clear();
    if (wasActive)
        emit activeChanged();
    emit positionChanged();
    emit recentTagsChanged();
    emit undoStackChanged();
}

void TriageSession::pullFromQueue()
{
    if (m_index >= m_history.size() && !m_queue.isEmpty())
        m_history.append(m_queue.takeFirst());
}

bool TriageSession::next()
{
    if (!m_active || m_index >= m_history.size())
        return false; // already finished
    ++m_index;
    pullFromQueue(); // forward through the history first; the queue only past its end
    emit positionChanged();
    return true;
}

bool TriageSession::previous()
{
    if (!m_active || m_index <= 0)
        return false;
    --m_index;
    emit positionChanged();
    return true;
}

void TriageSession::recordTag(const QString &name)
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty())
        return;
    // Same tag again moves to the front rather than appearing twice.
    for (int i = m_recent.size() - 1; i >= 0; --i)
        if (m_recent.at(i).compare(trimmed, Qt::CaseInsensitive) == 0)
            m_recent.removeAt(i);
    m_recent.prepend(trimmed);
    while (m_recent.size() > kRecentCap)
        m_recent.removeLast();
    emit recentTagsChanged();
}

void TriageSession::pushUndo(const QVariantMap &step)
{
    m_undo.append(step);
    emit undoStackChanged();
}

QVariantMap TriageSession::popUndo()
{
    if (m_undo.isEmpty())
        return {};
    const QVariantMap step = m_undo.takeLast();
    emit undoStackChanged();
    return step;
}

int TriageSession::removeCurrent()
{
    const int id = currentId();
    if (id < 0)
        return -1;
    m_history.removeAt(m_index);
    pullFromQueue(); // the index now names the next entry; refill past the end
    emit positionChanged();
    return id;
}

void TriageSession::insertCurrent(int photoId)
{
    if (!m_active || photoId < 0)
        return;
    m_history.insert(qBound(0, m_index, m_history.size()), photoId);
    emit positionChanged();
}
