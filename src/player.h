#pragma once

#include "audioprofiles.h"
#include "lyrics.h"
#include "mixer.h"
#include "playqueue.h"
#include "waveform.h"
#include "visualizer.h"

#include <QAudioBufferOutput>
#include <QAudioOutput>
#include <QMediaDevices>
#include <QMediaPlayer>
#include <QObject>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

class Library;
class QJSEngine;
class QQmlEngine;

// Playback engine: QMediaPlayer (ffmpeg backend) driven by a PlayQueue, with
// the decoded PCM tapped through Mixer for the "full sound" mode. Times are
// exposed to QML in seconds.
class Player : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QAbstractItemModel *queue READ queueModel CONSTANT)
    Q_PROPERTY(int queueIndex READ queueIndex NOTIFY queueChanged)
    Q_PROPERTY(int queueCount READ queueCount NOTIFY queueChanged)
    Q_PROPERTY(bool hasTrack READ hasTrack NOTIFY currentChanged)
    Q_PROPERTY(QVariantMap current READ current NOTIFY currentChanged)
    Q_PROPERTY(qint64 currentId READ currentId NOTIFY currentChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)
    Q_PROPERTY(qreal position READ position NOTIFY positionChanged)
    Q_PROPERTY(qreal duration READ duration NOTIFY durationChanged)
    Q_PROPERTY(qreal volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(bool shuffle READ shuffle WRITE setShuffle NOTIFY shuffleChanged)
    Q_PROPERTY(bool repeat READ repeat WRITE setRepeat NOTIFY repeatChanged)
    Q_PROPERTY(int repeatMode READ repeatMode WRITE setRepeatMode NOTIFY repeatChanged)
    Q_PROPERTY(QVariantList lyrics READ lyrics NOTIFY lyricsChanged)
    Q_PROPERTY(bool lyricsTimed READ lyricsTimed NOTIFY lyricsChanged)
    Q_PROPERTY(int lyricIndex READ lyricIndex NOTIFY lyricIndexChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(QVariantList peaks READ peaks NOTIFY peaksChanged)   // 0..1 per bar; empty while computing
    Q_PROPERTY(QVariantList spectrum READ spectrum NOTIFY visualizerChanged)          // 0..1 per band
    Q_PROPERTY(QVariantList scope READ scope NOTIFY visualizerChanged)                // min/max pairs
    Q_PROPERTY(bool visualizerActive READ visualizerActive NOTIFY visualizerChanged)
    Q_PROPERTY(bool matchLoudness READ matchLoudness WRITE setMatchLoudness NOTIFY matchChanged)
    Q_PROPERTY(qreal matchGain READ matchGain NOTIFY matchChanged)   // <= 1: we can only turn down
    Q_PROPERTY(bool fullness READ fullness WRITE setFullness NOTIFY fullnessChanged)
    Q_PROPERTY(bool fullnessActive READ fullnessActive NOTIFY fullnessChanged)
    Q_PROPERTY(QString loudnessText READ loudnessText NOTIFY matchChanged)
    Q_PROPERTY(QVariantList audioDevices READ audioDevices NOTIFY audioDevicesChanged)
    Q_PROPERTY(QString audioDevice READ audioDevice WRITE setAudioDevice NOTIFY audioDeviceChanged)
    Q_PROPERTY(QString currentAudioDevice READ currentAudioDevice NOTIFY audioDeviceChanged)
    Q_PROPERTY(QVariantList audioProfileChoices READ audioProfileChoices CONSTANT)
    Q_PROPERTY(QString audioProfileChoice READ audioProfileChoice WRITE setAudioProfileChoice NOTIFY audioProfileChanged)
    Q_PROPERTY(QString audioProfile READ audioProfile NOTIFY audioProfileChanged)
    Q_PROPERTY(QString audioProfileName READ audioProfileName NOTIFY audioProfileChanged)

public:
    explicit Player(Library *library, QObject *parent = nullptr);
    ~Player() override;

    static Player *create(QQmlEngine *qmlEngine, QJSEngine *jsEngine);
    static void setInstance(Player *player) { s_instance = player; }

    // What the output device is actually set to: the perceptual slider value
    // converted to a linear gain, scaled by the track's matching gain.
    static float outputVolume(qreal slider, float gain);

    QAbstractItemModel *queueModel() { return &m_queue; }
    PlayQueue *queue() { return &m_queue; }
    int queueIndex() const { return m_queue.currentIndex(); }
    int queueCount() const { return m_queue.count(); }
    bool hasTrack() const { return m_queue.current() != nullptr; }
    QVariantMap current() const { return m_currentMap; }
    const Track *currentTrack() const { return m_queue.current(); }
    qint64 currentId() const;
    bool playing() const;
    bool paused() const;
    qreal position() const { return m_player.position() / 1000.0; }
    qreal duration() const;
    qreal volume() const { return m_volume; }
    void setVolume(qreal volume);
    bool shuffle() const { return m_queue.isShuffled(); }
    void setShuffle(bool on);
    // 0 off, 1 whole queue, 2 current track. `repeat` is the queue-level view of it.
    enum RepeatMode { RepeatOff = 0, RepeatAll = 1, RepeatOne = 2 };
    bool repeat() const { return m_repeatMode != RepeatOff; }
    int repeatMode() const { return m_repeatMode; }
    void setRepeatMode(int mode);
    void setRepeat(bool on);
    QVariantList lyrics() const { return m_lyricsList; }
    bool lyricsTimed() const { return m_lyrics.timed; }
    int lyricIndex() const { return m_lyricIndex; }
    QString error() const { return m_error; }
    QVariantList peaks() const { return m_peaks; }
    QVariantList spectrum() const { return m_visualizer.spectrum(); }
    QVariantList scope() const { return m_visualizer.scope(); }
    bool visualizerActive() const { return m_visualizer.active(); }
    bool matchLoudness() const { return m_matchLoudness; }
    void setMatchLoudness(bool on);
    qreal matchGain() const { return m_matchGain; }
    bool fullness() const { return m_fullness; }
    void setFullness(bool on);
    // True while the tap actually feeds the mixer (a backend without
    // QAudioBufferOutput support falls back to plain output).
    bool fullnessActive() const { return m_fullness && !m_dspFailed; }
    QString loudnessText() const;
    QVariantList audioDevices() const;
    QString audioDevice() const { return m_audioDeviceId; }
    void setAudioDevice(const QString &id);
    QString currentAudioDevice() const;
    Q_INVOKABLE void selectAudioDevice(const QString &id) { setAudioDevice(id); }
    // "auto" follows the output device's name; anything else pins a profile
    // for the current device (remembered per device).
    QVariantList audioProfileChoices() const { return m_profileChoicesUi; }
    QString audioProfileChoice() const;
    void setAudioProfileChoice(const QString &id);
    QString audioProfile() const;       // the resolved profile id
    QString audioProfileName() const;

    // Replace the queue with `ids` (library track ids) and start at `start`.
    Q_INVOKABLE void playTracks(const QVariantList &ids, int start);
    Q_INVOKABLE void toggle();
    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void next();
    Q_INVOKABLE void previous();
    Q_INVOKABLE void seek(qreal seconds);
    Q_INVOKABLE void jump(int queueRow);
    Q_INVOKABLE void playNext(qint64 id);
    Q_INVOKABLE void enqueue(qint64 id);
    Q_INVOKABLE void removeAt(int queueRow);
    Q_INVOKABLE void move(int from, int to);
    Q_INVOKABLE void clearQueue();
    Q_INVOKABLE QVariantMap queueAt(int queueRow) const;   // track roles of a queue row

signals:
    void queueChanged();
    void currentChanged();
    void playingChanged();
    void positionChanged();
    void durationChanged();
    void volumeChanged();
    void shuffleChanged();
    void repeatChanged();
    void lyricsChanged();
    void lyricIndexChanged();
    void errorChanged();
    void seeked(qreal seconds);
    void peaksChanged();   // explicit jumps only, not normal progress
    void visualizerChanged();
    void matchChanged();
    void fullnessChanged();
    void audioDeviceChanged();
    void audioDevicesChanged();
    void audioProfileChanged();

private:
    void loadCurrent(bool autoplay);
    void refreshCurrentMap();
    void updateLyricIndex();
    void onMediaStatus(QMediaPlayer::MediaStatus status);
    void onError(QMediaPlayer::Error error, const QString &text);
    void onTrackUpdated(qint64 id);
    void onLibraryReloaded();
    void saveSettings() const;
    void saveQueue() const;
    void restoreQueue();
    void setError(const QString &error);
    void loadPeaks();
    void setPeaks(const QByteArray &bars);
    void applyMatch();
    void updateAudioDevice(bool force = false);
    void onAudioOutputsChanged();
    void onPlaybackStateChanged();
    void onMixerFailed();
    void armDspWatchdog();
    QString deviceProfileKey() const;
    void applyAudioProfile();

    static inline Player *s_instance = nullptr;

    Library *m_library;
    PlayQueue m_queue;
    QAudioOutput m_audio;        // declared first: must outlive m_player
    QMediaDevices m_mediaDevices;
    QAudioBufferOutput m_tap{Mixer::format()};   // before m_player: must outlive it
    Visualizer m_visualizer;
    Mixer m_mixer;
    QMediaPlayer m_player;
    QString m_loadedPath;
    quint64 m_loadedUid = 0;     // queue entry currently in m_player
    QVariantMap m_currentMap;
    qreal m_volume = 0.7;
    int m_repeatMode = RepeatOff;
    int m_errorStreak = 0;       // consecutive unplayable tracks, stops skip loops
    qint64 m_pendingSeekMs = -1; // restored position, applied once the media loads
    QTimer m_saveQueueTimer;
    Waveform m_waveform;
    QVariantList m_peaks;
    Lyrics m_lyrics;
    QVariantList m_lyricsList;
    int m_lyricIndex = -1;
    QString m_error;
    bool m_matchLoudness = false;
    float m_matchGain = 1.0f;
    QString m_audioDeviceId;
    bool m_fullness = true;
    bool m_dspFailed = false;   // no tap / no sink: plain output for this session
    QTimer m_dspWatchdog;
    QVariantList m_profileChoicesUi;   // built once: auto + every profile
    QVariantMap m_profileChoices;      // device key -> profile id (absent = auto)
};
