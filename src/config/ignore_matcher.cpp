#include "ignore_matcher.hpp"

namespace ghost {
namespace config {

namespace {

std::string normalizeSlashes(std::string value) {
    for (char& c : value) {
        if (c == '\\') c = '/';
    }
    while (value.rfind("./", 0) == 0) {
        value = value.substr(2);
    }
    return value;
}

}

bool IgnoreMatcher::matches(const std::string& filePath, const std::vector<std::string>& patterns) {
    std::string file = normalizeSlashes(filePath);
    for (const auto& rawPattern : patterns) {
        if (rawPattern.empty()) continue;
        std::string pattern = normalizeSlashes(rawPattern);

        if (pattern.back() == '/') {
            std::string dirPrefix = pattern.substr(0, pattern.size() - 1);
            if (file == dirPrefix ||
                file.rfind(dirPrefix + "/", 0) == 0 ||
                file.find("/" + dirPrefix + "/") != std::string::npos) {
                return true;
            }
            continue;
        }

        if (pattern.front() == '*') {
            std::string suffix = pattern.substr(1);
            if (file.size() >= suffix.size() &&
                file.compare(file.size() - suffix.size(), suffix.size(), suffix) == 0) {
                return true;
            }
            continue;
        }

        if (file == pattern || file.find(pattern) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}
}
