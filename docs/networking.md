# Servers, peers and relays

Disquisition uses plain TCP without encryption, authentication, private rooms
or access control. Use hosts and networks you trust. Relays hide client addresses
from other users while direct connections reveal them. Relays and servers can
read relayed voice and screen shares.

The public discovery server is `server.disquisition.app:9000`, and the public
relay is `relay.disquisition.app:3333`.

## Start a server

```sh
./build/server --port 9000 --db chat.db
```

The server creates the configured SQLite file if it does not exist, but creates
no message tables and stores no chat history.

## Direct connections

Each direct client opens a peer listener on an automatically selected port.
On a LAN, the server announces the source address it sees. In the desktop app,
set the server address and port in Preferences and leave the relay address
blank to connect directly. Across routed networks, set a reachable public host
and forward the TCP chat port through the firewall or router. Select
"Advertise local IP automatically" for clients on the same LAN.

If two connected users request the same name, the server gives the later user a suffix such as `-2`.

## Message delivery

After sign-in, the server sends the client a roster containing each user's
host and peer port. For each pair of users, the lexicographically earlier name
opens the connection. This produces one TCP connection per pair.

The desktop client sends messages live to the peer mesh. The server does not store messages or replay recent history to new clients.

The desktop client retries a lost server connection every three seconds.
Discovery stops while the server is unavailable.

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

In the desktop app, set the relay address to `relay.example.net` and the relay
port to `3333` in Preferences, then enter a name and choose Join.

The relay opens a separate server session and peer mesh for each attached user. Clients requesting the same name keep separate sessions; the server assigns a suffix such as `matt-2` to later arrivals. It keeps its roster in memory, does not have a database, and drops a user's in-memory state when that client disconnects. The central server handles names and discovery.

If the chat server goes down, the relay retries it every three seconds. Peer links that are already established may continue to carry live traffic.

The relay's `--listen` port defaults to `3333`.

### Optional direct fallback

In desktop Preferences, enable "Connect directly if relay fails (reveals your
IP)" to use the configured server when the relay is unavailable. This reveals
your address to the server and direct peers. See the
[desktop guide](desktop.md#relay-and-direct-fallback) for retry behavior and
voice transitions.

## Desktop media

Direct voice uses baresip SIP calls between peers. Relayed voice and all screen
shares travel through the chat server. See the [desktop guide](desktop.md) for
voice ports, direct fallback, screen sharing and permissions.

Older servers reject voice messages and disconnect clients that start a screen
share, so those video and voice updates require rebuilding `server` and `relay`.
Screen drawing uses the peer mesh instead. Its permissions and history live in
the presenting desktop client. The relay forwards a generic `PeerData` message
with a target, channel name and opaque payload, stamping the sender from the
established link. It does not interpret drawings or store drawing history.
Drawing requires updated desktop clients and relays, but no server changes.

For flags and defaults, see the [command-line reference](command-line.md).
