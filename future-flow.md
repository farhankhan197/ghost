# Future Flow: Owner-Controlled AI Attribution

## Goal

Ghost should feel like a complete maintainer-controlled policy system for open-source repositories, not only a CLI that audits attribution notes.

The core product promise:

> A repo owner can define how much AI-generated code is allowed, contributors can follow a clear setup path, and PRs are blocked or allowed by transparent policy using committed Git evidence.

The flow should be obvious from first install through merge:

```text
Maintainer initializes repo
-> contributor clones repo
-> contributor runs local setup
-> AI edits are captured
-> commit writes durable notes
-> push sends notes
-> PR audit reads owner policy from base branch
-> merge is allowed, warned, or blocked
```

## Product Principles

- Repo owner policy is the source of truth.
- Contributors should not need to understand Git notes to comply.
- CI should be the final enforcement authority.
- `ghost.yml` changes in a PR must not weaken the policy used to audit that same PR.
- The CLI should always explain whether it is showing current state, staged previews, or committed enforcement.
- Honest contributors should get exact fix commands.
- Restrictive open-source use cases should be first-class, not an edge case.

## Command Mental Model

Every command should map to one stage of the lifecycle.

| Command | Stage | Reads | Purpose |
|---|---|---|---|
| `ghost init --owner` | Maintainer setup | Git user, repo root | Create owner policy, hooks, CI workflow, contributor docs |
| `ghost init --contributor` | Contributor setup | `ghost.yml`, local hooks, agent configs | Install local compliance tooling |
| `ghost policy` | Policy inspection | `ghost.yml` | Show owner, rules, and enforcement meaning |
| `ghost status` | Current state | repo setup, working tree, sessions, HEAD notes | Explain what exists right now |
| `ghost check` | Pre-commit preview | staged diff plus live sessions | Predict attribution for files already staged |
| `ghost audit` | Committed enforcement | commits plus Git notes | Enforce policy on committed history |
| `ghost verify-pr` | Local CI simulation | range plus base-branch policy | Run the same policy gate contributors will see in PR |

Short version:

```text
status    = what exists now
check     = staged preview
audit     = committed enforcement
policy    = owner rules
verify-pr = local PR simulation
```

## 1. Maintainer Setup: `ghost init --owner`

`ghost init --owner` should become the canonical repo-owner flow.

It should:

- Detect the current Git email.
- Ask whether to make that email the repo policy owner.
- Generate or update `ghost.yml`.
- Offer policy modes instead of raw config first.
- Install repo hooks.
- Configure Git notes push/fetch refs.
- Add or update `.github/workflows/ghost-audit.yml`.
- Optionally create `GHOST.md` as a contributor guide.
- Print exact GitHub branch protection instructions.

Example:

```bash
ghost init --owner
```

Expected output:

```text
Owner policy created:
  owner: farhan@example.com
  mode: restrictive
  required: true
  threshold: 20%
  unverified: block

Installed:
  ghost.yml
  .github/workflows/ghost-audit.yml
  .git/hooks/post-commit
  .git/hooks/pre-push
  git notes push/fetch refs

Next:
  1. Commit ghost.yml and .github/workflows/ghost-audit.yml
  2. Push to main
  3. In GitHub branch protection, require the "Ghost Audit" check
```

## 2. Policy Modes

Raw YAML is powerful, but maintainers need product-level presets.

Add:

```bash
ghost policy set mode permissive
ghost policy set mode transparent
ghost policy set mode restrictive
ghost policy set mode locked
```

Suggested modes:

```yaml
# permissive
required: false
threshold: 100
on_exceed: warn
unverified: warn
```

```yaml
# transparent
required: true
threshold: 80
on_exceed: warn
unverified: warn
```

```yaml
# restrictive
required: true
threshold: 20
on_exceed: block
unverified: block
```

```yaml
# locked
required: true
threshold: 0
on_exceed: block
unverified: block
```

The restrictive and locked modes are the main open-source governance story.

## 3. Protected Policy Editing

Once `owner` is set, protected policy keys should only be editable by the configured owner or owner allowlist.

Protected keys:

- `owner`
- `owners`
- `mode`
- `required`
- `threshold`
- `on_exceed`
- `pr_comment`
- `untagged` / `untagged_policy`
- `unverified` / `unverified_policy`
- `gitai_fb` / `gitai_fallback`
- `ignore`
- future `policy_lock`

Future schema:

```yaml
version: 1

owners:
  - farhan@example.com
  - maintainer@example.com

policy:
  mode: restrictive
  locked: true

required: true
threshold: 20
on_exceed: block
unverified: block
```

Potential next step:

- Support `owners:` as a list, while keeping `owner:` as a backward-compatible alias.

## 4. Contributor Clone Flow

After a contributor clones a required repo, Ghost should quickly tell them what is missing and how to comply.

Flow:

```bash
git clone https://github.com/org/project
cd project
ghost status
```

If `required: true` and local setup is missing, show:

```text
This repository requires Ghost attribution.

Missing:
  local Git hooks
  Git notes push/fetch refs
  AI agent hooks

Run:
  ghost init --contributor

This will:
  install local hooks
  configure Git notes refs
  configure supported AI agents
```

Contributor setup:

```bash
ghost init --contributor
```

Expected behavior:

- Read owner policy from repo `ghost.yml`.
- Install local hooks only.
- Do not rewrite owner policy unless explicitly asked.
- Configure notes push/fetch.
- Detect AI agents and install hooks.
- Print exact next command:

```text
You are ready to contribute.

Before committing:
  ghost status

After staging:
  ghost check

Before pushing:
  ghost verify-pr origin/main..HEAD
```

## 5. AI Edit Capture Flow

The contributor should not need to know about checkpoint internals, but the system should be reliable and explainable.

Current low-level flow:

```text
agent pre-tool hook
-> ghost-checkpoint pre --agent <name>
-> snapshot working tree

agent writes code

agent post-tool hook
-> ghost-checkpoint post --agent <name> --model <model>
-> diff snapshot vs current
-> assign changed lines to session

git commit
-> post-commit hook
-> condense sessions
-> write refs/notes/ghost
-> write refs/notes/ghost-verified
```

Product-level improvements:

- Show agent/model capture health in `ghost status`.
- Warn when a supported agent is installed but its hook is missing.
- Deduplicate duplicate post-tool events.
- Always bind sessions to the current repo root.
- Show uncommitted session paths relative to the repo.
- Make model name visible in `status`, `check`, `audit`, and PR comments.

## 6. Pre-Push Enforcement

The pre-push hook should produce clear, policy-shaped errors.

If setup is missing:

```text
Push blocked by repo owner policy.

Reason:
  commit abc123 has no ghost-verified note

Owner policy:
  required: true
  unverified: block

Fix:
  ghost init --contributor
  recommit your changes
```

If the AI threshold is exceeded:

```text
Push blocked by repo owner policy.

AI-authored lines:
  actual: 41%
  allowed: 20%

Fix options:
  reduce AI-authored changes
  split the PR
  ask a maintainer to change policy
```

If notes are not configured to push:

```text
Push blocked because Ghost notes are not configured for this remote.

Run:
  ghost init --contributor

Or manually configure:
  git config --add remote.origin.push refs/notes/ghost:refs/notes/ghost
  git config --add remote.origin.push refs/notes/ghost-verified:refs/notes/ghost-verified
```

## 7. PR Audit as Source of Truth

The GitHub Action should be the final policy gate.

It should:

- Fetch Ghost notes.
- Build or install Ghost.
- Run `ghost audit --range BASE..HEAD --config-ref origin/main --json`.
- Read policy from the protected base branch.
- Ignore policy weakening inside the PR branch.
- Fail if `audit.blocked == true`.
- Post a clear PR comment.

The PR comment should show:

- Status: passed, warning, or blocked.
- Owner policy used.
- Base branch config ref used.
- Whether the PR modified `ghost.yml`.
- Total AI density.
- Per-commit attribution.
- Missing verified notes.
- Files over threshold.
- Suggested fix commands.

If the PR modifies `ghost.yml`, the comment should say:

```text
This PR modifies ghost.yml.

The audit still used policy from origin/main.
Policy changes only apply after maintainers merge them.
```

This makes config pinning visible and prevents confusion.

## 8. Local PR Simulation: `ghost verify-pr`

Add:

```bash
ghost verify-pr origin/main..HEAD
```

It should be a productized alias for:

```bash
ghost audit --range origin/main..HEAD --config-ref origin/main
```

But with better contributor-focused output:

```text
Local PR verification

Policy:
  source: origin/main:ghost.yml
  mode: restrictive
  threshold: 20%
  unverified: block

Range:
  origin/main..HEAD

Result:
  BLOCKED

Reason:
  commit abc123 is missing ghost-verified note

Fix:
  ghost init --contributor
  recommit the affected changes
```

Optional flags:

```bash
ghost verify-pr
ghost verify-pr origin/main..HEAD
ghost verify-pr --base origin/main
ghost verify-pr --json
```

Default behavior:

- Detect upstream/base branch if possible.
- Fall back to `origin/main`.
- Explain exactly which range is being audited.

## 9. Explainability Commands

Add either a standalone command:

```bash
ghost explain status
ghost explain check
ghost explain audit
ghost explain policy
```

Or flags:

```bash
ghost status --explain
ghost check --explain
ghost audit --explain
```

Each explanation should answer:

- What data does this command read?
- Is it pre-commit or post-commit?
- Does it enforce policy?
- What should I do next?

Example:

```text
ghost check

Reads:
  staged diff
  uncommitted Ghost sessions

Does not read:
  unstaged files
  future commits
  PR branch policy

Use it when:
  you have run git add and want to preview attribution before commit
```

## 10. Stronger Policy Integrity

Future hardening options:

- `owners:` allowlist.
- `policy.locked: true`.
- Signed policy commits.
- Signed Ghost notes.
- CODEOWNERS integration for `ghost.yml`.
- CI warning when `ghost.yml` changed without owner approval.
- GitHub App that verifies policy changes were approved by maintainers.
- Required check that blocks PRs which remove Ghost workflow files.
- Audit mode that detects whether Ghost notes were fetched successfully.

Potential `ghost policy lock` flow:

```bash
ghost policy lock
```

Writes:

```yaml
policy:
  locked: true
```

And prints:

```text
Policy locking enabled.

Recommended GitHub settings:
  - Require CODEOWNERS review for ghost.yml
  - Require Ghost Audit check before merge
  - Prevent force pushes to main
```

## 11. Generated Contributor Guide: `GHOST.md`

`ghost init --owner` should offer to create `GHOST.md`.

Suggested contents:

```markdown
# AI Attribution Policy

This repository uses Ghost to track AI-authored code.

## Maintainer Policy

- Ghost required: true
- AI threshold: 20%
- Unverified commits: block

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
```
```

## 12. Better Failure Messages

Every block should include:

- What failed.
- Which policy caused it.
- Where the policy came from.
- Whether the command is local or CI.
- Exact fix commands.

Bad:

```text
Audit failed.
```

Good:

```text
Ghost audit blocked this PR.

Policy:
  source: origin/main:ghost.yml
  unverified: block

Failure:
  commit abc123 has no ghost-verified note

Fix:
  install Ghost, recommit the change, and push notes

Commands:
  ghost init --contributor
  git commit --amend --no-edit
  git push
```

## 13. Implementation Roadmap

### Phase 1: Clarify Current Product

- Add `ghost policy`.
- Make `status`, `check`, and `audit` descriptions explicit.
- Ensure CI fails on full `audit.blocked`.
- Parse documented config aliases.
- Make JSON output safe for CI parsing.
- Remove confusing platform-specific fallback docs.

### Phase 2: Maintainer and Contributor Flows

- Add `ghost init --owner`.
- Add `ghost init --contributor`.
- Generate or update GitHub Actions workflow.
- Generate optional `GHOST.md`.
- Add policy mode presets.
- Add `ghost verify-pr`.

### Phase 3: Stronger Governance

- Add `owners:` allowlist.
- Add policy locking.
- Add CODEOWNERS guidance or automation.
- Detect PR changes to `ghost.yml`.
- Detect PR changes that remove Ghost workflow enforcement.
- Improve PR comments with policy source and fix commands.

### Phase 4: Trust and Integrity

- Signed notes.
- Signed policy.
- GitHub App enforcement.
- Better note fetch diagnostics.
- Remote note synchronization checks.
- Maintainer dashboard or badge.

## Open Questions

- Should `owner` be a Git email, GitHub username, or both?
- Should `owners:` be required for organizations?
- Should `locked` mode allow any local `ghost config set`, or require direct file edits plus CODEOWNERS?
- Should threshold count only new PR lines or whole codebase attribution?
- Should `required: true` block commits locally at pre-commit, or only block at pre-push and CI?
- Should `ghost verify-pr` fetch notes automatically?
- Should `ghost init --contributor` ever modify `ghost.yml`, or only local Git/agent state?

## Desired End State

The ideal maintainer story:

```bash
ghost init --owner
git add ghost.yml .github/workflows/ghost-audit.yml GHOST.md
git commit -m "Add Ghost AI attribution policy"
git push
```

Then enable the required GitHub check:

```text
Ghost Audit
```

The ideal contributor story:

```bash
git clone https://github.com/org/project
cd project
ghost init --contributor
# work with AI tools
git add .
ghost check
git commit -m "Implement feature"
ghost verify-pr origin/main..HEAD
git push
```

The ideal PR story:

```text
Ghost Audit: BLOCKED

Policy from origin/main:
  mode: restrictive
  threshold: 20%
  unverified: block

Reason:
  AI-authored lines are 41%

Fix:
  reduce AI-authored changes, split the PR, or ask a maintainer to change policy
```

That is the complete restrictive open-source workflow: owner policy, contributor setup, durable attribution, CI enforcement, and merge protection.
