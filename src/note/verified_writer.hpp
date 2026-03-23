#ifndef GHOST_NOTE_VERIFIED_WRITER_HPP
#define GHOST_NOTE_VERIFIED_WRITER_HPP

#include <string>
#include <ctime>

namespace ghost {
namespace note {

struct VerifiedNote {
    std::string schema;
    std::string ghost_version;
    std::string commit;
    time_t ts;
    std::string author;
    int sessions;
};

class VerifiedWriter {
public:
    static std::string write(const VerifiedNote& note);
};

}
}
#endif