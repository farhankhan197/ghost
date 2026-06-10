#include <gtest/gtest.h>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "audit/auditor.hpp"
#include "output/report.hpp"

namespace {

std::vector<std::string> linesOf(const std::string& s) {
    std::vector<std::string> lines;
    std::stringstream ss(s);
    std::string line;
    while (std::getline(ss, line)) {
        lines.push_back(line);
    }
    return lines;
}

}

TEST(ReportCli, CodebaseAuditShowsSingleAiTouchedFilesTable) {
#ifdef _WIN32
    _putenv_s("NO_COLOR", "1");
#else
    setenv("NO_COLOR", "1", 1);
#endif

    ghost::audit::CodebaseSummary summary;
    summary.target_sha = "1234567890abcdef1234567890abcdef12345678";
    summary.total_lines = 250;
    summary.ai_lines = 40;
    summary.commit_total_lines = 80;
    summary.commit_ai_lines = 20;
    summary.files = {
        {
            "tests/integration/test_audit_with_a_very_long_filename_that_used_to_collide.cpp",
            100,
            9,
            "farhankhan197",
            "opencode/qwen3.6-plus-free",
            "opencode/qwen3.6-plus-free",
            {{"opencode", "qwen3.6-plus-free", 9}},
            true
        },
        {
            "src/checkpoint/main.cpp",
            150,
            31,
            "farhankhan197",
            "opencode/deepseek-v4-flash-free-with-an-extra-long-model-name",
            "opencode/deepseek-v4-flash-free-with-an-extra-long-model-name",
            {
                {"opencode", "deepseek-v4-flash-free-with-an-extra-long-model-name", 24},
                {"opencode", "unknown", 7}
            },
            false
        }
    };

    ghost::audit::PolicyResult policy{true, false, false, "ok"};
    auto output = ghost::output::Report::formatCodebaseCLI(summary, policy);

    std::vector<size_t> progressColumns;
    for (const auto& line : linesOf(output)) {
        auto progress = line.find("[");
        if (progress != std::string::npos && line.find("%") != std::string::npos && line.find("•") != std::string::npos) {
            progressColumns.push_back(progress);
            EXPECT_LE(line.size(), static_cast<size_t>(132)) << line;
        }
    }

    ASSERT_GE(progressColumns.size(), static_cast<size_t>(2));
    for (size_t i = 1; i < progressColumns.size(); ++i) {
        EXPECT_EQ(progressColumns[0], progressColumns[i]);
    }

    EXPECT_EQ(output.find("path  tests/integration/"), std::string::npos);
    EXPECT_EQ(output.find("agent opencode/deepseek-v4-flash-free"), std::string::npos);
    EXPECT_EQ(output.find("Changes At 12345678"), std::string::npos);
    EXPECT_EQ(output.find("(no past AI attribution)"), std::string::npos);
    EXPECT_NE(output.find("Final Diff Policy"), std::string::npos);
    EXPECT_NE(output.find("AI-Touched Files"), std::string::npos);
    EXPECT_NE(output.find("current codebase attribution"), std::string::npos);
    EXPECT_NE(output.find("tests/integration/test_audit_wit..  opencode/qwen3.6-plus-free"), std::string::npos);
}
