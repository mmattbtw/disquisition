#!/bin/bash
set -euo pipefail

# CPack also uses attach's plist output and detach. Pass those through directly.
if [[ ${1:-} != create ]]; then
    exec hdiutil "$@"
fi

log=$(mktemp "${TMPDIR:-/tmp}/disquisition-hdiutil.XXXXXX")
trap 'rm "$log"' EXIT
for attempt in 1 2 3 4 5; do
    if hdiutil "$@" 2> "$log"; then
        cat "$log" >&2
        exit 0
    else
        result=$?
    fi
    cat "$log" >&2
    # macOS runners can temporarily hold the image's files during creation.
    # CPack supplies -ov, so retrying safely replaces any partial image.
    if [[ $attempt == 5 ]] || ! grep -Fq 'hdiutil: create failed - Resource busy' "$log"; then
        exit "$result"
    fi
    echo "Disk image creation is busy; retrying after $((attempt * 2)) seconds (attempt $attempt/5)." >&2
    sleep "$((attempt * 2))"
done
