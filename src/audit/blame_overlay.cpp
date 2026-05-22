#include "blame_overlay.hpp"

namespace ghost {
namespace audit {

FileAttribution BlameOverlay::overlay(
    const std::string& file_path,
    const git::BlameResult& blame,
    const std::map<std::string, note::NoteReader::Result>& ghostNotes
) {
    FileAttribution result;
    result.file_path = file_path;
    result.total_lines = 0;
    result.ai_lines = 0;

    result.lines.reserve(blame.size());

    for (size_t i = 0; i < blame.size(); ++i) {
        const std::string& commitSha = blame[i];
        int lineNum = static_cast<int>(i) + 1;

        LineAttribution la;
        la.line_number = lineNum;
        la.commit_sha = commitSha;
        la.is_ai = false;

        auto noteIt = ghostNotes.find(commitSha);
        if (noteIt != ghostNotes.end() && noteIt->second.success) {
            // O(1) file lookup using pre-indexed entries
            auto fileEntries = noteIt->second.entries_by_file.find(file_path);
            if (fileEntries != noteIt->second.entries_by_file.end()) {
                for (const auto& entry : fileEntries->second) {
                    if (entry.ranges.contains(lineNum)) {
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
        }

        result.lines.push_back(la);
        result.total_lines++;
        if (la.is_ai) result.ai_lines++;
    }

    return result;
}

}
}
