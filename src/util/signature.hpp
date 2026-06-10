#ifndef GHOST_UTIL_SIGNATURE_HPP
#define GHOST_UTIL_SIGNATURE_HPP

#include <map>
#include <string>

namespace ghost {
namespace util {

std::string hashFile(const std::string& path);
std::string hashText(const std::string& repoRoot, const std::string& content);
std::map<std::string, std::string> parseSimpleSignature(const std::string& content);
long long parseSignatureTs(const std::map<std::string, std::string>& sig);

}
}

#endif
