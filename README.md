# disquisition

Disquisition is a C++20 desktop group chat app.
Communities live on member devices, with text channels, channel voice and
screen sharing, invite links and QR codes, and automatic hosting by online
members. Desktop peers connect directly or through the relay; no discovery
server is needed. Chat is live-only, with optional local saving.

Community connections use authenticated TLS and signed metadata.

## Download

Get the desktop app from the [latest release](https://github.com/mmattbtw/disquisition/releases/latest):

- [macOS, Apple silicon](https://github.com/mmattbtw/disquisition/releases/latest/download/disquisition-desktop-macos-arm64.dmg)
- [Windows, x64](https://github.com/mmattbtw/disquisition/releases/latest/download/disquisition-desktop-windows-x64.zip)
- [Linux, x64](https://github.com/mmattbtw/disquisition/releases/latest/download/disquisition-desktop-linux-x64.tar.gz)

Open the desktop app, enter a name and select Connect, then create a
community or join with an invite. See the [community guide](docs/communities.md)
for direct connections, relay setup, hosting, and permissions.
Linux downloads need a compatible Qt 6.8 runtime.
See the [desktop guide](docs/desktop.md) for setup, voice and screen sharing.

## Build

Install a C++20 compiler, CMake 3.16+, Qt 6.6+, OpenSSL 3+, SQLite 3.24+,
FFmpeg development libraries, and pkg-config. The first desktop configure also
needs Git and network access for the pinned QR-code library.
On macOS with Homebrew:

```sh
brew install cmake qt openssl@3 sqlite ffmpeg pkgconf
make
ctest --test-dir build --output-on-failure
```

See [building and packaging](docs/building.md) for relay-only builds, Windows
setup, and macOS signing. The project builds the desktop app and `relay`.

## Run a local relay

```sh
./build/relay --listen 3333
```

In the desktop app's **Connection & saving**, set **Relay** to
`127.0.0.1:3333`, connect, and create a community. Share an invite with another
desktop client. The relay needs no discovery server or database.

## Documentation

- [Building and packaging](docs/building.md)
- [Desktop app](docs/desktop.md)
- [Communities, channels, invites and hosting](docs/communities.md)
- [Peers, encrypted connections, and relay routing](docs/networking.md)
- [Desktop and relay command-line reference](docs/command-line.md)
- [Git crash course](docs/git_crash_course.md)

## Contributors

- Matt Morris [@mmattbtw](https://github.com/mmattbtw)
- Jack Stefl
- Cameron Sapienza [@ohkee](https://github.com/ohkee)
- Zheer Shimeirani [@z-shim](https://github.com/z-shim)
- Jesse Tomlin [@ChaosSnakey](https://github.com/ChaosSnakey)

To add your name, follow the [Git crash course](docs/git_crash_course.md#guided-tutorial-add-your-name-to-readmemd).
