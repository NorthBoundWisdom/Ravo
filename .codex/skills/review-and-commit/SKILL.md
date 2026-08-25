---
name: review-and-commit
description: Review the working tree, commit and push dirty source-root seeds before pinning and committing the Ravo main repo. Use when the user asks to commit, 提交, 提交全部diff, 提交并推送, land changes, review-and-commit, or /review-and-commit.
---

# Review and Commit

Only commit when the user explicitly asks. Treat “提交全部 diff” as this whole
procedure, not “git commit in the parent repo.”

Grok loads the same workflow from `.grok/skills/review-and-commit/SKILL.md`.
Keep these two files aligned.

## 1. Inspect every repo, not only the parent

```sh
git branch --show-current
git status --short --branch
git log -5 --oneline
python3 configs/source_roots.py show --format json
```

Then inspect staged, unstaged, and untracked files in the parent. Also
`git status --short --branch` / `git diff` in every dirty dependency checkout:
manual `depsManualPath.*` first, otherwise `build/dependency_seed_repos/<Name>`.
Current direct seeds are `GeoControls` and `LibRaw`. Never edit
`build/dependency_source_roots/*`.

Do not commit `source_roots.lock.jsonc`, `CMakePresets.json`, `build/`,
`.freecm/`, `hooks/path.ini`, secrets, or machine-absolute SDK paths.

## 2. Seeds first (hard stop)

If a seed/source-root is dirty, or the parent already consumes unpublished
Theme tokens / QML / ABI from it:

1. Commit in that dependency repo, on its default branch (`main` for GeoControls).
2. Push that branch to its remote. Seed push is required for this workflow even
   when the user did not say “push the main repo.”
3. Confirm the SHA is on the remote:

   ```sh
   git ls-remote <remote> <sha>
   git ls-remote <remote> refs/heads/<branch>
   ```

4. Set `source_roots.lock.jsonc.in` `dependencies.<Name>.commit` to that
   published SHA. Keep `depsMode: pinned` in the template.
5. Only then commit the parent (product code + template + this skill if it
   changed).

Stop and report if the dependency is not on its default branch, cannot be
pushed, or `ls-remote` does not show the SHA. Never write a local-only SHA
into the template. Never commit the parent first and “push the seed later.”

FreeCM gitlink follows the same rule: the submodule commit must exist on its
remote before the parent records the pointer.

Parent `push` still needs an explicit user request.

## 3. Review and validate

```sh
git diff --stat
git diff
git diff --cached
git diff --check
```

Read new untracked files. Match each file to the requested scope. Preserve
unrelated user work.

Validate in proportion (use `$build-repo` for commands):

- Markdown / skill text: links, paths, `git diff --check`
- CMake / lock template: `--update` when the active lock must change, then
  configure and build an affected target
- C++ / catalog: build affected targets; run the relevant Ravo tests
- Do not call unrun checks “passed”

## 4. Reconcile documentation and TODOs

Before staging, classify whether the diff changes user behavior, product scope,
commands, configuration, architecture/ownership, validation, dependencies, or
legacy migration state.

- Update the single owning document in the same change:
  `Ravo/README.md`, `ARCHITECTURE.md`, `MIGRATION.md`, `TESTING.md`, an ADR,
  or `DevDocs/README.md` as appropriate.
- Fix factual paths, commands, names, versions, and coverage directly.
  Do not perform a broad narrative rewrite unless the user requested it.
- Root `TODO_<TOPIC>.md` files contain unfinished execution only. Move durable
  conclusions to stable truth sources, remove completed items, and hard-cut
  renames without redirects or compatibility copies.
- Validate relative links and search for stale references after deleting or
  renaming documentation.
- If no documentation changes are needed, record the concrete reason in the
  delivery report.

## 5. Commit the parent

1. Stage explicit paths.
2. Re-read `git diff --cached`.
3. Subject: `[type]: concise description`. Types: `feat` `fix` `refactor`
   `style` `docs` `test` `chore` `perf` `ci` `build` `enhancement`.
4. Do not amend unless the user asked.
5. Confirm with `git status --short --branch` and `git show --stat --oneline HEAD`.

If hooks reformat staged files, review those diffs before treating the commit
as done.

## 6. Report

Commit hashes (seed and parent), whether each seed was pushed, `ls-remote`
SHA, whether the lock template was updated, validation actually run, leftover
dirty files, documentation/TODO reconciliation, and that the parent was not
pushed unless requested.
