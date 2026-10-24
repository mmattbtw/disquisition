#!/bin/bash
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
fixture=$(mktemp -d)
trap 'rm -rf "$fixture"' EXIT
mkdir "$fixture/bin"
export PATH="$fixture/bin:$PATH"
export TEST_STATE="$fixture"

# Replay the actual CI diagnostic at the process boundary. Keep stdout separate
# from stderr because CPack parses hdiutil attach's plist output.
cat > "$fixture/bin/hdiutil" <<'EOF'
#!/bin/bash
set -euo pipefail
attempt=$(cat "$TEST_STATE/count")
echo $((attempt + 1)) > "$TEST_STATE/count"
printf '%s\n' "$@" > "$TEST_STATE/args"
if [[ $attempt -lt $TEST_FAILURES ]]; then
    echo "$TEST_ERROR" >&2
    exit "$TEST_EXIT"
fi
echo '<plist>success</plist>'
echo 'hdiutil diagnostic' >&2
EOF
cat > "$fixture/bin/sleep" <<'EOF'
#!/bin/bash
echo "$1" >> "$TEST_STATE/delays"
EOF
chmod +x "$fixture/bin/hdiutil" "$fixture/bin/sleep"

expect_equal() {
    if [[ $1 != "$2" ]]; then
        echo "Expected '$2', got '$1'" >&2
        exit 1
    fi
}

run_case() {
    local failures=$1 error=$2 exit_code=$3 verb=$4 expected_status=$5 expected_attempts=$6
    export TEST_FAILURES=$failures TEST_ERROR=$error TEST_EXIT=$exit_code
    echo 0 > "$fixture/count"
    : > "$fixture/delays"
    local status=0
    bash "$root/scripts/hdiutil-with-retry.sh" "$verb" -volname "disquisition staging" \
        > "$fixture/stdout" 2> "$fixture/stderr" || status=$?
    expect_equal "$status" "$expected_status"
    expect_equal "$(cat "$fixture/count")" "$expected_attempts"
    printf '%s\n' "$verb" -volname "disquisition staging" > "$fixture/expected-args"
    diff -u "$fixture/expected-args" "$fixture/args"
    if [[ $status == 0 ]]; then
        expect_equal "$(cat "$fixture/stdout")" '<plist>success</plist>'
        grep -q 'hdiutil diagnostic' "$fixture/stderr"
    else
        grep -Fq "$error" "$fixture/stderr"
    fi
}

busy='hdiutil: create failed - Resource busy'
run_case 0 "$busy" 1 create 0 1
expect_equal "$(cat "$fixture/delays")" ''
run_case 2 "$busy" 1 create 0 3
expect_equal "$(cat "$fixture/delays")" $'2\n4'
run_case 10 "$busy" 1 create 1 5
expect_equal "$(cat "$fixture/delays")" $'2\n4\n6\n8'
# Permanent errors, and verbs whose output CPack parses, must pass through once.
run_case 10 'hdiutil: create failed - Permission denied' 13 create 13 1
expect_equal "$(cat "$fixture/delays")" ''
run_case 10 "$busy" 16 attach 16 1
expect_equal "$(cat "$fixture/delays")" ''
run_case 0 "$busy" 1 attach 0 1
echo 'macOS DMG retry checks passed'
