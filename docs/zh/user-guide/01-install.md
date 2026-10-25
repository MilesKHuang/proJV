# 01 - 安装

> [English](../../en/user-guide/01-install.md) | **中文**

> 读完这页你能：在 Windows 或 Linux 上把 proJV 跑起来。

## 方式 A - 下载发布包（推荐）

1. 打开 [Releases](https://github.com/MilesKHuang/proJV/releases)，下载对应平台：
   - Windows：`proJV-<version>-windows-x64.zip`
   - Linux：`proJV-<version>-linux-x64.tar.gz`
2. 解压到任意目录，内容是：
   ```
   proJV_tui(.exe)
   projv_files/
   README.md  README_zh.md  LICENSE
   ```
3. 在该目录的终端里运行 `proJV_tui`。**可执行文件和 `projv_files/` 必须放在一起**。

### Windows：SmartScreen / Defender

二进制没有签名，所以可能被拦或被杀掉。

- SmartScreen：点「更多信息」->「仍要运行」。
- 被 Defender 删掉的话，用管理员 PowerShell 加排除项：
  ```powershell
  Add-MpPreference -ExclusionPath "C:\path\to\proJV"
  ```

### Linux：依赖

libcurl 用系统的。已在 Ubuntu 22.04（含 WSL2）上用 CMake 3.22 + g++ 11 编译并跑通测试：

```bash
sudo apt-get install -y libcurl4-openssl-dev     # Debian / Ubuntu
# sudo dnf install libcurl-devel                 # Fedora
```

只需要补这一个包；二进制链接的其余库（glibc、libstdc++、libcurl 自身的依赖）
普通桌面系统都有。确认本机还缺什么：

```bash
ldd ./proJV_tui | grep "not found"
```

头文件通常在 multiarch 路径（`/usr/include/x86_64-linux-gnu/curl/`），CMake 自己
能找到，不用手动指定。

## 方式 B - 自己编译

需要 CMake 3.16+ 和支持 C++20 的编译器。

### Windows

```powershell
# 打开 x64 Native Tools Command Prompt（VS 2019/2022）
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
# 产物：build\Release\proJV_tui.exe
```

### Linux

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
# 产物：build/proJV_tui
```

其余依赖（FTXUI、SQLiteCpp、nlohmann/json、toml++、loguru）都在 `external/` 里，
不需要 vcpkg / conan / npm / pip。

### 自己打包

```bash
cpack --config build/CPackConfig.cmake -B dist
```

和 CI 规则一致：Windows 出 ZIP + `windows-x64`，Linux 出 TGZ + `linux-x64`。
`config.toml`、`sessions/`、`prompts/` 被显式排除，本地打包不会把 Key 和聊天记录带出去。

## 怎么确认成功

应用启动后有输入框和状态栏。如果一闪就退，请在终端里手动运行，看错误信息。

---

下一篇 -> [02 第一次使用](02-first-run.md) | 想改它 -> [开发指南](../dev-guide/README.md)
