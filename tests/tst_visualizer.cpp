#include "visualizer.h"

#include <QtTest>

#include <cmath>
#include <vector>

namespace {

constexpr int kRate = 48000;

// A window of `hz` cosine, amplitude `amp`, as the analyser would see it.
std::vector<float> tone(double hz, double amp = 0.5, int n = Visualizer::kFftSize)
{
    std::vector<float> w(size_t(n), 0.0f);
    for (int i = 0; i < n; ++i)
        w[size_t(i)] = float(amp * std::sin(2.0 * M_PI * hz * i / kRate));
    return w;
}

int loudestBand(const QList<float> &levels)
{
    int best = 0;
    for (int i = 1; i < levels.size(); ++i) {
        if (levels[i] > levels[best])
            best = i;
    }
    return best;
}

float loudest(const QList<float> &levels)
{
    return levels[loudestBand(levels)];
}

} // namespace

class TestVisualizer : public QObject
{
    Q_OBJECT

private slots:
    void fftPutsEnergyInTheRightBin()
    {
        // Cosine at bin 7: the transform must peak there, not somewhere near.
        const int n = Visualizer::kFftSize;
        std::vector<float> re(size_t(n), 0.0f), im(size_t(n), 0.0f);
        for (int i = 0; i < n; ++i)
            re[size_t(i)] = float(std::cos(2.0 * M_PI * 7 * i / n));
        Visualizer::fft(re, im);

        int peak = 0;
        for (int k = 1; k < n / 2; ++k) {
            if (std::hypot(re[size_t(k)], im[size_t(k)]) > std::hypot(re[size_t(peak)], im[size_t(peak)]))
                peak = k;
        }
        QCOMPARE(peak, 7);
    }

    void fftOfSilenceIsSilence()
    {
        const int n = Visualizer::kFftSize;
        std::vector<float> re(size_t(n), 0.0f), im(size_t(n), 0.0f);
        Visualizer::fft(re, im);
        for (int k = 0; k < n / 2; ++k)
            QVERIFY(std::hypot(re[size_t(k)], im[size_t(k)]) < 1e-3f);
    }

    void aToneLightsUpTheBandItSitsIn()
    {
        const QList<float> low = Visualizer::bandLevels(tone(100.0), kRate);
        const QList<float> mid = Visualizer::bandLevels(tone(1000.0), kRate);
        const QList<float> high = Visualizer::bandLevels(tone(10000.0), kRate);
        QCOMPARE(low.size(), Visualizer::kBands);

        // The bands run 30 Hz .. 16 kHz logarithmically, so 100 Hz sits in the
        // first fifth, 1 kHz in the middle, 10 kHz near the top.
        QVERIFY(loudestBand(low) < 16);
        QVERIFY(loudestBand(mid) > 20 && loudestBand(mid) < 30);
        QVERIFY(loudestBand(high) > 38);
        QVERIFY(loudestBand(low) < loudestBand(mid));
        QVERIFY(loudestBand(mid) < loudestBand(high));

        // A half-scale tone is well above the display floor, silence is not.
        QVERIFY(loudest(mid) > 0.6f);
        const std::vector<float> quiet(size_t(Visualizer::kFftSize), 0.0f);
        const QList<float> none = Visualizer::bandLevels(quiet, kRate);
        for (float v : none)
            QCOMPARE(v, 0.0f);
    }

    void quieterTonesSitLower()
    {
        const QList<float> loud = Visualizer::bandLevels(tone(1000.0, 0.5), kRate);
        const QList<float> soft = Visualizer::bandLevels(tone(1000.0, 0.02), kRate);
        QVERIFY(loudest(soft) < loudest(loud));
    }

    void scopeTraceFollowsTheEnvelope()
    {
        // Min/max per column, so a tone spans the whole height of its columns
        // instead of drawing one sampled line.
        const QList<float> trace = Visualizer::scopeTrace(tone(1000.0, 0.5));
        QCOMPARE(trace.size(), Visualizer::kScopeValues);
        float lowest = 1.0f, highest = -1.0f;
        for (int c = 0; c < Visualizer::kScope; ++c) {
            const float lo = trace[c * 2], hi = trace[c * 2 + 1];
            QVERIFY(lo <= hi);
            // A column covers ~10 samples of a 48-sample cycle, so it must span
            // a good part of the wave rather than sit on one value.
            QVERIFY(hi - lo > 0.1f);
            lowest = qMin(lowest, lo);
            highest = qMax(highest, hi);
        }
        // The trace follows the amplitude of the signal it is fed.
        QVERIFY(highest > 0.4f);
        QVERIFY(lowest < -0.4f);
    }

    void scopeTraceOfSilenceIsFlat()
    {
        const std::vector<float> quiet(size_t(Visualizer::kFftSize), 0.0f);
        const QList<float> trace = Visualizer::scopeTrace(quiet);
        for (float v : trace)
            QCOMPARE(v, 0.0f);
    }

    void shortOrUnknownWindowsAreRejected()
    {
        // A window the analyser cannot work on must come back empty rather than
        // read past the end of the buffer.
        const std::vector<float> tiny(64, 0.25f);
        QCOMPARE(Visualizer::bandLevels(tiny, kRate).at(0), 0.0f);
        QCOMPARE(Visualizer::scopeTrace(tiny).at(0), 0.0f);
        QCOMPARE(Visualizer::bandLevels(tone(1000.0), 0).at(0), 0.0f);
    }
};

QTEST_MAIN(TestVisualizer)
#include "tst_visualizer.moc"
