#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

// yt-dlp as a download backend: the command line Flow runs and the lines it
// reads back. yt-dlp is told to print machine-readable "flow-*" lines
// (--print / --progress-template), so nothing depends on its human output.
namespace YtDlp {

// A YouTube / YouTube Music link (watch, youtu.be, playlist, album browse),
// with or without the https:// a pasted link sometimes lacks.
bool isSupportedUrl(const QString &url);
// The link as yt-dlp gets it: trimmed, https:// added when missing.
QString normalizedUrl(const QString &url);
// Whether a link names a whole list (playlist or album) rather than one song.
// watch?v=…&list=… is a song played from a list (often an endless radio), so
// it counts as a song; the UI can still ask for the list.
bool isPlaylistUrl(const QString &url);
// YouTube Music albums (browse/MPREb…, list=OLAK5uy…): their order is the
// track order, so it is written into the track number tag.
bool isAlbumUrl(const QString &url);

struct Request {
    QString url;
    QString outDir;    // finished files: <outDir>/<artist>/<album>/<title>.mp3
    QString tempDir;   // downloads + conversion in progress
    bool playlist = false;
    bool album = false;
};

// Arguments for `yt-dlp`: best audio → MP3 (VBR V0) with tags and a square
// cover embedded, so the files look like any other track in the library.
QStringList arguments(const Request &request);

struct Event {
    enum Kind { None, Progress, Item, File, Error };
    Kind kind = None;
    qint64 done = 0;     // Progress: bytes of the current item
    qint64 total = 0;    //   0 when yt-dlp does not know
    int index = 0;       // Item: 1-based position in the playlist (1 for a song)
    int count = 0;       //   entries being downloaded (1 for a song)
    QString title;       // Item: track title; playlist title in `list`
    QString artist;
    QString list;        // Item: playlist title, empty for a song
    QString path;        // File: absolute path of the finished file
    QString message;     // Error
};

// One line of yt-dlp output (stdout or stderr), without the newline.
Event parseLine(const QByteArray &line);

// A playlist title as a mix name ("Album - X" → "X").
QString listName(const QString &playlistTitle);

} // namespace YtDlp
