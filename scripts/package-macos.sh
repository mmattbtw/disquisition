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
    -DDISQUISITION_CODESIGN_HARDENED=ON \
    "-DDISQUISITION_APP_BUNDLE=$app" -P "$script_dir/../cmake/SignMacOS.cmake"

# Notarize only when credentials are given: a keychain profile saved with
# `xcrun notarytool store-credentials`, or an App Store Connect API key.
notary_args=()
if [[ -n ${DISQUISITION_NOTARY_PROFILE:-} ]]; then
    notary_args=(--keychain-profile "$DISQUISITION_NOTARY_PROFILE")
elif [[ -n ${DISQUISITION_NOTARY_KEY:-} ]]; then
    notary_args=(--key "$DISQUISITION_NOTARY_KEY" --key-id "${DISQUISITION_NOTARY_KEY_ID:?}"
        --issuer "${DISQUISITION_NOTARY_ISSUER:?}")
fi
# Sign the DMG with the app's identity. Ad-hoc signatures have no authority.
signer=$(codesign --display --verbose=2 "$app" 2>&1 | sed -n 's/^Authority=//p' | head -n 1)
if [[ ${#notary_args[@]} -gt 0 && $signer != "Developer ID Application: "* ]]; then
    echo "Notarization needs a Developer ID Application identity, not '${signer:-ad-hoc}'." >&2
    exit 1
fi

cpack --config "$script_dir/../cmake/MacDmg.cmake" \
    -D "CPACK_INSTALLED_DIRECTORIES=$staging/payload;/" \
    -D "CPACK_PACKAGE_FILE_NAME=$package_name" \
    -D "CPACK_DMG_VOLUME_NAME=${app_name%.app}" \
    -D "CPACK_PACKAGE_DIRECTORY=$staging/output"
dmg="$staging/output/$package_name.dmg"
if [[ -n $signer ]]; then
    codesign --force --timestamp --sign "$signer" "$dmg"
fi
if [[ ${#notary_args[@]} -gt 0 ]]; then
    # The status below decides failure, after printing Apple's log.
    xcrun notarytool submit "$dmg" "${notary_args[@]}" --wait --output-format json \
        > "$staging/notary.json" || true
    cat "$staging/notary.json"
    status=$(/usr/bin/plutil -extract status raw "$staging/notary.json" 2>/dev/null || true)
    if [[ $status != Accepted ]]; then
        submission=$(/usr/bin/plutil -extract id raw "$staging/notary.json" 2>/dev/null || true)
        if [[ -n $submission ]]; then
            xcrun notarytool log "$submission" "${notary_args[@]}" >&2 || true
        fi
        echo "Notarization failed with status '${status:-unknown}'." >&2
        exit 1
    fi
    xcrun stapler staple "$dmg"
    xcrun stapler validate "$dmg"
    spctl --assess --type open --context context:primary-signature --verbose=2 "$dmg"
fi
mv "$dmg" "$output_dir/$package_name.dmg"
echo "Created $output_dir/$package_name.dmg"
