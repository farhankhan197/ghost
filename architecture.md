# Ghost Architecture Overview

> **Purpose** – This document gives a high‑level, human‑friendly view of every moving part in the Ghost ecosystem, how they talk to each other, and why they exist. It’s meant for developers, reviewers, and anyone curious about the internals.

---

## 1. Core Concepts (the big picture)

```mermaid
flowchart TD
    subgraph Repo[Git Repository]
        A[Working Tree] --> B[Git Index]
        B --> C[Git Commit]
        C --> D[Remote (origin)]
    end

    subgraph Ghost[Ghost System]
        G1[ghost (CLI)]
        G2[ghost‑checkpoint (binary)]
        G3[SQLite DB (.git/ghost/ghost.db)]
        G4[Git Hooks]
        G5[Notes (`refs/notes/ghost` & `refs/notes/ghost‑verified`)]
    end

    subgraph Agents[AI Agents]
        A1[Claude]
        A2[Cursor]
        A3[Copilot]
        A4[Gemini]
        style Agents fill:#f9f,stroke:#333,stroke-width:2px
    end

    Repo --> G4
    G4 -->|pre‑commit| G2
    G4 -->|post‑commit| G1
    G2 --> G3
    G1 --> G5
    G5 --> Repo
    Agents -->|edit files| G2
    style Ghost fill:#bbf,stroke:#333,stroke-width:2px
```

**Legend**
- **CLI (`ghost`)** – User‑facing commands (`init`, `audit`, `status`, …). Handles configuration, runs the post‑commit logic, and writes notes.
- **Checkpoint binary (`ghost‑checkpoint`)** – Tiny helper invoked by AI‑agent hooks. Takes a *snapshot* before an edit and a *diff* after, turning it into a **session** (metadata about what changed and which agent/model performed it).
- **SQLite DB** – Persistent store inside `.git/ghost/ghost.db`. Holds:
  - *checkpoints* (snapshot files)
  - *sessions* (line‑range changes, agent, model, timestamps)
  - *note index* (fast lookup from commit SHA → note existence)
  - *rewrite log* (history‑rewriting events: rebase, amend, merge)
  - *working state* (temporary state across destructive Git ops)
- **Git Hooks** – Thin shell scripts installed in `.git/hooks/`:
  - `pre‑commit`, `post‑commit`, `pre‑push`, `post‑merge`, `post‑checkout`, `post‑rewrite`, `pre‑merge‑commit`
  - They delegate heavy work to the binaries above and ensure notes are always up‑to‑date.
- **Notes** – Two Git notes refs:
  - `refs/notes/ghost` – contains line‑level attribution JSON.
  - `refs/notes/ghost‑verified` – a lightweight “Ghost was running on this commit” witness written on *every* commit.
- **AI Agents** – Any external editor/tool that integrates with Ghost by installing a tiny hook (`agent_hooks.cpp`). When the agent writes a file, the hook calls `ghost‑checkpoint`.

---

## 2. Detailed Component Walk‑through

### 2.1 `ghost` (CLI)
- Parses sub‑commands (`init`, `install`, `audit`, `blame`, `status`, …).
- `init` creates `ghost.yml`, installs the Git hooks, and optionally installs the binary in the repo.
- `audit` reads notes, overlays them onto `git blame` output, aggregates AI line percentages and enforces the policy defined in `ghost.yml`.
- `status` reports hook health, DB sanity, and config values.

### 2.2 `ghost‑checkpoint`
1. **Pre‑edit** (`ghost‑checkpoint pre --agent <name> [--file <path>]`)
   - Captures the current file(s) into a snapshot stored under `.git/ghost/snapshot/`.
   - Records an entry in the SQLite `checkpoints` table.
2. **Post‑edit** (`ghost‑checkpoint post --agent <name> --model <model> [--file <path>]`)
   - Uses `git diff --no-index --unified=0` to compute the exact line‑range changes between snapshot and current file.
   - Builds a **session** JSON object containing:
     - `agent`, `model`, timestamps, added/deleted line counts, line‑range list.
   - Persists the session in SQLite `sessions` table.

### 2.3 SQLite Persistence (`ghost.db`)
| Table | Purpose |
|-------|---------|
| `checkpoints` | Mapping of snapshot IDs → files + timestamps |
| `sessions` | One session per AI edit (agent, model, line ranges) |
| `note_index` | Fast lookup: commit SHA → note existence flag |
| `rewrite_log` | Records Git rewrite events (rebase, amend, merge) |
| `working_state` | Temporary state saved before destructive ops (merge, checkout) |

All DB writes use **WAL mode** for concurrency safety and are protected by a retry loop on Windows file‑locking errors.

### 2.4 Git Hooks
| Hook | Trigger | Action |
|------|---------|--------|
| `pre‑commit` | `git commit` starts | No‑op (Ghost works via post‑commit) |
| `post‑commit` | After a commit is created | Reads *uncommitted* sessions from SQLite, assembles the final attribution note, writes `refs/notes/ghost` and always writes `refs/notes/ghost‑verified`. Cleans up sessions. |
| `pre‑push` | `git push` begins | Checks `ghost.yml` policy. If `required: true` and notes missing, aborts and prompts (or fails in CI). |
| `post‑merge` / `post‑checkout` / `post‑rewrite` | After a merge, checkout, or rebase | Calls `ghost working-state` to **save** sessions before the operation and **restore** them after, ensuring no data loss. |
| `pre‑merge‑commit` | Before an automatic merge commit | Saves working state so that any in‑flight sessions are kept. |

### 2.5 Note Schemas
- **`refs/notes/ghost`** (human‑readable top part + JSON)
  ```text
  src/main.cpp
    sess_a1b2c3 5-12,18,22-30
  ---
  {
    "schema": "ghost/1.0.0",
    "commit": "<sha>",
    "sessions": {
      "sess_a1b2c3": {
        "agent": "opencode",
        "model": "claude-sonnet-4-5",
        "ts_start": 1710000000,
        "ts_end": 1710000033,
        "additions": 85,
        "deletions": 3
      }
    }
  }
  ```
- **`refs/notes/ghost‑verified`** (simple JSON witness)
  ```json
  {"schema":"ghost-verified/1.0.0","ghost_version":"1.0.0","commit":"<sha>","ts":1710000042,"author":"Name <email>","sessions":2}
  ```

---

## 3. Interaction Flow (Typical Development Cycle)

1. **Repo setup** – `ghost init --yes` creates `ghost.yml` and installs the hooks.
2. **AI edit** – The AI agent (e.g., Claude) saves a file.
   - Its hook calls `ghost‑checkpoint pre` → snapshot stored.
   - After the edit, the hook calls `ghost‑checkpoint post` → session stored.
3. **Commit** – Developer runs `git commit`.
   - `post‑commit` hook reads all pending sessions, merges them, writes the two notes, and clears the sessions.
4. **Push** – `pre‑push` hook validates the policy.
   - If the repo *requires* Ghost and notes are missing, the push is blocked (or in CI the workflow fails).
5. **Rewrites (rebase, amend, squash)** – `post‑rewrite` hook copies existing notes to the new commits using the `rewrite_log` entries.
6. **CI audit** – GitHub Action runs `ghost audit` on PRs, generates a markdown report, and fails the check when the AI‑code threshold is exceeded.

---

## 4. Extensibility Points

- **New AI agents** – Add a JSON descriptor in `~/.ghost/agents.yml` and implement the tiny wrapper that calls `ghost‑checkpoint`. No change to Ghost core is required.
- **Custom policies** – Extend `ghost.yml` with extra keys; the `policy.cpp` module can read them and enforce additional rules.
- **Alternative storage** – SQLite is pluggable; the `persist` layer abstracts over the DB, so a future version could store sessions in RocksDB or a remote KV store.
- **UI integrations** – The CLI outputs JSON (`--json`) for UI front‑ends (VS Code extension, web dashboard).

---

## 5. Summary

Ghost weaves together three lightweight, well‑defined pieces:
1. **A thin CLI (`ghost`)** for user interaction and CI automation.
2. **A fast checkpoint binary (`ghost‑checkpoint`)** that turns every AI edit into a structured session.
3. **A persistent SQLite store** that ties checkpoints, sessions, and notes together, while Git hooks ensure everything stays in sync with the repository history.

Together they give developers **provable AI attribution**, **policy enforcement**, and **full‑history continuity** without sacrificing performance or workflow ergonomics.

---

*Document generated on 2026‑06‑07.*
