#include "mastering.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr float kRate = 48000.f;

// The curve. Small enough to stay out of the way on good masters, large enough
// to remove the thin, far-away sound of a laptop or earbud mix.
constexpr float kLowHz = 100.f;        // weight/body
constexpr float kLowDb = 4.f;
constexpr float kPresenceHz = 3200.f;  // vocals and guitars step forward
constexpr float kPresenceDb = 1.8f;
constexpr float kPresenceQ = 0.8f;
constexpr float kAirHz = 11000.f;      // detail, not sizzle
constexpr float kAirDb = 1.f;

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

Mastering::Mastering()
{
    for (int c = 0; c < 2; ++c) {
        m_low[c].lowShelf(kLowHz, kLowDb);
        m_presence[c].peaking(kPresenceHz, kPresenceDb, kPresenceQ);
        m_air[c].highShelf(kAirHz, kAirDb);
    }
}

void Mastering::reset()
{
    for (int c = 0; c < 2; ++c) {
        m_low[c].z1 = m_low[c].z2 = 0.f;
        m_presence[c].z1 = m_presence[c].z2 = 0.f;
        m_air[c].z1 = m_air[c].z2 = 0.f;
    }
    m_envSq = 0.f;
    m_limitEnv = 0.f;
}

void Mastering::Biquad::normalise(double nb0, double nb1, double nb2, double na0,
                                  double na1, double na2)
{
    b0 = float(nb0 / na0);
    b1 = float(nb1 / na0);
    b2 = float(nb2 / na0);
    a1 = float(na1 / na0);
    a2 = float(na2 / na0);
}

// RBJ cookbook shelves and bell, normalised and stored in transposed direct
// form II (run()).
void Mastering::Biquad::lowShelf(float f0, float db)
{
    const double A = std::pow(10.0, db / 40.0);
    const double w0 = 2.0 * M_PI * f0 / kRate;
    const double cw = std::cos(w0), sw = std::sin(w0);
    const double alpha = sw / 2.0 * std::sqrt(2.0);   // shelf slope S = 1
    const double sa = 2.0 * std::sqrt(A) * alpha;
    normalise(A * ((A + 1) - (A - 1) * cw + sa),
              2 * A * ((A - 1) - (A + 1) * cw),
              A * ((A + 1) - (A - 1) * cw - sa),
              (A + 1) + (A - 1) * cw + sa,
              -2 * ((A - 1) + (A + 1) * cw),
              (A + 1) + (A - 1) * cw - sa);
}

void Mastering::Biquad::peaking(float f0, float db, float q)
{
    const double A = std::pow(10.0, db / 40.0);
    const double w0 = 2.0 * M_PI * f0 / kRate;
    const double cw = std::cos(w0), sw = std::sin(w0);
    const double alpha = sw / (2.0 * q);
    normalise(1 + alpha * A, -2 * cw, 1 - alpha * A,
              1 + alpha / A, -2 * cw, 1 - alpha / A);
}

void Mastering::Biquad::highShelf(float f0, float db)
{
    const double A = std::pow(10.0, db / 40.0);
    const double w0 = 2.0 * M_PI * f0 / kRate;
    const double cw = std::cos(w0), sw = std::sin(w0);
    const double alpha = sw / 2.0 * std::sqrt(2.0);   // shelf slope S = 1
    const double sa = 2.0 * std::sqrt(A) * alpha;
    normalise(A * ((A + 1) + (A - 1) * cw + sa),
              -2 * A * ((A - 1) + (A + 1) * cw),
              A * ((A + 1) + (A - 1) * cw - sa),
              (A + 1) - (A - 1) * cw + sa,
              2 * ((A - 1) - (A + 1) * cw),
              (A + 1) - (A - 1) * cw - sa);
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
        float l = m_low[0].run(frames[i * 2]);
        float r = m_low[1].run(frames[i * 2 + 1]);
        l = m_presence[0].run(l);
        r = m_presence[1].run(r);
        l = m_air[0].run(l);
        r = m_air[1].run(r);

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
