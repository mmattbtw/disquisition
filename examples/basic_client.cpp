#include <iostream>
#include <limits>
#include <string>

#include <disquisition/client.h>

// Client calls this function whenever another user sends a message.
void showMessage(std::string sender, std::string message)
{
    std::cout << std::endl;
    std::cout << sender << ": " << message << std::endl;
    std::cout << "> " << std::flush;
}

// Run `make` from the examples folder to build this program. The Makefile
// tells g++ where the library headers and source files are located.
int main()
{
    std::string address;
    std::string name;
    std::string message;
    char connectionChoice;
    int color;

    std::cout << "Connect through a relay? (y/n): ";
    std::cin >> connectionChoice;

    std::cout << "Address (host:port): ";
    std::cin >> address;

    std::cout << "Name: ";
    std::cin >> name;

    std::cout << "Color (0-255): ";
    std::cin >> color;

    // Ignore the newline left in the input after reading the color.
    // This keeps the first getline call below from returning an empty string.
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

    disquisition::Client::ConnectionType connectionType;

    if (connectionChoice == 'y' || connectionChoice == 'Y')
    {
        connectionType = disquisition::Client::RELAY;
    }
    else
    {
        connectionType = disquisition::Client::DIRECT;
    }

    try
    {
        disquisition::Client client(address, connectionType);

        client.onMessage(showMessage);
        client.connect();
        client.setName(name);
        client.setColor(color);

        std::cout << "Connected. Type /quit to leave." << std::endl;

        while (true)
        {
            std::cout << "> ";
            std::getline(std::cin, message);

            if (message == "/quit")
            {
                break;
            }

            if (!message.empty())
            {
                client.sendMessage(message);
            }
        }

        client.disconnect();
    }
    catch (const std::exception& error)
    {
        std::cerr << "Error: " << error.what() << std::endl;
        return 1;
    }

    return 0;
}
