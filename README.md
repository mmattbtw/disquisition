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
for a LAN or a single machine rather than the open internet.

## Layout

```
src/common/protocol.{h,cpp}   shared wire format (length-prefixed frames)
src/server/                   poll(2) event loop + SQLite persistence
src/client/                   ncurses TUI + socket reader thread
src/client/peer_network.*     the peer-to-peer mesh (listener + dialer)
tests/p2p_smoke.py            end-to-end test: mesh, history, server death
```

## Building

Requires a C++17 compiler, CMake 3.16+, SQLite3, and ncurses.

```sh
make          # builds build/client and build/server
make test     # builds, then runs the end-to-end smoke test
make clean    # removes the build directory
```

(Or run the CMake commands from the Makefile directly if you prefer.)

## Running

Start the server (it creates the database on first run):

```sh
./build/server --port 9000 --db chat.db
```

Then connect one client per person:

```sh
./build/client --host 127.0.0.1 --port 9000
./build/client --host 127.0.0.1 --port 9000 --name matt
./build/client --host 127.0.0.1 --port 9000 --name matt --p2p-port 9011
```

Without `--name` the client asks for one. Duplicate names get a `-2` suffix.
`--p2p-port` pins the peer listener to a fixed port (handy through a
firewall); by default the OS picks a free one and the client reports it.
Messages are stored in the `messages` table and the last 50 are replayed to
everyone who joins.

### Client keys

| Key | Action |
| --- | --- |
| `Enter` | send |
| `Up` / `Down`, `PgUp` / `PgDn`, `Home` / `End` | scroll the message pane |
| `Ctrl-C` | quit |
| `/help`, `/users`, `/clear`, `/quit` | commands |

## Group Members

- Matt Morris [@mmattbtw](https://github.com/mmattbtw)
- Jack Stefl 
- Cameron Sapienza
- Zheer Shimeirani [@z-shim](https://github.com/z-shim)
- Jesse Tomlin [@ChaosSnakey](https://github.com/ChaosSnakey)

> [!TIP]
> Add your name to the above list if you are not already included.
>
> If you need help, you can follow the step by step guide here: https://github.com/mmattbtw/disquisition/blob/main/docs/git_crash_course.md#guided-tutorial-add-your-name-to-readmemd
