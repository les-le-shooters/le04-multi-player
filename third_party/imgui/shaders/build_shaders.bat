@echo off
rem 重新生成 imgui_impl_dx11_shaders.h（只有改了 .hlsl 才需要；需要 Windows SDK 的 fxc.exe）
setlocal
cd /d "%~dp0"
set FXC="C:\Program Files (x86)\Windows Kits\10\bin\10.0.22621.0\x64\fxc.exe"
%FXC% /nologo /T vs_4_0 /E main /Vn imgui_dx11_vs /Fh vs.h imgui_dx11_vs.hlsl || exit /b 1
%FXC% /nologo /T ps_4_0 /E main /Vn imgui_dx11_ps /Fh ps.h imgui_dx11_ps.hlsl || exit /b 1
(
echo // 由 shaders\build_shaders.bat 从 shaders\*.hlsl 生成，勿手改。
echo // imgui_impl_dx11.cpp 定义了 IMGUI_IMPL_DX11_PRECOMPILED 时用这里的字节码，不再在运行时调用 D3DCompile（不依赖 d3dcompiler_47.dll）
echo #pragma once
type vs.h
type ps.h
) > ..\imgui_impl_dx11_shaders.h
del vs.h ps.h
echo SHADERS OK
