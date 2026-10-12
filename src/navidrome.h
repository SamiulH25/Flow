#pragma once

#include "track.h"

#include <QList>
#include <QMetaType>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>

class QNetworkReply;

// Where a Navidrome (Subsonic API) server is and whose library to read. The
// password only derives per-request tokens; it is never sent in the clear.
struct NavidromeServer
{
    QString url;        // e.g. "https://music.example.com" or "http://host:4533/music"
    QString user;
    QString password;

    bool isValid() const;
    // Stable identity of server + user. Safe to put in track paths: no secrets.
    QString key() const;
};

namespace Navidrome {

// Remote tracks live in the library index as "navidrome://<key>/<song id>".
// Local files are absolute paths, so the two can never collide.
QString pathPrefix(const NavidromeServer &server);
QString trackPath(const NavidromeServer &server, const QString &songId);
bool isRemotePath(const QString &path);
QString songId(const QString &path);   // empty unless isRemotePath(path)

// Playable stream for a song, with a fresh salt and token. Not stored anywhere.
QUrl streamUrl(const NavidromeServer &server, const QString &songId);

// Subsonic token for one request: md5(password + salt), lowercase hex.
QString token(const QString &password, const QString &salt);

} // namespace Navidrome

struct NavidromeSyncResult
{
    bool ok = false;
    QString error;
    QList<Track> tracks;   // every song on the server, with cached covers
};
Q_DECLARE_METATYPE(NavidromeSyncResult)

struct NavidromeSyncOptions
{
    int albumPageSize = 500;   // getAlbumList2 page size
    int albumsInFlight = 4;    // albums fetched (with their cover) at once
};

// Reads a whole Navidrome library: album list, then each album's songs and
// cover. Runs on the caller's thread with asynchronous requests. It reports
// one result: either every song or an error. A failed sync reports nothing
// partial, so the caller keeps its previous index.
class NavidromeSync : public QObject
{
    Q_OBJECT
public:
    NavidromeSync(NavidromeServer server, QString coverDir,
                  NavidromeSyncOptions options = {}, QObject *parent = nullptr);

    void start();
    // Stops the sync; finished() is emitted once with ok=false.
    void cancel();

signals:
    void finished(NavidromeSyncResult result);

private:
    void fetchAlbumPage(int offset);
    void pumpAlbums();
    void fetchAlbum(const QString &albumId);
    void fetchCover(const QString &coverArtId, int first, int count);
    void albumDone();
    void fail(const QString &error);
    void complete();
    QString cachedCoverPath(const QString &coverArtId) const;
    QNetworkReply *send(const QUrl &url);

    NavidromeServer m_server;
    QString m_coverDir;
    NavidromeSyncOptions m_options;
    QNetworkAccessManager m_network;

    QStringList m_albumIds;
    int m_nextAlbum = 0;
    int m_albumsInFlight = 0;
    QList<Track> m_tracks;
    bool m_done = false;
};
