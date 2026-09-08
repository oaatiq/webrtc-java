@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 (
    echo vcvars64.bat failed
    exit /b 1
)
set "DEPOT_TOOLS_WIN_TOOLCHAIN=0"
set "vs2022_install=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools"
set "GYP_MSVS_OVERRIDE_PATH=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools"
set "PATH=%PATH%;C:\Users\aatiq\webrtc\depot_tools"
cd /d "C:\Users\aatiq\webrtc\src"
echo === Running ninja (webrtc.lib target only) ===
call ninja -C out/x64 obj/webrtc.lib
echo === ninja exit code: %errorlevel% ===
if exist "out\x64\obj\webrtc.lib" (
    echo === SUCCESS: webrtc.lib exists ===
    dir out\x64\obj\webrtc.lib
) else (
    echo === FAILED: webrtc.lib not found ===
    exit /b 1
)
