#include "layout.hpp"
#include "style.hpp"

#include <algorithm>
#include <cstdlib>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace ghost {
namespace output {

namespace {

static size_t columnsFromEnv() {
    const char* raw = std::getenv("COLUMNS");
    if (!raw || !*raw) return 0;
    try {
        int value = std::stoi(raw);
        return value > 0 ? static_cast<size_t>(value) : 0;
    } catch (...) {
        return 0;
    }
}

static std::string visiblePrefix(const std::string& value, size_t width) {
    size_t visible = 0;
    std::string out;
    bool escape = false;
    for (char ch : value) {
        if (ch == '\033') {
            escape = true;
            out += ch;
            continue;
        }
        if (escape) {
            out += ch;
            if (ch == 'm') escape = false;
            continue;
        }
        if (visible >= width) break;
        out += ch;
        visible++;
    }
    return out;
}

static std::string visibleSuffix(const std::string& value, size_t width) {
    std::string plain;
    bool escape = false;
    for (char ch : value) {
        if (ch == '\033') {
            escape = true;
            continue;
        }
        if (escape) {
            if (ch == 'm') escape = false;
            continue;
        }
        plain += ch;
    }
    if (plain.size() <= width) return plain;
    return plain.substr(plain.size() - width);
}

}

size_t Layout::terminalWidth(size_t fallback) {
    size_t envWidth = columnsFromEnv();
    if (envWidth > 0) return envWidth;

#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info{};
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(out, &info)) {
        int width = info.srWindow.Right - info.srWindow.Left + 1;
        if (width > 0) return static_cast<size_t>(width);
    }
#else
    struct winsize ws {};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        return static_cast<size_t>(ws.ws_col);
    }
#endif

    return fallback;
}

size_t Layout::contentWidth(size_t fallback, size_t maxWidth) {
    return std::min(terminalWidth(fallback), maxWidth);
}

std::string Layout::padRight(const std::string& value, size_t width) {
    size_t visible = Style::visibleLength(value);
    if (visible >= width) return value;
    return value + std::string(width - visible, ' ');
}

std::string Layout::ellipsizeMiddle(const std::string& value, size_t width) {
    if (Style::visibleLength(value) <= width) return value;
    if (width <= 2) return std::string(width, '.');
    size_t left = (width - 2) / 2;
    size_t right = width - 2 - left;
    std::string result = visiblePrefix(value, left) + ".." + visibleSuffix(value, right);
    if (value.find('\033') != std::string::npos) result += "\033[0m";
    return result;
}

std::string Layout::fitCell(const std::string& value, size_t width, size_t gap) {
    return padRight(ellipsizeMiddle(value, width), width) + std::string(gap, ' ');
}

std::vector<std::string> Layout::wrapVisible(const std::string& value, size_t width) {
    std::vector<std::string> lines;
    if (width == 0) {
        lines.push_back(value);
        return lines;
    }

    std::istringstream words(value);
    std::string word;
    std::string line;
    while (words >> word) {
        size_t candidate = line.empty() ? word.size() : line.size() + 1 + word.size();
        if (!line.empty() && candidate > width) {
            lines.push_back(line);
            line.clear();
        }
        if (word.size() > width) {
            if (!line.empty()) {
                lines.push_back(line);
                line.clear();
            }
            lines.push_back(ellipsizeMiddle(word, width));
            continue;
        }
        if (!line.empty()) line += " ";
        line += word;
    }
    if (!line.empty()) lines.push_back(line);
    if (lines.empty()) lines.push_back("");
    return lines;
}

std::string Layout::keyValue(const std::string& key, const std::string& value, size_t keyWidth) {
    return "  " + padRight(Style::dim(key), keyWidth) + "  " + value + "\n";
}

}
}
