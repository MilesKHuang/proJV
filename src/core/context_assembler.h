// proJV -- the one place system messages are assembled.
//
// Before this existed, seven call sites each built their own system message and
// nobody could answer "what exactly did this request send?". The layout is now
// fixed and generated in one place:
//
//   [P0] role           persisted   projv_files/prompts/<active>.md (Agent owns it)
//   [P1] environment    persisted   buildEnvironment()  -- workspace, roots, toolchains
//   [P2] project        persisted   buildProjectBlock() -- tree + AGENTS.md, one message
//   [P3] capabilities   per turn    buildCapabilities() -- tools, skills, MCP
//   [D ] dynamic        per turn    user request / todo / overview (Agent)
//
// Why P3 is not persisted: MCP servers register at a turn boundary and skills can
// be added at any time, so a stored copy is guaranteed to go stale. It is rebuilt
// every turn, but identical input produces identical bytes -- the KV prefix cache
// keeps hitting until the tool table actually changes.
//
// P1 and P2 are always non-empty (placeholders when their content is missing).
// That is what keeps the fixed prefix at exactly three system messages, which is
// the anchor both Session::applyCompaction() and the per-turn block rely on.
#pragma once

#include "client/models.h"

#include <string>
#include <vector>

namespace ctx {

struct McpServerInfo {
    std::string name;
    std::string description;        // mcp.json "description": what it is for
    std::vector<std::string> tools; // short tool names (prefix stripped)
};

struct CapabilityInputs {
    std::vector<ToolDefinition> tools; // full registry snapshot
    std::vector<std::string> allowed;  // role allowlist; empty means "everything"
    std::vector<std::string> skills;   // installed skill folders (SkillRunner::listSkills)
    std::string skillsDir;             // where config.json describes each skill
    std::vector<McpServerInfo> mcp;    // Ready + registered servers only
};

// P1. All paths may be empty; the block still renders every line, so the message
// keeps its shape (and its byte count for a given session).
std::string buildEnvironment(const std::string& workspace, const std::string& projvFiles,
                             const std::string& shellCwd, const std::string& cppCompiler,
                             const std::string& python, const std::string& clangFormat);

// P3. Never empty: with nothing to say it still states the tool count.
std::string buildCapabilities(const CapabilityInputs& in);

// "1234 chars / ~310 tokens". P3 is never persisted, so this log line is its only
// observable output.
std::string describe(const std::string& block);

// Group label for a builtin tool name. Exposed so test_capabilities can assert
// that every registered tool is covered: adding a tool without a group is the one
// way the capability map can silently lose a capability.
const char* groupOf(const std::string& toolName);

} // namespace ctx
