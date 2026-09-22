#pragma once

#include <QList>
#include <QString>

// Lyrics for one audio file: a sidecar .lrc next to the file wins, otherwise
// the embedded LYRICS tag (ID3 USLT / Vorbis LYRICS). LRC timestamps make
// the lyrics "timed" so the UI can follow playback.
struct Lyrics {
    struct Line {
        qint64 ms = -1;   // -1 for untimed lines
        QString text;
    };

    QList<Line> lines;
    bool timed = false;

    static Lyrics load(const QString &audioPath);
    static Lyrics parse(const QString &text);   // LRC or plain text

    // Index of the line playing at `ms`, or -1 (before the first line / untimed).
    int indexAt(qint64 ms) const;
};
