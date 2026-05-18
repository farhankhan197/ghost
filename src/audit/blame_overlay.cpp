#include "blame_overlay.hpp"

namespace ghost {
namespace audit {

FileAttribution BlameOverlay::overlay(
    const std::string& file_path,
    const std::map<int, std::string>& blame,
    const std::map<std::string, note::NoteReader::Result>& ghostNotes
) {
    FileAttribution result;
    result.file_path = file_path;
    result.total_lines = 0;
    result.ai_lines = 0;

    for (const auto& [lineNum, commitSha] : blame) {
        LineAttribution la;
        la.line_number = lineNum;
        la.commit_sha = commitSha;
        la.is_ai = false;

        auto noteIt = ghostNotes.find(commitSha);
        if (noteIt != ghostNotes.end() && noteIt->second.success) {
            for (const auto& entry : noteIt->second.entries) {
                if (entry.file_path == file_path && entry.ranges.contains(lineNum)) {
                    la.is_ai = true;
                    la.session_id = entry.session_id;
                    auto sessIt = noteIt->second.sessions.find(entry.session_id);
                    if (sessIt != noteIt->second.sessions.end()) {
                        la.agent = sessIt->second.agent;
                        la.model = sessIt->second.model;
                    }
                    break;
                }
            }
        }

        result.lines.push_back(la);
        result.total_lines++;
        if (la.is_ai) result.ai_lines++;
    }

    return result;
}

}
}
