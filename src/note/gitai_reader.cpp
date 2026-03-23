#include "gitai_reader.hpp"

namespace ghost {
namespace note {

NoteReader::Result GitAiReader::parse(const std::string& note_content) {
    NoteReader::Result result;
    result.success = false;
    result.error = "Not implemented";
    return result;
}

void GitAiReader::mapToGhostFormat(Session& session) {
    // Map git-ai fields to ghost: tool -> agent
}

}
}