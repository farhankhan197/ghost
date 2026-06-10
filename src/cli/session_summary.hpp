#ifndef GHOST_CLI_SESSION_SUMMARY_HPP
#define GHOST_CLI_SESSION_SUMMARY_HPP

#include "note/line_range.hpp"
#include "persist/db.hpp"

#include <string>
#include <vector>

namespace ghost {
namespace cli {
namespace SessionSummary {

std::vector<std::string> files(const std::string& jsonData, const std::string& repoRoot);
note::LineRangeSet rangesForFile(const std::string& jsonData, const std::string& filePath, const std::string& repoRoot);
void normalizePending(std::vector<persist::Session>& sessions, const std::string& repoRoot);

}
}
}

#endif
