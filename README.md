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

Linux:

```bash
curl -sSL https://raw.githubusercontent.com/farhankhan197/ghost/main/install.sh | bash
```

Windows PowerShell:

```powershell
irm https://raw.githubusercontent.com/farhankhan197/ghost/main/install.ps1 | iex
```

npm:

```bash
npm install -g @musunoa/ghost
```

The npm launcher checks GitHub releases periodically and updates the native
Ghost binaries automatically, including the copies used by local Git hooks in
`~/.ghost/bin`.

## Quick Start

Setup auto-detects whether you are the repo owner or a contributor:

```bash
ghost init
```

Tip: `-v` means **verbose**. Use `ghost --version` or `ghost -V` for version output.

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
| `ghost init` | repo + machine setup | Install Ghost binaries, auto-detect owner/contributor role, install repo Git hooks and global agent capture hooks |
| `ghost init --owner` | repo setup | Create owner policy, hooks, workflow, CODEOWNERS, and contributor guide |
| `ghost init --contributor` | local setup | Preserve checked-in policy while installing local repo hooks and global agent capture hooks |
| `ghost init --global` | machine setup | Install Ghost binaries and global agent capture hooks only (no repo changes) |
| `ghost status` | current repo | Show setup, staged work, pending sessions, and HEAD attribution |
| `ghost check` | staged diff | Preview AI attribution before commit |
| `ghost audit` | committed codebase | Show HEAD codebase attribution and final policy using Git notes |
| `ghost verify-pr` | final PR diff | Enforce base-branch policy on code that survives into the PR |
| `ghost policy` | `ghost.yml` | Inspect and manage owner-controlled rules |
| `ghost config` | `ghost.yml` | Print config, or `set` a key (owner-gated for protected keys) |
| `ghost banish` | `ghost.yml` | Owner-only helper to add/remove ignore patterns (banished paths) |
| `ghost notes sign|verify` | commit notes | Sign/verify `refs/notes/ghost` + `refs/notes/ghost-verified` digests (optional trusted SSH verification) |
| `ghost blame <file>` | file lines | Show grouped live-line attribution; use `--verbose` for line-level detail |
| `ghost show <commit>` | commit | Show parsed attribution note for one commit (Ghost note, or git-ai fallback) |
| `ghost stats [<range>]` | commit range | AI% stats for a range (default `HEAD~1..HEAD`) |
| `ghost completion <shell>` | machine | Generate shell completion script (bash/zsh/fish) |
| `ghost explain <topic>` | machine | Explain what a command reads/enforces (init/status/check/audit/verify-pr/policy) |
| `ghost install-hooks` | machine | Install global agent capture hooks (detected agents or `--agent <name>`) |
| `ghost uninstall-hooks` | machine | Remove global agent capture hooks (all or `--agent <name>`) |
| `ghost uninstall` | repo or machine | Remove Ghost from repo, or remove global install with `--global` |

### CLI conventions

- **Verbose output**: `--verbose` (or `-v`) can be used before commands, e.g. `ghost -v status` or `ghost --verbose audit --range origin/main..HEAD`.
- **Version output**: `ghost --version` or `ghost -V`.
- **Command names**: Ghost accepts full command names and aliases. Short prefixes are allowed only when unambiguous (e.g. `st` → `status`, but `sta` is ambiguous between `status` and `stats`).

## Policy

`ghost.yml` is the repo-owner contract.

```yaml
version: 1
mode: restrictive
required: true
threshold: 20
on_exceed: block
enforcement:
  scope: final_diff
  history: warn
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

In CI, use `ghost verify-pr --base origin/main` so PRs are enforced against the final diff and base-branch policy, not policy changes made inside the PR. Historical commits remain available as warnings or strict blocks through `enforcement.history`. Repositories with `trusted_signers` can require `ghost policy verify --trusted` and `ghost notes verify --trusted` for cryptographic provenance checks.

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

### Hook installation (what Ghost configures)

- **Repo hooks**: `ghost init` installs Git hooks in `.git/hooks/` (`post-commit`, `pre-push`, `post-rewrite`, `post-merge`, `post-checkout`, `pre-merge-commit`).
- **Global agent capture hooks**: `ghost init` also installs tool hooks into your global agent config directories (Codex, Claude Code, Cursor, Antigravity, OpenCode) so edits are captured regardless of which repo you’re in.
- **Notes transport**: `ghost init` configures `remote.origin.push` so Ghost note refs are pushed alongside commits.

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
