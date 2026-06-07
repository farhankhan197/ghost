# Ghost Bug Fixes

## Fix 1: `src/git/blame.cpp` — Parse dst-line instead of src-line

In the git blame `--line-porcelain` output format `<sha> <src-line> <dst-line> <count>`, the code incorrectly parses `src-line` (2nd field) instead of `dst-line` (3rd field). This causes completely wrong line-to-commit mapping.

**Change** (lines 53-58):

```cpp
// BEFORE:
currentCommit = first;
size_t secondSpace = line.find(' ', space + 1);
if (secondSpace != std::string::npos) {
    try {
        currentLine = std::stoi(line.substr(space + 1, secondSpace - space - 1));
    } catch (...) {}
}

// AFTER:
currentCommit = first;
// Format: <sha> <src-line> <dst-line> <count>
size_t secondSpace = line.find(' ', space + 1);
size_t thirdSpace = line.find(' ', secondSpace + 1);
if (secondSpace != std::string::npos && thirdSpace != std::string::npos) {
    try {
        currentLine = std::stoi(line.substr(secondSpace + 1, thirdSpace - secondSpace - 1));
    } catch (...) {}
}
```

---

## Fix 2: Cross-platform `2>nul` → `2>/dev/null`

Three files use `2>nul` which creates a file called `nul` on Unix instead of suppressing stderr.

### 2a. `src/git/blame.cpp` line 18-19

```cpp
// BEFORE:
cmd = "git blame --line-porcelain -- \"" + file_path + "\" 2>nul";

// AFTER:
cmd = "git blame --line-porcelain -- \"" + file_path + "\" 2>/dev/null";
```

### 2b. `src/git/notes.cpp` line 32

```cpp
// BEFORE:
std::string cmd = "git notes --ref=" + ref + " show " + commit_sha + " 2>nul";

// AFTER:
std::string cmd = "git notes --ref=" + ref + " show " + commit_sha + " 2>/dev/null";
```

### 2c. `src/rewrite/processor.cpp` line 200-201

```cpp
// BEFORE:
std::string cmd = "git -C \"" + repoRoot + "\" notes --ref=" + ref + " show " + sha + " 2>nul";

// AFTER:
std::string cmd = "git -C \"" + repoRoot + "\" notes --ref=" + ref + " show " + sha + " 2>/dev/null";
```

---

## Fix 3: `src/persist/db.cpp` — Guard against nullptr from `sqlite3_column_text`

When a SQLite column is NULL, `sqlite3_column_text()` returns nullptr. Constructing `std::string(nullptr)` is undefined behavior (crash).

### 3a. `loadCheckpoints` (lines 176-178)

```cpp
// BEFORE:
Checkpoint cp;
cp.id = sqlite3_column_int(stmt, 0);
cp.agent = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
cp.model = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
cp.target_file = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
cp.snapshot_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
cp.ts_start = static_cast<time_t>(sqlite3_column_int64(stmt, 5));
cp.processed = sqlite3_column_int(stmt, 6) != 0;

// AFTER:
Checkpoint cp;
cp.id = sqlite3_column_int(stmt, 0);
{ const char* v = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)); cp.agent = v ? v : ""; }
{ const char* v = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)); cp.model = v ? v : ""; }
{ const char* v = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3)); cp.target_file = v ? v : ""; }
{ const char* v = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4)); cp.snapshot_path = v ? v : ""; }
cp.ts_start = static_cast<time_t>(sqlite3_column_int64(stmt, 5));
cp.processed = sqlite3_column_int(stmt, 6) != 0;
```

### 3b. `loadSessions` (lines 234-245)

Same pattern for all string columns:

```cpp
// BEFORE:
Session s;
s.id = sqlite3_column_int(stmt, 0);
s.session_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
s.agent = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
s.model = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
s.author = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
s.ts_start = static_cast<time_t>(sqlite3_column_int64(stmt, 5));
s.ts_end = static_cast<time_t>(sqlite3_column_int64(stmt, 6));
s.additions = sqlite3_column_int(stmt, 7);
s.deletions = sqlite3_column_int(stmt, 8);
s.json_data = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
s.committed = sqlite3_column_int(stmt, 10) != 0;

// AFTER:
Session s;
s.id = sqlite3_column_int(stmt, 0);
{ const char* v = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)); s.session_id = v ? v : ""; }
{ const char* v = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)); s.agent = v ? v : ""; }
{ const char* v = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3)); s.model = v ? v : ""; }
{ const char* v = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4)); s.author = v ? v : ""; }
s.ts_start = static_cast<time_t>(sqlite3_column_int64(stmt, 5));
s.ts_end = static_cast<time_t>(sqlite3_column_int64(stmt, 6));
s.additions = sqlite3_column_int(stmt, 7);
s.deletions = sqlite3_column_int(stmt, 8);
{ const char* v = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9)); s.json_data = v ? v : ""; }
s.committed = sqlite3_column_int(stmt, 10) != 0;
```

### 3c. Other nullable retrievals

In `getNoteIndex` (lines 290-294):
```cpp
// BEFORE:
e.commit_sha = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
e.note_ref = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));

// AFTER:
{ const char* v = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)); e.commit_sha = v ? v : ""; }
{ const char* v = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)); e.note_ref = v ? v : ""; }
```

Same pattern for `getAllNoteIndex` (lines 315-316), `loadRewriteEvents` (lines 363-364), `loadWorkingState` (line 402), `saveRecoverySession` / `loadRecoverySessions` (lines 444-445).

---

## Fix 4: `src/commit/post_commit.cpp` — First commit handling

Replace `git diff --numstat HEAD~1..<sha>` with `git diff-tree` which works for all commits including root.

**Change** (lines 36-51):

```cpp
// BEFORE:
static std::set<std::string> getCommitChangedFiles(const std::string& repoRoot, const std::string& commitSha) {
    std::string range = "HEAD~1.." + commitSha;
    std::string output = runCommand("git diff --numstat " + range + " -- .");
    std::set<std::string> files;
    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty()) continue;
        std::istringstream iss(line);
        std::string adds, dels, path;
        if (iss >> adds >> dels >> path) {
            files.insert(path);
        }
    }
    return files;
}

// AFTER:
static std::set<std::string> getCommitChangedFiles(const std::string& repoRoot, const std::string& commitSha) {
    (void)repoRoot;
    std::string output = runCommand("git diff-tree --no-commit-id -r --name-only " + commitSha + " -- .");
    std::set<std::string> files;
    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty()) files.insert(line);
    }
    return files;
}
```

---

## Fix 5: `src/main.cpp` — `handleDoctor` auto-fix logic

**Change** (lines 867-883):

```cpp
// BEFORE:
    // Check 4b: History rewriting hooks
    {
        std::string hooks[] = {"post-rewrite", "post-merge", "post-checkout", "pre-merge-commit"};
        for (const auto& h : hooks) {
            std::string hookPath = repoRoot + "/.git/hooks/" + h;
            if (!fileExists(hookPath)) {
                std::cout << "  " << Style::warning("⚠ " + h + " hook missing") << "\n";
                if (autoFix) {
                    if (!postCommitExists) ghost::hooks::Installer::installRepo(repoRoot);
                } else {
                    allOk = false;
                }
            } else {
                std::cout << "  " << Style::success("✓ " + h + " hook") << "\n";
            }
        }
    }

// AFTER:
    // Check 4b: History rewriting hooks
    {
        std::string hooks[] = {"post-rewrite", "post-merge", "post-checkout", "pre-merge-commit"};
        bool anyMissing = false;
        for (const auto& h : hooks) {
            std::string hookPath = repoRoot + "/.git/hooks/" + h;
            if (!fileExists(hookPath)) {
                std::cout << "  " << Style::warning("⚠ " + h + " hook missing") << "\n";
                anyMissing = true;
            } else {
                std::cout << "  " << Style::success("✓ " + h + " hook") << "\n";
            }
        }
        if (anyMissing && autoFix) {
            ghost::hooks::Installer::installRepo(repoRoot);
            std::cout << "    " << Style::success("Fixed: installed hooks") << "\n";
        }
        if (anyMissing && !autoFix) {
            allOk = false;
        }
    }
```

---

## Fix 6: `src/main.cpp` — Interactive init agent hooks logic

**Change** (lines 756-761):

```cpp
// BEFORE:
    } else if (interactive) {
        // In interactive mode without agents selected, still install the opencode plugin
        // since it's the default and works for all repos
        if (ghost::hooks::AgentHooks::installAll(repoRoot, false)) {
            std::cout << "  " << Style::success("Installed default agent hooks") << "\n";
        }
    }

// AFTER:
    } // end if (!selectedAgents.empty())
    // (remove the else if (interactive) block entirely)
```

---

## Fix 7: `src/checkpoint/working_log.cpp` — JSON-escape values in `savePreState`

Add an escape helper and use it in `savePreState`.

**Add** this function before `savePreState` (around line 24):

```cpp
static std::string escapeJson(const std::string& str) {
    std::string result;
    for (char c : str) {
        switch (c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default: result += c;
        }
    }
    return result;
}
```

**Change** `savePreState` (lines 24-36):

```cpp
// BEFORE:
void WorkingLog::savePreState(const std::string& repoRoot, const std::string& agent, time_t ts, const std::vector<std::string>& files) {
    ensureGhostDir(repoRoot);
    std::string path = (fs::path(getGhostDir(repoRoot)) / "working.log").string();
    std::ofstream file(path);
    file << "{\"agent\":\"" << agent << "\",\"ts_start\":" << ts << ",\"files\":[";
    for (size_t i = 0; i < files.size(); ++i) {
        if (i > 0) file << ",";
        file << "\"" << files[i] << "\"";
    }
    file << "]}";
}

// AFTER:
void WorkingLog::savePreState(const std::string& repoRoot, const std::string& agent, time_t ts, const std::vector<std::string>& files) {
    ensureGhostDir(repoRoot);
    std::string path = (fs::path(getGhostDir(repoRoot)) / "working.log").string();
    std::ofstream file(path);
    file << "{\"agent\":\"" << escapeJson(agent) << "\",\"ts_start\":" << ts << ",\"files\":[";
    for (size_t i = 0; i < files.size(); ++i) {
        if (i > 0) file << ",";
        file << "\"" << escapeJson(files[i]) << "\"";
    }
    file << "]}";
}
```

---

## Fix 8: `src/git/notes.cpp` — Unique temp file path

**Change** (line 121):

```cpp
// BEFORE:
std::string tmpPath = std::string(".git/ghost-batch-") + safeRef + ".txt";

// AFTER:
std::string tmpPath = std::string(".git/ghost-batch-") + safeRef + "-" + std::to_string(std::time(nullptr)) + ".txt";
```

Add `#include <ctime>` at the top if not already present.

---

## Fix 9: `src/config/ghost_config.cpp` — Trailing newline

**Change** (lines 192-195):

```cpp
// BEFORE:
    for (size_t i = 0; i < lines.size(); ++i) {
        outFile << lines[i];
        if (i + 1 < lines.size()) outFile << "\n";
    }

// AFTER:
    for (const auto& l : lines) {
        outFile << l << "\n";
    }
```

---

## Fix 10: `ghost.yml` — Remove duplicates

Remove lines 15-18 (the second `build/` and second `.git/` entries) so the file reads:

```yaml
# Ghost configuration
# See: https://github.com/farhankhan197/ghost#configuration

version: 1
threshold: 80
required: false
on_exceed: block
pr_comment: true
untagged: human
unverified: warn
gitai_fb: true
owner: farhankhan.code@gmail.com
ignore:
  - .git/
  - STATE.md
  - build/
```
