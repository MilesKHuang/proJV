// proJV -- user-level install / shim management (docs/projv-install.md).
//
// The goal is one command and no administrator rights:
//
//     cmake --build build --config Release --target install   (runs --post-install)
//     proJV_tui.exe --install                                  (from a downloaded ZIP)
//
// Everything that decides *what* to do is a pure function so the unit tests can
// pin the copy semantics, the shim text and the PATH rules without touching the
// real profile. Only the run_* entry points touch the filesystem.
#pragma once

#include <string>
#include <vector>

namespace install_shim {

// The marker every shim carries. `--uninstall` deletes a file only when this is
// inside it, so a foreign `projv.cmd` that the user wrote is never removed.
constexpr const char* kMarker = "projv-shim v1";

// What to do with one file of the package when installing over an existing tree.
enum class CopyRule {
    Overwrite,      // the binary and the docs: a new version must win
    KeepIfMissing,  // theme / pytool / skills: never clobber user edits
    Never           // config.toml, sessions, logs, prompts: user data
};

// The copy decision for a path relative to the package root, using forward
// slashes ("projv_files/theme/a.json"). Pure.
CopyRule copy_rule(const std::string& relativePath);

// True for the directories whose *contents* are copied under KeepIfMissing.
bool is_keep_if_missing_tree(const std::string& relativePath);

// The Windows shim: four lines, absolute path, forwards the exit code. Pure.
// `name` is "projv" or "projv_tui"; `exePath` is the absolute binary path.
std::string shim_cmd(const std::string& name, const std::string& exePath);

// The POSIX fallback shim (used when a symlink cannot be created). Pure.
std::string shim_sh(const std::string& exePath);

// True when `text` was written by us.
bool has_marker(const std::string& text);

// PATH helpers. `merge_path` appends only once and keeps the existing order.
bool path_contains(const std::string& existing, const std::string& dir);
std::string merge_path(const std::string& existing, const std::string& dir);

// True when `child` is `parent` or lives inside it. Used to refuse --purge when
// the running binary is inside the directory it is about to delete.
bool is_inside(const std::string& child, const std::string& parent);

// The self-check block printed after an install (docs/projv-install.md D.4).
// Pure: the paths and the PATH verdict are inputs, not lookups.
std::string summary(const std::string& installed, const std::string& shimPath,
                    bool shimInPath, const std::string& resolvable,
                    const std::string& next, const std::string& undo);

// --- the filesystem entry points -------------------------------------
struct Options {
    std::string home;      // install directory (PROJV_HOME overrides the default)
    std::string shimDir;   // where the shims go (PROJV_SHIM_DIR overrides)
    std::string exePath;   // absolute path of the running binary
    std::string exeDir;    // directory of the running binary == package root
    std::string packageArg; // optional explicit package root for --install
};

// Defaults, honouring PROJV_HOME / PROJV_SHIM_DIR and the environment.
std::string default_home();
std::string default_shim_dir();
Options current_options();

// Each returns a process exit code (0 = success) and prints its own report.
// `install` copies the package tree then writes the shims; `post_install`
// assumes CMake already laid the files down and only writes the shims.
int run_install(const Options& opt);
int run_post_install(const Options& opt);
int run_uninstall(const Options& opt);
int run_purge(const Options& opt);

// Human-readable output collector, so tests can assert on the report text.
struct Report {
    std::vector<std::string> lines;

    void add(const std::string& line);
    std::string text() const;
};

} // namespace install_shim
