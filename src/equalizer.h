#pragma once

#include <vector>

// The EQ stage of the mixer: a configurable set of biquad bands (the selected
// device profile). The bands are built once by setEq() and never allocated
// while process() runs. There is no compression or width: this stage only
// shapes the tone and keeps the peaks inside full scale.
//
// Pure maths, no Qt, and deterministic: the tests drive it directly.
class Equalizer
{
public:
    enum class BandType { Peaking, LowShelf, HighShelf };

    struct Band
    {
        BandType type = BandType::Peaking;
        float fc = 1000.f;
        float gainDb = 0.f;
        float q = 1.f;        // peaking only; shelves use a fixed slope (S = 1)
    };

    Equalizer() = default;

    // Replaces the tone stage. `preampDb` is applied before the bands, as
    // AutoEQ presets expect (their positive corrections need the headroom).
    void setEq(const std::vector<Band> &bands, float preampDb);
    void reset();
    // `frames` interleaved stereo pairs (48 kHz), modified in place.
    void process(float *frames, int count);

private:
    struct Biquad
    {
        float b0 = 1.f, b1 = 0.f, b2 = 0.f, a1 = 0.f, a2 = 0.f;
        float z1 = 0.f, z2 = 0.f;

        void configure(const Band &band);
        float run(float x);
    };
    struct StereoBand
    {
        Biquad ch[2];
    };

    std::vector<StereoBand> m_eq;
    float m_preamp = 1.f;
};
