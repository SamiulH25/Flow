#pragma once

#include <QAudioDecoder>
#include <QByteArray>
#include <QList>
#include <QObject>

// Computes a track's waveform overview: kBars loudness values (RMS),
// normalised to 0..255. Decoding runs in QtMultimedia's ffmpeg threads; only
// the cheap per-buffer reduction happens here. One track at a time: a new
// request abandons the previous one (only the current track matters).
class Waveform : public QObject
{
    Q_OBJECT
public:
    static constexpr int kBars = 96;

    explicit Waveform(QObject *parent = nullptr);

    void request(qint64 trackId, const QString &path);
    qint64 pendingTrack() const { return m_trackId; }

    // Folds per-chunk energy into kBars RMS values normalised to 0..255.
    static QByteArray toBars(const QList<double> &chunkSumSquares, const QList<int> &chunkFrames);

signals:
    void ready(qint64 trackId, const QByteArray &bars);
    void failed(qint64 trackId);

private:
    void onBuffer();
    void onFinished();
    void onError();
    void addSample(double v);

    QAudioDecoder m_decoder;
    qint64 m_trackId = -1;
    int m_chunkFrames = 0;     // frames per analysis chunk (~20 ms)
    double m_acc = 0;
    int m_accFrames = 0;
    QList<double> m_sums;      // sum of squares per chunk
    QList<int> m_frames;       // frames per chunk
};
