# Read or set one NVIDIA driver-profile (DRS) DWORD setting on the profile that owns an exe.
# usage: python -I drs_setting.py <exe|global> <setting_hex> [value]      (no value = read only)
# e.g.   python -I drs_setting.py ffxv_s.exe B0D384C0 1             (Smooth Motion - Enable, community-catalogued ID)
import ctypes, sys

nv = ctypes.WinDLL("nvapi64.dll")
qi = nv.nvapi_QueryInterface; qi.restype = ctypes.c_void_p; qi.argtypes = [ctypes.c_uint]
def fn(iid, *args):
    return ctypes.CFUNCTYPE(ctypes.c_int, *args)(qi(iid))

Initialize = fn(0x0150E828)
CreateSession = fn(0x0694D52E, ctypes.POINTER(ctypes.c_void_p))
LoadSettings = fn(0x375DBD6B, ctypes.c_void_p)
FindApplicationByName = fn(0xEEE566B2, ctypes.c_void_p, ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_void_p), ctypes.c_void_p)
GetSetting = fn(0x73BF8338, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint, ctypes.c_void_p)
SetSetting = fn(0x577DD202, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)
SaveSettings = fn(0xFCBC7E14, ctypes.c_void_p)
DestroySession = fn(0xDAD9CFF8, ctypes.c_void_p)

W = 2048  # NvAPI_UnicodeString length
class App(ctypes.Structure):  # NVDRS_APPLICATION_V4
    _fields_ = [("version", ctypes.c_uint), ("isPredefined", ctypes.c_uint), ("appName", ctypes.c_wchar * W),
                ("userFriendlyName", ctypes.c_wchar * W), ("launcher", ctypes.c_wchar * W), ("fileInFolder", ctypes.c_wchar * W),
                ("flags", ctypes.c_uint), ("commandLine", ctypes.c_wchar * W)]
class Setting(ctypes.Structure):  # NVDRS_SETTING_V1 (value unions are 4100 bytes: binary = len + 4096)
    _fields_ = [("version", ctypes.c_uint), ("settingName", ctypes.c_wchar * W), ("settingId", ctypes.c_uint),
                ("settingType", ctypes.c_uint), ("settingLocation", ctypes.c_uint), ("isCurrentPredefined", ctypes.c_uint),
                ("isPredefinedValid", ctypes.c_uint), ("predefined", ctypes.c_uint), ("_p", ctypes.c_byte * 4096),
                ("current", ctypes.c_uint), ("_c", ctypes.c_byte * 4096)]
assert ctypes.sizeof(Setting) == 12320 and ctypes.sizeof(App) == 20492

def check(r, what):
    if r != 0: sys.exit(f"{what} failed: NVAPI status {r}")

exe, sid = sys.argv[1], int(sys.argv[2], 16)
check(Initialize(), "Initialize")
h = ctypes.c_void_p(); check(CreateSession(ctypes.byref(h)), "CreateSession"); check(LoadSettings(h), "LoadSettings")
app = App(version=ctypes.sizeof(App) | (4 << 16)); prof = ctypes.c_void_p()
if exe == "global":  # the active global profile (what NVCP's Global Settings tab edits)
    check(fn(0x617BFF9F, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p))(h, ctypes.byref(prof)), "GetCurrentGlobalProfile")
    app.appName = "global profile"
else:
    check(FindApplicationByName(h, exe, ctypes.byref(prof), ctypes.byref(app)), f"FindApplicationByName({exe})")
s = Setting(version=ctypes.sizeof(Setting) | (1 << 16))
r = GetSetting(h, prof, sid, ctypes.byref(s))
loc = {0: "profile", 1: "global", 2: "base", 3: "default"}
print(f"{exe}: profile app '{app.userFriendlyName or app.appName}'; setting {sid:08X} " +
      (f"= {s.current} (from {loc.get(s.settingLocation, s.settingLocation)})" if r == 0 else f"not set (status {r})"))
if len(sys.argv) > 3:
    n = Setting(version=ctypes.sizeof(Setting) | (1 << 16), settingId=sid, settingType=0, current=int(sys.argv[3], 0))
    check(SetSetting(h, prof, ctypes.byref(n)), "SetSetting"); check(SaveSettings(h), "SaveSettings")
    s2 = Setting(version=ctypes.sizeof(Setting) | (1 << 16)); check(GetSetting(h, prof, sid, ctypes.byref(s2)), "GetSetting")
    print(f"set {sid:08X} = {s2.current} (location {loc.get(s2.settingLocation, s2.settingLocation)})")
DestroySession(h)
