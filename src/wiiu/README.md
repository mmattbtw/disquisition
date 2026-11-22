# Disquisition for Wii U

A native PowerPC Wii U app built with devkitPPC and wut. It joins Disquisition
communities as a member, with text channels on the TV and GamePad and voice
channels through the GamePad microphone. It uses Nintendo's OSScreen, VPAD,
mic, sndcore2/AX, and ProcUI services directly, and produces an Aroma `.wuhb`
and a Homebrew Launcher `.rpx`, with no browser or PC companion.

The community client is shared with the 3DS app (`src/console`). It speaks the
desktop's community protocol: mutual TLS between members, signed community
metadata, and invite links. A worker thread runs it, separately from rendering
and audio.

## Install

For Aroma, copy the contents of `disquisition-wiiu-aroma` to the SD card:

```text
sd:/wiiu/apps/disquisition/disquisition.wuhb
```

Launch Disquisition from the Wii U Menu. For an RPX-compatible Homebrew
Launcher environment, use `disquisition-wiiu-hbl` instead:

```text
sd:/wiiu/apps/disquisition/disquisition.rpx
sd:/wiiu/apps/disquisition/meta.xml
```

Use the package for your environment; there is no channel installer. No fonts,
DSP dump, voice codec, or other runtime assets need to be copied separately.
An SD card is required to connect, since it holds the app's community identity.

## Connect

Enable the console's internet connection and turn on the GamePad. Open
Settings to set your display name (1 to 64 characters), relay, and chat color,
then choose Connect or press +. Fresh installs use the relay
`relay.disquisition.app` on port 3333. Enter another relay as `hostname` or
`hostname:port`. Once settings are saved, the app connects at startup.

The Wii U always connects through a relay. It opens no listening port and has
no direct mode, so its communities need a relay that the other members can also
reach. To run one, see [Connect through the relay](../../docs/communities.md#connect-through-the-relay).
Name, relay, and color change while disconnected.

## Join a community

Create the community and its invite on the desktop app. Invite links look like
`disquisition://join/...` and are a few hundred characters long. On the
Communities tab, either:

- choose **Use invite.txt** after saving the link as the first line of
  `sd:/wiiu/apps/disquisition/invite.txt`, or
- choose **Type invite** and enter the link with the keyboard (up to 1024
  characters; use `invite.txt` for longer links, such as one listing several
  direct-mode members).

The community shows as `Joining...` until an online member admits you. Choose
**Leave** twice to leave the selected community or cancel a pending invite.

## Channels and chat

The Communities tab lists your communities on the left and the selected
community's channels on the right: `#` marks text channels and a speaker marks
voice channels, with the number of members in voice. `*` marks unread messages.
Choose a text channel to open it in Chat. Choose a voice channel to join or
leave its voice.

Chat appears on both the TV and the GamePad. Messages are live-only, as on the
desktop: the app shows what arrives while it is connected and downloads no
history. A message sent while no other member is online shows `(not delivered)`.
Images from desktop members appear as `[sent an image]`. The transcript keeps
128 messages per channel while the app runs, and messages are limited to 2,000
bytes. The Members tab lists the selected community's members, online first,
with their voice channel and whether they are muted, deafened, or speaking.

## Voice

The app initializes the Wii U audio renderer at startup to end the system's
loading sound. The renderer stays active for the app's foreground lifetime;
leaving voice or disconnecting releases microphone/playback resources without
shutting down the renderer. HOME/background release and exit shut it down, and
returning to the foreground initializes it again.

Join voice from a voice channel, or press Y to join the selected community's
first voice channel. Push-to-talk is on by default; set Microphone to open mic
in Settings if preferred. Headphones reduce speaker feedback. The client adds
no software echo cancellation.

The app checks the microphone's reported 32 kHz sample rate, averages adjacent
samples, and sends 320 signed 16-bit samples per 20 ms frame. Wire samples are
explicitly little-endian, even though the Wii U CPU is big-endian. This is the
desktop's 16 kHz mono voice format. AX resamples incoming audio to its renderer
rate. Up to eight speakers mix with bounded per-speaker queues. Playback goes to
the GamePad speakers or its headphone jack, rather than the TV.

Opening the keyboard or losing GamePad input stops transmission without
announcing a mute to the community. If the GamePad disconnects, or the app goes
to the background with HOME, voice stops and you leave the voice channel; the
community stays connected. Join voice again afterward. A short AX watchdog stops
playback when the UI misses servicing its ring, preventing a stale speech
fragment from looping. A voice failure leaves text chat usable and reports the
service error.

## Controls

| Control | Action |
| --- | --- |
| Touch tabs or L/R | Switch Communities, Chat, Members, Settings |
| D-pad | Move through lists, scroll messages/members, or select a setting |
| D-pad left/right | Switch between communities and channels |
| A / touch | Open a community or channel, write a message, or change a setting |
| B | Return to the community list, or to the newest messages |
| + | Connect or disconnect |
| Y | Join/leave voice |
| X | Mute/unmute microphone |
| ZL | Deafen/undeafen |
| Hold ZR | Talk when push-to-talk is selected |
| - | Exit, including from the keyboard |
| HOME | System menu or Homebrew Launcher exit, depending on environment |

In the keyboard, tap keys or use the D-pad and A. Y toggles Shift; X deletes;
L/R move the insertion cursor; + or Done accepts; B cancels and keeps a message
draft. The keyboard covers all printable ASCII characters. The bundled bitmap
font displays ASCII; received non-ASCII characters appear as `?`. There is no
emoji picker or USB keyboard support.

On Aroma, - requests the Wii U Menu and continues processing ProcUI messages
until the system completes the exit transition. Rendering and input stop as
soon as exit is requested. The legacy Homebrew Launcher path keeps libwhb's
launcher relaunch behavior. Microphone/playback, audio renderer, network worker,
and display buffers are cleaned up before the app returns.

## SD card files

```text
sd:/wiiu/apps/disquisition/settings.cfg     name, relay, color, push-to-talk
sd:/wiiu/apps/disquisition/invite.txt       optional invite link to join
sd:/wiiu/apps/disquisition/community/       identity and community metadata
```

`settings.cfg` can be edited on a computer:

```ini
name=wiiu
relay=relay.disquisition.app
color=mint
push_to_talk=1
```

Colors are the desktop's names (`pink`, `mint`, `butter`, `periwinkle`,
`lilac`, `aqua`, `peach`), `#RRGGBB`, or an xterm color number. Saving writes a
temporary file before replacement and keeps the previous settings as
`settings.cfg.bak`, which is used if the main file is missing.

`community/identity.key` is this console's member identity. Keep it with the
rest of the `community` folder when backing up or moving the SD card; without
it, the console has to be invited again. Community files are replaced the same
way as settings, so an interrupted write leaves the previous copy.

## Build

Install devkitPro's packaged `wiiu-dev` group and `wiiu-mbedtls`, following
[wut's installation instructions](https://github.com/devkitPro/wut#install).
Then run:

```sh
make -C src/wiiu -j4
```

Or use the exact multi-platform image pinned in CI, which includes mbedTLS 2.28.8:

```sh
docker run --rm -v "$PWD:/work" -w /work/src/wiiu \
  devkitpro/devkitppc@sha256:44cb1a920e1ec3ec7c06767493c3b85f8d643d6137cc4661f0201895ac6e4967 \
  make -j4
```

The output is `src/wiiu/disquisition.wuhb` and `src/wiiu/disquisition.rpx`.
The [Wii U workflow](../../.github/workflows/wiiu.yml) builds both and uploads
separate SD-card packages. Native builds do not require Qt, FFmpeg, SQLite, or
the desktop toolchain.

Host verification uses the repository's normal commands:

```sh
cmake -S . -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

`console_client_test` checks the shared community client against the desktop's
community node and relay. The Wii U tests cover byte order, capture pairing and
reset, settings validation and backup, `invite.txt` parsing, SD storage
replacement and recovery, keyboard input, desktop chat colors, and text
wrapping. Exit tests check that Aroma requests the menu once without stopping
ProcUI early, and preserve the legacy launcher path for every supported
launcher title ID. They do not emulate the system's menu transition.

Text and borders use one pixel grid. The app draws a bundled public-domain
[font8x8 font by Daniel Hepper](https://github.com/dhepper/font8x8) at 2x scale
through `OSScreenPutPixelEx`. It does not use `OSScreenPutFontEx`, whose native
positioning caused labels to miss their touch targets and the footer to clip.
The host preview runs the same layout and glyph renderer as the console. The
render test checks button-label containment, non-overlapping touch targets,
printable glyphs, and complete footer rendering. Generate pixel previews of
each GamePad screen and the TV with:

```sh
./build/wiiu_render_test build-wiiu/ui-preview
```

The output is portable pixmap images at the native display resolutions.

## Hardware validation

The RPX and WUHB compile and link with devkitPPC r50, wut 1.9.1, and mbedTLS
2.28.8. The community version has not yet run on a console. Earlier builds of
this app, using the same rendering, input, audio, and lifecycle code with the
retired room protocol, launched on Aroma and chatted live.

Before calling the build stable, test Aroma and RPX launch, connecting through
a relay, joining with both invite methods, restarting with saved communities,
TV and GamePad layout, long messages and all keyboard layers, multiple
speakers, mute/deafen/PTT, editing during voice, GamePad disconnect,
HOME/resume, relay restarts, and Wi-Fi loss/recovery. TLS handshakes and
signature checks are slower on the console than on a computer, so measure how
long joining and reconnecting take.

## Development references

- [WiiUBrew homebrew development guide](https://wiiubrew.org/wiki/Homebrew_development_guide)
  explains the native wut/RPX toolchain.
- [ProgrammingOnTheU](https://github.com/yawut/ProgrammingOnTheU) covers wut,
  OSScreen, GamePad input, and lifecycle foundations.
- [wut headers and samples](https://github.com/devkitPro/wut) supply the current
  native API declarations and RPX/WUHB build rules.
- [Cemu's microphone implementation](https://github.com/cemu-project/Cemu/blob/main/src/Cafe/OS/libs/mic/mic.cpp)
  documents microphone state IDs and sample-ring interpretation used here.
