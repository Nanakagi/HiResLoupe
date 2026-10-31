# 高解像ルーペのビルド。使い方: .\build.ps1 -Sdk <RTX Video SDK のフォルダ>   (透かし入りの開発用DLLを使うなら -Dll dev)
# Build HiResLoupe. Usage: .\build.ps1 -Sdk <path to the NVIDIA RTX Video SDK>   (use -Dll dev for the watermarked development DLL)
# SDK の場所は -Sdk か環境変数 NV_RTX_VIDEO_SDK で指定する。SDK は NVIDIA から入手する（このリポジトリには含まれない）。
param(
    [string]$Sdk = $env:NV_RTX_VIDEO_SDK,
    [ValidateSet('dev', 'rel')][string]$Dll = 'rel'
)
$ErrorActionPreference = 'Stop'
if (-not $Sdk) { throw 'RTX Video SDK の場所が未指定: -Sdk <パス> か環境変数 NV_RTX_VIDEO_SDK で指定してください / Specify the SDK path with -Sdk or the NV_RTX_VIDEO_SDK environment variable.' }
$root = $PSScriptRoot
$out = Join-Path $root 'build'
New-Item -ItemType Directory -Force $out | Out-Null

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio の C++ ツールが見つからない' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'

$api = "$Sdk\samples\RTX_Video_API"
$cl = "cl /nologo /utf-8 /std:c++17 /EHsc /O2 /W3 /DNDEBUG /DUNICODE /D_UNICODE /I$Sdk\include /I$api " +
      "/Fo$out/ /Fe$root\HiResLoupe.exe $out\HiResLoupe.res $root\src\main.cpp $api\rtx_video_api_dx11_impl.cpp " +
      "/link /IMPLIB:$out\HiResLoupe.lib /LIBPATH:$Sdk\lib\Windows\x64 /SUBSYSTEM:WINDOWS d3d11.lib dxgi.lib dcomp.lib d3dcompiler.lib dwmapi.lib user32.lib gdi32.lib shell32.lib advapi32.lib"
cmd /c "`"$vcvars`" >nul && rc /nologo /fo$out\HiResLoupe.res $root\assets\HiResLoupe.rc && $cl"
if ($LASTEXITCODE -ne 0) { throw "ビルド失敗 (終了コード $LASTEXITCODE)" }

Copy-Item "$Sdk\bin\Windows\x64\$Dll\nvngx_vsr.dll" $root -Force
Write-Host "OK: $root\HiResLoupe.exe  (nvngx_vsr.dll = $Dll)"
