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
on port 3333. Under **User Settings**, enter a relay hostname, optionally
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
**Message History** controls local SQLite saving. Leave voice before changing
audio devices, and disconnect before changing connection details.

## Voice and screen sharing

Select a voice channel, then choose **Join voice** beneath its name. The current
text channel stays open. Participants and their mute, deafen, and sharing status
appear beneath the voice channel rather than in the member list.

**Mute**, **Deafen**, **Share screen**, and **Leave voice** appear only after joining
voice. You can join one voice channel at a time across all communities. Select
another voice channel and choose **Switch to this channel** to move the call.

Choose microphone and output devices in **User Settings** before joining.
Allow microphone access when the operating system asks. On macOS, enable
microphone access for disquisition in System Settings > Privacy & Security.

Choose **Share screen** while in voice to select a screen or window. Other peers
in that voice channel choose which streams to watch. macOS also requires Screen
Recording permission. Use **Stop sharing** to end the stream or **Leave voice**
to stop the call and share together.

Stream invitations and fullscreen titles use display names. Watching requests a
fresh keyframe, including when the shared screen is static. If the first frame
or subscription is missed, the viewer retries automatically.

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
A lost relay connection retries automatically without pressing Connect again.
Voice membership is retained, and watched streams resume when the peer returns.

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
when it can. Video and drawing travel through authenticated, encrypted community
peer links, directly or through a relay. Only peers in the same voice channel can
exchange drawing events. The sharing client owns permissions and drawing history,
validates incoming strokes, and sends the current ink to new viewers. Drawing
history stays in memory for the current share. A viewer who falls behind skips
video frames and resumes at the next keyframe, which arrives every two seconds.

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
