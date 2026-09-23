#include "ytdlp.h"

#include <QTest>

class TestYtDlp : public QObject
{
    Q_OBJECT

private slots:
    void recognisesLinks()
    {
        QVERIFY(YtDlp::isSupportedUrl(QStringLiteral("https://music.youtube.com/watch?v=2eZVbrO6Z1M")));
        QVERIFY(YtDlp::isSupportedUrl(QStringLiteral("  https://www.youtube.com/watch?v=x  ")));
        QVERIFY(YtDlp::isSupportedUrl(QStringLiteral("youtu.be/2eZVbrO6Z1M")));
        QVERIFY(YtDlp::isSupportedUrl(QStringLiteral("music.youtube.com/playlist?list=PLx")));
        QVERIFY(!YtDlp::isSupportedUrl(QStringLiteral("https://example.com/watch?v=x")));
        QVERIFY(!YtDlp::isSupportedUrl(QStringLiteral("https://notyoutube.com/watch?v=x")));
        QVERIFY(!YtDlp::isSupportedUrl(QStringLiteral("some song name")));
        QVERIFY(!YtDlp::isSupportedUrl(QString()));
        QCOMPARE(YtDlp::normalizedUrl(QStringLiteral(" youtu.be/abc ")), QStringLiteral("https://youtu.be/abc"));
    }

    void tellsSongsFromLists()
    {
        QVERIFY(!YtDlp::isPlaylistUrl(QStringLiteral("https://music.youtube.com/watch?v=abc")));
        // A song played from a list (or an endless radio) is still a song.
        QVERIFY(!YtDlp::isPlaylistUrl(QStringLiteral("https://music.youtube.com/watch?v=abc&list=RDAMVMabc")));
        QVERIFY(YtDlp::isPlaylistUrl(QStringLiteral("https://music.youtube.com/playlist?list=PLabc")));
        QVERIFY(YtDlp::isPlaylistUrl(QStringLiteral("https://music.youtube.com/browse/MPREb_Hb72dQECQNJ")));
        QVERIFY(YtDlp::isPlaylistUrl(QStringLiteral("https://www.youtube.com/playlist?list=PLabc")));

        QVERIFY(YtDlp::isAlbumUrl(QStringLiteral("https://music.youtube.com/browse/MPREb_Hb72dQECQNJ")));
        QVERIFY(YtDlp::isAlbumUrl(QStringLiteral("https://music.youtube.com/playlist?list=OLAK5uy_abc")));
        QVERIFY(!YtDlp::isAlbumUrl(QStringLiteral("https://music.youtube.com/playlist?list=PLabc")));
        QVERIFY(!YtDlp::isAlbumUrl(QStringLiteral("https://music.youtube.com/watch?v=a&list=OLAK5uy_abc")));
    }

    void buildsArguments()
    {
        YtDlp::Request r;
        r.url = QStringLiteral("https://music.youtube.com/watch?v=abc");
        r.outDir = QStringLiteral("/music/YouTube");
        r.tempDir = QStringLiteral("/cache/job-1");
        QStringList args = YtDlp::arguments(r);
        QVERIFY(args.contains(QStringLiteral("--ignore-config")));
        QVERIFY(args.contains(QStringLiteral("--no-playlist")));
        QVERIFY(!args.contains(QStringLiteral("--yes-playlist")));
        QVERIFY(!args.contains(QStringLiteral("playlist_index:%(track_number)s")));
        QVERIFY(args.contains(QStringLiteral("home:/music/YouTube")));
        QVERIFY(args.contains(QStringLiteral("temp:/cache/job-1")));
        QCOMPARE(args.at(args.indexOf(QStringLiteral("--audio-format")) + 1), QStringLiteral("mp3"));
        // The link comes last, after "--", so it can never be read as an option.
        QCOMPARE(args.last(), r.url);
        QCOMPARE(args.at(args.size() - 2), QStringLiteral("--"));

        r.playlist = true;
        r.album = true;
        args = YtDlp::arguments(r);
        QVERIFY(args.contains(QStringLiteral("--yes-playlist")));
        QVERIFY(args.contains(QStringLiteral("playlist_index:%(track_number)s")));
    }

    void parsesOutput()
    {
        YtDlp::Event e = YtDlp::parseLine("flow-progress 1024 2010455\n");
        QCOMPARE(e.kind, YtDlp::Event::Progress);
        QCOMPARE(e.done, 1024);
        QCOMPARE(e.total, 2010455);

        e = YtDlp::parseLine("flow-progress 3072 NA");   // size unknown
        QCOMPARE(e.kind, YtDlp::Event::Progress);
        QCOMPARE(e.total, 0);

        e = YtDlp::parseLine(R"(flow-item {"title": "Arcadia - Official Audio", "track": "Arcadia", )"
                             R"("artist": "Kevin MacLeod", "playlist_index": 2, "n_entries": 25, )"
                             R"("playlist_title": "Album - 25 Years, Vol. 1"})");
        QCOMPARE(e.kind, YtDlp::Event::Item);
        QCOMPARE(e.title, QStringLiteral("Arcadia"));   // the track name beats the video title
        QCOMPARE(e.artist, QStringLiteral("Kevin MacLeod"));
        QCOMPARE(e.index, 2);
        QCOMPARE(e.count, 25);
        QCOMPARE(e.list, QStringLiteral("Album - 25 Years, Vol. 1"));

        e = YtDlp::parseLine(R"(flow-item {"title": "Monkeys Spinning Monkeys"})");   // a song
        QCOMPARE(e.kind, YtDlp::Event::Item);
        QCOMPARE(e.title, QStringLiteral("Monkeys Spinning Monkeys"));
        QCOMPARE(e.index, 1);
        QCOMPARE(e.count, 1);
        QVERIFY(e.list.isEmpty());

        // Not a raw string: moc misreads \" inside one and drops the class.
        e = YtDlp::parseLine("flow-file \"/music/YouTube/Kevin MacLeod/Singles/\\\"Quoted\\\" é.mp3\"");
        QCOMPARE(e.kind, YtDlp::Event::File);
        QCOMPARE(e.path, QStringLiteral("/music/YouTube/Kevin MacLeod/Singles/\"Quoted\" é.mp3"));

        e = YtDlp::parseLine("ERROR: [youtube] dQw4w9WgXcQ: Video unavailable. This video is private");
        QCOMPARE(e.kind, YtDlp::Event::Error);
        QCOMPARE(e.message, QStringLiteral("Video unavailable. This video is private"));

        QCOMPARE(YtDlp::parseLine("[download] 50% of 2MiB").kind, YtDlp::Event::None);
        QCOMPARE(YtDlp::parseLine("flow-file ").kind, YtDlp::Event::None);
    }

    void namesLists()
    {
        QCOMPARE(YtDlp::listName(QStringLiteral("Album - 25 Years, Vol. 1")), QStringLiteral("25 Years, Vol. 1"));
        QCOMPARE(YtDlp::listName(QStringLiteral("EP - Night")), QStringLiteral("Night"));
        QCOMPARE(YtDlp::listName(QStringLiteral("Road trip")), QStringLiteral("Road trip"));
    }
};

QTEST_GUILESS_MAIN(TestYtDlp)
#include "tst_ytdlp.moc"
