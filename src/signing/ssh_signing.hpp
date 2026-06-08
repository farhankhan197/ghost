#ifndef GHOST_SIGNING_SSH_SIGNING_HPP
#define GHOST_SIGNING_SSH_SIGNING_HPP

#include <string>
#include "config/ghost_config.hpp"

namespace ghost {
namespace signing {

struct SignatureResult {
    bool ok = false;
    std::string signer;
    std::string key_fingerprint;
    std::string payload_b64;
    std::string signature_b64;
    std::string error;
};

std::string base64Encode(const std::string& input);
std::string base64Decode(const std::string& input);
std::string canonicalPolicyPayload(const std::string& repoRoot, const std::string& digest, const std::string& signer, long long ts);
std::string canonicalNotePayload(const std::string& commitSha, const std::string& ghostDigest, const std::string& verifiedDigest, const std::string& signer, long long ts);
bool hasTrustedSigners(const config::GhostConfig& cfg);
SignatureResult signPayload(const std::string& repoRoot, const std::string& ns, const std::string& payload, const config::GhostConfig& cfg);
bool verifyPayload(const std::string& repoRoot, const std::string& ns, const std::string& payload, const std::string& signatureB64, const std::string& signer, const config::GhostConfig& cfg, std::string& error);

}
}

#endif
