# Ghost Audit Two-Segment Output

## Concept
`ghost audit <sha>` shows two segments:
1. **CHANGES AT <sha>**: Files edited by AI in this specific commit
2. **CODEBASE ATTRIBUTION**: All other files with AI lines from past commits

Stats: total AI lines at that commit / total lines of code at that commit

## Changes

### 1. `src/audit/auditor.hpp`
- Add `in_commit` bool to `FileBlameSummary`
- Add `commit_ai_lines` and `commit_total_lines` to `CodebaseSummary`

### 2. `src/audit/auditor.cpp`
- Rewrite `runCodebaseBlame()` to:
  - Get files changed in commit via `git diff-tree`
  - Get ghost note for commit to identify AI-edited files
  - Blame all lines at sha, determine which lines belong to this commit
  - Split files into in_commit vs past_ai groups
  - Compute commit-level stats

### 3. `src/output/report.cpp`
- Update `formatCodebaseCLI()` to render two segments with separator
- Update `formatCodebaseJSON()` to include in_commit field and commit stats
