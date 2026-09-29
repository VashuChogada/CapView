@echo off
setlocal
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
  echo Could not find Visual Studio / Build Tools with the C++ workload.
  exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

if not exist build mkdir build
rc /nologo /i src /fo build\app.res src\app.rc || exit /b 1
cl /nologo /std:c++17 /utf-8 /O2 /MT /EHsc /W3 /DUNICODE /D_UNICODE /Fo:build\ /Fe:build\CapView.exe ^
  src\main.cpp src\ui.cpp src\video.cpp src\audio.cpp src\renderer.cpp src\settings.cpp src\util.cpp build\app.res ^
  /link /SUBSYSTEM:WINDOWS /MANIFEST:NO ^
  mfplat.lib mf.lib mfreadwrite.lib mfuuid.lib d3d11.lib dxgi.lib d3dcompiler.lib d2d1.lib dwrite.lib windowscodecs.lib ^
  ole32.lib user32.lib gdi32.lib shell32.lib avrt.lib cfgmgr32.lib dwmapi.lib || exit /b 1
echo Built build\CapView.exe
