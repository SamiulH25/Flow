#pragma once

#include <QByteArray>
#include <QHash>
#include <QPair>
#include <QString>
#include <QVariant>
#include <QVariantMap>

// One library entry. Mirrors a row of the `tracks` table.
struct Track {
    qint64 id = -1;
    QString path;
    QString title;
    QString artist;
    QString album;
    QString albumArtist;
    int trackNo = 0;
    int discNo = 0;
    int year = 0;
    qint64 durationMs = 0;
    QString format;         // "FLAC" | "MP3" | "WAV"
    int sampleRate = 0;     // Hz
    int bitsPerSample = 0;  // 0 for lossy formats
    int bitrate = 0;        // kbps
    int channels = 0;
    QString coverPath;      // cached local image, may be empty
    bool liked = false;
    qint64 addedAt = 0;     // unix seconds
    qint64 mtime = 0;       // file mtime (unix ms) at last scan
    qint64 size = 0;        // file size at last scan

    bool isHiRes() const { return bitsPerSample >= 24 || sampleRate > 48000; }
};

namespace TrackFormat {

QString duration(qint64 ms);          // "3:47"
QString sampleSpec(const Track &t);   // "96kHz/24-bit", "44.1kHz"
QString bitrate(int kbps);            // "912 kbps", "2.3 Mbps"
QString albumKey(const Track &t);     // groups tracks into albums

// Deterministic placeholder gradient for tracks without cover art.
QPair<QString, QString> placeholderColors(const QString &key);

} // namespace TrackFormat

// The role set QML delegates read (short names kept from the original mock):
// ti id, t title, a artist, al album, d duration text, s seconds, f format,
// khz sample spec, br bitrate text, hi hi-res, c1/c2 placeholder, cover url, liked.
namespace TrackRoles {

enum Role {
    Id = Qt::UserRole + 1,
    Title,
    Artist,
    Album,
    Duration,
    Seconds,
    Format,
    Spec,
    Bitrate,
    HiRes,
    Color1,
    Color2,
    Cover,
    Liked,
    Last = Liked,
};

QHash<int, QByteArray> names();
QVariant value(const Track &t, int role);
QVariantMap toVariantMap(const Track &t);

} // namespace TrackRoles
