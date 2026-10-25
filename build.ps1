$ErrorActionPreference = "Stop"

$repoRoot = $PSScriptRoot
$distDir = Join-Path $repoRoot "dist"
$objDir = Join-Path $env:TEMP "nfws_build_obj"

New-Item -ItemType Directory -Force -Path $distDir | Out-Null
New-Item -ItemType Directory -Force -Path $objDir | Out-Null

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -property installationPath
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"

cmd /c "`"$vcvars`" && rc.exe /nologo /fo `"$objDir\NewFolderWithSelection.res`" `"$repoRoot\src\ShellExtension\NewFolderWithSelection.rc`" && cl.exe /nologo /std:c++20 /O2 /EHsc /W4 /MT /LD /Fo`"$objDir\\`" `"$repoRoot\src\ShellExtension\NewFolderWithSelection.cpp`" `"$objDir\NewFolderWithSelection.res`" /link /DEF:`"$repoRoot\src\ShellExtension\NewFolderWithSelection.def`" /OUT:`"$distDir\NewFolderWithSelection.dll`" /IMPLIB:`"$objDir\NewFolderWithSelection.lib`""
if ($LASTEXITCODE -ne 0) {
    throw "Failed to build NewFolderWithSelection.dll"
}

Remove-Item $objDir -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item "$distDir\*.exp", "$distDir\*.lib", "$distDir\*.res" -Force -ErrorAction SilentlyContinue

Write-Host "Built $distDir\NewFolderWithSelection.dll"
