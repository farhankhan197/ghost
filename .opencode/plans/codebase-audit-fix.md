# Codebase Audit Fix

## Issue
`ghost audit` counts total lines only from files with AI lines, not from the entire codebase snapshot.

## Fix
In `src/audit/auditor.cpp`, move `grandTotal += fbs.total_lines` outside the `if (fbs.ai_lines > 0)` block so all files contribute to the denominator.

## Changes
- `src/audit/auditor.cpp` line ~384: count all files toward `grandTotal`
