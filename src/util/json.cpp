#include "json.hpp"

#include <sstream>

namespace ghost {
namespace util {

std::string Json::escape(const std::string& value) {
    std::ostringstream out;
    for (unsigned char c : value) {
        switch (c) {
            case '\\': out << "\\\\"; break;
            case '"': out << "\\\""; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 0x20) {
                    out << "\\u"
                        << "00"
                        << "0123456789abcdef"[(c >> 4) & 0x0f]
                        << "0123456789abcdef"[c & 0x0f];
                } else {
                    out << static_cast<char>(c);
                }
                break;
        }
    }
    return out.str();
}

}
}
