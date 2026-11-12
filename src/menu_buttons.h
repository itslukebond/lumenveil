// Mouse buttons for the Insert menu, kept free of Windows types so tests/menu_buttons_test.cpp covers them on the host.
#pragma once

// What the game's DirectInput mouse reads reported while the menu is open. The hooks call read() under the input
// lock; the menu takes a copy once per frame with take().
struct MouseButtons {
    bool down[3] = {};  // the last state each read reported
    bool hit[3] = {};   // pressed in some read since the menu last took the buttons

    void read(int i, bool pressed) { down[i] = pressed; if (pressed) hit[i] = true; }
    // A copy for this frame; the presses it holds are not reported again.
    MouseButtons take() { const MouseButtons b = *this; for (bool& h : hit) h = false; return b; }
};

// The button states to queue for the menu this frame, in order. fromDi: the cursor follows DirectInput this frame.
// A press counts from either DirectInput or the OS: under Wine an exclusive DirectInput mouse can keep the OS button
// state from changing, and one buffered read can carry a whole click. A press that is already over is queued as down
// then up, which the menu's input queue applies on two frames.
struct ButtonStates { int n; bool state[2]; };
inline ButtonStates buttonEvents(bool fromDi, bool diDown, bool hit, bool osDown) {
    const bool down = (fromDi && diDown) || osDown;
    if (hit && !down) return { 2, { true, false } };
    return { 1, { down || hit, false } };
}
