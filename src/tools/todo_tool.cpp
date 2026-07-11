#include "todo_tool.h"
#include "registry.h"
#include "json_utils.h"
#include <algorithm>

// -- Tool description strings (moved from prompts.h) --------------------
static constexpr const char* TOOL_UPDATE_TODO_DESC =
    "Manage a TODO task list for the current session. "
    "ALWAYS call this in a complete lifecycle:\n"
    "  1. init  -- when you start a new multi-step task.\n"
    "  2. update -- after EACH sub-task finished (brief describes what was done).\n"
    "  3. done  -- when ALL sub-tasks are complete (archives to overview, clears TODO).\n"
    "IMPORTANT: Without action='done', the TODO stays 'active' in the UI and "
     "the overview is never saved. Call done at the very end of the task.\n"
     "OVERVIEW: Max 6 entries. When a 7th is added the oldest 2 auto-merge "
     "into a '[Combined: ...]' entry. Keep each brief around 50 chars.";
static constexpr const char* TOOL_PARAM_TODO_ACTION =
    "init | update | done. init starts a new TODO, update records progress, done finalizes and archives.";
static constexpr const char* TOOL_PARAM_TODO_TASKS =
    "Markdown task list. Required for init. Example:\n"
    "1. Fix Particle.h\n2. Fix Source.h\n3. Compile verification";
static constexpr const char* TOOL_PARAM_TODO_FILE_STRUCTURE = "Key file list for context. Optional, for init.";
static constexpr const char* TOOL_PARAM_TODO_TASK = "Task name or identifier to update (for action=update).";
static constexpr const char* TOOL_PARAM_TODO_STATUS = "done | in_progress | pending (for action=update).";
static constexpr const char* TOOL_PARAM_TODO_BRIEF =
    "Structured completion summary. FORMAT: '<file path>: <change> [L<line>] — <result>' "
    "Example: 'Source.h: added #include <vector> [L10], fixed copy ctor [L42] — compile OK'\n"
    "Must include: file path(s), what changed, key result. "
    "Target 100-200 chars. For action=update.";

// Todo system message prefixes (used by buildTodoSystemMessage etc.)
static constexpr const char* TODO_SYSTEM_PREFIX = "[TODO] Remaining tasks -- work through these:\n";
static constexpr const char* OVERVIEW_SYSTEM_PREFIX = "[OVERVIEW] Completed work for your reference:\n";
static constexpr const char* USER_REQUEST_SYSTEM_PREFIX =
    "[USER REQUEST ON-GOING] This is the original request you are still working on -- stay focused on it:\n";
#include <regex>
#include <set>
#include <sstream>
#include <string>

// --- Constants ------------------------------------------------------
static constexpr size_t MAX_BRIEF_LEN = 200;
// No content truncation -- LLM controls brief length via tool description.
static constexpr size_t OVERVIEW_PRUNE_THRESHOLD = 7;   // when 7th entry arrives, prune to 6
static constexpr size_t OVERVIEW_MERGE_COUNT = 2;        // merge oldest 2 entries into 1

// --- Helpers --------------------------------------------------------

static std::string truncateStr(const std::string& s, size_t maxLen) {
    if (s.size() <= maxLen) return s;
    return s.substr(0, maxLen) + "...";
}

// Extract file paths from text(s) using regex.
// Matches common source file extensions (h/cpp/rs/toml/md/etc.).
// Returns a formatted suffix like " Files: Source.h, test.cpp." or empty string.
static std::string extractFilePaths(const std::vector<std::string>& texts) {
    std::set<std::string> files;
    std::regex pathRe(R"(([A-Za-z0-9._\-\\/]+\.(?:h|cpp|rs|toml|md|json|txt|py|c|cc|cxx|hpp|java|ts|js|css|html|cmake)))");
    for (const auto& t : texts) {
        auto begin = std::sregex_iterator(t.begin(), t.end(), pathRe);
        for (auto it = begin; it != std::sregex_iterator(); ++it) {
            files.insert(it->str());
        }
    }
    if (files.empty()) return "";
    std::string result = " Files: ";
    size_t count = 0;
    for (const auto& f : files) {
        if (count > 0) result += ", ";
        if (count >= 8) { result += "..."; break; }
        result += f;
        ++count;
    }
    result += ".";
    return result;
}

// When overview exceeds max entries, merge oldest N into one
// so the LLM sees them combined and can adjust understanding.
static void pruneOverview(std::vector<std::string>& overviews) {
    if (overviews.size() < OVERVIEW_PRUNE_THRESHOLD) return;

    // Collect titles and extract file paths from the entries being merged
    std::vector<std::string> titles;
    std::vector<std::string> entryTexts;  // full text for file path extraction
    for (size_t i = 0; i < OVERVIEW_MERGE_COUNT && i < overviews.size(); ++i) {
        entryTexts.push_back(overviews[i]);
        auto closeBracket = overviews[i].find(']');
        if (closeBracket != std::string::npos) {
            // Extract title from inside brackets
            std::string title = overviews[i].substr(1, closeBracket - 1);
            titles.push_back(title);
        } else {
            // No brackets found, use first 40 chars as title
            titles.push_back(overviews[i].substr(0, 40));
        }
    }

    // Build merged entry with combined titles
    std::string merged = "[Combined: ";
    for (size_t i = 0; i < titles.size(); ++i) {
        if (i > 0) merged += " + ";
        merged += titles[i];
    }
    merged += "]";

    // Extract and append file paths from the merged entries
    merged += extractFilePaths(entryTexts);

    // Remove oldest N, insert merged at front so time order is preserved
    overviews.erase(overviews.begin(), overviews.begin() + OVERVIEW_MERGE_COUNT);
    overviews.insert(overviews.begin(), merged);
}

// --- Tool implementation --------------------------------------------

void registerTodoTool(ToolRegistry& registry, TodoData* data, std::mutex* mtx) {
    ToolDefinition def;
    def.name = "update_todo";
    def.description = TOOL_UPDATE_TODO_DESC;
    def.parameters = {
        {"action", "string", TOOL_PARAM_TODO_ACTION, true},
        {"tasks", "string", TOOL_PARAM_TODO_TASKS, false},
        {"file_structure", "string", TOOL_PARAM_TODO_FILE_STRUCTURE, false},
        {"task", "string", TOOL_PARAM_TODO_TASK, false},
        {"status", "string", TOOL_PARAM_TODO_STATUS, false},
        {"brief", "string", TOOL_PARAM_TODO_BRIEF, false}
    };

    registry.registerTool(def, [data, mtx](const std::string& args) -> std::string {
        std::string action = extractStringArg(args, "action");

        // --- action: init -------------------------------------------
        if (action == "init") {
            std::lock_guard<std::mutex> lock(*mtx);
            std::string tasks = extractStringArg(args, "tasks");
            std::string fileStructure = extractStringArg(args, "file_structure");

            data->pendingBriefs.clear();

            std::string todo;
            if (!tasks.empty()) {
                todo += "Tasks:\n" + tasks;
            }
            if (!fileStructure.empty()) {
                if (!todo.empty()) todo += "\n";
                todo += "File structure:\n" + fileStructure;
            }
            data->pendingTodo = todo;

            // Save the user prompt that triggered this TODO
            data->activeUserPrompt = data->incomingUserPrompt;

            return "TODO initialized. " + std::to_string(data->pendingBriefs.size()) +
                   " briefs in memory.";
        }

        // --- action: update -----------------------------------------
        if (action == "update") {
            std::lock_guard<std::mutex> lock(*mtx);
            std::string brief = extractStringArg(args, "brief");
            std::string task = extractStringArg(args, "task");
            std::string status = extractStringArg(args, "status");

            // Append brief (truncated)
            if (!brief.empty()) {
                data->pendingBriefs.push_back(truncateStr(brief, MAX_BRIEF_LEN));
            }

            // Update task status in TODO display
            if (!task.empty() && !data->pendingTodo.empty() && !status.empty()) {
                // Try to find the task line and mark it
                // Using ASCII-only markers to avoid font rendering issues
                std::string targetPrefix;
                if (status == "done") targetPrefix = "[x] ";
                else if (status == "in_progress") targetPrefix = "[*] ";
                else targetPrefix = "[ ] ";

                // Look for the task string in each line
                std::string result;
                std::istringstream stream(data->pendingTodo);
                std::string line;
                bool updated = false;
                while (std::getline(stream, line)) {
                    // Skip lines already marked
                    bool alreadyMarked = (line.find("[x] ") != std::string::npos ||
                                          line.find("[*] ") != std::string::npos ||
                                          line.find("[ ] ") != std::string::npos);
                    if (!alreadyMarked && line.find(task) != std::string::npos && !updated) {
                        result += targetPrefix + line + "\n";
                        updated = true;
                    } else {
                        result += line + "\n";
                    }
                }
                // Only keep the updated version if we actually modified something
                if (updated) {
                    // Remove trailing newline
                    if (!result.empty() && result.back() == '\n')
                        result.pop_back();
                    data->pendingTodo = result;
                }
            }

            return "TODO updated. " + std::to_string(data->pendingBriefs.size()) +
                   " brief(s) accumulated.";
        }

        // --- action: done -------------------------------------------
        if (action == "done") {
            std::lock_guard<std::mutex> lock(*mtx);
            // Extract title from TODO
            std::string title;
            if (!data->pendingTodo.empty()) {
                size_t firstNewline = data->pendingTodo.find('\n');
                std::string firstLine = (firstNewline == std::string::npos)
                    ? data->pendingTodo
                    : data->pendingTodo.substr(0, firstNewline);
                // Strip "Tasks:" prefix
                if (firstLine.find("Tasks:") == 0) {
                    title = firstLine.substr(6);
                    // Trim leading whitespace
                    size_t start = title.find_first_not_of(" \t\r\n");
                    if (start != std::string::npos) title = title.substr(start);
                } else {
                    title = firstLine;
                }
            }

            // Build overview from briefs (no truncation -- LLM controls length)
            std::string overview = "[" + title + "] ";
            for (size_t i = 0; i < data->pendingBriefs.size(); ++i) {
                if (i > 0) overview += ", ";
                overview += data->pendingBriefs[i];
            }

            // Extract file paths from briefs and append to overview
            overview += extractFilePaths(data->pendingBriefs);

            // Add to overview list
            data->overviewSummary.push_back(overview);

            // Prune if threshold reached
            pruneOverview(data->overviewSummary);

            // Clear briefs, TODO, and user request
            data->pendingBriefs.clear();
            data->pendingTodo.clear();
            data->activeUserPrompt.clear();

            return "TODO completed. Overview archived. Total overviews: " +
                   std::to_string(data->overviewSummary.size());
        }

        return "Error: update_todo requires an 'action' parameter ('init'/'update'/'done'). "
               "You passed action='" + action + "'. "
               "Example: update_todo(action=\"update\", task=\"Fix Source.h\", status=\"done\", brief=\"Fixed copy constructor\")";
    });
}

// --- System message builders ----------------------------------------

std::string buildTodoSystemMessage(const TodoData& data) {
    if (data.pendingTodo.empty()) return "";
    return std::string(TODO_SYSTEM_PREFIX) + data.pendingTodo;
}

std::string buildOverviewSystemMessage(const TodoData& data) {
    if (data.overviewSummary.empty()) return "";
    std::string result = OVERVIEW_SYSTEM_PREFIX;
    for (size_t i = 0; i < data.overviewSummary.size(); ++i) {
        result += std::to_string(i + 1) + ". " + data.overviewSummary[i] + "\n";
    }
    // Remove trailing newline
    if (!result.empty() && result.back() == '\n') result.pop_back();
    return result;
}

std::string buildUserRequestSystemMessage(const TodoData& data) {
    if (data.activeUserPrompt.empty()) return "";
    return std::string(USER_REQUEST_SYSTEM_PREFIX) + data.activeUserPrompt;
}
