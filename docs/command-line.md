# Terminal client and command-line reference

Start a server as shown in the [README](../README.md#run-a-local-chat), then
connect from another terminal:

```sh
./build/client --host 127.0.0.1 --port 9000 --name matt
```

Running `./build/client` without arguments opens interactive setup, which
defaults to `relay.mmatt.net:9000`. With any command-line option, the default
server is `127.0.0.1:9000`.

For remote connections, address advertising and relay setup, see
[networking](networking.md).

## Command-line options

### Server

```text
-p, --port <port>       Listen port. Default: 9000
-d, --db <path>         SQLite file. Default: chat.db
-h, --help              Show help
```

Passing port `0` asks the operating system to select a free server port.

### Client

```text
-H, --host <host>          Server host. CLI default: 127.0.0.1
-p, --port <port>          Server port. Default: 9000
-n, --name <name>          Name to request at sign-in
    --p2p-port <port>      Direct peer listener. Default: 0, an automatic port
    --advertise <host>     Reachable address announced to peers
    --local                Automatically announce your local IPv4 address
    --relay <host[:port]>  Use a relay. Default relay port: 3333
    --leak-my-ip           Fall back to direct mode if the relay is unavailable
    --log <file>           Write a debug log to this file. Default: no log
    --save-messages <file> Save chat to a local SQLite file
    --max-saved-messages <count>  Keep only the newest count; requires --save-messages
-h, --help                 Show help
```

The saved-message limit defaults to unlimited. `--max-saved-messages` requires
`--save-messages` and a positive count.

### Relay

```text
-H, --host <host>       Chat server host. Default: 127.0.0.1
-p, --port <port>       Chat server port. Default: 9000
    --advertise <host>  Public address announced for the relay
    --listen <port>     Shared client and peer port. Default: 3333
-h, --help              Show help
```

For normal remote use, set `--advertise` to the relay's public DNS name or IP address.

### Logging

The server and relay log to stdout, one timestamped line per event, for example:

```text
[2026-09-24 01:36:07.247] [server] [info] alice joined from 127.0.0.1
```

The terminal client writes nothing to the console while its interface is open, so it logs only when given `--log <file>`. That file also includes debug detail from the networking code, such as every dial and reconnect attempt. The C++ library logs at debug level through spdlog's default logger, which is silent unless the host program enables debug output.

## Terminal controls

| Input | Action |
| --- | --- |
| `Enter` | Send the current line |
| `Up`, `Down` | Scroll one row |
| `PgUp`, `PgDn` | Scroll one page |
| `Home`, `End` | Jump to the oldest or newest message |
| `Ctrl-C`, `Ctrl-D` | Quit |
| `Ctrl-L` | Redraw the terminal |
| `/users` | List users and known connection routes |
| `/color <value>` | Set a named shade or an xterm-256 index |
| `/clear` | Clear the local message pane |
| `/save <path.db>`, `/save off` | Start or stop continuous local message saving |
| `/help` | Show commands |
| `/quit`, `/exit` | Quit |

Named colors are `pink`, `mint`, `butter`, `periwinkle`, `lilac`, `aqua`, and `peach`. Numeric colors range from `0` through `255`, except `1`, `2`, and `250`, which the interface reserves for system text. Numeric colors require a 256-color terminal. Your own messages appear white in your terminal regardless of the color sent to other users.

Names are trimmed, limited to 20 bytes, and have spaces changed to underscores. Message bodies are trimmed and limited to 2,000 bytes.

## Local message history

Local saving is off by default. In the terminal client, pass
`--save-messages <path.db>` or enter a file path during interactive setup. You
can also enter `/save <path.db>` while connected to start saving, or `/save off`
to stop. Pass `--max-saved-messages <count>` to retain only that many messages.
In the desktop app, open Preferences, check "Save chat messages
locally," choose a SQLite file, and optionally enter a maximum. Leaving the
maximum blank keeps every message. Both clients append each new chat message
to the file as it arrives or is sent. They reopen an existing database and
show its 1,000 most recent messages when you connect. The stored history can
grow beyond what the UI displays. `/clear` clears the local message pane but
does not delete saved messages.

The SQLite `saved_messages` table has `timestamp` in Unix seconds, `sender`,
`body`, and `color` columns. The clients do not save system notices or send
saved messages to the server.
