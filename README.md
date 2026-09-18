# disquisition

A tiny group text chat with a hybrid peer-to-peer design: a C++ server
handles accounts, peer discovery and message storage in SQLite, but live
chat travels directly between peers over raw TCP sockets. Kill the server
mid-conversation and the mesh keeps talking.

## How it works

Each client listens on its own peer port (picked automatically unless you
pass `--p2p-port`) and announces it at login. The server hands out the
on-line roster (`name, host, port`) and broadcasts joins and leaves. Peers
then form a full mesh: for every pair, the peer whose name sorts first
dials the other and introduces itself with a `Hello` frame, so there is
exactly one TCP connection per pair with no negotiation.

Sending a line delivers it straight to every connected peer and also
`Store`s a copy on the server for history. New joiners fetch recent
messages from the server only after their mesh has settled, and anything
arriving through both paths is shown once. If the server connection drops,
the client stays up on peer-to-peer alone and signs itself back in when
the server returns — it even starts without a server and waits for one.
Storage and history simply pause during an outage; anything sent meanwhile
travels peer-to-peer only.

Peers must be able to open TCP connections to each other, so this is built
for a LAN or a single machine rather than the open internet — unless you run a
relay (below).

## Relays: joining without port forwarding

A relay is a small, state-less service you run on any host that _can_ accept
inbound connections (a VPS, say). Many users share one relay and one port: each
client supplies its own name, and the relay signs into the server on that
user's behalf, advertises itself as the user's peer address, and does the whole
mesh for them — dialing reachable peers directly and calling other people's
relays. A client never listens for anything; it just dials the relay outbound,
so it works from behind NAT.

```sh
# on the public host: one relay, reachable at relay.mmatt.net:3333
./build/relay --host relay.mmatt.net --port 9000 \
              --advertise relay.mmatt.net --listen 3333

# on each laptop: no port forward, no --advertise
./build/client --relay relay.mmatt.net:3333 --name matt
./build/client --relay relay.mmatt.net:3333 --name jesse
```

Because several users share the port, peers dial it with a target: the `Hello`
frame names both the caller and the user it wants, so the relay routes the link
to the right person. Everyone else still sees each user arrive at the relay's
address, e.g. `matt joined (relay.mmatt.net:3333)`. The relay keeps no
database and stores no messages; it only forwards frames, and the server still
owns accounts, discovery and history. Relay-to-relay links need no special
support: a relay dials another user's relay exactly the way it dials any peer,
so there is still one connection per pair.

## Layout

```
src/common/protocol.{h,cpp}   shared wire format (length-prefixed frames)
src/server/                   poll(2) event loop + SQLite persistence
src/client/                   ncurses TUI + socket reader thread
src/client/peer_network.*     the peer-to-peer mesh (listener + dialer)
src/relay/                    shared multi-user relay (one port, many users)
```

## Building

Requires a C++17 compiler, CMake 3.16+, SQLite 3.24+, and ncurses.

```sh
make          # builds build/client, build/server and build/relay
make clean    # removes the build directory
```

(Or run the CMake commands from the Makefile directly if you prefer.)

## Running

Start the server (it creates the database on first run):

```sh
./build/server --port 9000 --db chat.db
```

### C++ client library

Link the `disquisition::client` CMake target and include
`<disquisition/client.h>`. This first version sends messages through a relay
or through direct peer connections.

```cpp
#include <iostream>
#include <string>
#include <disquisition/client.h>

void showMessage(std::string sender, std::string message);

disquisition::Client client("relay.mmatt.net:3333", disquisition::Client::RELAY);
client.onMessage(showMessage);
client.connect();
client.setName("matt");
client.sendMessage("what's up");
client.setColor(20);
client.disconnect();
```

The message function can be an ordinary function that accepts the sender and
message text:

```cpp
void showMessage(std::string sender, std::string message)
{
    std::cout << sender << ": " << message << std::endl;
}
```

`RELAY` is the default, so the second constructor argument may be left out.
Use `DIRECT` with the central server address to join the peer-to-peer network
without a relay:

```cpp
disquisition::Client client("relay.mmatt.net:9000", disquisition::Client::DIRECT);
```

The methods throw an exception when an address, name, message, color, or
network connection is invalid.

The [basic client example](examples/basic_client.cpp) asks for the connection
settings and then sends each line you type. It is kept separate from the CMake
build so it can be copied into a small class project. Build it from its own
folder:

```sh
cd examples
make
./basic_client
```

Then connect one client per person:

```sh
./build/client
./build/client --host 127.0.0.1 --port 9000
./build/client --host 127.0.0.1 --port 9000 --name matt
./build/client --host 127.0.0.1 --port 9000 --name matt --p2p-port 9011
```

With no arguments, the client asks for the server host, server port, and your
name before connecting. The host and port default to `relay.mmatt.net:9000`.
Despite its hostname, this is the chat server, not the relay service. When you
pass other arguments without `--name`, the chat screen asks for your name.
Duplicate names get a `-2` suffix. `--p2p-port` pins the peer listener to a fixed port (handy
through a firewall); by default the OS picks a free one and the client reports
it. Each connection starts on a random candy shade. Pick another with
`/color mint` (also `pink`, `butter`, `periwinkle`, `lilac`, `aqua`, or
`peach`), or use any unreserved xterm-256 index with `/color 123`. Each
message stores the color it was sent with, and the last 50 messages are
replayed to everyone who joins. Your own messages always show in white.

To join through a relay instead of accepting direct connections (no port
forward), pass `--relay` with the relay's host and port:

```sh
./build/client --relay relay.mmatt.net:3333 --name matt
```

The name you pass is what the relay signs in as, so any number of users can
share one relay.

Pass `--leak-my-ip` to fall back to the server and direct peer connections when
the relay is unavailable. The client keeps retrying the relay and switches back
when it returns. The fallback server defaults to the relay host on port 9000;
use `--host` and `--port` when the relay connects to a different server. This
exposes your IP address to the server and other peers:

```sh
./build/client --relay relay.mmatt.net:3333 --leak-my-ip --name matt
```

### Client keys

| Key                                                   | Action                  |
| ----------------------------------------------------- | ----------------------- |
| `Enter`                                               | send                    |
| `Up` / `Down`, `PgUp` / `PgDn`, `Home` / `End`        | scroll the message pane |
| `Ctrl-C`                                              | quit                    |
| `/help`, `/users`, `/color <name>`, `/clear`, `/quit` | commands                |

## Group Members

- Matt Morris [@mmattbtw](https://github.com/mmattbtw)
- Jack Stefl
- Cameron Sapienza [@ohkee](https://github.com/ohkee)
- Zheer Shimeirani [@z-shim](https://github.com/z-shim)
- Jesse Tomlin [@ChaosSnakey](https://github.com/ChaosSnakey)

> [!TIP]
> Add your name to the above list if you are not already included.
>
> If you need help, you can follow the step by step guide here: https://github.com/mmattbtw/disquisition/blob/main/docs/git_crash_course.md#guided-tutorial-add-your-name-to-readmemd
