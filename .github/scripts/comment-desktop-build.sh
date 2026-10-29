#!/usr/bin/env bash
set -euo pipefail

# Leaves a single "Desktop builds" comment on each pull request. Every run
# rewrites that same comment in place instead of adding another one, and a
# slower older run cannot overwrite the links of a newer run.

if [[ -z "${GITHUB_REPOSITORY:-}" || -z "${BUILD_RUN_ID:-}" || -z "${BUILD_HEAD_SHA:-}" ]]; then
  echo "GITHUB_REPOSITORY, BUILD_RUN_ID and BUILD_HEAD_SHA are required." >&2
  exit 1
fi

marker_name='desktop-build-downloads'
base_url="${GITHUB_SERVER_URL:-https://github.com}/${GITHUB_REPOSITORY}"
run_url="${base_url}/actions/runs/${BUILD_RUN_ID}"
sha="${BUILD_HEAD_SHA:0:7}"
temp_dir=$(mktemp -d)
trap 'rm -rf "$temp_dir"' EXIT

platforms=(
  "disquisition-desktop-macos-arm64|macOS, Apple silicon|disquisition-desktop-macos-arm64.dmg"
  "disquisition-desktop-windows-x64|Windows, x64|disquisition-desktop-windows-x64.zip"
  "disquisition-desktop-linux-x64|Linux, x64|disquisition-desktop-linux-x64.tar.gz"
)

# Resolve PRs through GitHub's API, including fork PRs whose workflow_run
# payload can have an empty pull_requests array. Ignore superseded commits.
gh api --paginate "repos/${GITHUB_REPOSITORY}/commits/${BUILD_HEAD_SHA}/pulls" \
  --jq ".[] | select(.state == \"open\" and .head.sha == \"${BUILD_HEAD_SHA}\" and .base.repo.full_name == \"${GITHUB_REPOSITORY}\") | .number" \
  > "$temp_dir/prs.txt"
if [[ ! -s "$temp_dir/prs.txt" ]]; then
  echo "No open PRs still point to this build's commit."
  exit 0
fi

# Artifact IDs are what the run's artifact links are built from. A run only
# holds its own artifacts, so a rerun of a different commit cannot leak in.
gh api --paginate "repos/${GITHUB_REPOSITORY}/actions/runs/${BUILD_RUN_ID}/artifacts" \
  --jq '.artifacts[] | [.id, .name, (.expired | tostring)] | @tsv' > "$temp_dir/artifacts.tsv"

: > "$temp_dir/rows.md"
missing=0
for platform in "${platforms[@]}"; do
  IFS='|' read -r artifact label file <<< "$platform"
  line=$(awk -F '\t' -v name="$artifact" '$2 == name { print; exit }' "$temp_dir/artifacts.tsv")
  if [[ -z "$line" ]]; then
    printf '| %s | Not available |\n' "$label" >> "$temp_dir/rows.md"
    missing=$((missing + 1))
    continue
  fi
  artifact_id=$(cut -f1 <<< "$line")
  expired=$(cut -f3 <<< "$line")
  if [[ "$expired" == "true" ]]; then
    printf '| %s | Expired |\n' "$label" >> "$temp_dir/rows.md"
    missing=$((missing + 1))
  else
    printf '| %s | [%s](%s/actions/runs/%s/artifacts/%s) |\n' \
      "$label" "$file" "$base_url" "$BUILD_RUN_ID" "$artifact_id" >> "$temp_dir/rows.md"
  fi
done

{
  printf '<!-- %s run=%s -->\n' "$marker_name" "$BUILD_RUN_ID"
  printf '\n### Desktop builds\n\n'
  if [[ -n "${BUILD_RESULT:-}" && "${BUILD_RESULT}" != "success" ]]; then
    printf '> [!WARNING]\n> The desktop build did not finish successfully (%s). Only the platforms that finished are downloadable.\n\n' "$BUILD_RESULT"
  fi
  printf '| Platform | Download |\n| --- | --- |\n'
  cat "$temp_dir/rows.md"
  printf '\n'
  if (( missing > 0 )); then
    printf 'See the [workflow run](%s) for the build logs.\n\n' "$run_url"
  fi
  printf 'macOS and Windows bundle Qt. Linux requires a compatible Qt 6.8 runtime.\n\n'
  printf 'Downloads are GitHub Actions ZIP artifacts. Extract them to get the installer or archive. They require signing in to GitHub and expire with artifact retention.\n\n'
  printf '<sub>[Workflow run](%s) · commit `%s`</sub>\n' "$run_url" "$sha"
} > "$temp_dir/body.md"

while read -r PR_NUMBER; do
  # Find the comment this workflow previously left, if any. Comments are listed
  # oldest first, so the last id is the newest one. The body is fetched
  # separately because it contains newlines, which do not survive a one-line list.
  existing_id=""
  existing_run=0
  gh api --paginate "repos/${GITHUB_REPOSITORY}/issues/${PR_NUMBER}/comments" \
    --jq ".[] | select(.user.login == \"github-actions[bot]\" and .user.type == \"Bot\") | select(.body | startswith(\"<!-- ${marker_name} run=\")) | .id" \
    > "$temp_dir/existing-ids.txt"
  if [[ -s "$temp_dir/existing-ids.txt" ]]; then
    existing_id=$(tail -n1 "$temp_dir/existing-ids.txt")
    existing_body=$(gh api "repos/${GITHUB_REPOSITORY}/issues/comments/${existing_id}" --jq .body)
    existing_run=$(grep -oE 'run=[0-9]+' <<< "$existing_body" | head -n1 | cut -d= -f2 || true)
    existing_run=${existing_run:-0}
  fi

  if (( existing_run > BUILD_RUN_ID )); then
    echo "Comment already shows the newer run ${existing_run}; leaving it alone."
    continue
  fi

  if [[ -n "$existing_id" ]]; then
    gh api -X PATCH "repos/${GITHUB_REPOSITORY}/issues/comments/${existing_id}" \
      -f body="$(cat "$temp_dir/body.md")" > /dev/null
    echo "Updated comment ${existing_id} on pull request ${PR_NUMBER}."
  else
    gh api -X POST "repos/${GITHUB_REPOSITORY}/issues/${PR_NUMBER}/comments" \
      -f body="$(cat "$temp_dir/body.md")" > /dev/null
    echo "Created a comment on pull request ${PR_NUMBER}."
  fi
done < "$temp_dir/prs.txt"
