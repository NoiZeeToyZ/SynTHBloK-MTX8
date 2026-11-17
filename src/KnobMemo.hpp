#pragma once
// M8Memo: a value worked out from one input (usually a knob) and the sample rate, kept until either changes.
// The voices and filters used to call pow / exp / tan / tanh on knob values every sample, though knobs rarely move
// (Nov 11 2026). Same formula on the same numbers, so the output is the same, sample for sample.

namespace {

struct M8Memo {
    float in = -1e30f, sr = 0.f, out = 0.f;
    template <class F> float get(float x, float sampleRate, F f) {
        if (x != in || sampleRate != sr) { in = x; sr = sampleRate; out = f(x); }
        return out;
    }
};

} // namespace
