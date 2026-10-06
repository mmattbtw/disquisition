# Disquisition for Wii U

A native PowerPC Wii U app built with devkitPPC and wut. It uses Nintendo's
OSScreen, VPAD, mic, sndcore2/AX, and ProcUI services directly. It produces an
Aroma `.wuhb` and a Homebrew Launcher `.rpx`, with no browser or PC companion.

Chat appears on both the TV and GamePad. The GamePad has Chat, Members, and
Settings tabs, a touch/controller keyboard, assigned names, message colors,
scrolling, microphone voice, mute, deafen, and push-to-talk. A network worker
handles DNS and nonblocking sockets independently of rendering and audio.

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

## Connect

Enable the console's internet connection and turn on the GamePad. Fresh installs
default to `relay.mmatt.net:3333` with requested name `wiiu`. The app waits for
you to choose Join; it does not automatically connect on launch.

Open Settings to change the relay host, TCP port, requested name, or message
color. Tap a field, or select it with the D-pad and press A. Changes save to SD
when you finish editing. Choose Save & join relay, or press + to join. Choose a
different requested name for each client on the same relay, since the relay can
reattach an existing session when the same requested name reconnects.

Use a Disquisition **relay**, usually port `3333`. The central server port,
usually `9000`, does not deliver live peer chat to a console-only connection.
This app never falls back to direct connections. For a LAN relay, run:

```sh
./build/server --port 9000 --db chat.db
./build/relay --host 127.0.0.1 --port 9000 --listen 3333 --advertise 192.168.1.20
```

Replace `192.168.1.20` with the computer's LAN IPv4 address and enter it in Wii U
Settings. Allow inbound TCP 3333 on that computer. Desktop, terminal, and 3DS
participants can join the same room.

## Controls

| Control | Action |
| --- | --- |
| Touch tabs or L/R | Switch Chat, Members, Settings |
| A / touch composer | Open message keyboard, or join while disconnected |
| + | Join, disconnect, or cancel reconnect attempts |
| D-pad up/down | Scroll messages/members, or select a settings field |
| B | Return to newest messages and dismiss the notice |
| Y | Join/leave voice |
| X | Mute/unmute microphone |
| ZL | Deafen/undeafen, also blocking microphone transmission |
| Hold ZR | Talk when push-to-talk is selected |
| - | Disconnect and exit |
| HOME | System menu or Homebrew Launcher exit, depending on environment |

In the editor, tap keys or use the D-pad and A. Y toggles Shift; X deletes;
L/R move the insertion cursor; + or Done accepts; B cancels and retains a
message draft. The keyboard covers all printable ASCII characters, including
spaces and punctuation. Messages are limited to 2,000 bytes and requested
names to 20 bytes. The native OSScreen font cannot display Unicode; received
non-ASCII characters appear as `?`. Incoming message bytes remain intact in
the session. There is no emoji picker or external-keyboard support.

The TV keeps showing chat while the GamePad is editing or displaying members
or settings. A color stripe marks each sender's chosen color. The transcript
keeps up to 128 messages in arrival order, so a console clock difference does
not move a new message into old chat. Sending returns to the newest messages.
The member list keeps up to 64 users and reports text/voice, speaking, muted,
and deafened states.

## Voice

Choose Join voice to enable the GamePad microphone and GamePad/headphone audio
output. Voice stays off until you join it. Push-to-talk is on by default. Set
Microphone to open mic in Settings if preferred; that setting can change while
connected. Headphones reduce speaker feedback. The client adds no software echo
cancellation.

The app checks the microphone's reported 32 kHz sample rate, averages adjacent
samples, and sends 320 signed 16-bit samples per 20 ms frame. Wire samples are
explicitly little-endian, even though the Wii U CPU is big-endian. The relay
format remains 16 kHz mono / 640 bytes, using voice-port marker `65535`, matching
the desktop and 3DS clients. AX resamples incoming audio to its renderer rate.
Up to eight users mix into one voice with bounded per-speaker queues. Playback
goes to the GamePad speakers or its headphone jack, rather than the TV.

Opening the editor, losing GamePad input, muting, or deafening immediately gates
microphone transmission. Closing the editor discards captured samples from
that interval. On HOME/foreground release or disconnection, hardware capture
and playback stop. Returning to the app reconnects text if it was connected,
but you must join voice again. A short AX watchdog stops playback when the UI
misses servicing its ring, preventing a stale speech fragment from looping.

An audio-service failure leaves text chat usable and reports an error. If voice
does not initialize, check GamePad connectivity and record the displayed service
code. Reconnects happen every three seconds. Periodic server requests detect an
upstream relay that stopped responding, even when its TCP socket remains open.
Voice frames drop under congestion to keep text/control traffic moving.

## SD settings

The app mounts the SD card with libwhb and uses wut's native filesystem wrappers.
Settings live at:

```text
sd:/wiiu/apps/disquisition/settings.cfg
```

You can also edit the file on a computer:

```ini
host=relay.mmatt.net
port=3333
name=wiiu
color=mint
push_to_talk=1
```

Saving writes and flushes a temporary file before replacement and keeps the
previous settings as `settings.cfg.bak`. If the main file is missing or cannot
be read, the backup is tried. Missing SD access does not block joining with
in-memory settings; the footer reports that changes could not be saved.
Text history is retained only for the running app. The server does not store or
replay chat. Messages have no acknowledgments, so a send interrupted by a
disconnect can be lost; the app does not automatically resend it.

## Build

Install devkitPro's packaged `wiiu-dev` group, including devkitPPC, wut, and Wii U
tools, following [wut's installation instructions](https://github.com/devkitPro/wut#install).
Then run:

```sh
make -C src/wiiu -j4
```

Or use the exact multi-platform image pinned in CI:

```sh
docker run --rm -v "$PWD:/work" -w /work/src/wiiu \
  devkitpro/devkitppc@sha256:44cb1a920e1ec3ec7c06767493c3b85f8d643d6137cc4661f0201895ac6e4967 \
  make -j4
```

The output is `src/wiiu/disquisition.wuhb` and `src/wiiu/disquisition.rpx`.
The [Wii U workflow](../../.github/workflows/wiiu.yml) builds both and uploads
separate SD-card packages. Native builds do not require Qt, FFmpeg, SQLite, or
the desktop/server toolchain. The platform-independent session comes from the
parent 3DS branch; its source is compiled into the Wii U binary without 3DS APIs.

Host verification uses the repository's normal commands:

```sh
cmake -S . -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The Wii U tests cover byte order, capture pairing/reset, mute/PTT/deafen,
reconnect voice state, message order, settings backup, keyboard input, and long
text wrapping. The end-to-end test connects the real console session/transport
to the real server and relay for bidirectional chat and voice frames.

## Hardware validation

The RPX and WUHB compile and link with devkitPPC r50 and wut 1.9.1. Actual Wii U
operation has not been verified here. A native build and host protocol tests do
not establish microphone timing, speaker behavior, touchscreen calibration, or
foreground lifecycle behavior on a console.

Before calling the build stable, test Aroma and RPX launch, saving/loading after
restart, TV and GamePad layout, long messages and all keyboard layers, scrolling,
multiple speakers, mute/deafen/PTT, editing during voice, GamePad disconnect,
HOME/resume, relay/server restarts, and Wi-Fi loss/recovery. Use a current relay
and server from this repository. The protocol is plaintext and unauthenticated,
so use a trusted server, relay, and network.

## Development references

- [WiiUBrew homebrew development guide](https://wiiubrew.org/wiki/Homebrew_development_guide)
  explains the native wut/RPX toolchain.
- [ProgrammingOnTheU](https://github.com/yawut/ProgrammingOnTheU) covers wut,
  OSScreen, GamePad input, and lifecycle foundations.
- [wut headers and samples](https://github.com/devkitPro/wut) supply the current
  native API declarations and RPX/WUHB build rules.
- [Cemu's microphone implementation](https://github.com/cemu-project/Cemu/blob/main/src/Cafe/OS/libs/mic/mic.cpp)
  documents microphone state IDs and sample-ring interpretation used here.
