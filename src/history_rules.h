#pragma once
// When the upscaler's history is reset. No Windows headers: tested on the host by tests/history_reset_test.cpp.
// A game frame (one game Present to the next) in which the upscaler did not run successfully, such as the map, a menu,
// a loading screen or a video, means the next run starts from fresh history, so none of the earlier scene blends in.
// upscaled(): a successful run this frame. endFrame(): at the game's Present; true when the next run owes a reset.
// gap: frames in a row without a run, for the log.
struct HistoryGate {
    bool ran = false;
    unsigned gap = 0;
    void upscaled() { ran = true; }
    bool endFrame() {
        const bool owed = !ran;
        gap = ran ? 0 : gap + 1;
        ran = false;
        return owed;
    }
};
