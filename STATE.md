# Ghost — Project State Summary

> Git Hook for Origin Source Tracking — AI code attribution for git repos using cryptographically-linked git notes.

## Architecture

Two binaries:
- **`ghost`** — main CLI (install, show, post-commit, version, audit, blame, stats, config)
- **`ghost-checkpoint`** — lightweight binary called by agent hooks (pre/post)

Data flow:
```
opencode edit/write/apply_patch
  → tool.execute.before → ghost-checkpoint pre --agent opencode
    → snapshots modified files to .git/ghost/snapshot/
    → saves metadata to .git/ghost/working.log
  → AI edits files
  → tool.execute.after → ghost-checkpoint post --agent opencode --model <model>
    → diffs snapshot vs current state per file
    → extracts changed line ranges via git diff --no-index --unified=0
    → writes session JSON to .git/ghost/sessions/<id>.json
git commit
  → .git/hooks/post-commit → ghost post-commit
    → reads all session JSON files
    → builds authorship log + session map
    → writes refs/notes/ghost (attribution, if sessions > 0)
    → writes refs/notes/ghost-verified (installation witness, always)
    → cleans up .git/ghost/sessions/
```

## Note Schemas

### `refs/notes/ghost` — Attribution note
```
src/main.cpp
  sess_a1b2c3 5-12,18,22-30
---
{
  "schema": "ghost/1.0.0",
  "commit": "<sha>",
  "sessions": {
    "sess_a1b2c3": {
      "session_id": "sess_a1b2c3",
      "agent": "opencode",
      "model": "claude-sonnet-4-5",
      "author": "Name <email>",
      "ts_start": 1710000000,
      "ts_end": 1710000033,
      "additions": 85,
      "deletions": 3
    }
  }
}
```

### `refs/notes/ghost-verified` — Installation witness (always written)
```json
{
  "schema": "ghost-verified/1.0.0",
  "ghost_version": "1.0.0",
  "commit": "<sha>",
  "ts": 1710000042,
  "author": "Name <email>",
  "sessions": 2
}
```

## Project Structure

```
ghost/
├── CMakeLists.txt
├── README.md
├── STATE.md                     ← this file
├── STEPS.md                     ← implementation log
│
├── .github/
│   └── workflows/
│       ├── release.yml          ← build binaries for win/linux/macos + publish to npm
│       └── ci.yml               ← run tests on PR/push to main
│       └── ghost-audit.yml      ← PR audit + comment posting
│
├── install.sh                   ← universal install script (mac/linux/wsl)
├── install.ps1                  ← PowerShell install script (windows)
├── init.sh                      ← npx-style one-liner: download + run `ghost init --yes`
├── init.ps1                     ← PowerShell npx-style one-liner
├── package.json                 ← npm wrapper: ghost-ai
├── .npmignore
│
├── winget/
│   └── farhankhan197.ghost-ai.* ← winget manifests
├── homebrew/
│   └── ghost-ai.rb              ← homebrew formula
├── scoop/
│   └── ghost-ai.json            ← scoop manifest
│
├── .opencode/
│   └── plugins/
│       └── ghost.ts             ← opencode plugin (triggers pre/post on edit/write/apply_patch)
│
├── src/
│   ├── main.cpp                 ← ghost CLI entry point
│   │
│   ├── checkpoint/
│   │   ├── main.cpp             ← ghost-checkpoint CLI (pre/post/show/reset)
│   │   ├── working_log.cpp/h    ← .git/ghost/ state management
│   │   ├── snapshot.cpp/h       ← pre-hook: git diff --name-only → copy files
│   │   └── session.cpp/h        ← post-hook: git diff --no-index → line ranges → JSON
│   │
│   ├── commit/
│   │   └── post_commit.cpp/h    ← reads sessions → writes both git notes → cleanup
│   │
│   ├── note/
│   │   ├── line_range.cpp/h     ← parse/serialize "5-12,18,22-30" with range merging ✅
│   │   ├── writer.cpp/h         ← serialize to ghost/1.0.0 format ✅
│   │   ├── reader.cpp/h         ← parse ghost notes back into data structures ✅
│   │   ├── verified_writer.cpp/h← serialize ghost-verified note ✅
│   │   ├── verified_reader.cpp/h← full JSON parsing ✅
│   │   └── gitai_reader.cpp/h   ← stub (returns "not implemented")
│   │
│   ├── git/
│   │   ├── notes.cpp/h          ← git notes show/write/exists via popen ✅
│   │   ├── repo.cpp/h           ← getRoot/getHead/isRepo via popen ✅
│   │   ├── blame.cpp/h          ← full porcelain blame parsing ✅
│   │   └── diff.cpp/h           ← numstat diff parsing ✅
│   │
│   ├── audit/
│   │   ├── auditor.cpp/h        ← orchestrate: fetch notes → blame → overlay → aggregate → policy
│   │   ├── blame_overlay.cpp/h  ← overlay ghost notes onto blame output
│   │   ├── aggregator.cpp/h     ← count AI lines / total lines per file, per commit
│   │   └── policy.cpp/h         ← enforce threshold + unverified_policy from ghost.yml
│   │
│   ├── output/
│   │   ├── report.cpp/h         ← CLI table output + JSON + streaming
│   │   ├── style.cpp/h          ← ANSI colors, spinner, progress bar, mascot
│   │   ├── interactive.cpp/h    ← arrow-key TUI menus, prompts, wizard (raw ANSI, zero deps)
│   │   └── color.cpp/h          ← TTY detection, NO_COLOR support ✅
│   │
│   ├── config/
│   │   └── ghost_config.cpp/h   ← read/write ghost.yml
│   │
│   └── hooks/
│       ├── installer.cpp/h      ← ghost install/uninstall + bootstrap + pre-push hook
│       ├── agent_hooks.cpp/h    ← hook writer for Claude, Cursor, Copilot, Codex, Gemini
│       └── agent_detector.cpp/h ← detect installed agents and their config paths
│   │
│   └── util/
│       └── thread_pool.hpp      ← header-only thread pool for parallel git blame
│
├── tests/
│   ├── CMakeLists.txt
│   ├── unit/
│   │   ├── test_line_range.cpp
│   │   ├── test_note_writer.cpp
│   │   ├── test_note_reader.cpp
│   │   ├── test_verified_writer.cpp
│   │   └── test_ghost_config.cpp
│   └── integration/
│       ├── test_checkpoint.cpp
│       ├── test_post_commit.cpp
│       ├── test_audit.cpp
│       └── test_installer.cpp
│
└── build/
    ├── ghost.exe
    └── ghost-checkpoint.exe
```

## Build System

```cmake
cmake_minimum_required(VERSION 3.20)
project(ghost VERSION 1.0.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
```

- **ghost-checkpoint**: main.cpp + working_log.cpp + snapshot.cpp + session.cpp + note + git libs
- **ghost**: main.cpp + post_commit.cpp + working_log.cpp + installer.cpp + agent_hooks + agent_detector + audit + output + config + note + git libs
- **ghost-tests**: unit + integration tests, gtest via FetchContent
- No external dependencies in runtime — manual JSON serialization (no nlohmann-json)

Build:
```bash
cd build && cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release && ninja
```

Run tests:
```bash
cd build && ninja ghost-tests && ctest --output-on-failure
```

## CLI Reference

### `ghost`
```
ghost init                 Initialize repo (config + hooks, no binaries)
ghost init --yes           Also install binaries if missing
ghost init --interactive   Guided setup wizard with arrow-key TUI
ghost init --dry-run       Preview what would be configured
ghost install              Install in current repo (binaries + plugin + hooks + bootstrap)
ghost install --global     Install plugin for ALL repos (~/.config/opencode/plugins/ghost.ts)
ghost uninstall            Remove from current repo
ghost uninstall --global   Remove global plugin
ghost install-hooks        Auto-configure AI agent hooks
ghost uninstall-hooks      Remove AI agent hooks
ghost show <commit>        Show formatted AI attribution for a commit
ghost audit [sha]          Run AI attribution audit (codebase blame view)
ghost audit --all          Audit all commits with ghost notes
ghost audit --range R      Audit commit range
ghost audit --json         Output JSON
ghost check                Predictive pre-commit audit (staged changes)
ghost check --json         JSON output
ghost blame <file>         Line-by-line attribution
ghost stats                AI% stats for HEAD
ghost stats --json         JSON output
ghost config               Show ghost.yml values
ghost config set K V       Set ghost.yml key = value
ghost doctor               Diagnose ghost setup and suggest fixes
ghost doctor --fix         Auto-fix issues where possible
ghost status               Show ghost status overview for this repo
ghost post-commit          Run post-commit hook (reads sessions → writes notes)
ghost version              Print version info
ghost completion <shell>   Generate shell completion script (bash/zsh/fish)
```

### `ghost-checkpoint`
```
ghost-checkpoint pre  --agent <name>                        Capture snapshot
ghost-checkpoint post --agent <name> --model <model>        Record session
ghost-checkpoint show                                      Show active session
ghost-checkpoint reset                                     Clear pre-state
```

## Implementation Status

| Component | Status | Notes |
|-----------|--------|-------|
| LineRangeSet | ✅ Done | Parse/serialize with range merging. Tests: 15 cases |
| NoteWriter | ✅ Done | Manual JSON serialization. Tests: 4 cases |
| NoteReader | ✅ Done | Parses top section + JSON sessions map. Tests: 7 cases |
| VerifiedWriter | ✅ Done | Pure JSON output. Tests: 3 cases |
| VerifiedReader | ✅ Done | Full JSON parsing. Tests: 3 cases |
| GitAiReader | ⚠️ Stub | Returns "not implemented" |
| Repo | ✅ Done | getRoot, getHead, isRepo |
| Notes | ✅ Done | show, write (via temp file), exists |
| Blame | ✅ Done | Full porcelain blame parsing with `--line-porcelain` |
| Diff | ✅ Done | Numstat diff parsing |
| Checkpoint pre | ✅ Done | Snapshots modified files |
| Checkpoint post | ✅ Done | Diffs, extracts ranges, writes session JSON |
| Post-commit | ✅ Done | Reads sessions, writes both notes, cleanup |
| Installer | ✅ Done | install/uninstall repo + global + bin + bootstrap + pre-push hook |
| Bootstrap | ✅ Done | Detects unpushed commits, confirms human authorship, logs to `.git/ghost/bootstrap.log` |
| Pre-push hook | ✅ Done | Standalone shell script: checks notes, first-push prompt, blocks subsequent pushes |
| OpenCode plugin | ✅ Done | Hooks edit/write/apply_patch, writes `.current_model` |
| Agent Hooks | ✅ Done | Claude, Cursor, Copilot, Codex, Gemini detection + installation |
| Agent Detector | ✅ Done | Detects installed agents and their config paths |
| Audit engine | ✅ Done | blame overlay, aggregation, policy enforcement, codebase blame view |
| Output | ✅ Done | CLI tables, JSON, streaming animations, spinner, progress bars |
| Config | ✅ Done | ghost.yml read/write with defaults |
| CI Integration | ✅ Done | `.github/workflows/ghost-audit.yml` — PR audit + markdown comment posting |
| Tests | ✅ Done | 43 tests (38 unit + 5 integration), gtest via FetchContent |
| Distribution | ✅ Done | install.sh, install.ps1, npm package `ghost-ai`, winget, homebrew, scoop manifests |
| Arrow-key TUI | ✅ Done | `output/interactive.cpp/h` — raw ANSI, zero deps, cross-platform |
| `ghost init` | ✅ Done | Repo scaffolding (config + hooks), `--interactive` wizard, `--yes` for binaries |
| `ghost doctor` | ✅ Done | Setup diagnostics with `--fix` auto-repair |
| `ghost status` | ✅ Done | Quick config + hook + session + commit overview |
| `ghost check` | ✅ Done | Predictive staged audit using active checkpoint sessions |
| Batch notes retrieval | ✅ Done | `git cat-file --batch` — 97% faster note fetching |
| Parallel blame | ✅ Done | Thread pool + `std::async` — 74% faster blame phase |
| Batch authors | ✅ Done | `git log --no-walk` — 97% fewer author popens |
| Performance benchmark | ✅ Done | `GHOST_BENCHMARK=1` env var for per-phase timing |

### Completed Phases

**Phase 1 — Core Note System**
- [x] `note/line_range.cpp/h` — parse/serialize line ranges
- [x] `note/writer.cpp/h` — serialize ghost/1.0.0 format
- [x] `note/reader.cpp/h` — parse ghost notes
- [x] `note/verified_writer.cpp/h` — ghost-verified note
- [x] `note/verified_reader.cpp/h` — full JSON parsing

**Phase 2 — Checkpoint Binary**
- [x] `checkpoint/main.cpp` — CLI entry point
- [x] `checkpoint/working_log.cpp/h` — `.git/ghost/` state
- [x] `checkpoint/snapshot.cpp/h` — pre-hook snapshot
- [x] `checkpoint/session.cpp/h` — post-hook diff + session JSON

**Phase 3 — Post-Commit**
- [x] `commit/post_commit.cpp/h` — reads sessions → writes notes → cleanup

**Phase 4 — Audit Engine**
- [x] `git/blame.cpp` — `git blame --line-porcelain` parsing
- [x] `git/diff.cpp` — numstat diff parsing
- [x] `audit/blame_overlay.cpp/h` — overlay notes onto blame
- [x] `audit/aggregator.cpp/h` — count AI lines per file/commit
- [x] `audit/policy.cpp/h` — threshold + unverified policy
- [x] `audit/auditor.cpp/h` — orchestration

**Phase 5 — Output + CI Integration**
- [x] `output/report.cpp/h` — CLI tables + JSON + streaming
- [x] `output/style.cpp/h` — colors, spinner, progress bar, mascot
- [x] `.github/workflows/ghost-audit.yml` — PR audit + comment

**Phase 6 — Hook Installer**
- [x] `hooks/installer.cpp/h` — install/uninstall + bootstrap + pre-push
- [x] `hooks/agent_hooks.cpp/h` — Claude, Cursor, Copilot, Codex, Gemini
- [x] `hooks/agent_detector.cpp/h` — detect installed agents

**Phase 7 — Distribution**
- [x] `install.sh` — universal install script
- [x] `install.ps1` — PowerShell install script
- [x] `package.json` — npm package `ghost-ai`
- [x] `winget/*.yaml` — winget manifests
- [x] `homebrew/ghost-ai.rb` — homebrew formula
- [x] `scoop/ghost-ai.json` — scoop manifest

**Phase 8 — Testing**
- [x] 38 unit tests (LineRangeSet, NoteWriter, NoteReader, VerifiedWriter, GhostConfig)
- [x] 5 integration tests (temp git repos, checkpoint, post-commit, audit, installer)
- [x] CI workflow: `.github/workflows/ci.yml` — tests on win/linux/macos x86_64+arm64

## What's Next

1. **GitAiReader** — Fallback implementation for reading `refs/notes/ai`
2. **Release** — Tag v0.1.0, trigger CI release workflow, set `NPM_TOKEN` secret

### Completed Polish
- [x] Per-command `--help` with examples and flags
- [x] Arrow-key TUI (raw ANSI, zero dependencies)
- [x] `ghost init` — npx-style repo scaffolding
- [x] `ghost doctor` / `ghost status` — diagnostics and overview
- [x] `ghost check` — predictive staged audit with session detection
- [x] Fuzzy command matching + suggestions
- [x] `--verbose` flag and standardized exit codes
- [x] Performance profiling — `GHOST_BENCHMARK=1` per-phase timing

### Performance Results (ghost repo, 39 commits, ~400 tracked files)

| Audit Type | Before | After | Improvement |
|---|---|---|---|
| Codebase blame (`ghost audit HEAD`) | 14,869ms | 2,750ms | **81%** |
| Per-commit (`ghost audit --all`) | 15,328ms | 5,872ms | **62%** |

**Key optimizations applied:**
- Flat vector blame (replaced `std::map` with `std::vector`)
- Binary search in `LineRangeSet::contains()` (O(R) → O(log R))
- File-indexed note entries (O(1) file lookup in overlay)
- Overlay cache per file (computed once, reused across commits)
- Diff-tree cache (eliminated redundant subprocess calls)
- Parallel `git blame` via thread pool (`std::async` across files)
- Batch author lookups (`git log --no-walk` with 50-SHA chunks)
- Batch notes retrieval (`git cat-file --batch` — 1 subprocess for all notes)
- Skip binary files via `ghost.yml` `ignore:` patterns

## Key Technical Details

### Note writing uses temp file
`Notes::write()` writes content to a temp file then uses `git notes add -f -F <file>` instead of `-m` because `-m` doesn't handle multi-line content well on Windows.

### Session detection handles two cases
1. **Modified tracked files** — captured in snapshot during `pre`, diffed during `post`
2. **New untracked files** — detected during `post` by comparing `git diff --name-only` against snapshot file list; new files get all lines counted as additions

### Manual JSON Handling
The project avoids external dependencies by using manual JSON serialization/deserialization logic in `writer.cpp` and `reader.cpp`.

### Pre-push hook is standalone
The `.git/hooks/pre-push` shell script has **no ghost binary dependency**. It reads `ghost.yml` directly with `grep` and checks for ghost notes with `git notes`. This ensures enforcement works even if ghost is uninstalled.

### Bootstrap step
When `ghost install` runs, it detects unpushed commits without ghost notes and asks for confirmation. A timestamped log is written to `.git/ghost/bootstrap.log`. The pre-push hook reads this log to determine if the user has already confirmed human authorship.

## Known Issues

- Windows CRLF warnings from git (LF in repo, CRLF in working tree)
- `git diff --no-index` paths use forward slashes on Windows — works but may have edge cases
- No JSON library — manual parsing is fragile for complex/edge-case JSON
- TUI cursor state may not restore on SIGINT during interactive menus

---

*Last Updated: May 22, 2026*
