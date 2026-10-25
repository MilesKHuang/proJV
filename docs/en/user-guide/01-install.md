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

libcurl is taken from the system:

```bash
sudo apt-get install -y libcurl4-openssl-dev     # Debian / Ubuntu
# sudo dnf install libcurl-devel                 # Fedora
```

Check what the binary still needs:

```bash
ldd ./proJV_tui
```

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

## Check it worked

```bash
./proJV_tui --help      # not required; the app just starts
```

The app starts, shows the input box and the status bar. If it exits
immediately, run it from a terminal (not by double-clicking) so you can read
the error.

---

Next -> [02 first run](02-first-run.md) | Want to change it -> [dev guide](../dev-guide/README.md)
