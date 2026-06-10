#include "args.hpp"

namespace ghost {
namespace cli {

Args::Args(int argc, char* argv[]) {
    args_.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        args_.push_back(argv[i] ? argv[i] : "");
    }
}

bool Args::hasFlag(const std::string& flag) const {
    for (size_t i = 1; i < args_.size(); ++i) {
        if (args_[i] == flag) return true;
    }
    return false;
}

bool Args::hasAnyFlag(const std::vector<std::string>& flags) const {
    for (const auto& flag : flags) {
        if (hasFlag(flag)) return true;
    }
    return false;
}

std::string Args::getValue(const std::string& flag) const {
    for (size_t i = 0; i + 1 < args_.size(); ++i) {
        if (args_[i] == flag) return args_[i + 1];
    }
    return "";
}

std::vector<std::string> Args::positional(size_t startIndex) const {
    std::vector<std::string> result;
    for (size_t i = startIndex; i < args_.size(); ++i) {
        if (!args_[i].empty() && args_[i][0] == '-') continue;
        result.push_back(args_[i]);
    }
    return result;
}

const std::vector<std::string>& Args::all() const {
    return args_;
}

}
}
