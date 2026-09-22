// Integration test: real Library scan + QMediaPlayer playback of silent WAVs.
#include "library.h"
#include "player.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

namespace {

bool writeWav(const QString &path, int seconds)
{
    const quint32 rate = 44100, channels = 2, bits = 16;
    const quint32 blockAlign = channels * bits / 8;
    const quint32 dataSize = rate * blockAlign * quint32(seconds);
    QByteArray h;
    const auto u32 = [&h](quint32 v) { v = qToLittleEndian(v); h.append(reinterpret_cast<const char *>(&v), 4); };
    const auto u16 = [&h](quint16 v) { v = qToLittleEndian(v); h.append(reinterpret_cast<const char *>(&v), 2); };
    h.append("RIFF");
    u32(36 + dataSize);
    h.append("WAVEfmt ");
    u32(16);
    u16(1);
    u16(quint16(channels));
    u32(rate);
    u32(rate * blockAlign);
    u16(quint16(blockAlign));
    u16(quint16(bits));
    h.append("data");
    u32(dataSize);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(h);
    f.write(QByteArray(dataSize, '\0'));
    return true;
}

int rowOfTitle(const Library &lib, const QString &title)
{
    const QVariantList ids = lib.visibleTrackIds();
    for (int i = 0; i < ids.size(); ++i) {
        if (lib.trackById(ids.at(i).toLongLong())->title == title)
            return i;
    }
    return -1;
}

} // namespace

class TestPlayer : public QObject
{
    Q_OBJECT

    QTemporaryDir m_tmp;
    std::unique_ptr<Library> m_lib;
    std::unique_ptr<Player> m_player;

private slots:
    void initTestCase()
    {
        QVERIFY(m_tmp.isValid());
        // Keep QSettings (volume/shuffle/repeat) out of the real config.
        qputenv("XDG_CONFIG_HOME", m_tmp.filePath(QStringLiteral("config")).toUtf8());
        const QString music = m_tmp.filePath(QStringLiteral("music"));
        QVERIFY(QDir().mkpath(music));
        QVERIFY(writeWav(music + QStringLiteral("/a.wav"), 2));
        QVERIFY(writeWav(music + QStringLiteral("/b.wav"), 2));
        QVERIFY(writeWav(music + QStringLiteral("/c.wav"), 3));
        QFile lrc(music + QStringLiteral("/c.lrc"));
        QVERIFY(lrc.open(QIODevice::WriteOnly));
        lrc.write("[00:00.20]one\n[00:01.00]two\n");
        lrc.close();

        m_lib = std::make_unique<Library>(music, m_tmp.filePath(QStringLiteral("library.db")),
                                          m_tmp.filePath(QStringLiteral("covers")));
        QTRY_VERIFY_WITH_TIMEOUT(!m_lib->scanning(), 10000);
        QCOMPARE(m_lib->totalCount(), 3);
        m_lib->setSortMode(QStringLiteral("title"));   // a, b, c
        m_player = std::make_unique<Player>(m_lib.get());
        m_player->setShuffle(false);
        m_player->setRepeat(false);
    }

    void playsAndAdvances()
    {
        Player &p = *m_player;
        p.playTracks(m_lib->visibleTrackIds(), 0);
        QCOMPARE(p.queueCount(), 3);
        QCOMPARE(p.current().value(QStringLiteral("t")).toString(), QStringLiteral("a"));
        QTRY_VERIFY_WITH_TIMEOUT(p.playing(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(p.position() > 0.3, 5000);
        QVERIFY(qAbs(p.duration() - 2.0) < 0.1);
        // a.wav ends after 2 s and b.wav follows on its own.
        QTRY_COMPARE_WITH_TIMEOUT(p.queueIndex(), 1, 6000);
        QTRY_VERIFY_WITH_TIMEOUT(p.playing(), 5000);
    }

    void pauseSeekPrevious()
    {
        Player &p = *m_player;
        p.toggle();
        QTRY_VERIFY(!p.playing());
        p.seek(1.5);
        QTRY_VERIFY(p.position() >= 1.4);
        p.previous();   // past 3 s? no: 1.5 s goes to the previous track
        QCOMPARE(p.queueIndex(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(p.playing(), 5000);
        p.next();
        QCOMPARE(p.queueIndex(), 1);
    }

    void lyricsFollowPlayback()
    {
        Player &p = *m_player;
        p.jump(2);   // c.wav has c.lrc
        QVERIFY(p.lyricsTimed());
        QCOMPARE(p.lyrics().size(), 2);
        QTRY_COMPARE_WITH_TIMEOUT(p.lyricIndex(), 0, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(p.lyricIndex(), 1, 3000);
    }

    void stopsAtEndOfQueue()
    {
        Player &p = *m_player;
        QCOMPARE(p.queueIndex(), 2);
        QTRY_VERIFY_WITH_TIMEOUT(!p.playing(), 6000);
        QCOMPARE(p.queueIndex(), 2);
        QVERIFY(p.position() < 0.1);
    }

    void repeatWraps()
    {
        Player &p = *m_player;
        p.setRepeat(true);
        p.seek(2.7);
        p.play();
        QTRY_COMPARE_WITH_TIMEOUT(p.queueIndex(), 0, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(p.playing(), 5000);
        p.setRepeat(false);
    }

    void queueEditsKeepPlayback()
    {
        Player &p = *m_player;
        const qint64 cId = m_lib->visibleTrackIds().at(2).toLongLong();
        p.playNext(cId);
        QCOMPARE(p.queueCount(), 4);
        QCOMPARE(p.queueAt(p.queueIndex() + 1).value(QStringLiteral("t")).toString(), QStringLiteral("c"));
        p.enqueue(cId);
        QCOMPARE(p.queueCount(), 5);
        const QString playing = p.current().value(QStringLiteral("t")).toString();
        p.move(p.queueIndex(), 4);
        QCOMPARE(p.queueIndex(), 4);
        QCOMPARE(p.current().value(QStringLiteral("t")).toString(), playing);
        p.removeAt(0);
        QCOMPARE(p.queueIndex(), 3);
        QCOMPARE(p.current().value(QStringLiteral("t")).toString(), playing);
    }

    void likeUpdatesCurrent()
    {
        Player &p = *m_player;
        QSignalSpy spy(&p, &Player::currentChanged);
        QVERIFY(!p.current().value(QStringLiteral("liked")).toBool());
        m_lib->toggleLike(p.currentId());
        QVERIFY(p.current().value(QStringLiteral("liked")).toBool());
        QCOMPARE(spy.count(), 1);
        QCOMPARE(m_lib->likedCount(), 1);
        m_lib->toggleLike(p.currentId());
        QCOMPARE(m_lib->likedCount(), 0);
    }

    void unplayableFileIsSkipped()
    {
        Player &p = *m_player;
        // b.wav gets damaged after it was indexed.
        const qint64 bId = m_lib->visibleTrackIds().at(1).toLongLong();
        QFile b(m_lib->trackById(bId)->path);
        QVERIFY(b.open(QIODevice::WriteOnly | QIODevice::Truncate));
        b.write("definitely not audio");
        b.close();

        p.playTracks(m_lib->visibleTrackIds(), rowOfTitle(*m_lib, QStringLiteral("b")));
        QCOMPARE(p.queueIndex(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(p.queueIndex(), 2, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(p.playing(), 5000);
    }

    void queueSurvivesRestart()
    {
        Player &p = *m_player;
        p.playTracks(m_lib->visibleTrackIds(), rowOfTitle(*m_lib, QStringLiteral("c")));
        QTRY_VERIFY_WITH_TIMEOUT(p.playing(), 5000);
        p.pause();
        p.seek(1.25);
        QTRY_VERIFY(p.position() >= 1.2);
        const qint64 cur = p.currentId();
        const int count = p.queueCount();
        const int index = p.queueIndex();

        m_player.reset();                                    // saves on destruction
        m_player = std::make_unique<Player>(m_lib.get());    // restores
        Player &q = *m_player;
        QCOMPARE(q.queueCount(), count);
        QCOMPARE(q.queueIndex(), index);
        QCOMPARE(q.currentId(), cur);
        QVERIFY(!q.playing());                               // never autoplays on launch
        QTRY_VERIFY_WITH_TIMEOUT(qAbs(q.position() - 1.25) < 0.1, 5000);
        q.play();
        QTRY_VERIFY_WITH_TIMEOUT(q.playing(), 5000);
        QVERIFY(q.position() >= 1.2);
    }

    void clearStops()
    {
        Player &p = *m_player;
        p.clearQueue();
        QVERIFY(!p.hasTrack());
        QTRY_VERIFY(!p.playing());
        QCOMPARE(p.currentId(), -1);
        QCOMPARE(p.current().value(QStringLiteral("t")).toString(), QString());

        m_player.reset();                                    // an empty queue stays empty
        m_player = std::make_unique<Player>(m_lib.get());
        QCOMPARE(m_player->queueCount(), 0);
        QVERIFY(!m_player->hasTrack());
    }

    void cleanupTestCase()
    {
        m_player.reset();
        m_lib.reset();
    }
};

QTEST_GUILESS_MAIN(TestPlayer)
#include "tst_player.moc"
