# Building and packaging

Run commands from the repository root unless a section says otherwise.

## Requirements

Both the desktop app and relay need a C++20 compiler, CMake 3.16+, and OpenSSL 3+
Crypto. The relay needs Qt 6.2+ Core/Network; the desktop app needs Qt 6.6+.

The desktop app also needs Qt Widgets and Multimedia, SQLite 3.24+, pkg-config,
and FFmpeg development libraries: `libavcodec` 60+, `libavutil`, and `libswscale`.
Git and network access are needed on the first desktop configure to fetch the
pinned ZXing-C++ revision used for QR export and import. Voice uses Qt audio.

On macOS with Homebrew:

```sh
brew install cmake qt openssl@3 sqlite ffmpeg pkgconf
```

## Ubuntu VPS relay setup

On Ubuntu 22.04, 24.04, or newer, install the development packages and build:

```sh
sudo apt-get update
sudo apt-get install -y build-essential cmake qt6-base-dev libssl-dev
make relay
./build-relay/relay --listen 3333
```

The relay uses Qt Core and Network without a GUI or display server. Ubuntu's
`qt6-base-dev` package supplies `Qt6Config.cmake` and is sufficient for the relay;
Qt Multimedia, SQLite, and FFmpeg are not needed. Use `make relay` on a VPS;
plain `make` also configures the desktop and its newer Qt dependencies.

If apt cannot find `qt6-base-dev`, enable Ubuntu's Universe repository:

```sh
sudo apt-get install -y software-properties-common
sudo add-apt-repository -y universe
sudo apt-get update
sudo apt-get install -y qt6-base-dev
```

If you already installed Qt outside the package manager, point CMake at it:

```sh
CMAKE_PREFIX_PATH=/path/to/Qt/6.x/gcc_64 make relay
```

## Build and test

```sh
cmake -S . -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

`make` runs the configure and build commands for the full project, including tests.
Build just one application with:

```sh
make desktop  # Desktop client in build-desktop
make relay    # Relay in build-relay
```

These targets use separate build directories, disable tests, and configure only
the requested application. `make desktop BUILD_CHANNEL=staging` selects the staging
client. Override `DESKTOP_BUILD_DIR` or `RELAY_BUILD_DIR` to choose another output
directory. `make clean` removes all three build directories.

The CMake application targets are
`relay` and `disquisition-desktop`. CMake skips the desktop target when its
Widgets/Multimedia or FFmpeg dependencies are unavailable. Core/Network and
OpenSSL are required.

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

On Linux, install `libxcb1-dev` for the X11 presenter overlay.

To build only the desktop app, configure with `-DBUILD_RELAY=OFF`.
For a relay-only build with no database or multimedia dependencies:

```sh
cmake -S . -B build-relay -DBUILD_DESKTOP_APP=OFF -DBUILD_TESTING=OFF
cmake --build build-relay --target relay --parallel
./build-relay/relay --listen 3333
```

The relay uses the Qt Core/Network and OpenSSL runtime libraries. It does not
need SQLite, FFmpeg, ncurses, or a separate discovery service.

### Windows

Install the dependencies above, including FFmpeg development libraries that
pkg-config can find. Build with a configured Qt and OpenSSL toolchain:

```powershell
cmake -S . -B build -DBUILD_RELAY=OFF
cmake --build build --config Release --target disquisition-desktop
```

The [desktop workflow](../.github/workflows/desktop.yml) contains the dependency
setup for all three platforms.

### Native macOS drawing test

The regular drawing tests use Qt's offscreen platform. To also check the
presenter panels against AppKit, run this from a macOS desktop session:

```sh
cmake --build build --target mac_drawing_panel_test
./build/mac_drawing_panel_test
```

This test checks that enabling the overlay and pen leaves the app inactive,
accepts native panel mouse events, forwards strokes to the shared drawing,
and renders the shared colors. It also supports selection and erasing, and
restores click-through on Escape. It briefly
shows native windows, so it runs separately from `ctest`.
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

```sh
scripts/package-macos.sh "build/disquisition development.app" build/disquisition-development.dmg
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
include/                       Component headers
src/common/                    Local message storage
src/community/                 Identities, signed metadata, and encrypted peer connections
src/relay/                     Community registration and encrypted tunnel routing
src/desktop/                   Qt desktop app and media
src/web/                       Project website
test/                          Model, network, persistence, media, and UI tests
docs/                          Build, usage, and contributor guides
```
