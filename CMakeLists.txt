cmake_minimum_required(VERSION 3.20)
project(ffxv_dlss LANGUAGES C CXX ASM_MASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")  # static CRT: no extra DLLs in the game folder

file(GLOB MINHOOK_SRC third_party/minhook/src/*.c third_party/minhook/src/hde/*.c)

add_library(dxgi SHARED src/main.cpp src/glyph2x.cpp src/fsr.cpp src/xess.cpp src/up12.cpp src/present12.cpp src/forward.asm src/dxgi.def ${MINHOOK_SRC})
target_include_directories(dxgi PRIVATE third_party/minhook/include src third_party/streamline/include
                           third_party/fidelityfx/api/include third_party/fidelityfx/upscalers/include third_party/fidelityfx/framegeneration/include
                           third_party/xess/inc)  # Intel XeSS headers (submodule); Intel's DLLs are loaded at run time, never linked
target_compile_definitions(dxgi PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(dxgi PRIVATE dxguid d3d12)
target_link_options(dxgi PRIVATE /PDBALTPATH:dxgi.pdb)  # the DLL records the bare .pdb name, not the build folder

# stage 2: DLSS (NGX static lib, /MT) + HUD fix
target_sources(dxgi PRIVATE src/dlss.cpp)
target_include_directories(dxgi PRIVATE third_party/dlss/include)
target_link_libraries(dxgi PRIVATE ${CMAKE_SOURCE_DIR}/third_party/dlss/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib windowscodecs ole32 version)

# Insert menu + fps counter (Dear ImGui, MIT), drawn by the D3D12 presenter
set(IMGUI third_party/imgui)
target_sources(dxgi PRIVATE src/menu.cpp src/res/dxgi.rc ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp
                            ${IMGUI}/backends/imgui_impl_dx12.cpp)
# the resource script embeds the menu logo, so a new logo.png rebuilds it
set_source_files_properties(src/res/dxgi.rc PROPERTIES OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/res/logo.png)
target_include_directories(dxgi PRIVATE ${IMGUI})
target_link_libraries(dxgi PRIVATE d3dcompiler)
