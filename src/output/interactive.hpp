#ifndef GHOST_OUTPUT_INTERACTIVE_HPP
#define GHOST_OUTPUT_INTERACTIVE_HPP

#include <string>
#include <vector>
#include <functional>

namespace ghost {
namespace output {
namespace interactive {

// Platform-agnostic raw key codes
enum KeyCode {
    KEY_UNKNOWN = -1,
    KEY_UP = 1000,
    KEY_DOWN = 1001,
    KEY_LEFT = 1002,
    KEY_RIGHT = 1003,
    KEY_ENTER = '\r',
    KEY_ESCAPE = 27,
    KEY_SPACE = ' ',
    KEY_TAB = '\t',
    KEY_BACKSPACE = 127,
    KEY_DELETE = 1004,
    KEY_HOME = 1005,
    KEY_END = 1006,
    KEY_CTRL_C = 3,
    KEY_Q = 'q',
    KEY_Y = 'y',
    KEY_N = 'n',
    KEY_1 = '1',
    KEY_2 = '2',
    KEY_3 = '3',
    KEY_4 = '4',
    KEY_5 = '5',
    KEY_6 = '6',
    KEY_7 = '7',
    KEY_8 = '8',
    KEY_9 = '9',
    KEY_0 = '0'
};

// Read a single keypress without buffering or echo
// Returns KeyCode enum values for special keys, ASCII for regular keys
int readKeyRaw();

// Check if stdin is a TTY
bool isInteractive();

// Cursor control helpers
void hideCursor();
void showCursor();
void clearLine();
void moveUp(int n = 1);
void moveDown(int n = 1);
void moveToColumn(int col);
void saveCursor();
void restoreCursor();

// A simple selectable menu with arrow key navigation
// Returns the selected index, or -1 if cancelled (ESC or q)
int selectMenu(const std::string& title,
               const std::vector<std::string>& items,
               int defaultIndex = 0);

// A yes/no confirmation prompt
// Returns true for yes, false for no
// Supports Y/n or y/N default
bool confirmPrompt(const std::string& question, bool defaultYes = true);

// Free text input with optional default value
std::string inputPrompt(const std::string& prompt,
                        const std::string& defaultValue = "",
                        bool allowEmpty = false);

// A multi-step wizard for configuration
struct WizardStep {
    std::string title;
    std::string description;
    std::vector<std::string> options;
    int defaultIndex = 0;
    bool allowCustom = false;  // If true, user can type custom value
};

// Run a wizard, returns selected indices for each step
// Empty vector means cancelled
std::vector<int> runWizard(const std::string& wizardTitle,
                           const std::vector<WizardStep>& steps);

} // namespace interactive
} // namespace output
} // namespace ghost

#endif
