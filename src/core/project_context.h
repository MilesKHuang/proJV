// proJV -- project context builder (shared by frontends).
//
// Builds the fixed "project" block that goes at the start of a conversation:
//   buildProjectContext()  -- a compact workspace overview, so the LLM knows
//                             where files actually live instead of guessing;
//   buildAgentsMdContext() -- the repository owner's AGENTS.md, verbatim;
//   buildProjectBlock()    -- the two above merged into ONE system message.
//
// Both halves have a byte budget and both halves say so in the text when they cut
// something: a silently truncated tree reads as "that is the whole workspace",
// which is worse than a shorter tree.
#pragma once

#include <string>

// Top-level directories (with their immediate sub-directories) plus top-level
// source files, capped at 600 characters. Returns "" for a missing directory.
std::string buildProjectContext(const std::string& workspacePath);

// The workspace root's AGENTS.md wrapped in [AGENTS.MD] ... [END AGENTS.MD].
// Only the root file is read (no recursion, no CLAUDE.md fallback); a missing,
// empty or unreadable file yields an empty string so nothing is injected.
// Content is capped at 8 KiB and a truncated file says so, with byte counts.
std::string buildAgentsMdContext(const std::string& workspacePath);

// P2: one system message holding both halves, with an explicit placeholder when
// either is empty. Never returns an empty string -- the fixed prefix must have a
// constant number of system messages (see src/core/context_assembler.h).
std::string buildProjectBlock(const std::string& workspacePath);
