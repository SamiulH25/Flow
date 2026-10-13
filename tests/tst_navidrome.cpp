// Navidrome (Subsonic API): token and URL rules, a whole-library sync against an
// in-process Subsonic server, and the library index the sync feeds.
#include "library.h"
#include "navidrome.h"

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QSettings>
#include <QFileInfo>
#include <QHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUrlQuery>

#include <functional>

namespace {

const QString kPassword = QStringLiteral("testpass123");
// JPEG magic followed by filler. The "" splits the literal so \xE0 is not read as \xE0c.
const QByteArray kCoverBytes = QByteArray("\xFF\xD8\xFF\xE0" "cover-bytes", 15);

struct Reply
{
    int status = 200;
    QByteArray type = "application/json";
    QByteArray body;
};

QByteArray jsonBytes(const QJsonObject &root)
{
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

// Payload of a successful call: the fields of `payload` next to status "ok".
Reply ok(QJsonObject payload)
{
    payload.insert(QStringLiteral("status"), QStringLiteral("ok"));
    payload.insert(QStringLiteral("version"), QStringLiteral("1.16.1"));
    return {200, "application/json", jsonBytes(QJsonObject{{QStringLiteral("subsonic-response"), payload}})};
}

Reply failed(int code, const QString &message)
{
    const QJsonObject error{{QStringLiteral("code"), code}, {QStringLiteral("message"), message}};
    const QJsonObject root{{QStringLiteral("status"), QStringLiteral("failed")},
                           {QStringLiteral("version"), QStringLiteral("1.16.1")},
                           {QStringLiteral("error"), error}};
    return {200, "application/json", jsonBytes(QJsonObject{{QStringLiteral("subsonic-response"), root}})};
}

// A Subsonic server that enforces the token rule Navidrome enforces: t must be
// md5(password + s). The test supplies what each method returns.
class FakeSubsonic : public QObject
{
    Q_OBJECT
public:
    using Handler = std::function<Reply(const QString &method, const QUrlQuery &query)>;

    FakeSubsonic()
    {
        connect(&m_server, &QTcpServer::newConnection, this, &FakeSubsonic::onConnection);
    }

    bool start() { return m_server.listen(QHostAddress::LocalHost); }
    QString baseUrl() const
    {
        return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort());
    }

    Handler handler;
    QStringList methods;         // every /rest method called, in order
    QStringList coverRequests;   // the id of every getCoverArt call, in order

private:
    void onConnection()
    {
        QTcpSocket *socket = m_server.nextPendingConnection();
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] { onRequest(socket); });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    }

    void onRequest(QTcpSocket *socket)
    {
        QByteArray &buffer = m_pending[socket];
        buffer += socket->readAll();
        if (!buffer.contains("\r\n\r\n"))
            return;
        const QString line = QString::fromUtf8(buffer.left(buffer.indexOf("\r\n")));
        m_pending.remove(socket);

        const QUrl target(line.split(u' ').value(1));
        const QString method = target.path().mid(target.path().lastIndexOf(u'/') + 1);
        const QUrlQuery query(target);
        methods.append(method);
        if (method == QLatin1String("getCoverArt"))
            coverRequests.append(query.queryItemValue(QStringLiteral("id")));

        const Reply reply = respond(method, query);
        socket->write("HTTP/1.1 " + QByteArray::number(reply.status) + " X\r\n"
                      "Content-Type: " + reply.type + "\r\n"
                      "Content-Length: " + QByteArray::number(reply.body.size()) + "\r\n"
                      "Connection: close\r\n\r\n" + reply.body);
        socket->disconnectFromHost();
    }

    Reply respond(const QString &method, const QUrlQuery &query)
    {
        const QString salt = query.queryItemValue(QStringLiteral("s"));
        if (query.queryItemValue(QStringLiteral("t")) != Navidrome::token(kPassword, salt))
            return failed(40, QStringLiteral("Wrong username or password"));
        if (!handler)
            return failed(0, QStringLiteral("no handler"));
        return handler(method, query);
    }

    QTcpServer m_server;
    QHash<QTcpSocket *, QByteArray> m_pending;
};

// Song shape copied from a real Navidrome 0.64 getAlbum response.
QJsonObject song(const QString &id, const QString &title, int track, const QString &suffix,
                 int bitRate, int bitDepth, const QString &album, const QString &coverArt)
{
    return {{QStringLiteral("id"), id},
            {QStringLiteral("title"), title},
            {QStringLiteral("artist"), QStringLiteral("Aurora Fields")},
            {QStringLiteral("album"), album},
            {QStringLiteral("track"), track},
            {QStringLiteral("year"), 2001},
            {QStringLiteral("coverArt"), coverArt},
            {QStringLiteral("size"), 24579},
            {QStringLiteral("suffix"), suffix},
            {QStringLiteral("duration"), 3},
            {QStringLiteral("bitRate"), bitRate},
            {QStringLiteral("samplingRate"), 44100},
            {QStringLiteral("bitDepth"), bitDepth},
            {QStringLiteral("channelCount"), 1}};
}

QJsonObject albumEntry(const QString &id, const QString &name, const QString &artist,
                       const QString &coverArt, const QJsonArray &songs = {})
{
    return {{QStringLiteral("id"), id},
            {QStringLiteral("name"), name},
            {QStringLiteral("artist"), artist},
            {QStringLiteral("coverArt"), coverArt},
            {QStringLiteral("song"), songs}};
}

// What the fake server currently holds. Tests flip these between syncs.
struct Catalogue
{
    bool albumB = true;      // false: the server deleted album B
    bool failAlbums = false; // true: getAlbum fails with a server error
};

Reply serveCatalogue(const QString &method, const QUrlQuery &query, const Catalogue &catalogue)
{
    const QJsonArray songsA{song(QStringLiteral("song-1"), QStringLiteral("Opening"), 1,
                                 QStringLiteral("mp3"), 64, 0, QStringLiteral("First Light"),
                                 QStringLiteral("al-a-cover")),
                            song(QStringLiteral("song-2"), QStringLiteral("Dawn Chorus"), 2,
                                 QStringLiteral("flac"), 129, 16, QStringLiteral("First Light"),
                                 QStringLiteral("al-a-cover"))};
    const QJsonArray songsB{song(QStringLiteral("song-3"), QStringLiteral("Quarry"), 1,
                                 QStringLiteral("mp3"), 64, 0, QStringLiteral("Stone Songs"),
                                 QStringLiteral("al-b-cover"))};
    const QJsonObject albumA = albumEntry(QStringLiteral("alb-a"), QStringLiteral("First Light"),
                                          QStringLiteral("Aurora Fields"), QStringLiteral("al-a-cover"),
                                          songsA);
    const QJsonObject albumB = albumEntry(QStringLiteral("alb-b"), QStringLiteral("Stone Songs"),
                                          QStringLiteral("Basalt Choir"), QStringLiteral("al-b-cover"),
                                          songsB);

    if (method == QLatin1String("getAlbumList2")) {
        // Paged by offset and size, as Navidrome does. Entries are summaries
        // (no songs), so the list omits the song array.
        QList<QJsonObject> all{albumA};
        if (catalogue.albumB)
            all.append(albumB);
        const int offset = query.queryItemValue(QStringLiteral("offset")).toInt();
        const int size = query.queryItemValue(QStringLiteral("size")).toInt();
        QJsonArray page;
        if (offset < all.size()) {
            for (QJsonObject a : all.mid(offset, size)) {
                a.remove(QStringLiteral("song"));
                page.append(a);
            }
        }
        if (page.isEmpty())
            return ok({{QStringLiteral("albumList2"), QJsonObject{}}});   // empty: no "album" key
        return ok({{QStringLiteral("albumList2"), QJsonObject{{QStringLiteral("album"), page}}}});
    }
    if (method == QLatin1String("getAlbum")) {
        if (catalogue.failAlbums)
            return failed(0, QStringLiteral("disk on fire"));
        const QString id = query.queryItemValue(QStringLiteral("id"));
        return ok({{QStringLiteral("album"), id == QLatin1String("alb-a") ? albumA : albumB}});
    }
    if (method == QLatin1String("getCoverArt")) {
        if (query.queryItemValue(QStringLiteral("id")) == QLatin1String("al-a-cover"))
            return {200, "image/jpeg", kCoverBytes};
        // Navidrome answers a missing cover with 200 and a JSON error.
        return ok({});
    }
    return failed(70, QStringLiteral("unknown method"));
}

NavidromeServer serverFor(const FakeSubsonic &fake)
{
    return NavidromeServer{fake.baseUrl(), QStringLiteral("bob"), kPassword};
}

// Runs one sync to completion (or a 10 s timeout, reported as a failure).
NavidromeSyncResult syncOnce(const NavidromeServer &server, const QString &coverDir,
                             NavidromeSyncOptions options = {})
{
    NavidromeSync sync(server, coverDir, options);
    NavidromeSyncResult result;
    bool done = false;
    QEventLoop loop;
    QObject::connect(&sync, &NavidromeSync::finished, &loop, [&](NavidromeSyncResult r) {
        result = std::move(r);
        done = true;
        loop.quit();
    });
    sync.start();
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    loop.exec();
    if (!done)
        result.error = QStringLiteral("timed out");
    return result;
}

const Track *findTrack(const QList<Track> &tracks, const QString &title)
{
    for (const Track &t : tracks) {
        if (t.title == title)
            return &t;
    }
    return nullptr;
}

} // namespace

class TestNavidrome : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QVERIFY(m_tmp.isValid());
        // Library and QSettings must never touch the developer's real Flow.conf.
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_tmp.filePath(QStringLiteral("config")));
        QVERIFY(m_fake.start());
        m_fake.handler = [this](const QString &method, const QUrlQuery &query) {
            return serveCatalogue(method, query, m_catalogue);
        };
    }

    void tokenFollowsSubsonicRule()
    {
        // md5("testpass123" + "saltA1"); the same value Navidrome 0.64 accepted.
        QCOMPARE(Navidrome::token(kPassword, QStringLiteral("saltA1")),
                 QStringLiteral("4a91df94bf9fdcef00cc7e151161dad3"));
    }

    void serverValidityAndKey()
    {
        const NavidromeServer base{QStringLiteral("https://music.example.com"), QStringLiteral("bob"), kPassword};
        QVERIFY(base.isValid());
        QVERIFY(!(NavidromeServer{QStringLiteral("ftp://music.example.com"), QStringLiteral("bob"), kPassword}).isValid());
        QVERIFY(!(NavidromeServer{QStringLiteral("https://music.example.com"), QStringLiteral("bob"), {}}).isValid());
        QVERIFY(!(NavidromeServer{{}, QStringLiteral("bob"), kPassword}).isValid());

        // A trailing slash is the same server; another user is another library.
        NavidromeServer slashed = base;
        slashed.url += u'/';
        QCOMPARE(slashed.key(), base.key());
        NavidromeServer other = base;
        other.user = QStringLiteral("alice");
        QVERIFY(other.key() != base.key());
        // The key goes into track paths, so it must not reveal the password.
        QVERIFY(!base.key().contains(kPassword));
    }

    void trackPathsRoundTrip()
    {
        const NavidromeServer server{QStringLiteral("https://music.example.com"), QStringLiteral("bob"), kPassword};
        const QString path = Navidrome::trackPath(server, QStringLiteral("6Z7WIKTVQvD06atYRIFqrn"));
        QVERIFY(Navidrome::isRemotePath(path));
        QVERIFY(path.startsWith(Navidrome::pathPrefix(server)));
        QCOMPARE(Navidrome::songId(path), QStringLiteral("6Z7WIKTVQvD06atYRIFqrn"));
        QVERIFY(!Navidrome::isRemotePath(QStringLiteral("/home/bob/Music/a.mp3")));
        QVERIFY(Navidrome::songId(QStringLiteral("/home/bob/Music/a.mp3")).isEmpty());
    }

    void streamUrlCarriesTokenNotPassword()
    {
        const NavidromeServer server{QStringLiteral("https://host.example/music/"), QStringLiteral("bob"), kPassword};
        const QUrl url = Navidrome::streamUrl(server, QStringLiteral("song-1"));
        const QUrlQuery q(url);
        QCOMPARE(url.path(), QStringLiteral("/music/rest/stream"));
        QCOMPARE(q.queryItemValue(QStringLiteral("id")), QStringLiteral("song-1"));
        QCOMPARE(q.queryItemValue(QStringLiteral("u")), QStringLiteral("bob"));
        const QString salt = q.queryItemValue(QStringLiteral("s"));
        QVERIFY(salt.size() >= 6);
        QCOMPARE(q.queryItemValue(QStringLiteral("t")), Navidrome::token(kPassword, salt));
        QVERIFY(!url.toString().contains(kPassword));
    }

    void syncReadsWholeLibraryAcrossPages()
    {
        m_catalogue = {};
        m_fake.methods.clear();
        m_fake.coverRequests.clear();
        const QString covers = m_tmp.filePath(QStringLiteral("covers-first"));
        NavidromeSyncOptions options;
        options.albumPageSize = 1;   // one album per page: two full pages, then an empty one
        const NavidromeSyncResult r = syncOnce(serverFor(m_fake), covers, options);

        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.tracks.size(), qsizetype(3));
        QCOMPARE(m_fake.methods.count(QStringLiteral("getAlbumList2")), qsizetype(3));

        const NavidromeServer server = serverFor(m_fake);
        const Track *flac = findTrack(r.tracks, QStringLiteral("Dawn Chorus"));
        QVERIFY(flac);
        QCOMPARE(flac->path, Navidrome::trackPath(server, QStringLiteral("song-2")));
        QCOMPARE(flac->format, QStringLiteral("FLAC"));
        QCOMPARE(flac->bitsPerSample, 16);
        QCOMPARE(flac->sampleRate, 44100);
        QCOMPARE(flac->bitrate, 129);
        QCOMPARE(flac->trackNo, 2);
        QCOMPARE(flac->durationMs, qint64(3000));
        QCOMPARE(flac->albumArtist, QStringLiteral("Aurora Fields"));
        QCOMPARE(flac->album, QStringLiteral("First Light"));

        // The album's cover is cached as a real image file; a cover that is not
        // an image (JSON error) leaves the album without one.
        const Track *opening = findTrack(r.tracks, QStringLiteral("Opening"));
        QVERIFY(opening);
        QVERIFY(QFileInfo(opening->coverPath).isFile());
        QFile cover(opening->coverPath);
        QVERIFY(cover.open(QIODevice::ReadOnly));
        QCOMPARE(cover.readAll(), kCoverBytes);
        const Track *quarry = findTrack(r.tracks, QStringLiteral("Quarry"));
        QVERIFY(quarry);
        QVERIFY(quarry->coverPath.isEmpty());
    }

    void syncCachesCoversBetweenRuns()
    {
        m_catalogue = {};
        m_fake.coverRequests.clear();
        const QString covers = m_tmp.filePath(QStringLiteral("covers-cache"));
        QVERIFY(syncOnce(serverFor(m_fake), covers).ok);
        QCOMPARE(m_fake.coverRequests, (QStringList{QStringLiteral("al-a-cover"), QStringLiteral("al-b-cover")}));

        // The real cover is on disk now, so only the album without one is asked again.
        QVERIFY(syncOnce(serverFor(m_fake), covers).ok);
        QCOMPARE(m_fake.coverRequests,
                 (QStringList{QStringLiteral("al-a-cover"), QStringLiteral("al-b-cover"),
                              QStringLiteral("al-b-cover")}));
    }

    void wrongPasswordFailsWithServerMessage()
    {
        NavidromeServer server = serverFor(m_fake);
        server.password = QStringLiteral("not-the-password");
        const NavidromeSyncResult r = syncOnce(server, m_tmp.filePath(QStringLiteral("covers-bad")));
        QVERIFY(!r.ok);
        QCOMPARE(r.error, QStringLiteral("Wrong username or password"));
        QVERIFY(r.tracks.isEmpty());
    }

    void libraryStreamsRemoteTracksAndDropsDeletedSongs()
    {
        const QString music = m_tmp.filePath(QStringLiteral("music-empty"));
        QVERIFY(QDir().mkpath(music));
        Library lib(music, m_tmp.filePath(QStringLiteral("lib.db")), m_tmp.filePath(QStringLiteral("lib-covers")));
        QTRY_VERIFY_WITH_TIMEOUT(!lib.scanning(), 10000);

        m_catalogue = {};
        lib.setNavidrome(m_fake.baseUrl(), QStringLiteral("bob"), kPassword);
        QVERIFY(lib.navidromeSyncing());
        QTRY_VERIFY_WITH_TIMEOUT(!lib.navidromeSyncing(), 10000);
        QCOMPARE(lib.totalCount(), 3);
        QCOMPARE(lib.navidromeStatus(), QStringLiteral("Synced 3 tracks in 2 albums"));

        const QList<Track> all = lib.tracks();   // named: pointers below must outlive the call
        const Track *quarry = findTrack(all, QStringLiteral("Quarry"));
        QVERIFY(quarry);
        QVERIFY(Navidrome::isRemotePath(quarry->path));
        QVERIFY(TrackRoles::value(*quarry, TrackRoles::Remote).toBool());   // the list's stream marker
        const QUrl source = lib.sourceUrl(*quarry);
        QVERIFY(source.toString().startsWith(m_fake.baseUrl() + QStringLiteral("/rest/stream?")));
        QCOMPARE(QUrlQuery(source).queryItemValue(QStringLiteral("id")), QStringLiteral("song-3"));

        // M3U export skips streams: other players cannot open navidrome:// paths, and a
        // stream URL would put the token into a plain text file.
        const qint64 mix = lib.createMix(QStringLiteral("Remote"));
        lib.addToMix(mix, quarry->id);
        const QString m3u = m_tmp.filePath(QStringLiteral("remote.m3u8"));
        const QVariantMap exported = lib.exportMix(mix, QUrl::fromLocalFile(m3u));
        QVERIFY(exported.value(QStringLiteral("ok")).toBool());
        QCOMPARE(exported.value(QStringLiteral("skipped")).toInt(), 1);
        QFile written(m3u);
        QVERIFY(written.open(QIODevice::ReadOnly));
        QVERIFY(!written.readAll().contains("navidrome"));

        // A track of another server has no stream from this one.
        Track foreign = *quarry;
        foreign.path = Navidrome::trackPath(NavidromeServer{QStringLiteral("https://elsewhere.example"),
                                                            QStringLiteral("bob"), kPassword},
                                            QStringLiteral("song-3"));
        QVERIFY(lib.sourceUrl(foreign).isEmpty());

        // The server drops album B: the next sync removes its songs from the index.
        m_catalogue.albumB = false;
        lib.navidromeSync();
        QTRY_VERIFY_WITH_TIMEOUT(!lib.navidromeSyncing(), 10000);
        QCOMPARE(lib.totalCount(), 2);
        QVERIFY(!findTrack(lib.tracks(), QStringLiteral("Quarry")));

        // A failed sync keeps what the index had.
        m_catalogue.failAlbums = true;
        lib.navidromeSync();
        QTRY_VERIFY_WITH_TIMEOUT(!lib.navidromeSyncing(), 10000);
        QCOMPARE(lib.totalCount(), 2);
        QVERIFY(lib.navidromeStatus().startsWith(QStringLiteral("Sync failed: disk on fire")));
        m_catalogue = {};

        // Forgetting the server removes its tracks and its settings.
        lib.navidromeForget();
        QCOMPARE(lib.totalCount(), 0);
        QVERIFY(lib.navidromeUrl().isEmpty());
        QVERIFY(lib.navidromeStatus().isEmpty());
    }

    void settingsAndLocalRescanKeepRemoteRows()
    {
        const QString music = m_tmp.filePath(QStringLiteral("music-rescan"));
        QVERIFY(QDir().mkpath(music));
        const QString db = m_tmp.filePath(QStringLiteral("rescan.db"));
        const QString covers = m_tmp.filePath(QStringLiteral("rescan-covers"));
        m_catalogue = {};
        {
            Library lib(music, db, covers);
            QTRY_VERIFY_WITH_TIMEOUT(!lib.scanning(), 10000);
            lib.setNavidrome(m_fake.baseUrl(), QStringLiteral("bob"), kPassword);
            QTRY_VERIFY_WITH_TIMEOUT(!lib.navidromeSyncing(), 10000);
            QCOMPARE(lib.totalCount(), 3);

            // A local rescan of the music folder must not prune the remote rows.
            lib.rescan();
            QTRY_VERIFY_WITH_TIMEOUT(!lib.scanning(), 10000);
            QCOMPARE(lib.totalCount(), 3);
        }

        // A new Library on the same database and settings file: the server, its
        // user and password are restored, so the remote tracks still stream.
        Library again(music, db, covers);
        QTRY_VERIFY_WITH_TIMEOUT(!again.scanning(), 10000);
        QCOMPARE(again.navidromeUrl(), m_fake.baseUrl());
        QCOMPARE(again.navidromeUser(), QStringLiteral("bob"));
        QCOMPARE(again.totalCount(), 3);
        const QList<Track> all = again.tracks();
        const Track *quarry = findTrack(all, QStringLiteral("Quarry"));
        QVERIFY(quarry);
        QVERIFY(!again.sourceUrl(*quarry).isEmpty());
    }

private:
    QTemporaryDir m_tmp;
    FakeSubsonic m_fake;
    Catalogue m_catalogue;
};

QTEST_GUILESS_MAIN(TestNavidrome)
#include "tst_navidrome.moc"
