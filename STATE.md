# Ghost — Project State Summary

> Git Hook for Origin Source Tracking — AI code attribution for git repos using cryptographically-linked git notes.

## Architecture

Two binaries:
- **`ghost`** — main CLI (install, show, post-commit, version)
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
│
├── .ghost/
│   └── hooks/
│       └── post-commit          ← shell wrapper: "ghost post-commit 2>/dev/null || true"
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
│   │   ├── verified_reader.cpp/h← stub (returns success, no parsing)
│   │   └── gitai_reader.cpp/h   ← stub (returns "not implemented")
│   │
│   ├── git/
│   │   ├── notes.cpp/h          ← git notes show/write/exists via popen ✅
│   │   ├── repo.cpp/h           ← getRoot/getHead/isRepo via popen ✅
│   │   ├── blame.cpp/h          ← stub (returns empty map)
│   │   └── diff.cpp/h           ← stub (returns empty vector)
│   │
│   └── hooks/
│       ├── installer.cpp/h      ← ghost install/uninstall logic
│       └── (claude_code, cursor, copilot, codex, junie, generic — not created)
│
├── build/
│   ├── ghost.exe
│   └── ghost-checkpoint.exe
│
└── tests/
    └── (empty)
```

## Build System

```cmake
cmake_minimum_required(VERSION 3.20)
project(ghost VERSION 1.0.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
```

- **ghost-checkpoint**: main.cpp + working_log.cpp + snapshot.cpp + session.cpp + note + git libs
- **ghost**: main.cpp + post_commit.cpp + working_log.cpp + installer.cpp + note + git libs
- No external dependencies — manual JSON serialization (no nlohmann-json)
- Only system dependency would be libcurl (not yet used)

Build:
```bash
cd build && cmake .. -G "MinGW Makefiles" && cmake --build .
```

## CLI Reference

### `ghost`
```
ghost install              Install in current repo (copies binaries + creates plugin + post-commit hook)
ghost install --global     Install plugin for ALL repos (~/.config/opencode/plugins/ghost.ts)
ghost install-bin          Just copy binaries to ~/.ghost/bin/
ghost uninstall            Remove from current repo
ghost uninstall --global   Remove global plugin
ghost show <commit>        Show formatted AI attribution for a commit
ghost post-commit          Run post-commit hook (reads sessions → writes notes)
ghost version              Print version
```

### `ghost-checkpoint`
```
ghost-checkpoint pre  --agent <name>
ghost-checkpoint post --agent <name> --model <model>
ghost-checkpoint show
ghost-checkpoint reset
```

## Implementation Status

| Component | Status | Notes |
|-----------|--------|-------|
| LineRangeSet | ✅ Done | Parse/serialize with range merging |
| NoteWriter | ✅ Done | Manual JSON serialization |
| NoteReader | ✅ Done | Parses top section + JSON sessions map |
| VerifiedWriter | ✅ Done | Pure JSON output |
| VerifiedReader | ✅ Done | Full JSON parsing implemented |
| GitAiReader | ⚠️ Stub | Returns "not implemented" |
| Repo | ✅ Done | getRoot, getHead, isRepo |
| Notes | ✅ Done | show, write (via temp file), exists |
| Blame | ✅ Done | Full porcelain blame parsing |
| Diff | ✅ Done | Numstat diff parsing |
| Checkpoint pre | ✅ Done | Snapshots modified files |
| Checkpoint post | ✅ Done | Diffs, extracts ranges, writes session JSON |
| Post-commit | ✅ Done | Reads sessions, writes both notes, cleanup |
| Installer | ✅ Done | install/uninstall repo + global + bin |
| OpenCode plugin | ✅ Done | Hooks edit/write/apply_patch |
| Agent Hooks | ✅ Done | Claude, Cursor, Copilot, Codex, Gemini |
| Audit engine | ✅ Done | blame overlay, aggregation, policy |
| Output | ✅ Done | CLI reports, JSON output |
| CI Integration | ❌ Not started | GitHub Actions workflow |
| Tests | ❌ Not started | No test files yet |

### Phase 4 — Audit Engine
The consumer. Runs in CI.

- [x] `git/blame.cpp` — run `git blame --line-porcelain`, parse output into `{line_num → commit_sha}` map
- [x] `git/diff.cpp` — get changed files + line ranges for a commit range
- [x] `audit/blame_overlay.cpp` — for each changed line: look up commit sha in blame, look up sha in ghost notes, assign attribution
- [x] `audit/aggregator.cpp` — count AI lines / total lines per file, per commit, per PR
- [x] `audit/policy.cpp` — read threshold from ghost.yml, enforce AI% threshold, enforce unverified_policy (block/warn/ignore) based on ghost-verified note presence
- [x] `audit/auditor.cpp` — orchestrate: fetch notes → build blame map → overlay → aggregate → enforce policy

### Phase 5 — Output + CI Integration
Makes it useful.

- [x] `output/report.cpp` — CLI table output (colored) + `--json` machine-readable output
- [ ] `output/pr_comment.cpp` — POST to GitHub API: format markdown report, attach to PR
- [x] `output/color.cpp` — ANSI color helpers (detect TTY, disable in CI)
- [ ] `ghost-audit.yml` — GitHub Actions workflow

### Phase 6 — Hook Installer
Makes it easy to adopt.

- [x] `hooks/installer.cpp` — detect which agents are installed, install appropriate hooks
- [x] `hooks/agent_hooks.cpp` — integrated hook writer for Claude, Cursor, Copilot, Codex, Gemini
- [x] `hooks/agent_detector.cpp` — detect installed agents and their config paths
- [x] `ghost install-hooks` and `ghost uninstall-hooks` commands

### Phase 7 — Polish
Makes it production-quality.

- [ ] Install script (`install.sh` + `install.ps1`)
- [ ] Man page / `--help` for all commands
- [ ] Performance profiling (target: <50ms for `ghost-checkpoint`, <500ms for `ghost audit` on typical PR)
- [ ] Cross-platform testing: Linux, macOS, Windows (WSL)
- [ ] Integration test suite with a fully scripted mock git repo

### Phase 8 — Testing (NEW)
- [ ] Unit tests for `LineRangeSet`
- [ ] Unit tests for `NoteWriter`/`NoteReader`
- [ ] Integration tests for `ghost-checkpoint`
- [ ] Integration tests for `ghost audit`

## Key Technical Details

### Note writing uses temp file
`Notes::write()` writes content to a temp file then uses `git notes add -f -F <file>` instead of `-m` because `-m` doesn't handle multi-line content well on Windows.

### Session detection handles two cases
1. **Modified tracked files** — captured in snapshot during `pre`, diffed during `post`
2. **New untracked files** — detected during `post` by comparing `git diff --name-only` against snapshot file list; new files get all lines counted as additions

### Manual JSON Handling
The project avoids external dependencies by using manual JSON serialization/deserialization logic in `writer.cpp` and `reader.cpp`.

## What's Next (Priority Order)

1. **Phase 8: Tests** — unit tests for note parser/serializer, integration tests (Immediate Priority)
2. **Phase 5 (Partial): CI Integration** — GitHub Actions workflow creation
3. **Phase 7: Polish** — install scripts, man pages, performance, cross-platform testing
4. **Stub completion** — GitAiReader fallback implementation


## Known Issues

- Windows CRLF warnings from git (LF in repo, CRLF in working tree)
- Session ID is deterministic (`sess_93e41c6e2091`) — `std::rand()` not seeded
- `git diff --no-index` paths use forward slashes on Windows — works but may have edge cases
- No JSON library — manual parsing is fragile for complex/edge-case JSON
