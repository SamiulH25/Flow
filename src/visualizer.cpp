#include "visualizer.h"

#include <QtMath>

#include <algorithm>
#include <cmath>

namespace {

// Window the spectrum is drawn in. Below this the bars sit on the floor, so
// quiet intros do not look broken; above it a full-scale sine still fits.
constexpr float kFloorDb = -74.0f;
constexpr float kCeilDb = -14.0f;
// A Hann window loses half its energy and splits each peak over three bins, so
// a full-scale sine peaks at N/4 rather than N/2.
constexpr float kFullScale = float(Visualizer::kFftSize) / 4.0f;
// The high end is quieter in most music: tilt the bands up by 6 dB per decade
// of frequency so the display does not look bass-heavy.
constexpr float kTiltDbPerDecade = 6.0f;

constexpr int kFrameMs = 16;     // ~60 Hz, what the display is drawn at
constexpr float kAttack = 0.55f;
constexpr float kRelease = 0.16f;
constexpr float kIdleDecay = 0.86f;
// Frames the display keeps decaying after the audio stops before it is
// considered at rest (~0.2 s). Once at rest the analyser stops publishing, so a
// paused player costs nothing and QML stops re-evaluating its bindings.
constexpr int kIdleStopFrames = 12;

// The window only depends on the FFT size; at 60 Hz the per-frame std::cos loop
// was the single largest cost in the analyser.
const std::vector<float> &hannWindow()
{
    static const std::vector<float> table = [] {
        const int n = Visualizer::kFftSize;
        std::vector<float> t(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i)
            t[size_t(i)] = float(0.5 - 0.5 * std::cos(2.0 * M_PI * i / (n - 1)));
        return t;
    }();
    return table;
}

} // namespace

Visualizer::Visualizer(QObject *parent)
    : QObject(parent)
    , m_ring(size_t(kFftSize), 0.0f)
    , m_levels(kBands, 0.0f)
    , m_scope(kScopeValues, 0.0f)
{
    for (int i = 0; i < kBands; ++i)
        m_spectrum.append(0.0);
    for (int i = 0; i < kScopeValues; ++i)
        m_scopeList.append(0.0);

    // Deliberately without a context object: the samples point into the
    // decoder's own frame, which is only alive while this call runs, and a
    // context object would let Qt hand the buffer over asynchronously. The
    // connection dies with m_tap, a member, so it cannot outlive the object.
    QObject::connect(&m_tap, &QAudioBufferOutput::audioBufferReceived,
                     [this](const QAudioBuffer &b) { onBuffer(b); });
    m_frameTimer.setTimerType(Qt::PreciseTimer);
    m_frameTimer.setInterval(kFrameMs);
    connect(&m_frameTimer, &QTimer::timeout, this, &Visualizer::onFrame);
    m_clock.start();
    m_frameTimer.start();
}

Visualizer::~Visualizer() = default;

void Visualizer::fft(std::vector<float> &re, std::vector<float> &im)
{
    const int n = int(re.size());
    if (n < 2 || int(im.size()) != n)
        return;
    for (int i = 1, j = 0; i < n; ++i) {   // bit-reversal permutation
        int bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            std::swap(re[size_t(i)], re[size_t(j)]);
            std::swap(im[size_t(i)], im[size_t(j)]);
        }
    }
    // One turn of twiddles for the current size, reused by every stage: a
    // stage of `len` takes every n/len-th entry. thread_local so two analysers
    // (or a test and the GUI) can never race on the lazy init.
    static thread_local std::vector<float> cosT, sinT;
    if (int(cosT.size()) != n) {
        cosT.resize(size_t(n));
        sinT.resize(size_t(n));
        for (int k = 0; k < n; ++k) {
            const double a = -2.0 * M_PI * k / n;
            cosT[size_t(k)] = float(std::cos(a));
            sinT[size_t(k)] = float(std::sin(a));
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const int half = len >> 1;
        const int step = n / len;
        for (int i = 0; i < n; i += len) {
            for (int k = 0; k < half; ++k) {
                const size_t tw = size_t(k * step);
                const float wr = cosT[tw], wi = sinT[tw];
                const size_t a = size_t(i + k), b = size_t(a + half);
                const float vr = re[b] * wr - im[b] * wi;
                const float vi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - vr;
                im[b] = im[a] - vi;
                re[a] += vr;
                im[a] += vi;
            }
        }
    }
}

QList<float> Visualizer::bandLevels(const std::vector<float> &window, int rate)
{
    QList<float> out(kBands, 0.0f);
    const int n = int(window.size());
    if (n < 8 || rate <= 0 || n != kFftSize)
        return out;

    // Reused across frames: this runs 60 times a second, so it must not
    // allocate two 8 KB vectors per call.
    static thread_local std::vector<float> re, im;
    re.assign(size_t(n), 0.0f);
    im.assign(size_t(n), 0.0f);
    const std::vector<float> &hann = hannWindow();
    for (int i = 0; i < n; ++i) {
        // Hann window: -31 dB sidelobes, so one note lights up its own band.
        re[size_t(i)] = window[size_t(i)] * hann[size_t(i)];
    }
    fft(re, im);

    const double firstHz = 30.0;
    const double lastHz = qMin(16000.0, rate * 0.48);
    const int bins = n / 2;
    for (int b = 0; b < kBands; ++b) {
        const double f0 = firstHz * std::pow(lastHz / firstHz, double(b) / kBands);
        const double f1 = firstHz * std::pow(lastHz / firstHz, double(b + 1) / kBands);
        int lo = int(f0 * n / rate);
        int hi = int(std::ceil(f1 * n / rate)) - 1;
        lo = std::clamp(lo, 1, bins - 1);
        hi = std::clamp(hi, lo, bins - 1);
        float mag = 0.0f;
        for (int k = lo; k <= hi; ++k) {
            const float m = std::hypot(re[size_t(k)], im[size_t(k)]);
            mag = std::max(mag, m);
        }
        const double centre = std::sqrt(f0 * f1);
        const double db = 20.0 * std::log10(double(mag) / kFullScale + 1e-7)
                + kTiltDbPerDecade * std::log10(centre / 1000.0);
        out[b] = float(std::clamp((db - kFloorDb) / (kCeilDb - kFloorDb), 0.0, 1.0));
    }
    return out;
}

QList<float> Visualizer::scopeTrace(const std::vector<float> &window)
{
    QList<float> out(kScopeValues, 0.0f);
    if (int(window.size()) < kScopeSpan)
        return out;
    const int from = int(window.size()) - kScopeSpan;
    for (int c = 0; c < kScope; ++c) {
        const int a = from + c * kScopeSpan / kScope;
        const int b = from + (c + 1) * kScopeSpan / kScope;
        // Min and max per column, not a decimated sample: a 1 kHz tone would
        // otherwise draw a straight line instead of its envelope.
        float lo = 1.0f, hi = -1.0f;
        for (int i = a; i < b; ++i) {
            lo = std::min(lo, window[size_t(i)]);
            hi = std::max(hi, window[size_t(i)]);
        }
        out[c * 2] = lo;
        out[c * 2 + 1] = hi;
    }
    return out;
}

void Visualizer::appendSamples(const QAudioBuffer &buffer)
{
    const QAudioFormat format = buffer.format();
    const int frames = int(buffer.frameCount());
    if (frames <= 0)
        return;
    if (format.sampleRate() != m_rate) {
        m_rate = format.sampleRate();
        std::fill(m_ring.begin(), m_ring.end(), 0.0f);
        m_write = m_count = 0;
    }

    // Read at most `frames` samples, never frames * channels. The ffmpeg
    // backend reports a full interleaved length in byteCount() but hands over
    // a shorter run, and reading the rest walks off the end of it. For stereo
    // that leaves the left channel, which is all a spectrum needs; the sample
    // rate is unaffected (a 1 kHz tone still lands in its own band).
    const int n = frames;
    auto push = [this](float v) {
        m_ring[size_t(m_write)] = v;
        m_write = (m_write + 1) % kFftSize;
        m_count = qMin(m_count + 1, kFftSize);
    };
    switch (format.sampleFormat()) {
    case QAudioFormat::Float: {
        const auto *d = buffer.constData<float>();
        for (int i = 0; i < n; ++i)
            push(d[i]);
        break;
    }
    case QAudioFormat::Int16: {
        const auto *d = buffer.constData<qint16>();
        for (int i = 0; i < n; ++i)
            push(float(d[i]) / 32768.0f);
        break;
    }
    case QAudioFormat::Int32: {
        const auto *d = buffer.constData<qint32>();
        for (int i = 0; i < n; ++i)
            push(float(d[i]) / 2147483648.0f);
        break;
    }
    case QAudioFormat::UInt8: {
        const auto *d = buffer.constData<quint8>();
        for (int i = 0; i < n; ++i)
            push((float(d[i]) - 128.0f) / 128.0f);
        break;
    }
    default:
        return;   // nothing we can read; the display simply rests at zero
    }
    m_lastAudioMs = m_clock.elapsed();
}

void Visualizer::latestWindow(int frames, std::vector<float> &out) const
{
    const int take = qMin(frames, m_count);
    out.resize(size_t(take));
    const int start = (m_write - take + kFftSize) % kFftSize;
    for (int i = 0; i < take; ++i)
        out[size_t(i)] = m_ring[size_t((start + i) % kFftSize)];
}

void Visualizer::onBuffer(const QAudioBuffer &buffer)
{
    QMutexLocker lock(&m_mutex);
    appendSamples(buffer);
}

void Visualizer::onFrame()
{
    static thread_local std::vector<float> window, scopeWindow;
    bool haveAudio = false;
    int rate = 0;
    {
        QMutexLocker lock(&m_mutex);
        // Nothing fresh in the ring means the player is paused, stopped or
        // between tracks: fall back to rest instead of showing the last window
        // forever. Blocks arrive every ~24 ms while frames tick every ~16 ms,
        // so freshness is measured in time, not per tick.
        haveAudio = m_count >= kFftSize && m_lastAudioMs >= 0
                && m_clock.elapsed() - m_lastAudioMs < kStaleMs;
        if (haveAudio) {
            rate = m_rate;
            latestWindow(kFftSize, window);
            latestWindow(kScopeSpan, scopeWindow);
        }
    }

    // At rest: the display is already showing zeros, so recomputing and
    // republishing 240 values every 16 ms would only make QML re-evaluate its
    // bindings for nothing. The timer keeps ticking (cheaply) to notice audio.
    if (!haveAudio && m_settled)
        return;

    if (haveAudio) {
        const QList<float> levels = bandLevels(window, rate);
        for (int b = 0; b < kBands; ++b) {
            float &v = m_levels[b];
            const float want = levels[b];
            v += (want - v) * (want > v ? kAttack : kRelease);
        }
    } else {
        for (int b = 0; b < kBands; ++b)
            m_levels[b] *= kIdleDecay;
    }

    for (int b = 0; b < kBands; ++b)
        m_spectrum[b] = m_levels[b];
    if (haveAudio) {
        m_scope = scopeTrace(scopeWindow);
    } else {
        for (int i = 0; i < kScopeValues; ++i)
            m_scope[i] = 0.0f;
    }
    for (int i = 0; i < kScopeValues; ++i)
        m_scopeList[i] = m_scope[i];

    if (!m_active && haveAudio) {
        m_active = true;
        emit activeChanged();
    } else if (m_active && !haveAudio) {
        m_active = false;
        emit activeChanged();
    }
    emit changed();

    if (haveAudio) {
        m_idleFrames = 0;
        m_settled = false;
    } else if (++m_idleFrames >= kIdleStopFrames) {
        // Decayed to the floor: publish exact zeros once, then go quiet.
        for (int b = 0; b < kBands; ++b) {
            m_levels[b] = 0.0f;
            m_spectrum[b] = 0.0;
        }
        for (int i = 0; i < kScopeValues; ++i) {
            m_scope[i] = 0.0f;
            m_scopeList[i] = 0.0;
        }
        emit changed();
        m_settled = true;
    }
}
