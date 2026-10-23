#pragma once

#include "mastering.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QMutex>
#include <QObject>

#include <atomic>
#include <vector>

class QAudioBuffer;
class QAudioSink;
class QIODevice;

// The output stage for the "full sound" mode: QMediaPlayer keeps decoding (and
// keeps the clock), but its own output is muted and the decoded PCM is tapped
// before its volume stage. This class runs that tap through Mastering and
// plays the result on its own QAudioSink.
//
// Two streams on the same device share one clock, so the tap's rate and the
// sink's consumption stay together; the ring only exists to absorb the
// renderer's bursts. If the backend has no tap (or the sink cannot open) the
// player falls back to plain output; see Player's watchdog.
class Mixer : public QObject
{
    Q_OBJECT
public:
    static constexpr int kRate = 48000;
    static QAudioFormat format();   // 48 kHz, stereo, float32

    explicit Mixer(QObject *parent = nullptr);
    ~Mixer() override;

    void setEnabled(bool on);
    void setDevice(const QAudioDevice &device);
    void setVolume(float volume);
    void suspend();
    void resume();
    void flush();                   // seek / track change: drop what is queued
    bool running() const { return m_started; }

    // Called on the renderer thread, directly from the tap.
    void process(const QAudioBuffer &buffer);

signals:
    void started();                 // the sink is open and pulling
    void failed();                  // no device, or the sink refused the format

private:
    class Pull;

    void startSink();
    void stopSink();
    void pull(char *out, int frames);
    qint64 availableBytes() const;   // the sink reads only what this reports

    QAudioDevice m_device;
    QAudioSink *m_sink = nullptr;
    Pull *m_pull = nullptr;
    float m_volume = 1.f;

    mutable QMutex m_mutex;
    std::vector<float> m_ring;      // interleaved stereo, circular
    std::vector<float> m_scratch;   // tap copy, so the buffer is never written
    int m_read = 0;
    int m_count = 0;
    Mastering m_mastering;

    std::atomic_bool m_enabled{false};
    std::atomic_bool m_startRequested{false};
    std::atomic_bool m_started{false};
};
