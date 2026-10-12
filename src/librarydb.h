#pragma once

#include "track.h"

#include <QHash>
#include <QList>
#include <QSqlDatabase>
#include <QString>

#include <memory>

// Thin wrapper around one SQLite connection to the library index.
// A connection may only be used from the thread that opened it, so the
// scanner thread and the GUI thread each own a LibraryDb.
class LibraryDb
{
public:
    struct Stamp {
        qint64 mtime = 0;
        qint64 size = 0;
    };

    struct Playlist {
        qint64 id = -1;
        QString name;
        QList<qint64> trackIds;   // in play order; may repeat
    };

    LibraryDb();
    ~LibraryDb();
    LibraryDb(const LibraryDb &) = delete;
    LibraryDb &operator=(const LibraryDb &) = delete;

    bool open(const QString &path);
    bool isOpen() const;
    QString lastError() const { return m_error; }

    QList<Track> loadAll();
    QHash<QString, Stamp> stamps();   // path -> stamp, for incremental scans
    bool upsert(const Track &t);      // keeps id, liked and added_at of existing rows
    bool remove(const QString &path);
    bool setLiked(qint64 id, bool liked);
    QByteArray peaks(qint64 id);                      // empty when not computed
    bool setPeaks(qint64 id, const QByteArray &peaks);
    bool setEnrichment(qint64 id, const Track::Enrichment &e);
    Track::Enrichment enrichment(qint64 id);
    // Loudness in dBFS (rms) and dBTP (peak), from the waveform pass.
    bool setLoudness(qint64 id, float rmsDb, float peakDb);

    QList<Playlist> playlists();
    qint64 createPlaylist(const QString &name);   // -1 on failure
    bool renamePlaylist(qint64 id, const QString &name);
    bool deletePlaylist(qint64 id);
    bool setPlaylistTracks(qint64 id, const QList<qint64> &trackIds);

    bool transaction();
    bool commit();
    bool rollback();

private:
    QSqlDatabase db() const;
    bool exec(const QString &sql);
    bool migrate();

    // Hot statements (one per scanned file, one per waveform) prepared once and
    // reused, instead of re-parsed by SQLite on every call. Held by pointer so
    // they can be destroyed before the connection is removed.
    struct Statements;
    std::unique_ptr<Statements> m_stmts;

    QString m_name;
    QString m_error;
};
