#include "mastering.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr float kRate = 48000.f;

// Compression is what makes the result feel close rather than just brighter:
// it raises the average level, and the makeup gain gives it back after the
// peaks have been tamed.
constexpr float kThresholdDb = -22.f;
constexpr float kRatio = 2.f;
constexpr float kMakeupDb = 6.f;
// Symmetric averaging, not attack/release: an asymmetric detector biases an
// RMS estimate towards the peaks of a low sine, which pulled the very bass it
// was supposed to lift back down.
constexpr float kDetectorMs = 30.f;

constexpr float kWidth = 1.10f;   // mid/side, a little wider than the source
constexpr float kCeiling = 0.95f;
constexpr float kLimitReleaseMs = 120.f;

inline float dbToLinear(float db)
{
    return std::pow(10.f, db / 20.f);
}

} // namespace

Mastering::Mastering() = default;

void Mastering::setEq(const std::vector<Band> &bands, float preampDb)
{
    m_eq.assign(bands.size(), {});
    for (size_t i = 0; i < bands.size(); ++i) {
        m_eq[i].ch[0].configure(bands[i]);
        m_eq[i].ch[1].configure(bands[i]);
    }
    m_preamp = dbToLinear(preampDb);
    reset();
}

void Mastering::reset()
{
    for (StereoBand &band : m_eq) {
        for (Biquad &ch : band.ch)
            ch.z1 = ch.z2 = 0.f;
    }
    m_envSq = 0.f;
    m_limitEnv = 0.f;
}

// RBJ cookbook shelves and bell, normalised and stored in transposed direct
// form II (run()). Shelves use a slope of S = 1; their Q field is ignored.
void Mastering::Biquad::configure(const Band &band)
{
    const double A = std::pow(10.0, band.gainDb / 40.0);
    const double w0 = 2.0 * M_PI * band.fc / kRate;
    const double cw = std::cos(w0), sw = std::sin(w0);
    double nb0, nb1, nb2, na0, na1, na2;

    switch (band.type) {
    case BandType::Peaking: {
        const double q = std::max(0.05, double(band.q));
        const double alpha = sw / (2.0 * q);
        nb0 = 1 + alpha * A;
        nb1 = -2 * cw;
        nb2 = 1 - alpha * A;
        na0 = 1 + alpha / A;
        na1 = -2 * cw;
        na2 = 1 - alpha / A;
        break;
    }
    case BandType::LowShelf: {
        const double alpha = sw / 2.0 * std::sqrt(2.0);
        const double sa = 2.0 * std::sqrt(A) * alpha;
        nb0 = A * ((A + 1) - (A - 1) * cw + sa);
        nb1 = 2 * A * ((A - 1) - (A + 1) * cw);
        nb2 = A * ((A + 1) - (A - 1) * cw - sa);
        na0 = (A + 1) + (A - 1) * cw + sa;
        na1 = -2 * ((A - 1) + (A + 1) * cw);
        na2 = (A + 1) + (A - 1) * cw - sa;
        break;
    }
    case BandType::HighShelf: {
        const double alpha = sw / 2.0 * std::sqrt(2.0);
        const double sa = 2.0 * std::sqrt(A) * alpha;
        nb0 = A * ((A + 1) + (A - 1) * cw + sa);
        nb1 = -2 * A * ((A - 1) + (A + 1) * cw);
        nb2 = A * ((A + 1) + (A - 1) * cw - sa);
        na0 = (A + 1) - (A - 1) * cw + sa;
        na1 = 2 * ((A - 1) - (A + 1) * cw);
        na2 = (A + 1) - (A - 1) * cw - sa;
        break;
    }
    }

    b0 = float(nb0 / na0);
    b1 = float(nb1 / na0);
    b2 = float(nb2 / na0);
    a1 = float(na1 / na0);
    a2 = float(na2 / na0);
}

float Mastering::Biquad::run(float x)
{
    const float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
}

void Mastering::process(float *frames, int count)
{
    const float makeup = dbToLinear(kMakeupDb);
    const float threshold = dbToLinear(kThresholdDb);
    constexpr float compExponent = 1.f - 1.f / kRatio;
    const float detector = 1.f - std::exp(-1.f / (kDetectorMs * 0.001f * kRate));
    const float limitRelease = std::exp(-1.f / (kLimitReleaseMs * 0.001f * kRate));

    for (int i = 0; i < count; ++i) {
        float l = frames[i * 2] * m_preamp;
        float r = frames[i * 2 + 1] * m_preamp;
        for (StereoBand &band : m_eq) {
            l = band.ch[0].run(l);
            r = band.ch[1].run(r);
        }

        // Stereo-linked RMS: both channels share one gain, so the image does
        // not wobble while the detector decides.
        const float meanSquare = 0.5f * (l * l + r * r);
        m_envSq += (meanSquare - m_envSq) * detector;

        float gain = 1.f;
        const float env = std::sqrt(m_envSq);
        if (env > threshold)
            gain = std::pow(threshold / env, compExponent);   // (T/E)^(1-1/ratio)
        l *= gain * makeup;
        r *= gain * makeup;

        // Mid/side width. At 1.0 this is a no-op; beyond it the sides (reverb,
        // doubled guitars) stand out without pulling the centre apart.
        const float mid = 0.5f * (l + r);
        const float side = 0.5f * (l - r) * kWidth;
        l = mid + side;
        r = mid - side;

        // Peak limiter: instant attack, exponential release. The chain's peaks
        // hit this and only this, so it is the one stage that may bend the
        // waveform on a loud master.
        const float peak = std::max(std::abs(l), std::abs(r));
        m_limitEnv = std::max(peak, m_limitEnv * limitRelease);
        if (m_limitEnv > kCeiling) {
            const float limit = kCeiling / m_limitEnv;
            l *= limit;
            r *= limit;
        }

        frames[i * 2] = l;
        frames[i * 2 + 1] = r;
    }
}
