# Servers, peers and relays

Disquisition uses plain TCP without encryption, authentication, private rooms
or access control. Use hosts and networks you trust. Relays hide client addresses
from other users while direct connections reveal them. Relays and servers can
read relayed voice and screen shares.

## Start a server

```sh
./build/server --port 9000 --db chat.db
```

The server creates the configured SQLite file if it does not exist, but creates
no message tables and stores no chat history.

## Direct connections

Each direct client opens a peer listener on an automatically selected port.
On a LAN, the server announces the source address it sees. Across routed
networks, pass a reachable address with `--advertise` and forward the chosen
`--p2p-port` through the firewall or router.

Pass `--local` to announce the client's local IPv4 address automatically. This is useful when the server sees a different address, such as when it runs on the same machine. The selected address appears in the client's greeting. Use `--advertise` instead when peers need a public address. With `--relay`, `--local` applies only during direct fallback enabled by `--leak-my-ip`.

```sh
./build/client \
  --host chat.example.net \
  --port 9000 \
  --name matt \
  --advertise matt.example.net \
  --p2p-port 9011
```

If two connected users request the same name, the server gives the later user a suffix such as `-2`.

## Message delivery

After sign-in, the server sends the client a roster containing each user's
host and peer port. For each pair of users, the lexicographically earlier name
opens the connection. This produces one TCP connection per pair.

The terminal client sends messages live to the peer mesh. The server does not store messages or replay recent history to new clients.

The terminal client retries a lost server connection every three seconds.
Existing peer links can continue carrying live messages while the server is
unavailable, but discovery stops.

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

The relay opens a separate server session and peer mesh for each attached user. Clients requesting the same name keep separate sessions; the server assigns a suffix such as `matt-2` to later arrivals. It keeps its roster in memory, does not have a database, and drops a user's in-memory state when that client disconnects. The central server handles names and discovery.

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

## Desktop media

Direct voice uses baresip SIP calls between peers. Relayed voice and all screen
shares travel through the chat server. See the [desktop guide](desktop.md) for
voice ports, direct fallback, screen sharing and permissions.

Rebuild and restart both `server` and `relay` when updating desktop media
support. Older servers reject voice messages and disconnect clients that start
a screen share.

For flags and defaults, see the [command-line reference](command-line.md).
