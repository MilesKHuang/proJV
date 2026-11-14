// proJV -- output shaping for exec_shell.
//
// Two layers truncate, and both keep the TAIL: compilers and test runners put
// the verdict last ('2 errors generated', '50% tests passed'), so keeping the
// head hides exactly what the caller needs.
//
//   1. the platform runner keeps at most kRunnerKeepBytes (a memory guard, not
//      a display policy) -- it drops the middle and says so;
//   2. exec_shell keeps the last `tail_lines` lines, or the last 200 KB when the
//      caller asks for full_output.
//
// Pure functions only: no I/O, no globals. Unit-tested in
// tests/unit/test_shell_output.cpp.
#pragma once
#include <cstddef>
#include <string>

namespace shell_output {

// Display policy for exec_shell.
constexpr int kDefaultTailLines = 400;
constexpr int kMinTailLines = 10;
constexpr int kMaxTailLines = 2000;
constexpr size_t kFullOutputMaxBytes = 200u * 1024u; // ~50k tokens

// Runner-side memory guard: 4 MB, several times the display budget so ordinary
// big outputs survive the first layer intact.
constexpr size_t kRunnerKeepBytes = 4u * 1024u * 1024u;

// Keep the last `maxBytes` bytes of `s`, backing off one byte at a time until
// the cut lands on a UTF-8 character boundary (so multi-byte text never turns
// into mojibake). Returns true when bytes were dropped.
bool keepTailBytes(std::string& s, size_t maxBytes);

// Keep the last `maxBytes` bytes of `s`. When the dropped middle is worth
// mentioning, splice in `[... N bytes dropped from the middle ...]` and keep a
// head slice too, so the first error line survives a runaway command.
void keepTailWithMiddleNotice(std::string& s, size_t maxBytes);

// What exec_shell did to one output.
struct Applied {
    std::string text;
    bool truncated = false;  // true when anything was dropped
    size_t droppedLines = 0; // lines dropped in tail mode
    size_t totalBytes = 0;   // size before shaping
};

// Shape `raw` for the model.
//   tailLines <= 0 -> kDefaultTailLines, clamped to [kMinTailLines, kMaxTailLines]
//   fullOutput     -> keep everything up to kFullOutputMaxBytes, then truncate
// Output shorter than the limit is returned byte-identical.
Applied apply(const std::string& raw, int tailLines, bool fullOutput);

} // namespace shell_output
