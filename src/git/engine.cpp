#include "engine.hpp"
#include "ref.hpp"
#include <git2.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace ghost {
namespace git {

namespace {

struct LibGit2Runtime {
    LibGit2Runtime() { git_libgit2_init(); }
    ~LibGit2Runtime() { git_libgit2_shutdown(); }
};

static LibGit2Runtime& runtime() {
    static LibGit2Runtime rt;
    return rt;
}

static std::string oidToString(const git_oid* oid) {
    if (!oid) return "";
    char out[GIT_OID_HEXSZ + 1] = {0};
    git_oid_tostr(out, sizeof(out), oid);
    return out;
}

static git_repository* openRepo(const std::string& repoRoot) {
    runtime();
    git_repository* repo = nullptr;
    std::string start = (repoRoot.empty() || repoRoot == ".")
        ? fs::current_path().generic_string()
        : fs::path(repoRoot).generic_string();

    fs::path directGit = fs::path(start) / ".git";
    if (fs::exists(directGit) && git_repository_open(&repo, directGit.generic_string().c_str()) == 0) {
        return repo;
    }

    git_buf discovered = GIT_BUF_INIT;
    if (git_repository_discover(&discovered, start.c_str(), 0, nullptr) != 0 || !discovered.ptr) {
        git_buf_dispose(&discovered);
        return nullptr;
    }
    int rc = git_repository_open(&repo, discovered.ptr);
    git_buf_dispose(&discovered);
    if (rc != 0) return nullptr;
    return repo;
}

static std::string runCommand(const std::string& cmd) {
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) return "";
    std::string result;
    char buffer[512];
    while (fgets(buffer, sizeof(buffer), pipe.get())) result += buffer;
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) result.pop_back();
    return result;
}

static std::string quietRedirect() {
#ifdef _WIN32
    return " 2>nul";
#else
    return " 2>/dev/null";
#endif
}

static std::string shellNotesRef(const std::string& ref) {
    if (ref == "ghost" || ref == "ai") return "refs/notes/" + ref;
    return ref;
}

static std::string quotePath(const std::string& path) {
    std::string out = "\"";
    for (char c : path) {
        if (c == '"') out += "\\\"";
        else out += c;
    }
    out += "\"";
    return out;
}

static std::string gitPrefix(const std::string& repoRoot) {
    std::string root = (repoRoot.empty() || repoRoot == ".")
        ? fs::current_path().string()
        : repoRoot;
    return "git -C " + quotePath(root) + " ";
}

static git_commit* lookupCommit(git_repository* repo, const std::string& spec) {
    if (!repo || spec.empty()) return nullptr;
    git_object* obj = nullptr;
    if (git_revparse_single(&obj, repo, spec.c_str()) != 0) return nullptr;

    git_commit* commit = nullptr;
    if (git_object_type(obj) == GIT_OBJECT_COMMIT) {
        commit = reinterpret_cast<git_commit*>(obj);
    } else {
        git_object* peeled = nullptr;
        if (git_object_peel(&peeled, obj, GIT_OBJECT_COMMIT) == 0) {
            commit = reinterpret_cast<git_commit*>(peeled);
        }
        git_object_free(obj);
    }
    return commit;
}

static std::string normalizePath(const char* path) {
    if (!path) return "";
    std::string out(path);
    for (char& c : out) {
        if (c == '\\') c = '/';
    }
    return out;
}

static bool isSafeBlobPath(const std::string& path) {
    if (path.empty() || path.find("..") != std::string::npos) return false;
    for (char c : path) {
        bool ok = (c >= 'a' && c <= 'z') ||
                  (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') ||
                  c == '/' || c == '.' || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

static bool isSafeConfigKey(const std::string& key) {
    if (key.empty()) return false;
    for (char c : key) {
        bool ok = (c >= 'a' && c <= 'z') ||
                  (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') ||
                  c == '.' || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

static std::string normalizedNotesRef(const std::string& ref) {
    if (ref == "ghost" || ref == "ai") return "refs/notes/" + ref;
    return ref;
}

struct TreePayload {
    std::vector<std::string>* files;
};

static int treeWalkCb(const char* root, const git_tree_entry* entry, void* payload) {
    if (git_tree_entry_type(entry) != GIT_OBJECT_BLOB) return 0;
    auto* data = static_cast<TreePayload*>(payload);
    std::string path = normalizePath((std::string(root ? root : "") + git_tree_entry_name(entry)).c_str());
    if (!path.empty()) data->files->push_back(path);
    return 0;
}

struct DiffPayload {
    std::set<std::string>* files;
};

static int diffFileCb(const git_diff_delta* delta, float, void* payload) {
    auto* data = static_cast<DiffPayload*>(payload);
    const char* path = delta->new_file.path ? delta->new_file.path : delta->old_file.path;
    std::string normalized = normalizePath(path);
    if (!normalized.empty()) data->files->insert(normalized);
    return 0;
}

struct NoteListPayload {
    std::map<std::string, std::string>* notes;
};

static int noteListCb(const git_oid* blobOid, const git_oid* annotatedOid, void* payload) {
    auto* data = static_cast<NoteListPayload*>(payload);
    (*data->notes)[oidToString(annotatedOid)] = oidToString(blobOid);
    return 0;
}

static std::set<std::string> shaFilter(const std::vector<std::string>& shas) {
    return std::set<std::string>(shas.begin(), shas.end());
}

static std::string blobContent(git_repository* repo, const git_oid* oid) {
    git_blob* blob = nullptr;
    if (!repo || !oid || git_blob_lookup(&blob, repo, oid) != 0) return "";
    const void* raw = git_blob_rawcontent(blob);
    git_object_size_t size = git_blob_rawsize(blob);
    std::string content;
    if (raw && size > 0) {
        content.assign(static_cast<const char*>(raw), static_cast<size_t>(size));
    }
    git_blob_free(blob);
    return content;
}

}

std::string Engine::discoverRoot(const std::string& startPath) {
    runtime();
    std::string start = (startPath.empty() || startPath == ".")
        ? fs::current_path().generic_string()
        : fs::path(startPath).generic_string();
    git_buf discovered = GIT_BUF_INIT;
    if (git_repository_discover(&discovered, start.c_str(), 0, nullptr) != 0 || !discovered.ptr) {
        git_buf_dispose(&discovered);
        return runCommand("git rev-parse --show-toplevel" + quietRedirect());
    }

    git_repository* repo = nullptr;
    if (git_repository_open(&repo, discovered.ptr) != 0) {
        git_buf_dispose(&discovered);
        return "";
    }
    const char* workdir = git_repository_workdir(repo);
    std::string root = workdir ? fs::path(workdir).lexically_normal().string() : "";
    git_repository_free(repo);
    git_buf_dispose(&discovered);
    while (!root.empty() && (root.back() == '/' || root.back() == '\\')) root.pop_back();
    return root;
}

std::string Engine::headSha(const std::string& repoRoot) {
    git_repository* repo = openRepo(repoRoot);
    if (!repo) return runCommand("git rev-parse --verify HEAD" + quietRedirect());
    git_reference* head = nullptr;
    if (git_repository_head(&head, repo) != 0) {
        git_repository_free(repo);
        return runCommand("git rev-parse --verify HEAD" + quietRedirect());
    }
    git_object* obj = nullptr;
    std::string sha;
    if (git_reference_peel(&obj, head, GIT_OBJECT_COMMIT) == 0) {
        sha = oidToString(git_object_id(obj));
        git_object_free(obj);
    }
    git_reference_free(head);
    git_repository_free(repo);
    return sha.empty() ? runCommand("git rev-parse --verify HEAD" + quietRedirect()) : sha;
}

std::string Engine::configString(const std::string& repoRoot, const std::string& key) {
    if (!isSafeConfigKey(key)) return "";
    auto fallback = [&]() {
        return runCommand(gitPrefix(repoRoot) + "config --get " + key + quietRedirect());
    };
    git_repository* repo = openRepo(repoRoot);
    if (!repo) return fallback();
    git_config* cfg = nullptr;
    if (git_repository_config(&cfg, repo) != 0) {
        git_repository_free(repo);
        return fallback();
    }
    git_buf buf = GIT_BUF_INIT;
    std::string value;
    if (git_config_get_string_buf(&buf, cfg, key.c_str()) == 0 && buf.ptr) {
        value = buf.ptr;
    }
    git_buf_dispose(&buf);
    git_config_free(cfg);
    git_repository_free(repo);
    return value.empty() ? fallback() : value;
}

std::string Engine::resolveCommit(const std::string& repoRoot, const std::string& commitish) {
    if (!Ref::isSafeCommitish(commitish)) return "";
    auto fallback = [&]() {
        return runCommand(gitPrefix(repoRoot) + "rev-parse --verify " + commitish + quietRedirect());
    };
    git_repository* repo = openRepo(repoRoot);
    if (!repo) return fallback();
    git_commit* commit = lookupCommit(repo, commitish);
    std::string sha = commit ? oidToString(git_commit_id(commit)) : "";
    if (commit) git_commit_free(commit);
    git_repository_free(repo);
    return sha.empty() ? fallback() : sha;
}

std::string Engine::commitAuthor(const std::string& repoRoot, const std::string& sha) {
    auto fallback = [&]() {
        return runCommand(gitPrefix(repoRoot) + "log -1 --format=\"%an <%ae>\" " + sha + quietRedirect());
    };
    git_repository* repo = openRepo(repoRoot);
    if (!repo) return fallback();
    git_commit* commit = lookupCommit(repo, sha);
    std::string author = "unknown";
    if (commit) {
        const git_signature* sig = git_commit_author(commit);
        if (sig) {
            author = std::string(sig->name ? sig->name : "unknown");
            if (sig->email && sig->email[0]) author += " <" + std::string(sig->email) + ">";
        }
        git_commit_free(commit);
    }
    git_repository_free(repo);
    return author == "unknown" ? fallback() : author;
}

std::map<std::string, std::string> Engine::commitAuthors(const std::string& repoRoot, const std::vector<std::string>& shas) {
    std::map<std::string, std::string> result;
    git_repository* repo = openRepo(repoRoot);
    if (!repo) return result;
    for (const auto& sha : shas) {
        git_commit* commit = lookupCommit(repo, sha);
        if (!commit) continue;
        const git_signature* sig = git_commit_author(commit);
        result[oidToString(git_commit_id(commit))] = sig && sig->name ? sig->name : "unknown";
        git_commit_free(commit);
    }
    git_repository_free(repo);
    return result;
}

std::vector<std::string> Engine::revList(const std::string& repoRoot, const std::string& range) {
    std::vector<std::string> result;
    if (!Ref::isSafeRange(range)) return result;
    std::string out = runCommand(gitPrefix(repoRoot) + "rev-list " + range + " -- ." + quietRedirect());
    std::istringstream stream(out);
    std::string sha;
    while (std::getline(stream, sha)) {
        while (!sha.empty() && (sha.back() == '\n' || sha.back() == '\r')) sha.pop_back();
        if (!sha.empty()) result.push_back(sha);
    }
    return result;
}

std::vector<std::string> Engine::changedFiles(const std::string& repoRoot, const std::string& commitSha) {
    std::set<std::string> files;
    if (!Ref::isSafeCommitish(commitSha)) return {};

    std::string out = runCommand(gitPrefix(repoRoot) + "diff-tree --root --no-commit-id -r --name-only " + commitSha + " -- ." + quietRedirect());
    std::istringstream stream(out);
    std::string file;
    while (std::getline(stream, file)) {
        while (!file.empty() && (file.back() == '\n' || file.back() == '\r')) file.pop_back();
        if (!file.empty()) files.insert(normalizePath(file.c_str()));
    }
    if (!files.empty()) return std::vector<std::string>(files.begin(), files.end());

    git_repository* repo = openRepo(repoRoot);
    if (!repo) return {};
    git_commit* commit = lookupCommit(repo, commitSha);
    if (!commit) {
        git_repository_free(repo);
        return {};
    }

    git_tree* newTree = nullptr;
    if (git_commit_tree(&newTree, commit) != 0) {
        git_commit_free(commit);
        git_repository_free(repo);
        return {};
    }

    size_t parentCount = git_commit_parentcount(commit);
    if (parentCount == 0) {
        std::vector<std::string> rootFiles;
        TreePayload payload{&rootFiles};
        git_tree_walk(newTree, GIT_TREEWALK_PRE, treeWalkCb, &payload);
        files.insert(rootFiles.begin(), rootFiles.end());
    } else {
        for (size_t i = 0; i < parentCount; ++i) {
            git_commit* parent = nullptr;
            git_tree* oldTree = nullptr;
            if (git_commit_parent(&parent, commit, static_cast<unsigned int>(i)) != 0) continue;
            if (git_commit_tree(&oldTree, parent) == 0) {
                git_diff* diff = nullptr;
                git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
                opts.flags |= GIT_DIFF_INCLUDE_TYPECHANGE;
                if (git_diff_tree_to_tree(&diff, repo, oldTree, newTree, &opts) == 0) {
                    DiffPayload payload{&files};
                    git_diff_foreach(diff, diffFileCb, nullptr, nullptr, nullptr, &payload);
                    git_diff_free(diff);
                }
                git_tree_free(oldTree);
            }
            git_commit_free(parent);
        }
    }

    git_tree_free(newTree);
    git_commit_free(commit);
    git_repository_free(repo);
    return std::vector<std::string>(files.begin(), files.end());
}

std::vector<std::string> Engine::treeFiles(const std::string& repoRoot, const std::string& commitSha) {
    std::vector<std::string> files;
    git_repository* repo = openRepo(repoRoot);
    if (!repo) return files;
    git_commit* commit = lookupCommit(repo, commitSha);
    if (!commit) {
        git_repository_free(repo);
        return files;
    }
    git_tree* tree = nullptr;
    if (git_commit_tree(&tree, commit) == 0) {
        TreePayload payload{&files};
        git_tree_walk(tree, GIT_TREEWALK_PRE, treeWalkCb, &payload);
        git_tree_free(tree);
    }
    git_commit_free(commit);
    git_repository_free(repo);
    std::sort(files.begin(), files.end());
    return files;
}

std::string Engine::showBlobAtRef(const std::string& repoRoot, const std::string& ref, const std::string& path) {
    if (!Ref::isSafeConfigRef(ref) || !isSafeBlobPath(path)) return "";
    auto fallback = [&]() {
        return runCommand(gitPrefix(repoRoot) + "show " + ref + ":" + path + quietRedirect());
    };
    git_repository* repo = openRepo(repoRoot);
    if (!repo) return fallback();
    std::string spec = ref + ":" + path;
    git_object* obj = nullptr;
    std::string content;
    if (git_revparse_single(&obj, repo, spec.c_str()) == 0 && git_object_type(obj) == GIT_OBJECT_BLOB) {
        git_blob* blob = reinterpret_cast<git_blob*>(obj);
        const void* raw = git_blob_rawcontent(blob);
        git_object_size_t size = git_blob_rawsize(blob);
        if (raw && size > 0) content.assign(static_cast<const char*>(raw), static_cast<size_t>(size));
        git_blob_free(blob);
    } else if (obj) {
        git_object_free(obj);
    }
    git_repository_free(repo);
    return content.empty() ? fallback() : content;
}

std::string Engine::noteShow(const std::string& repoRoot, const std::string& notesRef, const std::string& commitSha) {
    if (!Ref::isSafeNotesRef(notesRef) || !Ref::isSafeCommitish(commitSha)) return "";
    git_repository* repo = openRepo(repoRoot);
    auto fallback = [&]() {
        return runCommand(gitPrefix(repoRoot) + "notes --ref=" + shellNotesRef(notesRef) + " show " + commitSha + quietRedirect());
    };
    if (!repo) return fallback();
    git_commit* commit = lookupCommit(repo, commitSha);
    if (!commit) {
        git_repository_free(repo);
        return fallback();
    }
    git_note* note = nullptr;
    std::string ref = normalizedNotesRef(notesRef);
    std::string content;
    if (git_note_read(&note, repo, ref.c_str(), git_commit_id(commit)) == 0) {
        const char* message = git_note_message(note);
        if (message) content = message;
        git_note_free(note);
    }
    git_commit_free(commit);
    git_repository_free(repo);
    if (content.empty()) {
        content = fallback();
    }
    return content;
}

bool Engine::noteExists(const std::string& repoRoot, const std::string& notesRef, const std::string& commitSha) {
    return !noteShow(repoRoot, notesRef, commitSha).empty();
}

std::map<std::string, std::string> Engine::noteList(const std::string& repoRoot, const std::string& notesRef) {
    std::map<std::string, std::string> result;
    if (!Ref::isSafeNotesRef(notesRef)) return result;
    git_repository* repo = openRepo(repoRoot);
    if (repo) {
        std::string ref = normalizedNotesRef(notesRef);
        NoteListPayload payload{&result};
        git_note_foreach(repo, ref.c_str(), noteListCb, &payload);
        git_repository_free(repo);
    }
    if (result.empty()) {
        std::string out = runCommand(gitPrefix(repoRoot) + "notes --ref=" + shellNotesRef(notesRef) + " list" + quietRedirect());
        std::istringstream stream(out);
        std::string line;
        while (std::getline(stream, line)) {
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
            size_t space = line.find(' ');
            if (space == std::string::npos || space == 0) continue;
            result[line.substr(space + 1)] = line.substr(0, space);
        }
    }
    return result;
}

std::map<std::string, std::string> Engine::noteShowBatch(
    const std::string& repoRoot,
    const std::string& notesRef,
    const std::vector<std::string>& commitShas
) {
    std::map<std::string, std::string> result;
    if (!Ref::isSafeNotesRef(notesRef)) return result;
    git_repository* repo = openRepo(repoRoot);
    std::set<std::string> wanted = shaFilter(commitShas);
    auto notes = noteList(repoRoot, notesRef);
    if (repo) {
        for (const auto& [commitSha, blobSha] : notes) {
            if (!wanted.empty() && wanted.count(commitSha) == 0) continue;
            git_oid oid;
            if (git_oid_fromstr(&oid, blobSha.c_str()) != 0) continue;
            std::string content = blobContent(repo, &oid);
            if (!content.empty()) result[commitSha] = content;
        }
        git_repository_free(repo);
    }

    if (!commitShas.empty()) {
        for (const auto& sha : commitShas) {
            if (result.count(sha) > 0) continue;
            std::string content = noteShow(repoRoot, notesRef, sha);
            if (!content.empty()) result[sha] = content;
        }
    }
    return result;
}

}
}
