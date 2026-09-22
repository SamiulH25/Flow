#include "player.h"

#include "library.h"

#include <QJSEngine>
#include <QSet>
#include <QSettings>
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

    const QSettings settings;
    m_volume = qBound(0.0, settings.value(QStringLiteral("player/volume"), 0.7).toDouble(), 1.0);
    m_repeat = settings.value(QStringLiteral("player/repeat"), false).toBool();
    m_queue.setShuffled(settings.value(QStringLiteral("player/shuffle"), false).toBool());
    m_audio.setVolume(QtAudio::convertVolume(float(m_volume), QtAudio::LogarithmicVolumeScale,
                                             QtAudio::LinearVolumeScale));

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

    refreshCurrentMap();
}

Player::~Player()
{
    m_player.stop();
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
    QSettings settings;
    settings.setValue(QStringLiteral("player/volume"), m_volume);
    settings.setValue(QStringLiteral("player/repeat"), m_repeat);
    settings.setValue(QStringLiteral("player/shuffle"), m_queue.isShuffled());
}

void Player::setVolume(qreal volume)
{
    volume = qBound(0.0, volume, 1.0);
    if (qFuzzyCompare(volume + 1.0, m_volume + 1.0))
        return;
    m_volume = volume;
    // The slider is perceptual; the output wants a linear gain.
    m_audio.setVolume(QtAudio::convertVolume(float(volume), QtAudio::LogarithmicVolumeScale,
                                             QtAudio::LinearVolumeScale));
    saveSettings();
    emit volumeChanged();
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
    if (on == m_repeat)
        return;
    m_repeat = on;
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
            m_player.setSource(QUrl::fromLocalFile(t->path));
        } else {
            m_player.setPosition(0);   // same file again (repeat / duplicate entry)
        }
    }

    m_lyrics = t ? Lyrics::load(t->path) : Lyrics();
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

    if (t && autoplay)
        m_player.play();
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
        return;
    }
    if (status != QMediaPlayer::EndOfMedia)
        return;
    const int next = m_queue.nextIndex(m_repeat);
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

void Player::next()
{
    const int next = m_queue.nextIndex(m_repeat);
    if (next < 0) {
        m_player.stop();
        return;
    }
    m_queue.setCurrentIndex(next);
    loadCurrent(true);
}

void Player::previous()
{
    const int prev = m_queue.previousIndex(m_repeat);
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
    m_player.setPosition(qint64(seconds * 1000));
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
