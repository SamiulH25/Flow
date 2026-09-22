#pragma once

#include "track.h"

#include <QHash>
#include <QList>
#include <QSqlDatabase>
#include <QString>

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

    bool transaction();
    bool commit();

private:
    QSqlDatabase db() const;
    bool exec(const QString &sql);
    bool migrate();

    QString m_name;
    QString m_error;
};
