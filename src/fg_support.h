#pragma once
// Why NVIDIA's DLSS frame generation cannot run on this PC, from Streamline's slIsFeatureSupported result, in words for
// the log; and when a 500-frame window shows frame generation on but no frames generated. No Windows or Streamline
// headers: tests/fg_support_test.cpp checks it on the host, and present12.cpp ties the numbers to sl::Result.

namespace fg_support {

enum Result : int { kOk = 0, kDriverOutOfDate = 2, kOSOutOfDate = 3, kOSDisabledHWS = 4, kNoSupportedAdapter = 6, kAdapterNotSupported = 7 };

// The reason for a result, or nullptr: supported, or a result with no words here (the log gives its number).
inline const char* reason(int r) {
    switch (r) {
    case kOSDisabledHWS: return "Hardware-accelerated GPU scheduling is off in Windows";
    case kOSOutOfDate: return "Windows is too old";
    case kDriverOutOfDate: return "the NVIDIA driver is too old";
    case kNoSupportedAdapter: return "no graphics card that supports it was found";
    case kAdapterNotSupported: return "this graphics card does not support it";
    default: return nullptr;
    }
}

// Frame generation was on for every frame of the window and each rendered frame was shown once: none generated.
inline bool noneGenerated(unsigned onFrames, unsigned presented, unsigned rendered) {
    return rendered > 0 && onFrames == rendered && presented == rendered;
}

}  // namespace fg_support
