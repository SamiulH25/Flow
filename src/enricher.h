#pragma once

#include "track.h"

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <QQueue>
#include <QString>
#include <QTimer>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <memory>

#include <functional>

class Library;
class QJSEngine;
class QQmlEngine;
class QNetworkAccessManager;

// How the enricher talks to the network. The real one is QNetworkAccessManager;
// tests hand in a fake that answers from memory, so nothing they do needs a
// network connection.
class EnricherBackend
{
public:
    virtual ~EnricherBackend();
    // Fetches `url`; calls back exactly once. Always on the thread the backend
    // was made on.
    struct Answer {
        QByteArray body;
        QString error;
        // 5xx, timeouts, a dropped connection: the same request may well work
        // in a moment, so the enricher gives it one more go.
        bool retryable = false;
    };
    using Reply = std::function<void(const Answer &answer)>;
    virtual void get(const QString &url, const Reply &done) = 0;
};

// One track's worth of what MusicBrainz and the Cover Art Archive know.
struct EnrichmentResult {
    QString recordingId;
    QString releaseId;
    QString artistId;
    QString albumArtist;
    QString genre;
    QString coverUrl;
    int year = 0;
    int discNo = 0;
    int trackNo = 0;
    // The song is right but the release is a compilation: its album artist,
    // year and cover belong to the compilation, not to this track.
    bool compilation = false;

    bool isEmpty() const { return recordingId.isEmpty(); }
    // "Eagle-Eye Cherry · 1997 · Rock", or an empty string when nothing matched.
    QString summary() const;
    bool operator==(const EnrichmentResult &o) const;
};

// Fills gaps in the library's metadata from MusicBrainz (recording search,
// artist and release credits, genres) and downloads front covers from the
// Cover Art Archive. Nothing is ever written back to the audio files: the
// result lands in the index, in the `enrichment` table, and a rescan keeps it.
//
// Requests are serialised at one per kRequestGapMs, which is what MusicBrainz
// asks of unauthenticated clients, so a whole album takes a while but never
// trips the rate limit.
class Enricher : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(int pending READ pending NOTIFY busyChanged)
    Q_PROPERTY(int doneCount READ doneCount NOTIFY busyChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY busyChanged)

public:
    // MusicBrainz allows one request per second without an account.
    static constexpr int kRequestGapMs = 1100;
    static constexpr int kMaxQueue = 500;   // beyond this, the drop is silent
    static constexpr int kMaxRetries = 1;   // per track, for transient failures

    explicit Enricher(Library *library, EnricherBackend *backend = nullptr,
                      QObject *parent = nullptr);
    ~Enricher() override;

    static Enricher *create(QQmlEngine *qmlEngine, QJSEngine *jsEngine);
    static void setInstance(Enricher *enricher) { s_instance = enricher; }

    bool busy() const { return m_inFlight >= 0 || !m_queue.isEmpty(); }
    int pending() const { return m_queue.size() + (m_inFlight >= 0 ? 1 : 0); }
    int doneCount() const { return m_done; }
    QString lastError() const { return m_lastError; }

    // Queues one track. Returns false when it is already queued, already
    // enriched (without `force`), or unknown.
    Q_INVOKABLE bool enrich(qint64 trackId, bool force = false);
    // Queues several (an album, or everything a download just produced).
    // Returns how many were accepted.
    int enrichAll(const QList<qint64> &trackIds, bool force = false);

    // What the lookup would pick out of a search response, and what we make of
    // it. Static and network-free so the matching rules can be tested.
    static EnrichmentResult pick(const QByteArray &searchJson, const Track &track);
    // "Eagle-Eye Cherry" and "Eagle-Eye Cherry (feat. X)" should not look like
    // two different artists; punctuation, case and "the" are ignored.
    static bool sameName(const QString &a, const QString &b);
    static QString cleanName(const QString &s);

    // yt-dlp writes "Artist - Title" with the channel name as the artist, and
    // that combination matches nothing on MusicBrainz. Both shapes are undone
    // before searching; the scorer still has to agree on title and length.
    static QString searchTitle(const Track &t);
    static QString searchArtist(const Track &t);
    // The artist at the front of "Eminem - Stan", which is the real name even
    // when the artist field holds a channel handle.
    static QString titleArtist(const Track &t);
    static bool looksLikeChannel(const QString &artist);
    static bool endsWithChannelWord(const QString &artist);
    static bool isVariousArtists(const QString &name);
signals:
    void busyChanged();
    void enriched(qint64 trackId, bool ok, const QString &summary);
    // One line for a toast.
    void notice(const QString &text);

private:
    void pump();
    void runNext();
    // `searchedArtist` is non-empty when the artist-constrained search came back
    // empty, which is what triggers one title-only retry.
    void onSearch(const qint64 trackId, const Track &track, const EnricherBackend::Answer &answer,
                  const QString &searchedArtist = QString());
    void onReleaseInfo(const qint64 trackId, const Track &track, EnrichmentResult result,
                       const EnricherBackend::Answer &answer);
    void onCover(const qint64 trackId, const Track &track, EnrichmentResult result,
                 const EnricherBackend::Answer &answer);
    void finish(const qint64 trackId, bool ok, const QString &summary);
    void fail(const qint64 trackId, const QString &why);
    // Puts a track back at the front of the queue after a transient failure.
    void retry(const qint64 trackId, const QString &why);
    void setError(const QString &error);
    void fetchCoverOrFinish(const qint64 trackId, const Track &track,
                            const EnrichmentResult &result);
    void finishWithCoverless(const qint64 trackId, const Track &track,
                             const EnrichmentResult &result);
    // releaseId -> album artist + genre, so a whole album costs one extra
    // request instead of one per track.
    struct ReleaseInfo {
        QString albumArtist;
        QString genre;
    };
    void rememberRelease(const QString &releaseId, const ReleaseInfo &info);
    const ReleaseInfo *knownRelease(const QString &releaseId) const;
    static QString cachePath(const QString &coverDir, const QString &releaseId);

    static inline Enricher *s_instance = nullptr;
    Library *m_library;
    EnricherBackend *m_backend;   // owned only when we made it ourselves
    bool m_ownsBackend = false;
    QQueue<qint64> m_queue;
    qint64 m_inFlight = -1;
    QTimer m_gap;            // enforces kRequestGapMs between requests
    qint64 m_lastRequestMs = 0;
    QElapsedTimer m_clock;
    QHash<QString, ReleaseInfo> m_releases;
    QString m_coverDir;
    QHash<qint64, int> m_retries;   // trackId -> attempts already made
    int m_done = 0;
    QString m_lastError;
};
