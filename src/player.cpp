#include "player.h"

#include "library.h"

#include <QJSEngine>
#include <QSet>
#include "settings.h"
#include <QUrl>
#include <QtMultimedia/qaudio.h>

namespace {
constexpr qreal kRestartThreshold = 3.0;   // seconds: "previous" restarts the track past this
}

Player::Player(Library *library, QObject *parent)
    : QObject(parent)
    , m_library(library)
    , m_queue(this)
{
    m_player.setAudioOutput(&m_audio);
    // Live PCM for the now-playing visualizer (ffmpeg backend only; without it
    // the tap simply never fires and the analyser rests at zero).
    m_player.setAudioBufferOutput(m_visualizer.output());

    connect(&m_visualizer, &Visualizer::changed, this, &Player::visualizerChanged);

    const auto settings = flowSettings();
    m_volume = qBound(0.0, settings.value(QStringLiteral("player/volume"), 0.7).toDouble(), 1.0);
    // "player/repeat" (bool) predates repeat-one; read it as off / all.
    const int legacy = settings.value(QStringLiteral("player/repeat"), false).toBool() ? RepeatAll : RepeatOff;
    m_repeatMode = qBound(0, settings.value(QStringLiteral("player/repeatMode"), legacy).toInt(), 2);
    m_queue.setShuffled(settings.value(QStringLiteral("player/shuffle"), false).toBool());
    m_matchLoudness = settings.value(QStringLiteral("player/matchLoudness"), false).toBool();
    applyMatch();   // no track yet: sets the plain slider volume

    // queueChanged covers every edit so bindings on queueAt() stay fresh.
    connect(&m_queue, &PlayQueue::countChanged, this, &Player::queueChanged);
    connect(&m_queue, &PlayQueue::currentIndexChanged, this, &Player::queueChanged);
    connect(&m_queue, &PlayQueue::rowsMoved, this, &Player::queueChanged);
    connect(&m_queue, &PlayQueue::modelReset, this, &Player::queueChanged);
    connect(&m_queue, &PlayQueue::dataChanged, this, &Player::queueChanged);

    connect(&m_player, &QMediaPlayer::playbackStateChanged, this, &Player::playingChanged);
    connect(&m_player, &QMediaPlayer::durationChanged, this, &Player::durationChanged);
    connect(&m_player, &QMediaPlayer::positionChanged, this, [this] {
        emit positionChanged();
        updateLyricIndex();
    });
    connect(&m_player, &QMediaPlayer::mediaStatusChanged, this, &Player::onMediaStatus);
    connect(&m_player, &QMediaPlayer::errorOccurred, this, &Player::onError);

    connect(m_library, &Library::trackUpdated, this, &Player::onTrackUpdated);
    connect(m_library, &Library::reloaded, this, &Player::onLibraryReloaded);

    connect(&m_waveform, &Waveform::ready, this,
            [this](qint64 id, const QByteArray &bars, float rmsDb, float peakDb) {
                m_library->storePeaks(id, bars);
                m_library->storeLoudness(id, rmsDb, peakDb);
                if (id == currentId()) {
                    setPeaks(bars);
                    applyMatch();
                }
            });

    // Save the queue shortly after edits (crash safety) and again on exit.
    m_saveQueueTimer.setSingleShot(true);
    m_saveQueueTimer.setInterval(1000);
    connect(&m_saveQueueTimer, &QTimer::timeout, this, &Player::saveQueue);
    connect(this, &Player::queueChanged, &m_saveQueueTimer, qOverload<>(&QTimer::start));

    restoreQueue();
    refreshCurrentMap();
}

Player::~Player()
{
    saveQueue();
    m_player.stop();
}

void Player::saveQueue() const
{
    QVariantList ids;
    for (int i = 0; i < m_queue.count(); ++i)
        ids.append(m_queue.at(i)->id);
    auto settings = flowSettings();
    settings.setValue(QStringLiteral("queue/ids"), ids);
    settings.setValue(QStringLiteral("queue/index"), m_queue.currentIndex());
    // Before the media has loaded, m_player reports 0; keep the restored spot.
    const qint64 pos = m_pendingSeekMs >= 0 ? m_pendingSeekMs : m_player.position();
    settings.setValue(QStringLiteral("queue/positionMs"), pos);
}

void Player::restoreQueue()
{
    const auto settings = flowSettings();
    const QVariantList ids = settings.value(QStringLiteral("queue/ids")).toList();
    const int savedIndex = settings.value(QStringLiteral("queue/index"), -1).toInt();
    QList<Track> tracks;
    int current = -1;
    for (int i = 0; i < ids.size(); ++i) {
        const Track *t = m_library->trackById(ids.at(i).toLongLong());
        if (!t)
            continue;   // deleted since last run
        if (i == savedIndex)
            current = int(tracks.size());
        tracks.append(*t);
    }
    if (tracks.isEmpty())
        return;
    const bool sameTrack = current >= 0;
    m_queue.restore(tracks, sameTrack ? current : qMin(qMax(savedIndex, 0), int(tracks.size()) - 1));
    loadCurrent(false);   // ready to resume, but never start playing on launch
    if (sameTrack)
        m_pendingSeekMs = settings.value(QStringLiteral("queue/positionMs"), 0).toLongLong();
}

Player *Player::create(QQmlEngine *, QJSEngine *jsEngine)
{
    Q_ASSERT(s_instance);
    Q_ASSERT(jsEngine->thread() == s_instance->thread());
    QJSEngine::setObjectOwnership(s_instance, QJSEngine::CppOwnership);
    return s_instance;
}

qint64 Player::currentId() const
{
    const Track *t = m_queue.current();
    return t ? t->id : -1;
}

bool Player::playing() const
{
    return m_player.playbackState() == QMediaPlayer::PlayingState;
}

bool Player::paused() const
{
    return m_player.playbackState() == QMediaPlayer::PausedState;
}

qreal Player::duration() const
{
    if (m_player.duration() > 0)
        return m_player.duration() / 1000.0;
    // Before the decoder reports a length, fall back to the scanned one.
    const Track *t = m_queue.current();
    return t ? t->durationMs / 1000.0 : 0.0;
}

void Player::setError(const QString &error)
{
    if (m_error == error)
        return;
    m_error = error;
    if (!error.isEmpty())
        qWarning().noquote() << "flow: playback:" << error;
    emit errorChanged();
}

void Player::saveSettings() const
{
    auto settings = flowSettings();
    settings.setValue(QStringLiteral("player/volume"), m_volume);
    settings.setValue(QStringLiteral("player/repeatMode"), m_repeatMode);
    settings.remove(QStringLiteral("player/repeat"));
    settings.setValue(QStringLiteral("player/shuffle"), m_queue.isShuffled());
    settings.setValue(QStringLiteral("player/matchLoudness"), m_matchLoudness);
}

void Player::setVolume(qreal volume)
{
    volume = qBound(0.0, volume, 1.0);
    if (qFuzzyCompare(volume + 1.0, m_volume + 1.0))
        return;
    m_volume = volume;
    applyMatch();
    saveSettings();
    emit volumeChanged();
}

float Player::outputVolume(qreal slider, float gain)
{
    // The slider is perceptual, the output wants a linear gain, so the slider
    // has to be converted first and the matching gain then scales the linear
    // result. Scaling the raw slider value *before* the conversion (as if the
    // gain were a log-scale value) applies it on the wrong side of that curve:
    // the attenuation comes out up to twice as deep as the panel reports, and
    // past the knee of the curve the order flips, so a brick-walled master
    // plays quieter than a track ten dB below the target.
    const float linear = QtAudio::convertVolume(
            float(qBound(0.0, slider, 1.0)),
            QtAudio::LogarithmicVolumeScale, QtAudio::LinearVolumeScale);
    return float(qBound(0.0, double(linear) * double(gain), 1.0));
}

void Player::applyMatch()
{
    // With loudness matching on, a track louder than the target is turned down
    // to meet it — a player can never turn a track up past full scale, so quiet
    // ones are left exactly where the user put them.
    const Track *t = m_queue.current();
    const float gain = (m_matchLoudness && t && t->hasLoudness()) ? Waveform::matchGain(t->rmsDb)
                                                                  : 1.0f;
    m_matchGain = gain;
    m_audio.setVolume(outputVolume(m_volume, gain));
    emit matchChanged();
}

void Player::setMatchLoudness(bool on)
{
    if (on == m_matchLoudness)
        return;
    m_matchLoudness = on;
    applyMatch();
    saveSettings();
}

QString Player::loudnessText() const
{
    const Track *t = m_queue.current();
    if (!t || !t->hasLoudness())
        return QString();
    return QStringLiteral("%1 dBFS · peak %2")
            .arg(t->rmsDb, 0, 'f', 1)
            .arg(t->peakDb, 0, 'f', 1);
}

void Player::setShuffle(bool on)
{
    if (on == m_queue.isShuffled())
        return;
    m_queue.setShuffled(on);
    saveSettings();
    emit shuffleChanged();
}

void Player::setRepeat(bool on)
{
    if (on != repeat())
        setRepeatMode(on ? RepeatAll : RepeatOff);
}

void Player::setRepeatMode(int mode)
{
    mode = qBound(0, mode, 2);
    if (mode == m_repeatMode)
        return;
    m_repeatMode = mode;
    saveSettings();
    emit repeatChanged();
}

void Player::refreshCurrentMap()
{
    const Track *t = m_queue.current();
    // Always hand QML a full map so bindings never read `undefined`.
    m_currentMap = TrackRoles::toVariantMap(t ? *t : Track());
}

void Player::loadCurrent(bool autoplay)
{
    m_pendingSeekMs = -1;   // any explicit load supersedes a restored position
    const Track *t = m_queue.current();
    const quint64 uid = m_queue.uidAt(m_queue.currentIndex());

    if (!t) {
        m_player.stop();
        m_player.setSource(QUrl());
        m_loadedPath.clear();
        m_loadedUid = 0;
    } else {
        m_loadedUid = uid;
        if (t->path != m_loadedPath) {
            m_loadedPath = t->path;
            m_player.setSource(m_library->sourceUrl(*t));
        } else {
            m_player.setPosition(0);   // same file again (repeat / duplicate entry)
        }
    }

    // Lyrics are read from a local sidecar or embedded tags; a remote track has neither.
    m_lyrics = (t && !Navidrome::isRemotePath(t->path)) ? Lyrics::load(t->path) : Lyrics();
    m_lyricsList.clear();
    for (const Lyrics::Line &line : std::as_const(m_lyrics.lines))
        m_lyricsList.append(QVariantMap{{QStringLiteral("ms"), line.ms},
                                        {QStringLiteral("text"), line.text}});
    m_lyricIndex = -1;
    emit lyricsChanged();
    emit lyricIndexChanged();

    refreshCurrentMap();
    emit currentChanged();
    emit durationChanged();
    emit positionChanged();
    loadPeaks();
    applyMatch();

    if (t && autoplay)
        m_player.play();
}

void Player::loadPeaks()
{
    const Track *t = m_queue.current();
    if (!t) {
        setPeaks({});
        return;
    }
    // A cached overview is not enough on its own: the same pass measures the
    // track's loudness, so a track with bars but no numbers is decoded once.
    const QByteArray cached = m_library->cachedPeaks(t->id);
    if (cached.size() == Waveform::kBars)
        setPeaks(cached);
    else
        setPeaks({});
    if ((cached.size() != Waveform::kBars || !t->hasLoudness())
        && m_waveform.pendingTrack() != t->id) {
        m_waveform.request(t->id, m_library->sourceUrl(*t));
    }
}

void Player::setPeaks(const QByteArray &bars)
{
    QVariantList peaks;
    peaks.reserve(bars.size());
    for (char b : bars)
        peaks.append(quint8(b) / 255.0);
    if (peaks == m_peaks)
        return;
    m_peaks = peaks;
    emit peaksChanged();
}

void Player::updateLyricIndex()
{
    const int index = m_lyrics.indexAt(m_player.position());
    if (index == m_lyricIndex)
        return;
    m_lyricIndex = index;
    emit lyricIndexChanged();
}

void Player::onMediaStatus(QMediaPlayer::MediaStatus status)
{
    if (status == QMediaPlayer::BufferedMedia || status == QMediaPlayer::LoadedMedia) {
        m_errorStreak = 0;
        setError(QString());
        if (m_pendingSeekMs >= 0) {
            const qint64 ms = m_pendingSeekMs;
            m_pendingSeekMs = -1;
            m_player.setPosition(ms);
        }
        return;
    }
    if (status != QMediaPlayer::EndOfMedia)
        return;
    if (m_repeatMode == RepeatOne) {
        loadCurrent(true);   // same entry: rewinds and plays again
        return;
    }
    const int next = m_queue.nextIndex(m_repeatMode == RepeatAll);
    if (next < 0) {
        m_player.stop();   // end of queue: park at the start of the last track
        return;
    }
    m_queue.setCurrentIndex(next);
    loadCurrent(true);
}

void Player::onError(QMediaPlayer::Error error, const QString &text)
{
    if (error == QMediaPlayer::NoError)
        return;
    const Track *t = m_queue.current();
    setError(t ? QStringLiteral("%1 — %2").arg(t->title, text) : text);
    // Skip unplayable files, but give up once every entry has failed in a row.
    // Queued: the backend may report errors from inside setSource().
    const quint64 failedUid = m_loadedUid;
    QMetaObject::invokeMethod(this, [this, failedUid] {
        if (m_loadedUid != failedUid)
            return;   // the user already moved on
        if (++m_errorStreak >= m_queue.count()) {
            m_player.stop();
            return;
        }
        const int next = m_queue.nextIndex(true);
        if (next < 0)
            return;
        m_queue.setCurrentIndex(next);
        loadCurrent(true);
    }, Qt::QueuedConnection);
}

void Player::onTrackUpdated(qint64 id)
{
    const Track *t = m_library->trackById(id);
    if (!t)
        return;
    m_queue.updateTrack(*t);
    if (currentId() == id) {
        refreshCurrentMap();
        emit currentChanged();
    }
}

void Player::onLibraryReloaded()
{
    // Refresh queue snapshots with rescanned tags / art. Entries whose files
    // vanished keep their snapshot; playing them reports an error and skips.
    QSet<qint64> ids;
    for (int i = 0; i < m_queue.count(); ++i)
        ids.insert(m_queue.at(i)->id);
    for (qint64 id : std::as_const(ids)) {
        if (const Track *t = m_library->trackById(id))
            m_queue.updateTrack(*t);
    }
    refreshCurrentMap();
    emit currentChanged();
}

void Player::playTracks(const QVariantList &ids, int start)
{
    QList<Track> tracks;
    int startRow = -1;
    for (int i = 0; i < ids.size(); ++i) {
        const Track *t = m_library->trackById(ids.at(i).toLongLong());
        if (!t)
            continue;
        if (i >= start && startRow < 0)
            startRow = int(tracks.size());   // first playable at or after `start`
        tracks.append(*t);
    }
    if (tracks.isEmpty())
        return;
    m_errorStreak = 0;
    m_queue.replace(tracks, startRow < 0 ? 0 : startRow);
    loadCurrent(true);
}

void Player::toggle()
{
    if (!hasTrack()) {
        // Nothing queued yet: play what the library view shows.
        playTracks(m_library->visibleTrackIds(), 0);
        return;
    }
    if (playing())
        pause();
    else
        play();
}

void Player::play()
{
    if (!hasTrack())
        return;
    if (m_loadedUid != m_queue.uidAt(m_queue.currentIndex()))
        loadCurrent(true);
    else
        m_player.play();
}

void Player::pause()
{
    m_player.pause();
}

void Player::stop()
{
    m_player.stop();
}

void Player::next()
{
    const int next = m_queue.nextIndex(repeat());
    if (next < 0) {
        m_player.stop();
        return;
    }
    m_queue.setCurrentIndex(next);
    loadCurrent(true);
}

void Player::previous()
{
    const int prev = m_queue.previousIndex(repeat());
    if (position() > kRestartThreshold || prev < 0) {
        seek(0);
        return;
    }
    m_queue.setCurrentIndex(prev);
    loadCurrent(true);
}

void Player::seek(qreal seconds)
{
    if (!hasTrack())
        return;
    const qreal len = duration();
    seconds = qBound(0.0, seconds, len > 0 ? len : seconds);
    m_pendingSeekMs = -1;   // the user's seek wins over a restored position
    m_player.setPosition(qint64(seconds * 1000));
    emit seeked(seconds);
}

void Player::jump(int queueRow)
{
    if (!m_queue.setCurrentIndex(queueRow))
        return;
    m_errorStreak = 0;
    loadCurrent(true);
}

void Player::playNext(qint64 id)
{
    const Track *t = m_library->trackById(id);
    if (!t)
        return;
    const bool wasEmpty = !hasTrack();
    m_queue.insertNext(*t);
    if (wasEmpty) {
        m_queue.setCurrentIndex(0);
        loadCurrent(true);
    }
}

void Player::enqueue(qint64 id)
{
    const Track *t = m_library->trackById(id);
    if (!t)
        return;
    const bool wasEmpty = !hasTrack();
    m_queue.append(*t);
    if (wasEmpty) {
        m_queue.setCurrentIndex(m_queue.count() - 1);
        loadCurrent(true);
    }
}

void Player::removeAt(int queueRow)
{
    if (queueRow < 0 || queueRow >= m_queue.count())
        return;
    const bool wasCurrent = queueRow == m_queue.currentIndex();
    const bool wasPlaying = playing();
    m_queue.removeAt(queueRow);
    if (wasCurrent)
        loadCurrent(wasPlaying);
}

void Player::move(int from, int to)
{
    m_queue.move(from, to);
}

void Player::clearQueue()
{
    m_queue.clear();
    loadCurrent(false);
}

QVariantMap Player::queueAt(int queueRow) const
{
    const Track *t = m_queue.at(queueRow);
    return TrackRoles::toVariantMap(t ? *t : Track());
}
