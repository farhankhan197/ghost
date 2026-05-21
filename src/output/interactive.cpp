#include "interactive.hpp"
#include "style.hpp"
#include <iostream>
#include <cstring>

#ifdef _WIN32
#include <conio.h>
#include <windows.h>
#include <io.h>
#else
#include <termios.h>
#include <unistd.h>
#include <sys/select.h>
#endif

namespace ghost {
namespace output {
namespace interactive {

// ── Platform: TTY Detection ─────────────────────────────────────────

bool isInteractive() {
#ifdef _WIN32
    return _isatty(_fileno(stdin)) && _isatty(_fileno(stdout));
#else
    return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
#endif
}

// ── Platform: Raw Key Input ─────────────────────────────────────────

#ifdef _WIN32

static bool winConsoleInitialized = false;
static DWORD winOriginalMode = 0;

static void initWinConsole() {
    if (winConsoleInitialized) return;
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    if (hIn != INVALID_HANDLE_VALUE) {
        GetConsoleMode(hIn, &winOriginalMode);
        DWORD newMode = winOriginalMode;
        newMode &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
        SetConsoleMode(hIn, newMode);
    }
    winConsoleInitialized = true;
}

static void restoreWinConsole() {
    if (!winConsoleInitialized) return;
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    if (hIn != INVALID_HANDLE_VALUE) {
        SetConsoleMode(hIn, winOriginalMode);
    }
}

int readKeyRaw() {
    if (!isInteractive()) {
        int ch = std::getchar();
        return ch == EOF ? KEY_UNKNOWN : ch;
    }
    initWinConsole();
    int ch = _getch();
    if (ch == 0 || ch == 224) {
        int scan = _getch();
        switch (scan) {
            case 72: return KEY_UP;
            case 80: return KEY_DOWN;
            case 75: return KEY_LEFT;
            case 77: return KEY_RIGHT;
            case 71: return KEY_HOME;
            case 79: return KEY_END;
            default: return KEY_UNKNOWN;
        }
    }
    if (ch == 3) return KEY_CTRL_C;  // Ctrl+C
    if (ch == '\r') return KEY_ENTER;
    if (ch == 27) return KEY_ESCAPE;
    if (ch == ' ') return KEY_SPACE;
    if (ch == '\t') return KEY_TAB;
    if (ch == 8 || ch == 127) return KEY_BACKSPACE;
    if (ch >= 'A' && ch <= 'Z') return ch + ('a' - 'A');  // lowercase
    return ch;
}

#else  // Unix / macOS

static bool unixRawInitialized = false;
static struct termios unixOriginalTermios;

static void initUnixRaw() {
    if (unixRawInitialized) return;
    tcgetattr(STDIN_FILENO, &unixOriginalTermios);
    struct termios raw = unixOriginalTermios;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    unixRawInitialized = true;
}

static void restoreUnixRaw() {
    if (!unixRawInitialized) return;
    tcsetattr(STDIN_FILENO, TCSANOW, &unixOriginalTermios);
}

int readKeyRaw() {
    if (!isInteractive()) {
        int ch = std::getchar();
        return ch == EOF ? KEY_UNKNOWN : ch;
    }
    initUnixRaw();
    unsigned char ch = 0;
    if (read(STDIN_FILENO, &ch, 1) != 1) {
        return KEY_UNKNOWN;
    }
    if (ch == 27) {  // Escape sequence
        unsigned char seq[3] = {0, 0, 0};
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return KEY_ESCAPE;
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return KEY_ESCAPE;
        if (seq[0] == '[') {
            switch (seq[1]) {
                case 'A': return KEY_UP;
                case 'B': return KEY_DOWN;
                case 'C': return KEY_RIGHT;
                case 'D': return KEY_LEFT;
                case 'H': return KEY_HOME;
                case 'F': return KEY_END;
                default:
                    // Check for longer sequences (e.g. \033[3~ for Delete)
                    if (seq[1] >= '0' && seq[1] <= '9') {
                        unsigned char next;
                        if (read(STDIN_FILENO, &next, 1) == 1) {
                            if (next == '~' && seq[1] == '3') return KEY_DELETE;
                        }
                    }
                    return KEY_UNKNOWN;
            }
        }
        return KEY_ESCAPE;
    }
    if (ch == 3) return KEY_CTRL_C;  // Ctrl+C
    if (ch == '\r' || ch == '\n') return KEY_ENTER;
    if (ch == ' ') return KEY_SPACE;
    if (ch == '\t') return KEY_TAB;
    if (ch == 127 || ch == 8) return KEY_BACKSPACE;
    if (ch >= 'A' && ch <= 'Z') return ch + ('a' - 'A');  // lowercase
    return ch;
}

#endif

// ── Cursor Control ──────────────────────────────────────────────────

static bool cursorHidden = false;

void hideCursor() {
    if (cursorHidden) return;
    std::cout << "\033[?25l";
    std::cout.flush();
    cursorHidden = true;
}

void showCursor() {
    if (!cursorHidden) return;
    std::cout << "\033[?25h";
    std::cout.flush();
    cursorHidden = false;
}

void clearLine() {
    std::cout << "\033[2K\r";
}

void moveUp(int n) {
    if (n > 0) std::cout << "\033[" << n << "A";
}

void moveDown(int n) {
    if (n > 0) std::cout << "\033[" << n << "B";
}

void moveToColumn(int col) {
    std::cout << "\033[" << col << "G";
}

void saveCursor() {
    std::cout << "\0337";
}

void restoreCursor() {
    std::cout << "\0338";
}

// ── Menu ──────────────────────────────────────────────────────────

int selectMenu(const std::string& title,
               const std::vector<std::string>& items,
               int defaultIndex) {
    if (!isInteractive() || items.empty()) return defaultIndex;

    hideCursor();

    const std::string prefixSelected = Style::glow("▶ ");
    const std::string prefixUnselected = Style::dim("  ");
    const std::string footer = Style::dim("  ↑↓ navigate  •  enter select  •  q cancel");

    int selected = defaultIndex;
    if (selected < 0) selected = 0;
    if (selected >= (int)items.size()) selected = (int)items.size() - 1;

    // Initial render
    std::cout << "\n" << Style::bold(Style::blue(title)) << "\n";
    for (size_t i = 0; i < items.size(); ++i) {
        if ((int)i == selected) {
            std::cout << prefixSelected << Style::glow(items[i]) << "\n";
        } else {
            std::cout << prefixUnselected << Style::dim(items[i]) << "\n";
        }
    }
    std::cout << footer << "\n";
    std::cout.flush();

    while (true) {
        int key = readKeyRaw();
        int oldSelected = selected;

        if (key == KEY_UP) {
            selected--;
            if (selected < 0) selected = (int)items.size() - 1;
        } else if (key == KEY_DOWN) {
            selected++;
            if (selected >= (int)items.size()) selected = 0;
        } else if (key == KEY_ENTER || key == KEY_SPACE) {
            // Clear footer + move up, then show cursor
            clearLine();
            moveUp((int)items.size() + 2);
            for (size_t i = 0; i < items.size() + 2; ++i) {
                clearLine();
                moveDown(1);
            }
            moveUp((int)items.size() + 2);
            showCursor();
            return selected;
        } else if (key == KEY_ESCAPE || key == KEY_Q || key == KEY_CTRL_C) {
            // Cancel
            clearLine();
            moveUp((int)items.size() + 2);
            for (size_t i = 0; i < items.size() + 2; ++i) {
                clearLine();
                moveDown(1);
            }
            moveUp((int)items.size() + 2);
            showCursor();
            return -1;
        } else if (key >= KEY_1 && key <= KEY_9) {
            int idx = key - KEY_1;
            if (idx < (int)items.size()) {
                clearLine();
                moveUp((int)items.size() + 2);
                for (size_t i = 0; i < items.size() + 2; ++i) {
                    clearLine();
                    moveDown(1);
                }
                moveUp((int)items.size() + 2);
                showCursor();
                return idx;
            }
        }

        if (selected != oldSelected) {
            // Redraw: move up to title, then redraw all items
            moveUp((int)items.size() + 1);  // +1 for footer
            for (size_t i = 0; i < items.size(); ++i) {
                clearLine();
                if ((int)i == selected) {
                    std::cout << prefixSelected << Style::glow(items[i]);
                } else {
                    std::cout << prefixUnselected << Style::dim(items[i]);
                }
                if (i + 1 < items.size()) std::cout << "\n";
            }
            std::cout << "\n" << footer;
            std::cout.flush();
        }
    }
}

// ── Confirm Prompt ────────────────────────────────────────────────

bool confirmPrompt(const std::string& question, bool defaultYes) {
    if (!isInteractive()) {
        return defaultYes;
    }

    std::cout << "  " << question;
    if (defaultYes) {
        std::cout << Style::dim(" [Y/n]: ");
    } else {
        std::cout << Style::dim(" [y/N]: ");
    }
    std::cout.flush();

    while (true) {
        int key = readKeyRaw();
        if (key == KEY_ENTER) {
            std::cout << "\n";
            return defaultYes;
        } else if (key == KEY_Y) {
            std::cout << "y\n";
            return true;
        } else if (key == KEY_N) {
            std::cout << "n\n";
            return false;
        } else if (key == KEY_CTRL_C || key == KEY_ESCAPE) {
            std::cout << "\n";
            return defaultYes;  // Treat as default
        }
    }
}

// ── Input Prompt ────────────────────────────────────────────────────

std::string inputPrompt(const std::string& prompt,
                        const std::string& defaultValue,
                        bool allowEmpty) {
    if (!isInteractive()) {
        return defaultValue.empty() && !allowEmpty ? "" : defaultValue;
    }

    showCursor();
    std::cout << "  " << prompt;
    if (!defaultValue.empty()) {
        std::cout << Style::dim(" [" + defaultValue + "]: ");
    } else {
        std::cout << Style::dim(": ");
    }
    std::cout.flush();

    std::string input;
    while (true) {
        int key = readKeyRaw();
        if (key == KEY_ENTER) {
            std::cout << "\n";
            if (input.empty() && !defaultValue.empty()) {
                return defaultValue;
            }
            if (input.empty() && !allowEmpty) {
                std::cout << Style::dim("  (required, please enter a value)\n");
                std::cout << "  " << prompt << Style::dim(": ");
                std::cout.flush();
                continue;
            }
            return input;
        } else if (key == KEY_CTRL_C || key == KEY_ESCAPE) {
            std::cout << "\n";
            return defaultValue;
        } else if (key == KEY_BACKSPACE) {
            if (!input.empty()) {
                input.pop_back();
                std::cout << "\b \b";
                std::cout.flush();
            }
        } else if (key >= 32 && key < 127) {
            input += (char)key;
            std::cout << (char)key;
            std::cout.flush();
        }
    }
}

// ── Wizard ──────────────────────────────────────────────────────────

std::vector<int> runWizard(const std::string& wizardTitle,
                           const std::vector<WizardStep>& steps) {
    std::vector<int> results;
    if (!isInteractive() || steps.empty()) return results;

    std::cout << "\n" << Style::header(wizardTitle) << "\n";

    for (size_t stepIdx = 0; stepIdx < steps.size(); ++stepIdx) {
        const auto& step = steps[stepIdx];
        std::cout << Style::bold(Style::blue("Step " + std::to_string(stepIdx + 1) + "/" + std::to_string(steps.size())))
                  << "  " << Style::glow(step.title) << "\n";
        if (!step.description.empty()) {
            std::cout << "  " << Style::dim(step.description) << "\n";
        }
        std::cout << "\n";

        int choice = selectMenu("", step.options, step.defaultIndex);
        if (choice < 0) {
            // Cancelled
            std::cout << Style::dim("  Cancelled.\n\n");
            return {};
        }
        results.push_back(choice);

        std::cout << "  " << Style::dim("Selected: ") << Style::glow(step.options[choice]) << "\n\n";
    }

    std::cout << Style::success("  Configuration complete!") << "\n\n";
    return results;
}

} // namespace interactive
} // namespace output
} // namespace ghost
