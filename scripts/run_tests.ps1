param(
    [string]$ObsStudioDir = "",
    [string]$Configuration = "RelWithDebInfo"
)

# Builds ascent-obs/tests/*.cpp against an existing OBS build (see README
# steps 1-2) and runs them. Only the test sources are compiled; nothing in
# obs-studio is rebuilt.

$ErrorActionPreference = "Stop"

# --- Configuration ---
if ($ObsStudioDir -eq "") {
    $ObsStudioDir = Join-Path $PSScriptRoot "..\obs-studio"
}
$ObsStudioDir = (Resolve-Path $ObsStudioDir).Path
# headers come from this checkout's obs-studio; the build can live elsewhere
$ObsSourceDir = (Resolve-Path (Join-Path $PSScriptRoot "..\obs-studio")).Path

$AscentObsDir = (Resolve-Path (Join-Path $PSScriptRoot "..\ascent-obs")).Path
$SrcDir = Join-Path $AscentObsDir "ascent-obs"
$LibDir = Join-Path $AscentObsDir "libascentobs\src"
$TestsDir = Join-Path $AscentObsDir "tests"
$DepsDir = Join-Path $ObsStudioDir ".deps\obs-deps-2024-03-19-x64"
$BuildDir = Join-Path $ObsStudioDir "build_x64"
$RunDir = Join-Path $BuildDir "rundir\$Configuration"
$OutDir = Join-Path $env:TEMP "ascent-obs-tests"

$VsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$VsPath = & $VsWhere -latest -products * -property installationPath
$VcVars = Join-Path $VsPath "VC\Auxiliary\Build\vcvars64.bat"

New-Item -ItemType Directory -Force $OutDir | Out-Null

$Includes = @(
    $SrcDir,
    (Join-Path $LibDir "public"),
    (Join-Path $ObsSourceDir "libobs"),
    (Join-Path $BuildDir "config"),
    (Join-Path $BuildDir "libobs")
) | ForEach-Object { "/I`"$_`"" }

$Sources = @(
    (Join-Path $SrcDir "obs_control\obs_audio_source_control.cpp"),
    (Join-Path $SrcDir "obs_control\obs_utils.cpp"),
    (Join-Path $LibDir "internal\communications\protocol.cpp")
) | ForEach-Object { "`"$_`"" }

$Failed = 0
foreach ($Test in Get-ChildItem $TestsDir -Filter *_test.cpp) {
    $Exe = Join-Path $OutDir ($Test.BaseName + ".exe")
    Write-Host "--- build $($Test.Name) ---"
    $Cl = "cl /nologo /EHsc /std:c++17 /W4 /wd4127 /wd4201 /DUNICODE /D_UNICODE " +
          "/D_CRT_SECURE_NO_WARNINGS /D_HAVE_OBSCONFIG_H $($Includes -join ' ') " +
          "`"$($Test.FullName)`" $($Sources -join ' ') /Fo`"$OutDir\\`" /Fe`"$Exe`" " +
          "/link /LIBPATH:`"$BuildDir\libobs\$Configuration`" obs.lib"
    cmd /c "`"$VcVars`" >nul && $Cl"
    if ($LASTEXITCODE -ne 0) {
        Write-Host "build failed: $($Test.Name)" -ForegroundColor Red
        $Failed++
        continue
    }

    Write-Host "--- run $($Test.Name) ---"
    $env:PATH = "$RunDir\bin\64bit;$DepsDir\bin;$env:PATH"
    Push-Location "$RunDir\bin\64bit"
    & $Exe ($RunDir -replace '\\', '/') | Where-Object { $_ -notmatch '^debug:' }
    $ExitCode = $LASTEXITCODE
    Pop-Location
    if ($ExitCode -ne 0) {
        $Failed++
    }
}

if ($Failed -ne 0) {
    Write-Host "$Failed test binary(ies) failed" -ForegroundColor Red
    exit 1
}
Write-Host "all tests passed" -ForegroundColor Green
