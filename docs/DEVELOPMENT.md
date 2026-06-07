# Development

## Requirements

- CMake 3.20+
- C++20 compiler
- Git
- vcpkg with sqlite3

## Build

```bash
cmake -S . -B build
cmake --build build --target ghost ghost-checkpoint
```

## Test

```bash
cmake --build build --target ghost ghost-checkpoint ghost-tests
ctest --test-dir build --output-on-failure
```

Run a focused test:

```bash
build/tests/ghost-tests.exe --gtest_filter=PostCommitIntegration.*
```

## Project Layout

| Path | Purpose |
|---|---|
| `src/main.cpp` | CLI command handlers |
| `src/cli/` | command registry and help text |
| `src/checkpoint/` | `ghost-checkpoint` implementation |
| `src/commit/` | post-commit note writer |
| `src/audit/` | audit and blame overlay logic |
| `src/config/` | `ghost.yml` parsing and writes |
| `src/git/` | Git wrappers |
| `src/hooks/` | hook installers and agent integrations |
| `src/note/` | note schemas and parsers |
| `src/output/` | terminal rendering |
| `src/persist/` | SQLite database layer |
| `src/rewrite/` | history rewrite handling |
| `tests/` | unit and integration tests |

## Useful Manual Flow

```bash
mkdir /tmp/ghost-test
cd /tmp/ghost-test
git init
git config user.name "Test User"
git config user.email "test@example.com"

ghost init --owner --mode restrictive --github-owner @you
git add -A
git commit -m "init ghost"

ghost-checkpoint pre --agent opencode --file src/app.txt
# edit src/app.txt
ghost-checkpoint post --agent opencode --model test-model --file src/app.txt
ghost status
git add src/app.txt
ghost check
git commit -m "AI change"
ghost audit HEAD
```

## Maintenance Rules

- Keep pending attribution DB-backed.
- Keep durable attribution in Git notes.
- Do not add heuristic AI detection.
- Prefer shared path/range helpers over ad hoc parsing.
- Add regression tests for attribution math, note parsing, and Git rewrite behavior.
