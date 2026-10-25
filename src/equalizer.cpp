#include "equalizer.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr float kRate = 48000.f;

inline float dbToLinear(float db)
{
    return std::pow(10.f, db / 20.f);
}

} // namespace

void Equalizer::setEq(const std::vector<Band> &bands, float preampDb)
{
    m_eq.assign(bands.size(), {});
    for (size_t i = 0; i < bands.size(); ++i) {
        m_eq[i].ch[0].configure(bands[i]);
        m_eq[i].ch[1].configure(bands[i]);
    }
    m_preamp = dbToLinear(preampDb);
    reset();
}

void Equalizer::reset()
{
    for (StereoBand &band : m_eq) {
        for (Biquad &ch : band.ch)
            ch.z1 = ch.z2 = 0.f;
    }
}

// RBJ cookbook shelves and bell, normalised and stored in transposed direct
// form II (run()). Shelves use a slope of S = 1; their Q field is ignored.
void Equalizer::Biquad::configure(const Band &band)
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

float Equalizer::Biquad::run(float x)
{
    const float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
}

void Equalizer::process(float *frames, int count)
{
    for (int i = 0; i < count; ++i) {
        float l = frames[i * 2] * m_preamp;
        float r = frames[i * 2 + 1] * m_preamp;
        for (StereoBand &band : m_eq) {
            l = band.ch[0].run(l);
            r = band.ch[1].run(r);
        }
        // Profiles are preamp-compensated, but a stack of overlapping boosts
        // can still overshoot; full scale is the hard stop.
        frames[i * 2] = std::clamp(l, -1.f, 1.f);
        frames[i * 2 + 1] = std::clamp(r, -1.f, 1.f);
    }
}
