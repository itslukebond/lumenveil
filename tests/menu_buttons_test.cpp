// Host test for the Insert menu's mouse buttons (src/menu_buttons.h). Built and run by tests/check_menu_buttons.sh.
// Each case plays the game's DirectInput reads and the menu's frames in the order they happen, then counts the
// clicks the menu receives. The menu's input queue is modelled as Dear ImGui's default (trickled) queue: per frame
// it applies queued button states in order and stops at a second change to the same button, which waits for the
// next frame. A click is a change from up to down.
#include "menu_buttons.h"
#include <cstdio>
#include <deque>

static int g_fails;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fails; } } while (0)

struct Menu {
    MouseButtons shared;            // written by the hooks, taken by the menu
    std::deque<bool> queue;         // left button states waiting for the menu
    bool down = false;              // left button as the menu sees it
    int clicks = 0, downFrames = 0;

    void diRead(bool pressed) { shared.read(0, pressed); }
    // One menu frame: take the buttons, queue this frame's states, then let the menu apply them.
    void frame(bool fromDi, bool osDown = false) {
        const MouseButtons b = shared.take();
        const ButtonStates s = buttonEvents(fromDi, b.down[0], b.hit[0], osDown);
        for (int i = 0; i < s.n; ++i) queue.push_back(s.state[i]);
        bool changed = false;
        while (!queue.empty()) {
            const bool next = queue.front();
            if (next != down && changed) break;  // a second change waits for the next frame
            queue.pop_front();
            if (next != down) { changed = true; if (next) ++clicks; down = next; }
        }
        downFrames += down;
    }
};

int main() {
    {   // the whole click arrives in one buffered read between two menu frames
        Menu m;
        m.frame(true);
        m.diRead(true); m.diRead(false);
        m.frame(true); m.frame(true); m.frame(true);
        CHECK(m.clicks == 1, "click inside one read: menu got %d clicks, want 1", m.clicks);
        CHECK(!m.down, "click inside one read: button still down after the click");
    }
    {   // press in one read, release in the next, both before the menu's next frame
        Menu m;
        m.frame(true);
        m.diRead(true);
        m.diRead(false);
        m.frame(true); m.frame(true);
        CHECK(m.clicks == 1, "click across two reads: menu got %d clicks, want 1", m.clicks);
    }
    {   // held over several frames (a drag on a slider), then released
        Menu m;
        m.frame(true);
        m.diRead(true);
        for (int f = 0; f < 5; ++f) { m.frame(true); m.diRead(true); }
        CHECK(m.down, "held over frames: button not down while held");
        CHECK(m.downFrames == 5, "held over frames: down for %d frames, want 5", m.downFrames);
        m.diRead(false);
        m.frame(true); m.frame(true);
        CHECK(m.clicks == 1, "held over frames: menu got %d clicks, want 1", m.clicks);
        CHECK(!m.down, "held over frames: button still down after release");
    }
    {   // no input at all
        Menu m;
        for (int f = 0; f < 5; ++f) { m.diRead(false); m.frame(true); }
        CHECK(m.clicks == 0 && m.downFrames == 0, "idle: %d clicks, %d down frames, want 0 and 0", m.clicks, m.downFrames);
    }
    {   // one click must not come back on later frames
        Menu m;
        m.diRead(true); m.diRead(false);
        for (int f = 0; f < 10; ++f) { m.frame(true); m.diRead(false); }
        CHECK(m.clicks == 1, "no repeat: menu got %d clicks over 10 frames, want 1", m.clicks);
    }
    {   // two separate clicks give two clicks
        Menu m;
        m.diRead(true); m.diRead(false);
        m.frame(true); m.frame(true);
        m.diRead(true); m.diRead(false);
        m.frame(true); m.frame(true);
        CHECK(m.clicks == 2, "two clicks: menu got %d clicks, want 2", m.clicks);
    }
    {   // the OS reports the press and DirectInput does not (no DirectInput reads, or reads without the button)
        Menu m;
        m.frame(false, false); m.frame(false, true); m.frame(false, true); m.frame(false, false);
        CHECK(m.clicks == 1, "OS only, cursor from the OS: menu got %d clicks, want 1", m.clicks);
        Menu d;
        d.diRead(false); d.frame(true, false);
        d.diRead(false); d.frame(true, true);
        d.diRead(false); d.frame(true, false);
        CHECK(d.clicks == 1, "OS only, cursor from DirectInput: menu got %d clicks, want 1", d.clicks);
    }
    {   // both report the same press (Windows): still one click
        Menu m;
        m.frame(true);
        m.diRead(true); m.frame(true, true);
        m.diRead(true); m.frame(true, true);
        m.diRead(false); m.frame(true, false); m.frame(true, false);
        CHECK(m.clicks == 1, "OS and DirectInput together: menu got %d clicks, want 1", m.clicks);
    }
    if (g_fails) { std::printf("%d check(s) failed\n", g_fails); return 1; }
    std::printf("menu buttons: all checks passed\n");
    return 0;
}
