#pragma once

#include "track.h"

#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>

// Walks a music folder, reads tags with TagLib and syncs the SQLite index.
// Incremental: files whose mtime + size are unchanged are not re-read.
// Blocking; the Library runs it on a worker thread.
class LibraryScanner
{
public:
    struct Result {
        bool ok = false;
        QString error;
        int scanned = 0;    // audio files found
        int updated = 0;    // added or re-read
        int removed = 0;    // rows dropped for files that no longer exist
        int failed = 0;     // files TagLib could not open
        QStringList dirs;   // every directory visited, for QFileSystemWatcher
    };

    LibraryScanner(QString dbPath, QString coverDir);

    Result scan(const QString &root,
                const std::atomic_bool *cancel = nullptr,
                const std::function<void(int scanned)> &progress = {});

    // Reads tags + audio properties of one file. Cover art is written into
    // coverDir (content-addressed) when embedded, else a folder image is used.
    static bool readTrack(const QString &path, const QString &coverDir, Track *out);

    static bool isAudioFile(const QString &path);

private:
    QString m_dbPath;
    QString m_coverDir;
};
