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

type_text() {
    tmux send-keys -t "$session" -l "$1"
    sleep 0.05
}

clear_prompt() {
    tmux send-keys -t "$session" C-u
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

start_sample() {
    start_editor "$root/anka-oci-tag-editor" \
        "$root/testdata/pkg-cmake.tar.gz" "$root/testdata/pkg-jq.tar.gz"
    wait_for_text "anka-oci-tag-editor"
}

expand_local() {
    send_keys l
    send_keys l
    send_keys l
}

make build

start_sample
expand_local
shot "$out/tree.png"

start_sample
send_keys "/"
wait_for_text "Filter (empty clears)"
type_text "cmake"
send_keys Enter
wait_for_text 'filter: "cmake"'
send_keys G
shot "$out/filter.png"

start_sample
for key in l l l j j j j j j; do
    send_keys "$key"
done
send_keys "x"
send_keys "1"
shot "$out/marked.png"

start_sample
send_keys w
wait_for_text "Whiteout - path"
clear_prompt
type_text "usr/local/bin/oldtool"
send_keys Enter
wait_for_text "Whiteout in which archive"
send_keys "1"
wait_for_text "staged whiteout"
send_keys j
shot "$out/whiteout.png"

start_sample
expand_local
send_keys a
wait_for_text "Add - host file"
type_text "$root/testdata/README.md"
send_keys Enter
wait_for_text "Add - destination"
clear_prompt
type_text "usr/local/NOTES.md"
send_keys Enter
wait_for_text "Add to which archive"
send_keys "1"
wait_for_text "Add - owner"
send_keys Enter
wait_for_text "staged in"
shot "$out/staged.png"

start_sample
send_keys "?"
shot "$out/help.png"

start_sample
send_keys "/"
wait_for_text "Filter (empty clears)"
type_text "README.md"
send_keys Enter
wait_for_text "README.md"
send_keys j
send_keys j
send_keys Enter
wait_for_text "Preview which archive"
send_keys "1"
wait_for_text "q close"
shot "$out/preview.png"

start_sample
send_keys "/"
wait_for_text "Filter (empty clears)"
type_text "README.md"
send_keys Enter
wait_for_text "README.md"
send_keys j
send_keys j
send_keys d
wait_for_text "q close"
shot "$out/diff.png"

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
