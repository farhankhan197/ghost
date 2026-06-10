#include "notes_command.hpp"
#include "commands.hpp"
#include "exit_codes.hpp"
#include "config/ghost_config.hpp"
#include "git/notes.hpp"
#include "git/ref.hpp"
#include "git/repo.hpp"
#include "output/style.hpp"
#include "signing/ssh_signing.hpp"
#include "util/process.hpp"
#include "util/signature.hpp"

#include <ctime>
#include <iostream>
#include <sstream>
#include <string>

namespace ghost {
namespace cli {

static bool hasFlag(int argc, char* argv[], const std::string& flag) {
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == flag) return true;
    }
    return false;
}

static std::string getArg(int argc, char* argv[], const std::string& flag) {
    for (int i = 0; i < argc - 1; ++i) {
        if (std::string(argv[i]) == flag && i + 1 < argc) {
            return argv[i + 1];
        }
    }
    return "";
}

static std::string buildNoteSignature(const std::string& repoRoot, const std::string& commitSha) {
    auto cfg = config::GhostConfigReader::load(repoRoot);
    std::string ghostNote = git::Notes::show("refs/notes/ghost", commitSha);
    std::string verifiedNote = git::Notes::show("refs/notes/ghost-verified", commitSha);
    std::string signer = git::Repo::getUserEmail();
    std::string ghostDigest = ghostNote.empty() ? "absent" : util::hashText(repoRoot, ghostNote);
    std::string verifiedDigest = verifiedNote.empty() ? "absent" : util::hashText(repoRoot, verifiedNote);
    long long ts = static_cast<long long>(std::time(nullptr));

    std::ostringstream sig;
    if (signing::hasTrustedSigners(cfg)) {
        std::string signerPrincipal = signer.empty() ? "unknown" : signer;
        std::string payload = signing::canonicalNotePayload(commitSha, ghostDigest, verifiedDigest, signerPrincipal, ts);
        auto signedPayload = signing::signPayload(repoRoot, "ghost-notes", payload, cfg);
        if (signedPayload.ok) {
            sig << "schema: ghost-note-signature/2\n";
            sig << "commit: " << commitSha << "\n";
            sig << "ghost_digest: " << ghostDigest << "\n";
            sig << "verified_digest: " << verifiedDigest << "\n";
            sig << "signer: " << signedPayload.signer << "\n";
            sig << "ts: " << ts << "\n";
            sig << "namespace: ghost-notes\n";
            sig << "key_fingerprint: " << signedPayload.key_fingerprint << "\n";
            sig << "payload_b64: " << signedPayload.payload_b64 << "\n";
            sig << "signature_b64: " << signedPayload.signature_b64 << "\n";
            return sig.str();
        }
    }
    sig << "schema: ghost-note-signature/1\n";
    sig << "commit: " << commitSha << "\n";
    sig << "ghost_digest: " << ghostDigest << "\n";
    sig << "verified_digest: " << verifiedDigest << "\n";
    sig << "signer: " << (signer.empty() ? "unknown" : signer) << "\n";
    sig << "ts: " << ts << "\n";
    return sig.str();
}

int notes(int argc, char* argv[]) {
    std::string repoRoot = git::Repo::getRoot();
    if (repoRoot.empty()) {
        std::cerr << output::Style::error("Not in a git repository") << "\n";
        return kExitNotInRepo;
    }
    if (argc < 3) {
        CommandRegistry::printHelp("notes");
        return kExitError;
    }

    std::string action = argv[2];
    std::string range = getArg(argc, argv, "--range");
    bool trustedRequired = hasFlag(argc, argv, "--trusted");
    std::string commitSha = (argc >= 4 && std::string(argv[3])[0] != '-')
        ? argv[3]
        : git::Repo::getHead();
    if (commitSha.empty() && range.empty()) {
        std::cerr << output::Style::error("No commit selected") << "\n";
        return kExitError;
    }
    if (!commitSha.empty() && !git::Ref::isSafeCommitish(commitSha)) {
        std::cerr << output::Style::error("Invalid commit reference") << "\n";
        return kExitError;
    }
    if (!commitSha.empty()) {
        std::string resolved = util::Process::capture("git rev-parse --verify " + commitSha + " 2>&1");
        if (!resolved.empty() && resolved.find("fatal:") == std::string::npos) {
            commitSha = resolved;
        }
    }

    if (action == "sign") {
        std::string sig = buildNoteSignature(repoRoot, commitSha);
        if (!git::Notes::write("refs/notes/ghost-signatures", commitSha, sig)) {
            std::cerr << output::Style::error("Failed to write note signature") << "\n";
            return kExitError;
        }
        std::cout << output::Style::success("Signed Ghost notes for " + commitSha.substr(0, 8)) << "\n";
        return kExitOk;
    }

    if (action == "verify") {
        if (!range.empty()) {
            if (!git::Ref::isSafeRange(range)) {
                std::cerr << output::Style::error("Invalid commit range") << "\n";
                return kExitError;
            }
            std::string commits = util::Process::capture("git rev-list " + range + " 2>&1");
            if (commits.empty()) {
                std::cout << output::Style::success("No commits to verify in range") << "\n";
                return kExitOk;
            }
            std::istringstream stream(commits);
            std::string sha;
            bool ok = true;
            while (std::getline(stream, sha)) {
                while (!sha.empty() && (sha.back() == '\n' || sha.back() == '\r')) sha.pop_back();
                if (sha.empty()) continue;
                const char* fakeArgvTrusted[] = {argv[0], "notes", "verify", sha.c_str(), "--trusted"};
                const char* fakeArgv[] = {argv[0], "notes", "verify", sha.c_str()};
                int rc = trustedRequired
                    ? notes(5, const_cast<char**>(fakeArgvTrusted))
                    : notes(4, const_cast<char**>(fakeArgv));
                if (rc != kExitOk) ok = false;
            }
            return ok ? kExitOk : kExitBlocked;
        }

        std::string rawSig = git::Notes::show("refs/notes/ghost-signatures", commitSha);
        if (rawSig.empty()) {
            std::cerr << output::Style::error("No Ghost note signature found for " + commitSha.substr(0, 8) + "\n")
                      << output::Style::dim("  Run 'ghost notes sign " + commitSha + "'.\n");
            return kExitBlocked;
        }
        auto sig = util::parseSimpleSignature(rawSig);
        std::string ghostNote = git::Notes::show("refs/notes/ghost", commitSha);
        std::string verifiedNote = git::Notes::show("refs/notes/ghost-verified", commitSha);
        std::string ghostDigest = ghostNote.empty() ? "absent" : util::hashText(repoRoot, ghostNote);
        std::string verifiedDigest = verifiedNote.empty() ? "absent" : util::hashText(repoRoot, verifiedNote);

        bool ok = sig["ghost_digest"] == ghostDigest && sig["verified_digest"] == verifiedDigest;
        if (!ok) {
            std::cerr << output::Style::error("Ghost note signature mismatch for " + commitSha.substr(0, 8) + "\n")
                      << output::Style::dim("  ghost_digest:    " + ghostDigest + "\n")
                      << output::Style::dim("  signed ghost:    " + (sig["ghost_digest"].empty() ? "missing" : sig["ghost_digest"]) + "\n")
                      << output::Style::dim("  verified_digest: " + verifiedDigest + "\n")
                      << output::Style::dim("  signed verified: " + (sig["verified_digest"].empty() ? "missing" : sig["verified_digest"]) + "\n");
            return kExitBlocked;
        }
        if (sig["schema"] == "ghost-note-signature/2") {
            long long ts = util::parseSignatureTs(sig);
            std::string payload = signing::canonicalNotePayload(commitSha, ghostDigest, verifiedDigest, sig["signer"], ts);
            if (signing::base64Decode(sig["payload_b64"]) != payload) {
                std::cerr << output::Style::error("Ghost note signature payload mismatch for " + commitSha.substr(0, 8) + "\n");
                return kExitBlocked;
            }
            auto cfg = config::GhostConfigReader::load(repoRoot);
            std::string verifyError;
            if (!signing::verifyPayload(repoRoot, "ghost-notes", payload, sig["signature_b64"], sig["signer"], cfg, verifyError)) {
                std::cerr << output::Style::error("Ghost note SSH signature verification failed for " + commitSha.substr(0, 8) + "\n")
                          << output::Style::dim("  " + verifyError + "\n");
                return kExitBlocked;
            }
        } else if (trustedRequired) {
            std::cerr << output::Style::error("Trusted note verification requires a v2 SSH signature for " + commitSha.substr(0, 8) + "\n");
            return kExitBlocked;
        }
        std::cout << output::Style::success("Ghost note signature verified for " + commitSha.substr(0, 8)) << "\n";
        std::cout << "  signer: " << (sig["signer"].empty() ? "unknown" : sig["signer"]) << "\n";
        if (sig["schema"] == "ghost-note-signature/2") {
            std::cout << "  trusted: yes\n";
        }
        return kExitOk;
    }

    std::cerr << output::Style::error("Unknown notes action: " + action + "\n")
              << output::Style::dim("  Usage: ghost notes sign [commit]\n")
              << output::Style::dim("         ghost notes verify [commit]\n");
    return kExitError;
}

}
}
