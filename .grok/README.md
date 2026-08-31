# Ravo Grok project config

Grok does not scan Codex `.codex/skills`. `.grok/skills` is a relative symlink
to `../.codex/skills`, so both harnesses share one skill tree.

Edit SOP, scripts, and assets only under `.codex/skills/`. Command examples in
those files keep the `.codex/skills/...` paths.

## Invoke

- Slash: `/build-repo`, `/context-handoff`, `/review-and-commit`
- Menu: `/skills`
- Inspect: `grok inspect`

## Catalog

| Skill | Use |
| --- | --- |
| `build-repo` | Configure, build, test, run, or diagnose the Ravo C++20 project using its FreeCM-managed source roots and host tooling |
| `context-handoff` | Capture a durable, evidence-based continuation note for an unfinished Ravo task or investigation |
| `review-and-commit` | Review the working tree, commit and push dirty source-root seeds before pinning and committing the Ravo main repo |
