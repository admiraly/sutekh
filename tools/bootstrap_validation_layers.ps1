# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
# Builds the Khronos Vulkan validation layer (VK_LAYER_KHRONOS_validation) from its pinned source tag into
# local\toolchains\vvl-<version> (gitignored), for machines without the LunarG SDK. tools\env-windows.ps1 then points
# the Vulkan loader at it (VK_ADD_LAYER_PATH), and the render tests run with validation on. Needs git, CMake, Ninja,
# Python and the MSVC build tools (vcvars64.bat); takes a while (the layer and its SPIR-V dependencies).
param([string]$Tag = 'vulkan-sdk-1.4.363.0')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$version = $Tag -replace '^vulkan-sdk-', ''
$src = Join-Path $root "local\build\vvl-$version"
$out = Join-Path $root "local\toolchains\vvl-$version"
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2]) }
}
if (-not (Test-Path -LiteralPath $src)) {
    git clone --depth 1 -b $Tag https://github.com/KhronosGroup/Vulkan-ValidationLayers.git $src
    if ($LASTEXITCODE -ne 0) { throw 'clone failed' }
}
cmake -S $src -B "$src\build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DUPDATE_DEPS=ON -DBUILD_WERROR=OFF -DBUILD_TESTS=OFF "-DCMAKE_INSTALL_PREFIX=$out"
if ($LASTEXITCODE -ne 0) { throw 'configure failed' }
cmake --build "$src\build" --config Release
if ($LASTEXITCODE -ne 0) { throw 'build failed' }
cmake --install "$src\build" --config Release
if ($LASTEXITCODE -ne 0) { throw 'install failed' }
Write-Output "validation layer installed in $out"
