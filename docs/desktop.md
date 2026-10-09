# Desktop app

Download the app from the [latest release](https://github.com/mmattbtw/disquisition/releases/latest).
For a local build, see [building and packaging](building.md#desktop-app).

## Install

The downloads include a bundled `baresip` executable with the voice modules the
app uses. The macOS DMG and Windows archive include Qt. The Linux archive requires
a compatible Qt 6.8 runtime.

On macOS, open the `.dmg` and drag `disquisition` to the Applications folder
in the installer window. Then open `disquisition` from Applications and eject
the disk image.

## Join text chat

Open the app, enter a name and choose Join.

Fresh installs default to server `relay.mmatt.net:9000` and relay
`relay.mmatt.net:3333`, so they connect through the relay without changing
Preferences. Saved connection settings take precedence over these defaults.
For your own server, set the server address and port in Preferences. Use Cmd+,
on macOS to open it. Set the relay address, or leave it blank to connect directly.

## Voice chat

Voice stays off until you choose Join voice.
Choose Leave voice to exit the call without leaving the server. Use a different
voice SIP port in Preferences for each client running on the same machine. If
peers cannot directly reach the address seen by the server, enter a reachable
DNS name or IP as the public host and forward both the automatically chosen TCP
chat port and the chosen SIP/RTP ports when connecting directly.
For clients on the same LAN, select "Advertise local IP automatically" in
Preferences to announce the local IPv4 address. This takes precedence over the
saved public host while selected. It also applies during direct relay fallback.

## Relay and direct fallback

To keep your IP hidden from other users, keep a relay address and port in
Preferences. The desktop app then connects only to the relay;
it does not open a peer listener or a SIP/RTP socket. Relayed voice uses 16 kHz
mono PCM frames over that TCP connection. The relay passes those frames to the
chat server, which fans them out to the room. The desktop client converts
between the relay format and each audio device's preferred format, including
44.1 kHz and 48 kHz devices. Direct participants keep using baresip with other
direct participants and send a second audio stream for
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

## Voice details

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

Direct voice works best on a LAN or between publicly reachable hosts. The app
does not coordinate ICE/TURN credentials or RTP port forwarding, and it has no
authentication or media encryption. An internet deployment would need TURN and
DTLS-SRTP before treating calls as private.

## Screen sharing

Choose share screen after joining, then pick what to stream:
one application window from the Applications tab, or a whole display from
the Screens tab. Choose Go Live to start. Sharing a single window keeps the
rest of your desktop private, and lets you watch other shares fullscreen
without the stream capturing itself.
Shares appear on a stage above the chat, one tile per person sharing, and the
member list marks each sharer `LIVE`. Your own tile shows what viewers
receive. Other people's shares start as a "watch stream" button, so nobody
downloads video they did not ask for. Choose it, or double-click the sharer in
the member list, to start watching, and stop watching to close the video. Choose fullscreen
or double-click a video to fill the screen with it; press Esc or double-click
again to return. With more than one share on the stage, choose focus to
enlarge one above the others.

The app encodes H.264 at up to 1920×1080 and 30 fps, about 5 Mbps, on the GPU
when it can. Frames travel over the existing server connection, directly or
through the relay, and the server forwards each share only to its viewers.
Like relayed voice, video is not encrypted, so the relay and server can see
it. A viewer who falls behind skips frames and resumes at the next keyframe,
which arrives every two seconds. New viewers wait for one the same way.
Screen sharing needs a server and relay built from this version; older servers
disconnect clients that start a share.

On macOS, allow Disquisition in System Settings > Privacy & Security > Screen
& System Audio Recording, then quit and reopen the app. If it keeps asking
despite an enabled toggle, quit the app, remove the old Disquisition entry
with the minus button, add the current app bundle, and reopen it. An old
permission can refer to a previous ad-hoc build's signature. Keep using the
same certificate-backed signing identity to prevent this after rebuilds.

See [macOS signing](building.md#macos-signing) for keeping permission grants
across local rebuilds.

## Save messages locally

In Preferences, check "Save chat messages locally," choose a SQLite file and
optionally set a maximum message count. You can also use `/save <path.db>` or
`/save off`. See [local message history](command-line.md#local-message-history)
for storage and retention details.

## Release builds

Every push to `main` publishes a [desktop release](https://github.com/mmattbtw/disquisition/releases)
after the macOS, Windows, and Linux builds succeed. Each release has a
`main-<workflow run number>` tag, points to the exact commit built, and includes
download links in its release notes. Pull request and manually triggered builds
remain available in the [desktop workflow](../.github/workflows/desktop.yml) artifacts.

The [latest release page](https://github.com/mmattbtw/disquisition/releases/latest)
shows the commit and all downloads. The links become available after the first
successful release. Failed builds leave the previous release available. An
older build finishing later does not replace a newer release as latest.
