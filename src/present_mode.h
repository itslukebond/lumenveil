#pragma once
// How a launch presents its frames, chosen once when the game first creates its swap chain. No Windows headers: tested
// on the host by tests/present_mode_test.cpp.
//
// [d3d12] present: 0 off (the game presents on its own; no menu, counter or frame generation), 2 always our D3D12
// presenter, anything else (1, the default) automatic: our D3D12 presenter when a frame generation backend started at
// launch, which needs our swap chain, else native. Native: the game keeps its own swap chain and Present, and the menu
// and counter are drawn into its back buffer on its own D3D11 device, with no frame copy and no hand-over between
// devices. A window holds one flip-model swap chain, so the choice holds for the launch.
enum PresentMode { kPresentOff, kPresentNative, kPresentD3D12 };
inline PresentMode presentMode(int setting, bool backendStarted) {
    if (setting == 0) return kPresentOff;
    if (setting == 2 || backendStarted) return kPresentD3D12;
    return kPresentNative;
}
// Natively a D3D12 device is made only for FSR or XeSS upscaling, which run on it (DLSS runs on the game's device).
inline bool nativeNeedsD3D12(bool fsrFiles, bool xessFiles) { return fsrFiles || xessFiles; }
// Tearing on the game's own swap chain (Windows): variable refresh rate displays need it in a borderless window at
// interval 0. The flag goes on at creation and every ResizeBuffers when the system supports it, never under
// Wine/Proton; a Present carries it only at interval 0, and never again after one carrying it was refused.
inline bool nativeTearingFlag(bool supported, bool wine) { return supported && !wine; }
inline bool nativeTearing(bool supported, bool wine, unsigned interval, bool refused) {
    return nativeTearingFlag(supported, wine) && interval == 0 && !refused;
}
