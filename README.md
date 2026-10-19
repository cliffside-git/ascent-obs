# ascent-obs

OBS Studio fork for Ascent.

## Prerequisites

- Visual Studio 2022 with C++ desktop workload
- CMake

## Build Steps

### 1. Configure OBS Studio

```bash
cd obs-studio
cmake --preset windows-x64 -DENABLE_BROWSER=OFF -DENABLE_WEBSOCKET=OFF
```

### 2. Build OBS Studio

```bash
cmake --build build_x64 --config RelWithDebInfo
```

### 3. Build ascent-obs

Open `ascent-obs/ascent-obs.sln` in Visual Studio, set configuration to **RelWithDebInfo | x64**, and build.

### 4. Collect build output

```powershell
.\scripts\build_script.ps1
```

Output goes to `Desktop\ascent-obs`. Use `-DryRun` to preview, or `-TargetPath "C:\path"` to change the destination.

## Tests

```powershell
.\scripts\run_tests.ps1
```

Compiles `ascent-obs/tests/*_test.cpp` against the OBS build from steps 1-2 and runs them (OBS itself is not rebuilt). Use `-ObsStudioDir "C:\path\to\obs-studio"` to point at a build in another checkout.
