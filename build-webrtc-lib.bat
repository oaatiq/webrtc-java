@echo off
echo === Setting up MSVC environment ===
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 (
    echo vcvars64.bat failed
    exit /b 1
)

echo === Setting environment for M99 WebRTC ===
set "DEPOT_TOOLS_WIN_TOOLCHAIN=0"
set "vs2022_install=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools"
set "GYP_MSVS_OVERRIDE_PATH=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools"
rem Use the initialized depot_tools from the webrtc checkout, appended so VS ninja.exe wins
set "PATH=%PATH%;C:\Users\aatiq\webrtc\depot_tools"

cd /d "C:\Users\aatiq\webrtc\src"

echo === Step 1: gn gen ===
call gn gen out/x64 --args="is_debug=false is_clang=false target_cpu=\"x64\" treat_warnings_as_errors=false rtc_build_examples=false rtc_include_tests=false use_rtti=true use_custom_libcxx=false symbol_level=0 rtc_use_h264=false"
if errorlevel 1 (
    echo gn gen failed
    exit /b 1
)

echo === Step 2: Strip Clang-only -Wno-* flags from ninja files ===
python strip_clang_flags.py
if errorlevel 1 (
    echo strip_clang_flags.py failed
    exit /b 1
)

echo === Step 3: ninja build ===
call ninja -C out/x64
if errorlevel 1 (
    echo ninja build failed
    exit /b 1
)

echo === webrtc.lib built successfully ===
dir out\x64\obj\webrtc.lib
