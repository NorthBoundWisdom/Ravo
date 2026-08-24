---
name: review-and-commit
description: Review all DarkTableNext working-tree changes, run risk-proportional validation, and create an intentional Git commit when explicitly requested. Use for requests to inspect the complete diff, commit all relevant changes, or prepare a clean checkpoint.
---

# Review and Commit

Treat a commit as a verified repository checkpoint. Include only intended work, and only
commit when the user explicitly asks.

## Establish scope

1. Read root `AGENTS.md` and any deeper instructions.
2. Run `git status --short --branch` and `git log -5 --oneline`.
3. Inspect staged, unstaged, and untracked files; do not assume all changes were created in
   the current turn.
4. Match every changed file to the user's requested scope. Preserve unrelated user work and
   ask only if overlap makes safe separation impossible.

Never commit generated `CMakePresets.json`, the active `source_roots.lock.jsonc`, build trees,
dependency checkout contents, temporary reports, credentials, or machine-specific absolute
paths.

## Review the complete change

Inspect full patches, not only `--stat`:

```sh
git diff --stat
git diff
git diff --cached
git diff --check
```

Also read every relevant untracked file. Search for stale references when code, targets,
options, plugins, resources, or documents were removed. Check that build registration,
runtime registration, tests, and documentation agree.

For broad cleanup, verify that remaining search hits are intentional historical notes or
unrelated words, not reachable code. For documentation, verify links and commands against the
current repository.

## Validate proportionally

Use `$build-repo` for build and test details. At minimum:

- Markdown or agent workflow: validate frontmatter/links and run `git diff --check`;
- CMake/dependency changes: regenerate local state, configure, and build an affected target;
- C/C++ behavior: build and run focused or complete unit tests;
- GTK/UI: build the application and verify the affected interaction path;
- GPU/image changes: apply the documented CPU-correctness and benchmark gates.

Record commands and results. Do not translate “not run” into “passed.”

## Create the commit

1. Stage explicit files or hunks; avoid broad staging until all untracked files are audited.
2. Inspect `git diff --cached --stat` and the full `git diff --cached`.
3. Use the hook subject form `[type]: concise description`. Valid types are listed
   in `hooks/README.md`.
4. Commit without amending unless the user explicitly requested an amend.
5. Verify `git status --short --branch` and `git show --stat --oneline --decorate HEAD`.
   If hooks rewrote staged files, review those formatting-only diffs before
   treating the commit as complete.

If relevant changes remain uncommitted, state exactly what and why. Do not push unless the user
also requested publishing.

## Report

Return the commit hash and subject, the scope included, validation actually run, working-tree
state, and any residual risk. If no commit was created, lead with the blocking reason.
