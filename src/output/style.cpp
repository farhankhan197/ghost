#include "style.hpp"
#include <cstdlib>
#include <sstream>
#include <chrono>
#include <iostream>
#ifdef _WIN32

#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
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
    if (std::getenv("NO_COLOR") != nullptr) return false;
    
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

static bool stderrIsTty() {
#ifdef _WIN32
    return _isatty(_fileno(stderr)) != 0;
#else
    return isatty(STDERR_FILENO) != 0;
#endif
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

static std::string faint(const std::string& s) {
    return color(240, s);
}

std::string Style::glow(const std::string& s) { return color(231, s); }
std::string Style::purple(const std::string& s) { return color(135, s); }
std::string Style::violet(const std::string& s) { return color(141, s); }
std::string Style::blue(const std::string& s) { return color(75, s); }
std::string Style::muted(const std::string& s) { return color(248, s); }
std::string Style::dim(const std::string& s) {
    if (!Style::useColor()) return s;
    return "\033[38;5;245m" + s + "\033[0m";
}
std::string Style::bold(const std::string& s) { return Style::useColor() ? "\033[1m" + s + "\033[0m" : s; }

std::string Style::success(const std::string& s) { return color(42, s); }
std::string Style::warning(const std::string& s) { return color(214, s); }
std::string Style::error(const std::string& s) { return color(196, s); }

std::string Style::header(const std::string& s) {
    bool useUnicode = shouldUnicode();
    return "\n" + bold(violet("Ghost")) + " " + (useUnicode ? muted("·") : muted("-")) + " " + glow(s) + "\n";
}



std::string Style::subHeader(const std::string& s) {
    return "\n" + bold(violet("  " + s)) + "\n";
}

std::string Style::label(const std::string& s) {
    return dim("  " + s);
}

std::string Style::horizontalRule() {
    if (!useColor()) return "  --------------------------------";
    
    bool useUnicode = shouldUnicode();

    std::string s = "  ";
    for (int i = 0; i < 32; i++) {
        s += dim(useUnicode ? "─" : "-");
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
            oss << faint(useUnicode ? "·" : "-");
        }
    }
    oss << dim("]") << " " << glow(std::to_string((int)(pct * 100)) + "%");
    
    return oss.str();
}

std::string Style::animatedProgressBar(int current, int total, int width, int steps) {
    if (total <= 0) return dim("[ - ]");
    float pct = (float)current / total;
    int filled = (int)(pct * width);

    bool useUnicode = shouldUnicode();
    std::string result;

    for (int s = 0; s <= steps; s++) {
        int currentFilled = (int)((float)filled * s / steps);
        std::ostringstream oss;
        oss << dim("|");
        for (int i = 0; i < width; i++) {
            if (i < currentFilled) {
                oss << violet(useUnicode ? "█" : "#");
            } else {
                oss << faint(useUnicode ? "░" : "-");
            }
        }
        oss << dim("|") << " " << glow(std::to_string((int)(pct * 100)) + "%");
        result = oss.str();
    }
    return result;
}

std::string Style::spinner(int frame) {
    const char* frames[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
    const char* ascii[] = {"-", "\\", "|", "/"};
    bool u = shouldUnicode();
    if (u) return violet(frames[frame % 10]);
    return violet(ascii[frame % 4]);
}

void AnimatedSpinner::start(const std::string& message, bool enabled) {
    if (m_running) stop();
    m_enabled = enabled && stderrIsTty();
    if (!m_enabled) return;
    m_message = message;
    m_running = true;
    m_frame = 0;
    std::cerr << "\033[?25l";
    std::cerr.flush();
    m_cursorHidden = true;
    m_thread = std::thread(&AnimatedSpinner::render, this);
}

void AnimatedSpinner::update(const std::string& message) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_message = message;
}

void AnimatedSpinner::stop() {
    if (!m_running) return;
    m_running = false;
    if (m_thread.joinable()) m_thread.join();
    std::cerr << "\r\033[K";
    if (m_lastWidth > 0 && !Style::useColor()) {
        std::cerr << "\r" << std::string(m_lastWidth + 4, ' ') << "\r";
    }
    if (m_cursorHidden) {
        std::cerr << "\033[?25h";
        m_cursorHidden = false;
    }
    std::cerr.flush();
}

AnimatedSpinner::~AnimatedSpinner() {
    stop();
}

void AnimatedSpinner::render() {
    while (m_running) {
        std::string frame = Style::spinner(m_frame);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            std::string line = " " + frame + " " + Style::dim(m_message);
            m_lastWidth = Style::visibleLength(line);
            std::cerr << "\r\033[K" << line;
            std::cerr.flush();
        }
        m_frame++;
        std::this_thread::sleep_for(std::chrono::milliseconds(90));
    }
}

size_t Style::visibleLength(const std::string& s) {
    size_t len = 0;
    bool inEscape = false;
    for (size_t i = 0; i < s.length(); ++i) {
        if (s[i] == '\033') {
            inEscape = true;
        } else if (inEscape) {
            if (s[i] == 'm') inEscape = false;
        } else {
            len++;
        }
    }
    return len;
}

std::string Style::padRight(const std::string& s, size_t width) {
    size_t vlen = visibleLength(s);
    if (vlen >= width) return s;
    return s + std::string(width - vlen, ' ');
}


}
}
