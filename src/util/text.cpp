#include "text.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace ghost {
namespace util {

std::string Text::trim(const std::string& value) {
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) {
        start++;
    }
    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        end--;
    }
    return value.substr(start, end - start);
}

std::string Text::lower(const std::string& value) {
    std::string out = value;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

std::vector<std::string> Text::splitLines(const std::string& value) {
    std::vector<std::string> lines;
    std::istringstream stream(value);
    std::string line;
    while (std::getline(stream, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }
        lines.push_back(line);
    }
    if (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
        return lines;
    }
    return lines;
}

std::optional<int> Text::parseInt(const std::string& value) {
    std::string trimmed = trim(value);
    if (trimmed.empty()) return std::nullopt;
    size_t consumed = 0;
    try {
        int parsed = std::stoi(trimmed, &consumed);
        if (consumed != trimmed.size()) return std::nullopt;
        return parsed;
    } catch (...) {
        return std::nullopt;
    }
}

}
}
