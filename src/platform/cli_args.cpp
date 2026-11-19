#include "cli_args.h"

namespace cli_args {
namespace {

std::string versionText() {
    std::string s = "proJV ";
    s += PROJV_VERSION;
    s += "\n";
    s += "native C++ terminal agent for the DeepSeek API\n";
    return s;
}

// Two groups only: install / shim and MCP. The TUI keys (F1-F11) are runtime UI,
// not command line, and listing them here would go stale the first time a panel
// moves.
std::string helpText() {
    std::string s = versionText();
    s += "\nUsage:\n";
    s += "  projv [-V|--version] [-h|--help]\n";
    s += "  projv --install <package> | --post-install | --uninstall | --purge\n";
    s += "  projv --mcp-check | --mcp-call <server> <tool> [key=value ...]\n";
    s += "  projv run -p <prompt> [--jsonl] [--no-ts] [--max-turns N]\n";
    s += "  projv                        start the TUI (default)\n";
    s += "\nOptions:\n";
    s += "  -V, --version    print the version and exit\n";
    s += "  -h, --help       print this help and exit\n";
    s += "\nInstall / shim commands (run before the TUI, never start it):\n";
    s += "  --install <pkg>  lay out a release package next to the binary\n";
    s += "  --post-install   post-install step, called by cmake --install\n";
    s += "  --uninstall      remove installed files, keep projv_files\n";
    s += "  --purge          remove installed files and projv_files\n";
    s += "\nMCP commands:\n";
    s += "  --mcp-check                  spawn every configured MCP server and exit\n";
    s += "  --mcp-call <srv> <tool> ...  call one MCP tool, print the result, exit\n";
    s += "\nExit codes: 0 ok, 1 general error, 2 bad arguments, 3 bad configuration.\n";
    s += "\nrun: one prompt, no TUI. --jsonl emits the event stream (see `run --schema`).\n";
    s += "     exit codes are 0 ok, 1 error, 2 cancelled, 3 approval refused.\n";
    return s;
}

} // namespace

std::optional<int> parseMetaArgs(const std::vector<std::string>& args, std::string& outText) {
    bool wantHelp = false;
    bool wantVersion = false;
    for (const std::string& a : args) {
        if (a == "--help" || a == "-h")
            wantHelp = true;
        else if (a == "--version" || a == "-V")
            wantVersion = true;
    }
    if (wantHelp) {
        outText = helpText();
        return 0;
    }
    if (wantVersion) {
        outText = versionText();
        return 0;
    }
    return std::nullopt;
}

// ---- `run` --------------------------------------------------------------

bool isRunCommand(const std::vector<std::string>& args) {
    return !args.empty() && args[0] == "run";
}

std::optional<RunOptions> parseRun(const std::vector<std::string>& args, std::string& err) {
    RunOptions o;
    err.clear();
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto needValue = [&](const char* what) -> const std::string* {
            if (i + 1 >= args.size()) {
                err = std::string("run: ") + what + " needs a value";
                return nullptr;
            }
            return &args[++i];
        };
        if (a == "-p" || a == "--prompt") {
            const std::string* v = needValue("--prompt");
            if (!v) return std::nullopt;
            o.prompt = *v;
        } else if (a == "--jsonl") {
            o.jsonl = true;
        } else if (a == "--no-ts") {
            o.noTs = true;
        } else if (a == "--dump-request") {
            o.dumpRequest = true;
        } else if (a == "--schema") {
            o.schema = true;
        } else if (a == "--session") {
            const std::string* v = needValue("--session");
            if (!v) return std::nullopt;
            o.sessionPath = *v;
        } else if (a == "--max-turns") {
            const std::string* v = needValue("--max-turns");
            if (!v) return std::nullopt;
            try {
                o.maxTurns = std::stoi(*v);
            } catch (...) {
                err = "run: --max-turns expects a number";
                return std::nullopt;
            }
            if (o.maxTurns < 1) o.maxTurns = 1;
        } else if (a == "--approve") {
            const std::string* v = needValue("--approve");
            if (!v) return std::nullopt;
            if (*v != "ask" && *v != "never" && *v != "always") {
                err = "run: --approve expects ask | never | always";
                return std::nullopt;
            }
            o.approve = *v;
        } else {
            err = "run: unknown argument '" + a + "'";
            return std::nullopt;
        }
    }
    if (!o.schema && o.prompt.empty()) {
        err = "run: --prompt/-p is required";
        return std::nullopt;
    }
    return o;
}

} // namespace cli_args
