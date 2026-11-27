#pragma once
// MATRIX-SONG's link to the MATRIX modules, with no cables: a module that can be conducted derives from
// songlink::Member and joins in its constructor (leaves in its destructor). MATRIX-SONG finds the members by
// module id and, on a section change, asks each for a pattern, a mute or a loop action; its level strip sets
// the member's level, and the member reports its output peak for the strip's meter. The requests are
// atomics the member takes in its own process() (songPoll), so nothing crosses engine threads unguarded.
#include "plugin.hpp"
#include "OledUi.hpp"   // (BLOX THEMES: MTX parts themed)
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

// SONG CLOCK (Nov 25 2026, Nick: "COMPOSER without RANDROYD ... start and stop all connected modules"): each running
// COMPOSER publishes its clock here (one entry per COMPOSER, by module id); a member it conducts whose CLOCK jack is
// unpatched (and no CLOKWORX lane owns it) steps on the COMPOSER's 16ths, resets with it and follows its RUN and BPM.
// A patched CLOCK (RANDROYD's cables) always wins, so RANDROYD's racks play exactly as before. Members read it only;
// nothing here is written into a member, so plugins built before it never see it. Own key: never shares a layout.
namespace songclock {
static const int kN = 8;
struct Entry {
    std::atomic<int64_t> id{-1};          // the COMPOSER's module id (-1: free)
    std::atomic<uint32_t> beat{0};        // its heartbeat (every sample it runs): stale = gone / bypassed
    std::atomic<uint32_t> ticks{0};       // 16ths played
    std::atomic<uint32_t> resetGen{0};    // bumped on RESET (and on RUN: the song from the top)
    std::atomic<int> tickInBar{-1};       // the last 16th's place in its bar (0 = a downbeat)
    std::atomic<bool> running{false};
    std::atomic<float> bpm{120.f};
};
struct Slot { Entry e[kN]; };
inline Slot& slot() { static Slot* s = sharedslot::get<Slot>("SYNTHBLOK_SONGCLOCK_1"); return *s; }
inline Entry* find(int64_t id) {
    if (id < 0) return NULL;
    Slot& s = slot();
    for (int i = 0; i < kN; i++) if (s.e[i].id.load() == id) return &s.e[i];
    return NULL;
}
inline Entry* claim(int64_t id) {
    if (Entry* e = find(id)) return e;
    Slot& s = slot();
    for (int i = 0; i < kN; i++) {
        int64_t none = -1;
        if (s.e[i].id.compare_exchange_strong(none, id)) { s.e[i].running.store(false); return &s.e[i]; }
    }
    return NULL;   // 8 COMPOSERs already publishing
}
inline void leave(int64_t id) { if (Entry* e = find(id)) { e->running.store(false); e->id.store(-1); } }
}  // namespace songclock

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
        if (!cwOwned()) return scOn ? scEdge : jackEdge;   // free of CLOKWORX: the COMPOSER's 16th while it feeds this
        if (cwTicks.exchange(0) <= 0) return false;
        rollN = std::max(0, std::min(8, cwRollN.exchange(1)));   // 0: a SILENT step (CLOKWORX STOP): it advances, plays nothing
        return true;
    }
    bool cwReset() { return cwOwned() ? cwResetReq.exchange(false) : scOn && scRst; }
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
        songFollow(dt);
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

    // SONG CLOCK (see songclock above; member-side only). The member names its CLOCK jack, RUN switch and BPM knob
    // in its constructor (-1: it has none); songPoll() then runs songFollow() every sample.
    int scClockIn = -1, scRunParam = -1, scBpmParam = -1;
    bool scOn = false, scEdge = false, scRst = false, scWaitBar = false, scRunWas = false;
    uint32_t scTicks = 0, scResets = 0, scBeat = 0;
    int64_t scOwner = -1;
    float scQuiet = 0.f, scPeriod = 0.125f;
    void songFollowSetup(int clockIn, int runParam, int bpmParam = -1) { scClockIn = clockIn; scRunParam = runParam; scBpmParam = bpmParam; }
    // fed by a COMPOSER now: its 16ths replace the member's own clock (as CLOKWORX's do)
    bool songFed() { return cwOwned() || scOn; }
    float songPeriod() { return cwOwned() ? cwPeriod.load() : scPeriod; }
    void scSetRun(bool run) {
        if (self && scRunParam >= 0) self->params[scRunParam].setValue(run ? 1.f : 0.f);
    }
    void songFollow(float dt) {
        scEdge = scRst = false;
        if (!self || scClockIn < 0) { scOn = false; return; }
        int64_t own = songOwner.load();
        songclock::Entry* e = NULL;
        if (own >= 0 && songLinked.load() && !cwOwned() && !self->inputs[scClockIn].isConnected()) e = songclock::find(own);
        if (e) {   // a COMPOSER that stopped running process() (bypassed, removed) lets the member go after 0.1 s
            uint32_t b = e->beat.load();
            if (b != scBeat || own != scOwner) { scBeat = b; scQuiet = 0.f; }
            else if ((scQuiet += dt) > 0.1f) e = NULL;
        }
        if (!e) { scOn = false; return; }
        const bool run = e->running.load();
        const uint32_t rg = e->resetGen.load(), t = e->ticks.load();
        scPeriod = 15.f / std::max(1.f, e->bpm.load());
        if (!scOn || own != scOwner) {   // just joined: take the song's RUN; mid-song, come in on its next downbeat
            scOn = true; scOwner = own;
            scResets = rg; scTicks = t; scRunWas = run; scWaitBar = run;
            scSetRun(run);
        } else {
            if (run != scRunWas) { scRunWas = run; scSetRun(run); if (!run) scWaitBar = false; }
            if (rg != scResets) { scResets = rg; scRst = true; scWaitBar = false; }
            if (t != scTicks) {
                scTicks = t;
                if (!scWaitBar) scEdge = true;
                else if (e->tickInBar.load() == 0) { scWaitBar = false; scRst = scEdge = true; }   // step 1 on the bar
            }
        }
        if (scBpmParam >= 0 && self->paramQuantities[scBpmParam]) {   // the BPM knob shows the song's tempo
            ParamQuantity* q = self->paramQuantities[scBpmParam];
            float b = clamp(e->bpm.load(), q->getMinValue(), q->getMaxValue());
            if (std::fabs(self->params[scBpmParam].getValue() - b) > 0.01f) self->params[scBpmParam].setValue(b);
        }
    }
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
    return oled::tRGBA(C[r][0], C[r][1], C[r][2], a);
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
        nvgFillColor(args.vg, oled::tRGB(0x14, 0x0c, 0x04));
        nvgText(args.vg, box.size.x * 0.5f, box.size.y * 0.5f + 0.5f, t, NULL);
    }
};

}  // namespace songlink

SHAREDSLOT_WARM(kSongLinkWarm, songlink::reg())
SHAREDSLOT_WARM(kSongClockWarm, songlink::songclock::slot())
