#pragma once
// Under Proton the mod turns off NVIDIA's raw access chains Vulkan extension in both translation layers: DXVK (the
// game's D3D11) and vkd3d-proton (the mod's D3D12 device). With it on, NVIDIA's Linux drivers can hang the GPU as a
// scene starts. The mod does it through the two layers' own environment variables, set in the game process before
// either layer starts. These functions work out the new values and keep anything the player set; the Windows calls
// are in main.cpp.
#include <string>

namespace proton_env {

// Both layers read at most 259 characters; a longer value reads as unset, so the player's own settings would go too.
constexpr size_t kMaxLen = 259;

struct Outcome {
    enum Kind { kSet, kKept, kTooLong };
    Kind kind;
    std::string value;  // the new value when kind is kSet
};

// VKD3D_DISABLE_EXTENSIONS: extension names split by ',' or ';' (whole names only, as vkd3d-proton matches them).
inline Outcome vkd3dDisableList(const std::string& cur) {
    static const std::string kItem = "VK_NV_raw_access_chains";
    auto sep = [](char c) { return c == ',' || c == ';'; };
    for (size_t p = cur.find(kItem); p != std::string::npos; p = cur.find(kItem, p + 1)) {
        size_t end = p + kItem.size();
        if ((p == 0 || sep(cur[p - 1])) && (end == cur.size() || sep(cur[end]))) return {Outcome::kKept, {}};
    }
    std::string v = cur.empty() ? kItem : cur + "," + kItem;
    if (v.size() > kMaxLen) return {Outcome::kTooLong, {}};
    return {Outcome::kSet, v};
}

// DXVK_CONFIG: option lines split by ';'. If the player sets the option themselves, their value stays.
inline Outcome dxvkConfig(const std::string& cur) {
    static const std::string kKey = "dxvk.enableNvRawAccessChains";
    static const std::string kLine = kKey + " = False";
    for (size_t start = 0; start <= cur.size();) {
        size_t end = cur.find(';', start);
        if (end == std::string::npos) end = cur.size();
        size_t k = cur.find_first_not_of(" \t", start);
        if (k != std::string::npos && k < end && cur.compare(k, kKey.size(), kKey) == 0) {
            size_t after = k + kKey.size();
            if (after == end || cur[after] == ' ' || cur[after] == '\t' || cur[after] == '=') return {Outcome::kKept, {}};
        }
        start = end + 1;
    }
    std::string head = cur;
    while (!head.empty() && (head.back() == ' ' || head.back() == '\t')) head.pop_back();
    if (!head.empty() && head.back() == ';') head.pop_back();
    std::string v = head.empty() ? kLine : head + "; " + kLine;
    if (v.size() > kMaxLen) return {Outcome::kTooLong, {}};
    return {Outcome::kSet, v};
}

}  // namespace proton_env
