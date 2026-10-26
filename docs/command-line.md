# Server and relay command-line reference

For server and relay setup, see [networking](networking.md).

## Command-line options

### Server

```text
-p, --port <port>       Listen port. Default: 9000
-d, --db <path>         SQLite file. Default: chat.db
-h, --help              Show help
```

Passing port `0` asks the operating system to select a free server port.

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

The C++ library logs at debug level through spdlog's default logger, which is
silent unless the host program enables debug output.
