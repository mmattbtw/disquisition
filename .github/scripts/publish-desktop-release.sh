#!/usr/bin/env bash
set -euo pipefail

tag="main-${GITHUB_RUN_NUMBER}"
base_url="${GITHUB_SERVER_URL}/${GITHUB_REPOSITORY}"
assets=(
  release-assets/disquisition-desktop-macos-arm64.dmg
  release-assets/disquisition-desktop-windows-x64.zip
  release-assets/disquisition-desktop-linux-x64.tar.gz
)
for asset in "${assets[@]}"; do
  if [[ ! -s "$asset" ]]; then
    echo "Missing desktop archive: $asset" >&2
    exit 1
  fi
done

temp_dir=$(mktemp -d)
trap 'rm -rf "$temp_dir"' EXIT

# Only a 404 means there is no release. Authentication and server errors fail
# the job instead of being mistaken for an empty release history.
get_release() {
  if gh api "repos/${GITHUB_REPOSITORY}/releases/$1" > "$2"; then
    return 0
  fi
  jq -e '.status == "404"' "$2" > /dev/null
}

cat > "$temp_dir/notes.md" <<EOF
Desktop builds from [commit ${GITHUB_SHA:0:7}](${base_url}/commit/${GITHUB_SHA}) pushed to \`main\`.

| Platform | Download |
| --- | --- |
| macOS, Apple silicon | [Download DMG](${base_url}/releases/download/${tag}/disquisition-desktop-macos-arm64.dmg) |
| Windows, x64 | [Download ZIP](${base_url}/releases/download/${tag}/disquisition-desktop-windows-x64.zip) |
| Linux, x64 | [Download tar.gz](${base_url}/releases/download/${tag}/disquisition-desktop-linux-x64.tar.gz) |

The downloads bundle baresip. macOS and Windows also bundle Qt. Linux requires a compatible Qt 6.8 runtime.

Persistent links to the newest successful \`main\` build:

- [macOS, Apple silicon](${base_url}/releases/latest/download/disquisition-desktop-macos-arm64.dmg)
- [Windows, x64](${base_url}/releases/latest/download/disquisition-desktop-windows-x64.zip)
- [Linux, x64](${base_url}/releases/latest/download/disquisition-desktop-linux-x64.tar.gz)

[Build workflow](${base_url}/actions/runs/${GITHUB_RUN_ID})
EOF

get_release "tags/$tag" "$temp_dir/release.json"
if [[ $(jq -r '.status // ""' "$temp_dir/release.json") == 404 ]]; then
  gh release create "$tag" --target "$GITHUB_SHA" --draft --latest=false \
    --title "Desktop main #${GITHUB_RUN_NUMBER} (${GITHUB_SHA:0:7})" \
    --notes-file "$temp_dir/notes.md"
elif [[ $(jq -r '.draft' "$temp_dir/release.json") == false ]]; then
  # A rerun must not replace published assets or promote an older build.
  echo "Release $tag is already published."
  exit 0
fi

# A failed upload leaves a draft. Reruns can finish it before publication.
gh release upload "$tag" "${assets[@]}" --clobber

get_release latest "$temp_dir/latest.json"
latest_tag=$(jq -r '.tag_name // ""' "$temp_dir/latest.json")
make_latest=true
if [[ "$latest_tag" =~ ^main-([0-9]+)$ ]] &&
   (( BASH_REMATCH[1] > GITHUB_RUN_NUMBER )); then
  make_latest=false
fi

# The workflow serializes this job. An older push that builds more slowly still
# gets a release, but cannot roll the persistent download links backward.
gh release edit "$tag" --draft=false --latest="$make_latest"
cat "$temp_dir/notes.md" >> "$GITHUB_STEP_SUMMARY"
