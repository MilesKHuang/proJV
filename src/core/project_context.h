// proJV -- project context builder (shared by frontends).
//
// Builds two fixed system-message blocks that go at the start of a conversation:
//   buildProjectContext()  -- a compact workspace directory overview, so the LLM
//                             knows where files actually live instead of guessing;
//   buildAgentsMdContext() -- the repository owner's AGENTS.md, verbatim.
#pragma once

#include <string>

std::string buildProjectContext(const std::string& workspacePath);

// The workspace root's AGENTS.md wrapped in [AGENTS.MD] ... [END AGENTS.MD].
// Only the root file is read (no recursion, no CLAUDE.md fallback); a missing,
// empty or unreadable file yields an empty string so nothing is injected.
// Content is capped at 16 KiB and a truncated file says so in the tail.
std::string buildAgentsMdContext(const std::string& workspacePath);
