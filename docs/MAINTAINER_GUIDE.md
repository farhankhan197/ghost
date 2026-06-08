# Maintainer Guide

Ghost is designed around repo-owner control. The owner defines policy, contributors install repo Git hooks plus global agent capture hooks, and CI enforces the base-branch policy before merge.

## Owner Setup

Run:

```bash
ghost init
```

If Ghost can verify that you own or maintain the repository, this creates or updates:

- `ghost.yml`
- `GHOST.md`
- `.github/CODEOWNERS`
- `.github/workflows/ghost-audit.yml`
- repo Git hooks
- Git notes push/fetch configuration

Commit those files:

```bash
git add ghost.yml GHOST.md .github/CODEOWNERS .github/workflows/ghost-audit.yml
git commit -m "Add Ghost policy"
git push
```

Then enable branch protection:

- require the Ghost audit workflow
- require CODEOWNER review for policy/workflow changes
- protect the default branch

## Contributor Setup

Contributors run:

```bash
ghost init
```

Ghost detects the checked-in owner policy, preserves it, and installs the local repo Git hooks plus global agent capture hooks needed to capture attribution. Contributors can also force the safe path with `ghost init --contributor`.

## Policy Modes

| Mode | Typical Use |
|---|---|
| `permissive` | Attribution visibility without blocking |
| `transparent` | Require Ghost verification, allow high AI use |
| `restrictive` | Block PRs above a low AI threshold |
| `locked` | Allow no AI-attributed lines |

Change policy with:

```bash
ghost policy set mode restrictive
ghost policy lock
ghost policy sign
```

## CI Enforcement

The generated workflow runs:

```bash
ghost audit --range <base>..<head> --config-ref origin/main --json
```

The important detail is `--config-ref`: policy is read from the protected base branch, not from the PR branch. A contributor cannot weaken `ghost.yml` inside the same PR to pass the audit.

## Local PR Simulation

Before pushing:

```bash
ghost verify-pr origin/main..HEAD
```

Use this to see the same policy decision CI will make.

## Interpreting Commands

| Command | Reads | Enforces |
|---|---|---|
| `ghost status` | setup, working tree, pending sessions, HEAD notes | no |
| `ghost check` | staged diff plus pending sessions | local preview |
| `ghost audit` | committed history and notes | yes |
| `ghost verify-pr` | PR range and base policy | yes |
| `ghost policy` | `ghost.yml` | owner controls |

## Notes Push

Ghost attribution is stored in Git notes, so repos should push note refs:

```bash
git config --add remote.origin.push +refs/notes/ghost:refs/notes/ghost
git config --add remote.origin.push +refs/notes/ghost-verified:refs/notes/ghost-verified
git config --add remote.origin.push +refs/notes/ghost-signatures:refs/notes/ghost-signatures
```

`ghost init` configures these automatically.
