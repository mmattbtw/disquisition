#include <QCommandLineParser>
#include <QCoreApplication>
#include <cstdio>

#include "community/relay.h"

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("community-relay");
    QCommandLineParser parser;
    parser.setApplicationDescription(
        "Community rendezvous and end-to-end TLS tunnel; no discovery server required.");
    parser.addHelpOption();
    parser.addOption({"listen", "TCP listen port", "port", "3334"});
    parser.process(app);
    bool valid = false;
    const auto port = parser.value("listen").toUInt(&valid);
    if (!valid || port == 0 || port > 65535) {
        std::fprintf(stderr, "invalid port\n");
        return 1;
    }
    community::Relay relay;
    if (!relay.listen(static_cast<quint16>(port))) {
        std::fprintf(stderr, "%s\n", relay.errorString().toUtf8().constData());
        return 1;
    }
    std::printf("community relay listening on %u\n", relay.port());
    return app.exec();
}
