# Learning Path: Building the Producer Side

This guide recommends building the producer side first while learning the codebase.

## Recommended: Build the Producer Side First

This teaches you **how git notes actually work** while building something functional.

### Step-by-Step Learning Path:

**1. Git::Notes implementation** (`src/git/notes.cpp`)
- Learn: How to interact with git via CLI (`git notes show`, `git notes add`)
- Files to edit: `src/git/notes.cpp` (stub), `src/git/notes.hpp`

**2. Checkpoint binary - `pre` command** (`src/checkpoint/main.cpp`)
- Learn: How to capture current state using `git diff`
- Files to create/edit:
  - `src/checkpoint/snapshot.cpp` - capture pre-edit diff
  - `src/checkpoint/working_log.cpp` - manage `.git/ghost/working.log`

**3. Checkpoint binary - `post` command**
- Learn: How to compute line changes between snapshots
- Files to create/edit:
  - `src/checkpoint/session.cpp` - parse diff, extract changed lines, write session JSON

**4. Post-commit hook logic** (new file)
- Learn: How to read multiple session files and merge them
- File to create: `src/commit/post_commit.cpp`

---

## Why This Order?

| What You Learn | What You Build |
|----------------|----------------|
| Git notes plumbing | Working end-to-end flow |
| Git diff parsing | Session tracking |
| File I/O in C++ | JSON session storage |

Once you have this working, you can actually:
1. Run `ghost-checkpoint pre` → `ghost-checkpoint post`
2. Make a commit → see notes appear
3. Then move to the consumer side (reading/displaying notes)
