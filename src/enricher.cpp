#include "enricher.h"

#include "library.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJSEngine>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>

#include <cmath>

namespace {

constexpr char kUserAgent[] = "Flow/1.0 ( https://github.com/local/flow )";
constexpr char kWs[] = "https://musicbrainz.org/ws/2/";
constexpr char kCoverArt[] = "https://coverartarchive.org/release/";
constexpr int kCoverWidth = 640;
// How far a candidate recording's length may differ and still count as the
// same song. Tags are often rounded or simply wrong, so this is generous.
constexpr int kLengthSlackMs = 4000;
constexpr double kLengthSlackRatio = 0.06;
// Below this score a candidate is not the song we are looking for, however
// confident the search engine felt about it.
constexpr double kMinScore = 30.0;

QString artistCredit(const QJsonArray &credit)
{
    QStringList names;
    for (const QJsonValue &v : credit) {
        const QString name = v.toObject().value(QStringLiteral("name")).toString();
        if (!name.isEmpty())
            names.append(name);
    }
    return names.join(QStringLiteral(", "));
}

// "1997-08-12" -> 1997
int yearOf(const QString &date)
{
    if (date.size() < 4)
        return 0;
    bool ok = false;
    const int y = date.left(4).toInt(&ok);
    return ok ? y : 0;
}

// The real network. Redirects are followed (the Cover Art Archive answers with
// one) and every reply is delivered on the thread that made the request.
class NetBackend : public EnricherBackend
{
public:
    void get(const QString &url, const Reply &done) override
    {
        QNetworkRequest request{QUrl(url)};
        request.setRawHeader("User-Agent", kUserAgent);
        request.setRawHeader("Accept", "application/json, image/*");
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::NoLessSafeRedirectPolicy);
        QNetworkReply *reply = m_manager.get(request);
        QObject::connect(reply, &QNetworkReply::finished, reply, [reply, done] {
            Answer answer;
            answer.body = reply->readAll();
            if (reply->error() != QNetworkReply::NoError) {
                answer.error = reply->errorString();
                const int status =
                        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                answer.retryable = status >= 500 || status == 429 || status == 0;
            }
            reply->deleteLater();
            done(answer);
        });
    }

private:
    QNetworkAccessManager m_manager;
};

} // namespace

EnricherBackend::~EnricherBackend() = default;

QString EnrichmentResult::summary() const
{
    if (isEmpty())
        return QString();
    QStringList bits;
    if (!albumArtist.isEmpty())
        bits.append(albumArtist);
    if (year > 0)
        bits.append(QString::number(year));
    if (!genre.isEmpty())
        bits.append(genre);
    return bits.join(QStringLiteral(" · "));
}

bool EnrichmentResult::operator==(const EnrichmentResult &o) const
{
    return recordingId == o.recordingId && releaseId == o.releaseId
            && artistId == o.artistId && albumArtist == o.albumArtist && genre == o.genre
            && coverUrl == o.coverUrl && year == o.year && discNo == o.discNo
            && trackNo == o.trackNo;
}

QString Enricher::cleanName(const QString &s)
{
    QString out;
    out.reserve(s.size());
    for (const QChar c : s) {
        if (c == QLatin1Char('&')) {
            // "Mumford & Sons" and "Mumford and Sons" are the same band, and
            // only one of the two is how MusicBrainz spells it.
            if (!out.endsWith(QLatin1Char(' ')))
                out.append(QLatin1Char(' '));
            out.append(QStringLiteral("and "));
        } else if (c.isLetterOrNumber()) {
            out.append(c.toLower());
        } else if (!out.isEmpty() && !out.endsWith(QLatin1Char(' '))) {
            out.append(QLatin1Char(' '));
        }
    }
    return out.trimmed();
}

bool Enricher::isVariousArtists(const QString &name)
{
    return cleanName(name) == QStringLiteral("various artists");
}

bool Enricher::endsWithChannelWord(const QString &artist)
{
    static const QStringList suffixes = {
        QStringLiteral("official"), QStringLiteral("vevo"),  QStringLiteral("music"),
        QStringLiteral("tv"),      QStringLiteral("records"), QStringLiteral("audio"),
    };
    const QString lower = artist.trimmed().toLower();
    const bool oneWord = !lower.contains(QLatin1Char(' '));
    for (const QString &s : suffixes) {
        // "EminemMusic" and "Eminem Music" are both a channel; "Eminem" is not.
        if (oneWord ? lower.endsWith(s) : lower.endsWith(QLatin1Char(' ') + s))
            return true;
    }
    return false;
}

bool Enricher::looksLikeChannel(const QString &artist)
{
    const QString a = artist.trimmed();
    if (a.isEmpty())
        return true;
    if (endsWithChannelWord(a))
        return true;
    // "nashy9419" is a handle; "Eminem" is a band. A single word with digits in
    // it is the tell — one word is otherwise just an artist with a short name.
    return !a.contains(QLatin1Char(' ')) && a.contains(QRegularExpression(QStringLiteral("[0-9]")));
}

QString Enricher::titleArtist(const Track &t)
{
    for (const QString &sep : {QStringLiteral(" - "), QStringLiteral(" – "),
                               QStringLiteral(" — ")}) {
        const int at = t.title.indexOf(sep);
        if (at <= 0)
            continue;
        const QString head = t.title.left(at).trimmed();
        // Unlike the artist field, a single word here is a real name
        // ("Wheatus"), so only the channel words disqualify it.
        if (head.size() <= 40 && cleanName(head).size() >= 3 && !endsWithChannelWord(head))
            return head;
    }
    return QString();
}

QString Enricher::searchTitle(const Track &t)
{
    QString title = t.title.trimmed();
    // yt-dlp's convention is "Artist - Title"; the artist is also (a channel
    // name) in the artist field, so the whole thing has to come apart first.
    for (const QString &sep : {QStringLiteral(" - "), QStringLiteral(" – "),
                               QStringLiteral(" — ")}) {
        const int at = title.indexOf(sep);
        if (at <= 0)
            continue;
        const QString head = title.left(at).trimmed();
        const QString tail = title.mid(at + sep.size()).trimmed();
        if (head.size() <= 40 && cleanName(tail).size() >= 3)
            title = tail;
        break;
    }
    // "Stan (Short Version) ft. Dido" -> "Stan": a featured artist is not part
    // of the title, and leaving it in turns up nothing.
    const int ft = title.toLower().indexOf(QStringLiteral(" ft. "));
    if (ft > 0)
        title = title.left(ft).trimmed();
    // "(Short Version)", "(Remastered 2011)": the recording is called something
    // else on MusicBrainz, and the length check keeps the scorer honest.
    if (title.endsWith(QLatin1Char(')'))) {
        const int open = title.lastIndexOf(QLatin1Char('('));
        if (open > 0 && cleanName(title.left(open)).size() >= 3)
            title = title.left(open).trimmed();
    }
    return title;
}

QString Enricher::searchArtist(const Track &t)
{
    const QString artist = t.artist.trimmed();
    // The name at the front of the title beats the artist field: yt-dlp puts
    // the uploader there ("CLS Videos", "nashy9419", "WheatusOfficial"), and no
    // list of channel spellings catches them all.
    const QString fromTitle = titleArtist(t);
    if (!fromTitle.isEmpty())
        return fromTitle;
    if (!looksLikeChannel(artist))
        return artist;
    // The artist field is a channel, but yt-dlp often put the real name at the
    // front of the title; searching with it beats searching wide.
    return fromTitle;
}

bool Enricher::sameName(const QString &a, const QString &b)
{
    const QString x = cleanName(a), y = cleanName(b);
    if (x.isEmpty() || y.isEmpty())
        return false;
    const auto strip = [](const QString &v) {
        return v.startsWith(QLatin1String("the ")) ? v.mid(4) : v;
    };
    return strip(x) == strip(y);
}

EnrichmentResult Enricher::pick(const QByteArray &searchJson, const Track &track)
{
    EnrichmentResult best;
    double bestScore = 0.0;
    const QString wantedArtist = titleArtist(track);
    bool artistAgreed = false;

    const QJsonObject root = QJsonDocument::fromJson(searchJson).object();
    const QJsonArray recordings = root.value(QStringLiteral("recordings")).toArray();
    for (const QJsonValue &rv : recordings) {
        const QJsonObject rec = rv.toObject();
        const QString title = rec.value(QStringLiteral("title")).toString();
        const QString wanted = searchTitle(track);   // "Artist - Title" -> "Title"
        const QString artist = artistCredit(rec.value(QStringLiteral("artist-credit")).toArray());
        const int length = rec.value(QStringLiteral("length")).toInt();
        const QJsonArray releases = rec.value(QStringLiteral("releases")).toArray();

        double score = 0.0;
        if (sameName(title, wanted) || sameName(title, track.title)) {
            score += 60.0;
        } else {
            const QString a = cleanName(title), b = cleanName(wanted);
            if (!a.isEmpty() && !b.isEmpty() && (a.contains(b) || b.contains(a)))
                score += 20.0;
        }
        if (sameName(artist, track.artist) || sameName(artist, track.albumArtist))
            score += 25.0;
        // When the title names the artist, a candidate by anyone else is a
        // cover version, not this track: a tribute album has the same title
        // and nearly the same length.
        if (!wantedArtist.isEmpty()) {
            if (sameName(artist, wantedArtist)) {
                score += 30.0;
                artistAgreed = true;
            } else {
                score -= 30.0;
            }
        }
        if (!track.album.isEmpty()) {
            for (const QJsonValue &elv : releases) {
                if (sameName(elv.toObject().value(QStringLiteral("title")).toString(), track.album)) {
                    score += 15.0;
                    break;
                }
            }
        }
        if (track.durationMs > 0 && length > 0) {
            const int diff = std::abs(length - int(track.durationMs));
            if (diff <= kLengthSlackMs || double(diff) < double(track.durationMs) * kLengthSlackRatio)
                score += 25.0;
            else
                score -= 25.0;   // right name, wrong song: a live take or a remix
        }
        if (score <= bestScore)
            continue;

        const QJsonObject release = releases.isEmpty() ? QJsonObject() : releases.at(0).toObject();
        const QJsonArray media = release.value(QStringLiteral("media")).toArray();
        const QJsonArray mediumTracks = media.isEmpty()
                                               ? QJsonArray()
                                               : media.at(0).toObject().value(QStringLiteral("tracks")).toArray();
        const QJsonArray credit = rec.value(QStringLiteral("artist-credit")).toArray();

        bestScore = score;
        best.recordingId = rec.value(QStringLiteral("id")).toString();
        best.releaseId = release.value(QStringLiteral("id")).toString();
        best.artistId = credit.isEmpty()
                                ? QString()
                                : credit.at(0)
                                          .toObject()
                                          .value(QStringLiteral("artist"))
                                          .toObject()
                                          .value(QStringLiteral("id"))
                                          .toString();
        best.albumArtist = artistCredit(release.value(QStringLiteral("artist-credit")).toArray());
        best.compilation = isVariousArtists(best.albumArtist);
        best.year = yearOf(release.value(QStringLiteral("date")).toString());
        best.discNo = media.isEmpty() ? 0 : media.at(0).toObject().value(QStringLiteral("position")).toInt();
        best.trackNo = mediumTracks.isEmpty()
                               ? 0
                               : mediumTracks.at(0).toObject().value(QStringLiteral("position")).toInt();
        // Recomputed for every candidate: a later one with no release must not
        // inherit the cover of an earlier one.
        best.coverUrl = best.releaseId.isEmpty()
                                ? QString()
                                : QStringLiteral("%1%2/front-%3")
                                          .arg(QLatin1String(kCoverArt), best.releaseId)
                                          .arg(kCoverWidth);
    }
    if (bestScore < kMinScore)
        return EnrichmentResult();
    // We know who it is and nobody in the list is them: this is a different
    // recording that happens to share a title.
    if (!wantedArtist.isEmpty() && !artistAgreed)
        return EnrichmentResult();
    return best;
}

Enricher::Enricher(Library *library, EnricherBackend *backend, QObject *parent)
    : QObject(parent)
    , m_library(library)
    , m_backend(backend ? backend : new NetBackend)
    , m_ownsBackend(backend == nullptr)
    , m_coverDir(library ? library->coverDir() : QString())
{
    m_clock.start();
    m_lastRequestMs = -kRequestGapMs;   // the first request goes out at once
    m_gap.setSingleShot(true);
    connect(&m_gap, &QTimer::timeout, this, &Enricher::pump);
}

Enricher::~Enricher()
{
    if (m_ownsBackend)
        delete m_backend;
}

Enricher *Enricher::create(QQmlEngine *, QJSEngine *jsEngine)
{
    Q_ASSERT(s_instance);
    Q_ASSERT(jsEngine->thread() == s_instance->thread());
    QJSEngine::setObjectOwnership(s_instance, QJSEngine::CppOwnership);
    return s_instance;
}

bool Enricher::enrich(qint64 trackId, bool force)
{
    if (!m_library)
        return false;
    const Track *t = m_library->trackById(trackId);
    if (!t)
        return false;
    if (!force && t->isEnriched())
        return false;
    if (m_inFlight == trackId || m_queue.contains(trackId))
        return false;
    if (m_queue.size() >= kMaxQueue)
        return false;
    m_queue.enqueue(trackId);
    emit busyChanged();
    pump();
    return true;
}

int Enricher::enrichAll(const QList<qint64> &trackIds, bool force)
{
    int n = 0;
    for (const qint64 id : trackIds) {
        if (enrich(id, force))
            ++n;
    }
    return n;
}

void Enricher::pump()
{
    if (m_inFlight >= 0 || m_queue.isEmpty())
        return;
    // Requests are serialised: MusicBrainz asks unauthenticated clients for one
    // per second, and an album is a dozen of them.
    const qint64 sinceLast = m_clock.elapsed() - m_lastRequestMs;
    if (sinceLast < kRequestGapMs) {
        m_gap.start(int(kRequestGapMs - sinceLast));
        return;
    }
    runNext();
}

void Enricher::runNext()
{
    const qint64 id = m_queue.dequeue();
    m_inFlight = id;
    m_lastRequestMs = m_clock.elapsed();
    emit busyChanged();

    const Track *t = m_library->trackById(id);
    if (!t) {
        m_inFlight = -1;
        pump();
        return;
    }
    const Track track = *t;   // the library may change under us while we wait

    // The scanner invents a title from the filename and "Unknown Artist" when a
    // file carries no tags; searching MusicBrainz for those wastes a request on
    // every one of them.
    if (track.title.isEmpty()
        || track.artist.compare(QStringLiteral("Unknown Artist"), Qt::CaseInsensitive) == 0) {
        m_inFlight = -1;
        fail(id, QStringLiteral("%1 — no real title or artist to look up")
                        .arg(QFileInfo(track.path).fileName()));
        pump();
        return;
    }

    // A channel name in the artist field matches nothing, so the search drops
    // it and lets the title and the length do the work.
    const QString title = searchTitle(track);
    const QString artist = searchArtist(track);
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("query"),
                       artist.isEmpty() ? QStringLiteral("recording:\"%1\"").arg(title)
                                        : QStringLiteral("recording:\"%1\" AND artist:\"%2\"")
                                                  .arg(title, artist));
    query.addQueryItem(QStringLiteral("fmt"), QStringLiteral("json"));
    query.addQueryItem(QStringLiteral("limit"), QStringLiteral("10"));
    const QString url = QStringLiteral("%1recording?%2")
                                .arg(QLatin1String(kWs), query.toString(QUrl::FullyEncoded));
    m_backend->get(url, [this, id, track, artist](const EnricherBackend::Answer &answer) {
        onSearch(id, track, answer, artist);
    });
}

void Enricher::onSearch(const qint64 trackId, const Track &track,
                        const EnricherBackend::Answer &answer, const QString &searchedArtist)
{
    if (!answer.error.isEmpty()) {
        m_inFlight = -1;
        if (answer.retryable && m_retries.value(trackId) < kMaxRetries) {
            retry(trackId, answer.error);
        } else {
            fail(trackId, answer.error);
            pump();
        }
        return;
    }
    EnrichmentResult result = pick(answer.body, track);
    if (result.isEmpty() && !searchedArtist.isEmpty() && !track.title.isEmpty()) {
        // Nothing with that artist. The tag may simply spell the name the way
        // the label does not ("Mumford and Sons" against "Mumford & Sons"), so
        // the title gets one ask on its own before we call it a miss.
        QUrlQuery retry;
        retry.addQueryItem(QStringLiteral("query"),
                           QStringLiteral("recording:\"%1\"").arg(searchTitle(track)));
        retry.addQueryItem(QStringLiteral("fmt"), QStringLiteral("json"));
        retry.addQueryItem(QStringLiteral("limit"), QStringLiteral("10"));
        m_backend->get(QStringLiteral("%1recording?%2")
                               .arg(QLatin1String(kWs), retry.toString(QUrl::FullyEncoded)),
                       [this, trackId, track](const EnricherBackend::Answer &second) {
                           onSearch(trackId, track, second, QString());
                       });
        return;
    }
    if (result.isEmpty()) {
        m_inFlight = -1;
        fail(trackId, QStringLiteral("%1 — no match on MusicBrainz").arg(track.title));
        pump();
        return;
    }

    // Album artist and genre live on the release, not on the recording. They
    // are fetched once per release, so a whole album costs one extra request.
    if (!result.releaseId.isEmpty() && !knownRelease(result.releaseId)) {
        const QString url = QStringLiteral("%1release/%2?inc=genres+artist-credits&fmt=json")
                                    .arg(QLatin1String(kWs), result.releaseId);
        m_backend->get(url, [this, trackId, track, result](const EnricherBackend::Answer &a) {
            onReleaseInfo(trackId, track, result, a);
        });
        return;
    }
    if (const ReleaseInfo *known = knownRelease(result.releaseId)) {
        if (result.albumArtist.isEmpty())
            result.albumArtist = known->albumArtist;
        result.genre = known->genre;
    }
    fetchCoverOrFinish(trackId, track, result);
}

void Enricher::onReleaseInfo(const qint64 trackId, const Track &track, EnrichmentResult result,
                             const EnricherBackend::Answer &answer)
{
    if (answer.error.isEmpty()) {
        const QJsonObject release = QJsonDocument::fromJson(answer.body).object();
        ReleaseInfo info;
        info.albumArtist = artistCredit(release.value(QStringLiteral("artist-credit")).toArray());
        QStringList genres;
        const QJsonArray genreList = release.value(QStringLiteral("genres")).toArray();
        for (const QJsonValue &gv : genreList) {
            const QString name = gv.toObject().value(QStringLiteral("name")).toString();
            if (!name.isEmpty() && !genres.contains(name))
                genres.append(name);
            if (genres.size() == 3)   // a long tail nobody reads
                break;
        }
        info.genre = genres.join(QStringLiteral(", "));
        if (result.albumArtist.isEmpty())
            result.albumArtist = info.albumArtist;
        result.genre = info.genre;
        rememberRelease(result.releaseId, info);
    }
    fetchCoverOrFinish(trackId, track, result);
}

QString Enricher::cachePath(const QString &coverDir, const QString &releaseId)
{
    return QDir(coverDir).filePath(QStringLiteral("mb-%1.jpg").arg(releaseId));
}

void Enricher::onCover(const qint64 trackId, const Track &track, EnrichmentResult result,
                       const EnricherBackend::Answer &answer)
{
    if (answer.error.isEmpty() && !answer.body.isEmpty()) {
        const QString path = cachePath(m_coverDir, result.releaseId);
        if (!QFile::exists(path)) {
            QDir().mkpath(m_coverDir);
            QSaveFile file(path);
            if (file.open(QIODevice::WriteOnly)) {
                file.write(answer.body);
                file.commit();
            }
        }
        if (QFile::exists(path))
            result.coverUrl = path;   // a local path, not a URL, from here on
    }
    finishWithCoverless(trackId, track, result);
}

void Enricher::fetchCoverOrFinish(const qint64 trackId, const Track &track,
                                 const EnrichmentResult &result)
{
    if (result.coverUrl.isEmpty() || result.compilation || m_coverDir.isEmpty()) {
        finishWithCoverless(trackId, track, result);
        return;
    }
    m_backend->get(result.coverUrl, [this, trackId, track, result](const EnricherBackend::Answer &a) {
        onCover(trackId, track, result, a);
    });
}

void Enricher::finishWithCoverless(const qint64 trackId, const Track &track,
                                   const EnrichmentResult &result)
{
    m_inFlight = -1;
    Track::Enrichment e;
    e.recordingId = result.recordingId;
    e.releaseId = result.releaseId;
    e.artistId = result.artistId;
    e.albumArtist = result.compilation ? QString() : result.albumArtist;
    e.genre = result.genre;
    e.year = result.compilation ? 0 : result.year;
    e.discNo = result.discNo;
    e.trackNo = result.trackNo;
    // Until the cover is cached, coverUrl is still a web address; a local path
    // means the image is on disk and should stand in for the file's own art.
    // A compilation's cover is the compilation's, not this track's.
    e.coverPath = (result.compilation || result.coverUrl.startsWith(QLatin1String("http")))
                          ? QString()
                          : result.coverUrl;
    e.fetchedAt = int(QDateTime::currentSecsSinceEpoch());
    m_retries.remove(trackId);
    // The summary describes what was kept, not what the release said: a
    // compilation's album artist and year are not this track's.
    const QString kept = [&e] {
        QStringList bits;
        if (!e.albumArtist.isEmpty())
            bits.append(e.albumArtist);
        if (e.year > 0)
            bits.append(QString::number(e.year));
        if (!e.genre.isEmpty())
            bits.append(e.genre);
        return bits.join(QStringLiteral(" · "));
    }();
    if (m_library && m_library->applyEnrichment(trackId, e))
        finish(trackId, true, kept);
    else
        fail(trackId, QStringLiteral("%1 — could not save the result").arg(track.title));
    pump();
}

void Enricher::finish(const qint64 trackId, bool ok, const QString &summary)
{
    ++m_done;
    emit busyChanged();
    emit enriched(trackId, ok, summary);
    if (ok)
        emit notice(summary);
}

void Enricher::retry(const qint64 trackId, const QString &why)
{
    // Back to the front, so one flaky request does not push a whole album
    // behind it, and not endlessly: kMaxRetries bounds this.
    m_retries[trackId] = m_retries.value(trackId) + 1;
    m_queue.prepend(trackId);
    setError(why);
    emit busyChanged();
    pump();
}

void Enricher::fail(const qint64 trackId, const QString &why)
{
    setError(why);
    emit enriched(trackId, false, why);
}

void Enricher::setError(const QString &error)
{
    if (m_lastError == error)
        return;
    m_lastError = error;
    emit busyChanged();
}

void Enricher::rememberRelease(const QString &releaseId, const ReleaseInfo &info)
{
    if (!releaseId.isEmpty())
        m_releases.insert(releaseId, info);
}

const Enricher::ReleaseInfo *Enricher::knownRelease(const QString &releaseId) const
{
    if (releaseId.isEmpty())
        return nullptr;
    const auto it = m_releases.constFind(releaseId);
    return it == m_releases.constEnd() ? nullptr : &it.value();
}
