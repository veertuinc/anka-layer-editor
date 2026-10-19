#!/bin/bash
# Drive the editor in a fixed tmux pane and write the README PNGs.
# Needs tmux and python3. Does not need Anka: VM mode uses tests/fake-anka.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
session=oci-tag-editor-shots
out=$root/screenshots
mkdir -p "$out"

if ! command -v tmux >/dev/null; then
    echo "screenshots: tmux is not installed" >&2
    exit 1
fi

cleanup() {
    tmux kill-session -t "$session" 2>/dev/null || true
}
trap cleanup EXIT

wait_for_text() {
    needle=$1
    i=0
    while [ "$i" -lt 50 ]; do
        if ! tmux has-session -t "$session" 2>/dev/null; then
            echo "screenshots: the editor session exited before '$needle'" >&2
            return 1
        fi
        if tmux capture-pane -p -t "$session" | grep -q -- "$needle"; then
            return 0
        fi
        i=$((i + 1))
        sleep 0.1
    done
    echo "screenshots: timed out waiting for '$needle'" >&2
    tmux capture-pane -p -t "$session" >&2 || true
    return 1
}

send_keys() {
    key=$1
    tmux send-keys -t "$session" "$key"
    sleep 0.05
}

shot() {
    dest=$1
    sleep 0.15
    tmux capture-pane -e -p -N -t "$session" | python3 "$root/scripts/ansi_to_png.py" "$dest"
    echo "screenshots: wrote $dest"
}

start_editor() {
    cleanup
    tmux -f /dev/null new-session -d -s "$session" -x 100 -y 32 "$@"
}

make build

start_editor "$root/anka-oci-tag-editor" \
    "$root/testdata/pkg-cmake.tar.gz" "$root/testdata/pkg-jq.tar.gz"
wait_for_text "anka-oci-tag-editor"
send_keys l
send_keys l
send_keys l
shot "$out/tree.png"

start_editor "$root/anka-oci-tag-editor" \
    "$root/testdata/pkg-cmake.tar.gz" "$root/testdata/pkg-jq.tar.gz"
wait_for_text "anka-oci-tag-editor"
for key in l l l j j j j j j; do
    send_keys "$key"
done
send_keys " "
shot "$out/marked.png"

start_editor "$root/anka-oci-tag-editor" \
    "$root/testdata/pkg-cmake.tar.gz" "$root/testdata/pkg-jq.tar.gz"
wait_for_text "anka-oci-tag-editor"
send_keys "?"
shot "$out/help.png"

fake_state=$(mktemp -d "${TMPDIR:-/tmp}/oci-tag-editor-shots.XXXXXX")
printf 'stopped\n' >"$fake_state/status"
printf 'base\n' >"$fake_state/current_tag"
printf 'base\n' >"$fake_state/tags"
touch "$fake_state/working.img" "$fake_state/tag.ank"
# One shell command, so tmux does not treat "--vm" as its own option.
cleanup
tmux -f /dev/null new-session -d -s "$session" -x 100 -y 32 \
    "FAKE_ANKA_DIR=$(printf '%q' "$fake_state") FAKE_ANKA_LAYER=$(printf '%q' "$root/testdata/pkg-cmake.tar.gz") PATH=$(printf '%q' "$root/tests/fake-anka:$PATH") $(printf '%q' "$root/anka-oci-tag-editor") --vm demo"
wait_for_text "OCI tag"
send_keys l
send_keys l
send_keys l
shot "$out/vm.png"
rm -rf "$fake_state"
