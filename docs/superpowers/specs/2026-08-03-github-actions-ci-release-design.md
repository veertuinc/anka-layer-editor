# GitHub Actions CI and Release Design

## Goal

Add GitHub Actions so every change to `main` is built and tested on macOS and Linux, and a manual workflow can cut a versioned GitHub Release with darwin-arm64 and linux-amd64 binaries.

## Constraints (from product decisions)

- Platforms: macOS + Linux for CI; release binaries for `darwin-arm64` and `linux-amd64` only.
- Release trigger: manual `workflow_dispatch` with a `version` input (operator sets the version).
- Release creates the git tag and GitHub Release, then uploads binaries as release assets.
- Action artifacts used to pass binaries between jobs expire after 1 day; release assets on the GitHub Release persist.
- CI runs on push/PR to `main`, separate from the release workflow.
- Use `macos-latest` (not a pinned `macos-14`) and `ubuntu-latest`.

## Approach

Two workflows (Approach 1):

| File | Purpose |
|------|---------|
| `.github/workflows/ci.yml` | Build + test on push/PR to `main` |
| `.github/workflows/release.yml` | Manual release: build, tag, publish |

No reusable workflow. The project is a small Make-based C binary; duplication of a few build steps is acceptable.

## CI workflow

**Triggers:** `push` and `pull_request` targeting `main`.

**Matrix:**

| Runner | Role |
|--------|------|
| `macos-latest` | Apple Silicon host (darwin-arm64) |
| `ubuntu-latest` | Linux amd64 |

**Steps (each matrix leg):**

1. Checkout
2. Install build deps on Ubuntu (`libncurses-dev`, `zlib1g-dev`). macOS uses system zlib/ncurses.
3. Run `make` (builds the binary and runs headless round-trip tests).
4. Fail the job if build or tests fail.

No artifacts are uploaded from CI.

## Release workflow

**Trigger:** `workflow_dispatch` with input:

| Input | Description |
|-------|-------------|
| `version` | `MAJOR.MINOR.PATCH` without a leading `v`, e.g. `1.2.3`. A leading `v` is stripped if present. Workflow creates tag `v1.2.3`. |

**Permissions:** `contents: write` (create tag + release via `GITHUB_TOKEN`).

### Jobs

1. **validate**
   - Normalize version: strip a leading `v` if present so `1.2.3` and `v1.2.3` both become tag `v1.2.3`.
   - Require `MAJOR.MINOR.PATCH` where each part is digits only (e.g. `1.2.3`). Reject anything else.
   - Fail if tag `vX.Y.Z` already exists on the remote.

2. **build** (matrix, needs validate)
   - Same runners as CI: `macos-latest`, `ubuntu-latest`.
   - Install deps, run `make build test`.
   - Rename binary:
     - macOS → `anka-layer-editor-darwin-arm64`
     - Linux → `anka-layer-editor-linux-amd64`
   - `actions/upload-artifact` with `retention-days: 1`.

3. **release** (needs build)
   - Download both artifacts.
   - Create annotated tag `vX.Y.Z` on the commit that ran the workflow.
   - Create a GitHub Release with `gh release create`, attaching both binaries as release assets (permanent on the release).
   - Release title: `vX.Y.Z`. Body can be empty or a one-line note; no changelog generation.

### Error handling

- Failed build or tests on either OS abort the release; no tag or Release is created.
- Duplicate tag fails in validate before builds waste cycles (best-effort; race between validate and release is rare and would fail at tag create).
- If tag creation succeeds but asset upload fails, the workflow fails; operator can delete the incomplete release/tag and re-run, or attach assets manually.

## Out of scope

- Homebrew formula / package manager publishing
- Windows builds
- Cross-compile for additional arches (`darwin-amd64`, `linux-arm64`)
- Changelog generation / release notes beyond a simple title/body
- Signing or notarization of macOS binaries
- Publishing Action artifacts as the primary distribution channel

## Success criteria

- A PR or push to `main` runs CI on macOS and Linux; both must pass `make`.
- Running the release workflow with version `1.0.0` creates tag `v1.0.0`, a GitHub Release, and two downloadable binaries named as above.
- Intermediate Action artifacts from the release workflow expire after one day.
