# Building and packaging

Run commands from the repository root unless a section says otherwise.

## Requirements

For the server, relay, terminal client and C++ library on Linux or macOS:

- A C++20 compiler and CMake 3.16 or newer
- SQLite 3.24 or newer and ncurses, including development headers
- POSIX sockets and `poll`
- Git and network access on the first configure, to download spdlog

On macOS with Homebrew:

```sh
brew install cmake sqlite ncurses
```

CMake uses the Homebrew libraries when available.

## Build and test

```sh
make
ctest --test-dir build --output-on-failure
```

Or use CMake directly:

```sh
cmake -S . -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The build creates `build/server`, `build/client`, `build/relay` and the static
`disquisition::client` library. `make clean` removes the `build` directory.

## Desktop app

The desktop target also needs Qt 6.6 or newer with Widgets, Network and
Multimedia, plus pkg-config and FFmpeg development libraries. CMake checks for
`libavcodec` version 60 or newer, `libavutil` and `libswscale`. It skips the
desktop app if Qt or FFmpeg is missing.

```sh
cmake -S . -B build -DBUILD_DESKTOP_APP=ON
cmake --build build --target disquisition-desktop
```

To build only the server, relay, terminal client and library, configure with
`-DBUILD_DESKTOP_APP=OFF`. To build only the desktop pieces, use
`-DBUILD_LEGACY_TARGETS=OFF`.

Local desktop builds need `baresip` on `PATH` for direct voice chat. Install a
baresip build that includes `menu`, `mixminus`, `vumeter`, `ctrl_tcp`, `g711`
and the platform audio module: `coreaudio`, `wasapi` or `alsa`.
The app creates a small isolated baresip profile in the
platform application-data directory.

### Windows

Windows builds only the desktop pieces by default because the other targets
use POSIX sockets. Install the desktop dependencies above, including SQLite
headers and FFmpeg libraries that pkg-config can find. Run the server on Linux
or macOS.

```powershell
cmake -S . -B build -DBUILD_DESKTOP_APP=ON -DBUILD_LEGACY_TARGETS=OFF
cmake --build build --config Release --target disquisition-desktop
```

The [desktop workflow](../.github/workflows/desktop.yml) has the full dependency
setup for all three platforms.

### macOS signing

On macOS, local builds automatically use the first valid Apple Development
certificate in your keychain. Its signing identity lets macOS keep screen
recording and microphone permission grants across rebuilds. To choose a
specific certificate, configure with
`-DDISQUISITION_CODESIGN_IDENTITY="certificate name or SHA-1"`.
Existing build directories configured with `-` keep that setting; pass
`-DDISQUISITION_CODESIGN_IDENTITY=AUTO` to switch to automatic selection.
The build prints the selected identity and fails if signing fails.
After running `macdeployqt` or adding files to the app bundle, run
`cmake --build build --target disquisition-sign-macos` to sign and verify it
again with the configured identity.

Without an Apple Development certificate, builds fall back to ad-hoc signing
and print a warning. Use `-DDISQUISITION_CODESIGN_IDENTITY=-` to request this
explicitly. CI artifacts are also ad-hoc signed unless the runner has a
certificate. Their permissions may need to be granted again after updates.

## Package a macOS build

To package a local macOS build, use a baresip executable built with the static
voice modules from `cmake/bundled-baresip`, as in the desktop workflow:

```sh
scripts/package-macos.sh build/disquisition.app build-baresip/output/baresip build/disquisition.dmg
```

The script needs `macdeployqt` and `cpack` on `PATH`, plus Finder to set the
installer window layout. It bundles dependencies and signs the finished app
using the same automatic certificate selection as the build, with ad-hoc
signing as a fallback. If you configured a specific signing identity, set
`DISQUISITION_CODESIGN_IDENTITY` to that identity when running the script.
The DMG is not notarized.
The icon uses the desktop app's charcoal and mint colors, and the installer
uses a light background for readable Finder labels. Regenerate the artwork with
`swift scripts/generate-macos-artwork.swift`.

## Repository layout

```text
include/client/client.h        Public C++ library API
include/                       Headers, one folder per component below
src/common/                    Wire protocol, socket helpers and command-line parsing
src/server/                    Discovery server
src/client/                    Client library, terminal UI, connection and peer mesh
src/relay/                     Multi-user relay
src/desktop/                   Qt desktop app
src/web/                       Project website
test/                          Unit and end-to-end tests (run with ctest)
examples/                      Standalone library example
docs/                          Build, usage and contributor guides
```
