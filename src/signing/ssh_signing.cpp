#include "ssh_signing.hpp"
#include "util/process.hpp"
#include "util/files.hpp"
#include "util/text.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <ctime>
#include <vector>

namespace fs = std::filesystem;

namespace ghost {
namespace signing {

static util::Process::Result runProcess(
    const std::string& executable,
    std::vector<std::string> args,
    const std::string& cwd = "",
    const std::string& stdinText = ""
) {
    util::Process::Command command;
    command.executable = executable;
    command.args = std::move(args);
    command.cwd = cwd;
    command.stdinText = stdinText;
    return util::Process::capture(command);
}

static std::string normalizePublicKey(const std::string& key) {
    std::istringstream stream(util::Text::trim(key));
    std::string type;
    std::string body;
    stream >> type >> body;
    if (type.empty() || body.empty()) return "";
    return type + " " + body;
}

static std::string publicKeyForPrivateKey(const fs::path& privateKey) {
    fs::path pub = privateKey;
    pub += ".pub";
    std::string content = util::Files::readText(pub);
    if (!content.empty()) return normalizePublicKey(content);
    return normalizePublicKey(runProcess("ssh-keygen", {"-y", "-f", privateKey.string()}).stdoutText);
}

static std::string findSigningKey() {
    const char* envKey = std::getenv("GHOST_SIGNING_KEY");
    if (envKey && fs::exists(envKey)) return envKey;

    std::string configured = util::Text::trim(runProcess("git", {"config", "user.signingkey"}).stdoutText);
    if (!configured.empty() &&
        configured.find("not found") == std::string::npos &&
        configured.find("error") == std::string::npos) {
        fs::path p(configured);
        if (p.extension() == ".pub") p.replace_extension("");
        if (fs::exists(p)) return p.string();
    }

    std::string home = util::Files::homeDir();
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
    if (!util::Files::writeText(path, normalizePublicKey(publicKey) + "\n")) {
        return stableFallbackFingerprint(publicKey);
    }

    std::string out = runProcess("ssh-keygen", {"-lf", path.string()}).stdoutText;
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
    std::string remote = util::Text::trim(runProcess("git", {"remote", "get-url", "origin"}, repoRoot).stdoutText);
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
    if (!util::Files::writeText(payloadPath, payload)) {
        result.error = "Failed to write signature payload.";
        return result;
    }

    auto signResult = runProcess("ssh-keygen", {"-Y", "sign", "-f", keyPath, "-n", ns, payloadPath.string()});
    if (!signResult.ok() || !fs::exists(sigPath)) {
        result.error = "ssh-keygen failed to sign payload.";
        std::error_code ec;
        fs::remove(payloadPath, ec);
        return result;
    }

    result.ok = true;
    result.signer = signerPrincipal(*trusted);
    result.key_fingerprint = keyFingerprint(publicKey);
    result.payload_b64 = base64Encode(payload);
    result.signature_b64 = base64Encode(util::Files::readText(sigPath));

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

    if (!util::Files::writeText(payloadPath, payload) ||
        !util::Files::writeText(sigPath, base64Decode(signatureB64)) ||
        !util::Files::writeText(allowedPath, allowed.str())) {
        error = "Failed to write verification files.";
        return false;
    }

    auto verifyResult = runProcess(
        "ssh-keygen",
        {"-Y", "verify", "-f", allowedPath.string(), "-I", signer, "-n", ns, "-s", sigPath.string()},
        "",
        payload
    );

    std::error_code ec;
    fs::remove(payloadPath, ec);
    fs::remove(sigPath, ec);
    fs::remove(allowedPath, ec);

    if (!verifyResult.ok()) {
        error = "SSH signature verification failed.";
        return false;
    }
    return true;
}

}
}
