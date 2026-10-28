#include <QCommandLineParser>
#include <QCoreApplication>
#include <QTimer>
#include <csignal>
#include <cstdio>

#include "relay/relay.h"

namespace {
volatile std::sig_atomic_t stopping = 0;
void requestShutdown(int) {
    stopping = 1;
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("relay");
    QCommandLineParser parser;
    parser.setApplicationDescription(
        "Community peer routing and end-to-end encrypted TCP tunnels.");
    parser.addHelpOption();
    parser.addOption(
        {"listen", "TCP listening port (0 chooses an available port)", "port", "3333"});
    parser.process(app);
    const auto value = parser.value("listen");
    bool valid = false;
    const auto port = value.toUInt(&valid);
    for (const auto ch : value)
        valid = valid && ch >= QLatin1Char('0') && ch <= QLatin1Char('9');
    if (!valid || port > 65535) {
        std::fprintf(stderr, "invalid listen port\n");
        return 1;
    }
    std::signal(SIGINT, requestShutdown);
    std::signal(SIGTERM, requestShutdown);
#ifdef SIGPIPE
    std::signal(SIGPIPE, SIG_IGN);
#endif
    chat::Relay relay;
    if (!relay.listen(QHostAddress::Any, static_cast<quint16>(port))) {
        std::fprintf(stderr, "cannot listen: %s\n", qPrintable(relay.errorString()));
        return 1;
    }
    qInfo("listening on port %u", relay.serverPort());
    QTimer shutdown;
    QObject::connect(&shutdown, &QTimer::timeout, &app, [&] {
        if (stopping)
            app.quit();
    });
    shutdown.start(100);
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &relay, &chat::Relay::close);
    return app.exec();
}
