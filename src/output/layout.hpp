#ifndef GHOST_OUTPUT_LAYOUT_HPP
#define GHOST_OUTPUT_LAYOUT_HPP

#include <cstddef>
#include <string>
#include <vector>

namespace ghost {
namespace output {

class Layout {
public:
    static size_t terminalWidth(size_t fallback = 100);
    static size_t contentWidth(size_t fallback = 100, size_t maxWidth = 120);
    static std::string padRight(const std::string& value, size_t width);
    static std::string ellipsizeMiddle(const std::string& value, size_t width);
    static std::string fitCell(const std::string& value, size_t width, size_t gap = 2);
    static std::vector<std::string> wrapVisible(const std::string& value, size_t width);
    static std::string keyValue(const std::string& key, const std::string& value, size_t keyWidth = 12);
};

}
}

#endif
