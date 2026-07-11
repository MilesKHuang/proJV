#pragma once
#include <string>
#include <vector>
#include <mutex>

class ToolRegistry;

// --- Todo tool state ------------------------------------------------
// Holds the TODO list and overview history for the current session.
// Points to Agent members so the tool can mutate agent state directly.
struct TodoData {
    std::string pendingTodo;                     // Current TODO markdown
    std::vector<std::string> pendingBriefs;      // Briefs for current round
    std::vector<std::string> overviewSummary;    // Permanent overview history

    // Latest user prompt (set by Agent::sendMessage before each API call).
    // Atomic snapshot -- only valid during the tool execution triggered by that sendMessage.
    std::string incomingUserPrompt;

    // Saved user prompt from when TODO was initialized.
    // Present as long as a TODO is active; cleared on action=done.
    std::string activeUserPrompt;
};

// Register the update_todo tool
// data and mtx are from Agent; the tool executor locks mtx when mutating data.
void registerTodoTool(ToolRegistry& registry, TodoData* data, std::mutex* mtx);

// Build system message content for 【USER REQUEST ON-GOING】 injection
std::string buildUserRequestSystemMessage(const TodoData& data);

// Build system message content for TODO injection
std::string buildTodoSystemMessage(const TodoData& data);

// Build system message content for Overview injection
std::string buildOverviewSystemMessage(const TodoData& data);
