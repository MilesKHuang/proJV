#pragma once

#include <optional>
#include <string>
#include <vector>

namespace cli_args {

// Parse the two "meta" arguments that must work before anything else in main():
// they print and exit without touching the platform layer, the TUI or the
// session store.
//
// Returns the process exit code (always 0 today) and fills `outText` when the
// arguments are a meta command. Returns std::nullopt when the arguments belong
// to somebody else -- the --install family, the --mcp-check family, or no
// arguments at all -- and main() should keep going down its existing chain.
//
// Ordering is deliberate: --help wins over everything, so `projv --install
// --help` prints the general help instead of being swallowed by the install
// branch (runInstallCommand only looks for --install and ignores the rest).
//
// Pure function: no I/O, no globals except the compile-time PROJV_VERSION.
std::optional<int> parseMetaArgs(const std::vector<std::string>& args, std::string& outText);

// ---- `run`: the headless subcommand (design 08) -------------------------
//
//     projv_tui run -p "prompt" [--jsonl] [--no-ts] [--max-turns N]
//                   [--session PATH] [--approve never|always|ask]
//                   [--dump-request] [--schema]
//
// Exit codes for `run` only: 0 ok, 1 error (usage included), 2 cancelled,
// 3 an approval was needed but --approve never refused it.
struct RunOptions {
    std::string prompt;
    std::string sessionPath;     // "" = a fresh session
    std::string approve = "ask"; // ask | never | always
    int maxTurns = 1;            // counted in user submissions
    bool jsonl = false;          // one JSON object per line (events.h)
    bool noTs = false;           // drop timestamps -> byte-identical runs
    bool dumpRequest = false;    // first request body to stderr
    bool schema = false;         // print the event schema and exit
};

// True when the first argument is the `run` subcommand.
bool isRunCommand(const std::vector<std::string>& args);

// Parse `run ...`. Returns nullopt and fills `err` on an unknown or incomplete
// flag; `--schema` needs no prompt.
std::optional<RunOptions> parseRun(const std::vector<std::string>& args, std::string& err);

} // namespace cli_args
