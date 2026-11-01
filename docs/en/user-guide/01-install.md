# 01 - Install

> After this page you can run proJV on Windows or Linux.

## Option A - download a release (recommended)

1. Open the [Releases page](https://github.com/MilesKHuang/proJV/releases) and
   download the archive for your platform:
   - Windows: `proJV-<version>-windows-x64.zip`
   - Linux: `proJV-<version>-linux-x64.tar.gz`
2. Unpack it anywhere. You get:
   ```
   proJV_tui(.exe)
   projv_files/
   README.md  README_zh.md  LICENSE
   ```
3. Run `proJV_tui` from a terminal in that folder. **Keep the executable and
   `projv_files/` together** - that pair is the installation.

### Windows: SmartScreen / Defender

The binary is not code-signed, so Windows may warn or quarantine it.

- SmartScreen: "More info" -> "Run anyway".
- If Defender removes the file, add an exclusion (admin PowerShell):
  ```powershell
  Add-MpPreference -ExclusionPath "C:\path\to\proJV"
  ```

### Linux: dependencies

libcurl is taken from the system. Built and tested on Ubuntu 22.04 (also under
WSL2) with CMake 3.22 and g++ 11:

```bash
sudo apt-get install -y libcurl4-openssl-dev     # Debian / Ubuntu
# sudo dnf install libcurl-devel                 # Fedora
```

That is the only package you need to add; everything else the binary links to
is part of a normal desktop install (glibc, libstdc++, and libcurl's own
dependencies). Check what your machine still misses:

```bash
ldd ./proJV_tui | grep "not found"
```

The headers usually live in the multiarch path
(`/usr/include/x86_64-linux-gnu/curl/`), which CMake finds on its own.

## Option B - build from source

You need CMake 3.16+ and a C++20 compiler.

### Windows

```powershell
# x64 Native Tools Command Prompt for VS 2019/2022
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
# binary: build\Release\proJV_tui.exe
```

### Linux

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
# binary: build/proJV_tui
```

Everything else (FTXUI, SQLiteCpp, nlohmann/json, toml++, loguru) is vendored
in `external/`. No vcpkg, conan, npm or pip.

### Make a package yourself

```bash
cpack --config build/CPackConfig.cmake -B dist
```

Same rules as CI: ZIP + `windows-x64` on Windows, TGZ + `linux-x64` on Linux.
`config.toml`, `sessions/` and `prompts/` are excluded on purpose, so a local
package never ships your API key or history.

## Put `projv` on your PATH (one command, no admin)

Two routes, same result. Both write a small forwarder so you can type `projv`
from any project directory.

```
# after `cmake --build build --config Release --target install`
#   -> the install step calls --post-install for you

# or, from an unpacked ZIP:
proJV_tui.exe --install
```

Where things go:

| | Windows | Linux |
|---|---|---|
| install dir | `%LOCALAPPDATA%\Programs\projv` | `~/.local/share/projv` |
| command | `projv.cmd` + `projv_tui.cmd` in `%LOCALAPPDATA%\Microsoft\WindowsApps` (already on PATH) | symlink in `~/.local/bin` (falls back to a `#!/bin/sh` script with `exec`) |

Rules that keep an upgrade safe:

| Content | Behaviour on a second install |
|---|---|
| `proJV_tui.exe`, README, LICENSE | replaced (a new version must win) |
| `projv_files/theme, pytool, skills` | only added when missing - your edits survive |
| `projv_files/config.toml, sessions, logs, prompts` | **never touched** (this is your key and your history) |

```
projv_tui --uninstall      # removes only the forwarder (it checks its marker), keeps your data
projv_tui --purge          # deletes the install directory; refuses if your shell is inside it
```

If `%LOCALAPPDATA%\Microsoft\WindowsApps` is not writable on your machine, the
forwarder goes to `<install dir>\bin` and the output says
`(in PATH: no)` plus the one line you need to add. Nothing is installed
machine-wide and no administrator rights are involved.

## Check it worked

```bash
./proJV_tui --help      # not required; the app just starts
```

The app starts, shows the input box and the status bar. If it exits
immediately, run it from a terminal (not by double-clicking) so you can read
the error.

---

Next -> [02 first run](02-first-run.md) | Want to change it -> [dev guide](../dev-guide/README.md)
