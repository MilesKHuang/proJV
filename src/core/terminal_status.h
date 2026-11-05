#pragma once
// Terminal status reporting -- OSC 7501 "Program Status Protocol", rev 0.4.
//
// proJV is the SENDER, never the terminal. It tells the hosting terminal what
// it is doing (working / blocked / done / error) so a host that understands
// the sequence can show a tab badge, a sidebar row or a notification. A host
// that does not know the sequence drops it silently -- which is exactly why
// this can be on by default and why it is safe to fail.
//
// Spec: https://www.superlogical.com/rex/docs/build/program-status  (rev 0.4)
//
// Wire format, one sequence per status change:
//
//     ESC ] 7501 ; key=value[:key=value...] ESC \
//
// Three rules this module enforces (see docs/archive/projv-osc7501-support.md):
//  1. TTY gate -- when stdout is not a TTY every call is a no-op, so piping or
//     redirecting the TUI to a file/log stays byte-for-byte clean.
//  2. No std::cout -- POSIX write(2) / Windows WriteFile bypass CRT buffering
//     and locale conversion, so we never interleave with an FTXUI frame.
//  3. Dirty flag -- the agent thread only records the newest state; the UI
//     thread calls flushIfDirty() and performs the single write. No new thread,
//     no writing from the agent thread.
#include <cstddef>
#include <functional>
#include <string>

namespace terminal_status {

// Limits from the spec. A sequence longer than kMaxSequence is dropped whole:
// the host would discard (or mis-parse) it anyway.
constexpr size_t kMaxSequence = 4096;
constexpr size_t kMaxKey = 16;
constexpr size_t kMaxMsgPlain = 2048;
constexpr size_t kMaxMsgEncoded = 2732; // base64(2048)
constexpr size_t kMaxTitlePlain = 192;
constexpr size_t kMaxApp = 32;
constexpr size_t kMaxId = 128;

// Wire constants shared by the encoder and the tests.
constexpr const char* kStart = "\x1b]7501;";
constexpr const char* kEnd = "\x1b\\";

// Enable/disable reporting. Returns false when the status channel is not
// usable (disabled by config, PROJV_NO_OSC=1, or stdout is not a TTY).
// Disabled == every other function in this file is a no-op.
bool init(bool enabled);

// Send a final "clear" record for id=main, then go back to the no-op state.
// Safe to call twice (the second call does nothing).
void shutdown();

// Interrupted exit (Ctrl-C, window close): send a final "error" record and go
// back to the no-op state WITHOUT the "clear" that would erase the record. An
// abrupt exit must stay visible in the host's list. Safe to call twice, and
// safe to call after shutdown() (then it does nothing).
void abandon(const std::string& msg);

// True when a status report could actually reach the host.
bool available();

// Status setters. They only record "the newest state"; the write happens in
// flushIfDirty(). Calling the same state twice in a row is a no-op at the wire
// level (dedupe happens on the encoded sequence).
void working(const std::string& msg = "", int progress = -1);
void blocked(const std::string& kind, const std::string& msg);
void idle(const std::string& msg = "");
void done(const std::string& msg = "");
void error(const std::string& msg);
void clear();

// Write the pending state if it changed. Call from the UI thread (FTXUI render
// callback / streaming tick).
void flushIfDirty();

// Pure function: builds one OSC sequence, or an empty string when the input is
// invalid or the sequence would exceed kMaxSequence. Unit-tested directly.
//   state    : idle | working | blocked | done | error | clear
//   kind     : permission | question | auth (only used when state == blocked)
//   msg      : human-readable text, base64-encoded by this function
//   app, id  : application name and record id ("projv", "main")
//   progress : percent 0..100, only used when state == working; -1 = omit
std::string encode(const std::string& state, const std::string& kind, const std::string& msg,
                   const std::string& app, const std::string& id, int progress);

// --- Test seams ------------------------------------------------------
// Only the unit tests use these: they replace the TTY probe (so a test can
// pretend to be redirected) and the sink (so nothing is written to the real
// console). reset() restores the defaults and clears all pending state.
namespace test {
void setTtyProbe(std::function<bool()> probe);            // nullptr = real probe
void setSink(std::function<void(const std::string&)> s);   // nullptr = real write
void reset();
} // namespace test

} // namespace terminal_status
