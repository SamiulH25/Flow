#include "playqueue.h"

#include <QRandomGenerator>

PlayQueue::PlayQueue(QObject *parent)
    : QAbstractListModel(parent)
{
}

int PlayQueue::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : count();
}

QVariant PlayQueue::data(const QModelIndex &index, int role) const
{
    if (!checkIndex(index, CheckIndexOption::IndexIsValid | CheckIndexOption::ParentIsInvalid))
        return {};
    const int row = index.row();
    const Track &t = m_entries.at(row).track;
    if (role == Uid)
        return m_entries.at(row).uid;
    if (role == Divider) {
        if (row > 0 && TrackFormat::albumKey(m_entries.at(row - 1).track) == TrackFormat::albumKey(t))
            return QString();
        return t.album + QStringLiteral(" — ") + (t.albumArtist.isEmpty() ? t.artist : t.albumArtist);
    }
    return TrackRoles::value(t, role);
}

QHash<int, QByteArray> PlayQueue::roleNames() const
{
    QHash<int, QByteArray> roles = TrackRoles::names();
    roles.insert(Uid, "uid");
    roles.insert(Divider, "div");
    return roles;
}

const Track *PlayQueue::current() const
{
    return at(m_current);
}

const Track *PlayQueue::at(int row) const
{
    if (row < 0 || row >= count())
        return nullptr;
    return &m_entries.at(row).track;
}

quint64 PlayQueue::uidAt(int row) const
{
    return (row < 0 || row >= count()) ? 0 : m_entries.at(row).uid;
}

QRandomGenerator *PlayQueue::rng() const
{
    return m_rng ? m_rng : QRandomGenerator::global();
}

void PlayQueue::setCurrentRaw(int row)
{
    if (m_current == row)
        return;
    m_current = row;
    emit currentIndexChanged();
}

void PlayQueue::refreshDividers()
{
    if (!m_entries.isEmpty())
        emit dataChanged(index(0), index(count() - 1), {Divider});
}

void PlayQueue::shuffleAfterCurrent()
{
    // Fisher–Yates over the entries after the current one.
    const int first = m_current + 1;
    for (int i = count() - 1; i > first; --i) {
        const int j = first + int(rng()->bounded(i - first + 1));
        m_entries.swapItemsAt(i, j);
    }
}

void PlayQueue::replace(const QList<Track> &tracks, int start)
{
    beginResetModel();
    m_entries.clear();
    m_original.clear();
    for (const Track &t : tracks)
        m_entries.append(makeEntry(t));
    m_current = m_entries.isEmpty() ? -1 : qBound(0, start, count() - 1);
    if (m_shuffled && m_current >= 0) {
        // The chosen track plays first; everything else is shuffled after it.
        m_original = m_entries;
        m_entries.move(m_current, 0);
        m_current = 0;
        shuffleAfterCurrent();
    }
    endResetModel();
    emit countChanged();
    emit currentIndexChanged();
}

void PlayQueue::restore(const QList<Track> &tracks, int current)
{
    beginResetModel();
    m_entries.clear();
    m_original.clear();
    for (const Track &t : tracks)
        m_entries.append(makeEntry(t));
    // The pre-shuffle order is not saved; unshuffling keeps this order.
    if (m_shuffled)
        m_original = m_entries;
    m_current = m_entries.isEmpty() ? -1 : qBound(0, current, count() - 1);
    endResetModel();
    emit countChanged();
    emit currentIndexChanged();
}

void PlayQueue::insertNext(const Track &track)
{
    const Entry e = makeEntry(track);
    const int row = m_current + 1;
    beginInsertRows({}, row, row);
    if (m_shuffled) {
        // Keep it right after the current track in the unshuffled order too.
        qsizetype at = m_original.size();
        if (m_current >= 0) {
            const quint64 uid = m_entries.at(m_current).uid;
            for (qsizetype i = 0; i < m_original.size(); ++i) {
                if (m_original.at(i).uid == uid) {
                    at = i + 1;
                    break;
                }
            }
        }
        m_original.insert(at, e);
    }
    m_entries.insert(row, e);
    endInsertRows();
    refreshDividers();
    emit countChanged();
}

void PlayQueue::append(const Track &track)
{
    const Entry e = makeEntry(track);
    const int row = count();
    beginInsertRows({}, row, row);
    m_entries.append(e);
    if (m_shuffled)
        m_original.append(e);
    endInsertRows();
    refreshDividers();
    emit countChanged();
}

void PlayQueue::removeAt(int row)
{
    if (row < 0 || row >= count())
        return;
    const quint64 uid = m_entries.at(row).uid;
    beginRemoveRows({}, row, row);
    m_entries.removeAt(row);
    if (m_shuffled) {
        m_original.removeIf([uid](const Entry &e) { return e.uid == uid; });
    }
    endRemoveRows();

    int current = m_current;
    if (row < current)
        --current;
    else if (row == current && current >= count())
        current = count() - 1;   // removed the last entry while it was current
    refreshDividers();
    emit countChanged();
    setCurrentRaw(current);
}

void PlayQueue::move(int from, int to)
{
    if (from < 0 || from >= count() || to < 0 || to >= count() || from == to)
        return;
    // beginMoveRows wants the destination as "insert before" in the old layout.
    if (!beginMoveRows({}, from, from, {}, to > from ? to + 1 : to))
        return;
    m_entries.move(from, to);
    endMoveRows();

    int current = m_current;
    if (current == from)
        current = to;
    else if (from < current && to >= current)
        --current;
    else if (from > current && to <= current)
        ++current;
    refreshDividers();
    setCurrentRaw(current);
}

void PlayQueue::clear()
{
    if (m_entries.isEmpty())
        return;
    beginResetModel();
    m_entries.clear();
    m_original.clear();
    m_current = -1;
    endResetModel();
    emit countChanged();
    emit currentIndexChanged();
}

bool PlayQueue::setCurrentIndex(int row)
{
    if (row < 0 || row >= count())
        return false;
    setCurrentRaw(row);
    return true;
}

void PlayQueue::setShuffled(bool on)
{
    if (on == m_shuffled)
        return;
    beginResetModel();
    m_shuffled = on;
    if (on) {
        m_original = m_entries;
        shuffleAfterCurrent();
    } else {
        const quint64 uid = m_current >= 0 ? m_entries.at(m_current).uid : 0;
        m_entries = m_original;
        m_original.clear();
        m_current = -1;
        if (uid) {
            for (int i = 0; i < count(); ++i) {
                if (m_entries.at(i).uid == uid) {
                    m_current = i;
                    break;
                }
            }
        }
    }
    endResetModel();
    emit currentIndexChanged();
}

void PlayQueue::updateTrack(const Track &track)
{
    for (int i = 0; i < count(); ++i) {
        if (m_entries.at(i).track.id == track.id) {
            m_entries[i].track = track;
            emit dataChanged(index(i), index(i));
        }
    }
    for (Entry &e : m_original) {
        if (e.track.id == track.id)
            e.track = track;
    }
}

int PlayQueue::nextIndex(bool wrap) const
{
    if (m_entries.isEmpty())
        return -1;
    if (m_current + 1 < count())
        return m_current + 1;
    return wrap ? 0 : -1;
}

int PlayQueue::previousIndex(bool wrap) const
{
    if (m_entries.isEmpty())
        return -1;
    if (m_current > 0)
        return m_current - 1;
    return wrap ? count() - 1 : -1;
}
