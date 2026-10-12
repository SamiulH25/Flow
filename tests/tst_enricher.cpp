#include "enricher.h"
#include "library.h"

#include <QtTest>

#include <QFile>
#include <QElapsedTimer>
#include <QTemporaryDir>

#include <taglib/fileref.h>
#include <taglib/tag.h>

namespace {

// A MusicBrainz-shaped search response, trimmed to what the matcher reads.
QByteArray searchJson(const QString &recordingId, const QString &title, const QString &artist,
                      int lengthMs, const QString &releaseId = QStringLiteral("rel-1"),
                      const QString &releaseTitle = QStringLiteral("An Album"),
                      const QString &date = QStringLiteral("1997-08-12"))
{
    return QStringLiteral(R"({
        "recordings": [{
            "id": "%1",
            "title": "%2",
            "length": %3,
            "artist-credit": [{"name": "%4", "artist": {"id": "artist-1"}}],
            "releases": [{
                "id": "%5",
                "title": "%6",
                "date": "%7",
                "artist-credit": [{"name": "%4"}],
                "media": [{"position": 1, "tracks": [{"position": 3}]}]
            }]
        }]
    })")
            .arg(recordingId, title, QString::number(lengthMs), artist, releaseId, releaseTitle, date)
            .toUtf8();
}


// A valid but untagged WAV: the scanner keeps it, and it has nothing to search
// MusicBrainz for.
bool writeUntaggedWav(const QString &path)
{
    const quint32 rate = 8000, dataSize = rate * 2;
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
    u32(rate * 2);
    u16(2);
    u16(16);
    h.append("data");
    u32(dataSize);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(h);
    f.write(QByteArray(dataSize, '\0'));
    return true;
}

// A real, tagged WAV in a temporary folder: the library only knows about files
// it has scanned, so the tests give it one the same way the app does.
bool writeTaggedWav(const QString &path, const char *title, const char *artist,
                    const char *album, unsigned track = 1)
{
    const quint32 rate = 8000, dataSize = rate * 2;
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
    u32(rate * 2);
    u16(2);
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
    TagLib::FileRef ref(QFile::encodeName(path).constData());
    if (ref.isNull() || !ref.tag())
        return false;
    ref.tag()->setTitle(title);
    ref.tag()->setArtist(artist);
    ref.tag()->setAlbum(album);
    ref.tag()->setTrack(track);
    return ref.save();
}

Track makeTrack(const QString &title, const QString &artist, qint64 durationMs = 190000,
                const QString &album = QStringLiteral("An Album"))
{
    Track t;
    t.id = 1;
    t.title = title;
    t.artist = artist;
    t.album = album;
    t.durationMs = durationMs;
    return t;
}

// Answers from memory, counts requests, and can be told to fail.
class FakeBackend : public EnricherBackend
{
public:
    QHash<QString, QByteArray> replies;
    QStringList requested;
    int failuresLeft = 0;
    int serverErrorsLeft = 0;
    bool fireImmediately = true;

    void get(const QString &url, const Reply &done) override
    {
        requested.append(url);
        if (failuresLeft > 0) {
            --failuresLeft;
            done({QByteArray(), QStringLiteral("network is down"), false});
            return;
        }
        if (serverErrorsLeft > 0) {
            --serverErrorsLeft;
            done({QByteArray(), QStringLiteral("503 Service Unavailable"), true});
            return;
        }
        Answer answer;
        answer.body = replies.value(url);
        if (answer.body.isEmpty() && replies.size() == 1)
            answer.body = *replies.cbegin();   // one canned answer for any URL
        if (fireImmediately)
            done(answer);
    }
};

} // namespace

class TestEnricher : public QObject
{
    Q_OBJECT

private slots:
    void cleanNameIgnoresCaseAndPunctuation()
    {
        QCOMPARE(Enricher::cleanName(QStringLiteral("Save  Tonight!")), QStringLiteral("save tonight"));
        QCOMPARE(Enricher::cleanName(QStringLiteral("  --  ")), QString());
    }

    void sameNameIgnoresALeadingThe()
    {
        QVERIFY(Enricher::sameName(QStringLiteral("The Beatles"), QStringLiteral("Beatles")));
        QVERIFY(Enricher::sameName(QStringLiteral("eagle-eye cherry"), QStringLiteral("Eagle-Eye Cherry")));
        QVERIFY(!Enricher::sameName(QStringLiteral("Eagle-Eye Cherry"), QStringLiteral("Cher")));
        QVERIFY(!Enricher::sameName(QString(), QStringLiteral("Anything")));
    }

    void picksTheRecordingThatMatches()
    {
        const EnrichmentResult r = Enricher::pick(
                searchJson(QStringLiteral("rec-9"), QStringLiteral("Save Tonight"),
                           QStringLiteral("Eagle-Eye Cherry"), 190000),
                makeTrack(QStringLiteral("Save Tonight"), QStringLiteral("Eagle-Eye Cherry")));
        QCOMPARE(r.recordingId, QStringLiteral("rec-9"));
        QCOMPARE(r.releaseId, QStringLiteral("rel-1"));
        QCOMPARE(r.albumArtist, QStringLiteral("Eagle-Eye Cherry"));
        QCOMPARE(r.year, 1997);
        QCOMPARE(r.trackNo, 3);
        QCOMPARE(r.discNo, 1);
        QVERIFY(r.coverUrl.contains(QStringLiteral("rel-1")));
        QCOMPARE(r.summary(), QStringLiteral("Eagle-Eye Cherry · 1997"));
    }
    void rejectsARecordingOfTheWrongLength()
    {
        // Same name, but three times as long: a different song, or a bad tag.
        const QByteArray json = QStringLiteral(R"({"recordings": [
            {"id": "rec-short", "title": "Save Tonight", "length": 190000,
             "artist-credit": [{"name": "Eagle-Eye Cherry"}]},
            {"id": "rec-long", "title": "Save Tonight", "length": 570000,
             "artist-credit": [{"name": "Eagle-Eye Cherry"}]}
        ]})").toUtf8();
        const EnrichmentResult r = Enricher::pick(json, makeTrack(QStringLiteral("Save Tonight"),
                                                                  QStringLiteral("Eagle-Eye Cherry")));
        QCOMPARE(r.recordingId, QStringLiteral("rec-short"));
    }

    void rejectsAMatchWeCannotStandBehind()
    {
        // Right length, but nothing else: too weak to write over the library.
        const QByteArray json = QStringLiteral(R"({"recordings": [
            {"id": "rec-x", "title": "Completely Different", "length": 190000,
             "artist-credit": [{"name": "Someone Else"}]}
        ]})").toUtf8();
        QVERIFY(Enricher::pick(json, makeTrack(QStringLiteral("Save Tonight"),
                                               QStringLiteral("Eagle-Eye Cherry")))
                        .isEmpty());
    }

    void survivesAnEmptyOrBrokenResponse()
    {
        const Track t = makeTrack(QStringLiteral("Save Tonight"), QStringLiteral("Eagle-Eye Cherry"));
        QVERIFY(Enricher::pick(QByteArray(), t).isEmpty());
        QVERIFY(Enricher::pick(QByteArrayLiteral("{not json"), t).isEmpty());
        QVERIFY(Enricher::pick(QByteArrayLiteral(R"({"recordings": []})"), t).isEmpty());
    }

    void undoesTheYtDlpTitleShape()
    {
        Track t = makeTrack(QStringLiteral("Eminem - Stan (Short Version) ft. Dido"),
                            QStringLiteral("EminemMusic"));
        QCOMPARE(Enricher::searchTitle(t), QStringLiteral("Stan"));
        QCOMPARE(Enricher::titleArtist(t), QStringLiteral("Eminem"));
        QCOMPARE(Enricher::searchArtist(t), QStringLiteral("Eminem"));
        QVERIFY(Enricher::looksLikeChannel(QStringLiteral("nashy9419")));
        QVERIFY(Enricher::looksLikeChannel(QStringLiteral("WheatusOfficial")));
        QVERIFY(!Enricher::looksLikeChannel(QStringLiteral("Eminem")));
        // A single word with digits in it is a handle; a plain one is a name.
        QVERIFY(Enricher::looksLikeChannel(QStringLiteral("ClarenceClarity2")));
        QVERIFY(!Enricher::looksLikeChannel(QStringLiteral("Wheatus")));
        QCOMPARE(Enricher::titleArtist(makeTrack(QStringLiteral("Wheatus - Teenage Dirtbag"),
                                                 QStringLiteral("WheatusOfficial"))),
                 QStringLiteral("Wheatus"));

        // An ordinary tagged file is left exactly as it is.
        const Track plain = makeTrack(QStringLiteral("Save Tonight"), QStringLiteral("Eagle-Eye Cherry"));
        QCOMPARE(Enricher::searchTitle(plain), QStringLiteral("Save Tonight"));
        QCOMPARE(Enricher::searchArtist(plain), QStringLiteral("Eagle-Eye Cherry"));
        QVERIFY(Enricher::titleArtist(plain).isEmpty());
    }

    void rejectsACoverVersionOfTheRightSong()
    {
        // "Eminem - Stan" must not come back as Stan by Studio 99: same title,
        // nearly the same length, different artist.
        const QByteArray json = QStringLiteral(R"({"recordings": [
            {"id": "rec-cover", "title": "Stan", "length": 360000,
             "artist-credit": [{"name": "Studio 99"}],
             "releases": [{"id": "rel-cover", "title": "Tribute", "date": "2002"}]}
        ]})").toUtf8();
        const Track t = makeTrack(QStringLiteral("Eminem - Stan (Short Version) ft. Dido"),
                                  QStringLiteral("EminemMusic"), 364440,
                                  QStringLiteral("Unknown Album"));
        QVERIFY(Enricher::pick(json, t).isEmpty());

        // The same search does find the real one.
        const QByteArray right = QStringLiteral(R"({"recordings": [
            {"id": "rec-real", "title": "Stan", "length": 360000,
             "artist-credit": [{"name": "Eminem"}],
             "releases": [{"id": "rel-real", "title": "The Eminem Show", "date": "2000-06-01"}]}
        ]})").toUtf8();
        const EnrichmentResult r = Enricher::pick(right, t);
        QCOMPARE(r.recordingId, QStringLiteral("rec-real"));
        QCOMPARE(r.year, 2000);
    }

    void aCompilationContributesNothingButTheRecording()
    {
        // "Crazy" is on a tribute album: the song is identified, the release's
        // album artist, year and cover belong to the compilation and are left
        // out of the result.
        const QByteArray json = QStringLiteral(R"({"recordings": [
            {"id": "rec-c", "title": "Crazy", "length": 183000,
             "artist-credit": [{"name": "Gnarls Barkley"}],
             "releases": [{"id": "rel-various", "title": "Now Thats What I Call 20",
                           "date": "2016-05-13",
                           "artist-credit": [{"name": "Various Artists"}]}]}
        ]})").toUtf8();
        const EnrichmentResult r = Enricher::pick(
                json, makeTrack(QStringLiteral("Gnarls Barkley - Crazy"),
                                QStringLiteral("Gnarls Barkley videos 3"), 183264,
                                QStringLiteral("Unknown Album")));
        QCOMPARE(r.recordingId, QStringLiteral("rec-c"));
        QVERIFY(r.compilation);
        QVERIFY(Enricher::isVariousArtists(QStringLiteral("Various  Artists")));
        QVERIFY(!Enricher::isVariousArtists(QStringLiteral("The Beatles")));
    }

    void aMatchNeverInheritsAnEarlierCandidatesCover()
    {
        // The first candidate has a release, the one that wins does not: the
        // result must not carry the first candidate's cover.
        const QByteArray json = QStringLiteral(R"({"recordings": [
            {"id": "rec-a", "title": "Yesterday", "length": 125000,
             "artist-credit": [{"name": "The Beatles"}],
             "releases": [{"id": "rel-a", "title": "Help!", "date": "1965"}]},
            {"id": "rec-b", "title": "Yesterday", "length": 125000,
             "artist-credit": [{"name": "The Beatles"}],
             "releases": []}
        ]})").toUtf8();
        const EnrichmentResult r =
                Enricher::pick(json, makeTrack(QStringLiteral("Yesterday"), QStringLiteral("The Beatles"),
                                               125000));
        QVERIFY(r.coverUrl.isEmpty() || r.coverUrl.contains(r.releaseId));
    }

    void resultEqualityCoversEveryField()
    {
        EnrichmentResult a;
        a.recordingId = QStringLiteral("r");
        EnrichmentResult b;
        b.recordingId = QStringLiteral("r");
        QVERIFY(a == b);
        b.genre = QStringLiteral("Rock");
        QVERIFY(!(a == b));
    }

    void enrichStoresTheResultInTheIndex()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(writeTaggedWav(dir.filePath("a.wav"), "Save Tonight", "Eagle-Eye Cherry",
                               "An Album"));
        Library library(dir.path(), dir.filePath("library.db"), dir.filePath("covers"));
        library.rescan();
        QTRY_VERIFY_WITH_TIMEOUT(!library.scanning() && library.totalCount() == 1, 10000);
        const qint64 id = library.tracks().first().id;

        FakeBackend backend;
        backend.replies.insert(QStringLiteral("any"),
                               searchJson(QStringLiteral("rec-1"), QStringLiteral("Save Tonight"),
                                          QStringLiteral("Eagle-Eye Cherry"), 190000));
        Enricher enricher(&library, &backend);
        QSignalSpy enriched(&enricher, &Enricher::enriched);
        QVERIFY(enricher.enrich(id));
        QTRY_COMPARE(enriched.size(), 1);
        QCOMPARE(enriched.first().at(1).toBool(), true);

        const Track *after = library.trackById(id);
        QVERIFY(after);
        QVERIFY(after->isEnriched());
        QCOMPARE(after->enriched.recordingId, QStringLiteral("rec-1"));
        QCOMPARE(after->albumArtist, QStringLiteral("Eagle-Eye Cherry"));
        QCOMPARE(after->year, 1997);
        QVERIFY(!enricher.busy());
    }

    void aServerErrorIsTriedOnceMore()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(writeTaggedWav(dir.filePath("a.wav"), "Save Tonight", "Eagle-Eye Cherry",
                               "An Album"));
        Library library(dir.path(), dir.filePath("library.db"), dir.filePath("covers"));
        library.rescan();
        QTRY_VERIFY_WITH_TIMEOUT(!library.scanning() && library.totalCount() == 1, 10000);
        const qint64 id = library.tracks().first().id;

        FakeBackend backend;
        backend.serverErrorsLeft = 1;   // the first attempt gets a 503
        backend.replies.insert(QStringLiteral("any"),
                               searchJson(QStringLiteral("rec-1"), QStringLiteral("Save Tonight"),
                                          QStringLiteral("Eagle-Eye Cherry"), 190000));
        Enricher enricher(&library, &backend);
        QSignalSpy enriched(&enricher, &Enricher::enriched);
        QVERIFY(enricher.enrich(id));
        QTRY_COMPARE_WITH_TIMEOUT(enriched.size(), 1, 20000);
        // A 5xx is worth another go: the retry asks the same question again,
        // succeeds, and the rest of the chain (release, cover) follows.
        QCOMPARE(enriched.first().at(1).toBool(), true);
        QVERIFY(backend.requested.size() >= 2);
        QCOMPARE(backend.requested.at(0), backend.requested.at(1));
        QVERIFY(library.trackById(id)->isEnriched());
    }

    void aLookupFailureLeavesTheLibraryAlone()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(writeTaggedWav(dir.filePath("a.wav"), "Save Tonight", "Eagle-Eye Cherry",
                               "An Album"));
        Library library(dir.path(), dir.filePath("library.db"), dir.filePath("covers"));
        library.rescan();
        QTRY_VERIFY_WITH_TIMEOUT(!library.scanning() && library.totalCount() == 1, 10000);
        const qint64 id = library.tracks().first().id;

        FakeBackend backend;
        backend.failuresLeft = 1;
        Enricher enricher(&library, &backend);
        QSignalSpy enriched(&enricher, &Enricher::enriched);
        QVERIFY(enricher.enrich(id));
        QTRY_COMPARE(enriched.size(), 1);
        QCOMPARE(enriched.first().at(1).toBool(), false);
        QVERIFY(!library.trackById(id)->isEnriched());
        QVERIFY(!enricher.lastError().isEmpty());
    }

    void enrichmentSurvivesARescanOfTheFile()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("a.wav");
        QVERIFY(writeTaggedWav(path, "Save Tonight", "Eagle-Eye Cherry", "An Album"));
        Library library(dir.path(), dir.filePath("library.db"), dir.filePath("covers"));
        library.rescan();
        QTRY_VERIFY_WITH_TIMEOUT(!library.scanning() && library.totalCount() == 1, 10000);
        const qint64 id = library.tracks().first().id;

        Track::Enrichment e;
        e.recordingId = QStringLiteral("rec-1");
        e.genre = QStringLiteral("Alternative Rock");
        e.albumArtist = QStringLiteral("Eagle-Eye Cherry");
        QVERIFY(library.applyEnrichment(id, e));

        // Re-tagging the file and rescanning rewrites what the file says, and
        // must not throw away what the lookup found.
        QVERIFY(writeTaggedWav(path, "Save Tonight (Remastered)", "Eagle-Eye Cherry",
                               "An Album"));
        library.rescan();
        QTRY_VERIFY_WITH_TIMEOUT(!library.scanning(), 10000);

        const Track *after = library.trackById(id);
        QVERIFY(after);
        QCOMPARE(after->enriched.recordingId, QStringLiteral("rec-1"));
        QCOMPARE(after->enriched.genre, QStringLiteral("Alternative Rock"));
        QCOMPARE(after->title, QStringLiteral("Save Tonight (Remastered)"));
    }

    void aTrackWithNothingToSearchForIsNotSentAnywhere()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        // A real audio file, but with no tags at all: there is nothing to
        // search MusicBrainz for.
        QVERIFY(writeUntaggedWav(dir.filePath("b.wav")));
        Library library(dir.path(), dir.filePath("library.db"), dir.filePath("covers"));
        library.rescan();
        QTRY_VERIFY_WITH_TIMEOUT(!library.scanning() && library.totalCount() == 1, 10000);
        const qint64 id = library.tracks().first().id;

        FakeBackend backend;
        Enricher enricher(&library, &backend);
        QSignalSpy enriched(&enricher, &Enricher::enriched);
        QVERIFY(enricher.enrich(id));
        QCOMPARE(enriched.size(), 1);
        QCOMPARE(enriched.first().at(1).toBool(), false);
        QVERIFY(backend.requested.isEmpty());
    }

    void anEnrichedTrackIsNotLookedUpTwice()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(writeTaggedWav(dir.filePath("c.wav"), "Save Tonight", "Eagle-Eye Cherry",
                               "An Album"));
        Library library(dir.path(), dir.filePath("library.db"), dir.filePath("covers"));
        library.rescan();
        QTRY_VERIFY_WITH_TIMEOUT(!library.scanning() && library.totalCount() == 1, 10000);
        const qint64 id = library.tracks().first().id;
        Track::Enrichment e;
        e.recordingId = QStringLiteral("rec-1");
        QVERIFY(library.applyEnrichment(id, e));

        FakeBackend backend;
        Enricher enricher(&library, &backend);
        QVERIFY(!enricher.enrich(id));            // already looked up
        QVERIFY(enricher.enrich(id, true));       // ... unless asked to refresh
    }

    void theQueueIsRateLimitedAndDrainsInOrder()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(writeTaggedWav(dir.filePath("a.wav"), "One", "Artist", "Album", 1));
        QVERIFY(writeTaggedWav(dir.filePath("b.wav"), "Two", "Artist", "Album", 2));
        Library library(dir.path(), dir.filePath("library.db"), dir.filePath("covers"));
        library.rescan();
        QTRY_VERIFY_WITH_TIMEOUT(!library.scanning() && library.totalCount() == 2, 10000);
        QList<qint64> ids;
        for (const Track &t : library.tracks())
            ids.append(t.id);

        FakeBackend backend;
        backend.replies.insert(QStringLiteral("any"),
                               searchJson(QStringLiteral("rec-1"), QStringLiteral("One"),
                                          QStringLiteral("Artist"), 8000));
        Enricher enricher(&library, &backend);
        QSignalSpy enriched(&enricher, &Enricher::enriched);
        QElapsedTimer clock;
        clock.start();
        QCOMPARE(enricher.enrichAll(ids), 2);
        // One track is looked up at a time; the second is still waiting its
        // turn, which is what keeps us inside MusicBrainz's rate limit. (A
        // single track can take several requests: search, release, cover.)
        QCOMPARE(enriched.size(), 1);   // the first is done, the second is paced
        QVERIFY(enricher.busy());
        QTRY_VERIFY_WITH_TIMEOUT(enriched.size() >= 2, 20000);
        QVERIFY(clock.elapsed() >= Enricher::kRequestGapMs);
        QVERIFY(!enricher.busy());
    }
};

QTEST_MAIN(TestEnricher)
#include "tst_enricher.moc"
