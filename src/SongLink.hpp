#pragma once
// MATRIX-SONG's link to the MATRIX modules, with no cables: a module that can be conducted derives from
// songlink::Member and joins in its constructor (leaves in its destructor). MATRIX-SONG finds the members by
// module id and, on a section change, asks each for a pattern, a mute or a loop action; its level strip sets
// the member's level, and the member reports its output peak for the strip's meter. The requests are
// atomics the member takes in its own process() (songPoll), so nothing crosses engine threads unguarded.
#include "plugin.hpp"
#include "SharedSlot.hpp"
#include "UserHold.hpp"
#include <atomic>
#include <mutex>
#include <vector>
#include <algorithm>

namespace songlink {

// loop actions a section can hold for a module with a looper
enum Loop { L_NONE, L_PLAY, L_STOP, L_REC, L_ONCE, L_COUNT };
static const char* const kLoopName[L_COUNT] = {"-", "PLAY", "STOP", "REC", "ONCE"};

struct Member {
    Module* self = NULL;
    const char* songName = "?";   // the row label, e.g. "DRUM"
    int songPatterns = 8;         // 0: no patterns (MATRIX-ECHO)
    bool songHasLoop = false;     // takes loop actions
    // requests from MATRIX-SONG (-1 / L_NONE: none)
    std::atomic<int> reqPattern{-1}, reqLoop{L_NONE};
    std::atomic<bool> songMuted{false}, songLinked{false};
    std::atomic<int> songRow{-1};          // the Composer row conducting this module (0-7; -1: none), for its badge
    // the Composer conducting this module (its module id; -1: free). One at a time: a second Composer leaves an
    // owned module alone until its owner lets it go (empties the row, or is deleted).
    std::atomic<int64_t> songOwner{-1};
    std::atomic<float> songLevel{1.f};   // MATRIX-SONG's level strip for this module (0..1)
    std::atomic<float> songPk{0.f};      // the member's output peak (V), for the strip's meter
    float songGainSm = 1.f, songDt = 1.f / 48000.f, songPkHold = 0.f;

    // CLOKWORX (Nov 11 2026): a CLOKWORX lane that owns this member replaces its clock (jack or BPM) with its
    // own ticks (x2, triplets, swing, polymeter...). A tick can carry a TRUE ROLL: the step that starts on it
    // plays N hits and the sequence does not move on. cwOwner = the CLOKWORX's module id (-1: free; one at a time).
    bool cwRolls = false;                  // the member plays rolls (set in its constructor); else ticks only
    std::atomic<int64_t> cwOwner{-1};
    std::atomic<int> cwTicks{0}, cwRollN{1}, cwLane{-1};
    std::atomic<bool> cwResetReq{false};
    std::atomic<float> cwPeriod{0.125f};   // seconds per tick (the member's step length while owned)
    bool cwOwned() { return cwOwner.load(std::memory_order_relaxed) >= 0; }
    // the member's clock edge this sample: its own (jackEdge) when free, else CLOKWORX's tick. rollN: hits for
    // the step starting now (1 a plain step, 0 a silent one: stop-time). cwSilentStep turns a 0 into "1, silent".
    static bool cwSilentStep(int& rollN) { if (rollN > 0) return false; rollN = 1; return true; }
    // (rollN as below)
    // the step starting now (1: a plain step).
    bool cwClock(bool jackEdge, int& rollN) {
        rollN = 1;
        if (!cwOwned()) return jackEdge;
        if (cwTicks.exchange(0) <= 0) return false;
        rollN = std::max(0, std::min(8, cwRollN.exchange(1)));   // 0: a SILENT step (CLOKWORX STOP): it advances, plays nothing
        return true;
    }
    bool cwReset() { return cwOwned() && cwResetReq.exchange(false); }
    // what the member really played (its step, hits so far): CLOKWORX's step ring and its CLOKWORX_LOG test hook
    std::atomic<int> cwStepNow{-1}, cwHitCount{0};
    void cwNote(int step, int hits = 1) { cwStepNow.store(step, std::memory_order_relaxed); cwHitCount.fetch_add(hits, std::memory_order_relaxed); }

    // the member's own pattern now (0-based; -1: none), read by MATRIX-SONG's display
    virtual int songPatternNow() { return -1; }
    // called from songPoll() in the member's process(): switch to pattern p (0-based) / do a loop action
    virtual void songSetPattern(int p) {}
    virtual void songDoLoop(int a) {}

    // every sample, first thing in process(): takes the requests, and glides to the level (0 while muted;
    // 5 ms, so strip moves and mutes never click). Returns the gain for the member's main outputs.
    float songPoll(float dt) {
        // USER HOLD (UserHold.hpp): a module the user touched since the last NEW TRACK keeps its pattern and plays
        // through the Composer's mutes (its level strip still applies)
        uint32_t hg = userhold::gen();
        if (hg != holdGen) { holdGen = hg; holdOn = self && userhold::held(self->id); }
        int p = reqPattern.exchange(-1);
        if (p >= 0 && !holdOn) songSetPattern(p);
        int a = reqLoop.exchange(L_NONE);
        if (a != L_NONE && !holdOn) songDoLoop(a);
        float tg = songMuted.load() && !holdOn ? 0.f : clamp(songLevel.load(), 0.f, 1.f);
        songGainSm += (tg - songGainSm) * std::min(1.f, dt / 0.005f);
        songDt = dt;
        return songGainSm;
    }
    // after the outputs are set: their peak (falls about 20 dB a second), shown on MATRIX-SONG's strip
    void songMeter(float l, float r) {
        float a = std::max(std::fabs(l), std::fabs(r));
        songPkHold = std::max(a, songPkHold * (1.f - 2.3f * songDt));
        songPk.store(songPkHold, std::memory_order_relaxed);
    }
    virtual ~Member() {}
    // (at the END: only the member's own songPoll reads these, so plugins built before them still line up)
    uint32_t holdGen = 0;
    bool holdOn = false;
};

// a tiny spin lock (held for a few list operations at a time). Not std::mutex: the registry is shared by several
// plugins, and on Windows each plugin carries its own copy of the threads library behind std::mutex.
struct Spin {
    std::atomic<bool> busy{false};
    void lock() { bool e = false; while (!busy.compare_exchange_weak(e, true, std::memory_order_acquire)) e = false; }
    void unlock() { busy.store(false, std::memory_order_release); }
};
struct Registry {
    Spin m;
    std::vector<Member*> v;
};
// one registry for the whole Rack process, so the Composer (MTX Brains) finds members in every SynTHBloK plugin
inline Registry& reg() { static Registry* r = sharedslot::get<Registry>("SYNTHBLOK_SONGLINK_2"); return *r; }

inline void join(Member* mb, Module* self, const char* name, int patterns, bool hasLoop) {
    mb->self = self; mb->songName = name; mb->songPatterns = patterns; mb->songHasLoop = hasLoop;
    Registry& r = reg();
    std::lock_guard<Spin> lk(r.m);
    r.v.push_back(mb);
}
inline void leave(Member* mb) {
    Registry& r = reg();
    std::lock_guard<Spin> lk(r.m);
    r.v.erase(std::remove(r.v.begin(), r.v.end(), mb), r.v.end());
}

// a true roll inside a member: start(n, stepSec) on the step's first hit, then tick(dt) every sample says when
// each further hit falls (n - 1 of them, evenly through the step)
struct RollTimer {
    int left = 0;
    float every = 0.f, t = 0.f;
    void start(int n, float stepSec) { left = n - 1; every = stepSec / std::max(1, n); t = 0.f; }
    void stop() { left = 0; }
    bool tick(float dt) {
        if (left <= 0) return false;
        t += dt;
        if (t < every) return false;
        t -= every; left--;
        return true;
    }
};

// what MATRIX-SONG may know of a member (copied out under the lock)
struct Info { int64_t id; const char* name; int patterns; bool hasLoop; int patNow; int64_t owner; };
inline std::vector<Info> list() {
    Registry& r = reg();
    std::lock_guard<Spin> lk(r.m);
    std::vector<Info> out;
    for (Member* mb : r.v)
        if (mb->self && mb->self->id >= 0)
            out.push_back({mb->self->id, mb->songName, mb->songPatterns, mb->songHasLoop, mb->songPatternNow(), mb->songOwner.load()});
    std::sort(out.begin(), out.end(), [](const Info& a, const Info& b) { return a.id < b.id; });
    return out;
}
// run f on the member with this module id, under the lock (false: it is gone)
template <typename F> bool with(int64_t id, F f) {
    Registry& r = reg();
    std::lock_guard<Spin> lk(r.m);
    for (Member* mb : r.v) if (mb->self && mb->self->id == id) { f(*mb); return true; }
    return false;
}

// THE ROW COLOURS: the Composer's rows, the badge on each conducted module and RANDROYD's cables all share
// them (MATRIX pattern colours: yellow -> red), so a module's row reads at a glance
inline NVGcolor rowColor(int r, int a = 255) {
    static const unsigned char C[8][3] = {{0xff, 0xd7, 0x14}, {0xff, 0xb0, 0x00}, {0xff, 0x8c, 0x00}, {0xff, 0x6a, 0x00},
                                          {0xf0, 0x40, 0x00}, {0xcc, 0x1a, 0x00}, {0xb3, 0x17, 0x00}, {0xc2, 0x1a, 0x00}};
    r = std::max(0, std::min(7, r));
    return nvgRGBA(C[r][0], C[r][1], C[r][2], a);
}
// a small "ROW n" pill in the row's colour, top right of a conducted module (hidden while no Composer has it)
struct RowBadge : widget::Widget {
    Member* mb = NULL;
    template <class T> static RowBadge* create(Vec panel, T* module) {
        RowBadge* b = new RowBadge;
        b->mb = module;   // NULL in the module browser
        b->box.size = mm2px(Vec(10.5f, 4.2f));
        b->box.pos = Vec(panel.x - mm2px(7.6f) - b->box.size.x, mm2px(1.4f));
        return b;
    }
    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer != 1 || !mb || !mb->songLinked.load()) return;
        int r = mb->songRow.load();
        if (r < 0) return;
        NVGcolor c = rowColor(r);
        nvgBeginPath(args.vg);
        nvgRoundedRect(args.vg, 0.f, 0.f, box.size.x, box.size.y, box.size.y * 0.5f);
        nvgFillColor(args.vg, c);
        nvgFill(args.vg);
        std::shared_ptr<window::Font> font = APP->window->uiFont;
        if (!font) return;
        char t[8];
        snprintf(t, sizeof t, "ROW %d", r + 1);
        nvgFontFaceId(args.vg, font->handle);
        nvgFontSize(args.vg, box.size.y * 0.72f);
        nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(args.vg, nvgRGB(0x14, 0x0c, 0x04));
        nvgText(args.vg, box.size.x * 0.5f, box.size.y * 0.5f + 0.5f, t, NULL);
    }
};

}  // namespace songlink

SHAREDSLOT_WARM(kSongLinkWarm, songlink::reg())
