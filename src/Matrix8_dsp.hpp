#pragma once
// Shared DSP for SynTHBloK MATRIX-8 and MATRIX FORGE: the 8 row voices (delay and reverb: Echo_dsp.hpp).
#include "plugin.hpp"
#include "Echo_dsp.hpp"   // M8Delay / M8Reverb
#include "KnobMemo.hpp"   // M8Memo
#include "AnalogVoice.hpp"   // fastTanh, AnalogDrift, WarmLadder
#include <cmath>
#include <vector>

namespace {

// ---- synth voice ------------------------------------------------------------
enum VoiceParam { VP_PITCH, VP_CUTOFF, VP_RES, VP_DECAY, VP_WAVE, VP_LEVEL, VP_PAN, VP_DRIVE };
static const char* kBankName[8] = {"PITCH", "CUTOFF", "RESO", "DECAY", "WAVE", "LEVEL", "PAN", "DRIVE"};
// Starting strip values for a new module, per bank (row 1 = top voice).
// Pitch: a minor pentatonic run from C2 (bottom row) up; 36 semitones across a full strip.
static const float kDefaultVal[8][8] = {
    {17 / 36.f, 15 / 36.f, 12 / 36.f, 10 / 36.f, 7 / 36.f, 5 / 36.f, 3 / 36.f, 0.f},
    {0.62f, 0.62f, 0.62f, 0.62f, 0.62f, 0.62f, 0.62f, 0.62f},
    {0.25f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f},
    {0.35f, 0.35f, 0.35f, 0.35f, 0.35f, 0.35f, 0.35f, 0.35f},
    {0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f},
    {0.75f, 0.75f, 0.75f, 0.75f, 0.75f, 0.75f, 0.75f, 0.75f},
    {0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f},
    {0.15f, 0.15f, 0.15f, 0.15f, 0.15f, 0.15f, 0.15f, 0.15f}};

static float polyBlep(float t, float dt) {
    if (t < dt) { t /= dt; return t + t - t * t - 1.f; }
    if (t > 1.f - dt) { t = (t - 1.f) / dt; return t * t + t + t + 1.f; }
    return 0.f;
}

// Morphing wave at `phase` (0..1): saw -> pulse -> sine as w goes 0 -> 0.5 -> 1 (band-limited edges).
// pw: the pulse's width (0.5 = square); the voices wobble it a little so the square breathes like an analog one.
static float morphWave(float phase, float dt, float w, float pw = 0.5f) {
    float t2 = phase + (1.f - pw); if (t2 >= 1.f) t2 -= 1.f;
    float sq = (phase < pw ? 1.f : -1.f) + polyBlep(phase, dt) - polyBlep(t2, dt) + (1.f - 2.f * pw);   // no DC
    // only the two waves being blended are worked out (the sine is the costly one)
    if (w < 0.5f) {
        float saw = 2.f * phase - 1.f - polyBlep(phase, dt);
        return saw + (sq - saw) * (w * 2.f);
    }
    float sine = std::sin(2.f * float(M_PI) * phase);
    return sq + (sine - sq) * ((w - 0.5f) * 2.f);
}

// Morphing oscillator.
struct M8Osc {
    float phase = 0.f;
    float process(float dt, float w, float pw = 0.5f) {
        phase += dt;
        if (phase >= 1.f) phase -= 1.f;
        return morphWave(phase, dt, w, pw);
    }
};

// TPT state-variable filter: low-, band- and high-pass from one pass.
struct M8Svf {
    float ic1 = 0.f, ic2 = 0.f;
    void process(float x, float fc, float res, float sr, float& lp, float& bp, float& hp) {
        float g = std::tan(float(M_PI) * std::min(fc, sr * 0.45f) / sr);
        float k = 2.f - 1.9f * res;
        float a1 = 1.f / (1.f + g * (g + k)), a2 = g * a1, a3 = g * a2;
        float v3 = x - ic2;
        float v1 = a1 * ic1 + a2 * v3;
        float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.f * v1 - ic1;
        ic2 = 2.f * v2 - ic2;
        lp = v2; bp = v1; hp = x - k * v1 - v2;
    }
    // The same filter with analog-style saturation: the input is softly clipped and the resonant state is held
    // under +-2 by a tanh, so resonance thickens and sings instead of turning into a thin digital whistle, and
    // loud notes round off rather than going glassy. Quiet signals pass exactly as process() would.
    void processWarm(float x, float fc, float res, float sr, float& lp, float& bp, float& hp) {
        float g = std::tan(float(M_PI) * std::min(fc, sr * 0.45f) / sr);
        float k = 2.f - 1.9f * res;
        x = 1.2f * fastTanh(x * (1.f / 1.2f));
        float a1 = 1.f / (1.f + g * (g + k)), a2 = g * a1, a3 = g * a2;
        float v3 = x - ic2;
        float v1 = a1 * ic1 + a2 * v3;
        float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.f * fastTanh(0.5f * (2.f * v1 - ic1));
        ic2 = 2.f * v2 - ic2;
        lp = v2; bp = v1; hp = x - k * v1 - v2;
    }
};

// Shared "filter 2 + envelope" section (SynTHBloK style), the same for all 8 voices.
struct M8Global {
    float cutoff = 1.f, res = 0.f, envAmt = 0.f, mode = 0.f;   // 0..1 each
    float a = 0.f, d = 0.5f, s = 1.f, r = 0.6f;                // 0..1 each
    float gate = 0.125f;                                       // note length in seconds (one clock step)
};

// One voice: two detuned morphing oscillators plus a sine sub an octave down, a slow random
// "analog" drift on pitch and cutoff, resonant low-pass swept by its decay (the strips), then
// the shared filter 2 (LP -> BP -> HP morph) swept by the shared ADSR, drive, and the ADSR on
// the level as well.
//
// Analog pass (Nov 11 2026), so a pattern sounds like a real synth, not a chip: each oscillator drifts on its own
// (the two never lock into one steady beat), the voice's detune is a little different on every row, the square
// is a pulse whose width breathes slowly, a breath of noise under it, both filters saturate (warm, singing
// resonance), the voice filter follows the note (key tracking) and opens further on accents, and each pluck
// starts with a 1.5 ms attack instead of an instant jump (no click when a ringing note is hit again).
struct M8Voice {
    M8Osc oscA, oscB;
    M8Svf f1, f2;
    float subPhase = 0.f, env = 0.f, cut = 0.5f;
    float adsr = 0.f, gateLeft = 0.f, vel = 1.f;   // vel: the hit's level (an accent plays above 1)
    int stage = 0;   // 0 idle, 1 attack, 2 decay/sustain, 3 release
    bool plucking = false;                         // the pluck envelope's 1.5 ms attack is running
    float drift = 0.f, driftTarget = 0.f, driftTimer = 0.f;
    AnalogDrift dA, dB;                            // each oscillator's own wander
    float detune = 1.0041f;                        // +-7 cents, varied a little per row voice
    float pwPh = 0.f, pwRate = 0.5f;               // the pulse-width wobble
    uint32_t rng = 1;
    // values that depend only on a knob (and the sample rate) are worked out again only when it moves, not every
    // sample (Nov 11 2026: the same formulas, so the same output; MTX-8 and FORGE run 8 of these voices)
    M8Memo mA, mD, mR, mDecay, mDrive, mCut2, mCut1, mPluck, mKey;
    float panIn = -1e30f;
    float panL = 0.f, panR = 0.f;
    explicit M8Voice(uint32_t seed = 1) : dA(seed * 7u + 3u), dB(seed * 13u + 5u), rng(seed * 2654435761u + 1u) {
        oscB.phase = 0.37f;
        detune = std::pow(2.f, (5.5f + 3.f * rand01()) / 1200.f);   // +-5.5..8.5 cents
        pwPh = rand01();
        pwRate = 0.25f + 0.5f * rand01();
    }
    float rand01() { rng = rng * 1664525u + 1013904223u; return (rng >> 8) * (1.f / 16777216.f); }
    void trigger(float gate, float v = 1.f) { plucking = true; stage = 1; gateLeft = gate; vel = v; }
    // the pan law for this voice's PAN (cos / sin of pan * pi / 2), worked out when PAN moves
    void pan(float p, float& l, float& r) {
        if (p != panIn) { panIn = p; float a = p * float(M_PI) * 0.5f; panL = std::cos(a); panR = std::sin(a); }
        l = panL; r = panR;
    }
    float process(float sr, const float* p, const M8Global& g) {
        // ADSR: attack 1 ms..2 s, decay 5 ms..2 s, release 5 ms..4 s (time constants)
        auto coef = [&](float t) { return 1.f - std::exp(-1.f / (t * sr)); };
        gateLeft -= 1.f / sr;
        if (gateLeft <= 0.f && (stage == 1 || stage == 2)) stage = 3;
        if (stage == 1) {
            adsr += (1.2f - adsr) * mA.get(g.a, sr, [&](float a) { return coef(0.001f * std::pow(2000.f, a)); });
            if (adsr >= 1.f) { adsr = 1.f; stage = 2; }
        } else if (stage == 2) {
            adsr += (g.s - adsr) * mD.get(g.d, sr, [&](float d) { return coef(0.005f * std::pow(400.f, d)); });
        } else if (stage == 3) {
            adsr += (0.f - adsr) * mR.get(g.r, sr, [&](float r) { return coef(0.005f * std::pow(800.f, r)); });
            if (adsr < 1e-4f) { adsr = 0.f; stage = 0; }
        }

        // drift: a new random target every ~0.3-1 s, glided to; +-1 = +-6 cents / +-0.03 cutoff
        driftTimer -= 1.f / sr;
        if (driftTimer <= 0.f) { driftTarget = rand01() * 2.f - 1.f; driftTimer = 0.3f + 0.7f * rand01(); }
        drift += (driftTarget - drift) * (2.f / sr);

        float note = std::round(p[VP_PITCH] * 36.f);
        float semis = note + drift * 0.06f;
        float freq = 65.406f * std::pow(2.f, semis / 12.f);
        // each oscillator: up to +-3 cents of its own wander on top of the voice's detune
        // (2^x = 1 + x ln 2 this close to 0: 3 cents is x = 0.0025)
        float da = 1.f + dA.process(sr) * (3.f / 1200.f * 0.6931472f), db = 1.f + dB.process(sr) * (3.f / 1200.f * 0.6931472f);
        float w = p[VP_WAVE];
        pwPh += pwRate / sr;
        if (pwPh >= 1.f) pwPh -= 1.f;
        float pw = 0.5f + 0.07f * (4.f * std::fabs(pwPh - 0.5f) - 1.f);   // a slow triangle wobble
        float osc = 0.5f * (oscA.process(std::min(freq * detune * da / sr, 0.45f), w, pw)
                          + oscB.process(std::min(freq / detune * db / sr, 0.45f), w, 1.f - pw));
        subPhase += 0.5f * freq / sr;
        if (subPhase >= 1.f) subPhase -= 1.f;
        osc += 0.35f * std::sin(2.f * float(M_PI) * subPhase);
        osc += 0.006f * (rand01() * 2.f - 1.f);   // a breath of analog noise

        // pluck: 1.5 ms attack, then the decay (time constant 15 ms .. 1.2 s)
        if (plucking) {
            env += (1.05f - env) * mPluck.get(0.f, sr, [&](float) { return coef(0.0015f); });
            if (env >= 1.f) { env = 1.f; plucking = false; }
        } else {
            env *= mDecay.get(p[VP_DECAY], sr, [&](float d) { float decay = 0.015f * std::pow(80.f, d); return std::exp(-1.f / (decay * sr)); });
        }

        // cutoff 40 Hz .. 16 kHz (at C3; follows the note at 35%), +2.5 octaves of envelope sweep (more on an
        // accent, less on a soft hit); smoothed against zipper noise
        cut += (p[VP_CUTOFF] + drift * 0.03f - cut) * 0.002f;
        float sweep = 2.5f * (0.6f + 0.4f * vel);
        float key = mKey.get(note, sr, [](float n) { return std::pow(2.f, 0.35f * (n - 12.f) / 12.f); });
        float lp, bp, hp;
        f1.processWarm(osc, 40.f * key * mCut1.get(cut, sr, [](float c) { return std::pow(400.f, c); }) * std::pow(2.f, sweep * env), p[VP_RES], sr, lp, bp, hp);

        // filter 2: cutoff 20 Hz..20 kHz, envelope adds up to +6 octaves
        float fc2 = 20.f * mCut2.get(g.cutoff, sr, [](float c) { return std::pow(1000.f, c); }) * std::pow(2.f, 6.f * g.envAmt * adsr);
        float lp2, bp2, hp2;
        f2.processWarm(lp, fc2, g.res, sr, lp2, bp2, hp2);
        float y = g.mode < 0.5f ? lp2 + (bp2 - lp2) * (g.mode * 2.f) : bp2 + (hp2 - bp2) * ((g.mode - 0.5f) * 2.f);

        float drive = 1.f + 6.f * p[VP_DRIVE];
        return std::tanh(y * drive) / mDrive.get(drive, sr, [](float d) { return std::tanh(d); }) * env * adsr * p[VP_LEVEL] * vel;
    }
};

// The ping-pong delay (M8Delay) and reverb (M8Reverb) live in Echo_dsp.hpp, shared with MATRIX-ECHO.

} // namespace
