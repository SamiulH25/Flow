#include "audioprofiles.h"
#include "equalizer.h"

#include <QtTest>

#include <cmath>
#include <vector>

namespace {

constexpr int kRate = 48000;

using Band = Equalizer::Band;

// Runs a tone through a fresh equalizer and returns the settled tail.
std::vector<float> renderTone(const std::vector<Band> &bands, float preampDb,
                              double hz, double amp, double seconds, double skip = 0.5)
{
    Equalizer eq;
    eq.setEq(bands, preampDb);
    const int frames = int(kRate * seconds);
    std::vector<float> buf(size_t(frames) * 2);
    for (int i = 0; i < frames; ++i) {
        const float v = float(amp * std::sin(2.0 * M_PI * hz * i / kRate));
        buf[size_t(i) * 2] = v;
        buf[size_t(i) * 2 + 1] = v;
    }
    eq.process(buf.data(), frames);
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

class TestEqualizer : public QObject
{
    Q_OBJECT

private slots:
    void curveTiltsTheTone()
    {
        // The House profile, preamp included: relative to its own level, 60 Hz
        // must sit clearly above 8 kHz. There is no compressor to hide it.
        const double low = rms(renderTone(houseBands(), -4.f, 60, 0.25, 1.5));
        const double high = rms(renderTone(houseBands(), -4.f, 8000, 0.25, 1.5));
        QVERIFY(20.0 * std::log10(low / high) > 2.0);
    }

    void profileBandsShapeTheTone()
    {
        // This is how a device profile works: a -12 dB band at 8 kHz must pull
        // an 8 kHz tone down by about 12 dB.
        const double flat = rms(renderTone({}, 0.f, 8000, 0.02, 1.5));
        const std::vector<Band> cut = {{Equalizer::BandType::Peaking, 8000.f, -12.f, 1.2f}};
        const double shaped = rms(renderTone(cut, 0.f, 8000, 0.02, 1.5));
        const double delta = 20.0 * std::log10(shaped / flat);
        QVERIFY(delta < -10.0);
        QVERIFY(delta > -14.0);
    }

    void preampIsApplied()
    {
        const double flat = rms(renderTone({}, 0.f, 8000, 0.02, 1.5));
        const double quiet = rms(renderTone({}, -6.f, 8000, 0.02, 1.5));
        const double delta = 20.0 * std::log10(quiet / flat);
        QVERIFY(delta < -5.5);
        QVERIFY(delta > -6.5);
    }

    void fullScaleIsGuarded()
    {
        // A profile with more boost than headroom cannot push samples past
        // full scale: the last stage clamps.
        const std::vector<Band> boost = {{Equalizer::BandType::Peaking, 1000.f, 12.f, 1.0f}};
        const std::vector<float> out = renderTone(boost, 0.f, 1000, 0.9, 1.5);
        QVERIFY(peak(out) <= 1.0);
        for (float v : out)
            QVERIFY(std::isfinite(v));
    }

    void silenceStaysSilent()
    {
        Equalizer eq;
        eq.setEq(houseBands(), -4.f);
        std::vector<float> buf(2048, 0.f);
        eq.process(buf.data(), 1024);
        for (float v : buf)
            QCOMPARE(v, 0.0f);
    }
};

QTEST_MAIN(TestEqualizer)
#include "tst_equalizer.moc"
