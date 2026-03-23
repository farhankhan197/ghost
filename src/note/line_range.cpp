#include "line_range.hpp"
#include <sstream>
#include <algorithm>
#include <stdexcept>

namespace ghost {
namespace note {

std::vector<std::string> LineRangeSet::split(const std::string& str, char delimiter) {
    std::vector<std::string> result;
    std::stringstream ss(str);
    std::string item;

    while (std::getline(ss, item, delimiter)) {
        result.push_back(item);
    }

    return result;
}

std::string LineRangeSet::trim(const std::string& str) {
    size_t start = 0;
    while (start < str.length() && (str[start] == ' ' || str[start] == '\t')) {
        start++;
    }

    size_t end = str.length();
    while (end > start && (str[end - 1] == ' ' || str[end - 1] == '\t')) {
        end--;
    }

    return str.substr(start, end - start);
}

void LineRangeSet::mergeRanges() {
    if (ranges_.empty()) return;

    std::sort(ranges_.begin(), ranges_.end());

    std::vector<Range> merged;
    for (const auto& range : ranges_) {
        if (merged.empty() || range.start > merged.back().end + 1) {
            merged.push_back(range);
        } else {
            merged.back().end = std::max(merged.back().end, range.end);
        }
    }

    ranges_ = merged;
}

LineRangeSet LineRangeSet::parse(const std::string& input) {
    LineRangeSet result;

    if (input.empty()) {
        return result;
    }

    std::vector<std::string> parts = split(input, ',');

    for (const std::string& part : parts) {
        std::string trimmed = trim(part);

        if (trimmed.empty()) {
            continue;
        }

        size_t dashPos = trimmed.find('-');

        if (dashPos == std::string::npos) {
            int line = std::stoi(trimmed);
            if (line < 1) {
                throw std::invalid_argument("Line numbers must be >= 1");
            }
            result.ranges_.push_back({line, line});
        } else {
            std::string startStr = trimmed.substr(0, dashPos);
            std::string endStr = trimmed.substr(dashPos + 1);

            int start = std::stoi(startStr);
            int end = std::stoi(endStr);

            if (start < 1 || end < 1) {
                throw std::invalid_argument("Line numbers must be >= 1");
            }

            if (start > end) {
                throw std::invalid_argument("Invalid range: start > end");
            }

            result.ranges_.push_back({start, end});
        }
    }

    result.mergeRanges();

    return result;
}

std::string LineRangeSet::toString() const {
    if (ranges_.empty()) {
        return "";
    }

    std::ostringstream oss;

    for (size_t i = 0; i < ranges_.size(); ++i) {
        if (i > 0) {
            oss << ",";
        }

        const Range& r = ranges_[i];
        if (r.start == r.end) {
            oss << r.start;
        } else {
            oss << r.start << "-" << r.end;
        }
    }

    return oss.str();
}

std::vector<int> LineRangeSet::toLines() const {
    std::vector<int> lines;

    for (const auto& range : ranges_) {
        for (int i = range.start; i <= range.end; ++i) {
            lines.push_back(i);
        }
    }

    return lines;
}

bool LineRangeSet::empty() const {
    return ranges_.empty();
}

size_t LineRangeSet::lineCount() const {
    size_t count = 0;
    for (const auto& range : ranges_) {
        count += (range.end - range.start + 1);
    }
    return count;
}

}
}