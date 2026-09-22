#include "librarydb.h"
#include "libraryscanner.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/tpropertymap.h>

namespace {

// Minimal PCM WAV: header + `seconds` of silence.
bool writeWav(const QString &path, int sampleRate, int bits, int channels, int seconds)
{
    const quint32 blockAlign = quint32(channels * bits / 8);
    const quint32 dataSize = quint32(sampleRate) * blockAlign * quint32(seconds);
    QByteArray h;
    const auto u32 = [&h](quint32 v) { v = qToLittleEndian(v); h.append(reinterpret_cast<const char *>(&v), 4); };
    const auto u16 = [&h](quint16 v) { v = qToLittleEndian(v); h.append(reinterpret_cast<const char *>(&v), 2); };
    h.append("RIFF");
    u32(36 + dataSize);
    h.append("WAVEfmt ");
    u32(16);
    u16(1);   // PCM
    u16(quint16(channels));
    u32(quint32(sampleRate));
    u32(quint32(sampleRate) * blockAlign);
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

bool tagFile(const QString &path, const QString &title, const QString &artist,
             const QString &album, unsigned track, const QByteArray &picture = {})
{
    TagLib::FileRef f(QFile::encodeName(path).constData());
    if (f.isNull() || !f.tag())
        return false;
    f.tag()->setTitle(TagLib::String(title.toStdString(), TagLib::String::UTF8));
    f.tag()->setArtist(TagLib::String(artist.toStdString(), TagLib::String::UTF8));
    f.tag()->setAlbum(TagLib::String(album.toStdString(), TagLib::String::UTF8));
    f.tag()->setTrack(track);
    TagLib::PropertyMap props = f.properties();
    props.replace("ALBUMARTIST", TagLib::StringList("Various"));
    props.replace("DISCNUMBER", TagLib::StringList("2/3"));
    f.setProperties(props);
    if (!picture.isEmpty()) {
        TagLib::VariantMap pic;
        pic["data"] = TagLib::ByteVector(picture.constData(), unsigned(picture.size()));
        pic["mimeType"] = TagLib::String("image/png");
        pic["pictureType"] = TagLib::String("Front Cover");
        f.setComplexProperties("PICTURE", {pic});
    }
    return f.save();
}

void bumpMtime(const QString &path)
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadWrite));
    QVERIFY(f.setFileTime(QDateTime::currentDateTime().addSecs(60), QFileDevice::FileModificationTime));
}

QHash<QString, Track> byName(LibraryDb &db)
{
    QHash<QString, Track> out;
    for (const Track &t : db.loadAll())
        out.insert(QFileInfo(t.path).fileName(), t);
    return out;
}

} // namespace

class TestScanner : public QObject
{
    Q_OBJECT

    QTemporaryDir m_tmp;
    QString m_music;
    QString m_db;
    QString m_covers;
    const QByteArray m_png = QByteArray("\x89PNG\r\n\x1a\nfake-cover-bytes", 24);

private slots:
    void initTestCase()
    {
        QVERIFY(m_tmp.isValid());
        m_music = m_tmp.filePath(QStringLiteral("music"));
        m_db = m_tmp.filePath(QStringLiteral("data/library.db"));
        m_covers = m_tmp.filePath(QStringLiteral("covers"));
        QVERIFY(QDir().mkpath(m_music + QStringLiteral("/sub")));

        QVERIFY(writeWav(m_music + QStringLiteral("/hires.wav"), 96000, 24, 2, 1));
        QVERIFY(tagFile(m_music + QStringLiteral("/hires.wav"), QStringLiteral("Glass Horizon"),
                        QStringLiteral("Aerials"), QStringLiteral("Weightless"), 2, m_png));
        QVERIFY(writeWav(m_music + QStringLiteral("/plain.wav"), 44100, 16, 2, 2));
        QVERIFY(writeWav(m_music + QStringLiteral("/sub/folderart.WAV"), 48000, 16, 1, 1));
        QFile art(m_music + QStringLiteral("/sub/Cover.JPG"));
        QVERIFY(art.open(QIODevice::WriteOnly));
        art.write("jpeg");
        art.close();
        QFile junk(m_music + QStringLiteral("/notes.txt"));
        QVERIFY(junk.open(QIODevice::WriteOnly));
        QFile fake(m_music + QStringLiteral("/not-really.mp3"));   // audio suffix, no audio
        QVERIFY(fake.open(QIODevice::WriteOnly));
        fake.write(QByteArray(4096, 'x'));
    }

    void firstScanReadsEverything()
    {
        LibraryScanner scanner(m_db, m_covers);
        const auto r = scanner.scan(m_music);
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.scanned, 4);
        QCOMPARE(r.updated, 3);
        QCOMPARE(r.removed, 0);
        QCOMPARE(r.failed, 1);
        QVERIFY(r.dirs.contains(QDir(m_music).absolutePath()));
        QVERIFY(r.dirs.contains(QDir(m_music + QStringLiteral("/sub")).absolutePath()));

        LibraryDb db;
        QVERIFY(db.open(m_db));
        const auto tracks = byName(db);
        QCOMPARE(tracks.size(), 3);

        const Track hi = tracks.value(QStringLiteral("hires.wav"));
        QCOMPARE(hi.title, QStringLiteral("Glass Horizon"));
        QCOMPARE(hi.artist, QStringLiteral("Aerials"));
        QCOMPARE(hi.album, QStringLiteral("Weightless"));
        QCOMPARE(hi.albumArtist, QStringLiteral("Various"));
        QCOMPARE(hi.trackNo, 2);
        QCOMPARE(hi.discNo, 2);
        QCOMPARE(hi.format, QStringLiteral("WAV"));
        QCOMPARE(hi.sampleRate, 96000);
        QCOMPARE(hi.bitsPerSample, 24);
        QCOMPARE(hi.durationMs, 1000);
        QVERIFY(hi.isHiRes());
        QCOMPARE(TrackFormat::sampleSpec(hi), QStringLiteral("96kHz/24-bit"));
        QVERIFY(hi.addedAt > 0);
        QVERIFY(hi.coverPath.startsWith(m_covers));
        QVERIFY(hi.coverPath.endsWith(QStringLiteral(".png")));
        QFile cover(hi.coverPath);
        QVERIFY(cover.open(QIODevice::ReadOnly));
        QCOMPARE(cover.readAll(), m_png);

        const Track plain = tracks.value(QStringLiteral("plain.wav"));
        QCOMPARE(plain.title, QStringLiteral("plain"));
        QCOMPARE(plain.artist, QStringLiteral("Unknown Artist"));
        QCOMPARE(plain.album, QStringLiteral("Unknown Album"));
        QCOMPARE(plain.durationMs, 2000);
        QVERIFY(!plain.isHiRes());
        QCOMPARE(TrackFormat::sampleSpec(plain), QStringLiteral("44.1kHz/16-bit"));
        QVERIFY(plain.coverPath.isEmpty());

        const Track sub = tracks.value(QStringLiteral("folderart.WAV"));
        QCOMPARE(sub.format, QStringLiteral("WAV"));
        QCOMPARE(sub.coverPath, QDir(m_music).absolutePath() + QStringLiteral("/sub/Cover.JPG"));
    }

    void rescanIsIncrementalAndKeepsLikes()
    {
        LibraryDb db;
        QVERIFY(db.open(m_db));
        const Track before = byName(db).value(QStringLiteral("plain.wav"));
        QVERIFY(db.setLiked(before.id, true));

        LibraryScanner scanner(m_db, m_covers);
        auto r = scanner.scan(m_music);
        QVERIFY(r.ok);
        QCOMPARE(r.updated, 0);

        QVERIFY(tagFile(m_music + QStringLiteral("/plain.wav"), QStringLiteral("Paper Suns"),
                        QStringLiteral("Kasbo Lane"), QStringLiteral("Daydream"), 1));
        bumpMtime(m_music + QStringLiteral("/plain.wav"));
        r = scanner.scan(m_music);
        QVERIFY(r.ok);
        QCOMPARE(r.updated, 1);

        const Track after = byName(db).value(QStringLiteral("plain.wav"));
        QCOMPARE(after.id, before.id);
        QCOMPARE(after.addedAt, before.addedAt);
        QVERIFY(after.liked);
        QCOMPARE(after.title, QStringLiteral("Paper Suns"));
    }

    void missingRootLeavesIndexAlone()
    {
        LibraryScanner scanner(m_db, m_covers);
        const auto r = scanner.scan(m_tmp.filePath(QStringLiteral("unmounted")));
        QVERIFY(!r.ok);
        QVERIFY(!r.error.isEmpty());
        LibraryDb db;
        QVERIFY(db.open(m_db));
        QCOMPARE(db.loadAll().size(), 3);
    }

    void cancelledScanDoesNotPrune()
    {
        QVERIFY(QFile::remove(m_music + QStringLiteral("/sub/folderart.WAV")));
        std::atomic_bool cancel = true;
        LibraryScanner scanner(m_db, m_covers);
        const auto r = scanner.scan(m_music, &cancel);
        QVERIFY(!r.ok);
        QCOMPARE(r.removed, 0);
    }

    void deletedFilesArePruned()
    {
        LibraryScanner scanner(m_db, m_covers);
        const auto r = scanner.scan(m_music);
        QVERIFY(r.ok);
        QCOMPARE(r.removed, 1);
        LibraryDb db;
        QVERIFY(db.open(m_db));
        QVERIFY(!byName(db).contains(QStringLiteral("folderart.WAV")));
    }

    void otherRootsAreKept()
    {
        // Pointing Flow at another folder must not drop the first folder's rows.
        const QString other = m_tmp.filePath(QStringLiteral("other"));
        QVERIFY(QDir().mkpath(other));
        QVERIFY(writeWav(other + QStringLiteral("/x.wav"), 44100, 16, 2, 1));
        LibraryScanner scanner(m_db, m_covers);
        const auto r = scanner.scan(other);
        QVERIFY(r.ok);
        QCOMPARE(r.removed, 0);
        LibraryDb db;
        QVERIFY(db.open(m_db));
        const auto tracks = byName(db);
        QCOMPARE(tracks.size(), 3);   // hires + plain from the first root, x from this one
        QVERIFY(tracks.contains(QStringLiteral("hires.wav")));
        QVERIFY(tracks.contains(QStringLiteral("x.wav")));
    }

    void formatting()
    {
        QCOMPARE(TrackFormat::duration(227'400), QStringLiteral("3:47"));
        QCOMPARE(TrackFormat::duration(5'000), QStringLiteral("0:05"));
        QCOMPARE(TrackFormat::bitrate(912), QStringLiteral("912 kbps"));
        QCOMPARE(TrackFormat::bitrate(2304), QStringLiteral("2.3 Mbps"));
        Track t;
        t.sampleRate = 44100;
        QCOMPARE(TrackFormat::sampleSpec(t), QStringLiteral("44.1kHz"));
        t.sampleRate = 176400;
        t.bitsPerSample = 24;
        QCOMPARE(TrackFormat::sampleSpec(t), QStringLiteral("176.4kHz/24-bit"));
    }
};

QTEST_GUILESS_MAIN(TestScanner)
#include "tst_scanner.moc"
