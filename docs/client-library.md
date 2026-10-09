# C++ client library

CMake exposes the static library as `disquisition::client`. Include its public header with:

```cpp
#include <client/client.h>
```

A direct connection to the central server is the default:

```cpp
#include <iostream>
#include <string>

#include <client/client.h>

void showMessage(std::string sender, std::string body)
{
    std::cout << sender << ": " << body << '\n';
}

int main()
{
    disquisition::Client client("chat.example.net:9000");
    client.onMessage(showMessage);
    client.connect();
    client.setName("matt");
    client.setColor(20);
    client.sendMessage("hello");
    client.disconnect();
}
```

To connect through a relay, select `RELAY`:

```cpp
disquisition::Client client(
    "relay.example.net:3333",
    disquisition::Client::RELAY
);
```

The current library is smaller than the terminal client. It supports live send and receive in direct or relay mode. It does not reconnect after a connection failure, expose the user roster, or report the final suffixed name. The message callback runs on the library's background service thread, so callback code must be thread-safe. The API throws standard exceptions for invalid values, invalid call order, and connection failures.

See [`examples/basic_client.cpp`](../examples/basic_client.cpp) for an interactive example. It uses its own Makefile and compiles the required project sources directly. Build the main project once first, so CMake has downloaded spdlog:

```sh
cd examples
make
./basic_client
```
