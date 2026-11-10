// proJV -- user-level install / shim management. See install_shim.h.
#include "platform/install_shim.h"

#include "debug_log.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace install_shim {
namespace {

std::string lowerCopy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string normalizeSlashes(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

// CMake's install rule drops `proJV_tui${CMAKE_EXECUTABLE_SUFFIX}`, so the shim
// must point at that same file. Spelling ".exe" unconditionally wrote a Linux
// shim that exec'd a path nothing had created.
const char* installedExeName() {
#ifdef _WIN32
    return "proJV_tui.exe";
#else
    return "proJV_tui";
#endif
}

bool startsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

// Can we create a file in `dir`? The shim directory choice is "the first
// writable directory already on PATH", and being writable is the happy path.
bool isWritableDir(const std::string& dir) {
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) return false;
    const fs::path probe = fs::path(dir) / ".projv-write-probe";
    {
        std::ofstream out(probe, std::ios::binary);
        if (!out) return false;
    }
    fs::remove(probe, ec);
    return !ec;
}

bool isDirOnPath(const std::string& dir) {
    const char* pathVar = std::getenv("PATH");
    if (!pathVar || !*pathVar) return false;
    return path_contains(pathVar, dir);
}

std::string readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::string();
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool writeFile(const fs::path& path, const std::string& text) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out << text;
    return out.good();
}

bool copyOverwrite(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    return !ec;
}

std::string envOr(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    if (!value || !*value) return fallback;
    return std::string(value);
}

// Write one shim. Returns the path written, or "" on failure.
std::string writeShim(const std::string& dir, const std::string& name, const std::string& exePath) {
    std::error_code ec;
    fs::create_directories(dir, ec);
#ifdef _WIN32
    const fs::path target = fs::path(dir) / (name + ".cmd");
    if (!writeFile(target, shim_cmd(name, exePath))) return std::string();
    return target.string();
#else
    const fs::path link = fs::path(dir) / name;
    fs::remove(link, ec);
    fs::create_symlink(exePath, link, ec);
    if (!ec) return link.string();
    // Fallback: a shell script that execs the real binary, so signals and the
    // exit code reach the TUI unchanged.
    if (!writeFile(link, shim_sh(exePath))) return std::string();
    ::chmod(link.c_str(), 0755);
    return link.string();
#endif
}

} // namespace

void Report::add(const std::string& line) {
    lines.push_back(line);
}

std::string Report::text() const {
    std::string out;
    for (const auto& line : lines) {
        out += line;
        out += '\n';
    }
    return out;
}

CopyRule copy_rule(const std::string& relativePath) {
    const std::string p = lowerCopy(normalizeSlashes(relativePath));
    // User data: never read, never written.
    if (p == "projv_files/config.toml" || startsWith(p, "projv_files/sessions") ||
        startsWith(p, "projv_files/logs") || startsWith(p, "projv_files/prompts"))
        return CopyRule::Never;
    // The binary and the docs: an upgrade must replace them.
    if (p == "projv_tui.exe" || p == "projv_tui" || p == "readme.md" || p == "readme_zh.md" ||
        p == "license")
        return CopyRule::Overwrite;
    // Only the runtime payload ships: a build directory contains .lib / .pdb /
    // CMake junk that must never land in a user's install.
    if (startsWith(p, "projv_files/")) return CopyRule::KeepIfMissing;
    return CopyRule::Never;
}

std::string shim_cmd(const std::string&, const std::string& exePath) {
    std::ostringstream out;
    out << "@echo off\r\n";
    out << "rem " << kMarker
        << " -- created by proJV --install; `proJV_tui --uninstall` removes it\r\n";
    out << "\"" << exePath << "\" %*\r\n";
    out << "exit /b %ERRORLEVEL%\r\n";
    return out.str();
}

std::string shim_sh(const std::string& exePath) {
    std::ostringstream out;
    out << "#!/bin/sh\n";
    out << "# " << kMarker
        << " -- created by proJV --install; `proJV_tui --uninstall` removes it\n";
    out << "exec \"" << exePath << "\" \"$@\"\n";
    return out.str();
}

bool has_marker(const std::string& text) {
    return text.find(kMarker) != std::string::npos;
}

bool path_contains(const std::string& existing, const std::string& dir) {
    if (dir.empty()) return false;
    std::string wanted = normalizeSlashes(dir);
    while (!wanted.empty() && wanted.back() == '/')
        wanted.pop_back();
    std::string current;
    std::stringstream ss(existing);
#ifdef _WIN32
    const char separator = ';';
#else
    const char separator = ':';
#endif
    while (std::getline(ss, current, separator)) {
        std::string candidate = normalizeSlashes(current);
        while (!candidate.empty() && candidate.back() == '/')
            candidate.pop_back();
        if (lowerCopy(candidate) == lowerCopy(wanted)) return true;
    }
    return false;
}

std::string merge_path(const std::string& existing, const std::string& dir) {
    if (dir.empty() || path_contains(existing, dir)) return existing;
    if (existing.empty()) return dir;
#ifdef _WIN32
    return existing + ";" + dir;
#else
    return existing + ":" + dir;
#endif
}

bool is_inside(const std::string& child, const std::string& parent) {
    if (child.empty() || parent.empty()) return false;
    std::error_code ec;
    const std::string c = normalizeSlashes(fs::weakly_canonical(child, ec).string());
    std::error_code ec2;
    const std::string p = normalizeSlashes(fs::weakly_canonical(parent, ec2).string());
    if (ec || ec2) return false;
    if (lowerCopy(c) == lowerCopy(p)) return true;
    std::string prefix = lowerCopy(p);
    if (!prefix.empty() && prefix.back() != '/') prefix += '/';
    return startsWith(lowerCopy(c), prefix);
}

std::string summary(const std::string& installed, const std::string& shimPath, bool shimInPath,
                    const std::string& resolvable, const std::string& next,
                    const std::string& undo) {
    std::ostringstream out;
    out << "installed : " << installed << "\n";
    out << "shim      : " << shimPath << "  (in PATH: " << (shimInPath ? "yes" : "no") << ")\n";
    out << "projv     : resolves to " << (resolvable.empty() ? "(not on PATH yet)" : resolvable)
        << "\n";
    out << "next      : " << next << "\n";
    out << "undo      : " << undo << "\n";
    return out.str();
}

std::string default_home() {
    const std::string override = envOr("PROJV_HOME", "");
    if (!override.empty()) return override;
#ifdef _WIN32
    const std::string local = envOr("LOCALAPPDATA", "");
    if (!local.empty()) return (fs::path(local) / "Programs" / "projv").string();
    return "C:/ProgramData/projv";
#else
    const std::string home = envOr("HOME", "");
    if (!home.empty()) return (fs::path(home) / ".local" / "share" / "projv").string();
    return "/usr/local/share/projv";
#endif
}

std::string default_shim_dir() {
    const std::string override = envOr("PROJV_SHIM_DIR", "");
    if (!override.empty()) return override;
#ifdef _WIN32
    // `%LOCALAPPDATA%\Microsoft\WindowsApps` is already on PATH and writable by
    // the current user, which is why it is preferred. When it is not writable we
    // fall back to a bin directory beside the install and tell the user to add
    // it once (a PATH change cannot take effect in the current shell).
    const std::string local = envOr("LOCALAPPDATA", "");
    if (!local.empty()) {
        const std::string apps = (fs::path(local) / "Microsoft" / "WindowsApps").string();
        if (isWritableDir(apps)) return apps;
    }
    return (fs::path(default_home()) / "bin").string();
#else
    const std::string home = envOr("HOME", "");
    if (!home.empty()) {
        const std::string bin = (fs::path(home) / ".local" / "bin").string();
        if (isWritableDir(bin)) return bin;
    }
    return (fs::path(default_home()) / "bin").string();
#endif
}

Options current_options() {
    Options opt;
    opt.home = default_home();
    opt.shimDir = default_shim_dir();
#ifdef _WIN32
    wchar_t buffer[MAX_PATH] = {0};
    const DWORD n = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (n > 0) {
        const fs::path exe(buffer);
        opt.exePath = exe.string();
        opt.exeDir = exe.parent_path().string();
    }
#else
    std::error_code ec;
    const fs::path self = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) {
        opt.exePath = self.string();
        opt.exeDir = self.parent_path().string();
    }
#endif
    return opt;
}

int run_install(const Options& optIn) {
    Options opt = optIn;
    const std::string package = opt.packageArg.empty() ? opt.exeDir : opt.packageArg;
    Report report;

    if (package.empty()) {
        report.add("error: cannot tell where the package is; pass a directory");
        std::printf("%s", report.text().c_str());
        return 2;
    }
    if (is_inside(opt.home, package) && opt.home != package) {
        report.add("error: the install directory is inside the package directory");
        std::printf("%s", report.text().c_str());
        return 2;
    }

    std::error_code ec;
    fs::create_directories(opt.home, ec);
    if (ec) {
        report.add("error: cannot create " + opt.home + ": " + ec.message());
        std::printf("%s", report.text().c_str());
        return 1;
    }

    size_t copied = 0, kept = 0;
    for (auto it = fs::recursive_directory_iterator(
             package, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const fs::path rel = fs::relative(it->path(), package, ec);
        if (ec) continue;
        const std::string relText = normalizeSlashes(rel.string());
        const std::string lowerRel = lowerCopy(relText);
        if (startsWith(lowerRel, "build/") || lowerRel.find("/.git/") != std::string::npos)
            continue;

        const CopyRule rule = copy_rule(relText);
        if (rule == CopyRule::Never) continue;
        const fs::path target = fs::path(opt.home) / rel;
        if (rule == CopyRule::KeepIfMissing && fs::exists(target, ec)) {
            ++kept;
            continue;
        }
        if (copyOverwrite(it->path(), target))
            ++copied;
        else
            report.add("warning: could not copy " + relText);
    }

    const fs::path installedExe = fs::path(opt.home) / installedExeName();
    const std::string shim = writeShim(opt.shimDir, "projv", installedExe.string());
    const std::string shim2 = writeShim(opt.shimDir, "projv_tui", installedExe.string());
    if (shim.empty() || shim2.empty())
        report.add("warning: the files are installed but the `projv` command was not created");

    const bool inPath = isDirOnPath(opt.shimDir);
    if (!inPath) {
        report.add("note: " + opt.shimDir + " is not on PATH; add it once, then reopen the shell");
    }

    report.add("copied " + std::to_string(copied) + " file(s), kept " + std::to_string(kept) +
               " existing file(s)");
    report.add(
        summary(opt.home, shim, inPath, shim, "cd <project> && projv", "projv_tui --uninstall"));
    std::printf("%s", report.text().c_str());
    return shim.empty() ? 1 : 0;
}

int run_post_install(const Options& optIn) {
    Options opt = optIn;
    if (opt.home.empty()) opt.home = default_home();
    Report report;
    const fs::path exe = fs::path(opt.home) / installedExeName();
    if (!fs::exists(exe))
        report.add("warning: " + exe.string() + " not found; run --install from the package");
    const std::string shim = writeShim(opt.shimDir, "projv", exe.string());
    const std::string shim2 = writeShim(opt.shimDir, "projv_tui", exe.string());
    if (shim.empty() || shim2.empty()) report.add("warning: the `projv` command was not created");
    report.add(summary(opt.home, shim, isDirOnPath(opt.shimDir), shim, "cd <project> && projv",
                       "projv_tui --uninstall"));
    std::printf("%s", report.text().c_str());
    return shim.empty() ? 1 : 0;
}

int run_uninstall(const Options& optIn) {
    Report report;
    int removed = 0, refused = 0;
    for (const char* name : {"projv", "projv_tui"}) {
#ifdef _WIN32
        const fs::path target = fs::path(optIn.shimDir) / (std::string(name) + ".cmd");
#else
        const fs::path target = fs::path(optIn.shimDir) / name;
#endif
        std::error_code ec;
        if (!fs::exists(target, ec)) continue;
        const bool symlink = fs::is_symlink(target, ec);
        if (!symlink && !has_marker(readFile(target))) {
            report.add("refused: " + target.string() + " was not created by proJV -- leaving it");
            ++refused;
            continue;
        }
        fs::remove(target, ec);
        if (!ec) {
            ++removed;
            report.add("removed: " + target.string());
        }
    }
    if (removed == 0 && refused == 0) report.add("nothing to remove");
    report.add("kept your data in " + optIn.home + " (use --purge to delete it)");
    std::printf("%s", report.text().c_str());
    return refused > 0 ? 1 : 0;
}

int run_purge(const Options& optIn) {
    Report report;
    std::error_code ec;
    const fs::path cwd = fs::current_path(ec);
    if (!ec && is_inside(cwd.string(), optIn.home)) {
        report.add("refused: this directory is the install directory; run it from elsewhere");
        std::printf("%s", report.text().c_str());
        return 2;
    }
    if (is_inside(optIn.exePath, optIn.home)) {
        report.add("refused: the running binary is inside " + optIn.home);
        std::printf("%s", report.text().c_str());
        return 2;
    }
    const auto removed = fs::remove_all(optIn.home, ec);
    if (ec) {
        report.add("error: could not remove " + optIn.home + ": " + ec.message());
        std::printf("%s", report.text().c_str());
        return 1;
    }
    report.add("removed " + std::to_string(removed) + " item(s) from " + optIn.home);
    report.add("the `projv` command may still exist; run --uninstall to remove it");
    std::printf("%s", report.text().c_str());
    return 0;
}

} // namespace install_shim
