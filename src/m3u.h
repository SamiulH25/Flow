#pragma once

#include "track.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

// M3U / M3U8 playlists, as written by most players (VLC, foobar2000, mpd, …).
namespace M3u {

// Entries of a playlist as absolute, cleaned local paths, in order.
// Relative entries resolve against `baseDir` (the playlist's folder);
// file:// URLs are decoded; comments and #EXT lines are skipped;
// http(s) and other remote entries are dropped.
QStringList parse(const QString &text, const QString &baseDir);

// Bytes of a playlist file: UTF-8 (BOM tolerated), falling back to Latin-1
// for old .m3u files that are not valid UTF-8.
QString decode(const QByteArray &bytes);

// Extended M3U with #EXTINF lines and absolute paths, UTF-8.
QByteArray write(const QList<Track> &tracks);

} // namespace M3u
