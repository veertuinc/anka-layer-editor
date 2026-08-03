# Test layer archives

Tiny Homebrew-style `.tar.gz` fixtures that mimic `~/latest-310-*` layers
(`usr/local/{bin,etc,.brew,share}/...`) without the bulk.

Regenerate:

```bash
./testdata/build.sh
```

## Archives

| Archive | Role |
|---------|------|
| `pkg-cmake.tar.gz` | cmake-like formula (bins, completion, brew rb, README, sbom) |
| `pkg-wireguard.tar.gz` | wireguard-like formula; **no shared file paths** with cmake |
| `pkg-jq.tar.gz` | jq-like formula; shares `README.md` and `etc/bash_completion.d/common` with cmake |

## Scenarios

**No yellow** (directory scaffolding overlaps; no conflicting files):

```bash
./anka-layer-editor testdata/pkg-cmake.tar.gz testdata/pkg-wireguard.tar.gz
```

**Yellow + `!`** on shared files and the parents that lead to them
(`usr/`, `usr/local/`, `usr/local/etc/`, …):

```bash
./anka-layer-editor testdata/pkg-cmake.tar.gz testdata/pkg-jq.tar.gz
```

Shared paths:

- `usr/local/README.md` (different sizes → `!`)
- `usr/local/etc/bash_completion.d/common` (different sizes → `!`)

**Three layers** (cmake/jq conflict; wireguard stays unique):

```bash
./anka-layer-editor testdata/pkg-cmake.tar.gz testdata/pkg-wireguard.tar.gz testdata/pkg-jq.tar.gz
```
