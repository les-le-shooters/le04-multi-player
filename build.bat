@echo off
rem Build le04multi.dll (requires VS 2022 Build Tools, x86)
setlocal
cd /d "%~dp0"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || exit /b 1
if not exist build mkdir build
set IMGUI=third_party\imgui
set CFLAGS=/nologo /O2 /MT /EHsc /std:c++17 /utf-8 /W3 /DNDEBUG /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /DIMGUI_IMPL_DX11_PRECOMPILED /I%IMGUI% /Fobuild\
set SRC=src\dllmain.cpp src\core.cpp src\render.cpp src\input.cpp src\debug.cpp src\coop.cpp src\net.cpp src\launcher.cpp src\lang.cpp src\rollback.cpp
set IMGUI_SRC=%IMGUI%\imgui.cpp %IMGUI%\imgui_draw.cpp %IMGUI%\imgui_tables.cpp %IMGUI%\imgui_widgets.cpp %IMGUI%\imgui_impl_dx11.cpp %IMGUI%\imgui_impl_win32.cpp
cl %CFLAGS% /LD %SRC% %IMGUI_SRC% /Febuild\le04multi.dll /link d3d11.lib dxgi.lib user32.lib gdi32.lib dwmapi.lib ws2_32.lib winmm.lib iphlpapi.lib psapi.lib || exit /b 1
echo BUILD OK
