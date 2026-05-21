# Ghost Project — Implementation Steps

This document records all steps taken and remaining for building the ghost project.

---

## Project Overview

**Goal**: Build a C++ CLI tool for AI code attribution using git notes.

**Dependencies**:
- CMake 3.20+
- C++20
- MinGW (on Windows) or gcc/clang (on Linux/macOS)
- Git (for hooks, notes, blame, diff operations)

**No external runtime dependencies** — manual JSON serialization, manual YAML parsing.

---

## ✅ Completed Phases

### Phase 0: Project Setup (Completed)

- Directory structure: `src/note`, `src/git`, `src/checkpoint`, `src/commit`, `src/audit`, `src/output`, `src/config`, `src/hooks`
- CMakeLists.txt with two binaries (`ghost`, `ghost-checkpoint`) + gtest via FetchContent
- Subdirectory CMakeLists.txt for `note` and `git` libraries

### Phase 1: Core Note System (Completed)

**Files Created**:
- `src/note/line_range.hpp/cpp` — parse/serialize "5-12,18,22-30" with range merging
- `src/note/writer.hpp/cpp` — serialize to ghost/1.0.0 format
- `src/note/reader.hpp/cpp` — parse ghost notes back into data structures
- `src/note/verified_writer.hpp/cpp` — serialize ghost-verified note
- `src/note/verified_reader.hpp/cpp` — full JSON parsing
- `src/note/gitai_reader.hpp/cpp` — stub (returns "not implemented")

**Tests**: 15 unit tests for LineRangeSet, 7 for NoteReader, 4 for NoteWriter, 3 for VerifiedWriter, 3 for VerifiedReader

### Phase 2: Git Wrappers (Completed)

**Files Created**:
- `src/git/notes.hpp/cpp` — git notes show/write/exists via popen
- `src/git/repo.hpp/cpp` — getRoot, getHead, isRepo
- `src/git/blame.hpp/cpp` — full porcelain blame parsing with `--line-porcelain`
- `src/git/diff.hpp/cpp` — numstat diff parsing

### Phase 3: Checkpoint Binary (Completed)

**Files Created**:
- `src/checkpoint/main.cpp` — CLI entry point (pre/post/show/reset)
- `src/checkpoint/working_log.hpp/cpp` — `.git/ghost/` state management
- `src/checkpoint/snapshot.hpp/cpp` — pre-hook: capture current state
- `src/checkpoint/session.hpp/cpp` — post-hook: compute diff, write session JSON

### Phase 4: Post-Commit Note Writer (Completed)

**Files Created**:
- `src/commit/post_commit.hpp/cpp` — reads sessions → writes both git notes → cleanup

### Phase 5: Audit Engine (Completed)

**Files Created**:
- `src/audit/auditor.hpp/cpp` — orchestrate: fetch notes → blame → overlay → aggregate → policy
- `src/audit/blame_overlay.hpp/cpp` — overlay ghost notes onto blame output
- `src/audit/aggregator.hpp/cpp` — count AI lines / total lines per file, per commit
- `src/audit/policy.hpp/cpp` — enforce threshold + unverified_policy from ghost.yml

**Output**:
- `src/output/report.hpp/cpp` — CLI tables, JSON output, streaming with animations
- `src/output/style.hpp/cpp` — ANSI colors, spinner, progress bar, mascot
- `src/output/color.hpp/cpp` — TTY detection, NO_COLOR support

### Phase 6: Hook Installer (Completed)

**Files Created**:
- `src/hooks/installer.hpp/cpp` — ghost install/uninstall + bootstrap + pre-push hook
- `src/hooks/agent_hooks.hpp/cpp` — hook writer for Claude, Cursor, Copilot, Codex, Gemini
- `src/hooks/agent_detector.hpp/cpp` — detect installed agents and their config paths

**Features**:
- `ghost install` — copies binaries, creates plugin, writes hooks, configures git, bootstrap step
- `ghost install --global` — creates `~/.config/opencode/plugins/ghost.ts`
- `ghost install-hooks` / `ghost uninstall-hooks` — per-agent or all
- Pre-push hook — standalone shell script, no ghost binary dependency
- Bootstrap step — detects unpushed commits, confirms human authorship, logs to `.git/ghost/bootstrap.log`

### Phase 7: Config System (Completed)

**Files Created**:
- `src/config/ghost_config.hpp/cpp` — read/write `ghost.yml`

**Defaults**:
- `version: 1`
- `required: false`
- `threshold: 80`
- `on_exceed: block`
- `pr_comment: true`
- `untagged_policy: human`
- `unverified_policy: warn`
- `gitai_fallback: true`

### Phase 8: Distribution (Completed)

**Files Created**:
- `install.sh` — universal install script (mac/linux/wsl)
- `install.ps1` — PowerShell install script (windows)
- `package.json` — npm package `ghost-ai`
- `scripts/install.js` — npm postinstall: downloads binaries from GitHub Releases
- `bin/ghost`, `bin/ghost-checkpoint` — bash wrappers for npm
- `bin/ghost.cmd`, `bin/ghost-checkpoint.cmd` — Windows cmd wrappers for npm
- `bin/ghost.js`, `bin/ghost-checkpoint.js` — Node.js wrappers for npm
- `winget/farhankhan197.ghost-ai.*.yaml` — winget manifests
- `homebrew/ghost-ai.rb` — homebrew formula
- `scoop/ghost-ai.json` — scoop manifest

**CI Workflows**:
- `.github/workflows/release.yml` — build binaries for win/linux/macos x86_64+arm64, create GitHub Release, publish to npm
- `.github/workflows/ci.yml` — run tests on PR/push to main
- `.github/workflows/ghost-audit.yml` — PR audit + markdown comment posting

### Phase 9: Testing (Completed)

**Files Created**:
- `tests/CMakeLists.txt` — test executable target
- `tests/unit/test_line_range.cpp` — 15 unit tests
- `tests/unit/test_note_writer.cpp` — 4 unit tests
- `tests/unit/test_note_reader.cpp` — 7 unit tests
- `tests/unit/test_verified_writer.cpp` — 3 unit tests
- `tests/unit/test_ghost_config.cpp` — 5 unit tests
- `tests/integration/test_checkpoint.cpp` — temp git repo smoke test
- `tests/integration/test_post_commit.cpp` — temp git repo smoke test
- `tests/integration/test_audit.cpp` — temp git repo with commits
- `tests/integration/test_installer.cpp` — temp git repo smoke test

**Test Results**: 43/43 tests passing (38 unit + 5 integration)

---

## 🔄 Remaining Tasks

### Low Priority

| Item | Status | Notes |
|------|--------|-------|
| GitAiReader stub | Not started | Fallback when ghost notes absent |
| Man pages / `--help` per command | Not started | Currently generic usage only |
| Performance profiling | Not started | Target: <50ms checkpoint, <500ms audit |
| Fix deterministic session IDs | Not started | `std::rand()` not seeded |

### Known Issues

- Windows CRLF warnings from git (LF in repo, CRLF in working tree)
- Session ID is deterministic (`sess_93e41c6e2091`) — `std::rand()` not seeded
- `git diff --no-index` paths use forward slashes on Windows — works but may have edge cases
- No JSON library — manual parsing is fragile for complex/edge-case JSON

---

## 📋 Current Project Structure

```
ghost/
├── CMakeLists.txt              ← main build config + FetchContent for gtest
├── README.md                   ← user-facing documentation
├── STATE.md                    ← architecture + implementation status
├── STEPS.md                    ← this file: implementation log
│
├── install.sh                  ← universal install script
├── install.ps1                 ← PowerShell install script
├── package.json                ← npm wrapper: ghost-ai
├── .npmignore                  ← npm package exclusions
│
├── .github/workflows/
│   ├── release.yml             ← build + release binaries + publish npm
│   ├── ci.yml                  ← run tests on PR/push
│   └── ghost-audit.yml         ← PR audit + comment posting
│
├── winget/                     ← Windows Package Manager manifests
├── homebrew/                   ← Homebrew formula
├── scoop/                      ← Scoop manifest
│
├── .opencode/plugins/
│   └── ghost.ts                ← opencode plugin (triggers pre/post)
│
├── src/
│   ├── main.cpp                ← ghost CLI entry point
│   │
│   ├── checkpoint/
│   │   ├── main.cpp            ← ghost-checkpoint CLI
│   │   ├── working_log.hpp/cpp ← .git/ghost/ state management
│   │   ├── snapshot.hpp/cpp    ← pre-hook: capture current state
│   │   └── session.hpp/cpp     ← post-hook: compute diff → session JSON
│   │
│   ├── commit/
│   │   └── post_commit.hpp/cpp ← reads sessions → writes notes → cleanup
│   │
│   ├── note/
│   │   ├── line_range.hpp/cpp  ← parse/serialize line ranges
│   │   ├── writer.hpp/cpp      ← ghost/1.0.0 format serialization
│   │   ├── reader.hpp/cpp      ← ghost note parsing
│   │   ├── verified_writer.hpp/cpp ← ghost-verified note serialization
│   │   ├── verified_reader.hpp/cpp ← ghost-verified note parsing
│   │   └── gitai_reader.hpp/cpp    ← stub
│   │
│   ├── git/
│   │   ├── notes.hpp/cpp       ← git notes show/write/exists
│   │   ├── repo.hpp/cpp        ← repo root/head detection
│   │   ├── blame.hpp/cpp       ← porcelain blame parsing
│   │   └── diff.hpp/cpp        ← numstat diff parsing
│   │
│   ├── audit/
│   │   ├── auditor.hpp/cpp     ← orchestration: fetch → blame → overlay → aggregate → policy
│   │   ├── blame_overlay.hpp/cpp ← overlay notes onto blame
│   │   ├── aggregator.hpp/cpp  ← count AI lines per file/commit
│   │   └── policy.hpp/cpp      ← threshold + unverified policy enforcement
│   │
│   ├── output/
│   │   ├── report.hpp/cpp      ← CLI tables, JSON, streaming animations
│   │   ├── style.hpp/cpp       ← ANSI colors, spinner, progress bar, mascot
│   │   └── color.hpp/cpp       ← TTY detection, NO_COLOR
│   │
│   ├── config/
│   │   └── ghost_config.hpp/cpp ← read/write ghost.yml
│   │
│   └── hooks/
│       ├── installer.hpp/cpp   ← install/uninstall + bootstrap + pre-push
│       ├── agent_hooks.hpp/cpp  ← Claude, Cursor, Copilot, Codex, Gemini
│       └── agent_detector.hpp/cpp ← detect installed agents
│
└── tests/
    ├── CMakeLists.txt           ← test executable config
    ├── unit/
    │   ├── test_line_range.cpp  ← 15 tests: parse, serialize, merge, edge cases
    │   ├── test_note_writer.cpp ← 4 tests: structure, round-trip, empty, special chars
    │   ├── test_note_reader.cpp ← 7 tests: valid, empty, missing sep, multiple entries
    │   ├── test_verified_writer.cpp ← 3 tests: structure, zero sessions, round-trip
    │   └── test_ghost_config.cpp ← 5 tests: defaults, custom, save/load, ignore, invalid
    └── integration/
        ├── test_checkpoint.cpp  ← temp git repo smoke test
        ├── test_post_commit.cpp ← temp git repo smoke test
        ├── test_audit.cpp       ← temp git repo with commits
        └── test_installer.cpp   ← temp git repo smoke test
```

---

## 📝 Key Decisions

1. **Manual JSON** — No nlohmann-json dependency. All JSON is hand-serialized in writer.cpp and hand-parsed in reader.cpp.
2. **No vcpkg** — Build works without any package manager. Only system deps: cmake, ninja, git, C++ compiler.
3. **Standalone pre-push hook** — The shell script in `.git/hooks/pre-push` works without ghost binary installed. Reads `ghost.yml` with grep and checks notes with git commands.
4. **Bootstrap confirmation** — `ghost install` detects unpushed commits without notes and asks for confirmation. This prevents retroactive attribution gaming.
5. **Model detection** — Opencode plugin writes current model to `~/.ghost/.current_model` file. Checkpoint reads this during post-hook.
6. **Cross-platform paths** — All internal paths use forward slashes. Windows MSYS2 git handles them correctly.

---

*Last Updated: May 21, 2026*
