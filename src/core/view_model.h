// proJV -- the whole-screen view model (design 08.5, D4).
//
// One question, one answer: "what should be on the screen right now?" Every
// front-end asks it and gets the same bytes back, so "the GUI behaves like the
// TUI" stops being a claim a human checks by eye and becomes a snapshot diff.
//
//     view_model::Source  -- plain data a front-end can fill (no widgets, no
//                            FTXUI, no ImGui, no runtime pointer)
//     view_model::Screen  -- the derived screen: chat rows, dock rows, status
//                            line, status bar, the open modal
//     view_model::toJson  -- the whole screen as ONE JSON object, keys sorted
//
// Two rules this file is built on:
//
//   1. Derivations that must match the TUI byte for byte live HERE, not in a
//      front-end: the bubble -> chat-row mapping (chat_view.cpp's rules, down to
//      the "-- You --" labels, the 18-line reasoning window and the 5-line tool
//      result cap), the status line, the status bar segments, the "/" menu
//      window. A second copy in a front-end is the bug this file exists to stop.
//   2. Data a front-end already has as a live object (the MCP manager, the
//      config form, the session list) is flattened by the caller into the row
//      structs below. Those panels are read-only projections; they hold no state
//      a snapshot could disagree about.
//
// The view model never reads a clock, a file or the network, so the same Source
// always serializes to the same bytes -- that is what makes it a golden test.
#pragma once

#include <string>
#include <vector>

#include "models.h"
#include "agent.h"
#include "tui/bubble_model.h"
#include "tui/theme_colors.h"

namespace view_model {

// ---- small shared helpers (pure; the TUI uses them too, so no drift) ------

// Truncate a UTF-8 string to at most `maxCols` terminal columns (CJK = 2
// columns, ASCII = 1). Appends ".." when clipped. Byte-safe on UTF-8.
std::string truncateToCols(const std::string& s, int maxCols);

// Last line of a multi-paragraph narration: a strip must show what the agent is
// doing NOW, and the first line of a narration is the oldest thing in it.
std::string lastNarrationLine(const std::string& status);

// ThemeColors field name for a pointer-to-member ("" when unknown). This is how
// a colour reference survives into JSON: the snapshot never carries an RGB
// value, only the field a front-end must resolve through the active theme.
std::string fieldName(const std::string ThemeColors::*field);

// The one sentence a front-end shows when it refuses an action because a turn is
// running (new chat, switching session or model, a role switch, an MCP toggle, a
// config save). It lives here for the same reason a label does: the reader must not
// get different words from the terminal and from the window.
const char* busyRefusal();

// ---- chat area -----------------------------------------------------------

// One bordered block of the chat area. `kind` picks the layout, the rest are
// the already-derived strings the layout draws. Field names say what is drawn,
// and toJson writes all of them, so a missing field shows up as a diff.
struct ChatRow {
    std::string kind;   // user | thinking | speaker | assistant | diff | tool | result | system | text
    std::string label;  // "-- You --", "-- proJV --", ... ("" = no label)
    std::string labelColor;  // ThemeColors field
    std::string borderColor; // ThemeColors field
    std::string speaker;     // skill role name, "" when not a role
    std::string text;        // primary body
    std::string textColor;   // ThemeColors field for `text`
    bool markdown = false;   // `text` is markdown (assistant/speaker body)
    bool bold = false;       // vote lines are bold
    std::string extra;       // merged tool results (tool rows), "" otherwise
    std::string extraColor;
    std::string reasoningHead;    // thinking block header, "" when none
    std::string reasoningBody;    // expanded body (tail window)
    std::string reasoningSummary; // collapsed one-liner
    bool reasoningExpanded = false;
    bool earlierOmitted = false; // "(earlier thinking omitted)"
    bool vote = false;
    bool dissent = false;
    bool synthetic = false; // "[auto]" user round

    // -- folding (design 08.6, batch 1) --------------------------------------
    // A tool block is one assistant message, and its results are the bulk of the
    // transcript. These fields let a front-end draw it as one line and open it on
    // demand. The TUI reads none of them (chat_view.cpp is untouched), but the
    // derivation lives HERE so a snapshot can pin it and the fold cannot become a
    // second, drifting copy of the merge rule.
    std::string key;          // stable identity ("m<msgId>:<k>"), "" when unknown
    bool collapsible = false; // the row hides something worth opening
    bool collapsed = false;   // drawn as one line because the fold is on
    std::string summary;      // the line a folded row shows (first call)
    std::string status;       // "ok" | "failed" | "pending" | ""
    std::string diffSummary;  // "+12 -3" when the row carries diffs, "" otherwise
    std::string diffText;     // the diffs themselves, shown under the results
    std::string extraFull;    // results, uncapped (`extra` stays capped for the TUI)
    int lines = 0;            // result line count, 0 = unknown
    std::string durationText; // "12.4s" of tool time, "" when unknown

    // -- nesting (design 08.6, batch 2) --------------------------------------
    // A skill run's messages from ONE role fold into a container (`kind ==
    // "skillrun"`): `summary` is its header line, `children` the role blocks it
    // holds. One level deep on purpose -- a container of containers would need a
    // second fold state and buys nothing.
    std::vector<ChatRow> children;
};

// The live, still-streaming bubble (not yet a persisted message).
struct StreamingView {
    bool visible = false;
    bool hasReasoning = false;
    std::string reasoningHead;
    std::string reasoningBody;
    bool earlierOmitted = false;
    bool hasContent = false;
    std::string contentHead; // "-- proJV --"
    std::string content;     // markdown
};

// ---- strips and docks ----------------------------------------------------

struct AgentRow {
    std::string tag;        // "[Penny]"
    std::string name;       // "Penny"
    std::string activity;   // idle | working | error
    std::string detail;     // narration tail, or "working..." when silent
    int slot = 0;           // theme palette slot for the glyph
    bool idle = true;
    int unitCols = 39;
};

struct TaskRow {
    std::string tag; // "bg#3"
    std::string activity;
    std::string detail;
    int unitCols = 34;
};

struct TodoRow {
    std::string text;
    std::string kind; // pending | done | inprogress | open
};

struct Segment {
    std::string text;
    std::string color; // ThemeColors field name
};

struct SlashRow {
    std::string name;
    std::string args;
    std::string desc;
};

// ---- modal panels --------------------------------------------------------

struct ApprovalPanel {
    bool visible = false;
    std::string title;
    std::string intro;
    std::string command;
    std::vector<std::string> targets;
    int moreTargets = 0;
    std::string explain;
    std::string undo;
    bool showAlways = false;
    std::vector<std::string> buttons;
};

struct ConfigField {
    std::string label;
    std::string value;
    std::string kind; // text | password | number | radio | check | button | note
    std::vector<std::string> options; // radio only
    int index = 0;                    // radio only
    bool checked = false;             // check only
};

struct ConfigPanel {
    bool visible = false;
    std::string title;
    std::string hint;
    std::vector<ConfigField> fields;
    std::vector<std::string> buttons;
};

struct McpRow {
    bool enabled = false;
    bool trusted = true;
    std::string name;
    std::string protocol;
    std::string state;
    int tools = 0;
};

struct McpPanel {
    bool visible = false;
    std::vector<McpRow> rows;
    int selected = 0;
    std::string purpose;
    std::string error;
    std::string hint;
    std::vector<std::string> buttons;
};

struct ListPanel {
    bool visible = false;
    std::string title;
    std::string hint;
    std::vector<std::string> rows;
    int selected = 0;
};

struct ThemeEditorPanel {
    bool visible = false;
    std::string title;
    std::vector<std::string> fields;
    int selected = 0;
    std::string field;
    std::string hex;
    std::string hint;
    std::vector<std::string> buttons;
};

struct HelpRow {
    std::string key;
    std::string desc;
};

struct HelpPanel {
    bool visible = false;
    std::string title;
    std::vector<HelpRow> rows;
    std::string hint;
    std::vector<std::string> buttons;
};

struct QuitPanel {
    bool visible = false;
    std::string title;
    std::string body;
    std::vector<std::string> buttons;
};

struct WelcomePanel {
    bool visible = false;
    std::string title;
    std::string body;
    std::string keyLabel;
    std::string button;
    std::string hint;
};

// ---- input ---------------------------------------------------------------

struct InputView {
    std::string text;
    int cursor = 0;
    std::string placeholder;
};

// ---- the snapshot input --------------------------------------------------

struct Source {
    // identity / chrome
    std::string version;
    std::string modal = "none"; // none | config | approval | mcp | open | theme | theme_editor | model | help | quit | welcome
    bool dock = true;

    // engine state
    AgentPhase phase = AgentPhase::Idle;
    bool busy = false;
    int spinnerFrame = 0;
    std::string model;
    std::string workspace;
    std::string roleName;
    int roleIndex = 0;
    int promptFiles = 0;
    int queued = 0;
    std::string skillStatus;

    // chat
    std::vector<bubble_model::Bubble> bubbles;
    // The fold key of each bubble, in the same order ("" = no key). Bubble is
    // shared with the TUI and carries no id, so the key travels beside it: the
    // caller builds "<msgId>:<k>" where it appends the derived bubbles, the one
    // place that still knows which message produced which bubble.
    std::vector<std::string> bubbleKeys;
    // Fold a tool block to one line. `expandedKeys` are the rows the user opened
    // by hand. Same shape as reasoningExpanded: state a front-end owns, handed in
    // at derivation time, so build() stays a pure function of Source.
    bool toolsCollapsed = false;
    // The same switch for skill runs: a debate is the noisiest thing in a chat.
    bool skillsCollapsed = false;
    std::vector<std::string> expandedKeys;
    bool reasoningExpanded = false;
    AgentStatus status;
    int chatWidth = 0; // cells; 0 = not measured yet

    // status line / status bar inputs
    int promptTokens = 0;
    int completionTokens = 0;
    int msgCount = 0;
    int toolCount = 0;
    int pressure = 0; // Session::PressureLevel as int: Low=0
    size_t estimatedTokens = 0;
    size_t windowTokens = 0;
    double cost = 0.0;

    // lists the caller flattens
    std::vector<AgentRow> agents;
    std::vector<TaskRow> tasks;
    std::string todoText; // TodoData::pendingTodo
    std::vector<SlashRow> slashRows;
    bool slashOpen = false;
    int slashSelected = 0;
    InputView input;

    // panels
    ApprovalPanel approval;
    ConfigPanel config;
    McpPanel mcp;
    ListPanel open;
    ListPanel modelList;
    ListPanel themeList;
    ThemeEditorPanel themeEditor;
    HelpPanel help;
    QuitPanel quit;
    WelcomePanel welcome;
};

// ---- the derived screen --------------------------------------------------

struct Screen {
    std::string version;
    std::string modal;
    bool dock = true;

    std::string phaseName;
    std::string phaseColor;
    bool busy = false;
    int spinnerFrame = 0;
    std::string model;
    std::string workspace;
    std::string roleName;
    int roleIndex = 0;
    int promptFiles = 0;
    int queued = 0;

    std::vector<ChatRow> chat;
    StreamingView streaming;

    InputView input;
    bool slashOpen = false;
    int slashSelected = 0;
    int slashFirst = 0;
    std::vector<SlashRow> slashRows;
    int slashAbove = 0;
    int slashBelow = 0;

    bool agentsEmpty = true;
    std::vector<AgentRow> agents;
    bool tasksEmpty = true;
    std::vector<TaskRow> tasks;
    std::string todoTitle;
    bool todoEmpty = true;
    std::vector<TodoRow> todo;

    std::string statusText;
    std::string statusColor;
    bool statusSpinner = false;
    std::string skillStatus;

    std::vector<Segment> statusBar;

    std::vector<Segment> keycaps;

    ApprovalPanel approval;
    ConfigPanel config;
    McpPanel mcp;
    ListPanel open;
    ListPanel modelList;
    ListPanel themeList;
    ThemeEditorPanel themeEditor;
    HelpPanel help;
    QuitPanel quit;
    WelcomePanel welcome;
};

// Derive the screen. Deterministic: same Source in, same Screen out.
Screen build(const Source& src);

// The whole screen as one JSON object (2-space indent, keys sorted, UTF-8 kept
// as UTF-8 so a diff of two snapshots is readable).
std::string toJson(const Screen& screen);

// "Idle" / "Streaming" / ... for one phase. The same function build() uses for
// Screen::phaseName, so a pane card and the status line cannot spell it two ways.
std::string phaseName(AgentPhase p);

// Turn an update_todo list into dock rows (the "[x]" / "[*]" / "[ ]" rule of
// tui/todo_view.cpp; an unknown mark is "pending").
std::vector<TodoRow> todoRows(const std::string& pendingTodo);

// The 13 keycap hints of the bottom bar, in the TUI's order.
std::vector<Segment> keycapHints();

} // namespace view_model
