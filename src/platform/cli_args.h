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

} // namespace cli_args
