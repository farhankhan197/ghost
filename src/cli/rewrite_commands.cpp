#include "rewrite_commands.hpp"
#include "exit_codes.hpp"
#include "git/repo.hpp"
#include "output/style.hpp"
#include "rewrite/processor.hpp"
#include "rewrite/rewrite_log.hpp"
#include "rewrite/working_state.hpp"

#include <iostream>
#include <string>
#include <vector>

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

int rewriteLog(int argc, char* argv[]) {
    std::string repoRoot = git::Repo::getRoot();
    if (repoRoot.empty()) {
        std::cerr << output::Style::error("Not in a git repository") << "\n";
        return kExitNotInRepo;
    }

    if (hasFlag(argc, argv, "--stdin")) {
        auto mappings = rewrite::RewriteLog::readStdinMappings();
        if (mappings.empty()) {
            return kExitOk;
        }

        std::vector<std::string> oldShas, newShas;
        for (const auto& [oldSha, newSha] : mappings) {
            oldShas.push_back(oldSha);
            newShas.push_back(newSha);
        }

        bool isAmend = (mappings.size() == 1);
        if (isAmend) {
            rewrite::CommitAmendEvent ev;
            ev.original_commit = oldShas[0];
            ev.amended_commit_sha = newShas[0];
            rewrite::RewriteLog::append(repoRoot, rewrite::RewriteEventType::CommitAmend, ev.toJson());
            rewrite::Processor::processAmend(repoRoot, ev.original_commit, ev.amended_commit_sha);
        } else {
            rewrite::RebaseCompleteEvent ev;
            ev.original_commits = oldShas;
            ev.new_commits = newShas;
            rewrite::RewriteLog::append(repoRoot, rewrite::RewriteEventType::RebaseComplete, ev.toJson());
            rewrite::Processor::processRebase(repoRoot, ev.original_commits, ev.new_commits);
        }
        return kExitOk;
    }

    std::string eventType = getArg(argc, argv, "--event");
    std::string repoArg = getArg(argc, argv, "--repo");
    if (!repoArg.empty()) repoRoot = repoArg;

    if (!eventType.empty()) {
        if (eventType == "merge") {
            rewrite::RewriteLog::append(repoRoot, rewrite::RewriteEventType::Merge, "{}");
        } else if (eventType == "checkout") {
            std::string prev = getArg(argc, argv, "--prev");
            std::string next = getArg(argc, argv, "--new");
            rewrite::Processor::detectStashPop(repoRoot, prev, next);
            rewrite::RewriteLog::append(repoRoot, rewrite::RewriteEventType::Stash, "{}");
        }
        return kExitOk;
    }

    auto events = rewrite::RewriteLog::load(repoRoot, 20);
    if (events.empty()) {
        std::cout << "No rewrite events recorded.\n";
        return kExitOk;
    }

    std::cout << "Recent rewrite events:\n";
    for (const auto& ev : events) {
        std::cout << "  " << rewrite::eventTypeToString(ev.type)
                  << " " << ev.json_payload << "\n";
    }
    return kExitOk;
}

int workingState(int argc, char* argv[]) {
    std::string repoRoot = git::Repo::getRoot();
    if (repoRoot.empty()) {
        std::cerr << output::Style::error("Not in a git repository") << "\n";
        return kExitNotInRepo;
    }

    std::string repoArg = getArg(argc, argv, "--repo");
    if (!repoArg.empty()) repoRoot = repoArg;

    std::string key = getArg(argc, argv, "--key");
    if (key.empty()) key = "default";

    if (hasFlag(argc, argv, "--save")) {
        if (rewrite::WorkingState::save(repoRoot, key)) {
            std::cout << "Working state saved.\n";
            return kExitOk;
        }
        std::cerr << "Failed to save working state.\n";
        return kExitError;
    }
    if (hasFlag(argc, argv, "--restore")) {
        if (rewrite::WorkingState::restore(repoRoot, key)) {
            std::cout << "Working state restored.\n";
            return kExitOk;
        }
        std::cerr << "No saved working state found.\n";
        return kExitError;
    }
    if (hasFlag(argc, argv, "--clear")) {
        rewrite::WorkingState::clear(repoRoot, key);
        std::cout << "Working state cleared.\n";
        return kExitOk;
    }

    bool exists = rewrite::WorkingState::exists(repoRoot, key);
    std::cout << "Working state '" << key << "': " << (exists ? "present" : "empty") << "\n";
    return kExitOk;
}

}
}
