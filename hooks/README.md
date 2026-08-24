# Ravo Git hooks

This directory is the host installer for the shared FreeCM commit hooks. Hook
behavior lives in [`FreeCM/hooks/README.md`](../FreeCM/hooks/README.md). Do not
copy or edit FreeCM hook implementations here.

## Install

1. Initialize the FreeCM submodule if it is not already present:

```bash
git submodule update --init FreeCM
```

2. Create a local config from the sample and fill real tool paths:

```bash
cp hooks/path.ini.sample hooks/path.ini
```

`CLANG_FORMAT_PATH` must point to an executable `clang-format`.
`QMLFORMAT_PATH` is optional; leave it empty to skip QML/JS formatting.
`SOURCE_ROOTS=Ravo` keeps clang-format and qmlformat on first-party sources.
Frozen `legacy/` and the `FreeCM/` submodule stay outside that root.

3. Install into this repository's Git hooks directory:

```bash
python3 hooks/install.py
```

The installer uses Git's effective `core.hooksPath`. It never overwrites a
different existing hook by default:

```bash
python3 hooks/install.py --existing backup
python3 hooks/install.py --existing replace
```

`hooks/path.ini` is machine-local and must not be committed.

## What commits do

- Format staged C/C++ blobs under `Ravo/` with clang-format.
- Format staged QML/JS blobs under `Ravo/` when qmlformat is configured.
- Normalize staged text blobs to LF and strip trailing whitespace.
- Block staged blobs larger than 15MB.
- Require commit subjects of the form `[type]: description`.

Valid types: `feat`, `fix`, `refactor`, `style`, `docs`, `test`, `chore`,
`perf`, `ci`, `build`, `enhancement`.
