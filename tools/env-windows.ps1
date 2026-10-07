# Dot-source this file to enable the project-local x64 Windows toolchain.
$sutekhRoot = Split-Path -Parent $PSScriptRoot
$sutekhLlvmBin = Join-Path $sutekhRoot 'local\toolchains\llvm-23.1.3\LLVM\bin'
$sutekhPythonBin = Join-Path $sutekhRoot '.venv\Scripts'
$sutekhMsvc = Get-ChildItem -Path "${env:ProgramFiles(x86)}\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*" -Directory -ErrorAction SilentlyContinue |
    Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'include\vcruntime.h') } |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
$sutekhSdkRoot = "${env:ProgramFiles(x86)}\Windows Kits\10"
$sutekhSdk = Get-ChildItem -LiteralPath (Join-Path $sutekhSdkRoot 'Include') -Directory -ErrorAction SilentlyContinue |
    Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'ucrt\stdio.h') } |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (-not (Test-Path -LiteralPath (Join-Path $sutekhLlvmBin 'clang.exe')) -or -not $sutekhMsvc -or -not $sutekhSdk) {
    throw 'Required LLVM, MSVC headers, or Windows SDK not found.'
}
$env:PATH = "$sutekhLlvmBin;$sutekhPythonBin;$env:PATH"
$env:INCLUDE = (@((Join-Path $sutekhMsvc.FullName 'include'), (Join-Path $sutekhSdk.FullName 'ucrt'),
    (Join-Path $sutekhSdk.FullName 'shared'), (Join-Path $sutekhSdk.FullName 'um'), $env:INCLUDE) |
    Where-Object { $_ }) -join ';'
$env:LIB = (@((Join-Path $sutekhMsvc.FullName 'lib\x64'),
    (Join-Path $sutekhSdkRoot "Lib\$($sutekhSdk.Name)\ucrt\x64"),
    (Join-Path $sutekhSdkRoot "Lib\$($sutekhSdk.Name)\um\x64"), $env:LIB) |
    Where-Object { $_ }) -join ';'
$env:CC = Join-Path $sutekhLlvmBin 'clang.exe'
