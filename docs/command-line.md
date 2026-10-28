# Desktop and relay command-line reference

## Desktop

```text
disquisition [--community-data <directory>] [invite]
```

`--community-data` selects an isolated identity and metadata directory. Only one
running client can use a directory at a time. The optional positional argument
is a `disquisition://join/` invite link. The app also accepts invite links through
the operating system's URL handler where registered.

For example, on macOS:

```sh
./build/disquisition.app/Contents/MacOS/disquisition --community-data /tmp/second-community-client
```

On Linux and Windows, use `disquisition-desktop` or `disquisition-desktop.exe`.
Use the desktop interface to create communities, join invites, and choose relay
or direct connections. There is no terminal chat interface.

## Relay

```text
relay [--listen <port>]
```

`--listen` defaults to 3333. Port 0 asks the operating system to choose an
available port, which the relay prints at startup. Invalid ports or unknown
options cause startup to fail. `--help` lists supported options.

```sh
./build/relay --listen 3333
```

The relay listens for community registrations and encrypted peer tunnels. It
needs no upstream server address, advertised host, or database. Desktop clients
enter the reachable relay hostname and optional port in **User Settings**.
Stop it with Ctrl+C or SIGTERM on systems that provide that signal.

## Local message saving

**User Settings** controls optional SQLite storage, the database file,
and message retention. Community identities and metadata are always saved,
independently of chat saving. Messages remain live-only: reconnecting clients
do not fetch missed messages from the relay or other peers.
