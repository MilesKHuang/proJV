#pragma once
#include <string>

class ToolRegistry;

// web_search / fetch_url back onto DeepSeek's official Search / Fetch APIs
// (POST /v1/tools/search, POST /v1/tools/fetch). Tool names are unchanged so
// prompt [TOOL_CALL] declarations, allowedTools filtering and stored sessions
// need no migration. The client supplies baseUrl + Bearer auth + retry.
void registerSearchTool(ToolRegistry& registry, class DeepSeekClient& client);
void registerFetchTool(ToolRegistry& registry, class DeepSeekClient& client);
