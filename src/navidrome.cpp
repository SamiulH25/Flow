#include "navidrome.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QUrlQuery>

namespace {

constexpr char kApiVersion[] = "1.16.1";
constexpr char kClientName[] = "Flow";
constexpr int kTimeoutMs = 30000;
constexpr int kCoverSize = 600;

// Cover files are named by a hash of (server key, cover art id), so two
// servers never share a file and a changed cover gets a new name.
QString coverFileName(const QString &serverKey, const QString &coverArtId, const QString &ext)
{
    const QByteArray digest = QCryptographicHash::hash(
        (serverKey + u'\n' + coverArtId).toUtf8(), QCryptographicHash::Sha1);
    return QStringLiteral("navidrome-%1.%2").arg(QString::fromLatin1(digest.toHex()), ext);
}

QString randomSalt()
{
    static const char kAlphabet[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    QString salt;
    salt.reserve(12);
    for (int i = 0; i < 12; ++i)
        salt.append(QLatin1Char(kAlphabet[QRandomGenerator::global()->bounded(62)]));
    return salt;
}

// Every Subsonic call carries the same auth and client fields. The salt is
// fresh each time, so a token from one URL does not match any other.
QUrl endpoint(const NavidromeServer &server, const QString &method, const QUrlQuery &extra,
              bool json)
{
    QUrl url(server.url.trimmed());
    QString path = url.path();
    while (path.endsWith(u'/'))
        path.chop(1);
    url.setPath(path + QStringLiteral("/rest/") + method);

    const QString salt = randomSalt();
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("u"), server.user);
    query.addQueryItem(QStringLiteral("t"), Navidrome::token(server.password, salt));
    query.addQueryItem(QStringLiteral("s"), salt);
    query.addQueryItem(QStringLiteral("v"), QLatin1String(kApiVersion));
    query.addQueryItem(QStringLiteral("c"), QLatin1String(kClientName));
    if (json)
        query.addQueryItem(QStringLiteral("f"), QStringLiteral("json"));
    for (const auto &item : extra.queryItems())
        query.addQueryItem(item.first, item.second);
    url.setQuery(query);
    return url;
}

// The subsonic-response object of a JSON call, or an error message. Navidrome
// reports wrong credentials with HTTP 200 and status "failed", so both the
// transport and the envelope are checked.
QJsonObject envelope(QNetworkReply *reply, QString *error)
{
    if (reply->error() != QNetworkReply::NoError) {
        *error = reply->errorString();
        return {};
    }
    const QJsonObject root =
        QJsonDocument::fromJson(reply->readAll()).object().value(QStringLiteral("subsonic-response")).toObject();
    if (root.isEmpty()) {
        *error = QStringLiteral("not a Subsonic API response");
        return {};
    }
    if (root.value(QStringLiteral("status")).toString() != QStringLiteral("ok")) {
        const QJsonObject err = root.value(QStringLiteral("error")).toObject();
        *error = err.value(QStringLiteral("message")).toString();
        if (error->isEmpty())
            *error = QStringLiteral("the server reported an error");
        return {};
    }
    return root;
}

Track trackFromSong(const NavidromeServer &server, const QJsonObject &song,
                    const QString &albumArtist)
{
    Track t;
    t.path = Navidrome::trackPath(server, song.value(QStringLiteral("id")).toString());
    t.title = song.value(QStringLiteral("title")).toString();
    t.artist = song.value(QStringLiteral("artist")).toString();
    t.album = song.value(QStringLiteral("album")).toString();
    t.albumArtist = albumArtist;
    t.trackNo = song.value(QStringLiteral("track")).toInt();
    t.discNo = song.value(QStringLiteral("discNumber")).toInt();
    t.year = song.value(QStringLiteral("year")).toInt();
    t.durationMs = qint64(song.value(QStringLiteral("duration")).toDouble() * 1000.0);
    t.format = song.value(QStringLiteral("suffix")).toString().toUpper();
    t.sampleRate = song.value(QStringLiteral("samplingRate")).toInt();
    t.bitsPerSample = song.value(QStringLiteral("bitDepth")).toInt();
    t.bitrate = song.value(QStringLiteral("bitRate")).toInt();
    t.channels = song.value(QStringLiteral("channelCount")).toInt();
    t.size = song.value(QStringLiteral("size")).toInteger();
    return t;
}

} // namespace

// ---------------------------------------------------------------- NavidromeServer

bool NavidromeServer::isValid() const
{
    const QUrl u(url.trimmed());
    return u.isValid() && (u.scheme() == u"http" || u.scheme() == u"https")
        && !u.host().isEmpty() && !user.isEmpty() && !password.isEmpty();
}

QString NavidromeServer::key() const
{
    QString base = url.trimmed();
    while (base.endsWith(u'/'))
        base.chop(1);
    const QByteArray digest =
        QCryptographicHash::hash((base + u'\n' + user).toUtf8(), QCryptographicHash::Sha1);
    return QString::fromLatin1(digest.toHex().left(16));
}

// ---------------------------------------------------------------- Navidrome

namespace Navidrome {

QString pathPrefix(const NavidromeServer &server)
{
    return QStringLiteral("navidrome://%1/").arg(server.key());
}

QString trackPath(const NavidromeServer &server, const QString &songId)
{
    return pathPrefix(server) + songId;
}

bool isRemotePath(const QString &path)
{
    return path.startsWith(QStringLiteral("navidrome://"));
}

QString songId(const QString &path)
{
    if (!isRemotePath(path))
        return {};
    const QString rest = path.mid(QStringLiteral("navidrome://").size());
    const int slash = rest.indexOf(u'/');
    return slash < 0 ? QString() : rest.mid(slash + 1);
}

QUrl streamUrl(const NavidromeServer &server, const QString &songId)
{
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("id"), songId);
    return endpoint(server, QStringLiteral("stream"), query, false);
}

QString token(const QString &password, const QString &salt)
{
    const QByteArray digest =
        QCryptographicHash::hash((password + salt).toUtf8(), QCryptographicHash::Md5);
    return QString::fromLatin1(digest.toHex());
}

} // namespace Navidrome

// ---------------------------------------------------------------- NavidromeSync

NavidromeSync::NavidromeSync(NavidromeServer server, QString coverDir,
                             NavidromeSyncOptions options, QObject *parent)
    : QObject(parent)
    , m_server(std::move(server))
    , m_coverDir(std::move(coverDir))
    , m_options(options)
{
    if (m_options.albumPageSize < 1)
        m_options.albumPageSize = 1;
    if (m_options.albumsInFlight < 1)
        m_options.albumsInFlight = 1;
}

void NavidromeSync::start()
{
    fetchAlbumPage(0);
}

void NavidromeSync::cancel()
{
    fail(QStringLiteral("cancelled"));
}

QNetworkReply *NavidromeSync::send(const QUrl &url)
{
    QNetworkRequest request(url);
    request.setTransferTimeout(kTimeoutMs);
    return m_network.get(request);
}

void NavidromeSync::fetchAlbumPage(int offset)
{
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("type"), QStringLiteral("alphabeticalByName"));
    query.addQueryItem(QStringLiteral("size"), QString::number(m_options.albumPageSize));
    query.addQueryItem(QStringLiteral("offset"), QString::number(offset));
    QNetworkReply *reply =
        send(endpoint(m_server, QStringLiteral("getAlbumList2"), query, true));
    connect(reply, &QNetworkReply::finished, this, [this, reply, offset] {
        reply->deleteLater();
        if (m_done)
            return;
        QString error;
        const QJsonObject root = envelope(reply, &error);
        if (!error.isEmpty())
            return fail(error);
        // An empty library has no "album" key at all.
        const QJsonArray albums =
            root.value(QStringLiteral("albumList2")).toObject().value(QStringLiteral("album")).toArray();
        for (const QJsonValue &v : albums)
            m_albumIds.append(v.toObject().value(QStringLiteral("id")).toString());
        if (albums.size() == m_options.albumPageSize)
            fetchAlbumPage(offset + albums.size());
        else
            pumpAlbums();
    });
}

void NavidromeSync::pumpAlbums()
{
    while (!m_done && m_nextAlbum < m_albumIds.size()
           && m_albumsInFlight < m_options.albumsInFlight) {
        fetchAlbum(m_albumIds.at(m_nextAlbum++));
    }
    if (!m_done && m_albumsInFlight == 0 && m_nextAlbum >= m_albumIds.size())
        complete();
}

void NavidromeSync::fetchAlbum(const QString &albumId)
{
    ++m_albumsInFlight;
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("id"), albumId);
    QNetworkReply *reply = send(endpoint(m_server, QStringLiteral("getAlbum"), query, true));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (m_done)
            return;
        QString error;
        const QJsonObject root = envelope(reply, &error);
        if (!error.isEmpty())
            return fail(error);

        const QJsonObject album = root.value(QStringLiteral("album")).toObject();
        const QString albumArtist = album.value(QStringLiteral("artist")).toString();
        const QJsonArray songs = album.value(QStringLiteral("song")).toArray();
        const int first = int(m_tracks.size());
        for (const QJsonValue &s : songs)
            m_tracks.append(trackFromSong(m_server, s.toObject(), albumArtist));

        QString coverArt = album.value(QStringLiteral("coverArt")).toString();
        if (coverArt.isEmpty() && !songs.isEmpty())
            coverArt = songs.first().toObject().value(QStringLiteral("coverArt")).toString();
        const int count = int(m_tracks.size()) - first;
        if (coverArt.isEmpty()) {
            albumDone();
            return;
        }
        const QString cached = cachedCoverPath(coverArt);
        if (!cached.isEmpty()) {
            for (int i = first; i < first + count; ++i)
                m_tracks[i].coverPath = cached;
            albumDone();
            return;
        }
        fetchCover(coverArt, first, count);
    });
}

void NavidromeSync::fetchCover(const QString &coverArtId, int first, int count)
{
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("id"), coverArtId);
    query.addQueryItem(QStringLiteral("size"), QString::number(kCoverSize));
    QNetworkReply *reply = send(endpoint(m_server, QStringLiteral("getCoverArt"), query, false));
    connect(reply, &QNetworkReply::finished, this, [this, reply, coverArtId, first, count] {
        reply->deleteLater();
        if (m_done)
            return;
        // Covers are optional. Navidrome answers a missing cover with a 200 and
        // a JSON error, so only an actual image counts.
        QString path;
        const QString type = reply->header(QNetworkRequest::ContentTypeHeader).toString();
        if (reply->error() == QNetworkReply::NoError && type.startsWith(QStringLiteral("image/"))) {
            const QByteArray body = reply->readAll();
            const QString ext = type.endsWith(QStringLiteral("png")) ? QStringLiteral("png")
                                                                     : QStringLiteral("jpg");
            QDir().mkpath(m_coverDir);
            path = QDir(m_coverDir).filePath(coverFileName(m_server.key(), coverArtId, ext));
            QSaveFile file(path);
            if (!file.open(QIODevice::WriteOnly) || file.write(body) != body.size() || !file.commit())
                path.clear();
        }
        for (int i = first; i < first + count; ++i)
            m_tracks[i].coverPath = path;
        albumDone();
    });
}

QString NavidromeSync::cachedCoverPath(const QString &coverArtId) const
{
    for (const QString &ext : {QStringLiteral("jpg"), QStringLiteral("png")}) {
        const QString path = QDir(m_coverDir).filePath(coverFileName(m_server.key(), coverArtId, ext));
        if (QFileInfo::exists(path))
            return path;
    }
    return {};
}

void NavidromeSync::albumDone()
{
    --m_albumsInFlight;
    pumpAlbums();
}

void NavidromeSync::fail(const QString &error)
{
    if (m_done)
        return;
    m_done = true;
    NavidromeSyncResult result;
    result.error = error;
    emit finished(result);
}

void NavidromeSync::complete()
{
    m_done = true;
    NavidromeSyncResult result;
    result.ok = true;
    result.tracks = std::move(m_tracks);
    emit finished(result);
}
