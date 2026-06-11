#include "ux.hpp"

#include "layout.hpp"
#include "style.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace ghost {
namespace output {

int Ux::percent(int part, int total) {
    if (total <= 0) return 0;
    return std::min((part * 100) / total, 100);
}

std::string Ux::verdict(const audit::PolicyResult& policy) {
    if (policy.passed) return Style::success("PASSED");
    if (policy.blocked) return Style::error("BLOCKED");
    return Style::warning("WARNING");
}

std::string Ux::thresholdText(const audit::PolicyResult& policy) {
    if (policy.threshold < 0) return "";
    std::string text = "threshold " + std::to_string(policy.threshold) + "%";
    if (!policy.action.empty()) text += " " + policy.action;
    return text;
}

std::string Ux::policySummary(const config::GhostConfig& config) {
    std::string mode = config.mode.empty() ? "custom" : config.mode;
    std::string required = config.required ? "required" : "advisory";
    std::string action = config.on_exceed.empty() ? "warn" : config.on_exceed;
    return mode + " · " + required + " · " + std::to_string(config.threshold) + "% " + action;
}

std::string Ux::trustSummary(bool notesFetched, bool fetchSkipped, const std::string& remote) {
    if (fetchSkipped) return "notes fetch skipped";
    if (notesFetched) return "notes fetched from " + remote;
    return "notes not fetched";
}

std::string Ux::checkRow(const std::string& state, const std::string& label, const std::string& detail) {
    std::string normalized = state;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    std::string renderedState;
    if (normalized == "ready" || normalized == "fixed") {
        renderedState = Style::success(normalized);
    } else if (normalized == "missing") {
        renderedState = Style::warning(normalized);
    } else if (normalized == "broken") {
        renderedState = Style::error(normalized);
    } else {
        renderedState = Style::dim(normalized);
    }

    std::ostringstream out;
    out << "  " << Layout::padRight(renderedState, 10) << Style::glow(label);
    if (!detail.empty()) out << Style::dim("  " + detail);
    out << "\n";
    return out.str();
}

std::string Ux::nextBlock(const std::vector<std::string>& lines) {
    if (lines.empty()) return "";
    std::ostringstream out;
    out << "\n  " << Style::bold(Style::violet("Next")) << "\n";
    for (const auto& line : lines) {
        out << "    " << Style::glow(line) << "\n";
    }
    return out.str();
}

}
}
