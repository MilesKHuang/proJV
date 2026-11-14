// proJV -- wish_decide / wish_block registration (see wish_tool.cpp).
#pragma once
#include <mutex>

class ToolRegistry;
struct TodoData;
struct Wish;

// Both tools are always registered; Agent::getFilteredToolDefinitions() hides them
// while no wish is active, so the model never sees them outside /wish mode.
// data/todoMtx own the decisions list, wish/wishMtx own the mode state.
void registerWishTools(ToolRegistry& registry, TodoData* data, std::mutex* todoMtx, Wish* wish,
                       std::mutex* wishMtx);
