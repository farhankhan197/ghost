#ifndef GHOST_NOTE_WRITER_HPP
#define GHOST_NOTE_WRITER_HPP

#include "line_range.hpp"
#include <string>
#include <map>
#include <vector>
#include <ctime>

namespace ghost {
namespace note {

struct Session {
    std::string session_id;
    std::string agent;
    std::string model;
    std::string author;
    time_t ts_start;
    time_t ts_end;
    int additions;
    int deletions;
};

struct AuthorshipEntry {
    std::string file_path;
    std::string session_id;
    LineRangeSet ranges;
};

class NoteWriter {
public:
    static std::string write(
        const std::vector<AuthorshipEntry>& entries,
        const std::map<std::string, Session>& sessions,
        const std::string& commit_sha
    );

private:
    static std::string serializeTop(const std::vector<AuthorshipEntry>& entries);
    static std::string serializeJson(
        const std::string& commit_sha,
        const std::map<std::string, Session>& sessions
    );
};

}
}

#endif