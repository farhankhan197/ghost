#ifndef GHOST_AUDIT_BLAME_OVERLAY_HPP
#define GHOST_AUDIT_BLAME_OVERLAY_HPP

#include <string>
#include <vector>
#include <map>
#include "../note/reader.hpp"

namespace ghost {
namespace audit {

struct LineAttribution {
    int line_number;
    std::string commit_sha;
    bool is_ai;
    std::string session_id;
    std::string agent;
    std::string model;
};

struct FileAttribution {
    std::string file_path;
    int total_lines;
    int ai_lines;
    std::vector<LineAttribution> lines;
};

class BlameOverlay {
public:
    static FileAttribution overlay(
        const std::string& file_path,
        const std::map<int, std::string>& blame,
        const std::map<std::string, note::NoteReader::Result>& ghostNotes
    );
};

}
}

#endif
