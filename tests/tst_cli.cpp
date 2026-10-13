// flow-cli end to end: the real binary, a fake yt-dlp, and a throwaway home, so
// the argument handling, the exit codes and the JSON the CLI promises are all
// exercised without a network connection or a display.
//
// The library the CLI drives is the app's own, so this also covers the parts of
// the download path a script depends on: a playlist becomes a mix, per-track
// status and unavailable videos land in the result, and what the summary
// reports is what the index holds.
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>
#include "fakeytdlp.h"
#include "tagpath.h"

#include <taglib/fileref.h>
#include <taglib/tag.h>

#ifndef FLOW_CLI_BIN
#error "FLOW_CLI_BIN must point at the flow-cli binary"
#endif

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
    const TagPath name = tagPath(path);
    TagLib::FileRef f(name.c_str());
    if (f.isNull() || !f.tag())
        return false;
    f.tag()->setTitle(title);
    f.tag()->setArtist("Artist");
    return f.save();
}

struct Result
{
    int code = -1;
    QByteArray out;
    QByteArray err;
    QJsonObject json() const { return QJsonDocument::fromJson(out).object(); }
};

} // namespace

class TestCli : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QString m_music;
    QString m_fake;

    Result run(const QStringList &arguments)
    {
        QProcess process;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("XDG_DATA_HOME"), m_dir.filePath(QStringLiteral("data")));
        env.insert(QStringLiteral("XDG_CACHE_HOME"), m_dir.filePath(QStringLiteral("cache")));
        env.insert(QStringLiteral("XDG_CONFIG_HOME"), m_dir.filePath(QStringLiteral("config")));
        env.insert(QStringLiteral("FLOW_FAKE_DIR"), m_fake);
        process.setProcessEnvironment(env);
        process.start(QStringLiteral(FLOW_CLI_BIN),
                      QStringList{QStringLiteral("--library"), m_music,
                                  QStringLiteral("--yt-dlp"),
                                  fakeYtDlpPath(m_fake)}
                          + arguments);
        if (!process.waitForStarted(10'000))
            return Result{-1, {}, process.errorString().toUtf8()};
        if (!process.waitForFinished(120'000)) {
            process.kill();
            process.waitForFinished(5'000);
            return Result{-2, process.readAllStandardOutput(), process.readAllStandardError()};
        }
        return Result{process.exitCode(), process.readAllStandardOutput(),
                      process.readAllStandardError()};
    }

    QVariantList listJson()
    {
        const Result result = run({QStringLiteral("list"), QStringLiteral("--json")});
        return result.json().value(QStringLiteral("tracks")).toArray().toVariantList();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_music = m_dir.filePath(QStringLiteral("music"));
        m_fake = m_dir.filePath(QStringLiteral("fake"));
        QVERIFY(QDir().mkpath(m_music));
        QVERIFY(QDir().mkpath(m_fake));
        QVERIFY(writeTaggedWav(m_fake + QStringLiteral("/one.wav"), "One"));
        QVERIFY(writeTaggedWav(m_fake + QStringLiteral("/two.wav"), "Two"));
        QVERIFY(writeTaggedWav(m_fake + QStringLiteral("/three.wav"), "Three"));
        QVERIFY(installFakeYtDlp(m_fake));
    }

    // Every test function starts from the empty library a fresh install would
    // have: the song a test downloads must not turn up in the next one's count.
    void cleanup()
    {
        for (const QString &dir : {m_music, m_dir.filePath(QStringLiteral("data")),
                                   m_dir.filePath(QStringLiteral("cache")),
                                   m_dir.filePath(QStringLiteral("config"))})
            QDir(dir).removeRecursively();
        QDir().mkpath(m_music);
    }

    void doctorAndScanReportTheLibrary()
    {
        Result result = run({QStringLiteral("doctor"), QStringLiteral("--json")});
        QCOMPARE(result.code, 0);
        const QJsonObject doctor = result.json();
        QCOMPARE(doctor.value(QStringLiteral("music")).toString(), m_music);
        QCOMPARE(doctor.value(QStringLiteral("ytdlp")).toString(),
                 fakeYtDlpPath(m_fake));
        QVERIFY(doctor.value(QStringLiteral("ffmpeg")).toString().isEmpty()
                || QFile::exists(doctor.value(QStringLiteral("ffmpeg")).toString()));

        result = run({QStringLiteral("scan"), QStringLiteral("--json")});
        QCOMPARE(result.code, 0);
        QCOMPARE(result.json().value(QStringLiteral("tracks")).toInt(), 0);   // nothing yet

        // A bad option or an unknown command is a usage error, not a crash.
        QCOMPARE(run({QStringLiteral("list"), QStringLiteral("--format"), QStringLiteral("Bogus")}).code, 2);
        QCOMPARE(run({QStringLiteral("frobnicate")}).code, 2);
        QCOMPARE(run({QStringLiteral("download")}).code, 2);
    }

    void downloadsASongIntoTheIndex()
    {
        Result result = run({QStringLiteral("--json"), QStringLiteral("download"),
                             QStringLiteral("https://music.youtube.com/watch?v=song&list=PLx")});
        QCOMPARE(result.code, 0);
        const QJsonObject json = result.json();
        QVERIFY(json.value(QStringLiteral("ok")).toBool());
        QCOMPARE(json.value(QStringLiteral("songs")).toInt(), 1);
        const QJsonArray jobs = json.value(QStringLiteral("jobs")).toArray();
        QCOMPARE(jobs.size(), 1);
        const QJsonObject job = jobs.first().toObject();
        QCOMPARE(job.value(QStringLiteral("state")).toString(), QStringLiteral("done"));
        QCOMPARE(job.value(QStringLiteral("playlist")).toBool(), false);   // v= wins over list=
        QCOMPARE(job.value(QStringLiteral("title")).toString(), QStringLiteral("One"));
        // A watch link with v= takes only the song.
        QFile args(m_fake + QStringLiteral("/args"));
        QVERIFY(args.open(QIODevice::ReadOnly));
        QVERIFY(QString::fromUtf8(args.readAll()).contains(QStringLiteral("--no-playlist")));

        const QVariantList tracks = listJson();
        QCOMPARE(tracks.size(), 1);
        QCOMPARE(tracks.first().toMap().value(QStringLiteral("title")).toString(),
                 QStringLiteral("One"));
        QCOMPARE(tracks.first().toMap().value(QStringLiteral("artist")).toString(),
                 QStringLiteral("Artist"));
        // A single song is not a mix.
        QCOMPARE(run({QStringLiteral("mixes"), QStringLiteral("--json")})
                     .json().value(QStringLiteral("count")).toInt(), 0);
    }

    void playlistBecomesAMix()
    {
        Result result = run({QStringLiteral("--json"), QStringLiteral("download"),
                             QStringLiteral("https://music.youtube.com/playlist?list=PLroad")});
        QCOMPARE(result.code, 0);   // yt-dlp exits 1 over the gap, the job still counts as done
        const QJsonObject json = result.json();
        const QJsonObject job = json.value(QStringLiteral("jobs")).toArray().first().toObject();
        QCOMPARE(job.value(QStringLiteral("state")).toString(), QStringLiteral("done"));
        QCOMPARE(job.value(QStringLiteral("playlist")).toBool(), true);
        QCOMPARE(job.value(QStringLiteral("title")).toString(), QStringLiteral("Road trip"));
        QCOMPARE(job.value(QStringLiteral("songs")).toInt(), 2);
        QCOMPARE(job.value(QStringLiteral("unavailable")).toInt(), 1);
        const QJsonArray items = job.value(QStringLiteral("items")).toArray();
        QCOMPARE(items.size(), 3);
        QCOMPARE(items.at(1).toObject().value(QStringLiteral("state")).toString(),
                 QStringLiteral("failed"));

        const QJsonArray mixes = json.value(QStringLiteral("mixes")).toArray();
        QCOMPARE(mixes.size(), 1);
        QCOMPARE(mixes.first().toObject().value(QStringLiteral("name")).toString(),
                 QStringLiteral("Road trip"));
        QCOMPARE(mixes.first().toObject().value(QStringLiteral("tracks")).toInt(), 2);

        // The tracks are in the index, in playlist order, and in the mix.
        const QVariantList tracks = listJson();
        QCOMPARE(tracks.size(), 2);
        QStringList titles;
        for (const QVariant &track : tracks)
            titles.append(track.toMap().value(QStringLiteral("title")).toString());
        QVERIFY(titles.contains(QStringLiteral("Two")));
        QVERIFY(titles.contains(QStringLiteral("Three")));

        result = run({QStringLiteral("mixes"), QStringLiteral("--tracks")});
        QCOMPARE(result.code, 0);
        const QString text = QString::fromUtf8(result.out);
        const int two = text.indexOf(QStringLiteral("Two"));
        const int three = text.indexOf(QStringLiteral("Three"));
        QVERIFY(two >= 0 && three > two);   // playlist order, not sorted
    }

    void rejectsAndFailuresHaveTheirOwnExitCodes()
    {
        Result result = run({QStringLiteral("download"), QStringLiteral("https://example.com/x.mp3")});
        QCOMPARE(result.code, 2);
        QVERIFY(result.err.contains("YouTube"));
        QVERIFY(result.out.isEmpty());

        result = run({QStringLiteral("download"), QStringLiteral("https://youtu.be/x?v=bad")});
        QCOMPARE(result.code, 1);
        QVERIFY(QString::fromUtf8(result.out).contains(QStringLiteral("failed")));

        // An option that is known but wrong is still a usage error.
        QCOMPARE(run({QStringLiteral("list"), QStringLiteral("--sort"), QStringLiteral("nope")}).code, 2);
    }
};

QTEST_GUILESS_MAIN(TestCli)
#include "tst_cli.moc"
