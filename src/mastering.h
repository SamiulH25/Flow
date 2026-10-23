#pragma once

// The "full sound" chain: a fixed mastering pass applied to every frame on its
// way to the sound card. Weight below 100 Hz, presence around 3 kHz, a little
// air, RMS compression for density, a touch of stereo width, and a limiter so
// the boosts cannot clip. It is a taste, not a reference monitor chain: one
// setting that makes a normal library sound closer and less thin.
//
// Pure maths, no Qt, and deterministic: the tests drive it directly.
class Mastering
{
public:
    Mastering();

    void reset();
    // `frames` interleaved stereo pairs (48 kHz), modified in place.
    void process(float *frames, int count);

private:
    struct Biquad
    {
        float b0 = 1.f, b1 = 0.f, b2 = 0.f, a1 = 0.f, a2 = 0.f;
        float z1 = 0.f, z2 = 0.f;

        void normalise(double nb0, double nb1, double nb2, double na0,
                       double na1, double na2);
        void lowShelf(float f0, float db);
        void peaking(float f0, float db, float q);
        void highShelf(float f0, float db);
        float run(float x);
    };

    Biquad m_low[2];        // left, right: the same curve, separate state
    Biquad m_presence[2];
    Biquad m_air[2];

    float m_envSq = 0.f;    // RMS detector, mean square
    float m_limitEnv = 0.f; // limiter envelope, linear peak
};
