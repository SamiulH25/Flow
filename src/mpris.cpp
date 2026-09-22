#include "mpris.h"

#include "library.h"
#include "player.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QUrl>

namespace {
const QString kObjectPath = QStringLiteral("/org/mpris/MediaPlayer2");
const QString kPlayerInterface = QStringLiteral("org.mpris.MediaPlayer2.Player");

qlonglong toUs(qreal seconds)
{
    return qlonglong(seconds * 1'000'000.0);
}
} // namespace

// ---------------------------------------------------------------- Mpris

Mpris::Mpris(Player *player, Library *library, QObject *parent)
    : QObject(parent)
    , m_player(player)
    , m_library(library)
{
    new MprisRootAdaptor(this);
    auto *playerAdaptor = new MprisPlayerAdaptor(this);

    m_syncTimer.setSingleShot(true);
    m_syncTimer.setInterval(0);
    connect(&m_syncTimer, &QTimer::timeout, this, &Mpris::sync);

    for (auto signal : {&Player::playingChanged, &Player::currentChanged, &Player::queueChanged,
                        &Player::volumeChanged, &Player::shuffleChanged, &Player::repeatChanged,
                        &Player::durationChanged}) {
        connect(m_player, signal, this, &Mpris::scheduleSync);
    }
    connect(m_library, &Library::countsChanged, this, &Mpris::scheduleSync);
    connect(m_player, &Player::seeked, playerAdaptor,
            [playerAdaptor](qreal seconds) { emit playerAdaptor->Seeked(toUs(seconds)); });

    m_lastPlayer = playerProperties();
}

Mpris::~Mpris()
{
    if (!m_service.isEmpty()) {
        QDBusConnection bus = QDBusConnection::sessionBus();
        bus.unregisterObject(kObjectPath);
        bus.unregisterService(m_service);
    }
}

bool Mpris::registerOnBus()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        qWarning() << "flow: no D-Bus session bus, MPRIS disabled";
        return false;
    }
    if (!bus.registerObject(kObjectPath, this)) {
        qWarning().noquote() << "flow: MPRIS object registration failed:" << bus.lastError().message();
        return false;
    }
    // A second Flow instance must not steal the name; MPRIS allows suffixes.
    QString name = QStringLiteral("org.mpris.MediaPlayer2.flow");
    if (bus.interface()->isServiceRegistered(name))
        name += QStringLiteral(".instance%1").arg(QCoreApplication::applicationPid());
    if (!bus.registerService(name)) {
        qWarning().noquote() << "flow: MPRIS name registration failed:" << bus.lastError().message();
        bus.unregisterObject(kObjectPath);
        return false;
    }
    m_service = name;
    return true;
}

QDBusObjectPath Mpris::trackPath() const
{
    const Track *t = m_player->currentTrack();
    if (!t)
        return QDBusObjectPath(QStringLiteral("/org/mpris/MediaPlayer2/TrackList/NoTrack"));
    // Track ids are SQLite rowids: positive integers, valid in an object path.
    return QDBusObjectPath(QStringLiteral("/org/mpris/MediaPlayer2/Track/%1").arg(t->id));
}

QVariantMap Mpris::metadata() const
{
    const Track *t = m_player->currentTrack();
    QVariantMap m;
    m.insert(QStringLiteral("mpris:trackid"), QVariant::fromValue(trackPath()));
    if (!t)
        return m;
    m.insert(QStringLiteral("mpris:length"), toUs(m_player->duration()));
    m.insert(QStringLiteral("xesam:title"), t->title);
    m.insert(QStringLiteral("xesam:artist"), QStringList{t->artist});
    m.insert(QStringLiteral("xesam:album"), t->album);
    if (!t->albumArtist.isEmpty())
        m.insert(QStringLiteral("xesam:albumArtist"), QStringList{t->albumArtist});
    if (t->trackNo > 0)
        m.insert(QStringLiteral("xesam:trackNumber"), t->trackNo);
    if (t->discNo > 0)
        m.insert(QStringLiteral("xesam:discNumber"), t->discNo);
    m.insert(QStringLiteral("xesam:url"), QUrl::fromLocalFile(t->path).toString());
    if (!t->coverPath.isEmpty())
        m.insert(QStringLiteral("mpris:artUrl"), QUrl::fromLocalFile(t->coverPath).toString());
    return m;
}

QVariantMap Mpris::playerProperties() const
{
    const auto *a = findChild<MprisPlayerAdaptor *>();
    return {
        {QStringLiteral("PlaybackStatus"), a->playbackStatus()},
        {QStringLiteral("LoopStatus"), a->loopStatus()},
        {QStringLiteral("Shuffle"), a->shuffle()},
        {QStringLiteral("Metadata"), metadata()},
        {QStringLiteral("Volume"), a->volume()},
        {QStringLiteral("CanGoNext"), a->canGoNext()},
        {QStringLiteral("CanGoPrevious"), a->canGoPrevious()},
        {QStringLiteral("CanPlay"), a->canPlay()},
        {QStringLiteral("CanPause"), a->canPause()},
        {QStringLiteral("CanSeek"), a->canSeek()},
    };
}

void Mpris::scheduleSync()
{
    m_syncTimer.start();
}

void Mpris::sync()
{
    // Position is deliberately absent: clients extrapolate it and listen for
    // Seeked, per the MPRIS spec.
    const QVariantMap now = playerProperties();
    QVariantMap changed;
    for (auto it = now.cbegin(); it != now.cend(); ++it) {
        if (m_lastPlayer.value(it.key()) != it.value())
            changed.insert(it.key(), it.value());
    }
    m_lastPlayer = now;
    if (!changed.isEmpty())
        emitChanged(kPlayerInterface, changed);
}

void Mpris::emitChanged(const QString &interface, const QVariantMap &changed)
{
    if (m_service.isEmpty())
        return;
    QDBusMessage signal = QDBusMessage::createSignal(
        kObjectPath, QStringLiteral("org.freedesktop.DBus.Properties"),
        QStringLiteral("PropertiesChanged"));
    signal << interface << changed << QStringList();
    QDBusConnection::sessionBus().send(signal);
}

// ---------------------------------------------------------------- root

MprisRootAdaptor::MprisRootAdaptor(Mpris *mpris)
    : QDBusAbstractAdaptor(mpris)
    , m_mpris(mpris)
{
}

QStringList MprisRootAdaptor::supportedMimeTypes() const
{
    return {QStringLiteral("audio/flac"), QStringLiteral("audio/mpeg"),
            QStringLiteral("audio/wav"), QStringLiteral("audio/x-wav")};
}

void MprisRootAdaptor::Raise()
{
    emit m_mpris->raiseRequested();
}

void MprisRootAdaptor::Quit()
{
    QCoreApplication::quit();
}

// ---------------------------------------------------------------- player

MprisPlayerAdaptor::MprisPlayerAdaptor(Mpris *mpris)
    : QDBusAbstractAdaptor(mpris)
    , m_mpris(mpris)
{
}

QString MprisPlayerAdaptor::playbackStatus() const
{
    const Player *p = m_mpris->player();
    if (p->playing())
        return QStringLiteral("Playing");
    if (p->hasTrack() && p->paused())
        return QStringLiteral("Paused");
    return QStringLiteral("Stopped");
}

QString MprisPlayerAdaptor::loopStatus() const
{
    return m_mpris->player()->repeat() ? QStringLiteral("Playlist") : QStringLiteral("None");
}

void MprisPlayerAdaptor::setLoopStatus(const QString &status)
{
    // No single-track repeat; "Track" loops the queue like "Playlist".
    m_mpris->player()->setRepeat(status != u"None");
}

bool MprisPlayerAdaptor::shuffle() const
{
    return m_mpris->player()->shuffle();
}

void MprisPlayerAdaptor::setShuffle(bool on)
{
    m_mpris->player()->setShuffle(on);
}

double MprisPlayerAdaptor::volume() const
{
    return m_mpris->player()->volume();
}

void MprisPlayerAdaptor::setVolume(double volume)
{
    m_mpris->player()->setVolume(volume);
}

qlonglong MprisPlayerAdaptor::position() const
{
    return toUs(m_mpris->player()->position());
}

bool MprisPlayerAdaptor::canGoNext() const
{
    return m_mpris->player()->hasTrack();
}

bool MprisPlayerAdaptor::canGoPrevious() const
{
    return m_mpris->player()->hasTrack();
}

bool MprisPlayerAdaptor::canPlay() const
{
    // With an empty queue, Play starts the library view (like Space in the UI).
    return m_mpris->player()->hasTrack() || m_mpris->library()->totalCount() > 0;
}

bool MprisPlayerAdaptor::canPause() const
{
    return m_mpris->player()->hasTrack();
}

bool MprisPlayerAdaptor::canSeek() const
{
    return m_mpris->player()->hasTrack();
}

void MprisPlayerAdaptor::Next()
{
    m_mpris->player()->next();
}

void MprisPlayerAdaptor::Previous()
{
    m_mpris->player()->previous();
}

void MprisPlayerAdaptor::Pause()
{
    m_mpris->player()->pause();
}

void MprisPlayerAdaptor::PlayPause()
{
    m_mpris->player()->toggle();
}

void MprisPlayerAdaptor::Stop()
{
    m_mpris->player()->stop();
}

void MprisPlayerAdaptor::Play()
{
    Player *p = m_mpris->player();
    if (p->playing())
        return;
    if (p->hasTrack())
        p->play();
    else
        p->toggle();
}

void MprisPlayerAdaptor::Seek(qlonglong offsetUs)
{
    Player *p = m_mpris->player();
    if (!p->hasTrack())
        return;
    const qreal target = p->position() + offsetUs / 1'000'000.0;
    // Spec: seeking past the end behaves like Next.
    if (p->duration() > 0 && target > p->duration()) {
        p->next();
        return;
    }
    p->seek(qMax<qreal>(0, target));
}

void MprisPlayerAdaptor::SetPosition(const QDBusObjectPath &trackId, qlonglong positionUs)
{
    Player *p = m_mpris->player();
    // Stale requests (for a track that is no longer current) are ignored.
    if (trackId != m_mpris->trackPath() || positionUs < 0)
        return;
    const qreal seconds = positionUs / 1'000'000.0;
    if (p->duration() > 0 && seconds > p->duration())
        return;
    p->seek(seconds);
}

void MprisPlayerAdaptor::OpenUri(const QString &uri)
{
    const QUrl url(uri);
    if (!url.isLocalFile())
        return;
    if (const Track *t = m_mpris->library()->trackByPath(url.toLocalFile()))
        m_mpris->player()->playTracks({t->id}, 0);
}
