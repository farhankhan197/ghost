#ifndef GHOST_NOTE_READER_HPP
#define GHOST_NOTE_READER_HPP

#include "writer.hpp"
#include <string>
#include <map>
#include <vector>

namespace ghost {
namespace note {

class NoteReader {
public:
    struct Result {
        std::string commit_sha;
        std::vector<AuthorshipEntry> entries;
        std::map<std::string, Session> sessions;
        bool success;
        std::string error;
    };

    static Result parse(const std::string& note_content);
};

}
}
#endif