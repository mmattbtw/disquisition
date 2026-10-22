# Desktop app

The desktop app opens directly to communities. See the [community guide](communities.md)
for ownership, invites, host recovery, and local message saving.

Download the app from the [latest release](https://github.com/mmattbtw/disquisition/releases/latest).
For a local build, see [building and packaging](building.md#desktop-app).

## Install

The macOS DMG and Windows archive include Qt. The Linux archive requires a
compatible Qt 6.8 runtime. On macOS, open the DMG, drag disquisition into
Applications, and eject the disk image.

## Connect and join a community

Enter a name and select **Connect**. Fresh installs use `relay.disquisition.app`
on port 3333. Under **Connection & saving**, enter a relay hostname, optionally
followed by a port. No `relay://` prefix is needed. Saved settings take precedence;
clear the relay field to use direct connections and advertise a LAN address.

Select **Create** for a new community, or **Join** to paste an invite or open its
QR image. **Invite** lets owners and admins copy an invite link or save a QR code.
The relay forwards encrypted peer traffic. Community metadata lives on member
devices, and community connections do not need the discovery server.

Select a text channel to chat. Images can be attached with **Image** or
dropped into the chat. GIF search is available in builds configured with a search
provider key. Messages are live-only. Optional local SQLite saving records only
messages this device sends or receives.

## Voice and screen sharing

Select a voice channel, then choose **Join voice** beneath its name. The current
text channel stays open. Participants and their mute, deafen, and sharing status
appear beneath the voice channel rather than in the member list.

**Mute**, **Deafen**, **Share screen**, and **Leave voice** appear only after joining
voice. You can join one voice channel at a time across all communities. Select
another voice channel and choose **Switch to this channel** to move the call.

Choose microphone and output devices in **Connection & saving** before joining.
Allow microphone access when the operating system asks. On macOS, enable
microphone access for disquisition in System Settings > Privacy & Security.

Choose **Share screen** while in voice to select a screen or window. Other peers
in that voice channel choose which streams to watch. macOS also requires Screen
Recording permission. Use **Stop sharing** to end the stream or **Leave voice**
to stop the call and share together.

## Manage a community

Owners and admins open **Manage**. Categories run down the left side, with their
settings on the right:

- Community name and icon
- Text and voice channels, including names and topics
- Members and administrator roles
- Invite revocation
- Host priority

Changes save with the action button in each category. The owner assigns admins;
any member can host without gaining administrative privileges.

## Connection problems

If a desktop client's direct address is entered as a relay, the app explains
that the address belongs to a client. Enter the relay's hostname instead, or
clear Relay for direct mode. Repeated retries do not repeat the same error.
A relay or client running an older protocol must be updated before it can
connect to communities. The app keeps retrying known peers automatically.
