#include "m3u.h"

#include <QTest>

class TestM3u : public QObject
{
    Q_OBJECT

private slots:
    void parsesCommonForms()
    {
        const QString text = QStringLiteral(
            "\xFEFF#EXTM3U\r\n"
            "#EXTINF:227,Neon Coastline - Midnight Drive\r\n"
            "neon/01.flac\r\n"
            "\r\n"
            "# a comment\n"
            "/abs/Paper Suns.mp3\n"
            "file:///abs/with%20space/b.flac\n"
            "http://radio.example/stream\n"
            "..\\windows\\c.wav\n"
            "  ./sub/../d.flac  \n");
        const QStringList paths = M3u::parse(text, QStringLiteral("/music/lists"));
        QCOMPARE(paths, (QStringList{
            QStringLiteral("/music/lists/neon/01.flac"),
            QStringLiteral("/abs/Paper Suns.mp3"),
            QStringLiteral("/abs/with space/b.flac"),
            QStringLiteral("/music/windows/c.wav"),
            QStringLiteral("/music/lists/d.flac"),
        }));
    }

    void decodesLatin1Fallback()
    {
        QCOMPARE(M3u::decode(QByteArray("caf\xc3\xa9.mp3")), QStringLiteral("café.mp3"));   // UTF-8
        QCOMPARE(M3u::decode(QByteArray("caf\xe9.mp3")), QStringLiteral("café.mp3"));       // Latin-1
    }

    void writesAndRoundTrips()
    {
        Track a;
        a.path = QStringLiteral("/m/é a.flac");
        a.artist = QStringLiteral("Artist");
        a.title = QStringLiteral("Two\nLines");
        a.durationMs = 61'900;
        Track b;
        b.path = QStringLiteral("/m/b.mp3");
        b.artist = QStringLiteral("X");
        b.title = QStringLiteral("Y");
        const QByteArray bytes = M3u::write({a, b});
        const QString text = QString::fromUtf8(bytes);
        QVERIFY(text.startsWith(QStringLiteral("#EXTM3U\n#EXTINF:61,Artist - Two Lines\n/m/é a.flac\n")));
        QVERIFY(text.contains(QStringLiteral("#EXTINF:-1,X - Y\n/m/b.mp3\n")));
        QCOMPARE(M3u::parse(M3u::decode(bytes), QStringLiteral("/elsewhere")),
                 (QStringList{a.path, b.path}));
    }
};

QTEST_GUILESS_MAIN(TestM3u)
#include "tst_m3u.moc"
