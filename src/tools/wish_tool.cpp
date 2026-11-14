// proJV -- wish_decide / wish_block (the two tools that only exist under /wish).
//
// They are registered once at startup and filtered out of every request when no
// wish is active (Agent::getFilteredToolDefinitions). Registering and unregistering
// at runtime would touch ToolRegistry's bare maps from two threads; filtering cannot.
#include "wish_tool.h"

#include "json_utils.h"
#include "registry.h"
#include "core/wish.h"
#include "todo_tool.h"

#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr const char* WISH_DECIDE_DESC =
    "Record a decision you made on the user's behalf instead of asking them. In /wish "
    "mode you must pick the most defensible option and keep working; this tool is how "
    "the user later sees what you chose and why. One call per decision -- do not batch "
    "several decisions into one string.";
constexpr const char* WISH_DECIDE_DECISION = "What you decided, in one sentence";
constexpr const char* WISH_DECIDE_REASON =
    "Why this is the most defensible option, and what it costs if it is wrong";

constexpr const char* WISH_BLOCK_DESC =
    "Give up on the wish and stop. Use it only when you truly cannot proceed: a "
    "missing credential, a decision only the user can make, or a task that is "
    "impossible as specified. Never use it to ask a question you could answer by "
    "choosing.";
constexpr const char* WISH_BLOCK_REASON = "What is missing, and what you need from the user";

} // namespace

void registerWishTools(ToolRegistry& registry, TodoData* data, std::mutex* todoMtx, Wish* wish,
                       std::mutex* wishMtx) {
    {
        ToolDefinition def;
        def.name = "wish_decide";
        def.description = WISH_DECIDE_DESC;
        def.parameters = {{"decision", "string", WISH_DECIDE_DECISION, true},
                          {"reason", "string", WISH_DECIDE_REASON, false}};
        registry.registerTool(def, [data, todoMtx, wish, wishMtx](const std::string& args) {
            const std::string decision = extractStringArg(args, "decision");
            const std::string reason = extractStringArg(args, "reason");
            if (decision.empty()) return std::string("Error: Missing 'decision' argument");
            std::lock_guard<std::mutex> lk(*wishMtx);
            if (!wish->active) return std::string("Error: /wish is not active");
            ++wish->decisions;
            wish->progressedThisPass = true;
            const int n = wish->decisions;
            {
                std::lock_guard<std::mutex> tlk(*todoMtx);
                std::string line = decision;
                if (!reason.empty()) line += " -- " + reason;
                data->decisions.push_back(line);
            }
            return "[wish] decision #" + std::to_string(n) + " recorded: " + decision +
                   "\nKeep going -- the next step is in your own TODO list.";
        });
    }

    {
        ToolDefinition def;
        def.name = "wish_block";
        def.description = WISH_BLOCK_DESC;
        def.parameters = {{"reason", "string", WISH_BLOCK_REASON, true}};
        registry.registerTool(def, [wish, wishMtx](const std::string& args) {
            const std::string reason = extractStringArg(args, "reason");
            if (reason.empty()) return std::string("Error: Missing 'reason' argument");
            std::lock_guard<std::mutex> lk(*wishMtx);
            if (!wish->active) return std::string("Error: /wish is not active");
            wish->blocked = true;
            wish->blockReason = reason;
            return "[wish] blocked: " + reason +
                   "\nThe wish stops after this turn. Say what you did, what is left, and what "
                   "the user must supply.";
        });
    }
}
