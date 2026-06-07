# Architecture

Ghost is a local Git attribution system made of four parts:

- `ghost`: the user-facing CLI.
- `ghost-checkpoint`: a small helper called by AI agent hooks.
- `.git/ghost/ghost.db`: repo-local SQLite state.
- Git notes: durable attribution attached to commits.

## Runtime Flow

```text
agent tool event
-> ghost-checkpoint pre --agent <agent> --file <path>
-> snapshot saved under .git/ghost/snapshot
-> checkpoint row saved in SQLite
-> agent edits file
-> ghost-checkpoint post --agent <agent> --model <model> --file <path>
-> session row saved in SQLite
-> git commit
-> ghost post-commit
-> refs/notes/ghost and refs/notes/ghost-verified are written
```

The SQLite DB is the canonical store for pending sessions. Legacy JSON session files and `working.log` pre-state files are not part of the current flow.

## Main Modules

| Path | Responsibility |
|---|---|
| `src/main.cpp` | CLI command dispatch and user-facing workflows |
| `src/checkpoint/` | Pre/post snapshot capture and session construction |
| `src/persist/db.*` | SQLite persistence for checkpoints, sessions, indexes, rewrite state |
| `src/commit/` | Post-commit note generation and note index updates |
| `src/audit/` | Committed-history and codebase attribution auditing |
| `src/git/` | Git command wrappers for notes, blame, diff, paths, repo metadata |
| `src/note/` | Ghost, verified, and git-ai note parsing/writing |
| `src/hooks/` | Git hook and agent hook installation |
| `src/rewrite/` | Note and session preservation across rewrite/reset flows |
| `src/output/` | CLI styling and reports |

## Persistence

Ghost stores transient repo state in `.git/ghost/ghost.db`.

| Table | Purpose |
|---|---|
| `checkpoints` | Pre-edit snapshot metadata |
| `sessions` | Pending and committed agent edit sessions |
| `note_index` | Fast local cache of commit note state |
| `rewrite_log` | Rebase/amend/reset event history |
| `working_state` | Temporary state saved across Git operations |
| `recovery_sessions` | Sessions recovered from unwound commits |

Durable attribution is not the DB. Durable attribution is the Git notes attached to commits.

## Git Notes

Ghost writes:

- `refs/notes/ghost`: line-level attribution.
- `refs/notes/ghost-verified`: witness that Ghost processed the commit.
- `refs/notes/ghost-signatures`: digest attestations for Ghost notes.

Ghost can optionally read `refs/notes/ai` for git-ai interoperability when `gitai_fb: true`.

## Hooks

Repo hooks installed by `ghost init`:

- `post-commit`: converts pending sessions to commit notes.
- `pre-push`: enforces required-note policy before pushing.
- `post-rewrite`: migrates notes after amend/rebase.
- `post-merge`, `post-checkout`, `pre-merge-commit`: preserve pending state around Git operations.

Agent hooks call `ghost-checkpoint` around file-writing tools.

## Auditing

`ghost audit` reads committed Git history and note refs. It overlays note ranges onto `git blame` data, aggregates AI line counts, then enforces `ghost.yml`.

`ghost check` is separate: it reads the staged diff and pending sessions to preview what a future commit would do.
