#pragma once

#include <QAudioDecoder>
#include <QByteArray>
#include <QUrl>
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

    // A local file, or any URL QAudioDecoder reads (a Navidrome stream).
    void request(qint64 trackId, const QString &path) { request(trackId, QUrl::fromLocalFile(path)); }
    void request(qint64 trackId, const QUrl &source);
    qint64 pendingTrack() const { return m_trackId; }

    // Level in dBFS of the track as decoded, and the level of the matching
    // volume to reach it. Shared so the number in the panel and the number
    // the player applies can never drift apart.
    static constexpr float kTargetDb = -14.0f;
    static float toDb(double linear);
    // <= 1: a player can only ever turn a track down, never past full scale.
    static float matchGain(float rmsDb);

    // Folds per-chunk energy into kBars RMS values normalised to 0..255.
    static QByteArray toBars(const QList<double> &chunkSumSquares, const QList<int> &chunkFrames);

signals:
    void ready(qint64 trackId, const QByteArray &bars, float rmsDb, float peakDb);
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
    int m_accFrames = 0;        // samples in the chunk being accumulated
    double m_peak = 0.0;       // loudest sample seen since the request
    QList<double> m_sums;      // sum of squares per chunk
    QList<int> m_frames;       // frames per chunk
};
