#!/bin/bash
set -euo pipefail

if [[ $# != 2 ]]; then
    echo "Usage: $0 disquisition.app output.dmg" >&2
    exit 1
fi
if [[ $(uname -s) != Darwin ]]; then
    echo "DMG packaging requires macOS." >&2
    exit 1
fi
if [[ ! -d "$1" || "$2" != *.dmg ]]; then
    echo "Provide an app bundle and a .dmg output path." >&2
    exit 1
fi

script_dir=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$(dirname "$2")"
output_dir=$(cd "$(dirname "$2")" && pwd)
package_name=$(basename "$2" .dmg)
staging=$(mktemp -d "${TMPDIR:-/tmp}/disquisition-dmg.XXXXXX")
trap 'rm -rf "$staging"' EXIT
mkdir -p "$staging/payload"
app_name=$(basename "$1")
app="$staging/payload/$app_name"
ditto "$1" "$app"

# Deploy Qt dependencies, then sign the completed bundle.
deployqt=$(command -v "${MACDEPLOYQT:-macdeployqt}")
qt_libs=$("$(dirname "$deployqt")/qmake" -query QT_INSTALL_LIBS)
# Homebrew splits Qt modules into separate prefixes. Search its shared library
# directory first so macdeployqt also resolves dependencies of copied plugins.
executable=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$app/Contents/Info.plist")
binary="$app/Contents/MacOS/$executable"
qt_dependencies=$(otool -L "$binary" | awk '/^[[:space:]]*\/.*\/Qt[^\/]*\.framework\// { sub(/^[[:space:]]*/, ""); sub(/ \(compatibility.*$/, ""); print }')
while IFS= read -r dependency; do
    [[ -z "$dependency" ]] || install_name_tool -change "$dependency" "@rpath/${dependency##*/lib/}" "$binary"
done <<< "$qt_dependencies"
rpaths=$(otool -l "$binary" | awk '/cmd LC_RPATH/ { getline; getline; sub(/^[[:space:]]*path /, ""); sub(/ \(offset.*$/, ""); print }')
rpath_args=()
while IFS= read -r rpath; do
    [[ -z "$rpath" ]] || rpath_args+=(-delete_rpath "$rpath")
done <<< "$rpaths"
if [[ ${#rpath_args[@]} -gt 0 ]]; then
    install_name_tool "${rpath_args[@]}" "$binary"
fi
rpath_args=(-add_rpath "$qt_libs")
while IFS= read -r rpath; do
    [[ -z "$rpath" || "$rpath" == "$qt_libs" ]] || rpath_args+=(-add_rpath "$rpath")
done <<< "$rpaths"
install_name_tool "${rpath_args[@]}" "$binary"
"$deployqt" "$app" "-libpath=$qt_libs" 2>&1 | tee "$staging/deployment.log"
# macdeployqt can report missing dependencies without returning a failure status.
if grep -q '^ERROR:' "$staging/deployment.log"; then
    echo "Qt deployment failed; see the errors above." >&2
    exit 1
fi
cmake "-DDISQUISITION_CODESIGN_IDENTITY=${DISQUISITION_CODESIGN_IDENTITY:-AUTO}" \
    "-DDISQUISITION_APP_BUNDLE=$app" -P "$script_dir/../cmake/SignMacOS.cmake"

cpack --config "$script_dir/../cmake/MacDmg.cmake" \
    -D "CPACK_INSTALLED_DIRECTORIES=$staging/payload;/" \
    -D "CPACK_PACKAGE_FILE_NAME=$package_name" \
    -D "CPACK_DMG_VOLUME_NAME=${app_name%.app}" \
    -D "CPACK_PACKAGE_DIRECTORY=$staging/output"
mv "$staging/output/$package_name.dmg" "$output_dir/$package_name.dmg"
echo "Created $output_dir/$package_name.dmg"
