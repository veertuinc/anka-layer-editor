# anka-layer-editor

`anka-layer-editor` is a standalone terminal (TUI) tool for browsing and editing an Anka
filesystem-layer archive (`.tar.gz`) before you import/push it. It lets you:

- Navigate the archive hierarchy, expanding and collapsing directories.
- Mark files or whole subtrees for removal from the archive.
- Add files from the host machine at a chosen path inside the archive.
- Save the edited archive back in place (atomically).

It is a **completely separate product**. It does not link against, include, or modify
any Anka code. It only reads and writes a standard gzip tar, so Anka is involved solely
at the edges (you run those steps yourself).

## Where it fits in the Anka workflow

```bash
# 1. Export a filesystem layer from your modified template (Anka)
anka export --layer mytemplate /path/to/layer.tar.gz

# 2. Curate the layer interactively (this tool)
anka-layer-editor /path/to/layer.tar.gz

# 3. Restore the template and re-apply the edited layer (Anka)
anka pull --local mytemplate --tag vanilla --shrink
anka import --vmid mytemplate /path/to/layer.tar.gz

# 4. Push when satisfied (Anka)
anka push --layer -t mylayer mytemplate
```

## Build

Requires a C toolchain, `zlib`, and `ncurses` (all present by default on macOS and most
Linux distributions).

```bash
make
```

This produces the `anka-layer-editor` binary. To install:

```bash
sudo make install            # installs to /usr/local/bin
# or
make install PREFIX=~/.local
```

## Usage

```bash
anka-layer-editor <archive.tar.gz>
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
| `s` | Save the archive in place |
| `q` | Quit (asks to confirm if there are unsaved changes) |
| `?` | Toggle the help overlay |

Marked-for-removal rows are dimmed; staged additions are prefixed with `[+]`.

## How it works

- On open, the archive is scanned once and its entries are inserted into an in-memory
  path tree (directories are inferred from entry paths).
- Removal is tracked as a flag on tree nodes; a node is excluded on save if it or any of
  its ancestors is marked.
- Additions reference the host file by path and are read at save time.
- On save, the original archive is streamed entry-by-entry into a temporary file (kept
  entries copied, removed entries skipped), staged additions are appended, and the temp
  file is atomically renamed over the original. The original is left untouched unless the
  new archive is written successfully.

## Design notes

- Written in C using only system `zlib` (gzip) and `ncurses` (TUI).
- The tar reader/writer emits POSIX ustar headers with pax extension records for long
  paths / large sizes, matching what Anka's importer accepts.
- No persisted edit state: closing without saving discards changes.
