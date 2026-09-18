# disquisition

Disquisition is a small C++17 group chat program with peer-to-peer live delivery and a central coordination server. The server assigns names, announces peers, and stores recent messages in SQLite. Chat messages travel over direct TCP links between peers, either from the client itself or through an optional relay.

The repository builds four pieces:

- `server`, the discovery and history service
- `client`, an ncurses terminal client
- `relay`, a shared gateway for clients that cannot accept inbound connections
- `disquisition::client`, a static C++ client library

This is a plain TCP protocol. It does not provide encryption, authentication, private rooms, or access control. Use it only on networks and hosts you trust.

## Requirements

- CMake 3.16 or newer
- a C++17 compiler
- SQLite 3.24 or newer, including development headers
- ncurses, including development headers
- POSIX sockets and `poll`

## Build and test

```sh
make
ctest --test-dir build --output-on-failure
```

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

## How delivery works

After sign-in, the server sends the client a roster containing each user's host and peer port. For each pair of users, the lexicographically earlier name opens the connection. This produces one TCP connection per pair.

When the terminal client sends a message, it does two separate things:

1. It sends the live message to the peer mesh.
2. It sends a copy to the server for SQLite storage.

After joining, the terminal client waits for its peer links to settle and then requests recent history. It removes duplicates when the same message arrives from both the live mesh and history.

The terminal client retries a lost server connection every three seconds. Existing peer links can continue carrying live messages while the server is unavailable, but discovery and history storage stop. Messages sent during that outage are not added to SQLite later.

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

The relay opens a separate server session and peer mesh for each attached user. It keeps its roster in memory, does not have a database, and drops a user's in-memory state when that client disconnects. The central server still handles names, discovery, and stored history.

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
    --history <count>   Messages returned for history. Default: 50
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
    --leak-my-ip            Fall back to direct mode if the relay is unavailable
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
#include <disquisition/client.h>
```

A direct connection to the central server is the default:

```cpp
#include <iostream>
#include <string>

#include <disquisition/client.h>

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

The current library is smaller than the terminal client. It supports live send and receive, direct or relay mode, and history storage for sent messages. It does not request stored history, reconnect after a connection failure, expose the user roster, or report the final suffixed name. The message callback runs on the library's background service thread, so callback code must be thread-safe. The API throws standard exceptions for invalid values, invalid call order, and connection failures.

See [`examples/basic_client.cpp`](examples/basic_client.cpp) for an interactive example. It uses its own Makefile and compiles the required project sources directly:

```sh
cd examples
make
./basic_client
```

## Repository layout

```text
include/disquisition/client.h  Public C++ library API
src/lib/                       Client library implementation
src/common/                    Shared framed wire protocol
src/server/                    Discovery and SQLite history server
src/client/                    Terminal UI, connection code, and peer mesh
src/relay/                     Multi-user relay
test/                          Client library test
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
