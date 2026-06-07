# Contributing

Thanks for helping improve Ghost. This project is about making AI code attribution explicit, reviewable, and enforceable for repository owners.

## Setup

```bash
git clone https://github.com/farhankhan197/ghost.git
cd ghost
ghost init --contributor
cmake -S . -B build
cmake --build build --target ghost ghost-checkpoint ghost-tests
ctest --test-dir build --output-on-failure
```

Use the normal Ghost flow before committing:

```bash
ghost status
git add <files>
ghost check
git commit -m "Describe the change"
ghost audit HEAD
```

## Pull Requests

- Keep changes focused.
- Include tests for attribution, policy, hook, note, or CLI behavior changes.
- Update docs when commands, policy fields, or maintainer workflows change.
- Do not weaken `ghost.yml`, CODEOWNERS, or workflow enforcement to make a PR pass.
- Push Ghost note refs when a change contains recorded AI edits.

## Development Notes

See [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) for build details, project layout, and manual end-to-end testing.

