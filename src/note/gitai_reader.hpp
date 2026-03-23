#ifndef GHOST_NOTE_GITAI_READER_HPP
#define GHOST_NOTE_GITAI_READER_HPP

#include "reader.hpp"
#include <string>

namespace ghost {
namespace note {

class GitAiReader {
public:
    static NoteReader::Result parse(const std::string& note_content);
    static void mapToGhostFormat(Session& session);
};

}
}
#endif