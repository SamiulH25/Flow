#include "track.h"

#include <QUrl>

namespace TrackFormat {

QString duration(qint64 ms)
{
    const qint64 s = qMax<qint64>(0, ms / 1000);
    return QString::number(s / 60) + u':' + QString::number(s % 60).rightJustified(2, u'0');
}

QString sampleSpec(const Track &t)
{
    if (t.sampleRate <= 0)
        return {};
    QString spec = QString::number(t.sampleRate / 1000.0, 'g', 4) + QStringLiteral("kHz");
    if (t.bitsPerSample > 0)
        spec += u'/' + QString::number(t.bitsPerSample) + QStringLiteral("-bit");
    return spec;
}

QString bitrate(int kbps)
{
    if (kbps <= 0)
        return {};
    if (kbps < 1000)
        return QString::number(kbps) + QStringLiteral(" kbps");
    return QString::number(kbps / 1000.0, 'f', 1) + QStringLiteral(" Mbps");
}

QString albumKey(const Track &t)
{
    const QString &artist = t.albumArtist.isEmpty() ? t.artist : t.albumArtist;
    return t.album.toCaseFolded() + u'\x1f' + artist.toCaseFolded();
}

QPair<QString, QString> placeholderColors(const QString &key)
{
    // Palette taken from the original mock data / ui-demo.html.
    static const QPair<QString, QString> palette[] = {
        {QStringLiteral("#2a2438"), QStringLiteral("#0e0e12")},
        {QStringLiteral("#232329"), QStringLiteral("#101014")},
        {QStringLiteral("#e8e4d8"), QStringLiteral("#8a877e")},
        {QStringLiteral("#34343d"), QStringLiteral("#141419")},
        {QStringLiteral("#26262e"), QStringLiteral("#0f0f13")},
        {QStringLiteral("#3a352c"), QStringLiteral("#141419")},
        {QStringLiteral("#1e1e25"), QStringLiteral("#0e0e12")},
        {QStringLiteral("#2c2c34"), QStringLiteral("#101014")},
    };
    // FNV-1a: stable across runs, unlike qHash (seeded per process).
    quint32 h = 2166136261u;
    for (QChar c : key) {
        h ^= c.unicode();
        h *= 16777619u;
    }
    return palette[h % (sizeof(palette) / sizeof(palette[0]))];
}

} // namespace TrackFormat

namespace TrackRoles {

QHash<int, QByteArray> names()
{
    return {
        {Id, "ti"},        {Title, "t"},    {Artist, "a"},   {Album, "al"},
        {Duration, "d"},   {Seconds, "s"},  {Format, "f"},   {Spec, "khz"},
        {Bitrate, "br"},   {HiRes, "hi"},   {Color1, "c1"},  {Color2, "c2"},
        {Cover, "cover"},  {Liked, "liked"},
    };
}

QVariant value(const Track &t, int role)
{
    using namespace TrackFormat;
    switch (role) {
    case Id: return t.id;
    case Title: return t.title;
    case Artist: return t.artist;
    case Album: return t.album;
    case Duration: return duration(t.durationMs);
    case Seconds: return t.durationMs / 1000.0;
    case Format: return t.format;
    case Spec: return sampleSpec(t);
    case Bitrate: return bitrate(t.bitrate);
    case HiRes: return t.isHiRes();
    case Color1: return placeholderColors(albumKey(t)).first;
    case Color2: return placeholderColors(albumKey(t)).second;
    case Cover:
        return t.coverPath.isEmpty() ? QString() : QUrl::fromLocalFile(t.coverPath).toString();
    case Liked: return t.liked;
    }
    return {};
}

QVariantMap toVariantMap(const Track &t)
{
    QVariantMap map;
    const auto roles = names();
    for (auto it = roles.cbegin(); it != roles.cend(); ++it)
        map.insert(QString::fromLatin1(it.value()), value(t, it.key()));
    return map;
}

} // namespace TrackRoles
