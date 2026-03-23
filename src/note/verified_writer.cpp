#include "verified_writer.hpp"
#include <sstream>

namespace ghost {
namespace note {

std::string VerifiedWriter::write(const VerifiedNote& note) {
    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"schema\": \"" << note.schema << "\",\n";
    oss << "  \"ghost_version\": \"" << note.ghost_version << "\",\n";
    oss << "  \"commit\": \"" << note.commit << "\",\n";
    oss << "  \"ts\": " << note.ts << ",\n";
    oss << "  \"author\": \"" << note.author << "\",\n";
    oss << "  \"sessions\": " << note.sessions << "\n";
    oss << "}\n";
    return oss.str();
}

}
}