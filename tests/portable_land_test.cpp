// Host test for where a Portable mode change lands (src/menu_deck.h deck::landFocus, src/menu_look.h look::takeFocus),
// the same functions the menu calls, in a headless Dear ImGui context. Expected values come from the spec
// (docs/changes 2026-11-15-portable-mode-deck-restyle, AC6): the Deck layout opens on its Overlay page (page 2) with
// Portable mode focused (its third row, index 2); the desktop menu opens with its Portable mode switch focused, the nav
// cursor shown only when a controller has been used; with no change pending the switch takes no focus.
#include "menu_deck.h"
#include "menu_look.h"
#include "imgui_internal.h"
#include <cstdio>

static int g_fail;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

static void deckLanding() {
    deck::Nav nav; nav.page = 0; nav.focus[2] = 0;
    bool focusDesk = false;
    deck::landFocus(true, nav, focusDesk);                      // to the Deck layout
    CHECK(nav.page == 2); CHECK(nav.focus[2] == 2); CHECK(!focusDesk);
    deck::Nav nav2; nav2.page = 1; nav2.focus[1] = 3;
    deck::landFocus(false, nav2, focusDesk);                    // to the desktop menu
    CHECK(focusDesk); CHECK(nav2.page == 1); CHECK(nav2.focus[1] == 3);  // the Deck's own focus left as it was
}

struct Result { ImGuiID nav = 0, sw = 0, above = 0; bool cursor = false, pending = true; };
// The desktop menu's window, drawn as many frames as asked, with Position above the switch; pending is the menu's
// "take the switch's focus" flag at the first frame.
static Result desktop(bool pending, bool padUsed, int frames) {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; io.DeltaTime = 1.0f / 60; io.DisplaySize = ImVec2(1280, 800);
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    unsigned char* px; int w, h;
    io.Fonts->AddFontDefault(); io.Fonts->GetTexDataAsRGBA32(&px, &w, &h); io.Fonts->SetTexID((ImTextureID)1);
    look::ctx().s = 1;
    Result r; r.pending = pending;
    for (int i = 0; i < frames; ++i) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(100, 100)); ImGui::SetNextWindowSize(ImVec2(600, 300));
        ImGui::Begin("menu", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
        ImGui::Button("Position"); r.above = ImGui::GetItemID();
        bool on = false;
        look::portable("##portable", on, true); r.sw = ImGui::GetItemID();
        look::takeFocus(r.pending, padUsed);
        ImGui::End();
        ImGui::Render();
    }
    r.nav = GImGui->NavId; r.cursor = GImGui->NavCursorVisible;
    ImGui::DestroyContext();
    return r;
}

int main() {
    deckLanding();
    const Result a = desktop(true, true, 3);                    // from the Deck layout, a controller in use
    CHECK(!a.pending); CHECK(a.nav == a.sw); CHECK(a.cursor);
    const Result b = desktop(true, false, 3);                   // from the Deck layout by touch or mouse
    CHECK(!b.pending); CHECK(b.nav == b.sw); CHECK(!b.cursor);
    const Result c = desktop(false, true, 3);                   // an ordinary opening: the switch takes no focus
    CHECK(c.nav != c.sw);
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("portable_land_test passed\n");
    return 0;
}
