# disquisition

Disquisition is a small C++20 group chat program with peer-to-peer live delivery and a central coordination server. The server assigns names, announces peers, and does not store messages. Chat messages travel over direct TCP links between peers, either from the client itself or through an optional relay.

The repository builds five pieces:

- `server`, the discovery service
- `client`, an ncurses terminal client
- `relay`, a shared gateway for clients that cannot accept inbound connections
- `disquisition::client`, a static C++ client library
- `disquisition-desktop`, a Qt 6 desktop client with text chat and baresip voice

This is a plain TCP protocol. It does not provide encryption, authentication, private rooms, or access control. Use it only on networks and hosts you trust.

## Requirements

- CMake 3.16 or newer
- SQLite 3.24 or newer, including development headers
- a C++20 compiler
- ncurses, including development headers
- Git and network access the first time CMake configures, to download spdlog
- POSIX sockets and `poll`

On macOS with Homebrew, install the dependencies with `brew install cmake sqlite ncurses`.
Plain `make` then uses the Homebrew libraries when they are installed.

## Build and test

```sh
make
ctest --test-dir build --output-on-failure
```

The desktop app is built when Qt 6.5 or newer is available. It also expects the
`baresip` executable on `PATH` at runtime. Build it explicitly with:

```sh
cmake -S . -B build -DBUILD_DESKTOP_APP=ON
cmake --build build --target disquisition-desktop
```

On Windows, CMake builds only the portable desktop pieces by default because
the existing server, relay, terminal client, and library use POSIX sockets.
Run the server on Linux or macOS, then build the Windows app with Qt 6 and:

```powershell
cmake -S . -B build -DBUILD_DESKTOP_APP=ON -DBUILD_LEGACY_TARGETS=OFF
cmake --build build --config Release --target disquisition-desktop
```

Install a baresip build that includes `menu`, `mixminus`, `vumeter`, `ctrl_tcp`,
`g711`, and the platform audio module (`coreaudio`, `wasapi`, or `alsa`). The app
creates a small isolated baresip profile in the platform application-data
directory.

## Desktop voice chat

Start the normal server, then open `disquisition-desktop` on each computer.
Set the server address and port in Preferences (Cmd+, on macOS), enter a name,
and choose Join to enter text chat. Voice stays off until you choose Join voice.
Choose Leave voice to exit the call without leaving the server. Use a different
voice SIP port in Preferences for each client running on the same machine. If
peers cannot directly reach the address seen by the server, enter a reachable
DNS name or IP as the public host and forward both the automatically chosen TCP
chat port and the chosen SIP/RTP ports when connecting directly.

To keep your IP hidden from other users, set the relay address and port in
Preferences before joining. The desktop app then connects only to the relay;
it does not open a peer listener or a SIP/RTP socket. Relayed voice uses 16 kHz
mono PCM frames over that TCP connection. The relay passes those frames to the
chat server, which fans them out to the room. Direct participants keep using
baresip with other direct participants and send a second audio stream for
relayed participants. This means relayed voice is not end-to-end encrypted and
the relay and server can hear it. Use only a trusted relay and server.

"Connect directly if relay fails (reveals your IP)" is off by default. If
the relay is unavailable, the desktop client retries it every three seconds
without connecting directly. If direct fallback is enabled, a lost relay
connection uses the server address and port in Preferences. This reveals your
address to the server and direct peers while fallback is active. The client
checks the relay every three seconds without leaving the direct connection.
It switches back only after the relay confirms it can reach the chat server.
If you were in voice, it starts direct baresip voice during fallback and
switches back to relayed voice after that check succeeds. This reveals your IP to other
direct voice participants while fallback is active. Mute and deafen settings
carry over both transitions. The desktop health check requires a relay built
from the same version of this repository. Rebuild and restart both `server`
and `relay` after updating voice support; older servers reject voice messages.

For direct participants, the app uses the server roster to form one SIP call
per pair of users. Baresip's `mixminus` module combines those calls locally.
The name ordering rule makes only one side dial each pair, while the other
side auto-answers.

The member list reports voice state from baresip itself. `·` means text-only,
`○` means the SIP call is connected, and a green `●` means the microphone or
that peer's received audio is above the speaking threshold. The `vumeter`
module supplies audio levels and `ctrl_tcp` supplies call identity and state.

On macOS the app requests microphone access when you choose Join voice. Choose
the microphone and speaker from the `mic` and `out` dropdowns before joining
voice; the dropdowns refresh after permission is granted.

This first version is deliberately small. It works well on a LAN or between
publicly reachable hosts. It does not yet coordinate ICE/TURN credentials, RTP
port forwarding, authentication, or media encryption. Use it only with people
and networks you trust. A production internet deployment should add TURN and
DTLS-SRTP before treating the call as private.

The build creates `build/server`, `build/client`, `build/relay`, and the static client library. `make clean` removes the `build` directory.

You can also use CMake directly:

```sh
cmake -S . -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## Run a local chat

Start the server. It creates `chat.db` if the file does not exist.

```sh
./build/server --port 9000 --db chat.db
```

Open another terminal for each client:

```sh
./build/client --host 127.0.0.1 --port 9000 --name matt
./build/client --host 127.0.0.1 --port 9000 --name jesse
```

Each direct client opens a peer listener on an automatically selected port. On a LAN, the server announces the source address it sees. Across routed networks, pass a reachable address with `--advertise` and forward the chosen `--p2p-port` through the firewall or router.

```sh
./build/client \
  --host chat.example.net \
  --port 9000 \
  --name matt \
  --advertise matt.example.net \
  --p2p-port 9011
```

If two connected users request the same name, the server gives the later user a suffix such as `-2`.

Running `./build/client` with no arguments starts an interactive setup prompt. That prompt defaults to `relay.mmatt.net:9000`. When any command-line option is present, the normal command-line defaults are `127.0.0.1:9000`.

SQLite remains available for future server data. The server opens the configured database but creates no message tables and stores no chat history.

## How delivery works

After sign-in, the server sends the client a roster containing each user's host and peer port. For each pair of users, the lexicographically earlier name opens the connection. This produces one TCP connection per pair.

The terminal client sends messages live to the peer mesh. The server does not store messages or replay recent history to new clients.

The terminal client retries a lost server connection every three seconds. Existing peer links can continue carrying live messages while the server is unavailable, but discovery stops.

## Use a relay

A relay accepts outbound client connections and participates in the peer mesh for those clients. It is useful when clients are behind NAT or cannot expose a listening port. One relay process can host multiple users on one public port.

Run the relay on a host that can accept inbound TCP connections:

```sh
./build/relay \
  --host chat.example.net \
  --port 9000 \
  --advertise relay.example.net \
  --listen 3333
```

Then connect clients to it:

```sh
./build/client --relay relay.example.net:3333 --name matt
./build/client --relay relay.example.net:3333 --name jesse
```

The relay opens a separate server session and peer mesh for each attached user. It keeps its roster in memory, does not have a database, and drops a user's in-memory state when that client disconnects. The central server handles names and discovery.

If the chat server goes down, the relay retries it every three seconds. Peer links that are already established may continue to carry live traffic.

The relay port defaults to `3333` when it is omitted from `--relay`. The relay's own `--listen` option has the same default.

### Optional direct fallback

`--leak-my-ip` lets a relayed terminal client fall back to a direct server connection when the relay is unavailable. It starts a local peer listener, reveals the client address to the server and other peers, and switches back to the relay when it returns.

```sh
./build/client \
  --relay relay.example.net:3333 \
  --host chat.example.net \
  --port 9000 \
  --leak-my-ip \
  --name matt
```

If `--host` is absent, the fallback assumes that the chat server runs on the relay host at the selected server port, which defaults to `9000`.

## Command-line reference

### Server

```text
-p, --port <port>       Listen port. Default: 9000
-d, --db <path>         SQLite file. Default: chat.db
-h, --help              Show help
```

Passing port `0` asks the operating system to select a free server port.

### Client

```text
-H, --host <host>          Server host. CLI default: 127.0.0.1
-p, --port <port>          Server port. Default: 9000
-n, --name <name>          Name to request at sign-in
    --p2p-port <port>      Direct peer listener. Default: 0, an automatic port
    --advertise <host>     Reachable address announced to peers
    --relay <host[:port]>  Use a relay. Default relay port: 3333
    --leak-my-ip           Fall back to direct mode if the relay is unavailable
    --log <file>           Write a debug log to this file. Default: no log
-h, --help                 Show help
```

### Relay

```text
-H, --host <host>       Chat server host. Default: 127.0.0.1
-p, --port <port>       Chat server port. Default: 9000
    --advertise <host>  Public address announced for the relay
    --listen <port>     Shared client and peer port. Default: 3333
-h, --help              Show help
```

For normal remote use, set `--advertise` to the relay's public DNS name or IP address.

### Logging

The server and relay log to stdout, one timestamped line per event, for example:

```text
[2026-09-24 01:36:07.247] [server] [info] alice joined from 127.0.0.1
```

The terminal client writes nothing to the console while its interface is open, so it logs only when given `--log <file>`. That file also includes debug detail from the networking code, such as every dial and reconnect attempt. The C++ library logs at debug level through spdlog's default logger, which is silent unless the host program enables debug output.

## Terminal client controls

| Input | Action |
| --- | --- |
| `Enter` | Send the current line |
| `Up`, `Down` | Scroll one row |
| `PgUp`, `PgDn` | Scroll one page |
| `Home`, `End` | Jump to the oldest or newest message |
| `Ctrl-C`, `Ctrl-D` | Quit |
| `Ctrl-L` | Redraw the terminal |
| `/users` | List users and known connection routes |
| `/color <value>` | Set a named shade or an xterm-256 index |
| `/clear` | Clear the local message pane |
| `/help` | Show commands |
| `/quit`, `/exit` | Quit |

Named colors are `pink`, `mint`, `butter`, `periwinkle`, `lilac`, `aqua`, and `peach`. Numeric colors range from `0` through `255`, except `1`, `2`, and `250`, which the interface reserves for system text. Numeric colors require a 256-color terminal. Your own messages appear white in your terminal regardless of the color sent to other users.

Names are trimmed, limited to 20 bytes, and have spaces changed to underscores. Message bodies are trimmed and limited to 2,000 bytes.

## C++ client library

CMake exposes the static library as `disquisition::client`. Include its public header with:

```cpp
#include <client/client.h>
```

A direct connection to the central server is the default:

```cpp
#include <iostream>
#include <string>

#include <client/client.h>

void showMessage(std::string sender, std::string body)
{
    std::cout << sender << ": " << body << '\n';
}

int main()
{
    disquisition::Client client("chat.example.net:3333");
    client.onMessage(showMessage);
    client.connect();
    client.setName("matt");
    client.setColor(20);
    client.sendMessage("hello");
    client.disconnect();
}
```

To connect through a relay, select `RELAY`:

```cpp
disquisition::Client client(
    "relay.example.net:42069",
    disquisition::Client::RELAY
);
```

The current library is smaller than the terminal client. It supports live send and receive in direct or relay mode. It does not reconnect after a connection failure, expose the user roster, or report the final suffixed name. The message callback runs on the library's background service thread, so callback code must be thread-safe. The API throws standard exceptions for invalid values, invalid call order, and connection failures.

See [`examples/basic_client.cpp`](examples/basic_client.cpp) for an interactive example. It uses its own Makefile and compiles the required project sources directly. Build the main project once first, so CMake has downloaded spdlog:

```sh
cd examples
make
./basic_client
```

## Repository layout

```text
include/client/client.h        Public C++ library API
include/                       Headers, one folder per component below
src/common/                    Wire protocol, socket helpers and command-line parsing
src/server/                    Discovery server
src/client/                    Client library, terminal UI, connection and peer mesh
src/relay/                     Multi-user relay
test/                          Unit and end-to-end tests (run with ctest)
examples/                      Standalone library example
docs/                          Contributor notes
```

## Contributors

- Matt Morris [@mmattbtw](https://github.com/mmattbtw)
- Jack Stefl
- Cameron Sapienza [@ohkee](https://github.com/ohkee)
- Zheer Shimeirani [@z-shim](https://github.com/z-shim)
- Jesse Tomlin [@ChaosSnakey](https://github.com/ChaosSnakey)

To add your name, follow the [Git crash course](docs/git_crash_course.md#guided-tutorial-add-your-name-to-readmemd).
