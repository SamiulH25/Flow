#pragma once

#include <QAudioBuffer>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QTimer>
#include <QVariantList>

#include <QElapsedTimer>

#include <vector>

// Live analysis of what QMediaPlayer is playing, for the now-playing
// visualizer. The decoded PCM is downmixed to mono and kept in a ring buffer;
// every frame a 2048-point FFT is folded into kBands log-spaced levels and the
// newest samples into a min/max oscilloscope trace. Smoothing happens here, not
// in QML, so the display gets a steady 60 Hz stream of ready-to-draw numbers.
//
// Player owns the QAudioBufferOutput tap and calls append() directly from the
// renderer thread: the buffer points into the decoder's own frame, which is
// recycled as soon as the signal returns, so samples are copied out under
// m_mutex rather than queued. The frame timer then snapshots the ring and
// analyses it lock-free.
//
// Pure maths (the window -> bands/trace part) is static so it can be tested
// without an audio device.
class Visualizer : public QObject
{
    Q_OBJECT
public:
    static constexpr int kBands = 48;   // spectrum bars, 30 Hz .. 16 kHz, log spaced
    static constexpr int kScope = 96;   // oscilloscope columns
    static constexpr int kFftSize = 2048;
    static constexpr int kScopeSpan = 1024;   // samples the scope trace covers

    // A paused or ended player simply stops handing out buffers. Blocks arrive
    // every ~24 ms while frames are drawn every ~16 ms, so freshness has to be
    // measured in time, not in "did something arrive since the last tick".
    static constexpr int kStaleMs = 200;

    // Min/max per column: 2 * kScope values, each in -1..1, pairs (lo, hi).
    static constexpr int kScopeValues = kScope * 2;

    explicit Visualizer(QObject *parent = nullptr);
    ~Visualizer() override;

    // Called on the renderer thread with the decoded buffer (tap format).
    void append(const QAudioBuffer &buffer);

    QVariantList spectrum() const { return m_spectrum; }
    QVariantList scope() const { return m_scopeList; }
    bool active() const { return m_active; }

    // Band levels 0..1 and the min/max scope pairs for one window of samples,
    // oldest first. Exposed for tests.
    static QList<float> bandLevels(const std::vector<float> &window, int rate);
    static QList<float> scopeTrace(const std::vector<float> &window);

    // In-place radix-2 FFT of `re/im` (length a power of two).
    static void fft(std::vector<float> &re, std::vector<float> &im);

signals:
    void changed();         // spectrum and/or scope moved
    void activeChanged();

private:
    void onFrame();         // 60 Hz: analyse, smooth, publish

    // Both run with m_mutex held.
    void appendSamples(const QAudioBuffer &buffer);
    void latestWindow(int frames, std::vector<float> &out) const;

    QTimer m_frameTimer;

    mutable QMutex m_mutex;     // guards the ring and the buffer age
    std::vector<float> m_ring;  // newest sample last, capacity kFftSize
    int m_write = 0;
    int m_count = 0;
    int m_rate = 0;
    qint64 m_lastAudioMs = -1;  // -1: nothing buffered yet
    QElapsedTimer m_clock;

    QList<float> m_levels;      // smoothed 0..1 per band
    QVariantList m_spectrum;
    QList<float> m_scope;
    QVariantList m_scopeList;
    bool m_active = false;
    bool m_settled = false;     // at rest: stop republishing until audio returns
    int m_idleFrames = 0;
};
