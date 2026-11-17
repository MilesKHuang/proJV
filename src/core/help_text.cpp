// proJV -- the one command table (see help_text.h).
#include "help_text.h"

namespace help_text {
namespace {

// A markdown table cell cannot contain a raw '|': "/end_task <id> | all" would
// split the row in two. Escaped at render time so the menu keeps the readable
// form while the chat table stays well formed.
std::string escapeCell(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (const char c : s) {
        if (c == '|')
            out += "\\|";
        else
            out += c;
    }
    return out;
}

} // namespace

const std::vector<CmdHint>& commands() {
    // One entry per handler that actually exists:
    //   handleQuickCommand (agent.cpp) -> /clear /help /undo /undo! /restore
    //   TuiApp::sendMessage            -> /compress /wish /end_task /approval /workspace
    //   skill_trigger                  -> /skill list, /<skill> <topic>
    // A menu entry that does nothing when picked is worse than a missing one.
    static const std::vector<CmdHint> table = {
        {"/help", "", "Show the command help in chat"},
        {"/clear", "", "Clear the session (keeps the session file)"},
        {"/compress", "", "Summarize early messages to free context"},
        {"/undo", "", "Undo the file changes of the last turn"},
        {"/undo!", "", "Undo, discarding later hand edits too"},
        {"/restore", "<turn> confirm", "Roll the workspace back to before turn N"},
        {"/wish", "<what you want built>",
         "Decide it yourself, keep going until the TODO is empty"},
        {"/wish cancel", "", "Stop the wish; the TODO list is kept"},
        {"/end_task", "<id> | all", "Stop a background task (kills the process tree)"},
        {"/approval", "", "Show the approval mode and the remembered commands"},
        {"/approval clear", "", "Forget every remembered command"},
        {"/workspace", "", "Print the current workspace path"},
        {"/skill", "list", "List installed skills"},
    };
    return table;
}

const std::vector<KeyHint>& quickHints() {
    // Order: the discovery paths first, then the keys whose behaviour is not
    // obvious, then two commands that reward reading. Everything the hint bar at
    // the bottom already spells out (F1..F10, Enter, Esc=quit) is left out on
    // purpose -- repeating it is what made the first version a wall of text.
    static const std::vector<KeyHint> table = {
        {"/", "list every command (Enter fills it in; it does not send)"},
        {"@path", "attach an image -- Flash models only"},
        {"Esc", "cancel a turn / clear the line / ask before quitting"},
        {"Ctrl+C", "does nothing; the terminal uses it to copy a selection"},
        {"paste", "multi-line paste stays in the box (newlines become spaces)"},
        {"/undo", "take back the file changes of the last turn"},
        {"/wish", "let it decide the open questions and finish the TODO"},
    };
    return table;
}

std::string commandHelp() {
    std::string out =
        "### Quick Commands\n\n| Command | Description |\n|---------|-------------|\n";
    for (const auto& c : commands()) {
        std::string name = c.name;
        if (!c.args.empty()) name += " " + c.args;
        out += "| `" + escapeCell(name) + "` | " + escapeCell(c.desc) + " |\n";
    }
    out += "Type `/` in the input box to see this list and pick from it.\n";
    out += "\nproJV v" PROJV_VERSION " - native C++ DeepSeek AI agent (FTXUI).\n";
    return out;
}

} // namespace help_text
