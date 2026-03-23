#ifndef GHOST_NOTE_LINE_RANGE_HPP
#define GHOST_NOTE_LINE_RANGE_HPP

#include <string>
#include <vector>

namespace ghost {
namespace note {

struct Range {
    int start;
    int end;

    bool operator<(const Range& other) const {
        return start < other.start;
    }
};

class LineRangeSet {
public:
    static LineRangeSet parse(const std::string& input);

    std::string toString() const;
    std::vector<int> toLines() const;
    bool empty() const;
    size_t lineCount() const;

private:
    std::vector<Range> ranges_;

    static std::vector<std::string> split(const std::string& str, char delimiter);
    static std::string trim(const std::string& str);
    void mergeRanges();
};

}
}

#endif