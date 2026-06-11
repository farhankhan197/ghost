#include "attribution_notes.hpp"

#include "../git/engine.hpp"
#include "../note/gitai_reader.hpp"

namespace ghost {
namespace audit {

std::map<std::string, note::NoteReader::Result> AttributionNotes::load(
    const std::string& repoRoot,
    const std::vector<std::string>& commitShas,
    bool gitAiFallback
) {
    std::map<std::string, note::NoteReader::Result> notes;

    auto ghostBatch = git::Engine::noteShowBatch(repoRoot, "refs/notes/ghost", commitShas);
    for (const auto& [sha, raw] : ghostBatch) {
        if (!raw.empty()) {
            notes[sha] = note::NoteReader::parse(raw);
        }
    }

    if (!gitAiFallback) return notes;

    auto gitAiBatch = git::Engine::noteShowBatch(repoRoot, "refs/notes/ai", commitShas);
    for (const auto& [sha, raw] : gitAiBatch) {
        if (raw.empty() || notes.count(sha) > 0) continue;
        notes[sha] = note::GitAiReader::parse(raw);
    }

    return notes;
}

}
}
