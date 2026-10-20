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

watch_sources() {
    while true; do
        local running
        running=$(cat "$pidfile" 2>/dev/null || echo 0)
        if [ "$running" != 0 ] && source_changed_since "$bin"; then
            if [ -f "$failfile" ] && ! source_changed_since "$failfile"; then
                sleep 0.4
                continue
            fi
            echo "==> src changed, rebuilding" >&2
            if make build >"$logfile" 2>&1; then
                rm -f "$failfile"
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
