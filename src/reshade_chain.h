// Whether Lumenveil loads ReShade: the player's ReShade dxgi.dll is kept by the installer as dxgi.dll.before_ffxv_dlss,
// and main.cpp loads it at start-up underneath which ReShade wraps the game's own objects. Pure, so
// tests/reshade_chain_test.cpp checks it on the host; main.cpp reads the file and the setting.
#pragma once
#include <string_view>

namespace reshade_chain {

// ProductName from the file's version information is exactly "ReShade", ignoring case and surrounding spaces.
inline bool isReShade(std::wstring_view product) {
    auto space = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; };
    while (!product.empty() && space(product.front())) product.remove_prefix(1);
    while (!product.empty() && space(product.back())) product.remove_suffix(1);
    constexpr std::wstring_view want = L"reshade";
    if (product.size() != want.size()) return false;
    for (size_t i = 0; i < want.size(); ++i) {
        wchar_t c = product[i];
        if (c >= L'A' && c <= L'Z') c = wchar_t(c - L'A' + L'a');
        if (c != want[i]) return false;
    }
    return true;
}

struct Decision { bool load; const char* reason; };  // reason: logged as "reshade: not loaded (<reason>)"

// loadSetting: [reshade] load (1 unless set); keptExists: dxgi.dll.before_ffxv_dlss is there; product: its ProductName,
// empty when unreadable.
inline Decision decide(int loadSetting, bool keptExists, std::wstring_view product) {
    if (!loadSetting) return { false, "load=0 in ffxv_dlss.ini" };
    if (!keptExists) return { false, "no ReShade kept" };
    if (!isReShade(product)) return { false, "the kept dxgi.dll is not ReShade" };
    return { true, "" };
}

}  // namespace reshade_chain
