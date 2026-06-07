# Fix: opencode edits not tracked in audit

## Root cause
On Windows, session file paths use `\` (from `fs::relative().string()`) but `git diff-tree --name-only` always uses `/`. The string comparison in `post_commit.cpp:308` fails — entries are discarded, no ghost note is written, audit shows nothing.

## Changes

### 1. `src/commit/post_commit.cpp` line ~308
Add backslash-to-forward-slash normalization before the `commitFiles.find()` comparison:

```cpp
                if (!ec) entryPath = rel.string();
                }
                for (char& c : entryPath) if (c == '\\') c = '/';
                if (commitFiles.find(entryPath) == commitFiles.end()) continue;
```

### 2. `src/checkpoint/main.cpp` lines ~91 and ~145 (two locations)
Wrap the `targetFile = rel.string()` in a block and normalize separators:

```cpp
                if (!ec) {
                    targetFile = rel.string();
                    for (char& c : targetFile) if (c == '\\') c = '/';
                }
```
