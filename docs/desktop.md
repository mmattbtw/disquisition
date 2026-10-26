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

Each time you open the app, it picks a random default message color from the
presets: pink, mint, butter, periwinkle, lilac, aqua, or peach. Use `/color <value>`
to change it. Your color stays the same when you reconnect.

Fresh installs default to server `server.disquisition.app:9000` and relay
`relay.disquisition.app:3333`, so they connect through the relay without changing
Preferences. Saved connection settings take precedence over these defaults.
For your own server, set the server address and port in Preferences. Use Cmd+,
on macOS to open it. Set the relay address, or leave it blank to connect directly.

## Desktop penguins

Desktop penguins are off and hidden from Preferences and the menu until you
type `/waddleon` in chat. This enables them and permanently unlocks their
controls on your device. After unlocking, use **Desktop penguins** in the
disquisition menu or Preferences to turn them on or off. Once enabled, joining
the server puts a little Club Penguin character for you and every connected
member along the bottom of each display. They wander above other
applications, including when the chat window is minimized. Names appear below
the penguins. Their colors match their text chat colors, including named shades
and xterm-256 colors. Your `/color` command updates your penguin immediately;
other people's colors update when they send a text or image message. Until
their color is known, their penguin uses the default mint. Positions are local
to each desktop.

New text messages appear in speech bubbles above their sender for 5 to 12 seconds.
Penguins stop to speak, and a new message replaces that person's current bubble.
Long messages show a shortened preview; the full message stays in chat. Saved
history and system notices do not create bubbles. Leaving or losing the server
connection clears the overlay until you reconnect.

Penguins disappear after 10 minutes without a text or image message and return
when that person sends another message. New arrivals get the same grace period.
In Preferences, set **hide inactive penguins after** to a different number of
minutes, or **Never** to keep everyone visible. Voice activity, saved history,
and color changes do not reset the timeout. Hiding and reopening the overlay
does not reset it either.

The overlay lets mouse clicks and keyboard input pass through to your apps.
Toggle **Desktop penguins** in the disquisition menu, or use Preferences.
Uncheck **Let penguins wander** to keep characters still while retaining chat
bubbles. macOS Reduce Motion and Windows animation settings also stop wandering.
These choices are saved between launches. The sprites are included in the app.

On macOS, the overlay joins Spaces and can appear over fullscreen applications.
On Linux, always-on-top behavior depends on the window manager; Wayland
compositors may restrict overlays above other applications.

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

To annotate a share, the person sharing chooses **allow drawing** on their
tile. Drawing starts off for every new share. Everyone watching can then
choose **pen** and drag over the video. The default pen color matches the
person's chat color; the color menu offers other colors for new strokes.
A username tag follows each active drawer's latest endpoint, then disappears
after they stop. Drawings stay aligned when viewers resize the video or open
it fullscreen, and people who start watching later receive the current ink.

Choose **eraser** and drag across a line to erase its whole stroke. A stroke
is one continuous mouse drag. Viewers can erase their own strokes; the sharer
can erase anyone's. **Clear mine** removes only that person's drawings.
The sharer also has **select**: click a line or drag a box across strokes,
then choose **delete selected** or press Delete. Selected strokes are
highlighted and show their authors' names. **Clear all** removes everyone's
ink. Press Esc to leave any drawing or selection tool.

While sharing, right-click a person in the chat sidebar and choose
**Ban from drawing on my stream** to block their pen and eraser. The menu
changes to **Unban from drawing on my stream** to restore access. A banned
viewer can still watch and see other people's ink, and their existing ink
stays until it is erased. Bans apply to that chat name for the current stream,
survive reconnects and clearing or toggling drawing, and end when the stream
stops. The presenting client enforces them, so hiding the viewer's controls
is not the only protection.

Only the sharer can turn off drawing. Turning it off, ending the share or
losing the connection clears the ink. The drawing also clears automatically
after 4,096 accepted pen segments, including erased segments, to bound memory.

On macOS, Windows and X11, a transparent overlay shows the same drawings
over the actual shared display or application window. A small toolbar lets
the sharer draw, erase, select strokes, choose a color and clear drawings
without returning to chat. With drawing tools off, mouse input passes
through to the shared application. Press Esc to use the mouse normally.
For a window share, the overlay follows that window and hides while it is
minimized. Annotations refer to positions in the shared image and stay in
place when its contents scroll or change.

Use **undo**, Cmd+Z on macOS, or Ctrl+Z on Windows and Linux to remove your
most recent pen stroke. Undo removes the complete stroke for everyone watching
and leaves other people's drawings alone. Undo in a color text field edits the
text instead.

Type a six-digit hex color, such as `#12ABCD`, directly into the color control.
The `#` is optional. Choose **pick color...** from its dropdown for the full
color picker, including its HTML hex field. Presets and **chat color** remain
available. Custom colors apply to new pen strokes and the laser pointer.

Choose **laser pointer** to point by moving the mouse without holding a button.
Everyone watching sees a dot with a short fading trail in your chosen pen color,
with your name above it. The dot stays visible while you point at one spot.
Move outside the shared image or press Esc to hide it. Laser trails disappear
automatically, leave existing ink alone, and are not replayed to new viewers.
Laser pointers follow the same drawing permissions and individual bans as pens.

Wayland does not support this presenter overlay. If a window cannot be
identified uniquely on Windows or X11, or XCB support was unavailable at
build time, the sharer draws in their video preview instead. The toolbar
in the tile indicates this. The video preview and fullscreen annotations
still work for all participants. macOS display capture leaves out
Disquisition's own windows so the overlay is not captured a second time.
Windows excludes the overlay using capture exclusion on Windows 10 version
2004 and newer. X11 display capture can include the overlay in the video.

The app encodes H.264 at up to 1920×1080 and 30 fps, about 5 Mbps, on the GPU
when it can. Frames travel over the existing server connection, directly or
through the relay, and the server forwards each share only to its viewers.
Like relayed voice, video is not encrypted, so the relay and server can see
it. A viewer who falls behind skips frames and resumes at the next keyframe,
which arrives every two seconds. New viewers wait for one the same way.
Screen sharing needs a server and relay built from this version; older servers
disconnect clients that start a share.
Drawing needs updated desktop clients and, for relayed participants, a
relay with the generic peer-data channel. Erasing, selection, chat colors and laser pointers
need updated desktop clients on both ends; they require no further server
or relay changes. A server that already supports screen sharing needs no
drawing update. The sharing client owns drawing permissions and history,
validates incoming strokes and sends the current drawing to new viewers.
Choosing allow drawing takes effect locally immediately. Annotations travel
over direct peer connections or the relay's generic peer-data channel,
separately from video. The discovery server receives no annotation events
and stores no drawing state. Peer data is not encrypted. Drawing history
is kept only in the sharing client's memory for the current share.

If the sharing client or relay does not support drawing, the viewer shows
"drawing unavailable: update both apps and the relay" instead of silently
waiting for permission. Clients resubscribe and receive a fresh drawing
snapshot when their peer link reconnects.

On macOS, allow the app's channel name, such as `disquisition development`,
in System Settings > Privacy & Security > Screen
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
