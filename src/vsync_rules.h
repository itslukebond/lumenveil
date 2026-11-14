#pragma once
// How our presenter honours the game's VSync setting, and when the stall watchdog logs. No Windows headers: tested on
// the host by tests/vsync_test.cpp.
//
// On Windows our own swap chain presents at interval 0, so a Present never waits for the display while the game's
// window thread waits on the render thread. When the game asks for VSync there, the frame cap stands in for it: never
// uncapped and never above the auto cap (just under the refresh rate). Menu cap values: -1 Auto, 0 Off, above 0 custom.
//
// The interval our swap chain presents at. Windows, on our own or Streamline's swap chain: always 0. Under Wine/Proton
// interval 0 can pick an unsynchronised present mode, and FSR's frame generation swap chain adds the tearing flag at
// interval 0 itself (XeSS's is untested), so those keep the game's interval, as in 1.0.1.
inline unsigned vsyncInterval(unsigned si, bool wine, bool proxyFg) { return wine || proxyFg ? si : 0; }
// The cap stands in for VSync only where our Present dropped it (a cap under real VSync would judder).
inline bool vsyncByCap(unsigned si, bool wine, bool proxyFg) { return si > 0 && vsyncInterval(si, wine, proxyFg) == 0; }
// Our own pacing after Present: never with Streamline (Reflex's limit is in charge); with FSR or XeSS (frame generation
// swap chain, or upscaler 1 FSR / 2 XeSS) as in 1.0.1; and whenever the cap stands in for VSync. One limiter at a time.
inline bool ownPace(bool streamline, bool proxyFg, int upscaler, bool byCap) {
    return !streamline && (proxyFg || upscaler == 1 || upscaler == 2 || byCap);
}

inline int vsyncCap(int limit, int autoCap, bool wantSync) {
    if (!wantSync) return limit < 0 ? autoCap : limit;  // as in 1.0.1
    return limit > 0 && limit < autoCap ? limit : autoCap;
}

// The watchdog's decision, fed the present count every half second: a stall is 3 s with no new present after at
// least one, logged once when it begins and once when presents resume (stalledMs then holds how long it lasted).
enum { kStallNone, kStallBegan, kStallEnded };
struct StallWatch {
    unsigned last = 0;
    unsigned long long since = 0, stalledMs = 0;
    bool stalled = false;
    int step(unsigned count, unsigned long long nowMs, unsigned long long limitMs = 3000) {
        if (count != last) {
            const bool was = stalled;
            stalledMs = nowMs - since;
            last = count; since = nowMs; stalled = false;
            return was ? kStallEnded : kStallNone;
        }
        if (count == 0 || stalled || nowMs - since < limitMs) return kStallNone;
        stalled = true;
        return kStallBegan;
    }
};
