#!/bin/sh
# Build tiny Homebrew-style layer fixtures for multi-archive TUI testing.
# Mirrors the usr/local/{bin,etc,.brew,share} shape of ~/latest-310-* layers.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
STAGE="$ROOT/.stage"

rm -rf "$STAGE"
mkdir -p "$STAGE"

write() {
    path=$1
    body=$2
    mkdir -p "$STAGE/$(dirname "$path")"
    printf '%s\n' "$body" >"$STAGE/$path"
}

# Copy listed files from STAGE into a clean tree and pack as usr/local/...
pack() {
    name=$1
    shift
    tree="$STAGE/pack-$name"
    rm -rf "$tree"
    for f in "$@"; do
        mkdir -p "$tree/$(dirname "$f")"
        cp "$STAGE/$f" "$tree/$f"
    done
    (cd "$tree" && tar czf "$ROOT/$name" usr)
    rm -rf "$tree"
    echo "wrote $ROOT/$name ($(tar tzf "$ROOT/$name" | wc -l | tr -d ' ') members)"
}

# ---- unique members ----
write usr/local/bin/cmake                 "cmake-binary"
write usr/local/bin/ctest                 "ctest-binary"
write usr/local/bin/wg                    "wg-binary"
write usr/local/bin/wg-quick              "wg-quick-binary"
write usr/local/bin/jq                    "jq-binary"

write usr/local/.brew/cmake.rb            "brew formula: cmake"
write usr/local/.brew/wireguard-tools.rb  "brew formula: wireguard-tools"
write usr/local/.brew/jq.rb               "brew formula: jq"

write usr/local/etc/bash_completion.d/cmake "complete cmake"
write usr/local/etc/bash_completion.d/wg    "complete wg"
write usr/local/etc/bash_completion.d/jq    "complete jq"

write usr/local/share/doc/cmake/help.txt  "cmake help"
write usr/local/share/man/man8/wg.8       ".TH wg 8"
write usr/local/share/man/man1/jq.1       ".TH jq 1"

write usr/local/sbom.spdx.json            '{"name":"cmake"}'
write usr/local/COPYING                   "wireguard license text"
write usr/local/CHANGELOG.md              "jq changelog"

# ---- conflicting members (same guest path, different bodies) ----
# Staged under distinct names, then copied onto the shared path per pack.
write usr/local/README.md.cmake \
    "CMake layer readme"
write usr/local/README.md.jq \
    "jq layer readme - intentionally longer for size mismatch"
write usr/local/etc/bash_completion.d/common.cmake \
    "common completion from cmake"
write usr/local/etc/bash_completion.d/common.jq \
    "common completion from jq - longer body so sizes disagree"

# pkg-cmake: like ~/latest-310-cmake:*.tar.gz
cp "$STAGE/usr/local/README.md.cmake" "$STAGE/usr/local/README.md"
cp "$STAGE/usr/local/etc/bash_completion.d/common.cmake" \
   "$STAGE/usr/local/etc/bash_completion.d/common"
pack pkg-cmake.tar.gz \
    usr/local/bin/cmake \
    usr/local/bin/ctest \
    usr/local/.brew/cmake.rb \
    usr/local/etc/bash_completion.d/cmake \
    usr/local/etc/bash_completion.d/common \
    usr/local/share/doc/cmake/help.txt \
    usr/local/README.md \
    usr/local/sbom.spdx.json

# pkg-wireguard: like ~/latest-310-wireguard-tools:*.tar.gz
# Intentionally no shared file paths with cmake (dirs only).
pack pkg-wireguard.tar.gz \
    usr/local/bin/wg \
    usr/local/bin/wg-quick \
    usr/local/.brew/wireguard-tools.rb \
    usr/local/etc/bash_completion.d/wg \
    usr/local/share/man/man8/wg.8 \
    usr/local/COPYING

# pkg-jq: shares README.md and etc/.../common with cmake (yellow + '!').
cp "$STAGE/usr/local/README.md.jq" "$STAGE/usr/local/README.md"
cp "$STAGE/usr/local/etc/bash_completion.d/common.jq" \
   "$STAGE/usr/local/etc/bash_completion.d/common"
pack pkg-jq.tar.gz \
    usr/local/bin/jq \
    usr/local/.brew/jq.rb \
    usr/local/etc/bash_completion.d/jq \
    usr/local/etc/bash_completion.d/common \
    usr/local/share/man/man1/jq.1 \
    usr/local/README.md \
    usr/local/CHANGELOG.md

rm -rf "$STAGE"

cat <<'EOF'

Scenarios:
  # dirs overlap in the tree, no shared files -> no yellow
  ./anka-oci-tag-editor testdata/pkg-cmake.tar.gz testdata/pkg-wireguard.tar.gz

  # shared files README.md + etc/.../common -> yellow on those + parents
  ./anka-oci-tag-editor testdata/pkg-cmake.tar.gz testdata/pkg-jq.tar.gz

  # three layers (cmake/jq share files; wireguard unique)
  ./anka-oci-tag-editor testdata/pkg-cmake.tar.gz testdata/pkg-wireguard.tar.gz testdata/pkg-jq.tar.gz
EOF
