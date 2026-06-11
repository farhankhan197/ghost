#ifndef GHOST_AUDIT_ATTRIBUTION_NOTES_HPP
#define GHOST_AUDIT_ATTRIBUTION_NOTES_HPP

#include "../note/reader.hpp"

#include <map>
#include <string>
#include <vector>

namespace ghost {
namespace audit {

class AttributionNotes {
public:
    static std::map<std::string, note::NoteReader::Result> load(
        const std::string& repoRoot,
        const std::vector<std::string>& commitShas,
        bool gitAiFallback
    );
};

}
}

#endif
