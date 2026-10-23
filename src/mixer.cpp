#include "mixer.h"

#include <QAudioBuffer>
#include <QAudioSink>
#include <QIODevice>
#include <QMetaObject>

#include <algorithm>
#include <cstring>

namespace {
// Two seconds: long enough to absorb a stalled renderer, short enough that a
// device hiccup cannot leave the output far behind the player's clock.
constexpr int kCapacityFrames = Mixer::kRate * 2;
constexpr int kBytesPerFrame = 2 * int(sizeof(float));
}

class Mixer::Pull : public QIODevice
{
public:
    explicit Pull(Mixer *owner) : m_owner(owner) {}

    qint64 readData(char *data, qint64 maxSize) override
    {
        const int frames = int(maxSize / kBytesPerFrame);
        if (frames > 0)
            m_owner->pull(data, frames);
        return qint64(frames) * kBytesPerFrame;
    }
    // Qt's sink asks how much is available before reading; a custom QIODevice
    // that only implements readData is never read at all.
    qint64 bytesAvailable() const override { return m_owner->availableBytes(); }
    qint64 writeData(const char *, qint64) override { return -1; }
    bool isSequential() const override { return true; }

private:
    Mixer *m_owner;
};

QAudioFormat Mixer::format()
{
    QAudioFormat f;
    f.setSampleRate(kRate);
    f.setChannelCount(2);
    f.setSampleFormat(QAudioFormat::Float);
    return f;
}

Mixer::Mixer(QObject *parent)
    : QObject(parent)
    , m_ring(size_t(kCapacityFrames) * 2, 0.f)
{
}

Mixer::~Mixer()
{
    stopSink();
}

void Mixer::setEnabled(bool on)
{
    if (on == m_enabled)
        return;
    m_enabled = on;
    if (!on)
        stopSink();
    else
        m_startRequested = false;   // the next tapped buffer opens the sink
}

void Mixer::setDevice(const QAudioDevice &device)
{
    if (m_device == device)
        return;
    m_device = device;
    if (m_sink)
        stopSink();   // the next tapped buffer reopens on the new device
}

void Mixer::setVolume(float volume)
{
    m_volume = std::clamp(volume, 0.f, 1.f);
    if (m_sink)
        m_sink->setVolume(m_volume);
}

void Mixer::suspend()
{
    if (m_sink)
        m_sink->suspend();
}

void Mixer::resume()
{
    if (m_sink)
        m_sink->resume();
}

void Mixer::flush()
{
    {
        QMutexLocker lock(&m_mutex);
        m_read = 0;
        m_count = 0;
        m_mastering.reset();
    }
    // QAudioSink::reset() stops the stream for good on the PulseAudio backend
    // (reset() == stop + discard), and a stopped sink never pulls again. Close
    // it here; the next tapped buffer reopens it via process().
    if (m_sink)
        stopSink();
}

void Mixer::startSink()
{
    if (m_sink || !m_enabled)
        return;
    if (m_device.isNull()) {
        emit failed();
        return;
    }

    m_sink = new QAudioSink(m_device, format(), this);
    m_sink->setBufferSize(format().bytesForDuration(40000));   // ~40 ms of slack
    m_sink->setVolume(m_volume);
    m_pull = new Pull(this);
    m_pull->open(QIODevice::ReadOnly);
    connect(m_sink, &QAudioSink::stateChanged, this, [this](QtAudio::State state) {
        // A removed device stops the stream; hand the sound back to the player.
        if (state == QtAudio::StoppedState && m_sink && m_sink->error() != QtAudio::NoError)
            emit failed();
    });
    m_sink->start(m_pull);

    if (m_sink->error() != QtAudio::NoError) {
        stopSink();
        m_startRequested = true;   // no retry until the mode is toggled again
        emit failed();
        return;
    }
    m_started = true;
    emit started();
}

void Mixer::stopSink()
{
    if (m_sink) {
        disconnect(m_sink, nullptr, this, nullptr);   // no failure callback mid-teardown
        m_sink->stop();
        delete m_sink;
        m_sink = nullptr;
    }
    if (m_pull) {
        m_pull->close();
        delete m_pull;
        m_pull = nullptr;
    }
    {
        QMutexLocker lock(&m_mutex);
        m_read = 0;
        m_count = 0;
        m_mastering.reset();
    }
    m_startRequested = false;
    m_started = false;
}

void Mixer::process(const QAudioBuffer &buffer)
{
    if (!m_enabled)
        return;
    const QAudioFormat f = buffer.format();
    if (f.sampleRate() != kRate || f.channelCount() != 2
        || f.sampleFormat() != QAudioFormat::Float) {
        return;   // the tap was given a fixed format; anything else is a bug
    }
    const int frames = int(buffer.frameCount());
    if (frames <= 0)
        return;

    // The sink must be created on this object's thread (the GUI thread).
    if (!m_startRequested.exchange(true))
        QMetaObject::invokeMethod(this, &Mixer::startSink, Qt::QueuedConnection);

    QMutexLocker lock(&m_mutex);
    m_scratch.resize(size_t(frames) * 2);
    std::memcpy(m_scratch.data(), buffer.constData<float>(),
                m_scratch.size() * sizeof(float));
    m_mastering.process(m_scratch.data(), frames);

    // Overflow means the sink stopped consuming (a stalled device): drop the
    // oldest audio rather than let the output drift further behind the clock.
    const int drop = m_count + frames - kCapacityFrames;
    if (drop > 0) {
        m_read = (m_read + drop) % kCapacityFrames;
        m_count -= drop;
    }
    int write = (m_read + m_count) % kCapacityFrames;
    const int first = std::min(frames, kCapacityFrames - write);
    std::memcpy(m_ring.data() + size_t(write) * 2, m_scratch.data(),
                size_t(first) * 2 * sizeof(float));
    if (frames > first) {
        std::memcpy(m_ring.data(), m_scratch.data() + size_t(first) * 2,
                    size_t(frames - first) * 2 * sizeof(float));
    }
    m_count += frames;
}

qint64 Mixer::availableBytes() const
{
    QMutexLocker lock(&m_mutex);
    return qint64(m_count) * kBytesPerFrame;
}

void Mixer::pull(char *out, int frames)
{
    float *dst = reinterpret_cast<float *>(out);
    int got = 0;
    {
        QMutexLocker lock(&m_mutex);
        got = std::min(frames, m_count);
        const int first = std::min(got, kCapacityFrames - m_read);
        std::memcpy(dst, m_ring.data() + size_t(m_read) * 2,
                    size_t(first) * 2 * sizeof(float));
        if (got > first) {
            std::memcpy(dst + size_t(first) * 2, m_ring.data(),
                        size_t(got - first) * 2 * sizeof(float));
        }
        m_read = (m_read + got) % kCapacityFrames;
        m_count -= got;
    }
    // Underrun: keep the clock moving with silence instead of stalling.
    if (got < frames)
        std::memset(dst + size_t(got) * 2, 0, size_t(frames - got) * 2 * sizeof(float));
}
