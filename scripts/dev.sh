#!/bin/bash
# Run the editor and restart it when a file under src/ changes.
# Quit with q or Ctrl-C. A failed build leaves the running editor in place.
set -euo pipefail

# Background jobs in a non-interactive script get stdin from /dev/null unless
# stdin is redirected explicitly; keep the terminal on fd 3 for the editor child.
exec 3<&0

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
bin=$root/anka-oci-tag-editor
reload_status=75
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/oci-tag-editor-dev.XXXXXX")
pidfile=$tmpdir/pid
logfile=$tmpdir/build.log
failfile=$tmpdir/failed
stampfile=$tmpdir/build-stamp
child=0
watcher=0

cleanup() {
    if [ "$child" != 0 ]; then
        kill -TERM "$child" 2>/dev/null || true
    fi
    if [ "$watcher" != 0 ]; then
        kill "$watcher" 2>/dev/null || true
    fi
    rm -rf "$tmpdir"
}
trap 'exit 130' INT
trap 'exit 143' TERM
trap cleanup EXIT

source_changed_since() {
    local newer
    newer=$(find src \( -name '*.c' -o -name '*.h' \) -newer "$1" -print)
    [ -n "$newer" ]
}

source_changed_at_least_one_second_ahead_of_binary() {
    [ -f "$bin" ] || return 0
    python3 - "$bin" "$stampfile" "$root" <<'PY'
import os
import sys

binary_path, stamp_path, repository_root = sys.argv[1:4]
binary_mtime = os.path.getmtime(binary_path)
stamp_mtime = os.path.getmtime(stamp_path)
source_root = os.path.join(repository_root, "src")
for directory_path, _directory_names, file_names in os.walk(source_root):
    for file_name in file_names:
        if not (file_name.endswith(".c") or file_name.endswith(".h")):
            continue
        source_path = os.path.join(directory_path, file_name)
        source_mtime = os.path.getmtime(source_path)
        if source_mtime <= stamp_mtime:
            continue
        if source_mtime - binary_mtime >= 1.0:
            raise SystemExit(0)
raise SystemExit(1)
PY
}

sync_build_stamp_to_sources() {
    local path
    touch "$stampfile"
    while IFS= read -r -d '' path; do
        if [ "$path" -nt "$stampfile" ]; then
            touch -r "$path" "$stampfile"
        fi
    done < <(find src \( -name '*.c' -o -name '*.h' \) -print0)
}

watch_sources() {
    while true; do
        local running
        running=$(cat "$pidfile" 2>/dev/null || echo 0)
        if [ "$running" != 0 ] && source_changed_since "$stampfile"; then
            if [ -f "$failfile" ] && ! source_changed_since "$failfile"; then
                sleep 0.4
                continue
            fi
            if ! source_changed_at_least_one_second_ahead_of_binary; then
                sync_build_stamp_to_sources
                sleep 0.4
                continue
            fi
            echo "==> src changed, rebuilding" >&2
            # Force relink: Make 3.81 may skip when mtime shares a whole second.
            rm -f "$bin"
            if make build >"$logfile" 2>&1; then
                rm -f "$failfile"
                # Stamp before SIGTERM so find does not retrigger on subsecond mtimes.
                sync_build_stamp_to_sources
                kill -TERM "$running" 2>/dev/null || true
            else
                touch "$failfile"
                cat "$logfile" >&2
                echo "==> build failed; the editor is still running" >&2
            fi
        fi
        sleep 0.4
    done
}

make build
sync_build_stamp_to_sources

: >"$pidfile"
watch_sources &
watcher=$!

while true; do
    "$bin" "$@" <&3 3<&- &
    child=$!
    echo "$child" >"$pidfile"
    set +e
    wait "$child"
    status=$?
    set -e
    child=0
    echo 0 >"$pidfile"
    stty sane 2>/dev/null || true
    if [ "$status" -eq "$reload_status" ]; then
        echo "==> restarted" >&2
        continue
    fi
    exit "$status"
done
