// The Insert menu's three pages, shared by both layouts (the desktop menu and Portable): which rows each page shows for
// the state the menu reads, their short values, the help box's lines and each row's flag, and the status line. Pure: no
// ImGui, no Windows types, so tests/menu_pages_test.cpp covers every row and note on the host. drawMenu and
// drawDeckMenu fill a State from the same globals and draw what this decides, so the two layouts cannot drift apart.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "fg_support.h"  // support results and kScrgbAdvice, the same advice as the log

namespace pages {

enum Page { kUpscaling, kFrameGen, kOverlay, kPageCount };
// Every row, and the layout button (whose help the box shows while it is selected).
enum Id {
    kUpscaler, kUpEnabled, kScaling, kDlssModel, kSharpening, kGameSharpening,
    kBackend, kFgEnabled, kFgMode, kTarget, kLowLatency, kFrameCap, kCap,
    kCounter, kPosition, kHud,
    kLayout, kIdCount
};
enum Kind { kList, kToggle, kSlider };
enum Tone { kInk, kDim, kAmber, kGreen, kVendor };  // kVendor: the colour of `vendor` (0 NVIDIA, 1 AMD, 2 Intel)
enum Flag { kNoFlag, kRestartFlag, kWarnFlag };
enum Latency { kNoLatency, kReflex, kXell, kAntiLag };
constexpr int kMaxRows = 7;       // the Frame generation page's worst case
constexpr int kHelpLines = 3;     // the help box's height in lines
// What must fit, in each layout's design pixels (the desktop's at scale 1, Portable's at its 1280x800 design): the help
// box's width and text size, a list value's slot (between ‹ and ›) and size, and the status line's width and size.
struct Fit { float helpW, helpPx, valueW, valuePx, statusW, statusPx; };
constexpr Fit kDeskFit{ 518, 13, 264, 16, 518, 12 }, kDeckFit{ 662, 19, 300, 24, 662, 15 };

// What the menu shows, read once a frame by the menu from the mod's state. Upscaler and backend ids: 0 NVIDIA DLSS,
// 1 AMD FSR, 2 Intel XeSS.
struct State {
    bool nvidia = false;            // the adapter is NVIDIA's
    bool portable = false;          // the Portable layout is shown
    // upscaling
    int upscaler = -1;              // the setting: -1 Auto, else an id
    int want = 0;                   // the upscaler wanted: the setting, else the one running, else Auto's pick here
    int active = -1;                // the one running, -1 not known yet
    bool upOn = true;
    bool wantMissing = false;       // the wanted upscaler's files are not in place
    const char *dlssVersion = "", *fsrVersion = "", *xessVersion = "";
    int ratio = 0;                  // the game's scaling mode in %, 0: GraphicsConfig.ini not found
    int live = 0;                   // the scale running in % (0: not upscaling yet)
    int upPct = 33;                 // Ultra Performance's % for the wanted upscaler, 0 when it is not offered
    int xessLowest = 50;            // XeSS's lowest scale in %
    bool xessBelow = false;         // the wanted upscaler is XeSS and the scale is below its range
    unsigned inW = 0, inH = 0, outW = 0, outH = 0;  // the sizes running (0 when not upscaling)
    unsigned dispW = 0, dispH = 0;  // the output size, known before upscaling runs
    int preset = 0;                 // DLSS's model: 0 NVIDIA's default, 11 K, 13 M, 12 L, 10 J, 6 F, 5 E, else set by hand
    int sharp = 0, gameSharp = 100;
    // frame generation
    int backend = 0;                // the menu's choice: 0 Auto, else an id + 1
    int launchBackend = -1;         // the setting at launch
    int picked = 1, running = 1;    // the backend the choice resolves to, and the one running (or that launch resolved to)
    int fgBackend = -1;             // the backend that started, -1 none
    bool fgAvailable = false, generating = false, formatRefused = false, launchEnable = false, offAtLaunch = false;
    bool dlssg = true, fsrFiles = true, fsrFgDll = true, xessFgFiles = true;  // the backends' files are in place
    bool dynamicSupported = false;
    unsigned maxGenerated = 1;
    int fgSupport = 0;              // Streamline's DLSS frame generation support result, 0 supported (fg_support.h)
    bool wine = false;              // running under Wine or Proton
    int noFrames = 0;               // the last whole window added no frames: 1 no cause named, 2 with scRGB output (fg_support.h)
    int fgEnable = 0, frames = 1, target = 120, limit = 0, reflex = 1, antilag = 0;
    const char *dlssgVersion = "", *fsrFgVersion = "", *xessFgVersion = "";
    float shown = 0, rendered = 0;  // fps shown and rendered (generating)
    bool al2Shown = false, al2Ready = false, al2Linux = false;  // Anti-Lag 2's row, ready, or Linux (AMD's Windows driver)
    bool paces = false;             // the presenter paces frames itself (FSR or XeSS in use)
    bool capUsable = true;          // the frame cap applies (Reflex, the presenter's pacing or the game's VSync)
    int autoCap = 0;
    const char* sync = "G-Sync";
    // overlay
    bool counter = true, hud = false;
    int corner = 0;
};

inline const char* shortName(int id) { return id == 0 ? "DLSS" : id == 1 ? "FSR" : id == 2 ? "XeSS" : ""; }
inline const char* upName(int id) { return id == 0 ? "NVIDIA DLSS" : id == 1 ? "AMD FSR" : id == 2 ? "Intel XeSS" : ""; }

// ---- rows ------------------------------------------------------------------------------------------------------------
struct Row { Id id; const char* label; Kind kind; bool dim; };
inline bool showsModel(const State& s) { return s.want == 0 && (s.active == 0 || s.active < 0 || s.active > 2); }
inline Latency latency(const State& s) {
    return s.fgBackend == 0 ? kReflex : s.fgBackend == 2 ? kXell : s.al2Shown ? kAntiLag : kNoLatency;
}
inline int capMode(const State& s) { return s.limit < 0 ? 1 : s.limit == 0 ? 0 : 2; }  // 0 Off, 1 Auto, 2 Custom
inline bool fgDim(const State& s) { return !s.fgAvailable && !s.offAtLaunch; }

// The rows of a page, in order; *sub is the index the "Latency" heading sits before (-1 none). Returns the count.
inline int rows(int page, const State& s, Row out[kMaxRows], int* sub) {
    int n = 0;
    *sub = -1;
    auto add = [&](Id id, const char* label, Kind k, bool dim) { out[n++] = Row{ id, label, k, dim }; };
    if (page == kUpscaling) {
        add(kUpscaler, "Upscaler", kList, false);
        add(kUpEnabled, "Enabled", kToggle, false);
        add(kScaling, "Scaling mode", kList, s.ratio == 0);
        if (showsModel(s)) add(kDlssModel, "DLSS model", kList, false);
        add(kSharpening, "Sharpening", kSlider, false);
        add(kGameSharpening, "Game sharpening", kSlider, false);
    } else if (page == kFrameGen) {
        add(kBackend, "Backend", kList, false);
        add(kFgEnabled, "Enabled", kToggle, fgDim(s));
        add(kFgMode, "Mode", kList, fgDim(s) || s.fgBackend != 0);  // FSR and XeSS generate 2x: nothing to pick
        if (s.fgBackend == 0 && s.frames == 0 && s.dynamicSupported) add(kTarget, "Target fps", kSlider, fgDim(s));
        *sub = n;
        const Latency l = latency(s);
        if (l == kReflex) add(kLowLatency, "NVIDIA Reflex", kList, fgDim(s));
        else if (l == kXell) add(kLowLatency, "Xe Low Latency", kList, fgDim(s));
        else if (l == kAntiLag) add(kLowLatency, "Anti-Lag 2", kList, !s.al2Ready);
        add(kFrameCap, "Frame cap", kList, !s.capUsable);
        if (capMode(s) == 2) add(kCap, "Cap", kSlider, !s.capUsable);
    } else {
        add(kCounter, "FPS counter", kToggle, false);
        add(kPosition, "Position", kList, !s.counter);
        add(kHud, "Sharper HUD", kToggle, false);
    }
    return n;
}
// The page a row lives on.
inline int pageOf(Id id) { return id <= kGameSharpening ? kUpscaling : id <= kCap ? kFrameGen : kOverlay; }

// ---- values ------------------------------------------------------------------------------------------------------------
// A list's choices, short: what the desktop's list offers and the ‹ value › shows. Upscaler and backend start with Auto.
struct Scale { const char* name; int pct; };
inline int scaleModes(const State& s, Scale out[5]) {
    out[0] = { "Native AA", 100 }; out[1] = { "Quality", 67 }; out[2] = { "Balanced", 58 }; out[3] = { "Performance", 50 };
    out[4] = { "Ultra Performance", s.upPct ? s.upPct : 33 };
    return s.upPct ? 5 : 4;
}
inline const char* scaleName(const State& s, int pct) {
    Scale m[5];
    scaleModes(s, m);
    for (const Scale& x : m) if (x.pct == pct) return x.name;  // includes Ultra Performance's 33 when not offered
    return pct == 33 || pct == 34 ? "Ultra Performance" : "Custom";  // the other upscaler's Ultra Performance
}
constexpr int kPresets[] = { 0, 11, 13, 12, 10, 6, 5 };  // Default, K, M, L, J, F, E
inline const char* const kPresetNames[] = { "Default", "Model K", "Model M", "Model L", "Model J", "Model F", "Model E" };
inline int presetIndex(int preset) {
    for (int i = 0; i < 7; ++i) if (kPresets[i] == preset) return i;
    return -1;
}
inline const char* const kReflexNames[] = { "Off", "On", "Boost" };
inline const char* const kOnOff[] = { "Off", "On" };
inline const char* const kCorners[] = { "Top left", "Top right", "Bottom left", "Bottom right" };

// A row's value as shown: the main part in ink, then (when not empty) " · " and the resolved part dim.
struct Value { char main[48] = ""; char rest[32] = ""; };
inline Value value(Id id, const State& s) {
    Value v;
    auto put = [](char* b, size_t n, const char* t) { std::snprintf(b, n, "%s", t); };
    switch (id) {
    case kUpscaler:
        if (s.upscaler >= 0 && s.upscaler <= 2) put(v.main, sizeof v.main, upName(s.upscaler));
        else { put(v.main, sizeof v.main, "Auto"); put(v.rest, sizeof v.rest, shortName(s.want)); }
        break;
    case kScaling:
        if (!s.ratio) put(v.main, sizeof v.main, "Not found");
        else { put(v.main, sizeof v.main, scaleName(s, s.ratio)); std::snprintf(v.rest, sizeof v.rest, "%d%%", s.ratio); }
        break;
    case kDlssModel: {
        const int i = presetIndex(s.preset);
        if (i >= 0) put(v.main, sizeof v.main, kPresetNames[i]);
        else { std::snprintf(v.main, sizeof v.main, "Model %c", char('A' + s.preset - 1)); put(v.rest, sizeof v.rest, "ini"); }
        break;
    }
    case kBackend:
        if (s.backend >= 1 && s.backend <= 3) put(v.main, sizeof v.main, upName(s.backend - 1));
        else { put(v.main, sizeof v.main, "Auto"); put(v.rest, sizeof v.rest, shortName(s.picked)); }
        break;
    case kFgMode:
        if (s.fgBackend == 0 && s.frames == 0 && s.dynamicSupported) put(v.main, sizeof v.main, "Dynamic");
        else std::snprintf(v.main, sizeof v.main, "%d\xC3\x97", s.fgBackend == 0 ? std::max(s.frames, 1) + 1 : 2);  // no Dynamic: 2x
        break;
    case kLowLatency: {
        const Latency l = latency(s);
        if (l == kReflex) put(v.main, sizeof v.main, kReflexNames[s.reflex >= 0 && s.reflex <= 2 ? s.reflex : 1]);
        else if (l == kXell) put(v.main, sizeof v.main, kOnOff[s.reflex > 0]);  // the ini's Boost (2) shows as On
        else put(v.main, sizeof v.main, kOnOff[s.al2Ready && s.antilag]);    // Off while unavailable
        break;
    }
    case kFrameCap:
        if (capMode(s) == 1) { put(v.main, sizeof v.main, "Auto"); put(v.rest, sizeof v.rest, s.sync); }
        else put(v.main, sizeof v.main, capMode(s) ? "Custom" : "Off");
        break;
    case kPosition: put(v.main, sizeof v.main, kCorners[s.corner >= 0 && s.corner < 4 ? s.corner : 0]); break;
    case kUpEnabled: put(v.main, sizeof v.main, kOnOff[s.upOn]); break;
    case kFgEnabled: put(v.main, sizeof v.main, kOnOff[s.fgEnable != 0]); break;
    case kCounter: put(v.main, sizeof v.main, kOnOff[s.counter]); break;
    case kHud: put(v.main, sizeof v.main, kOnOff[s.hud]); break;
    case kSharpening: if (s.sharp) std::snprintf(v.main, sizeof v.main, "%d%%", s.sharp); else put(v.main, sizeof v.main, "Off"); break;
    case kGameSharpening: if (s.gameSharp) std::snprintf(v.main, sizeof v.main, "%d%%", s.gameSharp); else put(v.main, sizeof v.main, "Off"); break;
    case kTarget: std::snprintf(v.main, sizeof v.main, "%d fps", s.target); break;
    case kCap: std::snprintf(v.main, sizeof v.main, "%d fps", s.limit); break;
    default: break;
    }
    return v;
}

// ---- help --------------------------------------------------------------------------------------------------------------
// The help box: at most three paragraphs (each wraps), in order: the amber warning or the explanation, the green restart
// line, a dim state line. The row's flag: amber when it has a warning, else green when its change needs a restart.
struct Line { char text[256] = ""; Tone tone = kInk; };
struct Help { Line line[kHelpLines]; int n = 0; Flag flag = kNoFlag; };

inline bool restartScaling(const State& s) { return s.ratio && s.live && std::abs(s.live - s.ratio) > 1; }
// What the Frame generation row says when Streamline reports that DLSS frame generation cannot run on this PC, and the
// lines it takes in the wider-text layout; nullptr for a result with no words here (the log gives its number).
inline const char* supportAdvice(int r, int* lines) {
    *lines = 2;
    switch (r) {
    case fg_support::kOSDisabledHWS: *lines = 3; return "Windows has Hardware-accelerated GPU scheduling off, and DLSS frame generation needs it. Turn it on in Settings, System, Display, Graphics, then restart the PC.";
    case fg_support::kOSOutOfDate: return "Windows is too old for DLSS frame generation. Update Windows, then restart the game.";
    case fg_support::kDriverOutOfDate: return "The NVIDIA driver is too old for DLSS frame generation. Update the driver, then restart the game.";
    case fg_support::kNoSupportedAdapter:
    case fg_support::kAdapterNotSupported: return "This graphics card can't run DLSS frame generation. Pick FSR above instead.";
    default: return nullptr;
    }
}
inline bool restartBackend(const State& s) { return s.backend - 1 != s.launchBackend && s.picked != s.running; }
inline bool restartFg(const State& s) { return s.offAtLaunch && s.fgEnable; }

inline const char* const kTestedFsr = "Tested on an RTX 5090 (FSR 3.1). Untested on Radeon, FSR 4.";
inline const char* const kTestedXess = "Tested on an NVIDIA GeForce RTX 5090 only. Untested on Intel Arc.";
inline const char* const kMissing[3] = {
    "nvngx_dlss.dll not found. Copy it from NVIDIA's DLSS SDK into the ffxv_dlss folder and restart. INSTALL.txt has the link.",
    "AMD's FSR files not found. Put FidelityFX-SDK-2.4.0.zip in Downloads and run the installer again.",
    "Intel's XeSS files not found. Run the installer again with the XeSS SDK 3.0.2 zip in Downloads. INSTALL.txt has the link.",
};

// Each paragraph says how many lines it may take (1 or 2); one that would take the box past three lines is left out, so
// the order is the priority: the warning or explanation, the restart line, then the state line.
inline Help help(Id id, const State& s) {
    Help h;
    int used = 0;
    auto add = [&](Tone t, int cost, const char* fmt, auto... a) {
        if (h.n >= kHelpLines || used + cost > kHelpLines) return false;
        std::snprintf(h.line[h.n].text, sizeof h.line[h.n].text, fmt, a...);
        h.line[h.n++].tone = t;
        used += cost;
        return true;
    };
    auto warn = [&](int cost, const char* fmt, auto... a) { add(kAmber, cost, fmt, a...); h.flag = kWarnFlag; };
    auto restart = [&](const char* t) { add(kGreen, 1, "%s", t); if (h.flag == kNoFlag) h.flag = kRestartFlag; };
    switch (id) {
    case kUpscaler:
        if (s.want == 0 && !s.nvidia) warn(2, "%s", "DLSS needs an NVIDIA GeForce RTX card. The game uses its own anti-aliasing.");
        else if (s.wantMissing && s.want >= 0 && s.want <= 2) warn(2, "%s", kMissing[s.want]);
        else add(kInk, 1, "%s", "Auto picks DLSS on NVIDIA, XeSS on Intel and FSR on AMD cards.");
        if (s.want == 1) add(kDim, 1, "%s", kTestedFsr);
        else if (s.want == 2) add(kDim, 1, "%s", kTestedXess);
        break;
    case kUpEnabled: add(kInk, 1, "%s", "Off uses the game's own anti-aliasing."); break;
    case kScaling:
        if (!s.ratio) warn(2, "%s", "GraphicsConfig.ini not found, so the game's scaling mode cannot be changed here.");
        else if (s.xessBelow) {
            if (s.upPct) warn(2, "Intel XeSS needs %d%% or more. Pick Ultra Performance again (%d%% with XeSS) and restart the game.", s.xessLowest, s.xessLowest);
            else warn(2, "Intel XeSS needs %d%% or more. Pick Performance and restart the game.", s.xessLowest);
        } else add(kInk, 1, "%s", "The share of the output size the game renders. Lower is faster.");
        if (restartScaling(s)) restart("Restart the game to apply the new scaling mode.");
        else if (s.ratio && !s.live) add(kDim, 1, "%s", "Changes apply the next time the game starts.");
        break;
    case kDlssModel: {
        static const char* const about[] = {
            "Default lets NVIDIA pick the best model for each scaling mode.", "Model K: transformer, the best quality.",
            "Model M: transformer, made for Performance.", "Model L: transformer, made for Ultra Performance.",
            "Model J: transformer, like K with less ghosting, more flicker.", "Model F: CNN, deprecated.", "Model E: CNN, deprecated." };
        const int i = presetIndex(s.preset);
        if (*s.dlssVersion && std::atoi(s.dlssVersion) < 310)
            warn(2, "DLSS %s is older than 310: models J, K, L and M need 310, so NVIDIA's default is used.", s.dlssVersion);
        else if (i >= 0) add(kInk, 1, "%s", about[i]);
        else add(kInk, 1, "Model %c is set in ffxv_dlss.ini and kept until another is picked.", char('A' + s.preset - 1));
        break;
    }
    case kSharpening: add(kInk, 1, "%s", "Extra sharpening after upscaling."); break;
    case kGameSharpening: add(kInk, 1, "%s", "The game's own sharpening, after anti-aliasing. 100% is the game's look."); break;
    case kBackend: {
        const char* dimLine = nullptr;
        int dimCost = 1;
        char ver[96] = "";
        // DLSS frame generation unsupported here (Windows only: Wine may read GPU scheduling wrongly), whether it runs or its
        // start-up stopped after the check: the reason, unless a pending switch's restart line would then not fit
        int adviceLines = 0;
        const bool dlssFg = s.fgBackend == 0 || (!s.fgAvailable && s.running == 0 && s.dlssg);
        const char* const reported = dlssFg && !s.wine ? supportAdvice(s.fgSupport, &adviceLines) : nullptr;
        const char* advice = reported && restartBackend(s) && adviceLines + 1 > kHelpLines ? nullptr : reported;
        if (!s.fgAvailable) {
            if (s.running == 0 && advice) warn(adviceLines, "%s", advice);
            else if (s.running == 0 && s.dlssg && !reported) dimLine = "Not available: DLSS frame generation needs an RTX 40 or 50 card.";
            else if (s.running == 0) warn(2, "%s", "nvngx_dlssg.dll not found. Copy it from the DLSS SDK into ffxv_dlss\\streamline and restart.");
            else if (s.running == 2) {
                if (!s.xessFgFiles) warn(2, "%s", "XeSS frame generation needs libxess.dll, libxess_fg.dll and libxell.dll in ffxv_dlss\\xess.");
                else if (s.formatRefused) { dimLine = "Not available: XeSS frame generation does not take HDR output. Turn HDR off in the game."; dimCost = 2; }
                else if (s.launchEnable) { dimLine = "Not available: Intel XeSS frame generation did not start. ffxv_dlss.log has the reason."; dimCost = 2; }
            } else if (!s.fsrFiles) warn(1, "%s", "AMD's FSR files not found (see the Upscaling page).");
            else if (!s.fsrFgDll) warn(2, "%s", "amd_fidelityfx_framegeneration_dx12.dll not found in ffxv_dlss\\fidelityfx.");
            else if (s.launchEnable) { dimLine = "Not available: AMD FSR frame generation did not start. ffxv_dlss.log has the reason."; dimCost = 2; }
        } else if (advice) warn(adviceLines, "%s", advice);  // unsupported here first, then scRGB, then the version
        else if (s.fgBackend == 0 && s.noFrames == fg_support::kNoneScrgb && !restartBackend(s))  // a restart line comes first
            warn(2, "Adding no frames: DLSS frame generation does not support the game's 16-bit scRGB HDR output. %s", fg_support::kScrgbAdvice);
        else if (s.fgBackend == 0 && *s.dlssgVersion) { std::snprintf(ver, sizeof ver, "NVIDIA DLSS frame generation %s.", s.dlssgVersion); dimLine = ver; }
        else if (s.fgBackend == 1 && *s.fsrFgVersion) { std::snprintf(ver, sizeof ver, "AMD FSR frame generation %s.", s.fsrFgVersion); dimLine = ver; }
        else if (s.fgBackend == 2 && *s.xessFgVersion) { std::snprintf(ver, sizeof ver, "Intel XeSS frame generation %s, with Xe Low Latency.", s.xessFgVersion); dimLine = ver; }
        if (h.flag != kWarnFlag) add(kInk, 1, "%s", "Auto: DLSS on RTX 40 and 50, XeSS on Intel, else FSR.");
        if (restartBackend(s)) restart("Restart the game to switch frame generation.");
        if (dimLine) add(kDim, dimCost, "%s", dimLine);
        if (s.picked == 1 || s.picked == 2) add(kDim, 1, "%s", s.picked == 1 ? kTestedFsr : kTestedXess);  // tested-only
        break;
    }
    case kFgEnabled:
        add(kInk, 1, "%s", "Shows generated frames between the ones the game renders.");
        if (restartFg(s)) restart("Restart the game to switch frame generation.");
        if (s.generating && s.noFrames)
            add(kDim, 1, "%s", s.noFrames == fg_support::kNoneScrgb ? "On, but adding no frames with the game's HDR output." : "On, but adding no frames at the moment.");
        else if (s.generating && s.rendered > 0)
            add(kDim, 1, "Generating %.1f\xC3\x97: %.0f fps shown from %.0f rendered.", double(s.shown / s.rendered), double(s.shown), double(s.rendered));
        else if (s.fgAvailable && s.fgEnable) add(kDim, 1, "%s", "Idle: turns on in gameplay, where the upscaler runs.");
        else if (!s.fgEnable) add(kDim, 1, "%s", "Frame generation is off.");
        break;
    case kFgMode:
        add(kInk, 1, "%s", "Frames shown for each frame the game renders.");
        if (s.fgBackend != 0) add(kDim, 1, "%s", "AMD FSR and Intel XeSS frame generation show two.");
        else if (s.dynamicSupported) add(kDim, 1, "%s", "Dynamic follows the target frame rate.");
        break;
    case kTarget: add(kInk, 1, "%s", "The frame rate Dynamic mode aims for."); break;
    case kLowLatency: {
        const Latency l = latency(s);
        if (l == kReflex) {
            add(kInk, 1, "%s", s.fgEnable ? "Lower input lag. Frame generation needs it, so it stays on." : "Lower input lag.");
            if (s.reflex == 2) add(kDim, 1, "%s", "Boost keeps GPU clocks up when the CPU limits. More power.");
        } else if (l == kXell) add(kInk, 2, "%s", s.fgEnable ? "Lower input lag. On while XeSS frame generation is enabled, which needs it." : "Lower input lag.");
        else {
            if (s.al2Linux) add(kDim, 1, "%s", "Not available on Linux: Anti-Lag 2 needs AMD's Windows driver.");
            else if (!s.al2Ready) warn(2, "%s", "Not available: Anti-Lag 2 needs a Radeon RX 5000 or newer and Adrenalin 24.7.1 or newer.");
            else if (s.antilag) add(kInk, 2, "%s", s.paces ? "On: AMD's driver also holds back the game's next frame until just before the GPU needs it."
                                                           : "On: AMD's driver holds back the game's next frame until just before the GPU needs it.");
            else add(kInk, 2, "%s", s.paces ? "Off: the game waits for the GPU each frame instead of queueing frames ahead."
                                            : "Off: frames can queue ahead of the GPU, as without the mod.");
            add(kDim, 1, "%s", "Untested on Radeon so far: if the game stutters, turn it off.");
        }
        break;
    }
    case kFrameCap:
        if (capMode(s) == 1)
            add(kInk, 2, "Caps output at %d fps, just under the refresh, so %s stays engaged. Use V-Sync on in the driver.", s.autoCap, s.sync);
        else add(kInk, 1, "%s", "Off, Auto (just under the display's refresh) or a cap of your own.");
        if (latency(s) == kNoLatency && s.paces)  // FSR or XeSS without a latency row: the presenter's pacing
            add(kDim, 1, "%s", s.fgBackend == 1 || s.active == 1 ? "FSR has no Reflex: the game waits for the GPU each frame."
                                                                  : "The game waits for the GPU each frame: no frames queue ahead.");
        break;
    case kCap: add(kInk, 1, "%s", "The highest frame rate shown."); break;
    case kCounter: add(kInk, 1, "%s", "Frame rate, frame time and what is running, in a corner of the screen."); break;
    case kPosition: add(kInk, 1, "%s", "Where the FPS counter sits."); break;
    case kHud: add(kInk, 1, "%s", "Crisper HUD text, icons and title screen. Costs a little frame rate at 4K."); break;
    case kLayout:
        add(kInk, 2, "%s", "Portable: bigger text, pages and touch controls for handhelds. Switches now.");
        add(kDim, 1, "%s", s.portable ? "Changes apply and save immediately. Touch works too." : "Changes apply and save immediately.");
        break;
    default: break;
    }
    return h;
}

// ---- status line -------------------------------------------------------------------------------------------------------
// The upscaler running and its version (in its vendor's colour), render size → output size, and frame generation's state.
struct Status {
    char up[48] = ""; int upVendor = -1;          // -1: dim (not running)
    char size[64] = "";
    char fg[48] = ""; Tone fgTone = kInk; int fgVendor = -1;
};
inline Status status(const State& s) {
    Status st;
    if (!s.upOn) std::snprintf(st.up, sizeof st.up, "Upscaling off");
    else if (s.active >= 0 && s.active <= 2) {
        const char* ver = s.active == 0 ? s.dlssVersion : s.active == 1 ? s.fsrVersion : s.xessVersion;
        std::snprintf(st.up, sizeof st.up, *ver ? "%s %s" : "%s", shortName(s.active), ver);
        st.upVendor = s.active;
    } else std::snprintf(st.up, sizeof st.up, "%s", shortName(s.want));
    if (s.inW && s.outW) std::snprintf(st.size, sizeof st.size, "%u\xC3\x97%u \xE2\x86\x92 %u\xC3\x97%u", s.inW, s.inH, s.outW, s.outH);
    else if (s.ratio && s.dispW)
        std::snprintf(st.size, sizeof st.size, "%u\xC3\x97%u from next start", s.dispW * unsigned(s.ratio) / 100, s.dispH * unsigned(s.ratio) / 100);
    else if (s.ratio) std::snprintf(st.size, sizeof st.size, "%d%% from next start", s.ratio);
    if (s.generating && s.noFrames) {
        std::snprintf(st.fg, sizeof st.fg, "adding no frames");
        st.fgTone = s.noFrames == fg_support::kNoneScrgb ? kAmber : kDim;
    } else if (s.generating && s.rendered > 0) {
        std::snprintf(st.fg, sizeof st.fg, "generating %.1f\xC3\x97", double(s.shown / s.rendered));
        st.fgTone = kVendor; st.fgVendor = s.fgBackend;
    } else if (!s.fgAvailable && !s.offAtLaunch) { std::snprintf(st.fg, sizeof st.fg, "not available"); st.fgTone = kDim; }
    else if (!s.fgEnable || !s.fgAvailable) std::snprintf(st.fg, sizeof st.fg, "off");
    else std::snprintf(st.fg, sizeof st.fg, "waiting for gameplay");
    return st;
}

// The FPS counter's frame generation line; true when it shows a multiplier, drawn in the generating vendor's colour.
inline bool counterFg(char* out, size_t n, bool available, bool generating, int noFrames, float shown, float rendered) {
    if (generating && noFrames) { std::snprintf(out, n, "FG no frames"); return false; }
    if (generating && rendered > 0) { std::snprintf(out, n, "FG %.1f\xC3\x97", double(shown / rendered)); return true; }
    std::snprintf(out, n, "%s", available ? "FG off" : "FG n/a");
    return false;
}

// ---- pages -------------------------------------------------------------------------------------------------------------
// Tab turns the desktop page forward on a press; never with Shift (Steam's overlay key), while a number box is typed in
// or while a list is open.
inline int tabStep(bool pressed, bool shift, bool typing, bool busy) { return pressed && !shift && !typing && !busy ? 1 : 0; }
inline int turn(int page, int dir) { return ((page + dir) % kPageCount + kPageCount) % kPageCount; }

}  // namespace pages
