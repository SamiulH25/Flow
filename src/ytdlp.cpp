#include "ytdlp.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>

namespace YtDlp {

namespace {

QUrl toUrl(const QString &url)
{
    const QString s = url.trimmed();
    if (s.isEmpty() || s.contains(u' '))
        return {};
    return QUrl(s.contains(u"://") ? s : QStringLiteral("https://") + s);
}

bool isYouTubeHost(const QString &host)
{
    const QString h = host.toLower();
    return h == u"youtube.com" || h.endsWith(u".youtube.com") || h == u"youtu.be";
}

// "flow-item {…}" → the JSON after the tag, parsed as one value.
QJsonValue jsonAfter(const QByteArray &line, qsizetype tagLength)
{
    const QByteArray json = '[' + line.mid(tagLength) + ']';
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    return doc.isArray() ? doc.array().first() : QJsonValue();
}

// yt-dlp prints "NA" for fields it does not have.
qint64 number(const QByteArray &field)
{
    bool ok = false;
    const double v = field.toDouble(&ok);
    return ok ? qint64(v) : 0;
}

} // namespace

bool isSupportedUrl(const QString &url)
{
    const QUrl u = toUrl(url);
    return u.isValid() && (u.scheme() == u"https" || u.scheme() == u"http")
           && isYouTubeHost(u.host());
}

QString normalizedUrl(const QString &url)
{
    return toUrl(url).toString(QUrl::FullyEncoded);
}

bool isPlaylistUrl(const QString &url)
{
    const QUrl u = toUrl(url);
    const QString path = u.path();
    if (path.startsWith(u"/browse/") || path == u"/playlist")
        return true;
    const QUrlQuery query(u);
    return query.hasQueryItem(QStringLiteral("list")) && !query.hasQueryItem(QStringLiteral("v"))
           && u.host() != u"youtu.be";
}

bool isAlbumUrl(const QString &url)
{
    const QUrl u = toUrl(url);
    if (u.path().startsWith(u"/browse/MPREb"))
        return true;
    return isPlaylistUrl(url)
           && QUrlQuery(u).queryItemValue(QStringLiteral("list")).startsWith(u"OLAK5uy");
}

QStringList arguments(const Request &r)
{
    QStringList args{
        // Only what is below: a user's ~/.config/yt-dlp must not change the output.
        QStringLiteral("--ignore-config"),
        QStringLiteral("--no-simulate"), QStringLiteral("--quiet"), QStringLiteral("--no-warnings"),
        QStringLiteral("--progress"), QStringLiteral("--newline"),
        // One unavailable video must not stop the rest of a playlist.
        QStringLiteral("--ignore-errors"),
        r.playlist ? QStringLiteral("--yes-playlist") : QStringLiteral("--no-playlist"),
        QStringLiteral("--progress-template"),
        QStringLiteral("download:flow-progress %(progress.downloaded_bytes)s "
                       "%(progress.total_bytes,progress.total_bytes_estimate)s"),
        QStringLiteral("--print"),
        QStringLiteral("before_dl:flow-item "
                       "%(.{title,track,artist,playlist_index,n_entries,playlist_title})j"),
        QStringLiteral("--print"), QStringLiteral("after_move:flow-file %(filepath)j"),

        QStringLiteral("-f"), QStringLiteral("bestaudio/best"),
        QStringLiteral("--extract-audio"), QStringLiteral("--audio-format"), QStringLiteral("mp3"),
        QStringLiteral("--audio-quality"), QStringLiteral("0"),
        QStringLiteral("--embed-metadata"), QStringLiteral("--embed-thumbnail"),
        // Video thumbnails are 16:9; album art is square. PNG so the crop always
        // runs (yt-dlp skips converting a thumbnail that already has the format).
        QStringLiteral("--convert-thumbnails"), QStringLiteral("png"),
        QStringLiteral("--postprocessor-args"),
        QStringLiteral("ThumbnailsConvertor+FFmpeg_o:-vf crop='min(iw\\,ih)':'min(iw\\,ih)'"),
        // Video descriptions are not tags; the release year beats the upload date.
        QStringLiteral("--parse-metadata"), QStringLiteral(":(?P<meta_description>)"),
        QStringLiteral("--parse-metadata"), QStringLiteral(":(?P<meta_synopsis>)"),
        QStringLiteral("--parse-metadata"), QStringLiteral("%(release_year|)s:%(meta_date)s"),
    };
    if (r.album)
        args << QStringLiteral("--parse-metadata") << QStringLiteral("playlist_index:%(track_number)s");
    args << QStringLiteral("--paths") << QStringLiteral("home:") + r.outDir
         << QStringLiteral("--paths") << QStringLiteral("temp:") + r.tempDir
         << QStringLiteral("--output")
         << QStringLiteral("%(artist,uploader|Unknown artist)s/%(album|Singles)s/%(track,title)s.%(ext)s")
         << QStringLiteral("--") << normalizedUrl(r.url);
    return args;
}

Event parseLine(const QByteArray &raw)
{
    const QByteArray line = raw.trimmed();
    Event e;
    if (line.startsWith("flow-progress ")) {
        const QList<QByteArray> f = line.split(' ');
        if (f.size() >= 3) {
            e.kind = Event::Progress;
            e.done = number(f.at(1));
            e.total = number(f.at(2));
        }
    } else if (line.startsWith("flow-item ")) {
        const QJsonObject o = jsonAfter(line, 10).toObject();
        e.kind = Event::Item;
        e.title = o.value(u"track").toString();
        if (e.title.isEmpty())
            e.title = o.value(u"title").toString();
        e.artist = o.value(u"artist").toString();
        e.list = o.value(u"playlist_title").toString();
        e.index = qMax(1, o.value(u"playlist_index").toInt(1));
        e.count = qMax(e.index, o.value(u"n_entries").toInt(1));
    } else if (line.startsWith("flow-file ")) {
        e.path = jsonAfter(line, 10).toString();
        if (!e.path.isEmpty())
            e.kind = Event::File;
    } else if (line.startsWith("ERROR:")) {
        e.kind = Event::Error;
        e.message = QString::fromUtf8(line.mid(6)).trimmed();
        // "[youtube] dQw4w9WgXcQ: Video unavailable" → "Video unavailable"
        static const QRegularExpression prefix(QStringLiteral("^\\[[^\\]]+\\] [^:]+: "));
        e.message.remove(prefix);
    }
    return e;
}

QString listName(const QString &playlistTitle)
{
    QString name = playlistTitle.trimmed();
    for (const QString &prefix : {QStringLiteral("Album - "), QStringLiteral("EP - "),
                                  QStringLiteral("Single - ")}) {
        if (name.startsWith(prefix)) {
            name = name.mid(prefix.size()).trimmed();
            break;
        }
    }
    return name;
}

} // namespace YtDlp
