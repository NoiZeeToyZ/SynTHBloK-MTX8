#pragma once
// TEMPO LINK (Nov 16 2026, Nick: "lock them"): every MTX module whose CLOCK jack is unpatched shares ONE internal clock,
// so MTX-1, MTX-8 (and any other module that uses this) step together with nothing patched. One object per Rack process
// (SharedSlot), so modules in different plugins (free singles, the paid plugin) share it.
//  - The first linked module leads: it runs the 16th-note timer from the shared tempo and counts ticks; the others step
//    when the tick count changes. A leader that goes away (removed, CLOCK patched, bypassed) stops beating; after
//    ~0.1 s another linked module takes over, so the clock never stops while one is still linked.
//  - Tempo: turning any linked module's BPM sets the shared tempo; the others' BPM follow it (kept in step on screen).
//  - RESET on any linked module resets them all (the leader restarts its timer, so the next 16th is step 1 for all).
//  - A patched CLOCK always wins: that module leaves the link and follows its jack, as before.
#include <atomic>
#include <cstdint>
#include "SharedSlot.hpp"

namespace tempolink {

struct Shared {
    std::atomic<int64_t> leader{-1};
    std::atomic<uint32_t> tick{0};        // 16ths counted by the leader
    std::atomic<uint32_t> beat{0};        // the leader's heartbeat (every sample it runs)
    std::atomic<uint32_t> resetGen{0};    // bumped by any linked module's RESET
    std::atomic<float> bpm{120.f};
};
inline Shared& shared() { static Shared* s = sharedslot::get<Shared>("SYNTHBLOK_TEMPOLINK_1"); return *s; }

struct Out { bool clock = false; bool reset = false; };

struct Link {
    double timer = 0.0;
    uint32_t lastTick = 0, lastReset = 0, lastBeat = 0;
    float lastBpm = -1.f;     // this module's BPM knob as last seen (a change = the user turned it)
    float quiet = 0.f;        // seconds the leader's heartbeat has not moved
    bool joined = false;

    // Call every sample while CLOCK is unpatched. bpm = this module's BPM knob (in / out: set to the shared tempo when
    // another module changed it). reset = this module's own RESET this sample. Returns this sample's 16th and a reset
    // that came from another linked module.
    Out process(int64_t id, float dt, float& bpm, bool reset) {
        Shared& S = shared();
        Out o;
        if (!joined) {   // join in step with the others: no stale tick / reset counts
            joined = true;
            lastTick = S.tick.load(); lastReset = S.resetGen.load(); lastBeat = S.beat.load(); quiet = 0.f;
            lastBpm = -1.f;
        }
        // the tempo: our knob turned -> everyone's; else follow the shared one
        if (lastBpm >= 0.f && bpm != lastBpm) S.bpm.store(bpm);
        else if (S.bpm.load() != bpm) {
            if (S.leader.load() < 0 && lastBpm < 0.f) S.bpm.store(bpm);   // the first one in brings its tempo
            else bpm = S.bpm.load();
        }
        lastBpm = bpm;
        // leadership: free, or the leader went quiet
        int64_t none = -1;
        S.leader.compare_exchange_strong(none, id);
        uint32_t b = S.beat.load();
        if (b != lastBeat) { lastBeat = b; quiet = 0.f; } else quiet += dt;
        if (S.leader.load() != id && quiet > 0.1f) { S.leader.store(id); timer = 0.0; }
        bool lead = S.leader.load() == id;
        if (reset) S.resetGen.fetch_add(1);
        uint32_t rg = S.resetGen.load();
        bool anyReset = rg != lastReset;
        lastReset = rg;
        o.reset = anyReset && !reset;   // someone else's RESET (ours is handled by the caller already)
        if (lead) {
            S.beat.fetch_add(1);
            lastBeat = S.beat.load();
            if (anyReset) timer = 0.0;
            double step = 15.0 / std::max(1.f, S.bpm.load());
            timer += dt;
            if (timer >= step) { timer = std::min(timer - step, step); S.tick.fetch_add(1); }
        }
        uint32_t t = S.tick.load();
        o.clock = t != lastTick;
        lastTick = t;
        return o;
    }
    // CLOCK patched, bypassed or removed: hand the lead on
    void leave(int64_t id) {
        if (!joined) return;
        joined = false;
        int64_t me = id;
        shared().leader.compare_exchange_strong(me, -1);
    }
};

}  // namespace tempolink

SHAREDSLOT_WARM(kTempoLinkWarm, tempolink::shared())
