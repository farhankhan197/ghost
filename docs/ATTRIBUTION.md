# Attribution Model

Ghost attribution has two phases:

- Pending attribution before commit.
- Durable attribution after commit.

## Pending Sessions

When a supported AI agent edits a file, Ghost's installed tool hook calls:

```bash
ghost-checkpoint pre --agent opencode --file src/app.cpp
ghost-checkpoint post --agent opencode --model qwen3 --file src/app.cpp
```

The pre command captures a snapshot. The post command diffs snapshot versus current file and writes a session into `.git/ghost/ghost.db`.

Users should not run those commands manually during normal work. `ghost init` installs repo policy/Git hooks and installs the capture layer in the user's global agent config directories for OpenCode, Codex, Claude Code, Cursor, Antigravity (and any other supported agents that are selected/detected during setup). You can also install or remove global capture hooks explicitly with `ghost install-hooks` and `ghost uninstall-hooks`.

Attribution data remains repo-bound in `.git/ghost` and Git notes.

A session records:

- session id
- agent
- model
- Git author
- start/end timestamps
- additions/deletions
- changed file paths
- added line ranges

`ghost status` shows pending sessions. `ghost check` compares pending session ranges with staged additions.

## Commit Notes

After `git commit`, the post-commit hook runs:

```bash
ghost post-commit
```

It clips pending ranges to the actual committed added lines and writes `refs/notes/ghost` only when AI-attributed lines were committed.

It also writes `refs/notes/ghost-verified` on every commit, including human-only commits.

## `refs/notes/ghost`

Format:

```text
src/app.cpp
  sess_abc123 10-14,20
---
{
  "schema": "ghost/1.0.0",
  "commit": "<commit-sha>",
  "sessions": {
    "sess_abc123": {
      "session_id": "sess_abc123",
      "agent": "opencode",
      "model": "qwen3",
      "author": "Name <email@example.com>",
      "ts_start": 1780000000,
      "ts_end": 1780000010,
      "additions": 6,
      "deletions": 0
    }
  }
}
```

The top section is the fast line-range index. The JSON section is metadata.

## `refs/notes/ghost-verified`

Format:

```json
{
  "schema": "ghost-verified/1.0.0",
  "ghost_version": "0.1.17",
  "commit": "<commit-sha>",
  "ts": 1780000011,
  "author": "Name <email@example.com>",
  "sessions": 1
}
```

This note proves Ghost processed the commit. It does not mean the commit contains AI lines.

## Audit Semantics

`ghost audit` is commit-based. It does not read live pending sessions.

For uncommitted work:

- use `ghost status` to inspect captured sessions
- use `git add`
- use `ghost check` to preview staged attribution

For committed work:

- use `ghost audit HEAD`
- use `ghost audit --range base..head`
- use `ghost verify-pr`

## Carry-Forward

If a session is captured but a commit does not include those lines, the session remains pending. It can attach to a later commit when the matching file/ranges are committed.

If a commit consumes only part of a session, the remaining ranges stay pending.

## Interoperability

When `gitai_fb: true`, Ghost can read git-ai notes from `refs/notes/ai` when no Ghost note exists for a commit.
