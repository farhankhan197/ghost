# ghost — Git Hook for Origin Source Tracking
> A first-principles, owner-side AI code attribution system for Git repositories.

---

## Philosophy

Most AI attribution tools ask contributors to self-report. `ghost` flips this: **the repo owner mandates provenance**. Every line of code committed to a repository either carries a cryptographically-linked attribution note, or it is assumed to be human-written. There is no guessing, no heuristics, no LLMs analyzing code style. Attribution is ground truth — recorded at the exact moment an agent writes code — or it doesn't exist.

This is a learning project. The goal is to deeply understand:
- How git's plumbing works (notes, blame, refs, diff)
- How coding agents hook into the filesystem
- How CI/CD gates work at the PR level
- How to build a fast, dependency-light C++ CLI tool

---

## Project Name

**`ghost`** — Git Hook for Origin Source Tracking

Two binaries:
- `ghost` — main CLI + auditor
- `ghost-checkpoint` — lightweight binary called by agent hooks

---

## Core Concepts

### The Attribution Contract
Every commit in a ghost-enabled repo produces up to two notes. Together they give a complete, unambiguous picture of who wrote what and whether the tooling was even running.

**`refs/notes/ghost-verified`** — written on every commit by the post-commit hook, regardless of whether any agent was involved. This is the installation witness note. It proves ghost was running when this commit was made.

**`refs/notes/ghost`** — written only when at least one agent session occurred during the commit. Contains the line-by-line attribution data.

The CI auditor uses both together:

| `ghost-verified` | `ghost` | Meaning |
|---|---|---|
| ✅ Present | ✅ Present | Verified — AI lines attributed, human lines implicit |
| ✅ Present | ❌ Absent | Verified — fully human-written, ghost was running |
| ❌ Absent | ✅ Present | Partial — AI noted but install unconfirmed |
| ❌ Absent | ❌ Absent | Unverified — ghost was not running |

- **There is no `mark-human` command.** No escape hatch. No override. If you wrote it yourself, you simply don't have a session note — the verified note is sufficient and that is everything.
- Notes are pushed to remote alongside commits — they travel with the code
- git-ai fallback: `refs/notes/ai` is read when `refs/notes/ghost` is absent, but there is no git-ai equivalent of the verified note

### Trust Model

| Scenario | Trust level | Why |
|---|---|---|
| Voluntary self-install + agent hooks running | Highest | Contributor chose this, notes generated naturally |
| Repo-mandated setup + bootstrap confirmation | High | Bootstrap is explicit, timestamped, and logged |
| Setup done but agent hooks disabled manually | Medium | Unnotated commits assumed human — contributor's responsibility |
| No setup at all | Caught | CI blocks the PR before merge |
| Active lie (AI code with no note) | Active deception | No system prevents this — but it requires deliberate action with their identity attached |

Ghost does not try to catch liars. It makes lying an active, conscious choice rather than a passive omission — and it ensures honest contributors are never penalized.

---

## Installation Paths

There are two ways a contributor ends up with ghost running. Both produce identical notes. The CI auditor cannot and does not distinguish between them.

### Path 1 — Voluntary Self-Install
The contributor wants to track their own AI usage. They install ghost themselves, run `ghost install-hooks`, and their agents start generating notes from that point forward. No repo mandate required. This is the highest-trust path.

```bash
curl -sSL https://ghost-tool.dev/install.sh | bash
ghost install-hooks
```

### Path 2 — Repo-Mandated via `.ghost/setup.sh`
The repo owner ships a setup script inside the repository. Contributors are required to run it before their first push. The script:

1. Checks if `ghost` and `ghost-checkpoint` are installed — if not, downloads and installs them
2. Installs agent hooks for all detected agents on the machine
3. Installs the repo-local `pre-push` hook
4. Configures git to push `refs/notes/ghost` automatically alongside normal pushes
5. **Bootstrap step:** detects any unpushed commits that have no ghost notes, shows the contributor a clear summary of those commits, explains they will be permanently recorded as human-authored with no attribution data, asks for explicit confirmation, and writes a timestamped bootstrap log to `.git/ghost/bootstrap.log`
6. From this point forward, the pre-push hook enforces that every new commit either has a ghost note or is a new commit with no changed files

The bootstrap log is local only — it is not pushed. It exists so the contributor has a record of what they confirmed.

**There is no way to add notes retroactively after the bootstrap.** The window closes at confirmation.

### The Pre-Push Hook

Shipped as `.ghost/hooks/pre-push` inside the repo. Symlinked to `.git/hooks/pre-push` by `setup.sh`.

For every commit being pushed that was created after the bootstrap timestamp:
- If the commit touches any tracked files and has no `refs/notes/ghost` note → **push is rejected**
- The error message tells the contributor exactly which commits are missing notes
- The only resolution is to recommit with ghost running, or to explain to the repo owner why notes are absent

---

### What a Session Is
A session is one agent interaction: the window of time between a pre-hook and a post-hook call. It produces a diff of which lines changed, tagged to which agent and model wrote them. Multiple sessions can exist per commit.

### The Checkpoint Lifecycle
```
human edits code
       ↓
agent pre-tool hook  →  ghost-checkpoint pre  --agent <name>
       │                   snapshots current diff → .git/ghost/working.log
       ↓
agent writes code
       ↓
agent post-tool hook →  ghost-checkpoint post --agent <name> --model <model>
       │                   diffs snapshot vs current state
       │                   assigns changed lines to this session
       │                   stores session → .git/ghost/sessions/<session_id>.json
       ↓
developer commits
       ↓
git post-commit hook →  ghost condenses all sessions
                           builds authorship log (file → session_id → line ranges)
                           writes git note to refs/notes/ghost
                           cleans up .git/ghost/sessions/
```

---

## Note Schema — `ghost/1.0.0`

Stored as a git note on each commit under `refs/notes/ghost`.

```
# TOP SECTION (plain text, fast to parse)
# format: <filepath>\n  <session_id> <line_ranges>\n
src/main.cpp
  sess_a1b2c3 5-12,18,22-30
  sess_d4e5f6 30-45
src/utils.cpp
  sess_a1b2c3 1-80
---
{
  "schema": "ghost/1.0.0",
  "commit": "<sha>",
  "sessions": {
    "sess_a1b2c3": {
      "agent":      "claude-code",
      "model":      "claude-sonnet-4-5",
      "author":     "alice <alice@example.com>",
      "ts_start":   1710000000,
      "ts_end":     1710000033,
      "additions":  85,
      "deletions":  3,
      "session_id": "sess_a1b2c3"
    },
    "sess_d4e5f6": {
      "agent":      "cursor",
      "model":      "gpt-4o",
      "author":     "alice <alice@example.com>",
      "ts_start":   1710000040,
      "ts_end":     1710000055,
      "additions":  16,
      "deletions":  2,
      "session_id": "sess_d4e5f6"
    }
  }
}
```

### Design Decisions
- Top section is plain text so it can be parsed in microseconds without a JSON library
- `---` separator cleanly divides structure from metadata
- Line ranges use compact notation: `5-12,18,22-30` (ranges and individual lines mixed)
- Session IDs are random 6-byte hex strings (not prompt-linked, to keep it simple)
- Timestamps are Unix epoch integers (no timezone ambiguity)
- The schema version is in the JSON, not the ref name — easier to migrate

---

## Verified Note Schema — `ghost-verified/1.0.0`

Stored as a git note on every commit under `refs/notes/ghost-verified`. Written unconditionally by the post-commit hook — even if the commit is 100% human-written. This is the installation witness.

```json
{
  "schema":        "ghost-verified/1.0.0",
  "ghost_version": "1.2.0",
  "commit":        "<sha>",
  "ts":            1710000042,
  "author":        "alice <alice@example.com>",
  "sessions":      2
}
```

**Fields:**
- `schema` — always `ghost-verified/1.0.0`, for forward compatibility
- `ghost_version` — the exact version of ghost that wrote this note, useful for debugging schema migrations
- `commit` — redundant with the note's attachment point but useful for integrity checks
- `ts` — Unix epoch timestamp of when the post-commit hook ran
- `author` — git author identity of the commit (`git config user.name` + `user.email`)
- `sessions` — number of agent sessions recorded for this commit. `0` means fully human. Matches the number of session entries in `refs/notes/ghost` if that note exists.

### Design Decisions
- Intentionally minimal — this note answers only "was ghost running?" not "what did it do?"
- `sessions: 0` with no ghost note = definitive human commit. `sessions: 2` with no ghost note = bug to investigate.
- Kept as pure JSON (no plain-text top section) since there are no line ranges to encode
- Written atomically in the same post-commit hook that writes the session note, so both notes are always consistent

---

## git-ai Fallback Reader

When `ghost` encounters `refs/notes/ai` on a commit that has no `refs/notes/ghost` note, it falls back to reading the git-ai format (`authorship/3.0.0`). The fallback reader:
- Parses the git-ai top section (same line-range format, different session ID style)
- Maps git-ai's `tool` + `model` fields to ghost's `agent` + `model`
- Marks the output as `source: git-ai` in the report so it's distinguishable
- Does NOT write ghost notes — read-only fallback

---

## Project Structure

```
ghost/
├── CMakeLists.txt
├── README.md
├── PLAN.md                          ← this file
├── ghost.yml.example                ← repo owner config
│
├── src/
│   ├── main.cpp                     ← ghost binary entry point
│   │
│   ├── checkpoint/
│   │   ├── main.cpp                 ← ghost-checkpoint binary entry point
│   │   ├── snapshot.cpp/h           ← pre-hook: capture working tree diff
│   │   ├── session.cpp/h            ← post-hook: record agent session
│   │   └── working_log.cpp/h        ← .git/ghost/ state manager
│   │
│   ├── commit/
│   │   └── post_commit.cpp/h        ← condenses sessions → authorship log → writes both ghost and ghost-verified notes
│   │
│   ├── note/
│   │   ├── writer.cpp/h             ← ghost note serializer (ghost/1.0.0)
│   │   ├── reader.cpp/h             ← ghost note parser
│   │   ├── line_range.cpp/h         ← line range parser/encoder ("5-12,18,22-30")
│   │   ├── verified_writer.cpp/h    ← ghost-verified note serializer
│   │   ├── verified_reader.cpp/h    ← ghost-verified note parser
│   │   └── gitai_reader.cpp/h       ← git-ai authorship/3.0.0 fallback parser
│   │
│   ├── git/
│   │   ├── blame.cpp/h              ← wraps git blame --line-porcelain
│   │   ├── diff.cpp/h               ← wraps git diff for PR commit ranges
│   │   ├── notes.cpp/h              ← git notes fetch/show/append wrappers
│   │   └── repo.cpp/h               ← repo detection, HEAD, commit range helpers
│   │
│   ├── audit/
│   │   ├── auditor.cpp/h            ← main audit orchestrator
│   │   ├── blame_overlay.cpp/h      ← overlays note data onto git blame output
│   │   ├── aggregator.cpp/h         ← per-file and per-PR AI% stats
│   │   └── policy.cpp/h             ← threshold config + enforcement (exit code)
│   │
│   ├── output/
│   │   ├── report.cpp/h             ← JSON + CLI report formatter
│   │   ├── pr_comment.cpp/h         ← GitHub PR comment via REST API (libcurl)
│   │   └── color.cpp/h              ← terminal color helpers
│   │
│   ├── hooks/
│   │   ├── installer.cpp/h          ← ghost install-hooks orchestrator
│   │   ├── claude_code.cpp/h        ← writes ~/.claude/settings.json hooks
│   │   ├── cursor.cpp/h             ← writes cursor settings hooks
│   │   ├── copilot.cpp/h            ← writes VSCode copilot hooks
│   │   ├── codex.cpp/h              ← writes ~/.codex/config.json hooks
│   │   ├── opencode.cpp/h           ← writes ~/.opencode/config.json hooks
│   │   ├── junie.cpp/h              ← writes JetBrains plugin hooks
│   │   └── generic.cpp/h            ← reads ~/.ghost/agents.yml, installs any agent
│   │
│   └── config/
│       ├── ghost_config.cpp/h       ← reads ghost.yml (threshold, token, etc.)
│       └── agent_config.cpp/h       ← reads ~/.ghost/agents.yml
│
├── .github/
│   └── workflows/
│       └── ghost-audit.yml          ← ready-to-use GitHub Actions workflow
│
└── tests/
    ├── note/                        ← unit tests for parser/serializer round-trips
    ├── audit/                       ← unit tests for blame overlay + aggregation
    ├── fixtures/                    ← sample git notes, blame outputs, diffs
    └── integration/                 ← end-to-end test with a mock git repo
```

---

## CLI Reference

### `ghost`
```
ghost install-hooks                  install agent hooks for all detected agents
ghost install-hooks --agent <name>   install hooks for a specific agent only
ghost uninstall-hooks                remove all installed hooks

ghost audit                          run full audit on current PR / HEAD
ghost audit --range <sha1>..<sha2>   audit a specific commit range
ghost audit --threshold 80           override config threshold for this run

ghost blame <file>                   line-by-line attribution for a file
ghost blame <file> --json            machine-readable output

ghost stats                          AI% stats for HEAD commit
ghost stats <sha1>..<sha2>           stats for a commit range
ghost stats --json                   JSON output

ghost show <commit>                  print raw ghost note for a commit
ghost show <commit> --fallback       also check refs/notes/ai if no ghost note

ghost config                         show current config
ghost config set threshold 70        set AI% threshold

ghost version                        print version
```

### `ghost-checkpoint`
```
ghost-checkpoint pre  --agent <name>
ghost-checkpoint post --agent <name> --model <model>
ghost-checkpoint show                print current working log
ghost-checkpoint reset               clear working log and sessions (use after aborted agent run)
```

> **There is no `ghost mark-human` or any equivalent command.** The absence of a note is the complete and sufficient declaration of human authorship. No command will ever be added to assert this — such a command would become an abuse vector immediately.

---

## Configuration — `ghost.yml`

Placed in the root of the repo. Read by the GitHub Actions workflow and by `ghost audit`.

```yaml
# ghost.yml
version: 1

# Reject PRs where AI-authored lines exceed this percentage
threshold: 80

# What to do when threshold is exceeded: "block" or "warn"
on_exceed: block

# Post a comment on the PR with the attribution report
pr_comment: true

# Files/patterns to exclude from attribution counting
# (same semantics as .gitignore)
ignore:
  - "*.lock"
  - "vendor/**"
  - "dist/**"
  - "**/__snapshots__/**"

# Commits with no ghost or git-ai notes are always treated as fully human-authored.
# There is no override for this — absence of a note is the declaration.
untagged_policy: human

# How to handle commits missing a ghost-verified note (ghost was not running):
# "block"  = reject the PR entirely
# "warn"   = post a warning comment but allow merge
# "ignore" = do not check for verified notes at all
unverified_policy: warn

# Fallback to git-ai notes if ghost notes are absent
gitai_fallback: true
```

---

## Agent Hook Configs

How `ghost install-hooks` configures each agent:

### Claude Code (`~/.claude/settings.json`)
```json
{
  "hooks": {
    "PreToolUse": [{
      "matcher": "Write|Edit|MultiEdit",
      "hooks": [{ "type": "command", "command": "ghost-checkpoint pre --agent claude-code 2>/dev/null || true" }]
    }],
    "PostToolUse": [{
      "matcher": "Write|Edit|MultiEdit",
      "hooks": [{ "type": "command", "command": "ghost-checkpoint post --agent claude-code --model \"$(echo $CLAUDE_MODEL)\" --hook-input \"$(cat)\" 2>/dev/null || true" }]
    }]
  }
}
```

### Cursor
Cursor exposes pre/post tool hooks via `~/.cursor/mcp.json` and workspace settings. Same pattern as Claude Code — two hooks, pre captures snapshot, post records session.

### GitHub Copilot (VSCode)
Copilot exposes hooks via the VSCode extension API. `ghost install-hooks` writes a `.vscode/tasks.json` trigger for Copilot edit events.

### Codex (`~/.codex/config.json`)
```json
{
  "hooks": {
    "before_apply": "ghost-checkpoint pre --agent codex",
    "after_apply":  "ghost-checkpoint post --agent codex --model \"$CODEX_MODEL\""
  }
}
```

### OpenCode, Junie, Generic
Same two-hook pattern. Generic agents are configured via `~/.ghost/agents.yml`:
```yaml
agents:
  - name: my-custom-agent
    pre_hook:  "ghost-checkpoint pre --agent my-custom-agent"
    post_hook: "ghost-checkpoint post --agent my-custom-agent --model unknown"
    config_path: ~/.my-agent/config.json
    hook_key: hooks
```

---

## GitHub Actions Workflow

```yaml
# .github/workflows/ghost-audit.yml
name: Ghost AI Audit

on:
  pull_request:
    types: [opened, synchronize, reopened]

jobs:
  audit:
    runs-on: ubuntu-latest
    permissions:
      contents: read
      pull-requests: write   # needed to post PR comment

    steps:
      - uses: actions/checkout@v4
        with:
          fetch-depth: 0     # full history needed for blame

      - name: Fetch ghost notes
        run: |
          git fetch origin refs/notes/ghost:refs/notes/ghost 2>/dev/null || true
          git fetch origin refs/notes/ghost-verified:refs/notes/ghost-verified 2>/dev/null || true
          git fetch origin refs/notes/ai:refs/notes/ai 2>/dev/null || true

      - name: Install ghost
        run: curl -sSL https://ghost-tool.dev/install.sh | bash

      - name: Run audit
        env:
          GITHUB_TOKEN: ${{ secrets.GITHUB_TOKEN }}
        run: |
          ghost audit \
            --range ${{ github.event.pull_request.base.sha }}..${{ github.event.pull_request.head.sha }} \
            --pr-number ${{ github.event.pull_request.number }} \
            --repo ${{ github.repository }}
```

Set this check as **required** in branch protection rules → PRs cannot merge if `ghost audit` exits with code 1.

---

## Build System

**CMake** with two targets:

```cmake
cmake_minimum_required(VERSION 3.20)
project(ghost VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Dependencies (header-only or vendored — no package manager required)
# - nlohmann/json  (vendored, single header) — JSON parsing
# - libcurl        (system)                  — GitHub API POST

find_package(CURL REQUIRED)

# ghost-checkpoint binary (must stay lightweight, fast startup)
add_executable(ghost-checkpoint
  src/checkpoint/main.cpp
  src/checkpoint/snapshot.cpp
  src/checkpoint/session.cpp
  src/checkpoint/working_log.cpp
  src/note/line_range.cpp
  src/git/repo.cpp
  src/config/agent_config.cpp
)

# ghost main binary
add_executable(ghost
  src/main.cpp
  src/note/writer.cpp
  src/note/reader.cpp
  src/note/line_range.cpp
  src/note/gitai_reader.cpp
  src/git/blame.cpp
  src/git/diff.cpp
  src/git/notes.cpp
  src/git/repo.cpp
  src/audit/auditor.cpp
  src/audit/blame_overlay.cpp
  src/audit/aggregator.cpp
  src/audit/policy.cpp
  src/output/report.cpp
  src/output/pr_comment.cpp
  src/output/color.cpp
  src/hooks/installer.cpp
  src/hooks/claude_code.cpp
  src/hooks/cursor.cpp
  src/hooks/copilot.cpp
  src/hooks/codex.cpp
  src/hooks/opencode.cpp
  src/hooks/junie.cpp
  src/hooks/generic.cpp
  src/commit/post_commit.cpp
  src/config/ghost_config.cpp
  src/config/agent_config.cpp
)
target_link_libraries(ghost CURL::libcurl)
```

**No external package managers.** `nlohmann/json` is vendored as a single header (`include/json.hpp`). Only system dependency is `libcurl` (available on all major Linux distros and macOS).

---

## Implementation Phases

### Phase 1 — Core Note System
The heart of everything. Nothing else works without this.

- [ ] `line_range.cpp` — encode/decode `5-12,18,22-30` format
- [ ] `note/writer.cpp` — serialize authorship log to ghost/1.0.0 format
- [ ] `note/reader.cpp` — parse ghost/1.0.0 notes from git notes show output
- [ ] `note/gitai_reader.cpp` — parse git-ai authorship/3.0.0 as fallback
- [ ] `note/verified_writer.cpp` — serialize ghost-verified note to JSON
- [ ] `note/verified_reader.cpp` — parse ghost-verified note
- [ ] `git/notes.cpp` — wrappers: fetch refs/notes/ghost + refs/notes/ghost-verified, show note for commit, append note
- [ ] Unit tests: round-trip serialize → parse for both note types, edge cases (empty file, single line, overlapping ranges)

### Phase 2 — Checkpoint Binary
The producer. Runs on the contributor's machine inside agent hooks.

- [ ] `git/repo.cpp` — detect repo root, get HEAD sha, run git commands via popen
- [ ] `checkpoint/working_log.cpp` — read/write `.git/ghost/` state
- [ ] `checkpoint/snapshot.cpp` — pre-hook: run `git diff` and store snapshot
- [ ] `checkpoint/session.cpp` — post-hook: diff snapshot vs current, extract changed line ranges, write session JSON
- [ ] `checkpoint/main.cpp` — CLI entry: `pre` and `post` subcommands
- [ ] Manual test: run pre/post hooks by hand, inspect `.git/ghost/sessions/`

### Phase 3 — Post-Commit Note Writer
Bridges the checkpoint data and the git note.

- [ ] `commit/post_commit.cpp` — read all sessions from `.git/ghost/sessions/`, merge into single authorship log per file, write `refs/notes/ghost` (if sessions > 0), write `refs/notes/ghost-verified` unconditionally, clean up sessions
- [ ] `git/repo.cpp` — add `post-commit` hook installation to repo
- [ ] End-to-end test: install hooks → trigger agent → commit → `git log --show-notes=ghost`

### Phase 4 — Audit Engine
The consumer. Runs in CI.

- [ ] `git/blame.cpp` — run `git blame --line-porcelain`, parse output into `{line_num → commit_sha}` map
- [ ] `git/diff.cpp` — get changed files + line ranges for a commit range
- [ ] `audit/blame_overlay.cpp` — for each changed line: look up commit sha in blame, look up sha in ghost notes, assign attribution
- [ ] `audit/aggregator.cpp` — count AI lines / total lines per file, per commit, per PR
- [ ] `audit/policy.cpp` — read threshold from ghost.yml, enforce AI% threshold, enforce unverified_policy (block/warn/ignore) based on ghost-verified note presence
- [ ] `audit/auditor.cpp` — orchestrate: fetch notes → build blame map → overlay → aggregate → enforce policy

### Phase 5 — Output + CI Integration
Makes it useful.

- [ ] `output/report.cpp` — CLI table output (colored) + `--json` machine-readable output
- [ ] `output/pr_comment.cpp` — POST to GitHub API: format markdown report, attach to PR
- [ ] `output/color.cpp` — ANSI color helpers (detect TTY, disable in CI)
- [ ] `ghost-audit.yml` — GitHub Actions workflow
- [ ] Test: run `ghost audit` on a test repo, verify PR comment posts

### Phase 6 — Hook Installer
Makes it easy to adopt.

- [ ] `hooks/installer.cpp` — detect which agents are installed, install appropriate hooks
- [ ] `hooks/claude_code.cpp` — read/write `~/.claude/settings.json` safely (parse existing JSON, merge)
- [ ] `hooks/cursor.cpp` — cursor hook config
- [ ] `hooks/copilot.cpp` — copilot hook config
- [ ] `hooks/codex.cpp` — codex hook config
- [ ] `hooks/opencode.cpp` — opencode hook config
- [ ] `hooks/junie.cpp` — junie hook config
- [ ] `hooks/generic.cpp` — generic agent YAML reader
- [ ] `ghost install-hooks` and `ghost uninstall-hooks` commands

### Phase 7 — Polish
Makes it production-quality.

- [ ] Install script (`install.sh` + `install.ps1`)
- [ ] Man page / `--help` for all commands
- [ ] Performance profiling (target: <50ms for `ghost-checkpoint`, <500ms for `ghost audit` on typical PR)
- [ ] Cross-platform testing: Linux, macOS, Windows (WSL)
- [ ] Integration test suite with a fully scripted mock git repo

---

## Future Ideas

These are not planned for v1 but are worth designing toward.

### Near-term
- **`ghost blame <file>`** — interactive terminal UI showing human/AI attribution side by side with syntax highlighting, similar to `tig blame`
- **`ghost diff`** — like `git diff` but every added line shows who wrote it (human or agent + model)
- **Squash/rebase preservation** — when a PR is squash-merged via GitHub UI, reconstruct authorship by listening to the `pull_request` merged webhook and running `ghost squash-authorship`
- **`.ghost-ignore`** — per-repo ignore file for excluding generated files from stats, same semantics as `.gitignore`

### Medium-term
- **Web dashboard** — a self-hostable server that aggregates ghost data across repos: AI% per developer, per team, per repo, over time. Built on the JSON output of `ghost audit`
- **VS Code extension** — show inline ghost attribution in the editor gutter (which agent wrote this line) using git notes data
- **`ghost verify`** — a separate command that checks note integrity: are the line ranges still valid given the current file state? Useful for repos where notes have drifted
- **Signed notes** — attach a GPG/SSH signature to each ghost note so notes cannot be forged after the fact. Critical for high-trust environments

### Long-term
- **Protocol spec** — publish `ghost/1.0.0` as an open standard so other tools can produce compatible notes. The goal is for ghost to become the reader, not the only writer
- **IDE plugins** — JetBrains, Neovim plugins that show attribution live as you code
- **Multi-remote sync** — sync ghost notes across forks automatically, not just origin
- **Stats API** — expose a simple HTTP API so CI dashboards, Slack bots, and other tooling can query AI attribution data without parsing JSON
- **Agent model registry** — a maintained mapping of `agent:model` strings to capabilities/pricing so the report can show cost estimates for AI-written code

---

## Key Technical Challenges

**Line range drift** — git blame tracks which commit last touched a line, but line numbers shift as code evolves. The overlay engine must use `git blame --line-porcelain` which gives the original commit and original line number, then map back through the note. This is the hardest algorithmic part of the project.

**Concurrent sessions** — if two agents edit the same file in overlapping sessions (unusual but possible), line ranges can conflict. The post-commit merger needs a clear precedence rule: last-write-wins by timestamp.

**Hook config file safety** — agent config files (e.g. `~/.claude/settings.json`) may be edited by other tools concurrently. The hook installer must parse existing JSON, merge cleanly, and write atomically (write to temp file, rename).

**Notes ref distribution** — neither `refs/notes/ghost` nor `refs/notes/ghost-verified` are pushed by default. The install script must configure both via:
```
git config --add remote.origin.push refs/notes/ghost
git config --add remote.origin.push refs/notes/ghost-verified
```
If either ref fails to push, the CI auditor will correctly flag commits as unverified rather than silently passing them.

**Windows compatibility** — `ghost-checkpoint` runs in the hot path of every agent edit. On Windows (non-WSL), `popen()` behavior differs. Paths use backslashes. The checkpoint binary needs careful platform abstraction.

---

## Non-Goals

- **Detecting AI code without notes** — ghost does not and will never use heuristics to guess whether code is AI-generated. If there is no note, the code is human. Period.
- **Blocking all AI code** — the threshold is configurable down to 0% but the default is 80%. The tool is for visibility and policy enforcement, not for banning AI use.
- **Replacing git-ai** — ghost is a compatible alternative and learning project, not a competitor. The fallback reader exists to interoperate, not to absorb.
- **Cloud dependency** — ghost is fully local and offline-capable. The only network calls are the GitHub API comment (optional) and the notes push (which is just git).
