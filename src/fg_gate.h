#pragma once
// Frame generation waits until the game is past its start-up scene and its first loading screen. At launch the game
// renders a short scene through DLSS before the title menu, then the title runs without DLSS. The gate opens once a DLSS
// stretch has ended and the screen has gone 2 s without DLSS (title up), or once one stretch has run 30 s (no start-up
// scene seen). The first load's loading screen can already be drawn through DLSS and run straight into the world, so
// after the title frame generation also waits for 20 s of DLSS frames (a pause of 8 frames or more stops the count, it
// does not restart it). Neither wait comes back; after both, frame generation follows frames as before.
struct FgGate {
    static constexpr int kRunFrames = 8;                  // same hysteresis as FG on/off
    static constexpr unsigned long long kTitleGapMs = 2000, kLongStretchMs = 30000;
    static constexpr unsigned long long kFirstLoadMs = 20000;  // DLSS time after the title before frame generation
    enum Opened { kNo, kTitleGap, kLongStretch };
    bool open = false;
    Opened how = kNo;
    bool inStretch = false, sawStretch = false;
    int valid = 0, invalid = 0;                           // DLSS frames in the current stretch / frames without since
    unsigned long long stretchSince = 0, lastValid = 0;
    bool ready = false;                                   // both waits are over: frame generation may start
    unsigned long long upscaledMs = 0;                    // DLSS time inside stretches since the gate opened by title gap

    // Call on every presented frame; tagged = DLSS ran on it. Returns true once frame generation may start.
    bool frame(bool tagged, unsigned long long now) {
        const bool wasOpen = open, wasInStretch = inStretch;
        const unsigned long long prev = lastValid;
        // A gap that has reached 2 s counts even when the frame that ends it is already a DLSS frame: a load can
        // show nothing across the 2 s mark and then the world.
        if (!open && !inStretch && sawStretch && now - lastValid >= kTitleGapMs) { open = true; how = kTitleGap; }
        if (tagged) {
            invalid = 0;
            lastValid = now;
            if (!inStretch) { inStretch = true; stretchSince = now; valid = 0; }
            ++valid;
        } else if (inStretch && ++invalid >= kRunFrames) {
            inStretch = false;
            sawStretch |= valid >= kRunFrames;
        }
        if (!open) {
            if (!inStretch && sawStretch && now - lastValid >= kTitleGapMs) { open = true; how = kTitleGap; }
            else if (inStretch && now - stretchSince >= kLongStretchMs) { open = true; how = kLongStretch; }
        }
        if (open && !ready) {
            if (how == kLongStretch) ready = true;  // 30 s of DLSS frames have already passed
            else if (tagged && wasOpen && wasInStretch && (upscaledMs += now - prev) >= kFirstLoadMs) ready = true;
        }
        return ready;
    }
};
