// Download jobs end to end against a fake yt-dlp: a shell script that speaks
// the same "flow-*" lines, writes real (tagged WAV) files and fails on cue.
#include "downloader.h"
#include "library.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

#include <taglib/fileref.h>
#include <taglib/tag.h>

namespace {

bool writeTaggedWav(const QString &path, const char *title)
{
    const quint32 rate = 8000, blockAlign = 2, dataSize = rate * blockAlign;
    QByteArray h;
    const auto u32 = [&h](quint32 v) { v = qToLittleEndian(v); h.append(reinterpret_cast<const char *>(&v), 4); };
    const auto u16 = [&h](quint16 v) { v = qToLittleEndian(v); h.append(reinterpret_cast<const char *>(&v), 2); };
    h.append("RIFF");
    u32(36 + dataSize);
    h.append("WAVEfmt ");
    u32(16);
    u16(1);
    u16(1);
    u32(rate);
    u32(rate * blockAlign);
    u16(quint16(blockAlign));
    u16(16);
    h.append("data");
    u32(dataSize);
    {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly))
            return false;
        f.write(h);
        f.write(QByteArray(dataSize, '\0'));
    }
    TagLib::FileRef f(QFile::encodeName(path).constData());
    if (f.isNull() || !f.tag())
        return false;
    f.tag()->setTitle(title);
    f.tag()->setArtist("Artist");
    return f.save();
}

// Speaks Flow's yt-dlp protocol for a few made-up links.
const char *const fakeYtDlp = R"sh(#!/bin/sh
printf '%s\n' "$@" > "$FLOW_FAKE_DIR/args"
home=
for a; do case $a in home:*) home=${a#home:} ;; esac; done
for url; do :; done
case $url in
*v=song*)
    mkdir -p "$home/Artist/Singles"
    echo 'flow-item {"title": "Song (Official Video)", "track": "One"}'
    echo 'flow-progress 50 100'
    echo 'flow-progress 100 NA'
    cp "$FLOW_FAKE_DIR/one.wav" "$home/Artist/Singles/One.wav"
    printf 'flow-file "%s"\n' "$home/Artist/Singles/One.wav"
    ;;
*list=PL*)
    mkdir -p "$home/Artist/Mixed"
    echo 'flow-item {"title": "Two", "playlist_index": 1, "n_entries": 3, "playlist_title": "Road trip"}'
    cp "$FLOW_FAKE_DIR/two.wav" "$home/Artist/Mixed/Two.wav"
    printf 'flow-file "%s"\n' "$home/Artist/Mixed/Two.wav"
    echo 'ERROR: [youtube] gone: Video unavailable' >&2
    echo 'flow-item {"title": "Three", "playlist_index": 3, "n_entries": 3, "playlist_title": "Road trip"}'
    cp "$FLOW_FAKE_DIR/three.wav" "$home/Artist/Mixed/Three.wav"
    printf 'flow-file "%s"' "$home/Artist/Mixed/Three.wav"
    exit 1
    ;;
*v=bad*)
    echo 'ERROR: [youtube] bad: Video unavailable. This video is private' >&2
    exit 1
    ;;
*v=slow*)
    echo 'flow-item {"title": "Slow"}'
    sleep 30
    ;;
esac
)sh";

} // namespace

class TestDownloader : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QString m_music;
    QString m_fake;
    QString m_temp;
    Library *m_library = nullptr;
    Downloader *m_downloader = nullptr;

    QVariant role(int row, int role) const
    {
        return m_downloader->data(m_downloader->index(row), role);
    }
    QString state(int row) const { return role(row, Downloader::State).toString(); }
    QString detail(int row) const { return role(row, Downloader::Detail).toString(); }
    QString args() const
    {
        QFile f(m_fake + QStringLiteral("/args"));
        return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
    }
    qint64 idOf(const QString &title) const
    {
        for (const Track &t : m_library->tracks()) {
            if (t.title == title)
                return t.id;
        }
        return -1;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_music = m_dir.filePath(QStringLiteral("music"));
        m_fake = m_dir.filePath(QStringLiteral("fake"));
        m_temp = m_dir.filePath(QStringLiteral("cache/yt-dlp"));
        QVERIFY(QDir().mkpath(m_music));
        QVERIFY(QDir().mkpath(m_fake));
        QVERIFY(writeTaggedWav(m_fake + QStringLiteral("/one.wav"), "One"));
        QVERIFY(writeTaggedWav(m_fake + QStringLiteral("/two.wav"), "Two"));
        QVERIFY(writeTaggedWav(m_fake + QStringLiteral("/three.wav"), "Three"));
        QFile script(m_fake + QStringLiteral("/yt-dlp"));
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write(fakeYtDlp);
        script.close();
        QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        qputenv("FLOW_FAKE_DIR", QFile::encodeName(m_fake));

        m_library = new Library(m_music, m_dir.filePath(QStringLiteral("library.db")),
                                m_dir.filePath(QStringLiteral("covers")), this);
        QTRY_VERIFY(!m_library->scanning());
        m_downloader = new Downloader(m_library, m_temp, script.fileName(), this);
        QVERIFY(m_downloader->available());
        QCOMPARE(m_downloader->destination(), m_music + QStringLiteral("/YouTube"));
    }

    void rejectsOtherLinks()
    {
        QVERIFY(!m_downloader->add(QStringLiteral("https://example.com/song.mp3"), false).isEmpty());
        QVERIFY(!m_downloader->add(QStringLiteral("a song name"), false).isEmpty());
        QCOMPARE(m_downloader->rowCount(), 0);
    }

    void downloadsASong()
    {
        QSignalSpy notices(m_downloader, &Downloader::notice);
        QCOMPARE(m_downloader->add(QStringLiteral("music.youtube.com/watch?v=song"), false), QString());
        QCOMPARE(m_downloader->rowCount(), 1);
        QVERIFY(m_downloader->busy());
        QTRY_COMPARE(state(0), QStringLiteral("done"));
        QVERIFY(!m_downloader->busy());
        QCOMPARE(role(0, Downloader::Title).toString(), QStringLiteral("One"));
        QCOMPARE(role(0, Downloader::Progress).toDouble(), 1.0);
        QCOMPARE(detail(0), QStringLiteral("1 song"));
        QCOMPARE(notices.size(), 1);
        QCOMPARE(notices.first().first().toString(), QStringLiteral("Downloaded One"));

        const QStringList a = args().split(u'\n');
        QVERIFY(a.contains(QStringLiteral("--no-playlist")));
        QVERIFY(a.contains(QStringLiteral("https://music.youtube.com/watch?v=song")));
        QVERIFY(a.contains(QStringLiteral("home:") + m_music + QStringLiteral("/YouTube")));

        // The rescan puts it in the library; no mix for a single song.
        QTRY_VERIFY(idOf(QStringLiteral("One")) >= 0);
        QCOMPARE(m_library->mixModel()->rowCount(), 0);
        // Its working folder is gone.
        QVERIFY(QDir(m_temp).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
    }

    void playlistBecomesAMix()
    {
        QSignalSpy mixes(m_library, &Library::mixAdded);
        QSignalSpy notices(m_downloader, &Downloader::notice);
        QCOMPARE(m_downloader->add(QStringLiteral("https://music.youtube.com/playlist?list=PLroad"), true),
                 QString());
        QTRY_COMPARE(state(0), QStringLiteral("done"));
        QCOMPARE(role(0, Downloader::Title).toString(), QStringLiteral("Road trip"));
        QCOMPARE(detail(0), QStringLiteral("2 songs · 1 unavailable"));   // exit 1, but files came
        QVERIFY(args().split(u'\n').contains(QStringLiteral("--yes-playlist")));

        QTRY_COMPARE(mixes.size(), 1);
        const qint64 mixId = mixes.first().at(0).toLongLong();
        QVERIFY(mixId >= 0);
        QCOMPARE(mixes.first().at(1).toString(), QStringLiteral("Road trip"));
        QCOMPARE(mixes.first().at(2).toInt(), 2);
        QCOMPARE(m_library->mixTrackIds(mixId),
                 (QVariantList{idOf(QStringLiteral("Two")), idOf(QStringLiteral("Three"))}));
        QCOMPARE(notices.last().first().toString(), QStringLiteral("Mix “Road trip” · 2 songs"));
    }

    void reportsFailures()
    {
        QSignalSpy notices(m_downloader, &Downloader::notice);
        QCOMPARE(m_downloader->add(QStringLiteral("https://youtu.be/x?v=bad"), false), QString());
        QTRY_COMPARE(state(0), QStringLiteral("failed"));
        QCOMPARE(detail(0), QStringLiteral("Video unavailable. This video is private"));
        QCOMPARE(notices.size(), 1);
        QVERIFY(notices.first().first().toString().startsWith(QStringLiteral("Download failed")));
    }

    void queuesAndCancels()
    {
        QCOMPARE(m_downloader->add(QStringLiteral("https://www.youtube.com/watch?v=slow"), false), QString());
        QCOMPARE(m_downloader->add(QStringLiteral("https://www.youtube.com/watch?v=slow"), false),
                 QStringLiteral("Already downloading"));
        QCOMPARE(m_downloader->add(QStringLiteral("https://www.youtube.com/watch?v=song2"), false), QString());
        // Newest first: the second job waits behind the slow one.
        QCOMPARE(state(0), QStringLiteral("queued"));
        QCOMPARE(detail(0), QStringLiteral("Waiting"));
        QTRY_COMPARE(detail(1), QStringLiteral("Slow"));

        const int queuedId = role(0, Downloader::JobId).toInt();
        m_downloader->cancel(queuedId);
        QCOMPARE(state(0), QStringLiteral("canceled"));

        QElapsedTimer timer;
        timer.start();
        m_downloader->cancel(role(1, Downloader::JobId).toInt());
        QTRY_COMPARE(state(1), QStringLiteral("canceled"));
        QVERIFY(timer.elapsed() < 10'000);   // the whole group died, not just sh
        QVERIFY(!m_downloader->busy());

        const int before = m_downloader->rowCount();
        m_downloader->clearFinished();
        QCOMPARE(before, 5);
        QCOMPARE(m_downloader->rowCount(), 0);
    }
};

QTEST_GUILESS_MAIN(TestDownloader)
#include "tst_downloader.moc"
