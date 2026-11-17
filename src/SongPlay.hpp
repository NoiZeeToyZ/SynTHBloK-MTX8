#pragma once
// PLAY plays the song (Nick, Nov 16 2026): with a COMPOSER in the patch, the controller's PLAY on any MTX module
// (the JAM's PLAY button, or the PLAY of any GridController.hpp profile) toggles the COMPOSER's RUN, which starts
// the whole song from the top, and every PLAY light shows the COMPOSER's run state. No COMPOSER: PLAY is the
// module's own (RUN; LOOP plays its loop), as before.
// The COMPOSER claims the slot from its process() and lets go when bypassed or deleted; the members only read it
// and leave PLAY presses for the COMPOSER to take in its own process(), so nothing crosses threads unguarded.
// One object per Rack process (SharedSlot.hpp), so the free MTX-8 finds a COMPOSER in the paid plugin.
// GPL-3.0-or-later like the free singles.
#include <atomic>
#include <cstdint>
#include "SharedSlot.hpp"

namespace songplay {

struct Slot {
    std::atomic<int64_t> composer{-1};   // module id of the COMPOSER in the patch (-1: none)
    std::atomic<bool> running{false};    // its RUN
    std::atomic<int> presses{0};         // PLAY presses waiting for it
};
inline Slot& slot() { static Slot* s = sharedslot::get<Slot>("SYNTHBLOK_SONGPLAY_1"); return *s; }

// --- the COMPOSER, every sample: publishes its RUN; true = PLAY was pressed (an odd number of times): toggle RUN
inline bool composerPoll(int64_t id, bool running) {
    Slot& s = slot();
    int64_t none = -1;
    s.composer.compare_exchange_strong(none, id);
    if (s.composer.load() != id) return false;   // another COMPOSER has it (the first one in the patch)
    s.running.store(running);
    return (s.presses.exchange(0) & 1) != 0;
}
inline void composerLeave(int64_t id) {
    Slot& s = slot();
    int64_t me = id;
    if (s.composer.compare_exchange_strong(me, -1)) { s.running.store(false); s.presses.store(0); }
}

// --- the members
inline bool present() { return slot().composer.load() >= 0; }
// the controller's PLAY: true = it went to the COMPOSER (the module does nothing itself)
inline bool pressPlay() {
    if (!present()) return false;
    slot().presses.fetch_add(1);
    return true;
}
// the PLAY light: the COMPOSER's run state when there is one, else the module's own
inline int light(int own) { return present() ? (slot().running.load() ? 127 : 0) : own; }

}  // namespace songplay

SHAREDSLOT_WARM(kSongPlayWarm, songplay::slot())
