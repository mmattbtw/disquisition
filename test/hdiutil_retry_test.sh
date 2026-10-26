#!/bin/bash
set -euo pipefail

script_dir=$(cd "$(dirname "$0")" && pwd)
wrapper="$script_dir/../scripts/hdiutil-retry.sh"
staging=$(mktemp -d "${TMPDIR:-/tmp}/disquisition-hdiutil-test.XXXXXX")
trap 'rm -rf "$staging"' EXIT
export PATH="$staging:$PATH"
export HDIUTIL_TEST_STATE="$staging/attempts"

# Stub only the system command and clock. A busy detach returns EBUSY (16).
cat > "$staging/hdiutil" <<'EOF'
#!/bin/bash
set -euo pipefail
[[ $# == 2 && "$2" == '/Volumes/disquisition 1' ]] || exit 99
attempt=$(cat "$HDIUTIL_TEST_STATE")
attempt=$((attempt + 1))
echo "$attempt" > "$HDIUTIL_TEST_STATE"
echo "disk image output"
if [[ $attempt -le $HDIUTIL_TEST_FAILURES ]]; then
    echo "disk image error" >&2
    exit "$HDIUTIL_TEST_STATUS"
fi
EOF
printf '#!/bin/bash\nexit 0\n' > "$staging/sleep"
chmod +x "$staging/hdiutil" "$staging/sleep"

check() {
    local name=$1 verb=$2 failures=$3 status=$4 expected_status=$5 expected_attempts=$6
    echo 0 > "$HDIUTIL_TEST_STATE"
    export HDIUTIL_TEST_FAILURES=$failures HDIUTIL_TEST_STATUS=$status
    local actual_status=0
    bash "$wrapper" "$verb" '/Volumes/disquisition 1' > "$staging/stdout" 2> "$staging/stderr" || actual_status=$?
    if [[ $actual_status != "$expected_status" || $(cat "$HDIUTIL_TEST_STATE") != "$expected_attempts" ]]; then
        echo "FAIL: $name (status=$actual_status, attempts=$(cat "$HDIUTIL_TEST_STATE"))" >&2
        cat "$staging/stderr" >&2
        exit 1
    fi
    grep -q '^disk image output$' "$staging/stdout"
    if [[ $failures -gt 0 ]]; then
        grep -q '^disk image error$' "$staging/stderr"
    fi
    echo "PASS: $name"
}

check 'detach succeeds immediately' detach 0 16 0 1
check 'busy detach recovers' detach 2 16 0 3
check 'busy detach stops after five attempts' detach 10 16 16 5
check 'other detach errors propagate immediately' detach 10 1 1 1
check 'create is never retried' create 10 16 16 1
check 'attach output passes through' attach 0 16 0 1
[[ $(cat "$staging/stdout") == 'disk image output' ]]
