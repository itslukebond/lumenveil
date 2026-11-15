// Host test for src/menu_pages.h, the page model both menu layouts draw from: the rows of each page across the state
// matrix, the short values, the help box's lines and flags, the status line and the page keys; then the fit test with
// the bundled TeX Gyre Adventor at both layouts' sizes. Expected values come from the spec
// (docs/changes 2026-11-15-menu-cleanup-layout-button: Expected behaviour, AC6 to AC15 and the note table), not the code.
// Usage: menu_pages_test <path to texgyreadventor-regular.otf>
#include "menu_pages.h"
#include "menu_deck.h"  // the Portable label slot and the desktop controller footer (review findings 5 and 7)
#include "imgui.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <set>
#include <vector>

static int g_fail;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

using namespace pages;

static std::string ids(int page, const State& s, int* subOut = nullptr) {
    Row r[kMaxRows]; int sub = 0;
    const int n = rows(page, s, r, &sub);
    if (subOut) *subOut = sub;
    std::string out;
    for (int i = 0; i < n; ++i) { if (i == sub) out += "| "; out += r[i].label; out += ";"; }
    return out;
}
static bool hasText(const Help& h, const char* t, Tone tone) {
    for (int i = 0; i < h.n; ++i) if (std::strstr(h.line[i].text, t) && h.line[i].tone == tone) return true;
    return false;
}
static State nvidiaPc() {  // an RTX card running DLSS and DLSS frame generation
    State s;
    s.nvidia = true; s.want = 0; s.active = 0; s.upscaler = -1; s.dlssVersion = "310.9.16";
    s.ratio = 67; s.live = 67; s.inW = 2560; s.inH = 1440; s.outW = 3840; s.outH = 2160; s.dispW = 3840; s.dispH = 2160;
    s.backend = 0; s.launchBackend = -1; s.picked = 0; s.running = 0; s.fgBackend = 0; s.fgAvailable = true; s.fgEnable = 1;
    s.dynamicSupported = true; s.maxGenerated = 3; s.frames = 1; s.reflex = 1; s.dlssgVersion = "310.9.16";
    return s;
}
static State amdPc() {  // a Radeon running FSR and FSR frame generation, Anti-Lag 2 ready
    State s;
    s.want = 1; s.active = 1; s.fsrVersion = "3.1.4"; s.ratio = 58; s.live = 58; s.inW = 1114; s.inH = 626; s.outW = 1920; s.outH = 1080;
    s.backend = 0; s.picked = 1; s.running = 1; s.fgBackend = 1; s.fgAvailable = true; s.fgEnable = 1; s.paces = true;
    s.al2Shown = true; s.al2Ready = true; s.sync = "FreeSync";
    return s;
}

// AC9: the same rows for the same state, whichever layout (the model takes no layout); the Portable layout gains the DLSS
// rows wherever the desktop shows them.
static void rowSets() {
    State s = nvidiaPc();
    CHECK(ids(kUpscaling, s) == "Upscaler;Enabled;Scaling mode;DLSS model;Sharpening;Game sharpening;");
    State p = s; p.portable = true;
    for (int pg = 0; pg < 3; ++pg) CHECK(ids(pg, p) == ids(pg, s));
    CHECK(ids(kFrameGen, s) == "Backend;Enabled;Mode;| NVIDIA Reflex;Frame cap;");
    s.frames = 0;  // Dynamic: its target
    CHECK(ids(kFrameGen, s) == "Backend;Enabled;Mode;Target fps;| NVIDIA Reflex;Frame cap;");
    s.limit = 140;  // Custom cap: the worst case, seven rows and the heading
    CHECK(ids(kFrameGen, s) == "Backend;Enabled;Mode;Target fps;| NVIDIA Reflex;Frame cap;Cap;");
    Row r[kMaxRows]; int sub; CHECK(rows(kFrameGen, s, r, &sub) == 7);
    s.limit = -1; CHECK(ids(kFrameGen, s) == "Backend;Enabled;Mode;Target fps;| NVIDIA Reflex;Frame cap;");
    CHECK(ids(kOverlay, s) == "FPS counter;Position;Sharper HUD;");
    // no DLSS model where DLSS is not wanted, or where another upscaler runs
    State a = amdPc();
    CHECK(ids(kUpscaling, a) == "Upscaler;Enabled;Scaling mode;Sharpening;Game sharpening;");
    State b = nvidiaPc(); b.active = 1;  CHECK(ids(kUpscaling, b).find("DLSS model") == std::string::npos);
    b.active = -1; CHECK(ids(kUpscaling, b).find("DLSS model") != std::string::npos);  // not known yet: shown
    // AMD: Anti-Lag 2 when shown, else no latency row
    CHECK(ids(kFrameGen, a) == "Backend;Enabled;Mode;| Anti-Lag 2;Frame cap;");
    a.al2Shown = false; CHECK(ids(kFrameGen, a) == "Backend;Enabled;Mode;| Frame cap;");
    // Intel XeSS frame generation: Xe Low Latency
    State x = amdPc(); x.fgBackend = 2; x.al2Shown = false; CHECK(ids(kFrameGen, x) == "Backend;Enabled;Mode;| Xe Low Latency;Frame cap;");
    // not available: rows stay, dimmed
    State n = nvidiaPc(); n.fgAvailable = false; n.fgBackend = -1;
    rows(kFrameGen, n, r, &sub);
    CHECK(r[1].dim && r[2].dim);
    CHECK(ids(kFrameGen, n) == "Backend;Enabled;Mode;| Frame cap;");
    // the fixed 2x mode is dim with FSR and XeSS
    rows(kFrameGen, amdPc(), r, &sub); CHECK(r[2].dim && !r[1].dim);
    // the counter's position dims with the counter off; the scaling mode without GraphicsConfig.ini
    State o = s; o.counter = false; rows(kOverlay, o, r, &sub); CHECK(r[1].dim);
    o.ratio = 0; rows(kUpscaling, o, r, &sub); CHECK(r[2].dim);
    // every row's page
    CHECK(pageOf(kUpscaler) == 0 && pageOf(kGameSharpening) == 0 && pageOf(kBackend) == 1 && pageOf(kCap) == 1 && pageOf(kCounter) == 2 && pageOf(kHud) == 2);
}

static std::string val(Id id, const State& s) {
    const Value v = value(id, s);
    return *v.rest ? std::string(v.main) + " | " + v.rest : std::string(v.main);
}
// AC10: short values, the resolved part separate (drawn dim after a dot).
static void values() {
    State s = nvidiaPc();
    CHECK(val(kUpscaler, s) == "Auto | DLSS");
    s.upscaler = 2; CHECK(val(kUpscaler, s) == "Intel XeSS");
    CHECK(val(kScaling, s) == "Quality | 67%");
    s.ratio = 100; CHECK(val(kScaling, s) == "Native AA | 100%");
    s.ratio = 58; CHECK(val(kScaling, s) == "Balanced | 58%");
    s.ratio = 50; CHECK(val(kScaling, s) == "Performance | 50%");
    s.ratio = 33; CHECK(val(kScaling, s) == "Ultra Performance | 33%");
    s.upPct = 0; s.ratio = 34; CHECK(val(kScaling, s) == "Ultra Performance | 34%");
    s.ratio = 75; CHECK(val(kScaling, s) == "Custom | 75%");
    s.ratio = 0; CHECK(val(kScaling, s) == "Not found");
    s.preset = 0; CHECK(val(kDlssModel, s) == "Default");
    s.preset = 11; CHECK(val(kDlssModel, s) == "Model K");
    s.preset = 13; CHECK(val(kDlssModel, s) == "Model M");
    s.preset = 12; CHECK(val(kDlssModel, s) == "Model L");
    s.preset = 10; CHECK(val(kDlssModel, s) == "Model J");
    s.preset = 6; CHECK(val(kDlssModel, s) == "Model F");
    s.preset = 5; CHECK(val(kDlssModel, s) == "Model E");
    s.preset = 14; CHECK(val(kDlssModel, s) == "Model N | ini");
    s.reflex = 0; CHECK(val(kLowLatency, s) == "Off");
    s.reflex = 1; CHECK(val(kLowLatency, s) == "On");
    s.reflex = 2; CHECK(val(kLowLatency, s) == "Boost");
    s.limit = -1; CHECK(val(kFrameCap, s) == "Auto | G-Sync");
    s.limit = 0; CHECK(val(kFrameCap, s) == "Off");
    s.limit = 90; CHECK(val(kFrameCap, s) == "Custom"); CHECK(val(kCap, s) == "90 fps");
    CHECK(val(kBackend, s) == "Auto | DLSS");
    s.backend = 2; CHECK(val(kBackend, s) == "AMD FSR");
    s.frames = 0; CHECK(val(kFgMode, s) == "Dynamic");
    s.frames = 3; CHECK(val(kFgMode, s) == "4\xC3\x97");
    State a = amdPc();
    CHECK(val(kFgMode, a) == "2\xC3\x97");  // FSR: the fixed 2x
    CHECK(val(kFrameCap, a) == "Off");
    a.limit = -1; CHECK(val(kFrameCap, a) == "Auto | FreeSync");
    a.antilag = 1; CHECK(val(kLowLatency, a) == "On");
    a.al2Ready = false; CHECK(val(kLowLatency, a) == "Off");  // Off while unavailable
    State x = amdPc(); x.fgBackend = 2; x.al2Shown = false; x.reflex = 2; CHECK(val(kLowLatency, x) == "On");  // the ini's Boost
    CHECK(val(kPosition, a) == "Top left");
    a.corner = 3; CHECK(val(kPosition, a) == "Bottom right");
    CHECK(val(kSharpening, a) == "Off");
    a.sharp = 40; CHECK(val(kSharpening, a) == "40%");
}

// AC12, AC13, AC14: one case per row of the note table, with tone and flag; amber wins over green.
static void helpLines() {
    State s = nvidiaPc();
    Help h = help(kUpscaler, s);
    CHECK(hasText(h, "Auto picks DLSS on NVIDIA", kInk)); CHECK(h.flag == kNoFlag);
    // DLSS on a non-NVIDIA card; upscaler files missing
    State a = amdPc(); a.want = 0;
    h = help(kUpscaler, a); CHECK(hasText(h, "DLSS needs an NVIDIA GeForce RTX card", kAmber)); CHECK(h.flag == kWarnFlag);
    CHECK(!hasText(h, "Auto picks", kInk));  // the warning replaces the explanation
    for (int u = 0; u < 3; ++u) {
        State m = nvidiaPc(); m.want = u; m.wantMissing = true;
        h = help(kUpscaler, m); CHECK(hasText(h, "not found", kAmber)); CHECK(h.flag == kWarnFlag);
    }
    // tested-only notes: upscaler, frame generation backend, Anti-Lag 2
    a = amdPc();
    CHECK(hasText(help(kUpscaler, a), "Untested on Radeon", kDim));
    State xi = amdPc(); xi.want = 2; CHECK(hasText(help(kUpscaler, xi), "Untested on Intel Arc", kDim));
    CHECK(hasText(help(kBackend, a), "Untested on Radeon", kDim));
    CHECK(hasText(help(kLowLatency, a), "Untested on Radeon", kDim));
    // GraphicsConfig.ini not found
    State g = nvidiaPc(); g.ratio = 0;
    h = help(kScaling, g); CHECK(hasText(h, "GraphicsConfig.ini not found", kAmber)); CHECK(h.flag == kWarnFlag);
    // the scaling restart; changes apply the next time
    g = nvidiaPc(); g.ratio = 50;  // live 67
    h = help(kScaling, g); CHECK(hasText(h, "Restart the game to apply the new scaling mode.", kGreen)); CHECK(h.flag == kRestartFlag);
    g.ratio = 68; CHECK(help(kScaling, g).flag == kNoFlag);  // within 1%
    g.live = 0; h = help(kScaling, g); CHECK(hasText(h, "Changes apply the next time the game starts.", kDim)); CHECK(h.flag == kNoFlag);
    // XeSS's lowest scale, both wordings; with the restart too, amber wins
    State x = amdPc(); x.want = 2; x.xessBelow = true; x.ratio = 33; x.live = 58; x.upPct = 34; x.xessLowest = 34;
    h = help(kScaling, x); CHECK(hasText(h, "Pick Ultra Performance again (34% with XeSS)", kAmber));
    CHECK(hasText(h, "Restart the game to apply", kGreen)); CHECK(h.flag == kWarnFlag);
    x.upPct = 0; x.xessLowest = 50; CHECK(hasText(help(kScaling, x), "Pick Performance and restart the game.", kAmber));
    // DLSS model descriptions per value; DLSS older than 310
    s.preset = 11; CHECK(hasText(help(kDlssModel, s), "best quality", kInk));
    s.preset = 0; CHECK(hasText(help(kDlssModel, s), "NVIDIA pick", kInk));
    s.dlssVersion = "3.7.20"; h = help(kDlssModel, s); CHECK(hasText(h, "older than 310", kAmber)); CHECK(h.flag == kWarnFlag);
    // game sharpening
    CHECK(hasText(help(kGameSharpening, s), "100% is the game's look", kInk));
    // FG backend restart; FSR off at launch
    State r = amdPc(); r.backend = 3; r.picked = 2; r.launchBackend = -1;
    h = help(kBackend, r); CHECK(hasText(h, "Restart the game to switch frame generation.", kGreen)); CHECK(h.flag == kRestartFlag);
    State off = amdPc(); off.fgAvailable = false; off.offAtLaunch = true; off.fgEnable = 1; off.fgBackend = -1;
    h = help(kFgEnabled, off); CHECK(hasText(h, "Restart the game to switch frame generation.", kGreen)); CHECK(h.flag == kRestartFlag);
    // frame generation not available: all eight variants, amber ones flagged
    struct NA { int running; bool dlssg, xessFg, fmt, launch, fsrFiles, fsrFg; const char* text; Tone tone; };
    const NA na[] = {
        { 0, true, true, false, true, true, true, "needs an RTX 40 or 50 card", kDim },
        { 0, false, true, false, true, true, true, "nvngx_dlssg.dll not found", kAmber },
        { 2, true, false, false, true, true, true, "libxess_fg.dll", kAmber },
        { 2, true, true, true, true, true, true, "HDR output", kDim },
        { 2, true, true, false, true, true, true, "Intel XeSS frame generation did not start", kDim },
        { 1, true, true, false, true, false, true, "FSR files not found", kAmber },
        { 1, true, true, false, true, true, false, "amd_fidelityfx_framegeneration_dx12.dll not found", kAmber },
        { 1, true, true, false, true, true, true, "AMD FSR frame generation did not start", kDim },
    };
    for (const NA& v : na) {
        State t = amdPc(); t.fgAvailable = false; t.fgBackend = -1; t.running = v.running; t.picked = v.running; t.dlssg = v.dlssg;
        t.xessFgFiles = v.xessFg; t.formatRefused = v.fmt; t.launchEnable = v.launch; t.fsrFiles = v.fsrFiles; t.fsrFgDll = v.fsrFg;
        h = help(kBackend, t);
        CHECK(hasText(h, v.text, v.tone));
        CHECK(h.flag == (v.tone == kAmber ? kWarnFlag : kNoFlag));
        CHECK(std::string(status(t).fg) == "not available");
    }
    // the backends' versions (with Xe Low Latency)
    CHECK(hasText(help(kBackend, nvidiaPc()), "NVIDIA DLSS frame generation 310.9.16.", kDim));
    State fv = amdPc(); fv.fsrFgVersion = "3.1.5"; CHECK(hasText(help(kBackend, fv), "AMD FSR frame generation 3.1.5.", kDim));
    fv.fgBackend = 2; fv.xessFgVersion = "2.0.2"; CHECK(hasText(help(kBackend, fv), "XeSS frame generation 2.0.2, with Xe Low Latency", kDim));
    // generating; idle with its reason
    State gen = nvidiaPc(); gen.generating = true; gen.shown = 120; gen.rendered = 60;
    CHECK(hasText(help(kFgEnabled, gen), "Generating 2.0", kDim));
    CHECK(hasText(help(kFgEnabled, nvidiaPc()), "Idle: turns on in gameplay", kDim));
    // Reflex, Xe Low Latency and Anti-Lag 2 states
    State rf = nvidiaPc(); CHECK(hasText(help(kLowLatency, rf), "Frame generation needs it", kInk));
    rf.reflex = 2; CHECK(hasText(help(kLowLatency, rf), "Boost keeps GPU clocks up", kDim));
    State xl = amdPc(); xl.fgBackend = 2; xl.al2Shown = false; CHECK(hasText(help(kLowLatency, xl), "XeSS frame generation is enabled", kInk));
    State al = amdPc();
    al.al2Ready = false; al.al2Linux = true; CHECK(hasText(help(kLowLatency, al), "Not available on Linux", kDim));
    al.al2Linux = false; h = help(kLowLatency, al); CHECK(hasText(h, "RX 5000", kAmber)); CHECK(h.flag == kWarnFlag);
    al.al2Ready = true; al.antilag = 0; CHECK(hasText(help(kLowLatency, al), "Off: the game waits for the GPU", kInk));
    al.paces = false; CHECK(hasText(help(kLowLatency, al), "Off: frames can queue ahead", kInk));
    al.antilag = 1; CHECK(hasText(help(kLowLatency, al), "On: AMD's driver holds back", kInk));
    al.paces = true; CHECK(hasText(help(kLowLatency, al), "On: AMD's driver also holds back", kInk));
    // the latency text for FSR and XeSS with no latency row; frame cap Auto
    State lt = amdPc(); lt.al2Shown = false; CHECK(hasText(help(kFrameCap, lt), "FSR has no Reflex", kDim));
    lt.active = 2; lt.fgBackend = -1; lt.fgAvailable = false; CHECK(hasText(help(kFrameCap, lt), "no frames queue ahead.", kDim));
    lt.limit = -1; lt.autoCap = 141; CHECK(hasText(help(kFrameCap, lt), "Caps output at 141 fps", kInk));
    // Sharper HUD, FPS counter and Position; the layout button
    CHECK(hasText(help(kHud, s), "Crisper HUD text", kInk));
    CHECK(hasText(help(kCounter, s), "Frame rate, frame time", kInk));
    CHECK(hasText(help(kPosition, s), "FPS counter sits", kInk));
    h = help(kLayout, s); CHECK(hasText(h, "bigger text, pages and touch controls", kInk)); CHECK(hasText(h, "Changes apply and save immediately.", kDim));
    State pt = s; pt.portable = true; CHECK(hasText(help(kLayout, pt), "Touch works too.", kDim));
    // every row has an explanation or warning, never more than three paragraphs
    for (int id = 0; id < kIdCount; ++id) { h = help(Id(id), al); CHECK(h.n >= 1 && h.n <= kHelpLines); }
}

// AC15: the status line's pieces.
static void statusLine() {
    Status st = status(nvidiaPc());
    CHECK(std::string(st.up) == "DLSS 310.9.16"); CHECK(st.upVendor == 0);
    CHECK(std::string(st.size) == "2560\xC3\x97" "1440 \xE2\x86\x92 3840\xC3\x97" "2160");
    CHECK(std::string(st.fg) == "waiting for gameplay");
    State g = amdPc(); g.generating = true; g.shown = 150; g.rendered = 75;
    st = status(g); CHECK(std::string(st.up) == "FSR 3.1.4"); CHECK(st.upVendor == 1);
    CHECK(std::string(st.fg) == "generating 2.0\xC3\x97"); CHECK(st.fgTone == kVendor); CHECK(st.fgVendor == 1);
    State n = nvidiaPc(); n.inW = n.outW = 0; n.ratio = 50;
    st = status(n); CHECK(std::string(st.size) == "1920\xC3\x97" "1080 from next start");
    n.fgEnable = 0; CHECK(std::string(status(n).fg) == "off");
    n.upOn = false; CHECK(std::string(status(n).up) == "Upscaling off");
}

// AC6: pages wrap both ways.
static void pageKeys() {
    CHECK(turn(2, 1) == 0); CHECK(turn(0, -1) == 2); CHECK(turn(1, 1) == 2);
}

// AC10, AC14, AC15: everything fits, measured with the bundled font at each layout's sizes.
static std::vector<State> states() {
    std::vector<State> v;
    for (int vendor = 0; vendor < 3; ++vendor)
        for (int fg = -1; fg < 3; ++fg)
            for (int cap = -1; cap <= 1; ++cap)
                for (int al = 0; al < 4; ++al)
                    for (int warn = 0; warn < 2; ++warn) {
                        State s = vendor == 0 ? nvidiaPc() : amdPc();
                        s.nvidia = vendor == 0;
                        if (vendor == 2) { s.want = 2; s.active = 2; s.xessVersion = "3.0.2.123"; s.xessBelow = warn; s.ratio = 33; s.upPct = 0; }
                        s.fgBackend = fg; s.fgAvailable = fg >= 0; s.running = fg < 0 ? vendor : fg; s.launchEnable = true;
                        s.limit = cap < 0 ? -1 : cap == 0 ? 0 : 480; s.autoCap = 238;
                        s.al2Shown = al > 0; s.al2Ready = al == 3; s.al2Linux = al == 1; s.antilag = 1; s.paces = vendor != 0;
                        s.wantMissing = warn; s.dlssVersion = warn ? "3.7.20" : "310.10.100"; s.preset = warn ? 14 : 13;
                        s.dlssg = !warn; s.xessFgFiles = !warn; s.fsrFgDll = !warn; s.formatRefused = warn && vendor == 2;
                        s.backend = warn ? 2 : 0; s.picked = warn ? 1 : s.running; s.launchBackend = -1;
                        s.generating = fg >= 0 && warn; s.shown = 238; s.rendered = 79; s.reflex = 2; s.frames = 0;
                        s.inW = s.outW = s.dispW = 3840; s.inH = s.outH = s.dispH = 2160; s.live = warn ? 50 : 100;
                        s.sync = vendor == 2 ? "Adaptive Sync" : "FreeSync";
                        s.portable = warn;
                        v.push_back(s);
                    }
    return v;
}
static int lines(ImFont* f, float px, float w, const char* t) {
    const ImVec2 sz = f->CalcTextSizeA(px, FLT_MAX, w, t);
    return int(sz.y / px + 0.5f);
}
static void fit(const char* fontPath) {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    static const ImWchar ranges[] = { 0x20, 0xFF, 0x2190, 0x2193, 0 };
    ImFont* f = io.Fonts->AddFontFromFileTTF(fontPath, 19, nullptr, ranges);
    CHECK(f != nullptr);
    if (!f) { ImGui::DestroyContext(); return; }
    unsigned char* px; int w, h;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    int worstHelp = 0, cut = 0;
    std::set<std::string> said;  // each overflow reported once
    auto say = [&](const std::string& m) { ++cut; if (said.insert(m).second) std::printf("%s\n", m.c_str()); };
    char m[1200];
    float widestValue = 0, widestStatus = 0;
    for (const Fit& z : { kDeskFit, kDeckFit }) {
        for (const State& s : states()) {
            for (int id = 0; id < kIdCount; ++id) {
                const Help hp = help(Id(id), s);
                int n = 0;
                for (int i = 0; i < hp.n; ++i) n += lines(f, z.helpPx, z.helpW, hp.line[i].text);
                worstHelp = std::max(worstHelp, n);
                if (n > kHelpLines) { std::snprintf(m, sizeof m, "help of row %d needs %d lines at %.0f: %s / %s / %s", id, n, z.helpW, hp.line[0].text, hp.line[1].text, hp.line[2].text); say(m); }
                const Value v = value(Id(id), s);
                char t[96]; std::snprintf(t, sizeof t, *v.rest ? "%s \xC2\xB7 %s" : "%s", v.main, v.rest);
                const float vw = f->CalcTextSizeA(z.valuePx, FLT_MAX, 0, t).x;
                widestValue = std::max(widestValue, vw / z.valueW);
                if (vw > z.valueW) { std::snprintf(m, sizeof m, "value %s is %.0f wide in %.0f", t, vw, z.valueW); say(m); }
            }
            const Status st = status(s);
            char t[200]; std::snprintf(t, sizeof t, "%s   %s   Frame generation %s", st.up, st.size, st.fg);
            const float sw = f->CalcTextSizeA(z.statusPx, FLT_MAX, 0, t).x;
            widestStatus = std::max(widestStatus, sw / z.statusW);
            if (sw > z.statusW) { std::snprintf(m, sizeof m, "status %s is %.0f wide in %.0f", t, sw, z.statusW); say(m); }
        }
        // the worst status the spec names
        const char* worst = "DLSS 310.10.100   3840\xC3\x97" "2160 \xE2\x86\x92 3840\xC3\x97" "2160   Frame generation waiting for gameplay";
        CHECK(f->CalcTextSizeA(z.statusPx, FLT_MAX, 0, worst).x <= z.statusW);
    }
    CHECK(cut == 0);
    std::printf("fit: help at most %d lines, widest value %.0f%% of its slot, widest status %.0f%% of the line\n",
                worstHelp, widestValue * 100, widestStatus * 100);
    ImGui::DestroyContext();
}

// A DLSS-G card without Dynamic shows the 2x it runs and no target; Reflex's help says it stays on only while frame
// generation is enabled; every row's label fits the Portable label slot (26 px, 48 px into the row, 4 px before the
// controls); the desktop controller footer fits its clip at scale 1 (the 540 px content less the 110 px the version
// keeps), with the menu's 16 px font.
static void reviewFixes(const char* fontPath) {
    State s = nvidiaPc(); s.frames = 0; s.dynamicSupported = false;
    CHECK(!std::strcmp(value(kFgMode, s).main, "2\xC3\x97"));
    CHECK(ids(kFrameGen, s).find("Target fps") == std::string::npos);
    s.dynamicSupported = true;
    CHECK(!std::strcmp(value(kFgMode, s).main, "Dynamic"));
    CHECK(ids(kFrameGen, s).find("Target fps") != std::string::npos);
    auto says = [](const Help& h, const char* t) { for (int i = 0; i < h.n; ++i) if (std::strstr(h.line[i].text, t)) return true; return false; };
    State r = nvidiaPc(); r.fgEnable = 0; r.reflex = 0;
    CHECK(!says(help(kLowLatency, r), "stays on"));
    r.fgEnable = 1;
    CHECK(says(help(kLowLatency, r), "stays on"));
    State x = nvidiaPc(); x.fgBackend = 2; x.fgEnable = 0; x.reflex = 0;
    CHECK(!says(help(kLowLatency, x), "needs it"));

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; io.DisplaySize = ImVec2(1280, 800); io.DeltaTime = 1.0f / 60;
    static const ImWchar ranges[] = { 0x20, 0xFF, 0x2190, 0x2193, 0 };
    ImFont* f = io.Fonts->AddFontFromFileTTF(fontPath, 16, nullptr, ranges);
    CHECK(f != nullptr);
    if (!f) { ImGui::DestroyContext(); return; }
    unsigned char* px; int w, h;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    io.Fonts->SetTexID((ImTextureID)1);
    const float slot = deck::kCtlL - deck::kRowL - 52;
    std::set<std::string> said;
    for (const State& st : states())
        for (int p = 0; p < kPageCount; ++p) {
            Row rr[kMaxRows]; int sub = 0;
            const int n = rows(p, st, rr, &sub);
            for (int i = 0; i < n; ++i) {
                const float lw = f->CalcTextSizeA(26, FLT_MAX, 0, rr[i].label).x * 0.95f;  // drawn at most 5% smaller, never shortened
                if (lw > slot && said.insert(rr[i].label).second) { std::printf("Portable label %s is %.1f wide in %.0f\n", rr[i].label, lw, slot); CHECK(lw <= slot); }
            }
        }
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(ImVec2(1000, 100));
    ImGui::Begin("hints", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int v0 = dl->VtxBuffer.Size;
    pad::drawHints(false, false, 1);
    float right = -1e9f;
    for (int i = v0; i < dl->VtxBuffer.Size; ++i) if (dl->VtxBuffer[i].pos.y >= at.y) right = std::max(right, dl->VtxBuffer[i].pos.x);
    std::printf("controller footer: %.1f px of 430\n", right - at.x);
    CHECK(right - at.x <= 540 - 110);
    ImGui::End();
    ImGui::Render();
    ImGui::DestroyContext();
}

int main(int argc, char** argv) {
    rowSets();
    values();
    helpLines();
    statusLine();
    pageKeys();
    if (argc > 1) reviewFixes(argv[1]);
    if (argc > 1) fit(argv[1]);
    else { std::printf("FAIL: no font path given for the fit test\n"); ++g_fail; }
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("menu_pages_test passed\n");
    return 0;
}
