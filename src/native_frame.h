// Native presentation: the one place the order of the menu's frame on the game's own swap chain lives. No Windows
// headers, so tests/native_frame_test.cpp drives it on the host with counting fakes (stress S1). menu.cpp instantiates it
// with the real D3D11 operations.
//
// Ops: acquire() the back buffer (false: nothing held); key() its size and format; view() a render target view on it
// (false: nothing more held); rebuild(key) the mod's own textures for it; save() the game's state; draw(); restore();
// releaseView(); releaseBuffer(). A swap chain cannot be resized or recreated while a reference is out, so every path
// ends holding nothing.
#pragma once

struct NativeTargetKey {
    unsigned w, h, fmt;
    bool operator==(const NativeTargetKey& o) const { return w == o.w && h == o.h && fmt == o.fmt; }
};

enum NativeFrameResult { kNfNotWanted, kNfAcquireFailed, kNfViewFailed, kNfRebuildFailed, kNfDrawFailed, kNfDrawn };

// wanted: the counter or the menu is shown. built: the key the mod's textures were last built for (cleared on failure).
template <class Ops> NativeFrameResult nativeFrame(Ops& o, bool wanted, NativeTargetKey& built) {
    if (!wanted) return kNfNotWanted;
    if (!o.acquire()) return kNfAcquireFailed;
    NativeFrameResult r = kNfDrawn;
    const NativeTargetKey k = o.key();
    if (!o.view()) {
        r = kNfViewFailed;
    } else {
        if (!(k == built)) {
            if (o.rebuild(k)) built = k;
            else { built = NativeTargetKey{}; r = kNfRebuildFailed; }
        }
        if (r == kNfDrawn) {
            o.save();
            if (!o.draw()) r = kNfDrawFailed;
            o.restore();
        }
        o.releaseView();
    }
    o.releaseBuffer();
    return r;
}
