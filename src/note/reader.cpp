#include "reader.hpp"
#include <sstream>
#include <algorithm>

namespace ghost {
namespace note {

NoteReader::Result NoteReader::parse(const std::string& note_content) {
    Result result;
    result.success = false;

    if (note_content.empty()) {
        result.error = "Empty note content";
        return result;
    }

    size_t separatorPos = note_content.find("---");
    if (separatorPos == std::string::npos) {
        result.error = "Missing separator";
        return result;
    }

    std::string topSection = note_content.substr(0, separatorPos);
    std::string jsonSection = note_content.substr(separatorPos + 3);

    result.success = true;
    return result;
}

}
}