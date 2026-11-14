// proJV -- config.toml round-trip for the keys the F2 panel edits.
//
// The bug class this pins down: a key exists in AppConfig, the panel does not show
// it, and the file quietly disagrees with the UI. Every key the panel writes must
// load back to the same value.
#include "doctest/doctest.h"
#include "core/config.h"
#include "client/models.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path writeToml(const std::string& leaf, const std::string& body) {
    const fs::path p = fs::temp_directory_path() / ("projv_cfg_" + leaf + ".toml");
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << body;
    out.close();
    return p;
}

} // namespace

TEST_CASE("config: approval_mode defaults to balanced when the key is absent") {
    const fs::path p =
        writeToml("auto_default", "api_key = \"k\"\nmodel = \"deepseek-v4-flash\"\n");
    AppConfig cfg;
    CHECK(loadTomlConfig(cfg, p.string()));
    CHECK(cfg.approvalMode == ApprovalMode::Balanced);
    std::error_code ec;
    fs::remove(p, ec);
}

TEST_CASE("config: approval_mode reads all three stops") {
    {
        const fs::path p = writeToml("am_strict", "api_key = \"k\"\napproval_mode = \"strict\"\n");
        AppConfig cfg;
        CHECK(loadTomlConfig(cfg, p.string()));
        CHECK(cfg.approvalMode == ApprovalMode::Strict);
        std::error_code ec;
        fs::remove(p, ec);
    }
    {
        const fs::path p = writeToml("am_loose", "api_key = \"k\"\napproval_mode = \"loose\"\n");
        AppConfig cfg;
        CHECK(loadTomlConfig(cfg, p.string()));
        CHECK(cfg.approvalMode == ApprovalMode::Loose);
        std::error_code ec;
        fs::remove(p, ec);
    }
    {
        // Nonsense must not silently become the most permissive stop.
        const fs::path p = writeToml("am_bogus", "api_key = \"k\"\napproval_mode = \"yolo\"\n");
        AppConfig cfg;
        CHECK(loadTomlConfig(cfg, p.string()));
        CHECK(cfg.approvalMode == ApprovalMode::Balanced);
        std::error_code ec;
        fs::remove(p, ec);
    }
}

TEST_CASE("config: the retired auto_approve boolean still maps") {
    {
        const fs::path p = writeToml("aa_off", "api_key = \"k\"\nauto_approve = false\n");
        AppConfig cfg;
        CHECK(loadTomlConfig(cfg, p.string()));
        CHECK(cfg.approvalMode == ApprovalMode::Strict);
        std::error_code ec;
        fs::remove(p, ec);
    }
    {
        const fs::path p = writeToml("aa_on", "api_key = \"k\"\nauto_approve = true\n");
        AppConfig cfg;
        CHECK(loadTomlConfig(cfg, p.string()));
        CHECK(cfg.approvalMode == ApprovalMode::Balanced);
        std::error_code ec;
        fs::remove(p, ec);
    }
    {
        // approval_mode wins when both are present.
        const fs::path p =
            writeToml("aa_both", "api_key = \"k\"\nauto_approve = true\napproval_mode = "
                                 "\"loose\"\n");
        AppConfig cfg;
        CHECK(loadTomlConfig(cfg, p.string()));
        CHECK(cfg.approvalMode == ApprovalMode::Loose);
        std::error_code ec;
        fs::remove(p, ec);
    }
}

TEST_CASE("config: remembered commands load as exact text") {
    const fs::path p = writeToml("grants", R"(api_key = "k"
approval_grants = ["rm -rf build", "git push"]
)");
    AppConfig cfg;
    CHECK(loadTomlConfig(cfg, p.string()));
    REQUIRE(cfg.approvalGrants.size() == 2);
    CHECK(cfg.approvalGrants[0] == "rm -rf build");
    CHECK(cfg.approvalGrants[1] == "git push");
    std::error_code ec;
    fs::remove(p, ec);
}

TEST_CASE("config: max_tokens is clamped to the legal range on load") {
    {
        const fs::path p = writeToml("mt_high", "api_key = \"k\"\nmax_tokens = 200000\n");
        AppConfig cfg;
        CHECK(loadTomlConfig(cfg, p.string()));
        CHECK(cfg.maxTokens == kMaxOutputTokens);
        std::error_code ec;
        fs::remove(p, ec);
    }
    {
        const fs::path p = writeToml("mt_low", "api_key = \"k\"\nmax_tokens = 10\n");
        AppConfig cfg;
        CHECK(loadTomlConfig(cfg, p.string()));
        CHECK(cfg.maxTokens == kMinOutputTokens);
        std::error_code ec;
        fs::remove(p, ec);
    }
    {
        // The legacy default is migrated, not clamped to the minimum.
        const fs::path p = writeToml("mt_legacy", "api_key = \"k\"\nmax_tokens = 16384\n");
        AppConfig cfg;
        CHECK(loadTomlConfig(cfg, p.string()));
        CHECK(cfg.maxTokens == kMaxOutputTokens);
        std::error_code ec;
        fs::remove(p, ec);
    }
    {
        const fs::path p = writeToml("mt_ok", "api_key = \"k\"\nmax_tokens = 32000\n");
        AppConfig cfg;
        CHECK(loadTomlConfig(cfg, p.string()));
        CHECK(cfg.maxTokens == 32000); // untouched inside the range
        std::error_code ec;
        fs::remove(p, ec);
    }
}

TEST_CASE("config: the keys the F2 panel edits all load") {
    const fs::path p = writeToml("panel", R"(api_key = "k"
model = "deepseek-v4-pro"
base_url = "https://example.test"
max_tokens = 32000
temperature = 0.7
reasoning_effort = "low"
context_window = 65536
cpp_compiler_path = "C:/cl.exe"
python_path = "C:/py.exe"
clang_format_path = "C:/cf.exe"
snapshots_max_workspace_gb = 5
snapshots_keep_days = 3
osc_status = false
approval_mode = "strict"
approval_grants = ["rm -rf build"]
theme = "nord"
)");
    AppConfig cfg;
    CHECK(loadTomlConfig(cfg, p.string()));
    CHECK(cfg.apiKey == "k");
    CHECK(cfg.model == "deepseek-v4-pro");
    CHECK(cfg.baseUrl == "https://example.test");
    CHECK(cfg.maxTokens == 32000);
    CHECK(cfg.temperature == doctest::Approx(0.7));
    CHECK(cfg.reasoningEffort == "low");
    CHECK(cfg.contextWindow == 65536);
    CHECK(cfg.cppCompilerPath == "C:/cl.exe");
    CHECK(cfg.pythonPath == "C:/py.exe");
    CHECK(cfg.clangFormatPath == "C:/cf.exe");
    CHECK(cfg.snapshotsMaxWorkspaceGb == 5);
    CHECK(cfg.snapshotsKeepDays == 3);
    CHECK_FALSE(cfg.oscStatus);
    CHECK(cfg.approvalMode == ApprovalMode::Strict);
    CHECK(cfg.themeName == "nord");
    std::error_code ec;
    fs::remove(p, ec);
}
