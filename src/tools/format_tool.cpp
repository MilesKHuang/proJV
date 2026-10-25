// format_tool.cpp -- format_file: clang-format, with a zero-dependency
// whitespace fallback for machines without clang-format installed.
#include "format_tool.h"
#include "file_tool.h"
#include "registry.h"
#include "json_utils.h"
#include "platform/iprocess_runner.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// clang-format on a single file takes tens to hundreds of milliseconds.
// This is deliberately far below kProcessTimeoutMs (30 min): a hung
// formatter must never stall a concurrent worker run.
constexpr int kFormatTimeoutMs = 30000;
constexpr int kWhitespaceTabWidth = 4;

bool readWholeFile(const std::string& path, std::string& out) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return false;
    out.assign((std::istreambuf_iterator<char>(ifs)), {});
    return true;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> v;
    std::istringstream is(s);
    std::string line;
    while (std::getline(is, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        v.push_back(line);
    }
    return v;
}

// Rough "how much changed" number for the report. Not a real diff.
int diffLines(const std::string& a, const std::string& b) {
    const std::vector<std::string> x = splitLines(a);
    const std::vector<std::string> y = splitLines(b);
    const size_t common = std::min(x.size(), y.size());
    int n = 0;
    for (size_t i = 0; i < common; ++i) if (x[i] != y[i]) ++n;
    n += static_cast<int>(x.size() > y.size() ? x.size() - y.size() : y.size() - x.size());
    return n;
}

// Nearest .clang-format, walking up from the file to the workspace root.
bool findClangFormatConfig(const fs::path& filePath, const fs::path& workspace) {
    std::error_code ec;
    fs::path stop;
    if (!workspace.empty()) stop = fs::absolute(workspace, ec);
    fs::path dir = filePath.parent_path();
    for (int guard = 0; guard < 64 && !dir.empty(); ++guard) {
        if (fs::exists(dir / ".clang-format", ec)) return true;
        if (!stop.empty() && fs::exists(stop, ec)) {
            std::error_code ec2;
            if (fs::equivalent(dir, stop, ec2)) break;
        }
        const fs::path parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    return false;
}

std::string quotePath(const std::string& p) { return "\"" + p + "\""; }

// Safe text cleanup only: no re-indent, no re-wrap, no semantic change.
// - leading tabs expand to 4 spaces (indent only; inner tabs left alone)
// - trailing spaces/tabs removed
// - exactly one final newline
// - original line ending (CRLF vs LF) preserved
std::string normalizeWhitespace(const std::string& in) {
    if (in.empty()) return in;
    const bool crlf = in.find("\r\n") != std::string::npos;
    const std::string eol = crlf ? "\r\n" : "\n";

    std::vector<std::string> lines = splitLines(in);
    while (!lines.empty() && lines.back().empty()) lines.pop_back();   // trailing blank lines

    std::ostringstream out;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& s = lines[i];
        std::string fixed;
        fixed.reserve(s.size() + 8);
        size_t k = 0;
        while (k < s.size() && (s[k] == ' ' || s[k] == '\t')) {
            if (s[k] == '\t') fixed.append(kWhitespaceTabWidth, ' ');
            else fixed.push_back(' ');
            ++k;
        }
        fixed += s.substr(k);
        while (!fixed.empty() && (fixed.back() == ' ' || fixed.back() == '\t')) fixed.pop_back();
        out << fixed;
        if (i + 1 < lines.size()) out << eol;
    }
    std::string text = out.str();
    if (!text.empty()) text += eol;
    return text;
}

} // namespace

void registerFormatTool(ToolRegistry& registry, const std::string& workspacePath,
                        IProcessRunner* procRunner, const std::string& clangFormatPath) {
    ToolDefinition def;
    def.name = "format_file";
    def.description =
        "Format one file. mode=\"clang\" (default) runs clang-format in place and uses the "
        "nearest .clang-format above the file; if the project has none it refuses and asks for "
        "an explicit style, because falling back to LLVM would restyle the whole project. "
        "mode=\"whitespace\" needs no external program and only does safe cleanup: leading tabs "
        "-> 4 spaces, trailing whitespace removed, exactly one final newline, line endings "
        "preserved. Reports how many lines changed. Never changes code semantics. Prefer "
        "edit_file for logic edits; use this for formatting only.";
    def.parameters = {
        {"path", "string", "File to format", true},
        {"mode", "string", "'clang' (default) or 'whitespace'", false},
        {"style", "string", "clang mode only: -style value (default: nearest .clang-format, else LLVM)", false}
    };

    registry.registerTool(def, [workspacePath, procRunner, clangFormatPath](
                                   const std::string& args) -> std::string {
        const std::string path = extractStringArg(args, "path");
        if (path.empty()) return "Error: Missing required 'path' argument for format_file.";

        std::string resolved;
        const std::string err = validateToolPath(path, workspacePath, resolved);
        if (!err.empty()) return err;

        std::error_code ec;
        if (!fs::is_regular_file(resolved, ec)) return "Error: Not a file: " + resolved;

        const std::string mode = lower(extractStringArg(args, "mode", "clang"));

        std::string before;
        if (!readWholeFile(resolved, before)) return "Error: Cannot read: " + resolved;

        if (mode == "whitespace") {
            const std::string after = normalizeWhitespace(before);
            if (after == before)
                return "No changes: " + resolved + " (whitespace already clean)";
            std::ofstream ofs(resolved, std::ios::binary | std::ios::trunc);
            if (!ofs) return "Error: Cannot write: " + resolved;
            ofs << after;
            const int changed = diffLines(before, after);
            return "Formatted (whitespace): " + resolved + " - " +
                   std::to_string(changed) + " line(s) changed";
        }

        if (mode != "clang")
            return "Error: Unknown mode '" + mode + "'. Use \"clang\" or \"whitespace\".";

        if (!procRunner)
            return "Error: No process runner available; use mode=\"whitespace\" instead.";

        const std::string exe = clangFormatPath.empty() ? std::string("clang-format")
                                                         : clangFormatPath;
        std::string styleArg;
        const std::string style = extractStringArg(args, "style");
        if (!style.empty()) {
            styleArg = " -style=" + style;
        } else if (findClangFormatConfig(fs::path(resolved), fs::path(workspacePath))) {
            styleArg = " -style=file";
        } else {
            // No .clang-format anywhere above the file. Falling back to LLVM here
            // would silently restyle the whole project (2-space indent, joined
            // 2-space blocks); measured on this repo that is ~35% of all lines.
            // Refuse, and say what the options are.
            return "Error: no .clang-format found above " + resolved +
                   ". Add one, pass style=\"LLVM\" (or another style) explicitly, "
                   "or call format_file with mode=\"whitespace\".";
        }

        ProcessConfig cfg;
        cfg.command = quotePath(exe) + " -i" + styleArg + " " + quotePath(resolved);
        cfg.workDir = workspacePath;
        cfg.timeoutMs = kFormatTimeoutMs;
        cfg.idleTimeoutMs = 0;   // clang-format is silent; no idle cutoff

        const ProcessResult r = procRunner->Run(cfg);
        if (r.cancelled) return "Error: clang-format cancelled";
        if (r.timedOut) return "Error: clang-format timed out after 30s";
        if (r.exitCode != 0) {
            std::string detail = r.stderr_.empty() ? r.stdout_ : r.stderr_;
            const std::string low = lower(detail);
            const bool missing = detail.empty() ||
                low.find("not recognized") != std::string::npos ||
                low.find("not found") != std::string::npos ||
                low.find("no such file") != std::string::npos;
            if (missing)
                return "Error: clang-format not found ('" + exe + "'). Install it, set "
                       "clang_format_path in config.toml, or call format_file with "
                       "mode=\"whitespace\".";
            if (detail.size() > 400) detail.resize(400);
            return "Error: clang-format failed on " + resolved + ": " + detail;
        }

        std::string after;
        if (!readWholeFile(resolved, after)) return "Error: Cannot read back: " + resolved;
        if (after == before)
            return "No changes: " + resolved + " (already formatted)";
        const int changed = diffLines(before, after);
        return "Formatted (clang-format): " + resolved + " - " +
               std::to_string(changed) + " line(s) changed";
    });
}
