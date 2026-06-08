# Ghost

AI code attribution and repo-owner policy enforcement for Git.

Ghost records which AI agent changed which lines, stores that attribution in Git notes, and lets maintainers enforce AI-code policy in local checks and CI. It is built for open-source repositories where the owner needs clear provenance rules rather than voluntary self-reporting.

## What Ghost Does

- Captures AI edits from supported agent hooks.
- Stores pending attribution in `.git/ghost/ghost.db`.
- Writes durable notes on commit:
  - `refs/notes/ghost`
  - `refs/notes/ghost-verified`
  - `refs/notes/ghost-signatures` for digest or trusted SSH signatures
- Shows current setup and pending sessions with `ghost status`.
- Predicts staged attribution before commit with `ghost check`.
- Audits committed history with `ghost audit`.
- Supports owner-controlled policy through `ghost.yml`, CODEOWNERS, and GitHub Actions.

Ghost does not guess whether code is AI-generated. No note means human unless repo policy treats missing verification as a warning or block.

## Install

macOS / Linux:

```bash
curl -sSL https://raw.githubusercontent.com/farhankhan197/ghost/main/install.sh | bash
```

Windows PowerShell:

```powershell
irm https://raw.githubusercontent.com/farhankhan197/ghost/main/install.ps1 | iex
```

npm:

```bash
npm install -g ghost-ai
```

Other package channels are maintained for Homebrew, Winget, and Scoop.

## Quick Start

Setup auto-detects whether you are the repo owner or a contributor:

```bash
ghost init
```

Maintainers can still be explicit:

```bash
ghost init --owner --mode restrictive --github-owner @your-org-or-user
git add ghost.yml GHOST.md .github/CODEOWNERS .github/workflows/ghost-audit.yml
git commit -m "Add Ghost policy"
```

Contributor setup:

```bash
ghost init --contributor
```

Daily flow:

```bash
ghost status
git add <files>
ghost check
git commit -m "Your change"
ghost audit HEAD
```

## Command Model

| Command | Scope | Purpose |
|---|---|---|
| `ghost init` | repo + machine setup | Auto-detect owner/contributor role, install repo Git hooks and global agent capture hooks |
| `ghost init --owner` | repo setup | Create owner policy, hooks, workflow, CODEOWNERS, and contributor guide |
| `ghost init --contributor` | local setup | Preserve checked-in policy while installing local repo hooks and global agent capture hooks |
| `ghost status` | current repo | Show setup, staged work, pending sessions, and HEAD attribution |
| `ghost check` | staged diff | Preview AI attribution before commit |
| `ghost audit` | committed history | Enforce policy using Git notes |
| `ghost verify-pr` | PR range | Simulate CI with base-branch policy |
| `ghost policy` | `ghost.yml` | Inspect and manage owner-controlled rules |
| `ghost blame <file>` | file lines | Show line-by-line attribution |

## Policy

`ghost.yml` is the repo-owner contract.

```yaml
version: 1
mode: restrictive
required: true
threshold: 20
on_exceed: block
unverified: block
owner: maintainer@example.com
owners:
  - maintainer@example.com
ignore:
  - ".git/"
  - "vendor/**"
gitai_fb: true
trusted_signers:
  - name: Maintainer
    email: maintainer@example.com
    ssh_key: ssh-ed25519 AAAA...
```

In CI, use `ghost audit --config-ref origin/main` so PRs are audited against base-branch policy, not policy changes made inside the PR. Repositories with `trusted_signers` can require `ghost policy verify --trusted` and `ghost notes verify --trusted` for cryptographic provenance checks.

## Attribution Lifecycle

```text
AI agent hook
-> ghost-checkpoint pre/post
-> SQLite pending session
-> git commit
-> post-commit hook
-> refs/notes/ghost + refs/notes/ghost-verified
-> ghost audit / blame / verify-pr
```

Pending sessions live in the repo-local SQLite DB. Durable attribution lives in Git notes and should be pushed with commits.

## Documentation

- [Contributor Guide](CONTRIBUTING.md)
- [Repository Ghost Policy](GHOST.md)
- [Maintainer Guide](docs/MAINTAINER_GUIDE.md)
- [Attribution Model](docs/ATTRIBUTION.md)
- [Architecture](docs/ARCHITECTURE.md)
- [Development](docs/DEVELOPMENT.md)
- [Design System](docs/DESIGN_SYSTEM.md)
- [Security](SECURITY.md)

## Build From Source

```bash
cmake -S . -B build
cmake --build build --target ghost ghost-checkpoint
ctest --test-dir build --output-on-failure
```

Requires C++20, CMake 3.20+, Git, and sqlite3 through vcpkg.

## Non-Goals

- Detecting AI code by heuristics.
- Rewriting old commits to invent attribution.
- Requiring cloud infrastructure.
- Replacing Git review or maintainer judgment.

## License

MIT.
