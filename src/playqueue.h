#pragma once

#include "track.h"

#include <QAbstractListModel>
#include <QList>

class QRandomGenerator;

// The play queue: an ordered list of track snapshots plus the current index.
// The same track may appear more than once, so rows are identified by a uid.
//
// Shuffle only reorders what comes after the current entry and remembers the
// original order, so turning shuffle off restores it around the current track.
class PlayQueue : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(int currentIndex READ currentIndex NOTIFY currentIndexChanged)

public:
    enum Role {
        Uid = TrackRoles::Last + 1,
        Divider,   // "Album — Artist" when the album changes from the row above
    };

    explicit PlayQueue(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return int(m_entries.size()); }
    int currentIndex() const { return m_current; }
    const Track *current() const;
    const Track *at(int row) const;
    quint64 uidAt(int row) const;   // 0 when out of range
    bool isShuffled() const { return m_shuffled; }

    void replace(const QList<Track> &tracks, int start);
    void insertNext(const Track &track);
    void append(const Track &track);
    void removeAt(int row);
    void move(int from, int to);
    void clear();
    bool setCurrentIndex(int row);
    void setShuffled(bool on);
    void updateTrack(const Track &track);   // refresh snapshots (e.g. liked)

    // Where playback goes next / previously; -1 when there is nowhere to go.
    int nextIndex(bool wrap) const;
    int previousIndex(bool wrap) const;

    // Tests inject a seeded generator for deterministic shuffles.
    void setRandomGenerator(QRandomGenerator *rng) { m_rng = rng; }

signals:
    void countChanged();
    void currentIndexChanged();

private:
    struct Entry {
        quint64 uid;
        Track track;
    };

    Entry makeEntry(const Track &t) { return {++m_nextUid, t}; }
    void shuffleAfterCurrent();
    void setCurrentRaw(int row);
    void refreshDividers();
    QRandomGenerator *rng() const;

    QList<Entry> m_entries;
    QList<Entry> m_original;   // unshuffled order, only kept while shuffled
    int m_current = -1;
    bool m_shuffled = false;
    quint64 m_nextUid = 0;
    QRandomGenerator *m_rng = nullptr;
};
