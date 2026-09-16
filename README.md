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

## Commit messages

Every commit and PR description follows a fixed format (`What` / `Why` / `Previous behavior` / `Blast radius` / `Other options considered` / `Testing`); CI rejects anything else. Once per clone, turn on the local hook and template so you find out at commit time instead:

```bash
scripts/commit-format/install.sh
```

The full rule is in `AGENTS.md`. Changing the rule or any workflow needs a PR approved by a code owner (`.github/CODEOWNERS`).
