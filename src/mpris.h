#pragma once

#include <QDBusAbstractAdaptor>
#include <QDBusObjectPath>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>

class Library;
class Player;

// MPRIS2 (org.mpris.MediaPlayer2) so playerctl, Hyprland keybinds and shell
// widgets can see and drive Flow. Owns the D-Bus object; the adaptors below
// are thin views over Player.
class Mpris : public QObject
{
    Q_OBJECT
public:
    Mpris(Player *player, Library *library, QObject *parent = nullptr);
    ~Mpris() override;

    // Claims org.mpris.MediaPlayer2.flow (or .flow.instance<pid> when taken).
    bool registerOnBus();
    QString serviceName() const { return m_service; }

    Player *player() const { return m_player; }
    Library *library() const { return m_library; }

    // Snapshots of the property sets, used for change detection.
    QVariantMap playerProperties() const;
    QVariantMap metadata() const;
    QDBusObjectPath trackPath() const;

signals:
    void raiseRequested();

private:
    void scheduleSync();
    void sync();
    void emitChanged(const QString &interface, const QVariantMap &changed);

    Player *m_player;
    Library *m_library;
    QString m_service;
    QVariantMap m_lastPlayer;
    QTimer m_syncTimer;   // coalesces bursts of Player signals into one D-Bus signal
};

class MprisRootAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2")
    Q_PROPERTY(bool CanQuit READ canQuit)
    Q_PROPERTY(bool CanRaise READ canRaise)
    Q_PROPERTY(bool HasTrackList READ hasTrackList)
    Q_PROPERTY(QString Identity READ identity)
    Q_PROPERTY(QString DesktopEntry READ desktopEntry)
    Q_PROPERTY(QStringList SupportedUriSchemes READ supportedUriSchemes)
    Q_PROPERTY(QStringList SupportedMimeTypes READ supportedMimeTypes)

public:
    explicit MprisRootAdaptor(Mpris *mpris);

    bool canQuit() const { return true; }
    bool canRaise() const { return true; }
    bool hasTrackList() const { return false; }
    QString identity() const { return QStringLiteral("Flow"); }
    QString desktopEntry() const { return QStringLiteral("flow"); }
    QStringList supportedUriSchemes() const { return {QStringLiteral("file")}; }
    QStringList supportedMimeTypes() const;

public slots:
    void Raise();
    void Quit();

private:
    Mpris *m_mpris;
};

class MprisPlayerAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2.Player")
    Q_PROPERTY(QString PlaybackStatus READ playbackStatus)
    Q_PROPERTY(QString LoopStatus READ loopStatus WRITE setLoopStatus)
    Q_PROPERTY(double Rate READ rate WRITE setRate)
    Q_PROPERTY(bool Shuffle READ shuffle WRITE setShuffle)
    Q_PROPERTY(QVariantMap Metadata READ metadata)
    Q_PROPERTY(double Volume READ volume WRITE setVolume)
    Q_PROPERTY(qlonglong Position READ position)
    Q_PROPERTY(double MinimumRate READ rate)
    Q_PROPERTY(double MaximumRate READ rate)
    Q_PROPERTY(bool CanGoNext READ canGoNext)
    Q_PROPERTY(bool CanGoPrevious READ canGoPrevious)
    Q_PROPERTY(bool CanPlay READ canPlay)
    Q_PROPERTY(bool CanPause READ canPause)
    Q_PROPERTY(bool CanSeek READ canSeek)
    Q_PROPERTY(bool CanControl READ canControl)

public:
    explicit MprisPlayerAdaptor(Mpris *mpris);

    QString playbackStatus() const;
    QString loopStatus() const;
    void setLoopStatus(const QString &status);
    double rate() const { return 1.0; }
    void setRate(double) {}   // fixed rate; spec allows ignoring
    bool shuffle() const;
    void setShuffle(bool on);
    QVariantMap metadata() const { return m_mpris->metadata(); }
    double volume() const;
    void setVolume(double volume);
    qlonglong position() const;
    bool canGoNext() const;
    bool canGoPrevious() const;
    bool canPlay() const;
    bool canPause() const;
    bool canSeek() const;
    bool canControl() const { return true; }

public slots:
    void Next();
    void Previous();
    void Pause();
    void PlayPause();
    void Stop();
    void Play();
    void Seek(qlonglong offsetUs);
    void SetPosition(const QDBusObjectPath &trackId, qlonglong positionUs);
    void OpenUri(const QString &uri);

signals:
    void Seeked(qlonglong positionUs);

private:
    Mpris *m_mpris;
};
