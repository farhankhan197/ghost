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

### Phase 4: SQLite Persistence + Per-Edit Granularity + History Preservation (Completed)

**Files Created**:
- `src/sqlite/` — SQLite3 via vcpkg (`sqlite3[fts5]`), no longer vendored
- `src/persist/db.hpp/cpp` — Database class with 8 tables: checkpoints, sessions, note_index, rewrite_log, working_state, recovery_sessions. Global singleton `getRepoDb()` / `closeRepoDb()`. WAL mode, synchronous=NORMAL.
- `src/commit/note_index.hpp/cpp` — SHA→note mapping. `update()`, `get()`, `getAll()`, `remove()`, `migrateEntry()`.
- `src/rewrite/rewrite_log.hpp/cpp` — JSONL event types: RebaseStart/Complete/Abort, CherryPickStart/Complete/Abort, Merge, MergeSquash, Reset, CommitAmend, Stash. `append()`, `load()`, `readStdinMappings()` for post-rewrite hook stdin.
- `src/rewrite/processor.hpp/cpp` — Note migration: `processRebase()` (copy notes original→new SHAs), `processAmend()` (move note to amended SHA), `processCherryPick()`, `processMergeSquash()` (save working state), `processReset()` (recovery sessions for unwound commits), `detectStashPop()` (restore working state). `copyNote()` via `git notes copy`.
- `src/rewrite/working_state.hpp/cpp` — `save()` / `restore()` / `clear()` / `exists()` for sessions across destructive git operations.
- `src/checkpoint/snapshot.hpp/cpp` — `captureSingle()` added for per-file snapshots.
- `src/checkpoint/main.cpp` — `--file` flag for per-edit checkpoint granularity.
- `src/commit/post_commit.cpp` — Merges DB uncommitted sessions + DB recovery sessions + legacy file-based sessions into single note. Updates note_index. Clears checkpoints/sessions/recovery after commit.
- `src/hooks/installer.cpp` — Added `post-rewrite`, `post-merge`, `post-checkout`, `pre-merge-commit` hooks. Raw string literals use `R"HOOK(...)"HOOK` delimiter.
- `src/main.cpp` — New handlers: `handleRewriteLog()`, `handleWorkingState()`. Updated `handleDoctor()` (checks 4 new hooks), `handleStatus()` (shows DB checkpoints, sessions, note index).
- `src/cli/commands.cpp` — Registered `rewrite-log` (`rl`) and `working-state` (`ws`) commands.
- `.opencode/plugins/ghost.ts` — Plugin updated to extract `input.path`/`input.file`/`input.files[0]` and pass `--file` to checkpoint.

**Tests**: 4 new integration tests — `DbCreateAndCheckpoint`, `RewriteLogAppendAndRead`, `NoteIndexRoundTrip`, `WorkingStateSaveRestore`

**Total Test Results**: 47/47 tests passing (38 unit + 9 integration)

### Phase 5: Post-Commit Note Writer (Completed)

**Files Updated**:
- `src/commit/post_commit.hpp/cpp` — reads sessions from DB + legacy → writes both git notes → cleanup

### Phase 6: Audit Engine (Completed)

**Files Created**:
- `src/audit/auditor.hpp/cpp` — orchestrate: fetch notes → blame → overlay → aggregate → policy
- `src/audit/blame_overlay.hpp/cpp` — overlay ghost notes onto blame output
- `src/audit/aggregator.hpp/cpp` — count AI lines / total lines per file, per commit
- `src/audit/policy.hpp/cpp` — enforce threshold + unverified_policy from ghost.yml

**Output**:
- `src/output/report.hpp/cpp` — CLI tables, JSON output, streaming with animations
- `src/output/style.hpp/cpp` — ANSI colors, spinner, progress bar, mascot
- `src/output/color.hpp/cpp` — TTY detection, NO_COLOR support

### Phase 7: Hook Installer (Completed)

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

### Phase 8: Config System (Completed)

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

### Phase 9: Distribution (Completed)

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

### Phase 10: Testing (Completed)

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

**Test Results**: 47/47 tests passing (38 unit + 9 integration)

---

### Phase 11: Blame Overlay Fix + Status Timeline (Completed)

**Problem**: `ghost blame` only checked HEAD's ghost note — the overlay showed `is_ai: false` for every line from anything other than the most recent commit. The stale `.current_model` file leaked model names from closed AI sessions into new repos.

**Fixes**:

- `src/main.cpp:260-266` — `handleBlame` now collects all unique commit SHAs from `blame.lines` and fetches ghost notes via `showBatch` (same batch mechanism the auditor uses). Previously only checked `Notes::show(refs/notes/ghost, headSha)`.
- `.opencode/plugins/ghost.ts` — `writeModelFile` now uses `unlinkSync` instead of `writeFileSync("")` when model is empty. `session.updated` always calls `writeModelFile` (never skips), ensuring `.current_model` is deleted when no session is active.
- `src/hooks/installer.cpp` `PLUGIN_CONTENT` — Same fix applied to template so future `ghost init` installs get the corrected plugin.
- `src/main.cpp:999-1049` — `handleStatus` enhanced with live uncommitted checkpoint timeline: cumulative bar (AI add %) + per-session rows with `timeAgo`, `+adds`, `-dels`, `agent/model`. Reads uncommitted sessions from DB sorted by `ts_start` DESC.

**Verification**:
- Cross-repo AI edit tracking: Plugin writes `ghost/testing/test.txt` → checkpoint routed to ghost repo's DB → commit → `ghost audit` shows correct AI counts
- Cross-repo checkpoint isolation: Session for ghost repo's own file does NOT leak into test-repo's DB
- Stale model fix: After closing AI session, `.current_model` is absent → checkpoint `post` picks up `currentModel = "unknown"` instead of stale `qwen3.6-plus-free`
- Blame overlay: `ghost blame src/main.cpp --json` shows `is_ai: true` for 519/1516 lines (34%) from historical commits, not just HEAD
- All 47 tests pass

**Files Updated**:
- `src/main.cpp` — blame overlay (all-commit `showBatch`), status timeline
- `.opencode/plugins/ghost.ts` — stale model cleanup
- `src/hooks/installer.cpp` — same fix in `PLUGIN_CONTENT` template

---

## 🔄 Remaining Tasks

### Low Priority

| Item | Status | Notes |
|------|--------|-------|
| GitAiReader stub | Not started | Fallback when ghost notes absent |
| Man pages / `--help` per command | Not started | Currently generic usage only |
| Arrow-key TUI | Done | Raw ANSI, zero deps, cross-platform |
| `ghost init` / `ghost doctor` / `ghost status` | Done | Repo setup, diagnostics, overview (status now has live timeline) |
| Fix `ghost check` staged audit | Done | Predictive AI% using active sessions |
| Performance profiling | Done | `GHOST_BENCHMARK=1` per-phase timing |
| Fix deterministic session IDs | Done | `std::random_device` + `std::mt19937` |
| SQLite persistence | Done | 8 tables, WAL mode, bundled amalgamation |
| Per-file checkpoint | Done | `--file` flag for per-edit granularity |
| History rewrite preservation | Done | rebase, amend, cherry-pick, merge --squash, reset --soft, stash pop |
| Working state recovery | Done | Save/restore across destructive git ops |
| Note index | Done | SHA→note mapping in SQLite |
| Blame overlay for all commits | Done | `showBatch` replaces single-HEAD lookup |
| Stale `.current_model` cleanup | Done | Plugin deletes file when model empty |
| Status live timeline | Done | Checkpoint bar + per-session rows with timeAgo |

### Remaining Gaps vs git-ai

| Feature | Priority | Notes |
|---------|----------|-------|
| Multi-agent hook install | High | `ghost install-hooks` for Cursor, Claude Code, Copilot, Codex, Gemini |
| `ghost diff` attribution | Medium | Per-line AI attribution in diffs |
| Prompt preservation | Medium | Wire prompt text into checkpoint sessions |
| Squash authorship | Low | Reconstruct AI/human authorship after squash |
| Post-clone hook | Low | Auto-fetch ghost notes from origin on clone |
| IDE integration | Low | VS Code extension, JetBrains plugin |

### Known Issues

- Windows CRLF warnings from git (LF in repo, CRLF in working tree)
- `git diff --no-index` paths use forward slashes on Windows — works but may have edge cases
- No JSON library — manual parsing is fragile for complex/edge-case JSON
- TUI cursor state may not restore on SIGINT during interactive menus
- SQLite file locking on Windows requires `closeRepoDb()` + retry loop in tests
- `git notes copy <from> <to>` requires git 2.6+ (available since 2015)
- Working state recovery for `reset --soft` may accumulate recovery sessions if user never commits again

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
│   │   ├── main.cpp            ← ghost-checkpoint CLI (pre/post/show/reset with --file)
│   │   ├── working_log.hpp/cpp ← .git/ghost/ legacy state management
│   │   ├── snapshot.hpp/cpp    ← pre-hook: capture current state (single-file + all modified)
│   │   └── session.hpp/cpp     ← post-hook: compute diff → session JSON
│   │
│   ├── commit/
│   │   ├── post_commit.hpp/cpp ← reads DB sessions → writes notes → cleanup
│   │   └── note_index.hpp/cpp  ← SHA→note mapping in SQLite
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
│   ├── hooks/
│   │   ├── installer.hpp/cpp   ← install/uninstall + bootstrap + pre-push + post-rewrite + post-merge + post-checkout + pre-merge-commit
│   │   ├── agent_hooks.hpp/cpp  ← Claude, Cursor, Copilot, Codex, Gemini
│   │   └── agent_detector.hpp/cpp ← detect installed agents
│   │
│   ├── persist/
│   │   └── db.hpp/cpp           ← SQLite persistence layer (8 tables, WAL mode)
│   │
│   ├── rewrite/
│   │   ├── rewrite_log.hpp/cpp ← JSONL rewrite event log
│   │   ├── processor.hpp/cpp   ← Note migration across history rewrites
│   │   └── working_state.hpp/cpp ← Save/restore working state across git ops
│   │
│   ├── sqlite/
│   │   └── (sqlite3 provided by vcpkg — no vendored files)
│   │
│   └── util/
│       └── thread_pool.hpp      ← header-only thread pool for parallel git blame
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
        ├── test_installer.cpp   ← temp git repo smoke test
        └── test_rewrite.cpp     ← SQLite + rewrite log + note index + working state
```

---

## 📝 Key Decisions

1. **Manual JSON** — No nlohmann-json dependency. All JSON is hand-serialized in writer.cpp and hand-parsed in reader.cpp.
2. **No vcpkg** — Build works without any package manager. Only system deps: cmake, ninja, git, C++ compiler.
3. **Standalone pre-push hook** — The shell script in `.git/hooks/pre-push` works without ghost binary installed. Reads `ghost.yml` with grep and checks notes with git commands.
4. **Bootstrap confirmation** — `ghost install` detects unpushed commits without notes and asks for confirmation. This prevents retroactive attribution gaming.
5. **Model detection** — Opencode plugin writes current model to `~/.ghost/.current_model` file. Checkpoint reads this during post-hook.
6. **Cross-platform paths** — All internal paths use forward slashes. Windows MSYS2 git handles them correctly.
7. ~~**SQLite amalgamation** — Bundled as single C file (`src/sqlite/sqlite3.c`, ~9MB source) to maintain zero external runtime dependency.~~ **Now fetched via vcpkg** (`sqlite3[fts5]`), linked as `unofficial::sqlite3::sqlite3`.
8. **Subprocess + batching strategy** — Validated against git-ai which uses `gix` (Rust-native git lib). No need to migrate to libgit2; batching gives sufficient performance.
9. **Per-edit granularity** — `--file` arg in checkpoint, not full IDE plugin rewrite. Plugin extracts file path from tool input heuristically (`input.path`, `input.file`, `input.files[0]`).
10. **History rewriting** — Shell scripts call `ghost rewrite-log --stdin` / `ghost working-state --save` — no daemon, no Rust interop, minimal complexity.
11. **Note migration** — Uses `git notes copy <from> <to>` (available since git 2.6+).
12. **Working state recovery for `reset --soft`** — Commits between old HEAD and new HEAD are unwound, their notes read, stored as recovery sessions, merged into next commit.
13. **Multiple checkpoints per commit** — DB accumulates checkpoints, post-commit merges all into single note. Last-write-wins for overlapping ranges by timestamp.

---

*Last Updated: June 7, 2026*
