# anka-layer-editor

[![CI](https://github.com/veertuinc/anka-layer-editor/actions/workflows/ci.yml/badge.svg?branch=edge)](https://github.com/veertuinc/anka-layer-editor/actions/workflows/ci.yml)

`anka-layer-editor` is a standalone terminal (TUI) tool for browsing and editing Anka
filesystem-layer archives (`.tar.gz`) before you import/push them. It lets you:

- Navigate the archive hierarchy, expanding and collapsing directories.
- Mark files or whole subtrees for removal from the archive.
- Add files from the host machine at a chosen path inside the archive.
- Save the edited archive back in place (atomically).
- Open several layers at once (up to 8) and see which of them hold each path.

It is a **completely separate product**. It does not link against, include, or modify
any Anka code. It only reads and writes a standard gzip tar, so Anka is involved solely
at the edges (you run those steps yourself).

## Where it fits in the Anka workflow

```bash
# 1. Export a filesystem layer from your modified template (Anka)
anka export --layer mytemplate /path/to/layer.tar.gz

# 2. Curate the layer interactively (this tool; pass several layers to compare them)
anka-layer-editor /path/to/layer.tar.gz

# 3. Restore the template and re-apply the edited layer (Anka)
anka pull --local mytemplate --tag vanilla --shrink
anka import --vmid mytemplate /path/to/layer.tar.gz

# 4. Push when satisfied (Anka)
anka push mytemplate --tag mylayer http://zot-registry:15455/v2
```

## Build

Requires a C toolchain, `zlib`, and `ncurses` (present by default on macOS).

```bash
make               # builds the binary and runs the headless round-trip tests
make build         # binary only
make test          # tests only
```

This produces the `anka-layer-editor` binary. To install:

```bash
sudo make install            # installs to /usr/local/bin
# or
make install PREFIX=~/.local
```

## CI

Pushes and pull requests to `edge` run GitHub Actions CI on `macos-latest` and
`ubuntu-24.04-arm` (`make`: build + headless tests).

## Release

Cut a release from the Actions tab: run the **Release** workflow and set
`version` to `MAJOR.MINOR.PATCH` (for example `1.0.0`). The workflow:

1. Builds and tests on macOS and Linux
2. Creates tag `vX.Y.Z` and a GitHub Release
3. Attaches `anka-layer-editor-darwin-arm64` and `anka-layer-editor-linux-arm64`

## Usage

```bash
anka-layer-editor <archive1.tar.gz> [archive2.tar.gz archive3.tar.gz ...]
```

### Keys

| Key | Action |
|-----|--------|
| Up / Down, `j` / `k` | Move selection |
| Right / `l` | Expand directory |
| Left / `h` | Collapse directory (or jump to parent) |
| `Space` | Toggle mark-for-removal on the selected file/subtree |
| `a` | Add a host file: prompts for host path, then destination path in the archive |
| `/` | Filter by substring (auto-reveals matching branches; `Esc`/empty clears) |
| `s` | Save every changed archive in place |
| `q` | Quit (asks to confirm if there are unsaved changes) |
| `?` | Toggle the help overlay |

Marked-for-removal rows are dimmed; staged additions are prefixed with `[+]`.

## Comparing several layers

Pass more than one archive to browse them together:

```bash
anka-layer-editor layer-a.tar.gz layer-b.tar.gz
```

Tiny Homebrew-style fixtures for trying this live are in [`testdata/`](testdata/)
(regenerate with `./testdata/build.sh`).

Every path appears once, and a membership column shows which archives hold it:

```
X [12]  v etc/          both archives contain etc/
  [1.]    > opt/        only the first archive
  [.2]    > var/        only the second archive
  [.+]      added.txt   staged into the second archive, not saved yet
```

Files that exist in more than one archive are yellow. Parent folders that lead to
such a file are yellow too, so you can expand down toward the overlap; folders that
merely appear in several archives without a shared file underneath stay normal. A
`!` after the column means the entries disagree about the entry's type or size.
Paths are matched by name only; file contents are never hashed or compared.

Edits stay scoped to an archive:

- `Space` on a path held by one archive marks it there directly. On a shared path it
  asks which archive to mark, or `a` for all of them.
- `a` asks which archive receives the new file, since an addition belongs to one tar.
- `s` writes every archive with unsaved changes and reports how many were saved. If one
  fails, saving stops there and the archives already written stay saved.

The header lists the loaded archives by number, with `*` marking unsaved changes.

## How it works

- On open, each archive is scanned once and its entries are inserted into a single
  in-memory path tree (directories are inferred from entry paths). A path found in
  several archives becomes one node that records which archives hold it.
- Removal and staged additions are per-archive flags on tree nodes; a node is excluded
  from an archive on save if it or any of its ancestors is marked for that archive.
- Additions reference the host file by path and are read at save time.
- On save, the original archive is streamed entry-by-entry into a temporary file (kept
  entries copied, removed entries skipped), staged additions are appended, and the temp
  file is atomically renamed over the original. The original is left untouched unless the
  new archive is written successfully. Each archive is written independently, so an
  archive you did not change is never rewritten.

## Design notes

- Written in C using only system `zlib` (gzip) and `ncurses` (TUI).
- The tar reader/writer emits POSIX ustar headers with pax extension records for long
  paths / large sizes, matching what Anka's importer accepts.
- No persisted edit state: closing without saving discards changes.
