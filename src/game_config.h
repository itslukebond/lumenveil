// Where the game keeps its settings, kept free of Windows types so tests/game_config_test.cpp covers it on the host.
#pragma once
#include <string>

// The game keeps its settings under USERPROFILE\Documents, even when Windows has moved the Documents folder (OneDrive
// folder backup, or the folder's Location tab), so the menu builds the folder from USERPROFILE, not from the Documents
// known folder. Empty in, empty out.
inline std::wstring gameSettingsRoot(std::wstring userProfile) {
    while (!userProfile.empty() && (userProfile.back() == L'\\' || userProfile.back() == L'/')) userProfile.pop_back();
    if (userProfile.empty()) return {};
    return userProfile + L"\\Documents\\My Games\\FINAL FANTASY XV\\Steam\\";
}

// An environment variable read through get(buffer, size), which answers as GetEnvironmentVariableW does: the size
// needed (terminator included) when the buffer is null or too small, the length copied on success, 0 when unset.
// Empty when the variable is unset or empty, or grew between the two calls.
template <class Get> std::wstring envValue(Get get) {
    std::wstring v(get(nullptr, 0), L'\0');
    const unsigned long n = v.empty() ? 0 : get(v.data(), (unsigned long)v.size());
    v.resize(n < v.size() ? n : 0);
    return v;
}
