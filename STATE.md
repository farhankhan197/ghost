# Ghost – Project State Overview

## What is Ghost?
Ghost is an open‑source tool that records **exact AI attribution** for every line of code committed to a Git repository. It uses Git notes (`refs/notes/ghost` and `refs/notes/ghost-verified`) to store cryptographically‑linked metadata, guaranteeing provable provenance without heuristics.

## Key Benefits for Users
- **Transparency** – know which AI wrote which lines.
- **Policy Enforcement** – CI can block PRs that exceed a configurable AI‑generated‑code threshold.
- **Cross‑Platform** – works on Windows, macOS, Linux, and WSL.
- **Multiple Distribution Channels** – script, npm, Homebrew, Winget, Scoop.

## High‑Level Architecture
```
ghost (CLI) ──► Git hooks (pre‑commit, post‑commit, pre‑push, …)
   │                     │
   │                     └─► ghost‑checkpoint (captures snapshots & diffs)
   │
   └─► SQLite DB (.git/ghost/ghost.db)
          • checkpoints
          • sessions
          • note index
          • rewrite log
          • working state
```
- **Checkpoint binary** (`ghost-checkpoint`) is invoked by AI agents before and after edits to capture snapshots and compute line‑range changes.
- **Post‑commit hook** aggregates sessions, writes notes, and cleans up.
- **Rewrite log** tracks history‑rewriting operations (rebase, amend, merge) to migrate notes.
- **SQLite persistence** ensures fast look‑ups and reliable state across Git operations.

## Build & Install
```bash
# Build from source (requires C++20, CMake, Ninja, vcpkg)
git clone https://github.com/farhankhan197/ghost.git
cd ghost
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
# Install binaries globally
./build/ghost install --global
```
Or use one of the pre‑built installers:
- `curl …/install.sh | bash` (macOS/Linux/WSL)
- PowerShell script via `irm …/install.ps1 | iex` (Windows)
- `npm install -g ghost-ai`
- Homebrew, Winget, Scoop packages.

## Quick Usage
```bash
# In any repo
ghost init --yes        # sets up hooks & default ghost.yml
# Edit files (AI‑assisted editors trigger hooks automatically)
git add .
git commit -m "Add feature"
git push               # pre‑push enforces policy
```
Key CLI commands:
- `ghost status` – view config & hook health.
- `ghost audit [range]` – run attribution audit.
- `ghost blame <file>` – per‑line attribution.
- `ghost check` – predictive audit of staged changes.
- `ghost config set <key> <value>` – edit `ghost.yml`.

## CI Integration
Add the following workflow to `.github/workflows/ghost-audit.yml`:
```yaml
name: Ghost AI Audit
on:
  pull_request:
    types: [opened, synchronize, reopened]
jobs:
  audit:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
        with:
          fetch-depth: 0
      - name: Fetch ghost notes
        run: |
          git fetch origin refs/notes/ghost:refs/notes/ghost || true
          git fetch origin refs/notes/ghost-verified:refs/notes/ghost-verified || true
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
The workflow blocks PR merges when the audit exits with a non‑zero code.

## Contributing & Roadmap
- Follow `CONTRIBUTING.md` for guidelines.
- Current roadmap includes: GUI dashboard, VS Code extension, signed notes, and multi‑remote sync.

---
*Last Updated: June 6, 2026*
