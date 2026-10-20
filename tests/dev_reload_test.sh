#!/bin/bash
# Check `scripts/dev.sh` in a tmux pane: arrow keys reach the editor, and they
# still reach it after a source change restarts the editor.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
session=oci-tag-editor-dev-test

if ! command -v tmux >/dev/null; then
    echo "dev_reload_test: tmux is not installed" >&2
    exit 1
fi

cleanup() {
    tmux kill-session -t "$session" 2>/dev/null || true
}
trap cleanup EXIT

fail() {
    echo "dev_reload_test: FAIL: $1" >&2
    tmux capture-pane -p -t "$session" >&2 2>/dev/null || true
    exit 1
}

pane_has() {
    tmux capture-pane -p -t "$session" | grep -q -- "$1"
}

wait_for_pane() {
    local needle=$1 attempt=0
    while [ "$attempt" -lt 100 ]; do
        pane_has "$needle" && return 0
        attempt=$((attempt + 1))
        sleep 0.1
    done
    return 1
}

# Right expands usr/, Down selects usr/local. The details line then starts
# with the selected path.
expect_arrow_keys_move_selection() {
    local when=$1
    tmux send-keys -t "$session" Right
    wait_for_pane "v usr/" || fail "Right did not expand usr/ ($when)"
    tmux send-keys -t "$session" Down
    wait_for_pane "^ usr/local " || fail "Down did not select usr/local ($when)"
}

editor_pid() {
    pgrep -f "$root/anka-oci-tag-editor testdata/pkg-cmake.tar.gz" | head -1
}

expect_pid_stable() {
    local when=$1 pid
    pid=$(editor_pid)
    [ -n "$pid" ] || fail "the editor is not running ($when)"
    sleep 2
    local later
    later=$(editor_pid)
    [ "$pid" = "$later" ] || fail "the editor restarted without a source change ($when, pid $pid -> $later)"
}

cleanup
tmux -f /dev/null new-session -d -s "$session" -x 100 -y 24 \
    "./scripts/dev.sh testdata/pkg-cmake.tar.gz"
wait_for_pane "anka-oci-tag-editor - pkg-cmake.tar.gz" || fail "the editor did not start"
expect_arrow_keys_move_selection "first start"
expect_pid_stable "after first start"

# GNU Make 3.81 compares timestamps in whole seconds. find -newer does not.
# A source file that is newer than the binary by less than one second must
# not restart the editor in a loop.
python3 - "$root" << 'PY'
import os, sys, time
root = sys.argv[1]
binary = os.path.join(root, "anka-oci-tag-editor")
source = os.path.join(root, "src", "main.c")
now = time.time()
os.utime(binary, (now, now))
os.utime(source, (now + 0.7, now + 0.7))
PY
expect_pid_stable "after a subsecond source mtime"

touch src/main.c
wait_for_pane "> usr/" || fail "the editor did not restart after a source change"
expect_arrow_keys_move_selection "after restart"
expect_pid_stable "after restart"

echo "dev_reload_test: ok"
