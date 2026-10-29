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
on port 3333. Once your display name is saved, the app connects automatically
at startup using your saved connection settings. Under **User Settings**, enter a
relay hostname, optionally followed by a port. No `relay://` prefix is needed.
Saved settings take precedence; clear the relay field to use direct connections
and advertise a LAN address.

Select **Create** for a new community, or **Join** to paste an invite or open its
QR image. **Invite** lets owners and admins copy an invite link or save a QR code.
While an invite is being resolved, the sidebar shows a **Joining community**
progress indicator; it disappears when the host admits you or rejects the invite.
The relay forwards encrypted peer traffic. Community metadata lives on member
devices, and community connections do not need the discovery server.

Select a text channel to chat. Images can be attached with **Image** or
dropped into the chat. GIF search is available in builds configured with a search
provider key. Messages are live-only. Optional local SQLite saving records only
messages this device sends or receives.

## Your profile and settings

Open **User Settings** to edit your profile. **User** contains your display name,
profile picture, and bio. Pictures are cropped to a square and resized before
sharing; bios can contain up to 500 characters. Save applies your profile to all
communities on this identity, including while disconnected. Changes reach peers
when you connect. Cancel discards the dialog's edits.

Click a person's name or avatar in chat, the member list, or a voice channel to
open their profile card. Profiles show the display name, picture, and bio. A
person without a picture gets an initial avatar. Existing saved messages that
predate sender identities retain their original names.

The settings categories run down the left, with the selected page on the right.
**Audio** contains devices and push-to-talk, **Notifications** contains message
and activity alerts, **Connection** contains relay and direct addresses, and
**Message History** controls local SQLite saving. Microphone and output devices
can be changed during a call; audio reconnects in place without dropping you
from the call. Disconnect before changing connection details.

## Desktop penguins

Desktop penguins are off and hidden from User Settings and the menu until you
type `/waddleon` in chat. This enables them and permanently unlocks their
controls on your device. After unlocking, use **Desktop penguins** in the
disquisition menu or User Settings to turn them on or off. Once enabled, connecting
to a community puts a little Club Penguin character for you and every connected
member of the selected community along the bottom of each display. They wander above other
applications, including when the chat window is minimized. Names appear below
the penguins. Their colors match their text chat colors, including named shades
and xterm-256 colors. Your `/color` command updates your penguin immediately;
other people's colors update when they send a text or image message. Until
their color is known, their penguin uses the default mint. Positions are local
to each desktop.

New text messages appear in speech bubbles above their sender for 5 to 12 seconds.
Penguins stop to speak, and a new message replaces that person's current bubble.
Long messages show a shortened preview; the full message stays in chat. Saved
history and system notices do not create bubbles. Leaving or losing the community
connection clears the overlay until you reconnect.

Penguins disappear after 10 minutes without a text or image message and return
when that person sends another message. New arrivals get the same grace period.
In User Settings, set **hide inactive penguins after** to a different number of
minutes, or **Never** to keep everyone visible. Voice activity, saved history,
and color changes do not reset the timeout. Hiding and reopening the overlay
does not reset it either.

The overlay lets mouse clicks and keyboard input pass through to your apps.
Toggle **Desktop penguins** in the disquisition menu, or use User Settings.
Uncheck **Let penguins wander** to keep characters still while retaining chat
bubbles. macOS Reduce Motion and Windows animation settings also stop wandering.
These choices are saved between launches. The sprites are included in the app.

After unlocking, the **custom sprite** dropdown in User Settings lets you choose
**Custom images** and upload two images: **Standing**, facing forward, and
**Walking**, facing left. The walking image flips when moving right. Both images
keep their original colors and represent you on other members' desktops. Other members
use Club Penguin by default, or their own uploaded sprites. Your images are sent
over authenticated, encrypted community links to current members and new arrivals
while you are connected. Transparent PNGs work best; JPEG, WebP and GIF are
also accepted, using the first frame of animated images. Choose files under
10 MB and no larger than 4096 by 4096 pixels. The app saves resized copies when
you save User Settings, so you can move or delete the originals. Cancel discards
changes. Choose **Off (Club Penguin)** to restore the bundled penguins without
losing your uploaded images.

Sprite synchronization uses community peer connections and needs updated desktop
clients. Sprites belong to each authenticated peer identity, including when members
share a display name. Saved images are announced to new peers and again after
reconnecting. Turning custom sprites off restores your Club Penguin sprite for
everyone. Switching communities shows that community's online members.

On macOS, the overlay joins Spaces and can appear over fullscreen applications.
On Linux, always-on-top behavior depends on the window manager; Wayland
compositors may restrict overlays above other applications.

## Chat colors and linked media

Each app session starts with a random pastel chat color. Use `/color pink`,
`/color mint`, `/color butter`, `/color periwinkle`, `/color lilac`, `/color aqua`,
or `/color peach` to change it. Numeric xterm colors from 0 to 255 also work,
except reserved colors 1, 2 and 250. Your name uses the chosen color in chat and
the member list; message text keeps its normal color. Messages carry the color to peers and local history, and the
drawing tool's **chat color** follows it.

In **User Settings > Message History**, turn **Show linked GIFs and images in
chat** off to stop loading linked media automatically. Uploaded images still
appear in chat. The setting is saved on this device.

## Voice and screen sharing

Select a voice channel, then choose **Join voice** beneath its name. The current
text channel stays open. Participants and their mute, deafen, and sharing status
appear beneath the voice channel rather than in the member list. A participant's
row lights up green while their microphone is active.

**Mute**, **Deafen**, **Share screen**, and **Leave voice** appear only after joining
voice. You can join one voice channel at a time across all communities. Select
another voice channel and choose **Switch to this channel** to move the call.

Choose microphone and output devices in **User Settings** before joining, or
change them during a call to reconnect audio in place.
If the selected devices fail, the call falls back to the previous or default
devices. If none can start, you leave voice and receive an error.
Allow microphone access when the operating system asks. On macOS, enable
microphone access for disquisition in System Settings > Privacy & Security.

Choose **Share screen** while in voice to select a screen or window. Other peers
in that voice channel choose which streams to watch. macOS also requires Screen
Recording permission. Use **Stop sharing** to end the stream or **Leave voice**
to stop the call and share together. On macOS, full-screen shares include
disquisition's own windows and the app is also listed as a window source;
drawing and annotation overlays stay out of the capture.

Profile and community metadata updates keep the current call connected.

Stream invitations and fullscreen titles use display names. Watching requests a
fresh keyframe, including when the shared screen is static. If the first frame
or subscription is missed, the viewer retries automatically.

## Leave or delete a community

Any member other than the owner can right-click the community in the sidebar
and choose **Leave community**. This removes your membership;
you need a new invite to rejoin. The owner can open **Manage > Community** and
choose **Delete community** to permanently delete it for all members.
Both actions ask for confirmation and work even while peers are synchronizing.
Members who were offline learn about the change when they reconnect. Saved local message files are kept.

Disconnecting keeps your memberships and owner or admin permissions. Owners and
admins can manage saved community metadata while offline; updates synchronize
when they reconnect. Metadata updates keep established peer and voice connections
active.

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

Owners and admins can drag a participant's name or avatar from one voice channel
to another within the same community. The participant stays connected to audio
with their mute and deafen settings. Screen sharing and watched streams from the
previous channel stop. Members who are not in voice cannot be dragged into a call.

## Connection problems

If a desktop client's direct address is entered as a relay, the app explains
that the address belongs to a client. Enter the relay's hostname instead, or
clear Relay for direct mode. Repeated retries do not repeat the same error.
A relay or client running an older protocol must be updated before it can
connect to communities. The app keeps retrying known peers automatically.
A lost relay connection retries automatically without pressing Connect again.
Voice membership is retained, and watched streams resume when the peer returns.

## Facecam

After joining a voice channel, choose **Turn camera on**. Allow camera access
when prompted, then choose a device if you have more than one. Your camera
replaces your picture in your tile in the call. People in the same voice
channel with an updated app see your camera in your tile automatically.

Camera and screen sharing work simultaneously. **Turn camera off** stops only
your camera; **Stop sharing** stops only your screen share. Leaving voice or
being moved to another channel turns the camera off. It stays off until you
turn it on again. Mute and deafen affect sound, so use **Turn camera off** to
stop sending video.

Camera video uses the same encrypted peer connections as screen sharing,
at up to 1280×720 and 30 fps. On macOS, camera access is under System Settings
> Privacy & Security > Camera for the app's channel name.

## Screen sharing

Choose share screen after joining, then pick what to stream:
one application window from the Applications tab, or a whole display from
the Screens tab. Choose Go Live to start. Sharing a single window keeps the
rest of your desktop private, and lets you watch other shares fullscreen
without the stream capturing itself.
Shares appear on a stage above the chat, one tile per person sharing, and the
voice channel lists each participant's sharing status. Your own tile shows what viewers
receive. Other people's shares start as a "watch stream" button, so nobody
downloads video they did not ask for. Choose it to start watching, and stop watching to close the video. Choose fullscreen
or double-click a video to fill the screen with it; press Esc or double-click
again to return. With more than one share on the stage, choose focus to
enlarge one above the others.

**Share audio**, on by default in the picker, sends the share's sound with
the picture. A window share sends only that application's sound. A screen
share sends everything the computer plays except Disquisition itself, so
voice chat is never sent back to the people in it. Viewers hear a share only
while watching it, through the output device chosen in User Settings, and
deafening silences shares as well as voice. Right-click a stream you are
watching, in its tile or fullscreen, to set its volume without changing voice
chat. The app remembers it for each person who shares. Sharing sound works on macOS, on
Windows 10 version 2004 and newer, and on Linux with PulseAudio or PipeWire.
A window share goes out without sound when its application cannot be
identified, such as on Wayland or when two windows share a title; the chat
says when a share has no sound and why.

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
stays until it is erased. Bans apply to that member's authenticated identity for the current stream,
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
when it can. Video, sound and drawing travel through authenticated, encrypted
community peer links, directly or through a relay. Only peers in the same voice
channel can exchange drawing events. The sharing client owns permissions and
drawing history, validates incoming strokes, and sends the current ink to new
viewers. Drawing history stays in memory for the current share. A viewer who
falls behind skips video frames and resumes at the next keyframe, which arrives
every two seconds. Sound is 128 kbps Opus. Video goes to each connection a chunk
at a time, so sound and voice are not held up behind a large frame, and sound
carries on when the picture skips.

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

In **User Settings**, enable local saving, choose a SQLite file, and
optionally set a maximum message count. History stays on this device and is
separate for each community and text channel.

Local saving is off by default. Leaving the maximum blank keeps every saved
message. The app loads the 1,000 most recent saved messages for the selected
channel; the database can retain more than the UI displays.

The SQLite `saved_messages` table stores `timestamp` in Unix seconds, `sender`,
`body`, and `color`, with `community_id`, `channel_id`, and `message_id` identifying
the message's scope. Saved history stays on this device and is not replayed to
other members.

## Notifications and mentions

Desktop notifications, unread badges, and taskbar alerts apply to incoming
community messages while the app is in the background. Type `@` to complete a
member name. `@everyone` and `@here` mention the community. Mentions are highlighted
in chat. Settings offer all messages or mentions only, suppression of group
mentions, and join/share activity notifications. Use **Mute notifications** in the
app menu or `/dnd` to toggle notification muting.

## Chat commands

Type `/help` for the command list and `/color` for the available color names and
your current choice. `/users` lists online community members. `/voice` joins the
selected voice channel; `/leavevoice` leaves the call. `/mute`, `/unmute`,
`/deafen`, and `/undeafen` control your audio. `/gif [search]` opens the GIF picker,
`/save` opens User Settings, `/dnd` toggles notifications, and `/clear` clears the
current channel's local display without deleting saved messages. `/leave` asks to
leave the current community. `/quit` and `/exit` close the app.

Commands run locally and are not sent as chat. Mentions highlight `@name`,
`@everyone`, and `@here`; a message mentioning you also has a highlighted background.
A standalone GIF link shows the GIF without its URL while linked previews are
enabled. Turning previews off keeps the URL visible.

## Push to talk

In **User Settings**, enable **Hold a key to talk** and choose a key.
Voice transmits only while that key is held. Manual mute and deafen still take
precedence. Windows and X11 support global keys. On macOS, allow Input Monitoring
and restart the app to use the key while another application is focused.
Wayland supports the key while Disquisition is focused. Leaving or switching
voice channels releases the key and stops the previous voice session.

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
