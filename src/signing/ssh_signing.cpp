#include "ssh_signing.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <ctime>
#include <vector>

namespace fs = std::filesystem;

namespace ghost {
namespace signing {

static std::string runCommand(const std::string& cmd) {
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) return "";
    std::string result;
    char buffer[512];
    while (fgets(buffer, sizeof(buffer), pipe.get())) result += buffer;
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) result.pop_back();
    return result;
}

static int runCommandRc(const std::string& cmd) {
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) return -1;
    char buffer[512];
    while (fgets(buffer, sizeof(buffer), pipe.get())) {}
    return pclose(pipe.release());
}

static std::string trim(const std::string& value) {
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) start++;
    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) end--;
    return value.substr(start, end - start);
}

static std::string quotePath(const fs::path& path) {
    std::string s = path.string();
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else out += c;
    }
    out += "\"";
    return out;
}

static std::string homeDir() {
    const char* home = std::getenv("USERPROFILE");
    if (!home) home = std::getenv("HOME");
    return home ? home : "";
}

static std::string readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return "";
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static bool writeFile(const fs::path& path, const std::string& content) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) return false;
    out << content;
    return true;
}

static std::string normalizePublicKey(const std::string& key) {
    std::istringstream stream(trim(key));
    std::string type;
    std::string body;
    stream >> type >> body;
    if (type.empty() || body.empty()) return "";
    return type + " " + body;
}

static std::string publicKeyForPrivateKey(const fs::path& privateKey) {
    fs::path pub = privateKey;
    pub += ".pub";
    std::string content = readFile(pub);
    if (!content.empty()) return normalizePublicKey(content);
    return normalizePublicKey(runCommand("ssh-keygen -y -f " + quotePath(privateKey) + " 2>&1"));
}

static std::string findSigningKey() {
    const char* envKey = std::getenv("GHOST_SIGNING_KEY");
    if (envKey && fs::exists(envKey)) return envKey;

    std::string configured = trim(runCommand("git config user.signingkey 2>&1"));
    if (!configured.empty() &&
        configured.find("not found") == std::string::npos &&
        configured.find("error") == std::string::npos) {
        fs::path p(configured);
        if (p.extension() == ".pub") p.replace_extension("");
        if (fs::exists(p)) return p.string();
    }

    std::string home = homeDir();
    if (home.empty()) return "";
    for (const auto& name : {"id_ed25519", "id_ecdsa", "id_rsa"}) {
        fs::path candidate = fs::path(home) / ".ssh" / name;
        if (fs::exists(candidate)) return candidate.string();
    }
    return "";
}

static std::string signerPrincipal(const config::TrustedSigner& signer) {
    if (!signer.email.empty()) return signer.email;
    if (!signer.github.empty()) return signer.github;
    if (!signer.name.empty()) return signer.name;
    return "ghost-signer";
}

static const config::TrustedSigner* trustedSignerForPublicKey(const config::GhostConfig& cfg, const std::string& publicKey) {
    std::string normalized = normalizePublicKey(publicKey);
    if (normalized.empty()) return nullptr;
    for (const auto& signer : cfg.trusted_signers) {
        if (normalizePublicKey(signer.ssh_key) == normalized) return &signer;
    }
    return nullptr;
}

static std::string stableFallbackFingerprint(const std::string& publicKey) {
    unsigned long long hash = 1469598103934665603ull;
    for (unsigned char c : normalizePublicKey(publicKey)) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    std::ostringstream out;
    out << "ghost-key-" << std::hex << hash;
    return out.str();
}

static fs::path tempRoot(const std::string& repoRoot) {
    std::error_code ec;
    fs::path root = repoRoot.empty() ? fs::temp_directory_path() : fs::path(repoRoot) / ".git" / "ghost";
    fs::create_directories(root, ec);
    return root;
}

static std::string keyFingerprint(const std::string& publicKey) {
    fs::path path = tempRoot("") / ("ghost-key-" + std::to_string(std::time(nullptr)) + ".pub");
    if (!writeFile(path, normalizePublicKey(publicKey) + "\n")) {
        return stableFallbackFingerprint(publicKey);
    }

    std::string out = runCommand("ssh-keygen -lf " + quotePath(path) + " 2>&1");
    std::error_code ec;
    fs::remove(path, ec);

    std::istringstream stream(out);
    std::string bits;
    std::string fingerprint;
    stream >> bits >> fingerprint;
    if (fingerprint.empty() || fingerprint.find("SHA256:") == std::string::npos) {
        return stableFallbackFingerprint(publicKey);
    }
    return fingerprint;
}

static const char b64Table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const std::string& input) {
    std::string out;
    int val = 0;
    int valb = -6;
    for (unsigned char c : input) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            out.push_back(b64Table[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(b64Table[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

std::string base64Decode(const std::string& input) {
    std::vector<int> table(256, -1);
    for (int i = 0; i < 64; i++) table[static_cast<unsigned char>(b64Table[i])] = i;
    std::string out;
    int val = 0;
    int valb = -8;
    for (unsigned char c : input) {
        if (std::isspace(c) || c == '=') continue;
        if (table[c] == -1) break;
        val = (val << 6) + table[c];
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<char>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

std::string canonicalPolicyPayload(const std::string& repoRoot, const std::string& digest, const std::string& signer, long long ts) {
    std::string remote = trim(runCommand("git -C " + quotePath(repoRoot) + " remote get-url origin 2>&1"));
    if (remote.find("fatal:") != std::string::npos ||
        remote.find("error:") != std::string::npos ||
        remote.find("No such remote") != std::string::npos) {
        remote.clear();
    }

    std::ostringstream out;
    out << "ghost-policy-signature/2\n";
    out << "repo: " << remote << "\n";
    out << "policy: ghost.yml\n";
    out << "policy_digest: " << digest << "\n";
    out << "signer: " << signer << "\n";
    out << "ts: " << ts << "\n";
    return out.str();
}

std::string canonicalNotePayload(const std::string& commitSha, const std::string& ghostDigest, const std::string& verifiedDigest, const std::string& signer, long long ts) {
    std::ostringstream out;
    out << "ghost-note-signature/2\n";
    out << "commit: " << commitSha << "\n";
    out << "ghost_digest: " << ghostDigest << "\n";
    out << "verified_digest: " << verifiedDigest << "\n";
    out << "signer: " << signer << "\n";
    out << "ts: " << ts << "\n";
    return out.str();
}

bool hasTrustedSigners(const config::GhostConfig& cfg) {
    for (const auto& signer : cfg.trusted_signers) {
        if (!normalizePublicKey(signer.ssh_key).empty()) return true;
    }
    return false;
}

SignatureResult signPayload(const std::string& repoRoot, const std::string& ns, const std::string& payload, const config::GhostConfig& cfg) {
    SignatureResult result;
    std::string keyPath = findSigningKey();
    if (keyPath.empty()) {
        result.error = "No SSH signing key found. Set GHOST_SIGNING_KEY or git config user.signingkey.";
        return result;
    }

    std::string publicKey = publicKeyForPrivateKey(keyPath);
    const auto* trusted = trustedSignerForPublicKey(cfg, publicKey);
    if (!trusted) {
        result.error = "Signing key is not listed in trusted_signers.";
        return result;
    }

    long long stamp = static_cast<long long>(std::time(nullptr));
    fs::path root = tempRoot(repoRoot);
    fs::path payloadPath = root / ("sign-payload-" + std::to_string(stamp) + ".txt");
    fs::path sigPath = payloadPath;
    sigPath += ".sig";
    if (!writeFile(payloadPath, payload)) {
        result.error = "Failed to write signature payload.";
        return result;
    }

    std::string cmd = "ssh-keygen -Y sign -f " + quotePath(keyPath) + " -n " + ns + " " + quotePath(payloadPath) + " 2>&1";
    int rc = runCommandRc(cmd);
    if (rc != 0 || !fs::exists(sigPath)) {
        result.error = "ssh-keygen failed to sign payload.";
        std::error_code ec;
        fs::remove(payloadPath, ec);
        return result;
    }

    result.ok = true;
    result.signer = signerPrincipal(*trusted);
    result.key_fingerprint = keyFingerprint(publicKey);
    result.payload_b64 = base64Encode(payload);
    result.signature_b64 = base64Encode(readFile(sigPath));

    std::error_code ec;
    fs::remove(payloadPath, ec);
    fs::remove(sigPath, ec);
    return result;
}

bool verifyPayload(const std::string& repoRoot, const std::string& ns, const std::string& payload, const std::string& signatureB64, const std::string& signer, const config::GhostConfig& cfg, std::string& error) {
    if (signer.empty()) {
        error = "Missing signer principal.";
        return false;
    }
    if (!hasTrustedSigners(cfg)) {
        error = "No trusted_signers configured.";
        return false;
    }

    long long stamp = static_cast<long long>(std::time(nullptr));
    fs::path root = tempRoot(repoRoot);
    fs::path payloadPath = root / ("verify-payload-" + std::to_string(stamp) + ".txt");
    fs::path sigPath = root / ("verify-payload-" + std::to_string(stamp) + ".sig");
    fs::path allowedPath = root / ("allowed-signers-" + std::to_string(stamp) + ".txt");

    std::ostringstream allowed;
    for (const auto& trusted : cfg.trusted_signers) {
        std::string key = normalizePublicKey(trusted.ssh_key);
        if (key.empty()) continue;
        allowed << signerPrincipal(trusted) << " " << key << "\n";
    }

    if (!writeFile(payloadPath, payload) ||
        !writeFile(sigPath, base64Decode(signatureB64)) ||
        !writeFile(allowedPath, allowed.str())) {
        error = "Failed to write verification files.";
        return false;
    }

    std::string cmd = "ssh-keygen -Y verify -f " + quotePath(allowedPath) +
        " -I \"" + signer + "\" -n " + ns + " -s " + quotePath(sigPath) +
        " < " + quotePath(payloadPath) + " 2>&1";
    int rc = runCommandRc(cmd);

    std::error_code ec;
    fs::remove(payloadPath, ec);
    fs::remove(sigPath, ec);
    fs::remove(allowedPath, ec);

    if (rc != 0) {
        error = "SSH signature verification failed.";
        return false;
    }
    return true;
}

}
}
