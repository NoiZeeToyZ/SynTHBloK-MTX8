#pragma once
// MTX-8's sequencer helpers (Nov 16 2026, Nick: "move [MTX-8] to free as well, give it the upgrades"), kept free of Rack
// so dev/mtx8_test.cpp can check them offline:
//  - KEY + SCALE: the same 12 scales, in the same order, as MTX-DUO / MTX Quad (patches store the number), plus a 13th,
//    CHROMATIC = the old MTX-8 (strip A = semitones from C2, nothing quantised). A patch saved before the upgrade loads
//    as CHROMATIC in C, so it plays exactly as it did.
//  - the internal tempo used when CLOCK is unpatched (as DUO / DRUM: 40..240 BPM, 16th-note steps).
//  - RANDOM: MATRIX FORGE's random (a theme and variations over four chords, in key) without RANDROYD's genre rules
//    (they live in the paid plugin) and without FORGE's accents (MTX-8 has none).
#include <algorithm>
#include <cmath>
#include <vector>

namespace m8seq {

static const int kScaleCount = 12;   // = MTX-DUO's list; index kScaleCount = CHROMATIC (MTX-8 only)
static const int kChromatic = kScaleCount;
static const char* const kKeyName[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static const char* const kScaleName[kScaleCount + 1] = {"MINOR", "MAJOR", "DORIAN", "PHRYGIAN", "MIXOLYD", "HARM MIN",
    "MIN PENT", "MAJ PENT", "PHRYG DOM", "HIRAJOSHI", "PELOG", "BHAIRAV", "CHROMATIC"};
static const int kScaleLen[kScaleCount] = {7, 7, 7, 7, 7, 7, 5, 5, 7, 5, 5, 7};
static const int kScale[kScaleCount][7] = {
    {0, 2, 3, 5, 7, 8, 10}, {0, 2, 4, 5, 7, 9, 11}, {0, 2, 3, 5, 7, 9, 10}, {0, 1, 3, 5, 7, 8, 10},
    {0, 2, 4, 5, 7, 9, 10}, {0, 2, 3, 5, 7, 8, 11}, {0, 3, 5, 7, 10}, {0, 2, 4, 7, 9},
    {0, 1, 4, 5, 7, 8, 10}, {0, 2, 3, 7, 8}, {0, 1, 3, 7, 8}, {0, 1, 4, 5, 7, 8, 11}};

// semitones above the key root for strip semitone n (0..36): the nearest note of the scale (a tie goes down);
// CHROMATIC (or an unknown scale) leaves it as it is
inline int snap(int n, int scale) {
    if (scale < 0 || scale >= kScaleCount) return n;
    int oct = n / 12, pc = n % 12, best = 0, bestD = 99;
    for (int i = 0; i <= kScaleLen[scale]; i++) {
        int t = i < kScaleLen[scale] ? kScale[scale][i] : 12;
        int d = std::abs(t - pc);
        if (d < bestD) { bestD = d; best = t; }
    }
    return oct * 12 + best;
}
// a voice's note in semitones above C2 for its strip A value (0..1), moved up to the KEY (C = 0): the old MTX-8 played
// round(v * 36), which is CHROMATIC in C
inline int voiceNote(float v, int key, int scale) {
    return snap((int) std::round(v * 36.f), scale) + key;
}

// the internal clock: one 16th-note step every 15 / BPM seconds (as MTX-DUO / MTX-DRUM; the timer is a double, so at
// 40 BPM the float sum of ~18000 sample steps can't drift the tempo by a sample a step)
struct IntClock {
    double timer = 0.0;
    bool process(float dt, float bpm) {
        double step = 15.0 / std::max(1.f, bpm);
        timer += dt;
        if (timer >= step) { timer = std::min(timer - step, step); return true; }
        return false;
    }
};

// RANDOM: 8 related patterns over a chord progression, and new strip values on all 8 banks; always in KEY / SCALE.
// MATRIX FORGE's random (the theme, its variations and fills, the bass / upper row split, the strip ranges): the bottom
// four rows are the chord roots in the bass, the top four the chord tones that cover every chord best; patterns 1..4
// play the four chords and 5..8 play them again, each only on the rows that belong to its chord. Pitches are written
// as semitones above the key root (so they are on the scale: snap() leaves them alone).
// rnd() in [0, 1); pat[pattern][row][step]; val[bank][row] with bank 0 = PITCH (VoiceParam order of Matrix8_dsp.hpp).
template <class Rnd>
void randomize(Rnd rnd, int scale, bool pat[8][8][8], float val[8][8]) {
    auto pick = [&](int n) { return std::min((int) (rnd() * n), n - 1); };
    auto spread = [&](int k, int rot) {   // k hits spread evenly over the 8 steps
        int m = 0;
        for (int i = 0; i < 8; i++) if ((i * k) % 8 < k) m |= 1 << ((i + rot) % 8);
        return m;
    };
    int s = scale >= 0 && scale < kScaleCount ? scale : 0;
    const int* sc = kScale[s];
    int n = kScaleLen[s];
    auto tone = [&](int deg, int i) { return sc[(deg + 2 * i) % n]; };   // pitch class of a chord's root / 3rd / 5th
    auto inScale = [&](int pc) { for (int i = 0; i < n; i++) if (sc[i] == pc % 12) return true; return false; };
    // FORGE's progressions; on a 7-note scale only chords with a perfect fifth, on a 5-note one any degree it has
    static const int kProg[6][4] = {{0, 5, 3, 4}, {0, 4, 5, 3}, {0, 3, 4, 0}, {0, 3, 0, 4}, {0, 5, 3, 0}, {0, 2, 3, 4}};
    auto ok = [&](int d) { return d < n && (n < 7 || (tone(d, 2) - tone(d, 0) + 12) % 12 == 7); };
    std::vector<int> fit;
    for (int i = 0; i < 6; i++) if (ok(kProg[i][0]) && ok(kProg[i][1]) && ok(kProg[i][2]) && ok(kProg[i][3])) fit.push_back(i);
    int prog[4] = {0, 0, 0, 0};
    if (!fit.empty()) { const int* kp = kProg[fit[pick((int) fit.size())]]; for (int k = 0; k < 4; k++) prog[k] = kp[k]; }
    int cpc[4][3];   // each chord's triad (pitch classes)
    for (int k = 0; k < 4; k++) for (int i = 0; i < 3; i++) cpc[k][i] = tone(prog[k], i);
    auto inChord = [&](int pc, int k) { for (int i = 0; i < 3; i++) if (cpc[k][i] == pc % 12) return true; return false; };
    // bass rows (7 = lowest .. 4): the chord roots, then the key's fifth, its octave and other scale notes as spares
    std::vector<int> bass;
    for (int k = 0; k < 4; k++) if (std::find(bass.begin(), bass.end(), tone(prog[k], 0)) == bass.end()) bass.push_back(tone(prog[k], 0));
    for (int x : {7, 12, 3, 4, 5, 10, 9, 2})
        if ((int) bass.size() < 4 && inScale(x) && std::find(bass.begin(), bass.end(), x) == bass.end()) bass.push_back(x);
    for (int x = 0; (int) bass.size() < 4; x++) bass.push_back(sc[x % n] + 12);   // (never needed: every scale has 4 notes)
    std::sort(bass.begin(), bass.end());
    // upper rows (3 .. 0 = highest): the 4 chord tones that cover the chords best (two or more each)
    std::vector<int> cand;
    for (int k = 0; k < 4; k++) for (int i = 0; i < 3; i++)
        if (std::find(cand.begin(), cand.end(), cpc[k][i]) == cand.end()) cand.push_back(cpc[k][i]);
    for (int i = 0; i < n && (int) cand.size() < 4; i++)   // one chord only: other scale notes fill in
        if (std::find(cand.begin(), cand.end(), sc[i]) == cand.end()) cand.push_back(sc[i]);
    std::vector<int> best;
    float bestScore = -1.f;
    int nc = (int) cand.size();
    for (int a = 0; a < nc; a++) for (int b = a + 1; b < nc; b++) for (int c = b + 1; c < nc; c++) for (int d = c + 1; d < nc; d++) {
        int lo = 9, sum = 0;
        for (int k = 0; k < 4; k++) {
            int h = inChord(cand[a], k) + inChord(cand[b], k) + inChord(cand[c], k) + inChord(cand[d], k);
            lo = std::min(lo, h); sum += h;
        }
        float score = lo * 100.f + sum + rnd();
        if (score > bestScore) { bestScore = score; best = {cand[a], cand[b], cand[c], cand[d]}; }
    }
    std::vector<int> upper;
    for (int pc : best) upper.push_back(pc + (pc + 12 <= 14 ? 24 : 12));   // above the bass, within 3 octaves
    std::sort(upper.begin(), upper.end());
    int pitch[8];
    for (int i = 0; i < 4; i++) { pitch[7 - i] = bass[i]; pitch[3 - i] = upper[i]; }
    for (int r = 0; r < 8; r++) val[0][r] = pitch[r] / 36.f;
    // the theme (bit c = step c): rows 3 and 2 hit the downbeat together, row 1 answers off the beat
    static const int kBass[4] = {0x11, 0x01, 0x91, 0x21};
    static const int kStab[3] = {0x11, 0x01, 0x05};
    static const int kOff[4] = {0x44, 0x22, 0x4C, 0xA0};
    int theme[8];
    int bassR = kBass[pick(4)];
    for (int r = 4; r < 8; r++) theme[r] = bassR;
    theme[3] = kStab[pick(3)];
    theme[2] = spread(2 + pick(2), 0);
    theme[1] = kOff[pick(4)];
    theme[0] = spread(3 + pick(3), pick(8));
    for (int p = 0; p < 8; p++) {
        int k = p % 4;
        for (int r = 0; r < 8; r++) {
            bool on = r >= 4 ? pitch[r] % 12 == tone(prog[k], 0) : inChord(pitch[r], k);
            int m = on ? theme[r] : 0;
            if (p > 0 && r < 4 && m && rnd() < 0.35f) m ^= 1 << (1 + 2 * pick(4));   // a variation off the beat
            if ((p == 3 || p == 7) && r < 2 && m) m |= 0xC0;                       // the fill: the last two steps
            for (int c = 0; c < 8; c++) pat[p][r][c] = m >> c & 1;
        }
    }
    // the sound banks, in FORGE's ranges (every voice clean, the bass centred and a little louder)
    for (int r = 0; r < 8; r++) {
        bool low = r >= 4;
        val[1][r] = 0.4f + rnd() * 0.4f;                                   // CUTOFF
        val[2][r] = rnd() * 0.4f;                                          // RESO
        val[3][r] = low ? 0.3f + rnd() * 0.35f : 0.12f + rnd() * 0.4f;     // DECAY
        val[4][r] = rnd();                                                 // WAVE
        val[5][r] = low ? 0.7f + rnd() * 0.15f : 0.55f + rnd() * 0.2f;     // LEVEL
        val[6][r] = low ? 0.5f : 0.3f + rnd() * 0.4f;                      // PAN
        val[7][r] = rnd() * 0.3f;                                          // DRIVE
    }
}

}  // namespace m8seq
