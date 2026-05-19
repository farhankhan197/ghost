#include "style.hpp"
#include <cstdlib>
#include <sstream>
#include <vector>
#ifdef _WIN32

#include <windows.h>
#endif


namespace ghost {
namespace output {

static bool s_utf8Initialized = false;

static bool shouldHighlight() {
    const char* nocolor = std::getenv("NO_COLOR");
    if (nocolor != nullptr) return false;
    const char* term = std::getenv("TERM");
    return term != nullptr || std::getenv("WT_SESSION") != nullptr;
}

static bool shouldUnicode() {
    if (std::getenv("GHOST_FORCE_ASCII") != nullptr) return false;
    if (std::getenv("GHOST_FORCE_UTF8") != nullptr) return true;
    
    bool isWindows = false;
#ifdef _WIN32
    isWindows = true;
#endif

    if (!isWindows) return true;

    // On Windows, only enable if we are CERTAIN it's a modern terminal
    // and not just a generic environment.
    bool hasWT = std::getenv("WT_SESSION") != nullptr;
    bool hasVSCode = std::getenv("TERM_PROGRAM") != nullptr && std::string(std::getenv("TERM_PROGRAM")) == "vscode";
    
    return hasWT || hasVSCode;
}

bool Style::useColor() {
#ifdef _WIN32
    if (!s_utf8Initialized && shouldUnicode()) {
        SetConsoleOutputCP(CP_UTF8);
        s_utf8Initialized = true;
    }
#endif
    return shouldHighlight();
}


static std::string color(int code, const std::string& s) {
    if (!Style::useColor()) return s;
    return "\033[38;5;" + std::to_string(code) + "m" + s + "\033[0m";
}

std::string Style::glow(const std::string& s) { return color(231, s); }
std::string Style::purple(const std::string& s) { return color(135, s); }
std::string Style::violet(const std::string& s) { return color(141, s); }
std::string Style::blue(const std::string& s) { return color(75, s); }
std::string Style::muted(const std::string& s) { return color(242, s); }
std::string Style::dim(const std::string& s) { return "\033[2m" + color(240, s); }
std::string Style::bold(const std::string& s) { return Style::useColor() ? "\033[1m" + s + "\033[0m" : s; }

std::string Style::success(const std::string& s) { return color(42, s); }
std::string Style::warning(const std::string& s) { return color(214, s); }
std::string Style::error(const std::string& s) { return color(196, s); }

std::string Style::header(const std::string& s) {
    bool useUnicode = shouldUnicode();
    return "\n" + bold(violet(" GHOST ")) + " " + (useUnicode ? muted("▫") : muted("|")) + " " + glow(s) + "\n";
}



std::string Style::subHeader(const std::string& s) {
    return "\n" + bold(blue("  " + s)) + "\n";
}

std::string Style::label(const std::string& s) {
    return dim("  " + s);
}

std::string Style::horizontalRule() {
    if (!useColor()) return "  ----------------------------------------";
    
    bool useUnicode = shouldUnicode();

    std::string s = "  ";
    for (int i = 0; i < 40; i++) {
        s += "\033[2m\033[38;5;141m";
        if (!useUnicode) {
            s += (i % 4 == 0) ? "+" : "-";
        } else {
            s += (i % 4 == 0) ? "▫" : "─";
        }
        s += "\033[0m";
    }
    return s;
}


std::string Style::progressBar(int current, int total, int width) {
    if (total <= 0) return dim("[ - ]");
    float pct = (float)current / total;
    int filled = (int)(pct * width);
    
    bool useUnicode = shouldUnicode();

    std::ostringstream oss;
    oss << dim("[");
    for (int i = 0; i < width; i++) {
        if (i < filled) {
            oss << violet(useUnicode ? "█" : "#");
        } else {
            if (useUnicode) {
                oss << "\033[38;5;236m" << "░" << "\033[0m";
            } else {
                oss << dim("-");
            }
        }
    }
    oss << dim("]") << " " << glow(std::to_string((int)(pct * 100)) + "%");
    return oss.str();
}





std::string Style::ghostLogo() {
    bool u = shouldUnicode();
    return violet("  .---.  \n") +
           violet(" / ") + glow(u ? "o o" : "0 0") + violet(" \\ \n") +
           violet(" |  ") + glow("^") + violet("  | \n") +
           violet(" '---'  \n");
}

std::vector<std::string> Style::mascot() {
    bool u = shouldUnicode();
    if (u) {
        return {
            violet("  ╭───╮  "),
            violet("  │") + glow("◕ ◕") + violet("│  "),
            violet("  ╰───╯  "),
            violet("   ") + dim("▿") + "     "
        };
    } else {
        return {
            "  .---.  ",
            " | o o | ",
            "  '---'  ",
            "   ' '   "
        };
    }
}


std::string Style::spinner(int frame) {
    const char* frames[] = {"▖", "▘", "▝", "▗"};
    const char* pulse[] = {"░", "▒", "▓", "█", "▓", "▒"};
    bool u = shouldUnicode();
    if (u) return violet(frames[frame % 4]);
    return violet("*");
}


}
}
