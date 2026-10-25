#include "audioprofiles.h"
#include "mastering.h"

#include <QtTest>

#include <cmath>
#include <vector>

namespace {

constexpr int kRate = 48000;

using Band = Mastering::Band;

// Runs a tone through a fresh chain with `bands` and returns the settled tail:
// the filters and the compressor need a moment before the level is meaningful.
std::vector<float> renderTone(const std::vector<Band> &bands, double hz, double amp,
                              double seconds, double skip = 0.75)
{
    Mastering mastering;
    mastering.setEq(bands, 0.f);
    const int frames = int(kRate * seconds);
    std::vector<float> buf(size_t(frames) * 2);
    for (int i = 0; i < frames; ++i) {
        const float v = float(amp * std::sin(2.0 * M_PI * hz * i / kRate));
        buf[size_t(i) * 2] = v;
        buf[size_t(i) * 2 + 1] = v;
    }
    mastering.process(buf.data(), frames);
    return std::vector<float>(buf.begin() + qsizetype(skip * kRate) * 2, buf.end());
}

double rms(const std::vector<float> &x)
{
    if (x.empty())
        return 0.0;
    double sum = 0.0;
    for (float v : x)
        sum += double(v) * v;
    return std::sqrt(sum / double(x.size()));
}

double peak(const std::vector<float> &x)
{
    double p = 0.0;
    for (float v : x)
        p = std::max(p, double(std::abs(v)));
    return p;
}

const std::vector<Band> &houseBands()
{
    return audioProfile(QStringLiteral("house"))->bands;
}

} // namespace

class TestMastering : public QObject
{
    Q_OBJECT

private slots:
    void lowEndComesForward()
    {
        // Below the compressor's threshold, so this measures the curve itself:
        // the low shelf must lift 60 Hz clearly more than the presence/air
        // stage lifts 8 kHz. (At a normal level the 2:1 compressor halves any
        // pre-compression difference; compressionShrinksDynamics covers that.)
        const double inRms = 0.02 / std::sqrt(2.0);
        const double lowGain =
                20.0 * std::log10(rms(renderTone(houseBands(), 60, 0.02, 2.0)) / inRms);
        const double highGain =
                20.0 * std::log10(rms(renderTone(houseBands(), 8000, 0.02, 2.0)) / inRms);
        QVERIFY(lowGain > 8.0);              // 6 dB makeup + the shelf
        QVERIFY(lowGain - highGain > 2.0);   // weight, not just level
    }

    void profileBandsShapeTheTone()
    {
        // This is how a device profile works: a -12 dB band at 8 kHz must pull
        // an 8 kHz tone down by about 12 dB, without touching the compressor.
        const double flat = rms(renderTone({}, 8000, 0.02, 2.0));
        const std::vector<Band> cut = {{Mastering::BandType::Peaking, 8000.f, -12.f, 1.2f}};
        const double shaped = rms(renderTone(cut, 8000, 0.02, 2.0));
        const double delta = 20.0 * std::log10(shaped / flat);
        QVERIFY(delta < -10.0);
        QVERIFY(delta > -14.0);
    }

    void compressionShrinksDynamics()
    {
        // 20 dB step: 1 s loud, 1 s quiet. The compressor is slow on purpose,
        // so measure the settled half of each segment.
        Mastering mastering;
        mastering.setEq(houseBands(), 0.f);
        const int frames = kRate * 2;
        std::vector<float> buf(size_t(frames) * 2);
        for (int i = 0; i < frames; ++i) {
            const float amp = i < kRate ? 0.6f : 0.06f;
            const float v = float(amp * std::sin(2.0 * M_PI * 1000.0 * i / kRate));
            buf[size_t(i) * 2] = v;
            buf[size_t(i) * 2 + 1] = v;
        }
        mastering.process(buf.data(), frames);

        const auto segment = [&](double fromSec, double toSec) {
            return std::vector<float>(buf.begin() + qsizetype(fromSec * kRate) * 2,
                                      buf.begin() + qsizetype(toSec * kRate) * 2);
        };
        const double loud = rms(segment(0.7, 1.0));
        const double quiet = rms(segment(1.7, 2.0));
        const double ratio = loud / quiet;
        QVERIFY(ratio < 6.0);    // input step was 10x (20 dB)
        QVERIFY(ratio > 1.5);    // still a step: not a wall of mush
    }

    void limiterHoldsTransients()
    {
        // Silence, then a full-scale tone. The 30 ms RMS detector cannot react
        // to the attack; the limiter must keep the output under the ceiling.
        Mastering mastering;
        mastering.setEq(houseBands(), 0.f);
        const int frames = kRate;
        std::vector<float> buf(size_t(frames) * 2, 0.f);
        for (int i = frames / 2; i < frames; ++i) {
            const float v = float(0.99 * std::sin(2.0 * M_PI * 1000.0 * i / kRate));
            buf[size_t(i) * 2] = v;
            buf[size_t(i) * 2 + 1] = v;
        }
        mastering.process(buf.data(), frames);

        for (float v : buf)
            QVERIFY(std::isfinite(v));
        QVERIFY(peak(buf) <= 0.951);   // kCeiling, with a little float slack
        QVERIFY(rms(std::vector<float>(buf.begin(), buf.begin() + frames)) < 1e-6);
    }

    void silenceStaysSilent()
    {
        Mastering mastering;
        mastering.setEq(houseBands(), 0.f);
        std::vector<float> buf(2048, 0.f);
        mastering.process(buf.data(), 1024);
        for (float v : buf)
            QCOMPARE(v, 0.0f);
    }
};

QTEST_MAIN(TestMastering)
#include "tst_mastering.moc"
