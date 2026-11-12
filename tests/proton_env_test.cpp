// Host test for src/proton_env.h, which works out the values of VKD3D_DISABLE_EXTENSIONS and DXVK_CONFIG the mod sets
// under Proton. Built and run by tests/check_proton_env.sh. Every expected string is written out by hand.
#include "proton_env.h"
#include <cstdio>
#include <string>

static int g_fails;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fails; } } while (0)

using proton_env::Outcome;

static const char* name(Outcome::Kind k) {
    return k == Outcome::kSet ? "set" : k == Outcome::kKept ? "kept" : "too long";
}

static void vk(const std::string& in, Outcome::Kind kind, const std::string& value = "") {
    Outcome o = proton_env::vkd3dDisableList(in);
    CHECK(o.kind == kind, "vkd3d \"%s\": got %s, want %s", in.c_str(), name(o.kind), name(kind));
    if (kind == Outcome::kSet)
        CHECK(o.value == value, "vkd3d \"%s\": got \"%s\", want \"%s\"", in.c_str(), o.value.c_str(), value.c_str());
}

static void dx(const std::string& in, Outcome::Kind kind, const std::string& value = "") {
    Outcome o = proton_env::dxvkConfig(in);
    CHECK(o.kind == kind, "dxvk \"%s\": got %s, want %s", in.c_str(), name(o.kind), name(kind));
    if (kind == Outcome::kSet)
        CHECK(o.value == value, "dxvk \"%s\": got \"%s\", want \"%s\"", in.c_str(), o.value.c_str(), value.c_str());
}

int main() {
    // vkd3d-proton: a list of extension names split by ',' or ';'.
    vk("", Outcome::kSet, "VK_NV_raw_access_chains");
    vk("VK_EXT_foo", Outcome::kSet, "VK_EXT_foo,VK_NV_raw_access_chains");
    vk("VK_EXT_foo;VK_EXT_bar", Outcome::kSet, "VK_EXT_foo;VK_EXT_bar,VK_NV_raw_access_chains");
    vk("VK_NV_raw_access_chains", Outcome::kKept);
    vk("VK_EXT_foo,VK_NV_raw_access_chains", Outcome::kKept);
    vk("VK_NV_raw_access_chains;VK_EXT_foo", Outcome::kKept);
    // A longer name that only contains ours is a different extension: ours still goes on the end.
    vk("VK_NV_raw_access_chains2", Outcome::kSet, "VK_NV_raw_access_chains2,VK_NV_raw_access_chains");
    vk("XVK_NV_raw_access_chains", Outcome::kSet, "XVK_NV_raw_access_chains,VK_NV_raw_access_chains");
    // Both layers read at most 259 characters; a longer value reads as unset. ",VK_NV_raw_access_chains" is 24.
    vk(std::string(235, 'a'), Outcome::kSet, std::string(235, 'a') + ",VK_NV_raw_access_chains");   // 259
    vk(std::string(236, 'a'), Outcome::kTooLong);                                                  // 260

    // DXVK: option lines split by ';'.
    dx("", Outcome::kSet, "dxvk.enableNvRawAccessChains = False");
    dx("dxgi.maxFrameRate = 60", Outcome::kSet, "dxgi.maxFrameRate = 60; dxvk.enableNvRawAccessChains = False");
    dx("dxgi.maxFrameRate = 60;", Outcome::kSet, "dxgi.maxFrameRate = 60; dxvk.enableNvRawAccessChains = False");
    dx("dxgi.maxFrameRate = 60; ", Outcome::kSet, "dxgi.maxFrameRate = 60; dxvk.enableNvRawAccessChains = False");
    // The player's own setting of the option wins, whatever its value.
    dx("dxvk.enableNvRawAccessChains = True", Outcome::kKept);
    dx("dxgi.maxFrameRate = 60; dxvk.enableNvRawAccessChains=False", Outcome::kKept);
    dx("dxgi.maxFrameRate = 60;  dxvk.enableNvRawAccessChains = True", Outcome::kKept);
    // "; dxvk.enableNvRawAccessChains = False" is 38 characters.
    dx(std::string(221, 'a'), Outcome::kSet, std::string(221, 'a') + "; dxvk.enableNvRawAccessChains = False");  // 259
    dx(std::string(222, 'a'), Outcome::kTooLong);                                                               // 260

    if (g_fails) { std::printf("%d check(s) failed\n", g_fails); return 1; }
    std::printf("proton_env_test: all checks passed\n");
    return 0;
}
