#include "waveform.h"

#include <QAudioBuffer>
#include <QAudioFormat>
#include <QUrl>

#include <cmath>

namespace {
// 0 dBFS is full scale; anything below the floor is reported as the floor
// rather than as -infinity, so the panel never shows nonsense.
constexpr double kDbFloor = -90.0;
}

float Waveform::toDb(double linear)
{
    if (linear <= 1e-6)
        return float(kDbFloor);
    return float(qBound(kDbFloor, 20.0 * std::log10(linear), 0.0));
}

float Waveform::matchGain(float rmsDb)
{
    // A player can only attenuate: the volume of a track that is already
    // quieter than the target is left exactly where the user put it.
    if (rmsDb <= kTargetDb || rmsDb <= kDbFloor)
        return 1.0f;
    return float(std::pow(10.0, (kTargetDb - rmsDb) / 20.0));
}

Waveform::Waveform(QObject *parent)
    : QObject(parent)
{
    // No downsample: ffmpeg decodes the whole stream either way, and asking for
    // 8 kHz only resamples the result — which both blurs the overview and
    // overshoots the peaks, so the levels would read a couple of dB high.
    connect(&m_decoder, &QAudioDecoder::bufferReady, this, &Waveform::onBuffer);
    connect(&m_decoder, &QAudioDecoder::finished, this, &Waveform::onFinished);
    connect(&m_decoder, qOverload<QAudioDecoder::Error>(&QAudioDecoder::error), this,
            &Waveform::onError);
}

void Waveform::request(qint64 trackId, const QUrl &source)
{
    m_trackId = -1;   // anything the old decode still reports is ignored
    m_decoder.stop();
    m_trackId = trackId;
    m_chunkFrames = 0;
    m_acc = 0;
    m_accFrames = 0;
    m_sums.clear();
    m_frames.clear();
    m_peak = 0.0;
    m_decoder.setSource(source);
    m_decoder.start();
}

void Waveform::addSample(double v)
{
    m_acc += v * v;
    const double a = std::abs(v);
    if (a > m_peak)
        m_peak = a;
    if (++m_accFrames >= m_chunkFrames) {
        m_sums.append(m_acc);
        m_frames.append(m_accFrames);
        m_acc = 0;
        m_accFrames = 0;
    }
}

void Waveform::onBuffer()
{
    const QAudioBuffer buffer = m_decoder.read();
    if (!buffer.isValid() || m_trackId < 0)
        return;
    const QAudioFormat fmt = buffer.format();
    const int channels = fmt.channelCount();
    if (channels <= 0)
        return;
    if (m_chunkFrames == 0)
        m_chunkFrames = qMax(1, fmt.sampleRate() / 50);   // 20 ms chunks

    const qsizetype frames = buffer.frameCount();
    const auto mixFrame = [&](auto sampleAt) {
        for (qsizetype f = 0; f < frames; ++f) {
            double sum = 0;
            for (int c = 0; c < channels; ++c)
                sum += sampleAt(f * channels + c);
            addSample(sum / channels);
        }
    };
    switch (fmt.sampleFormat()) {
    case QAudioFormat::Float: {
        const float *d = buffer.constData<float>();
        mixFrame([d](qsizetype i) { return double(d[i]); });
        break;
    }
    case QAudioFormat::Int16: {
        const qint16 *d = buffer.constData<qint16>();
        mixFrame([d](qsizetype i) { return d[i] / 32768.0; });
        break;
    }
    case QAudioFormat::Int32: {
        const qint32 *d = buffer.constData<qint32>();
        mixFrame([d](qsizetype i) { return d[i] / 2147483648.0; });
        break;
    }
    case QAudioFormat::UInt8: {
        const quint8 *d = buffer.constData<quint8>();
        mixFrame([d](qsizetype i) { return (int(d[i]) - 128) / 128.0; });
        break;
    }
    default:
        break;
    }
}

void Waveform::onFinished()
{
    if (m_trackId < 0)
        return;
    if (m_accFrames > 0) {
        m_sums.append(m_acc);
        m_frames.append(m_accFrames);
    }
    const qint64 id = m_trackId;
    m_trackId = -1;
    if (m_sums.isEmpty()) {
        emit failed(id);
        return;
    }
    // The same pass that draws the overview also measures the track: the sum of
    // squares is already there, and the highest sample is free to keep. Both
    // are read at 8 kHz, so they describe the track rather than the whole
    // spectrum — good enough to compare tracks, which is all they are for.
    double squares = 0.0;
    qint64 frames = 0;
    for (int i = 0; i < m_sums.size(); ++i) {
        squares += m_sums.at(i);
        frames += m_frames.at(i);
    }
    const double rms = frames > 0 ? std::sqrt(squares / double(frames)) : 0.0;
    emit ready(id, toBars(m_sums, m_frames), toDb(rms), toDb(m_peak));
}


void Waveform::onError()
{
    if (m_trackId < 0)
        return;
    const qint64 id = m_trackId;
    m_trackId = -1;
    m_decoder.stop();
    emit failed(id);
}

QByteArray Waveform::toBars(const QList<double> &chunkSumSquares, const QList<int> &chunkFrames)
{
    QByteArray out(kBars, '\0');
    const qsizetype n = chunkSumSquares.size();
    if (n == 0)
        return out;
    QList<double> rms(kBars, 0.0);
    double loudest = 0;
    for (int b = 0; b < kBars; ++b) {
        // Chunks [begin, end) belong to bar b; short tracks reuse chunks.
        const qsizetype begin = b * n / kBars;
        const qsizetype end = qMax(begin + 1, (b + 1) * n / kBars);
        double sum = 0;
        qint64 frames = 0;
        for (qsizetype i = begin; i < end && i < n; ++i) {
            sum += chunkSumSquares.at(i);
            frames += chunkFrames.at(i);
        }
        rms[b] = frames > 0 ? std::sqrt(sum / double(frames)) : 0.0;
        loudest = qMax(loudest, rms[b]);
    }
    if (loudest <= 1e-9)
        return out;   // digital silence: all zero
    for (int b = 0; b < kBars; ++b)
        out[b] = char(quint8(qRound(255.0 * rms[b] / loudest)));
    return out;
}
