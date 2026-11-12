// The frame generation backend for this launch ([fg] backend). One backend per session: it decides which swapchain the
// presenter creates. Pure, so tests/gate_test.cpp covers every combination on the host.
#pragma once

// -1 auto (or any value out of range) = DLSS-G on an NVIDIA GPU with nvngx_dlssg.dll in place, XeSS-FG on an Intel GPU
// with Intel's three XeSS files, else FSR FG; 0 DLSS-G, 1 FSR FG and 2 XeSS-FG are used as picked, on any GPU.
// Returns 0 DLSS-G, 1 FSR FG or 2 XeSS-FG; whether that backend then starts is up to its files and the GPU.
inline int fgPickBackend(int setting, unsigned vendor, bool dlssgFile, bool xessFiles) {
    if (setting >= 0 && setting <= 2) return setting;
    if (vendor == 0x10DE && dlssgFile) return 0;
    if (vendor == 0x8086 && xessFiles) return 2;
    return 1;
}
