# Ghost – AI Code Attribution for Git

## Overview
Ghost is a tool that automatically records which AI agent wrote each line of code in a Git repository. By storing cryptographically‑linked attribution notes directly in Git, Ghost provides transparent provenance without relying on heuristics or guesswork.

## Features
- **Per‑line AI attribution** recorded in `refs/notes/ghost`
- **Installation verification** via `refs/notes/ghost-verified`
- Works with major AI agents (Claude, Cursor, Copilot, Gemini, etc.)
- Cross‑platform support (Linux, macOS, Windows)
- Seamless CI integration – GitHub Action `ghost-audit.yml`
- Multiple distribution channels: script, npm, Homebrew, Winget, Scoop

## Installation
### macOS / Linux
```bash
curl -sSL https://raw.githubusercontent.com/farhankhan197/ghost/main/install.sh | bash
```
### Windows PowerShell
```powershell
irm https://raw.githubusercontent.com/farhankhan197/ghost/main/install.ps1 | iex
```
### npm
```bash
npm install -g ghost-ai
```
### Homebrew (macOS)
```bash
brew install farhankhan197/tap/ghost-ai
```
### Winget (Windows)
```bash
winget install farhankhan197.ghost-ai
```
### Scoop (Windows)
```bash
scoop install ghost-ai
```

## Quick Start
```bash
# Inside any Git repository
ghost init --yes   # installs hooks and creates ghost.yml
# Make a change with an AI‑enabled editor, then commit as usual
git push           # pre‑push hook enforces attribution policy
```

## Configuration (`ghost.yml`)
```yaml
version: 1
required: false   # set to true to mandate attribution for this repo
threshold: 80     # block PRs when AI‑generated lines exceed 80%
on_exceed: block  # options: block, warn, allow
```
Edit `ghost.yml` with `ghost config set <key> <value>`.

When `owner` is set, protected policy keys can only be changed by that Git user. Use `ghost policy` to see who controls the repo policy and which enforcement stage applies.

Maintainers can start with a complete restrictive setup:

```bash
ghost init --owner --mode restrictive --github-owner @your-github-user
```

Contributors should preserve the checked-in policy and install only local compliance hooks:

```bash
ghost init --contributor
```

## Usage
- `ghost status` – overview of setup, staged/unstaged work, uncommitted agent sessions, and the HEAD note
- `ghost policy` – show repo owner controls, protected rules, and enforcement stages
- `ghost audit [range]` – enforce policy against committed history using git notes
- `ghost blame <file>` – line‑by‑line attribution for a file
- `ghost check` – preview attribution for staged changes before commit
- `ghost verify-pr [range]` – simulate the PR audit locally with base-branch policy
- `ghost explain <command>` – explain what each command reads and whether it enforces policy
- `ghost policy sign` / `ghost policy verify` – attest and verify `ghost.yml`
- `ghost notes sign` / `ghost notes verify` – attest and verify Ghost note integrity

`ghost audit` is commit-based: it reads committed Git history plus `refs/notes/ghost`, `refs/notes/ghost-verified`, and optional `refs/notes/ai` fallback notes. Live agent edits are captured first as uncommitted checkpoint sessions; use `ghost status` to inspect those sessions and `ghost check` to evaluate staged changes before committing.

## Contributing
Contributions are welcome! Please read `CONTRIBUTING.md` for guidelines, run the test suite with `ctest`, and open pull requests against the `main` branch.

## License
MIT © 2026 Farhan Khan. See `LICENSE` for details.


<!-- model-test: deepseek-v4-flash-free -->

> Owner-side AI code attribution for Git repositories. Know exactly which agent wrote every line.

---

## Philosophy

Most AI attribution tools ask contributors to self-report. `ghost` flips this: **the repo owner mandates provenance**. Every line of code committed to a repository either carries a cryptographically-linked attribution note, or it is assumed to be human-written. No guessing, no heuristics, no LLMs analyzing code style — attribution is ground truth, recorded at the exact moment an agent writes code.

Ghost is a learning project that explores how git's plumbing works (notes, blame, refs, diff), how coding agents hook into the filesystem, and how CI/CD gates work at the PR level.

---

## Quick Start

```bash
# Install ghost
curl -sSL https://raw.githubusercontent.com/farhankhan197/ghost/main/install.sh | bash

# Initialize in any repo
cd your-repo
ghost init --interactive   # guided TUI wizard, or
ghost init --yes           # one-shot with defaults
ghost init --owner         # maintainer setup: policy + hooks + CI workflow + GHOST.md
ghost init --contributor   # contributor setup: local hooks only, preserves ghost.yml
```

That's it. Ghost creates:
- `ghost.yml` — your repo's attribution policy
- `.git/hooks/` — post-commit, pre-push, post-rewrite, post-merge, post-checkout, pre-merge-commit hooks
- Git config to push `refs/notes/ghost` and `refs/notes/ghost-verified`

For global tracking across all repos:
```bash
ghost init --global
```

---

## Core Concepts

### The Attribution Contract

Every commit in a ghost-enabled repo produces up to two git notes:

| `refs/notes/ghost-verified` | `refs/notes/ghost` | Meaning |
|---|---|---|
| Present | Present | Verified — AI lines attributed, human lines implicit |
| Present | Absent | Verified — fully human-written, ghost was running |
| Absent | Present | Partial — AI noted but install unconfirmed |
| Absent | Absent | Unverified — ghost was not running |

- **`refs/notes/ghost-verified`** — written on **every** commit by the post-commit hook. The installation witness. Proves ghost was running.
- **`refs/notes/ghost`** — written **only** when at least one agent session occurred. Contains line-by-line attribution data (which agent, model, and which lines changed).
- **There is no `mark-human` command.** No escape hatch. The absence of a session note is the complete declaration of human authorship.
- Notes are pushed to remote alongside commits — they travel with the code.
- git-ai fallback: `refs/notes/ai` is read when `refs/notes/ghost` is absent.

### Trust Model

| Scenario | Trust level |
|---|---|
| Voluntary self-install + agent hooks running | Highest |
| Repo-mandated setup + bootstrap confirmation | High |
| Setup done but agent hooks disabled manually | Medium |
| No setup at all | Caught by CI |
| Active lie (AI code with no note) | Active deception — no system prevents this, but it requires deliberate action |

Ghost does not try to catch liars. It makes lying an active, conscious choice rather than a passive omission — and it ensures honest contributors are never penalized.

### Repo Requirements

A repo can declare ghost required by adding to `ghost.yml`:
```yaml
required: true
```

When `required: true`:
- Pre-push hook checks for ghost notes on commits being pushed
- First push without notes → user can confirm human-written or install ghost
- Subsequent pushes without notes → blocked until ghost is installed

This gives new contributors a one-time grace period to set up.

### Owner-Controlled Policy

Ghost is designed for open-source maintainers who need enforceable AI provenance rules, not just voluntary badges. The repo owner declares the policy in `ghost.yml`; contributors can inspect it, but owner-protected keys cannot be changed locally by a different Git identity once `owner` is configured.

Protected policy keys include:
- `owner`
- `owners`
- `locked`
- `policy_locked`
- `required`
- `threshold`
- `on_exceed`
- `pr_comment`
- `untagged` / `untagged_policy`
- `unverified` / `unverified_policy`
- `gitai_fb` / `gitai_fallback`
- `ignore`

Use:
```bash
ghost policy
```

`ghost policy` answers four maintainer questions:
- Who owns the repo policy?
- Can the current Git user change protected rules?
- What will be blocked, warned, or allowed?
- Which command is showing setup state, staged previews, or committed enforcement?

In CI, use `ghost audit --config-ref origin/main` so the audit reads `ghost.yml` from the protected base branch. That prevents a PR from weakening `threshold`, `unverified`, or `required` in the same branch it is trying to merge.

`ghost init --owner` also creates `.github/CODEOWNERS` for `ghost.yml`, `GHOST.md`, and the Ghost audit workflow. Enable "Require review from Code Owners" in branch protection so policy and enforcement changes need maintainer approval.

### Enforcement Stages

| Command / Stage | What it reads | What it means |
|---|---|---|
| `ghost status` | Current repo setup, working tree, uncommitted sessions, HEAD notes | Operational state. It does not enforce committed history. |
| `ghost check` | Staged diff plus live session data | Pre-commit prediction for files already added with `git add`. |
| `post-commit hook` | Completed session data under `.git/ghost` | Writes durable `refs/notes/ghost` and `refs/notes/ghost-verified`. |
| `pre-push hook` | Outgoing commits and notes | Blocks required repos when attribution setup is missing. |
| `ghost audit` / CI | Committed Git history and git notes | Final policy gate for PRs and releases. |

### How It Works

```
SCENARIO 1: Ghost installed, commits normally
  ghost init → hooks auto-configured → AI edits → commit
  → ghost notes written → push allowed

SCENARIO 2: First push to required repo, no ghost
  push → prompt appears →
    [1] Install ghost now (recommended)
    [2] I confirm this is human-written (one-time only)
    [3] Cancel push

SCENARIO 3: Subsequent push, no ghost
  push → blocked → must install ghost

SCENARIO 4: Push with ghost notes
  push → notes exist → allowed
```

### Session Lifecycle

A session is one agent interaction: the window between a pre-hook and a post-hook call. Multiple sessions can exist per commit.

```
agent pre-tool hook  →  ghost-checkpoint pre --agent <name>
                         snapshots current state
agent writes code
agent post-tool hook →  ghost-checkpoint post --agent <name> --model <model>
                         diffs snapshot vs current
                         assigns changed lines to this session
git commit
post-commit hook     →  ghost condenses all sessions
                         writes git note to refs/notes/ghost
                         writes verified note to refs/notes/ghost-verified
```

Before `git commit`, Ghost stores agent activity as uncommitted checkpoint/session state under `.git/ghost`. After `git commit`, the post-commit hook turns that state into durable git notes. `ghost audit` reads the committed notes; `ghost status` shows the current repo state across setup, working tree, sessions, and HEAD notes; `ghost check` previews the staged diff only.

Per-edit checkpointing is supported via `--file <path>` for sub-file granularity.

---

## Installation

**macOS / Linux:**
```bash
curl -sSL https://raw.githubusercontent.com/farhankhan197/ghost/main/install.sh | bash
```

**Windows (PowerShell):**
```powershell
irm https://raw.githubusercontent.com/farhankhan197/ghost/main/install.ps1 | iex
```

**npm:**
```bash
npm install -g ghost-ai
```

**winget (Windows):**
```bash
winget install farhankhan197.ghost-ai
```

**Homebrew (macOS):**
```bash
brew install farhankhan197/tap/ghost-ai
```

**Scoop (Windows):**
```bash
scoop install ghost-ai
```

**Build from source:**
```bash
git clone https://github.com/farhankhan197/ghost.git
cd ghost
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/ghost init --yes
```

Requires: C++20 compiler, CMake 3.20+, Ninja, Git, vcpkg (sqlite3 + nlohmann-json + gtest).

---

## Installation Paths

There are two ways a contributor ends up with ghost running. Both produce identical notes — the CI auditor cannot distinguish between them.

### Path 1 — Voluntary Self-Install
The contributor wants to track their own AI usage. Install ghost, run `ghost init`. No repo mandate required.

### Path 2 — Repo-Mandated
The repo owner sets `required: true` in `ghost.yml`. Contributors run `ghost init` before their first push. The command:
1. Creates `ghost.yml` with attribution policy
2. Writes `.git/hooks/` — post-commit, pre-push, post-rewrite, post-merge, post-checkout, pre-merge-commit
3. Configures git to push `refs/notes/ghost` and `refs/notes/ghost-verified`
4. **Bootstrap step:** detects unpushed commits without ghost notes, asks for confirmation of human authorship, logs to `.git/ghost/bootstrap.log`

**There is no way to add notes retroactively after the bootstrap.** The window closes at confirmation.

### The Pre-Push Hook
The pre-push hook is a standalone shell script with **no ghost binary dependency**. It reads `ghost.yml` directly with `grep` and checks ghost notes with `git notes`. Enforcement works even if ghost is uninstalled.

**Behavior:**
1. Check if `ghost.yml` has `required: true`
2. If false → allow push
3. If true → check commits for ghost notes
4. All commits have notes → allow push
5. Some commits missing notes:
   - First time → show prompt to install or confirm human-written
   - Subsequent → block push

---

## Note Schemas

### `refs/notes/ghost` — Attribution note (`ghost/1.0.0`)
```
src/main.cpp
  sess_a1b2c3 5-12,18,22-30
  sess_d4e5f6 30-45
---
{
  "schema": "ghost/1.0.0",
  "commit": "<sha>",
  "sessions": {
    "sess_a1b2c3": {
      "agent": "claude-code",
      "model": "claude-sonnet-4-5",
      "author": "alice <alice@example.com>",
      "ts_start": 1710000000,
      "ts_end": 1710000033,
      "additions": 85,
      "deletions": 3
    }
  }
}
```

- Top section: plain text, fast to parse — `<filepath>\n  <session_id> <line_ranges>`
- Bottom section: JSON with session metadata
- Separated by `---`

### `refs/notes/ghost-verified` — Installation witness (`ghost-verified/1.0.0`)
```json
{
  "schema": "ghost-verified/1.0.0",
  "ghost_version": "1.0.0",
  "commit": "<sha>",
  "ts": 1710000042,
  "author": "alice <alice@example.com>",
  "sessions": 2
}
```
Written unconditionally on every commit, even 100% human ones.

---

## CLI Reference

### `ghost`

```
Usage: ghost <command> [options]

Setup:
  init                  Initialize ghost in repo (config + hooks)
  init --yes            One-shot: config + hooks + binaries
  init --owner          Maintainer setup: restrictive policy, CI workflow, GHOST.md
  init --owner --mode locked  Allow no AI-authored lines
  init --owner --github-owner @org/team  Generate CODEOWNERS entries
  init --contributor    Contributor setup: local hooks and notes refs only
  init --interactive    Guided TUI wizard with arrow-key menus
  init --dry-run        Preview what would be configured
  init --global         Install globally for all repos (~/.config/opencode/plugins/ghost.ts)
  install               [DEPRECATED] Redirects to ghost init --yes
  uninstall             Remove ghost from current repo
  install-hooks         Auto-configure hooks for all detected AI agents
  install-hooks --agent <name>   Install hooks for a specific agent
  uninstall-hooks       Remove all AI agent hooks

Inspection:
  audit [<commit>]      Audit committed history using ghost/git-ai notes
  audit --all           Audit all commits with ghost notes
  audit --range R       Audit a specific commit range
  audit --threshold N   Override config threshold for this run
  audit --config-ref R  Load ghost.yml from a git ref (e.g., origin/main)
  audit --json          Machine-readable JSON output
  verify-pr [range]     Simulate PR audit locally using base-branch policy
  verify-pr --base R    Use a different base ref than origin/main
  verify-pr --no-fetch  Do not fetch Ghost notes before auditing
  check                 Preview attribution for staged changes before commit
  check --json          JSON output
  blame <file>          Line-by-line attribution for a file
  blame <file> --json   JSON output
  stats [<range>]       AI% stats for HEAD or a range
  show <commit>         Show formatted ghost note for a commit

Configuration:
  config                Show current ghost.yml values
  config set <key> <val> Set ghost.yml key = value (owner-gated for policy)
  policy                Show owner controls and enforcement stages
  policy set mode <m>   Apply policy preset: permissive, transparent, restrictive, locked
  policy lock           Lock protected policy keys
  policy unlock --force Unlock policy before owner edits
  policy sign           Write ghost-policy.sig for ghost.yml
  policy verify         Verify ghost.yml against ghost-policy.sig
  notes sign [commit]   Sign Ghost notes for a commit
  notes verify [commit] Verify Ghost notes for a commit
  notes verify --range R Verify Ghost note signatures for a range
  banish <path> [...]   Banish files from AI tracking (owner only)
  banish --list         Show banished paths
  banish --clear [...]  Remove files from banish list

Diagnostics:
  doctor                Diagnose ghost setup and suggest fixes
  doctor --fix          Auto-fix issues where possible
  status                Show setup, working tree, sessions, and HEAD note state
  status --json         JSON status output
  explain <topic>       Explain what a command reads and enforces
  explain status        Explain current-state inspection
  explain check         Explain staged pre-commit preview
  explain audit         Explain committed enforcement
  explain verify-pr     Explain local PR simulation

Internal (hook use):
  post-commit           Run post-commit hook processing (reads sessions → writes notes)
  rewrite-log --stdin   Read stdin from post-rewrite hook
  rewrite-log --event <type>  Log a git rewrite event (merge, checkout, etc.)
  working-state --save/--restore/--clear  Manage working state across git operations

Other:
  completion <shell>    Generate shell completion script (bash, zsh, fish)
  version               Print version info
  help [command]        Show help
```

> **Tip:** Use `GHOST_BENCHMARK=1 ghost audit ...` to see per-phase timing.

> **Note:** `ghost audit` only evaluates committed revisions. For uncommitted work, use `ghost status` to see live checkpoint sessions or `ghost check` after staging files.

### `ghost-checkpoint` (called by AI agent hooks)

```
ghost-checkpoint pre  --agent <name> [--file <path>]     Capture snapshot
ghost-checkpoint post --agent <name> --model <model> [--file <path>]  Record session
ghost-checkpoint show                                     Show active session
ghost-checkpoint reset                                    Clear pre-state
```

---

## Configuration — `ghost.yml`

Placed in repo root by `ghost init`. Read by CI audit workflow and CLI commands.

> **Config Pinning:** In CI, ghost reads `ghost.yml` from the **base branch** via `--config-ref`, not from the PR branch. Only the repo owner controls threshold and policy.

```yaml
version: 1
mode: restrictive
locked: false

# Whether this repo mandates ghost for attribution
required: true

# Reject PRs where AI-authored lines exceed this percentage
threshold: 20

# What to do when threshold is exceeded: "block", "warn", or "allow"
on_exceed: block

# Post a comment on the PR with the attribution report
pr_comment: true

# Repo owner email (backward-compatible single-owner form)
owner: admin@example.com

# Owner email allowlist for local protected policy edits
owners:
  - admin@example.com
  - maintainer@example.com

# Files/patterns to exclude from attribution counting (same semantics as .gitignore)
ignore:
  - "*.lock"
  - "vendor/**"

# Commits with no ghost or git-ai notes are always treated as fully human-authored
untagged: human

# How to handle commits missing a ghost-verified note:
# "block" = reject the PR, "warn" = allow with warning, "ignore" = skip check
unverified: block

# Fallback to git-ai notes if ghost notes are absent
gitai_fb: true
```

---

## Agent Hook Configs

Ghost detects and configures hooks for these agents automatically:

| Agent | Config File |
|-------|-------------|
| Claude Code | `~/.claude/settings.json` |
| Cursor | `~/.cursor/mcp.json` + workspace settings |
| GitHub Copilot | `.vscode/tasks.json` |
| Codex | `~/.codex/config.json` |
| OpenCode | `~/.config/opencode/plugins/ghost.ts` |
| Gemini / Junie | JetBrains plugin hooks |
| Generic | `~/.ghost/agents.yml` for custom agents |

Each agent gets two hooks: `pre` captures a snapshot before edits, `post` diffs snapshot vs current and records the session.

---

## GitHub Actions Workflow

Add `.github/workflows/ghost-audit.yml` to your repo:

```yaml
name: Ghost AI Audit
on:
  pull_request:
    types: [opened, synchronize, reopened]
jobs:
  audit:
    runs-on: ubuntu-latest
    permissions:
      contents: read
      pull-requests: write
    steps:
      - uses: actions/checkout@v4
        with:
          fetch-depth: 0
      - name: Fetch ghost notes
        run: |
          git fetch origin refs/notes/ghost:refs/notes/ghost 2>/dev/null || true
          git fetch origin refs/notes/ghost-verified:refs/notes/ghost-verified 2>/dev/null || true
      - name: Install ghost
        run: curl -sSL https://raw.githubusercontent.com/farhankhan197/ghost/main/install.sh | bash
      - name: Run audit
        env:
          GITHUB_TOKEN: ${{ secrets.GITHUB_TOKEN }}
        run: |
          ghost audit \
            --range ${{ github.event.pull_request.base.sha }}..${{ github.event.pull_request.head.sha }} \
            --config-ref origin/${{ github.event.pull_request.base.ref }}
```

Set this check as **required** in branch protection rules. PRs cannot merge if `ghost audit` exits non-zero because owner policy blocked the change.

The workflow pins policy to the base branch. If a PR edits `ghost.yml`, the PR comment will call that out and explain that the current audit still used `origin/<base>:ghost.yml`. If a PR edits or removes the Ghost workflow, the comment flags that as a governance-sensitive change for maintainers to review.

Optional integrity signing:

```bash
ghost policy sign
git add ghost.yml ghost-policy.sig
git commit -m "Sign Ghost policy"
```

Post-commit hooks write `refs/notes/ghost-signatures` automatically for new commits. CI verifies `ghost-policy.sig` and note signatures when they exist. These signatures are Git-stored digest attestations intended to detect accidental or manual tampering; stronger GPG/Sigstore-backed signatures can build on the same flow later.

---

## Non-Goals

- **Detecting AI code without notes** — ghost does not and will never use heuristics to guess whether code is AI-generated. No note = human. Period.
- **Blocking all AI code** — threshold is configurable down to 0% but defaults to 80%. The tool is for visibility and policy enforcement.
- **Replacing git-ai** — ghost is a compatible alternative with a fallback reader for interoperability.
- **Cloud dependency** — ghost is fully local and offline-capable. The only network calls are the optional GitHub API comment and the git notes push.

---

## Performance

For a 39-commit repo with ~400 tracked files:

| Audit Type | Before | After | Improvement |
|---|---|---|---|
| Codebase blame (`ghost audit HEAD`) | 14,869ms | 2,750ms | **81%** |
| Per-commit (`ghost audit --all`) | 15,328ms | 5,872ms | **62%** |

Key optimizations: flat vector blame, binary search in line ranges, file-indexed note entries, overlay cache, parallel `git blame` via thread pool, batch author lookups, batch notes retrieval via `git cat-file --batch`.

---

## Project State

For implementation details, architecture, build system, project structure, and contributor documentation, see **[STATE.md](STATE.md)**.

For the full implementation log and development history, see **[STEPS.md](STEPS.md)**.

---

## Future Ideas

**Near-term:** interactive `ghost blame` TUI, `ghost diff` with inline attribution, squash/rebase authorship reconstruction.

**Medium-term:** self-hostable web dashboard, VS Code extension with inline attribution, `ghost verify` for note integrity checks, signed notes (GPG/SSH).

**Long-term:** open protocol spec for `ghost/1.0.0`, IDE plugins (JetBrains, Neovim), multi-remote note sync, stats API, agent model registry with cost estimates.
