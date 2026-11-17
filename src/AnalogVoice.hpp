#pragma once
// Analog-style building blocks shared by the DUO and MATRIX-8 voices (Nov 2026 "lush" pass):
// a cheap tanh, slow random pitch drift, and a driven 4-pole ladder (the QUAD's design, with the cheap tanh).
// No Rack headers, so tools/sound_check/ can render the voices offline.
#include <cmath>
#include <cstdint>
#include <algorithm>

namespace {

// tanh, close enough for audio (Pade 3/2, exact at 0, clamped to +-1 past |x| = 3). Smooth and monotonic.
static inline float fastTanh(float x) {
    if (x > 3.f) return 1.f;
    if (x < -3.f) return -1.f;
    float x2 = x * x;
    return x * (27.f + x2) / (27.f + 9.f * x2);
}

// A slow random wander in -1..1: a new target every 0.3-1.2 s, glided to. Each oscillator gets its own, so the
// beating between detuned oscillators never settles into one fixed, machine-like rate.
struct AnalogDrift {
    uint32_t rng;
    float v = 0.f, target = 0.f, timer = 0.f;
    explicit AnalogDrift(uint32_t seed = 1) : rng(seed * 2246822519u + 374761393u) {}
    float rand01() { rng = rng * 1664525u + 1013904223u; return (rng >> 8) * (1.f / 16777216.f); }
    float process(float sr) {
        timer -= 1.f / sr;
        if (timer <= 0.f) { target = rand01() * 2.f - 1.f; timer = 0.3f + 0.9f * rand01(); }
        v += (target - v) * (1.5f / sr);
        return v;
    }
};

// Driven 4-pole ladder, 2x oversampled. Every stage saturates, so high resonance sings instead of whistling and
// can't run away; the passband is compensated so the low end stays as RES goes up.
// res 0..1 (self-oscillates near the top), drive 0..1 (pushes the input stage).
struct WarmLadder {
    float st[4] = {0.f, 0.f, 0.f, 0.f};
    float last = 0.f;
    float resIn = -1.f, k = 0.f;   // k worked out again only when RES moves
    void reset() { st[0] = st[1] = st[2] = st[3] = 0.f; last = 0.f; }
    float process(float in, float fc, float res, float sr, float drive = 0.f) {
        float hdt = 0.5f / sr;
        fc = std::min(std::max(fc, 15.f), 0.4f / hdt);
        float g = 1.f - std::exp(-2.f * float(M_PI) * fc * hdt);
        if (res != resIn) { resIn = res; k = 4.2f * std::pow(res, 1.2f); }
        float comp = 1.f + 0.55f * k;
        float pre = 1.f + 2.f * drive;
        float x0 = fastTanh(in * pre) / pre * (1.f + 0.5f * drive);
        float sum = 0.f;
        for (int os = 0; os < 2; os++) {
            float xin = os == 0 ? 0.5f * (last + x0) : x0;
            float u = fastTanh(xin * comp - k * st[3]);
            float t0 = fastTanh(st[0]), t1 = fastTanh(st[1]), t2 = fastTanh(st[2]);
            st[0] += g * (u - t0);
            st[1] += g * (t0 - t1);
            st[2] += g * (t1 - t2);
            st[3] += g * (t2 - fastTanh(st[3]));
            sum += st[3];
        }
        last = x0;
        return sum * 0.5f;
    }
};

} // namespace
