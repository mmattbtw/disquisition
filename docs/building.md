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

Local builds, including `make` and direct CMake builds, default to
`disquisition development`, even when building a checkout of `main`.
The desktop workflow builds pull requests as `disquisition staging` and
`main` as `disquisition`. Manual workflow runs use production on `main`
and staging on other branches.

Each channel has a separate macOS bundle identifier, preferences and
application-data directory, so development and staging do not reuse production's
permission grants or settings. Production keeps its existing identity and paths.
Permission grants still depend on the signing identity described below.

To select a channel explicitly, use `make BUILD_CHANNEL=staging` or configure
CMake with `-DDISQUISITION_BUILD_CHANNEL=staging`. Valid channels are
`development`, `staging` and `production`. CMake remembers the selected channel
in the build directory; `make` selects development unless `BUILD_CHANNEL` is set.

The desktop target also needs Qt 6.6 or newer with Widgets, Network and
Multimedia, plus pkg-config and FFmpeg development libraries. CMake checks for
`libavcodec` version 60 or newer, `libavutil` and `libswscale`. It skips the
desktop app if Qt or FFmpeg is missing.
On Linux, install the XCB development package, such as `libxcb1-dev` on
Debian or Ubuntu, for the presenter drawing overlay on X11. CMake detects
it through pkg-config. Without it, annotations remain available in the
video preview and fullscreen viewer.

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

### Native macOS drawing test

The regular drawing tests use Qt's offscreen platform. To also check the
presenter panels against AppKit, run this from a macOS desktop session:

```sh
cmake --build build --target mac_drawing_panel_test
./build/mac_drawing_panel_test
```

This test checks that enabling the overlay and pen leaves the app inactive,
forwards native strokes to the shared drawing, renders the shared colors,
supports selection and erasing, and restores click-through on Escape. It briefly shows native windows, so it runs separately from `ctest`.
For a manual check, put another app over Disquisition, enable the presenter
pen, draw, change colors, and press Escape. The client should stay behind the
shared app, and Escape should let clicks reach that app again.

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
scripts/package-macos.sh "build/disquisition development.app" build-baresip/output/baresip build/disquisition-development.dmg
```

The script needs `macdeployqt` and `cpack` on `PATH`, plus Finder to set the
installer window layout. Pass the bundle for the channel you built, such as
`build/disquisition staging.app` or `build/disquisition.app`.
It preserves the app's name, bundles dependencies and signs the finished app
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
