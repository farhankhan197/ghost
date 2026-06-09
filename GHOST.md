# AI Attribution Policy

This repository uses Ghost to track AI-authored code.

## Maintainer Policy

- Ghost required: true
- AI threshold: 80%
- Unverified commits: warn

## Contributor Setup

Run:

```bash
ghost init --contributor
```

Before committing:

```bash
ghost status
git add <files>
ghost check
```

Before pushing:

```bash
ghost verify-pr origin/main..HEAD
```

Pull requests are audited by the Ghost Audit required check.
