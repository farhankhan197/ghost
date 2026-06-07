# Attribution Flow Implementation Plan

## Goal

Make Ghost's existing attribution flow much more precise, deterministic, and audit-safe without adding new product-facing features.

This plan improves the current lifecycle:

```text
agent checkpoint
-> session ranges
-> git commit
-> refs/notes/ghost
-> ghost audit / blame / check
```

The target behavior is simple:

- Ghost notes should describe only lines that are actually attributable in the committed diff.
- Existing AI attribution should survive normal edits, renames, and line movement where Git can prove continuity.
- Audit math should use line ranges and blame overlays as the source of truth, not coarse session totals.
- Duplicate or malformed capture events should never inflate attribution.
- The same paths and ranges should mean the same thing on Windows, Linux, and WSL.

## Current Baseline

Ghost already has strong pieces:

- `LineRangeSet` for compact line ranges.
- `refs/notes/ghost` with per-file, per-session ranges.
- `refs/notes/ghost-verified` for commit verification.
- git-ai fallback note parsing.
- `ghost blame` using Git blame plus Ghost notes.
- `ghost audit` aggregating attribution across commits.
- Post-commit now clips session ranges to actual committed added-line ranges.

The remaining work is mostly about preserving, validating, and counting attribution more carefully.

## Design Principles

- Prefer Git evidence over session claims.
- Treat note ranges as canonical attribution.
- Treat session totals as metadata only.
- Normalize paths once, reuse everywhere.
- Reject unsafe attribution quietly for policy math, but explain it in status/debug output.
- Keep the existing note schema compatible unless a change is absolutely required.
- Add regression tests for every attribution math change.

## Phase 1: Shared Attribution Utilities

### 1. Centralize Repo Path Normalization

Current path normalization appears in multiple places:

- `src/main.cpp`
- `src/commit/post_commit.cpp`
- `src/checkpoint/main.cpp`

Create a shared helper, likely:

```text
src/git/path.hpp
src/git/path.cpp
```

Responsibilities:

- Convert absolute paths under repo root to repo-relative paths.
- Convert `\` to `/`.
- Remove leading `./`.
- Reject paths that escape the repo.
- Optionally preserve case as Git reports it.

Use it in:

- checkpoint capture
- session parsing
- post-commit note generation
- status session display
- check prediction
- blame lookup
- audit file matching

Tests:

- absolute Windows path under repo root
- absolute POSIX path under repo root
- relative path with `./`
- path with backslashes
- path outside repo is rejected

### 2. Move Diff Range Parsing Into Git Module

Post-commit now parses committed added ranges locally. Move this into `src/git/diff`.

Add structures:

```cpp
struct DiffHunk {
    std::string path;
    std::string old_path;
    int old_start;
    int old_count;
    int new_start;
    int new_count;
};

struct DiffRanges {
    std::map<std::string, LineRangeSet> added;
    std::map<std::string, LineRangeSet> deleted;
    std::map<std::string, std::string> renames;
};
```

Add APIs:

```cpp
Diff::getCommittedRanges(commitSha, options)
Diff::getStagedRanges(options)
```

Git commands:

```bash
git diff-tree --root --find-renames --no-ext-diff --unified=0 <sha> -- .
git diff --cached --find-renames --no-ext-diff --unified=0 -- .
```

Tests:

- new file
- appended lines
- insertion in middle
- deletion-only hunk
- mixed modification hunk
- rename with no content changes
- rename with edits

## Phase 2: Post-Commit Attribution Correctness

### 3. Validate Session Entries Before Attribution

Before a session entry can become a Ghost note entry:

- session id must be non-empty
- file path must normalize inside repo
- file must appear in commit diff, or be mapped via rename
- ranges must parse successfully
- intersected ranges must be non-empty
- agent/model should default to `unknown` only after validation

Malformed sessions should not crash post-commit.

Behavior:

- skip invalid entry
- keep verified note creation
- include session count as captured activity
- do not create a Ghost note unless valid attributed ranges exist

Tests:

- malformed range does not crash
- outside-repo path is ignored
- empty session id ignored
- invalid file path ignored
- valid entries in same commit still write note

### 4. Deduplicate Duplicate Agent Sessions

Duplicate opencode/codex hook firings can create multiple sessions with identical content.

Add a fingerprint for session attribution entries:

```text
repo_root + normalized_path + ranges + agent + model + ts window bucket
```

For duplicate sessions:

- prefer the earliest valid session id for note attribution
- merge ranges if the same session id repeats
- do not double-count metadata used in status/check

Keep this internal. No new CLI surface.

Tests:

- identical duplicate session files produce one note entry
- same file/model but different ranges merge correctly only when same session id
- two different sessions touching different ranges remain separate

### 5. Correct Session Metadata Totals In Notes

After clipping ranges to committed lines, raw `session.additions` can overstate attributed lines.

Without changing schema, set note session metadata to the attributed count for that commit:

```text
session.additions = sum(note ranges for that session)
session.deletions = committed deleted ranges if attributable, otherwise raw deletions only when safe
```

This keeps existing readers compatible and makes PR comments/status less misleading.

Tests:

- session claims `20` additions but note range is `6-7`; note JSON says `additions: 2`
- two files for one session sum correctly
- skipped ranges do not count

## Phase 3: Preserve Existing Attribution

### 6. Carry Attribution Through Renames

Use `--find-renames` diff data.

Cases:

- pure rename: no new Ghost note needed, blame still points to old commit
- rename plus edits: new AI ranges should apply to added/modified lines in new path
- prior AI ranges should remain discoverable through blame commits

Important detail:

Ghost notes are keyed by commit and file path at the time of that commit. For old lines, blame returns old commits, but `BlameOverlay` currently looks up the current file path in old notes. That can fail after renames.

Implementation approach:

- Enhance blame parsing to capture original filename per line from porcelain output.
- Add `line.file_path_at_commit`.
- `BlameOverlay` should check attribution using that per-line filename first, then current path as fallback.

Tests:

- AI-authored file is renamed; `ghost blame new/path` still shows AI lines
- rename plus append attributes old AI lines to original session and new lines to new session
- audit after rename does not drop AI percentage to zero

### 7. Preserve Attribution Through Modified AI Lines

When a line with prior AI attribution is edited without an active AI session, it should not automatically become human if the edit is a mechanical or small continuation of that line.

Conservative approach:

- For each staged/committed hunk, map old lines to new lines.
- If an added/replacement line overlaps an old AI-attributed range from the parent commit, carry the attribution forward unless a new session claims it.
- New active session attribution wins over carried attribution.

Data source:

- parent commit Ghost notes
- hunk old/new ranges
- blame at parent commit when necessary

Rules:

- exact replacement of an AI line remains AI
- inserting new lines adjacent to AI lines does not automatically mark them AI unless session claims them
- deleting AI lines reduces current AI count
- mixed hunks only carry attribution for mapped replacement lines

Tests:

- edit an AI line by hand; attribution carries
- insert human line next to AI block; inserted line remains human
- active AI session editing AI line attributes to current session
- deletion of AI line lowers codebase AI count

## Phase 4: Audit Math Hardening

### 8. Use Ranges As Source Of Truth Everywhere

Audit code should avoid using session metadata totals for line counts when range/blame data is available.

Update:

- `runCodebaseBlame`
- PR summary calculations
- file summaries
- commit summaries where possible

Rule:

```text
AI lines = count of blame lines whose commit note contains that file/range
```

Session totals should only be displayed as metadata.

Tests:

- inflated session metadata does not inflate audit
- clipped note ranges determine audit total
- multiple sessions in one file count exact covered lines

### 9. Validate Notes After Writing

After writing `refs/notes/ghost`:

- read the note back
- parse it
- verify every entry has:
  - file path
  - session id present in JSON
  - non-empty range
  - positive line count
- verify JSON session map contains only referenced sessions

If validation fails:

- remove or avoid writing the invalid Ghost note
- still write verified note with captured session count
- return non-zero only if policy requires attribution

Tests:

- bad writer output is detected
- orphan session JSON is not emitted
- empty range never appears in notes

### 10. Deterministic Note Output

Ensure output is stable:

- sort files lexicographically
- sort session ids lexicographically
- merge duplicate ranges
- escape JSON fields consistently
- avoid emitting unreferenced sessions

Some of this already happens by using maps, but it should be intentional and tested.

Tests:

- two equivalent input orderings produce identical note text
- special characters in model/author/path round-trip safely
- duplicate ranges serialize once

## Phase 5: `ghost check` Prediction Accuracy

### 11. Use Staged Range Intersections

`ghost check` currently predicts per-file by matching sessions to file names and then assigning all staged additions.

Improve without changing UX:

- parse staged added ranges
- intersect uncommitted session ranges with staged ranges
- count only intersecting lines
- use prior HEAD notes only for staged ranges that overlap existing AI-attributed lines

This makes `ghost check` match post-commit behavior much more closely.

Tests:

- session touches file but staged hunk does not overlap; predicted AI additions are zero
- partial overlap predicts only overlapping lines
- active checkpoint fallback remains conservative
- ignored files still excluded

### 12. Align Status Counts With Attributable Pending Lines

`ghost status` currently shows raw uncommitted session totals. Keep that useful, but internally distinguish:

- captured session additions
- staged attributable additions

Without adding a new visible feature, avoid implying raw session totals equal the next commit's attribution.

Low-risk output wording:

```text
captured ai additions
```

instead of:

```text
ai additions
```

Tests:

- status still displays sessions
- duplicate sessions do not inflate captured totals after dedupe

## Phase 6: Compatibility With git-ai

### 13. Normalize git-ai Parsed Paths And Sessions

`GitAiReader` should feed the same normalized internal shape as Ghost notes.

Work:

- normalize paths from git-ai notes
- reject invalid ranges
- ensure model/agent mapping is stable
- populate `entries_by_file`

Tests:

- git-ai note with multiple files maps to Ghost shape
- malformed git-ai range is ignored, not fatal
- model name appears in blame/audit

### 14. Match git-ai Granularity Expectations

For compatibility expectations, Ghost should support:

- multiple sessions per file
- sparse ranges
- renamed files through blame filename tracking
- exact per-line model display in `ghost blame`

No new commands required.

## Execution Order

Recommended order:

1. Shared path normalization.
2. Move diff range parsing into `src/git/diff`.
3. Session validation and malformed-session tests.
4. Session dedupe.
5. Correct note metadata totals.
6. Blame filename tracking for renames.
7. Rename attribution tests.
8. Carry attribution through modified AI lines.
9. Audit math cleanup to use ranges only.
10. Post-write note validation.
11. Deterministic note output tests.
12. `ghost check` staged range prediction.
13. git-ai normalization hardening.

## Acceptance Criteria

The attribution flow is considered robust when:

- broad session ranges cannot overcount commit attribution
- duplicate hook events cannot double-count attribution
- malformed session files cannot crash or poison notes
- note metadata totals match committed attributed ranges
- AI attribution survives file renames
- edited AI lines remain attributable when conservative diff mapping proves continuity
- human insertions beside AI code are not automatically marked AI
- audit percentages are derived from line ranges and blame, not raw session totals
- `ghost check` closely predicts post-commit note output
- all behavior is covered by focused integration tests

## Non-Goals For This Pass

These are intentionally out of scope:

- new note schema version
- hosted service
- GitHub App
- cryptographic identity beyond the existing signing work
- UI dashboards
- semantic authorship detection
- probabilistic AI detection

Ghost should remain evidence-based: agent sessions, Git diffs, Git blame, and Git notes.
