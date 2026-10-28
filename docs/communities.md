# Communities

Communities live on member devices. The creator owns the community; any member
can host its metadata and admit peers with an existing valid invite. The
discovery `server` process is not involved in community connections.

Communities are a feature of the desktop client and the existing `relay`.
The relay only accepts community registrations and encrypted peer tunnels.
The desktop app and relay are the project's two application targets.

## Start on a LAN

In the desktop app, enter a name and select **Connect**, then
**Create**. A new community starts with `#general` and a voice channel named
Lounge. Select **Invite** to copy an invite link or save its QR image.
Another client selects **Join** and pastes the link or opens the QR image.

The default relay is `relay.disquisition.app` on port 3333. Relay settings accept
a hostname or `hostname:port`, with no scheme required.

For direct mode, clear **Relay** in **User Settings**. Direct mode
advertises a LAN address automatically. **User Settings**
lets you specify a hostname and listening port for connections across routed
networks. The address must be reachable, including any required port forwarding.
The automatically selected port is reused after restarting a saved community.

To run another desktop identity on the same computer:

```sh
./build/disquisition.app/Contents/MacOS/disquisition --community-data /tmp/disquisition-second
```

On Linux and Windows, use the desktop executable's platform-specific name with
the same `--community-data` option. Only one process can open a given identity
directory at a time. macOS builds register `disquisition://` invite links;
links can also be pasted into Join on every platform.
Keep `identity.key` with the metadata when backing up or moving this directory;
that key preserves the client's membership and owner/admin identity.

## Connect through the relay

Run this on a reachable machine:

```sh
./build/relay --listen 3333
```

Set **Relay** to `hostname` or `hostname:3333` before connecting each client.
Relay mode opens no direct listening port and advertises no direct client IP.
The relay sees the connecting addresses and routing IDs, but peer traffic,
invite challenges, metadata, chat, and media travel inside mutual TLS tunnels.

Build the relay with community support enabled, then restart it with the
updated binary. The relay has no upstream server or database. Its `--listen`
port handles community registrations and encrypted tunnels. This repository
change does not deploy or upgrade any public relay service.

The relay keeps only live routing registrations and pending tunnels. It does
not own community metadata, issue invites, decide permissions, or store history.
If the peer originally named in an invite is offline, the relay can
route the request to another online member of that community.

Direct invites contain up to eight bootstrap peers. If none is reachable, a
joining client retries automatically. Changed public IPs or a relay that no
longer exists require a reachable routing hint, such as an updated invite.

## Ownership, administration, and hosting

- The creator remains the owner and assigns or removes admins.
- The owner and each admin can independently edit the name, icon, channels,
  topics, invite policy, host priority, and member removals. There is no voting.
- Any member can act as host and validate an already-authorized invite without
  an admin online. Admission updates the member list automatically.
- Hosting does not grant administrative permissions or transfer ownership.
- With no owner or admin online, existing communication and invite admission
  continue. Administrative edits wait until an authorized client is present.

**Manage** has a category sidebar for community settings, channels, members,
invites, and host priority. It contains the channel editor, admin assignments,
invite revocations, member removal, and a draggable host priority list. Removing
a member blocks that peer identity from rejoining with another invite. The
current UI does not provide an unblock or ownership-transfer operation.

Host priority chooses the first reachable listed member. The owner and then
the remaining peer IDs provide a deterministic fallback. A disconnected group
can have its own preferred host until it reconnects with other members.
Invites start with the issuer, which already knows the newly issued code.
A bootstrap peer that knows a synchronized higher-priority host validates the
invite and redirects the join request there.

## Reconnects and network splits

Identity keys and community metadata always persist, even with chat saving off.
When everyone is offline, the community has no active host. The first returning
member restores its saved metadata and retries known peers with bounded
backoff and jitter. It can host the community while alone.

A returning member exchanges metadata with reachable peers before processing
new admissions or administrative edits. Connections still synchronizing block
those operations. After startup recovery attempts settle, unreachable peers do
not prevent a lone member from accepting an existing invite.

Each metadata operation identifies its author, causal parents, and a logical
sequence number, and carries an Ed25519 signature. Peers merge the operation
sets and derive the same result. Concurrent edits to the same setting use a
deterministic order of logical sequence and operation ID; unrelated edits survive.
No wall clock determines administrative authority.

An owner-issued admin grant authorizes an admin's edits. Revoking that grant
blocks concurrent and later edits under it. Member removal and invite revocation
also win against concurrent admissions. An admission accepted before an invite
revocation remains valid; revoking an invite does not remove existing members.
Removing a host preserves the admissions it accepted before its removal.

A partition cannot receive changes made elsewhere until it reconnects. Peers
therefore validate invites and permissions against their latest known accepted
metadata. On reconnect, missing operations are validated, durably saved, and
distributed before admissions resume. A former host's cached state cannot
replace a newer operation set.

Metadata is bounded to 8 MiB and 8,192 operations per community. This version
exchanges bounded snapshots when revisions differ; log compaction and fetching
only missing operations can be added without changing the ownership model.
The current mesh limits each client to 256 simultaneous peer connections.

## Chat, voice, and local saving

One authenticated connection per peer pair carries traffic for all shared
communities. Packets identify both the community and channel. Clients send only
to members of that community and validate membership on receipt.

Select a text channel to send messages, images, or GIFs. Messages are live-only.
If nobody is connected, the local echo says it was not delivered. It will not
be queued for later delivery. Reconnecting clients do not download missed chat.

Select a voice channel and choose **Join voice** beneath its name. Your text
channel stays open. Participants appear beneath the voice channel, with icons
for mute, deafen, and screen sharing. Call controls appear only while connected.
A client joins one voice channel at a time across its communities. Muting, deafening, audio,
screen announcements, and screen subscriptions stay within that channel.
Screen frames go only to peers who choose to watch the share. Leaving the
channel or losing membership stops the local call and share.

**User Settings** controls optional SQLite saving, its file path, and an
optional maximum number of messages. Only chat this device received or sent
is saved. History is scoped by community and channel, including after restart.
Existing message files upgrade in place without deleting their records.
Messages without a community/channel scope are not mixed into community chat.
Uploaded images are not saved as chat history. Community metadata and identity
storage are independent of this option.

## Build and verification

Community support needs Qt 6.6+ Core/Network and OpenSSL 3+. The desktop also
uses the existing Qt Multimedia/FFmpeg dependencies. QR support fetches a pinned
ZXing-C++ release and links it statically. `-DBUILD_DESKTOP_APP=OFF` builds the
relay and core tests without multimedia dependencies. Use `-DBUILD_RELAY=OFF`
for desktop-only builds.

The community model tests cover signatures, permission boundaries, independent
admin decisions, admissions without admins, revocations, split reconciliation,
and metadata persistence. Network tests run real direct and relay connections,
host migration, recovery before admission, connection sharing, channel isolation,
voice scoping, all-members-offline recovery, and live-only delivery. QR tests
encode and read invite images. SQLite tests cover legacy migration, channel
isolation, duplicate handling, retention, and rollback.
The desktop UI test connects an isolated client, exercises creation and admin
dialogs, checks channel separation and undelivered local echoes, and captures
`build/community-ui.png` without accessing microphone or screen hardware.

When the relay target is enabled, network tests launch the actual `relay`
without any discovery service and check admission, host migration, recovery,
invalid options, and rejection of the retired room protocol.
