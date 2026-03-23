#include "verified_reader.hpp"

namespace ghost {
namespace note {

VerifiedReader::Result VerifiedReader::parse(const std::string& json_content) {
    Result result;
    result.success = false;

    if (json_content.empty()) {
        result.error = "Empty content";
        return result;
    }

    result.success = true;
    return result;
}

}
}