#include "waveform.h"

#include <QAudioBuffer>
#include <QAudioFormat>
#include <QUrl>

#include <cmath>

namespace {
constexpr int kAnalysisRate = 8000;   // plenty for an overview, keeps decode cheap
}

Waveform::Waveform(QObject *parent)
    : QObject(parent)
{
    QAudioFormat format;
    format.setSampleRate(kAnalysisRate);
    format.setChannelCount(1);
    format.setSampleFormat(QAudioFormat::Float);
    m_decoder.setAudioFormat(format);   // a hint; buffers are read generically anyway

    connect(&m_decoder, &QAudioDecoder::bufferReady, this, &Waveform::onBuffer);
    connect(&m_decoder, &QAudioDecoder::finished, this, &Waveform::onFinished);
    connect(&m_decoder, qOverload<QAudioDecoder::Error>(&QAudioDecoder::error), this,
            &Waveform::onError);
}

void Waveform::request(qint64 trackId, const QString &path)
{
    m_trackId = -1;   // anything the old decode still reports is ignored
    m_decoder.stop();
    m_trackId = trackId;
    m_chunkFrames = 0;
    m_acc = 0;
    m_accFrames = 0;
    m_sums.clear();
    m_frames.clear();
    m_decoder.setSource(QUrl::fromLocalFile(path));
    m_decoder.start();
}

void Waveform::addSample(double v)
{
    m_acc += v * v;
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
    emit ready(id, toBars(m_sums, m_frames));
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
