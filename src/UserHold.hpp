#pragma once
// USER HOLD (Nov 12 2026, Nick: "let user change knob state after new track loads and don't let the rack change it until
// NEW TRACK is pushed"). Once the user clicks or drags anything on a module's panel (RANDROYD watches the mouse), that
// module is HELD until the next NEW TRACK / NEW RACK: COMPOSER's pattern switches and section mutes (SongLink songPoll),
// MORPH's recalls and RANDROYD's own button presses leave it alone. ROLL / MUTATE still work (the user presses them).
// One table for the whole Rack process (SharedSlot.hpp), lock-free: members read it from their audio thread through a
// generation number, so a scan happens only when the list changes.
#include <atomic>
#include <cstdint>
#include "SharedSlot.hpp"

namespace userhold {

static const int kMax = 128;
struct Table {
    std::atomic<int64_t> ids[kMax];
    std::atomic<uint32_t> gen{1};
    Table() { for (int i = 0; i < kMax; i++) ids[i].store(-1); }
};
inline Table& get() { static Table* t = sharedslot::get<Table>("SYNTHBLOK_USERHOLD_1"); return *t; }

inline uint32_t gen() { return get().gen.load(std::memory_order_acquire); }
inline bool held(int64_t id) {
    if (id < 0) return false;
    Table& t = get();
    for (int i = 0; i < kMax; i++) if (t.ids[i].load(std::memory_order_relaxed) == id) return true;
    return false;
}
// UI thread only (RANDROYD): true when the module was not held before
inline bool hold(int64_t id) {
    if (id < 0 || held(id)) return false;
    Table& t = get();
    for (int i = 0; i < kMax; i++) if (t.ids[i].load() < 0) { t.ids[i].store(id); t.gen.fetch_add(1, std::memory_order_release); return true; }
    return false;   // full: 128 modules held already
}
inline void clear() {
    Table& t = get();
    for (int i = 0; i < kMax; i++) t.ids[i].store(-1);
    t.gen.fetch_add(1, std::memory_order_release);
}

}  // namespace userhold
