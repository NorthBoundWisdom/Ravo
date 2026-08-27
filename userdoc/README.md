# Ravo User Handbook

`userdoc/` is the source for the English Ravo end-user handbook. It documents
the current source-backed behavior of Ravo Studio and the supported `ravo` CLI.

The handbook follows the current development baseline. It is not a promise that
every migration item in the repository is already a finished darktable
replacement, and it does not document the frozen `legacy/` application as a
supported runtime.

## Read the handbook

Start with [the handbook home page](docs/en/index.md), then follow:

1. [Install and launch](docs/en/quick-start/install-and-launch.md)
2. [First launch and import](docs/en/quick-start/first-launch-and-import.md)
3. [Five-minute tour](docs/en/quick-start/five-minute-tour.md)
4. [Library and review](docs/en/guides/library-and-review.md)
5. [Viewer and scopes](docs/en/guides/viewer-and-scopes.md)
6. [Develop](docs/en/guides/develop.md)
7. [Export and sharing](docs/en/guides/export-and-share.md)

The [CLI guide](docs/en/guides/cli.md) is the best entry point for automation
and headless workflows.

## Build the handbook locally

From the repository root:

```text
python3 -m pip install -r userdoc/requirements.txt
python3 -m mkdocs serve -f userdoc/mkdocs.yml
python3 -m mkdocs build -f userdoc/mkdocs.yml --strict
```

The generated handbook is written to `userdoc/site/` and is local build output;
it is intentionally not part of the source documentation.

## GitHub Pages publication

The repository publishes the handbook through
`.github/workflows/userdoc-pages.yml`. Pull requests run the strict MkDocs
build without deploying. A push to `main` that changes `userdoc/**` or the
workflow builds `userdoc/site/` and deploys it to:

<https://northboundwisdom.github.io/Ravo/>

The generated site is uploaded as a Pages artifact and is not committed to a
branch. Keep secrets and private material out of the handbook because the
published site is public.

## Authoring rules

- Write user-facing behavior, not internal target or class descriptions.
- Treat `Ravo/README.md` and the Ravo source as the behavior authority.
- Keep original-file safety, rebuildable previews, structured failures, and
  explicit unsupported states visible in task instructions.
- Describe a feature as available only when it is exposed through the current
  Studio or CLI path.
- Record platform differences and untested release conditions instead of
  silently generalizing one host's result to every platform.

The durable engineering boundaries remain in the repository's architecture,
migration, and testing documents; this handbook translates those boundaries
into user decisions and recovery steps.
