# Ghost Policy

This repository uses Ghost to record AI-assisted edits and enforce repository-owner policy.

## Contributor Flow

Install local Ghost hooks:

```bash
ghost init --contributor
```

Before every commit:

```bash
ghost status
git add <files>
ghost check
git commit -m "Your change"
ghost audit HEAD
```

## What Reviewers Expect

- AI-assisted edits should have Ghost attribution.
- `ghost check` should explain staged attribution before commit.
- `ghost audit` should pass for the committed range.
- Policy changes must be reviewed like security-sensitive changes.

## Enforcement

The checked-in policy lives in `ghost.yml`. CI should audit pull requests with the base-branch policy so contributors cannot weaken the rules inside the same PR.

For the full maintainer workflow, see [docs/MAINTAINER_GUIDE.md](docs/MAINTAINER_GUIDE.md).

