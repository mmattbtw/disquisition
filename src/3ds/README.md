# Disquisition for Nintendo 3DS

A native Homebrew Launcher client for the same chat as the terminal and Qt
desktop apps. The top screen shows messages; the bottom screen has Chat,
Members, and Settings tabs. It uses libctru and Citro2D, with the system font
and software keyboard.

Text chat includes live send/receive, stored history, assigned names, a member
roster, and the desktop's message colors. Experimental voice includes microphone
capture, speaker/headphone playback, mixing for up to eight speakers, mute,
deafen, and push-to-talk. Voice is off until you choose Join voice.

## Install

Copy the contents of the `disquisition-3ds` build artifact to your SD card.
The resulting files should be:

```text
sdmc:/3ds/disquisition/disquisition.3dsx
sdmc:/3ds/disquisition/disquisition.smdh
```

Launch Disquisition from the Homebrew Launcher with Wi-Fi enabled. Open Settings,
tap Relay to enter your relay's DNS name or IPv4 address, then set the port and
name in the next row. Tap the left side of that row for the port, or its right
side for the name. Tap Save & join relay.

Use a relay, rather than the central server's port. Disquisition delivers live
text over its peer mesh, so a client connected only to the central server would
miss live chat. This client uses one outbound connection to the relay and never
falls back to a direct connection.

For example, on a computer reachable from the 3DS:

```sh
./build/server --port 9000 --db chat.db
./build/relay --host 127.0.0.1 --port 9000 --listen 3333 --advertise 192.168.1.20
```

Replace `192.168.1.20` with that computer's LAN IPv4 address. On the 3DS, set
Relay to `192.168.1.20` and Port to `3333`. Allow inbound TCP 3333 through the
computer's firewall. On desktop, use the same relay in Preferences, or join the
central server directly with a reachable peer address. Everyone shares one room.
Choose distinct requested names when using the same relay, whose current behavior
reattaches an existing session if another client requests the same name.

## Controls

| Control | Action |
| --- | --- |
| Touch tabs | Switch Chat, Members, or Settings |
| A in Chat | Join the relay, or open the message keyboard |
| Touch composer | Open keyboard, or join/cancel when disconnected |
| Y | Join/leave voice |
| Hold L | Talk while push-to-talk is selected |
| X | Mute/unmute microphone |
| Touch Deafen | Disable received audio and microphone transmission |
| D-pad up/down | Scroll chat or the member list |
| R | Return to newest messages |
| B | Return to Chat and dismiss the local notice |
| SELECT | Open Settings |
| START | Disconnect and exit to Homebrew Launcher |

The keyboard's OK button sends a message; Cancel leaves it unsent. The input
allows 500 UTF-16 code units, and the shared protocol caps the result at 2,000
UTF-8 bytes. Names use the protocol's 20-byte limit. Tap Color in Settings to
cycle the seven named colors. Leave the relay before changing its host, port,
name, or color. Push-to-talk can change while connected. Switching to open
microphone transmits immediately if you are in voice and neither muted nor
deafened.

Sending returns the transcript to the newest message. Live messages appear in
arrival order, so a different date or time on the console cannot insert your
new message into older chat. History loads above live messages, and repeated
history records do not move messages already displayed.

Settings are saved to `sdmc:/3ds/disquisition/settings.cfg`. The app never
automatically connects on launch. You can also edit this file on a computer:

```ini
host=192.168.1.20
port=3333
name=handheld
color=mint
push_to_talk=1
```

Settings use the native 3DS SD archive API. Saving checks folder creation,
file writes, flushing, and replacement, and keeps the previous file as
`settings.cfg.bak` for recovery. A save failure does not prevent joining with
the settings currently entered.

## Troubleshooting connection and settings

For `relay.mmatt.net`, use port `3333` for the relay. Port `9000` on that same
hostname is the central server and does not accept a relay client session.

The line below the title always shows the connection state. Local notices,
including settings failures, appear in the top screen's footer instead of
covering that state. Press B to dismiss a notice. SD errors identify the
operation and native result, such as `SD open card: 0x...` or
`SD write settings: 0x...`. The `3ds/disquisition` path on the card must be a
directory, with `disquisition.3dsx` inside it, rather than a file named
`disquisition`.

Network errors distinguish Wi-Fi being disconnected, DNS lookup, socket
creation, nonblocking setup, TCP connection, and send/receive. Socket failures
include the system error number in brackets. If a connection still fails,
record both that status line and any SD error in the footer. The worker retries
every three seconds; a save warning will not hide those attempts.

Older builds could report `TCP connect` with code `-26` while the connection
was actually completing. The 3DS socket service can leave its raw
connection-in-progress value in `SO_ERROR` after the socket becomes writable.
The client now checks for a connected peer, waits within the connection timeout
when there is no peer yet, and preserves actual connection errors. See the
[libctru report](https://github.com/devkitPro/libctru/issues/412).

## Voice

Dump the DSP firmware using Luma3DS's Rosalina menu: Miscellaneous options,
then Dump DSP firmware. The resulting `sdmc:/3ds/dspfirm.cdc` is needed for
NDSP playback. Chat still works if audio initialization fails; the app shows
the service's error code. This requirement comes from the
[devkitPro audio examples](https://github.com/devkitPro/3ds-examples/blob/master/audio/README.md).

The microphone captures signed 16-bit PCM at `MICU_SAMPLE_RATE_16360`, whose
documented clock is 16364.479 Hz. A streaming linear resampler converts it to
16 kHz. The client sends 320 samples per frame as 640 little-endian bytes,
matching `RelayAudio` in the desktop app. It announces voice port `65535`, the
existing marker for TCP audio, so desktop clients can exchange audio with it.
There is no SIP, RTP, codec, or separate gateway to install on the 3DS.

Received users have separate bounded queues and mix into one NDSP channel at
16 kHz. The mixer clips summed samples to signed 16-bit range. It handles up to
eight simultaneous speakers and keeps at most six frames per speaker. The
member list highlights names after measured audio exceeds the same speaking
threshold as the desktop. The 3DS speaker can feed back into its microphone;
headphones and the default push-to-talk mode help. There is no echo cancellation.

The app temporarily mutes microphone transmission while the system keyboard is
open. Closing the lid, leaving the application, or losing the connection stops
voice. After a reconnect you must choose Join voice again. The worker retries
the relay every three seconds, and requests history periodically to detect an
unresponsive upstream server. Voice frames drop under network congestion so
they cannot build up an unlimited delay. TCP voice can still stutter on poor Wi-Fi.

Use a current server and relay from this repository. Older versions may reject
voice messages. The existing Disquisition protocol has no encryption or
authentication; the relay and server can read chat and audio.

## Build

Install devkitPro's `3ds-dev` package group, including devkitARM, libctru,
Citro2D, Citro3D, and 3DS tools. Use the packaged libctru, rather than copying a
different source tree over it. Start with
[devkitPro's setup instructions](https://devkitpro.org/wiki/Getting_Started).

```sh
make -C src/3ds -j4
```

Or build using the same pinned devkitPro container as CI:

```sh
docker run --rm -v "$PWD:/work" -w /work/src/3ds \
  devkitpro/devkitarm:20260610 make -j4
```

The output is `src/3ds/disquisition.3dsx`, with an embedded SMDH and a
separate `.smdh` alongside it. The [3DS workflow](../../.github/workflows/3ds.yml)
builds and uploads an SD card folder on pushes and pull requests. This is a
`.3dsx` app, with no CIA installer or HOME Menu title.

## Validation and device testing

The native build targets ARMv6K and uses no New 3DS-only APIs. It has compiled
with devkitARM in the container above. Actual Old/New 3DS operation, microphone
and speaker timing, system keyboard behavior, sleep/resume, and Wi-Fi recovery
still need hardware testing. A successful build does not establish those results.

Before calling a device build stable, test sending and receiving on both
screens, long messages, scrolling while messages arrive, multiple voice users,
mute/deafen/PTT, typing during voice, DSP firmware missing, lid/HOME suspend,
relay/server restarts, and a Wi-Fi outage. The local view retains only the most
recent 128 messages and 64 members. Messages have no delivery acknowledgments,
so an interrupted send can be lost and is never automatically resent.

## Implementation references

- Drake Rochelle's supplied *Nintendo 3DS Homebrewing - Getting Started Guide*
  informed the `.3dsx` and Homebrew Launcher build/install flow.
- [libctru microphone API](https://github.com/devkitPro/libctru/blob/master/libctru/include/3ds/services/mic.h)
  supplies buffer alignment, PCM encoding, and exact sample clocks.
- [devkitPro microphone example](https://github.com/devkitPro/3ds-examples/blob/master/audio/mic/source/main.c)
  demonstrates the shared capture ring.
- [libctru NDSP API](https://libctru.devkitpro.org/channel_8h.html) supplies
  playback buffers and output configuration.
- [Citro2D text API](https://github.com/devkitPro/citro2d/blob/master/include/c2d/text.h)
  supplies system-font rendering.
- [libctru filesystem API](https://github.com/devkitPro/libctru/blob/master/libctru/include/3ds/services/fs.h)
  supplies direct SD archive access and file write/flush operations.
- [devkitPro application template](https://github.com/devkitPro/3ds-examples/blob/master/templates/application/Makefile)
  is the basis of the Makefile.
