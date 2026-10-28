# Peers and relay routing

Community metadata lives on member devices. Clients use direct TCP connections
or a relay to reach one another. The project has no central discovery-server
program or global room protocol.

## Direct connections

Clear **Relay** in the desktop connection settings to accept peer connections
directly. The client advertises a LAN address automatically; set a reachable
hostname and listening port when connecting across routed networks. Public
connections may require port forwarding. Direct peers can see each other's
network addresses.

Invites carry the community ID, owner identity, invite code, and routing hints.
Only an authorized member validates admission and distributes community
metadata. Existing members restore their saved metadata and retry known peers
when reconnecting.

## Relay connections

```sh
./build/relay --listen 3333
```

Desktop clients default to `relay.disquisition.app:3333`. A custom setting can
be a hostname or `hostname:port`, without a URI scheme. In relay mode, clients
open outbound registrations and tunnels and do not advertise direct IPs.

The relay authenticates routing registrations by peer identity. It pairs a
caller's connection with an outbound connection from the target peer, then
forwards bytes unchanged. If an invite's original host is offline, it can route
to another registered member of that community. This routing hint grants no
community membership or administrative authority.

Peer connections use mutual TLS with identity-bound certificates. The relay
sees connecting addresses and routing IDs, but chat, voice, screen sharing,
invite validation, and metadata exchange remain inside peer TLS tunnels. The
relay does not hold community metadata, invite codes, or message history.

One authenticated connection per peer pair carries traffic for all shared
communities. Clients enforce community membership and channel scope. Voice and
screen sharing remain scoped to the joined voice channel.

The relay is the only network service to deploy for relayed communities. It
has no upstream discovery connection, health probe, room roster, or database.
Changes to the public relay require deploying the updated relay binary.

## Reconnection

The relay holds live routing registrations and pending tunnels in memory.
Restarting it drops those connections. Clients retry their registrations and
peer links automatically. Chat stays live-only and is not replayed on reconnect.

See [Communities](communities.md) for admission policy, hosting, split recovery,
and signed metadata synchronization.
