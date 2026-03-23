#ifndef GHOST_NOTE_VERIFIED_READER_HPP
#define GHOST_NOTE_VERIFIED_READER_HPP

#include "verified_writer.hpp"
#include <string>

namespace ghost {
namespace note {

class VerifiedReader {
public:
    struct Result {
        VerifiedNote note;
        bool success;
        std::string error;
    };

    static Result parse(const std::string& json_content);
};

}
}
#endif