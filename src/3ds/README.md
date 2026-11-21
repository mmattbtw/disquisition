# Disquisition for Nintendo 3DS

A native Homebrew Launcher client that joins Disquisition communities as a
member, alongside the desktop app. The top screen shows the selected text
channel; the bottom screen has Communities, Channels, Chat, Members, and
Settings tabs. It uses libctru and Citro2D with the system font and software
keyboard, and the shared console community client in `src/console` (mbedTLS
and Monocypher) for signed metadata and TLS peer connections.

It has live text chat with the desktop's message colors, unread markers, a
member list with voice status, and experimental channel voice: microphone
capture, speaker/headphone playback, mixing for up to eight speakers, mute,
deafen, and push-to-talk. Voice is off until you join a voice channel.

## Install

Copy the contents of the `disquisition-3ds` build artifact to your SD card.
The resulting files should be:

```text
sdmc:/3ds/disquisition/disquisition.3dsx
sdmc:/3ds/disquisition/disquisition.smdh
```

Launch Disquisition from the Homebrew Launcher with Wi-Fi enabled.

## Connect and join a community

1. Open **Settings**. Tap **Name** to set your display name (1 to 64
   characters). **Relay** defaults to `relay.disquisition.app`; enter another
   hostname, optionally as `host:port` (port 3333 by default). Tap **Color** to
   cycle the desktop's chat colors.
2. Tap **Connect**. The app never connects automatically on launch.
3. Get an invite link from a desktop member (**Invite** copies one). It looks
   like `disquisition://join/...` and is usually 140 to 250 characters long.
   The keyboard accepts up to 1024 characters; use `invite.txt` for longer
   links, such as one listing several direct-mode members.
4. On **Communities**, either tap **Type invite** and enter the rest of the
   link, or save the link as the first line of
   `sdmc:/3ds/disquisition/invite.txt` on a computer and tap **invite.txt**.

The community shows **Joining...** until an online member admits the console.
Tap a joining entry, then **Cancel join**, to give up. To leave a community,
select it, tap **Leave**, then **Confirm**. Rejoining needs a new invite.

## Channels, chat, and members

Tap a community to open its channels. Text channels open the chat on the top
screen; a dot marks unread messages, also shown on the Communities and Channels
tabs. Tap a voice channel to join it, or tap it again to leave. **Y** joins the
community's first voice channel or leaves voice.

Chat is live-only, as on the desktop: there is no history download, and the app
keeps the last 128 messages per channel until it exits. A message sent while no
other member is online is marked **Not delivered**. Images from desktops appear
as `[sent an image]`. Live messages appear in arrival order, and sending returns
the transcript to the newest message.

**Members** lists the selected community, online members first, with each
member's voice channel, mute or deafen state, and a highlight while speaking.

## Controls

| Control | Action |
| --- | --- |
| Touch tabs | Switch Communities, Channels, Chat, Members, or Settings |
| A | Connect, or open the message keyboard in Chat |
| Y | Join or leave voice |
| Hold L | Talk while push-to-talk is selected |
| X | Mute or unmute the microphone |
| Touch Deafen | Stop received audio and microphone transmission |
| D-pad up/down | Scroll the chat, or the list on Communities, Channels, and Members |
| R | Return to newest messages |
| B | Return to Chat and dismiss the local notice |
| SELECT | Open Settings |
| START | Disconnect and exit to Homebrew Launcher |

The keyboard's OK button sends a message; Cancel leaves it unsent. Messages
allow 500 characters. Disconnect before changing your name, relay, or color;
push-to-talk can change while connected. Switching to open microphone transmits
immediately if you are in voice and neither muted nor deafened.

## Files

Settings are saved to `sdmc:/3ds/disquisition/settings.cfg` when they change.
You can also edit this file on a computer:

```ini
name=handheld
relay=relay.disquisition.app
color=mint
push_to_talk=1
```

Community data lives in `sdmc:/3ds/disquisition/community/`: `identity.key`
is the console's member identity, each `<id>.community.json` holds a
community's signed metadata, and `pending.json` holds joins in progress.
Deleting `identity.key` means rejoining every community with new invites.

Files use the native 3DS SD archive API. Saving checks folder creation, file
writes, flushing, and replacement, and keeps the replaced file as `<name>.bak`,
which loads if the console lost power mid-save. A settings save failure does not
prevent connecting with the settings currently entered.

## Troubleshooting

The line below the title always shows the connection state, including
`Wi-Fi disconnected.` while the console has no Wi-Fi. The square next to your
name turns green while the relay is connected. The relay reconnects on its own
every few seconds after a failure.

Local notices, including file errors, appear in the top screen's footer. Press
B to dismiss one. SD errors identify the operation and native result, such as
`SD open card: 0x...` or `SD write settings.cfg: 0x...`. The `3ds/disquisition`
path on the card must be a directory, with `disquisition.3dsx` inside it,
rather than a file named `disquisition`. `SSL service: 0x...` at launch means
the console's SSL service, which supplies random numbers for keys and TLS, is
unavailable.

The 3DS socket service can leave its raw connection-in-progress value (`-26`)
in `SO_ERROR` after a socket becomes writable. The client checks for a
connected peer instead of reporting that as a failure. See the
[libctru report](https://github.com/devkitPro/libctru/issues/412).

## Voice

Dump the DSP firmware using Luma3DS's Rosalina menu: Miscellaneous options,
then Dump DSP firmware. The resulting `sdmc:/3ds/dspfirm.cdc` is needed for
NDSP playback. Chat still works if audio initialization fails; the app shows
the service's error code. This requirement comes from the
[devkitPro audio examples](https://github.com/devkitPro/3ds-examples/blob/master/audio/README.md).

The microphone captures signed 16-bit PCM at `MICU_SAMPLE_RATE_16360`, whose
documented clock is 16364.479 Hz. A streaming linear resampler converts it to
16 kHz. The client sends 320 samples per 20 ms frame as 640 little-endian
bytes, the desktop's voice channel audio format, over the same encrypted peer
connections as chat.

Received members have separate bounded queues and mix into one NDSP channel at
16 kHz, clipped to the signed 16-bit range. The mixer handles up to eight
simultaneous speakers and keeps at most six frames per speaker. The 3DS speaker
can feed back into its microphone; headphones and the default push-to-talk mode
help. There is no echo cancellation.

The app mutes the microphone while the system keyboard is open. Closing the
lid or leaving the application leaves voice and stops the microphone. Chat
stays connected, or reconnects by itself after sleep; join voice again
afterwards. Voice frames drop under network congestion so they cannot build up
an unlimited delay. Relayed voice can still stutter on poor Wi-Fi.

## Limitations

- The console connects only through a relay. It opens no listening port, so
  other members always reach it through the relay.
- It cannot create communities, channels, or invites, change community
  settings, send images, or share its screen.
- The 3DS clock has no time zone. Message times from other devices are shown in
  UTC, and desktops show the console's messages offset by its UTC difference.
- Joining a large community verifies every signed metadata operation on the
  console, so the first sync can take a while on an Old 3DS.

## Build

Install devkitPro's `3ds-dev` package group, including devkitARM, libctru,
Citro2D, Citro3D, and 3DS tools, plus `3ds-mbedtls`. Use the packaged libctru,
rather than copying a different source tree over it. Start with
[devkitPro's setup instructions](https://devkitpro.org/wiki/Getting_Started).

```sh
make -C src/3ds -j4
```

Or build using the same pinned devkitPro container as CI, which already
includes `3ds-mbedtls`:

```sh
docker run --rm -v "$PWD:/work" -w /work/src/3ds \
  devkitpro/devkitarm:20260610 make -j4
```

The output is `src/3ds/disquisition.3dsx`, with an embedded SMDH and a
separate `.smdh` alongside it. The [3DS workflow](../../.github/workflows/3ds.yml)
builds and uploads an SD card folder on pushes and pull requests. This is a
`.3dsx` app, with no CIA installer or HOME Menu title.

## Validation and device testing

The native build targets ARMv6K and uses no New 3DS-only APIs. It compiles with
devkitARM in the container above. The community client it shares is tested on
a computer against the desktop community code and relay
(`test/console_client_test.cpp`). Actual Old/New 3DS operation, SD storage,
TLS performance, microphone and speaker timing, system keyboard behavior,
sleep/resume, and Wi-Fi recovery still need hardware testing. A successful
build does not establish those results.

Before calling a device build stable, test joining with both invite methods,
sending and receiving on both screens, long messages, scrolling while messages
arrive, unread markers, multiple voice users, mute/deafen/PTT, typing during
voice, DSP firmware missing, lid/HOME suspend, relay restarts, and a Wi-Fi
outage.

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
