#!/bin/bash
set -euo pipefail

# Everything else goes through the create retry.
if [[ ${1:-} != detach ]]; then
    exec "$(dirname "$0")/hdiutil-with-retry.sh" "$@"
fi

# Finder can keep the image busy briefly after CPack lays out its window.
# Retry EBUSY only, without forcing an unmount or hiding permanent errors.
for attempt in 1 2 3 4 5; do
    status=0
    hdiutil "$@" || status=$?
    if [[ $status != 16 || $attempt == 5 ]]; then
        exit "$status"
    fi
    echo "Disk image is busy; retrying detach ($attempt/5)." >&2
    sleep 2
done
