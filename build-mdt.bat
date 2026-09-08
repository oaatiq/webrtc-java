@echo off
echo === Setting up MSVC environment ===
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 (
    echo vcvars64.bat failed
    exit /b 1
)

echo === Setting JAVA_HOME ===
set "JAVA_HOME=C:\Users\aatiq\.gradle\jdks\eclipse_adoptium-21-amd64-windows.2"

echo === Adding depot_tools to PATH (appended so VS ninja.exe wins) ===
set "PATH=%PATH%;C:\Users\aatiq\webrtc\depot_tools"
set "DEPOT_TOOLS_WIN_TOOLCHAIN=0"

echo === Telling M99 WebRTC that VS 2026 is "VS 2022" ===
set "vs2022_install=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools"
set "GYP_MSVS_OVERRIDE_PATH=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools"

echo === Verifying tools ===
where cl
where java
where python

echo === Starting Maven build ===
cd /d "C:\Users\aatiq\webrtc-java"
"C:\Users\aatiq\apache-maven-3.9.9\bin\mvn.cmd" package -DskipTests 2>&1
