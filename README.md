# disquisition

Disquisition is a C++20 group chat app with desktop and terminal clients.
The desktop app supports voice chat and screen sharing. A central server
handles names and discovery; text messages travel between peers, directly or
through an optional relay. The server does not store chat history.

There is no encryption or authentication. Use it with people and networks you
trust.

## Download

Get the desktop app from the [latest release](https://github.com/mmattbtw/disquisition/releases/latest):

- [macOS, Apple silicon](https://github.com/mmattbtw/disquisition/releases/latest/download/disquisition-desktop-macos-arm64.dmg)
- [Windows, x64](https://github.com/mmattbtw/disquisition/releases/latest/download/disquisition-desktop-windows-x64.zip)
- [Linux, x64](https://github.com/mmattbtw/disquisition/releases/latest/download/disquisition-desktop-linux-x64.tar.gz)

Open the app, enter a name and choose Join. Fresh installs use the default
server and relay. Linux downloads need a compatible Qt 6.8 runtime.
See the [desktop guide](docs/desktop.md) for setup, voice and screen sharing.

## Build

For Linux or macOS, install a C++20 compiler, CMake 3.16+, SQLite 3.24+ and
ncurses development headers. The first build also needs Git and network access.
On macOS with Homebrew:

```sh
brew install cmake sqlite ncurses
```

Build and run the tests:

```sh
make
ctest --test-dir build --output-on-failure
```

The desktop app also needs Qt and FFmpeg. See [building and packaging](docs/building.md)
for desktop dependencies, Windows builds and macOS signing.

## Run a local chat

Start the server:

```sh
./build/server --port 9000 --db chat.db
```

Run each client in a separate terminal:

```sh
./build/client --host 127.0.0.1 --port 9000 --name matt
./build/client --host 127.0.0.1 --port 9000 --name jesse
```

## Documentation

- [Building and packaging](docs/building.md)
- [Desktop app](docs/desktop.md)
- [Servers, peers and relays](docs/networking.md)
- [Terminal client and command-line reference](docs/command-line.md)
- [C++ client library](docs/client-library.md)
- [Git crash course](docs/git_crash_course.md)

## Contributors

- Matt Morris [@mmattbtw](https://github.com/mmattbtw)
- Jack Stefl
- Cameron Sapienza [@ohkee](https://github.com/ohkee)
- Zheer Shimeirani [@z-shim](https://github.com/z-shim)
- Jesse Tomlin [@ChaosSnakey](https://github.com/ChaosSnakey)

To add your name, follow the [Git crash course](docs/git_crash_course.md#guided-tutorial-add-your-name-to-readmemd).
