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

} // namespace cli_args
