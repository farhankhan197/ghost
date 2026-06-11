#include "notes.hpp"
#include "engine.hpp"
#include "ref.hpp"

namespace ghost {
namespace git {

std::string Notes::show(const std::string& ref, const std::string& commit_sha) {
    return show(".", ref, commit_sha);
}

std::string Notes::show(const std::string& repo_root, const std::string& ref, const std::string& commit_sha) {
    if (!Ref::isSafeNotesRef(ref) || !Ref::isSafeCommitish(commit_sha)) return "";
    return Engine::noteShow(repo_root, ref, commit_sha);
}

bool Notes::write(const std::string& ref, const std::string& commit_sha, const std::string& content) {
    return write(".", ref, commit_sha, content);
}

bool Notes::write(const std::string& repo_root, const std::string& ref, const std::string& commit_sha, const std::string& content) {
    if (!Ref::isSafeNotesRef(ref) || !Ref::isSafeCommitish(commit_sha)) return false;
    return Engine::noteWrite(repo_root, ref, commit_sha, content);
}

bool Notes::exists(const std::string& ref, const std::string& commit_sha) {
    return exists(".", ref, commit_sha);
}

bool Notes::exists(const std::string& repo_root, const std::string& ref, const std::string& commit_sha) {
    if (!Ref::isSafeNotesRef(ref) || !Ref::isSafeCommitish(commit_sha)) return false;
    return Engine::noteExists(repo_root, ref, commit_sha);
}

std::map<std::string, std::string> Notes::showBatch(const std::string& ref) {
    return showBatch(".", ref, {});
}

std::map<std::string, std::string> Notes::showBatch(const std::string& repo_root, const std::string& ref) {
    return showBatch(repo_root, ref, {});
}

std::map<std::string, std::string> Notes::showBatch(
    const std::string& ref,
    const std::vector<std::string>& commit_shas
) {
    return showBatch(".", ref, commit_shas);
}

std::map<std::string, std::string> Notes::showBatch(
    const std::string& repo_root,
    const std::string& ref,
    const std::vector<std::string>& commit_shas
) {
    std::map<std::string, std::string> result;
    if (!Ref::isSafeNotesRef(ref)) return result;
    for (const auto& sha : commit_shas) {
        if (!Ref::isSafeCommitish(sha)) return result;
    }
    
    result = Engine::noteShowBatch(repo_root, ref, commit_shas);

    if (!commit_shas.empty()) {
        for (const auto& sha : commit_shas) {
            if (result.count(sha) > 0) continue;
            std::string content = show(repo_root, ref, sha);
            if (!content.empty()) {
                result[sha] = content;
            }
        }
    }
    
    return result;
}

}
}
