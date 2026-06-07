#include "ref.hpp"
#include <cctype>

namespace ghost {
namespace git {

bool Ref::isSafeToken(const std::string& value) {
    if (value.empty() || value.size() > 256) return false;
    if (value[0] == '-') return false;
    if (value.find("..") != std::string::npos) {
        // Ranges are allowed only through isSafeRange.
        return false;
    }
    for (unsigned char c : value) {
        if (std::isalnum(c)) continue;
        switch (c) {
            case '_':
            case '-':
            case '.':
            case '/':
            case '@':
            case '{':
            case '}':
            case '~':
            case '^':
                continue;
            default:
                return false;
        }
    }
    return true;
}

bool Ref::isSafeCommitish(const std::string& value) {
    return isSafeToken(value);
}

bool Ref::isSafeRange(const std::string& value) {
    if (value.empty() || value.size() > 512) return false;
    if (value[0] == '-') return false;
    for (unsigned char c : value) {
        if (std::isalnum(c)) continue;
        switch (c) {
            case '_':
            case '-':
            case '.':
            case '/':
            case '@':
            case '{':
            case '}':
            case '~':
            case '^':
                continue;
            default:
                return false;
        }
    }
    return true;
}

bool Ref::isSafeConfigRef(const std::string& value) {
    return isSafeToken(value);
}

bool Ref::isSafeNotesRef(const std::string& value) {
    if (!isSafeToken(value)) return false;
    return value == "ghost" ||
           value == "ai" ||
           value.rfind("refs/notes/", 0) == 0;
}

}
}
