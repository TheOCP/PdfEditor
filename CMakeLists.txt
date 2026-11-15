cmake_minimum_required(VERSION 3.21)

project(PdfEditor LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

foreach(cfg "" _DEBUG _RELEASE _RELWITHDEBINFO _MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY${cfg} ${CMAKE_BINARY_DIR}/bin)
endforeach()

include(FetchContent)

# Dear ImGui (same version as PixelToolkit). No CMakeLists of its own, so the
# core + Win32/DX11 backends are compiled straight into the exe.
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.5
)
FetchContent_MakeAvailable(imgui)
set(IMGUI_DIR ${imgui_SOURCE_DIR})

# PDFium (Chromium's PDF engine), prebuilt by bblanchon/pdfium-binaries.
# Pinned release + SHA256 so the build is reproducible.
FetchContent_Declare(
    pdfium
    URL      https://github.com/bblanchon/pdfium-binaries/releases/download/chromium/8111/pdfium-win-x64.tgz
    URL_HASH SHA256=3452d11031050ec65e57f180215b57b9e6d465e3fbedafa8a77ad90885c7d23e
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(pdfium)
set(PDFium_DIR ${pdfium_SOURCE_DIR})
find_package(PDFium REQUIRED)

add_executable(PdfEditor WIN32
    src/main.cpp
    src/app.cpp
    src/canvas.cpp
    src/panels.cpp
    src/dialogs.cpp
    src/pdf_doc.cpp
    src/pdf_ops.cpp
    src/imageio.cpp
    src/sysfonts.cpp
    src/gfx.cpp
    src/selftest.cpp
    res/app.rc
    res/app.manifest

    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/misc/cpp/imgui_stdlib.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
)

target_include_directories(PdfEditor PRIVATE
    src
    res
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${IMGUI_DIR}/misc/cpp
)

target_link_libraries(PdfEditor PRIVATE
    pdfium
    d3d11 dxgi d3dcompiler
    comdlg32 ole32 oleaut32 shell32 shlwapi windowscodecs advapi32 user32 gdi32 winspool
)

target_compile_definitions(PdfEditor PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_compile_options(PdfEditor PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/utf-8 /W3 /permissive- /MP>)
# res/app.manifest carries DPI awareness + common controls; keep the linker's own UAC block.

# pdfium.dll + its licenses next to the exe.
add_custom_command(TARGET PdfEditor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${pdfium_SOURCE_DIR}/bin/pdfium.dll"
        "$<TARGET_FILE_DIR:PdfEditor>/pdfium.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
        "${pdfium_SOURCE_DIR}/licenses"
        "$<TARGET_FILE_DIR:PdfEditor>/licenses"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${pdfium_SOURCE_DIR}/LICENSE"
        "$<TARGET_FILE_DIR:PdfEditor>/licenses/pdfium-LICENSE.txt"
    COMMENT "Copying pdfium.dll next to PdfEditor.exe")
