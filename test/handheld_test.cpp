#include "check.h"
#include "session.h"
#include "settings.h"
#include "transport.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

using chat::MsgType;

namespace {

void settingsTest() {
    handheld::Settings settings;
    settings.host = "relay.mmatt.net";
    settings.name = "my 3ds";
    settings.color = "peach";
    settings.pushToTalk = false;
    const auto decoded = handheld::parseSettings(handheld::encodeSettings(settings));
    CHECK(decoded.host == "relay.mmatt.net" && decoded.port == 3333);
    CHECK(decoded.name == "my_3ds" && decoded.color == "peach" && !decoded.pushToTalk);
    const auto invalid = handheld::parseSettings("host=http://bad\nport=999999\nname= \ncolor=invalid\n");
    CHECK(invalid.host.empty() && invalid.port == 3333 && invalid.name == "3ds" && invalid.color == "mint");
    const auto windows = handheld::parseSettings("host=relay.mmatt.net\r\nport=3333\r\nname=matt\r\n");
    CHECK(windows.host == "relay.mmatt.net" && windows.name == "matt" && windows.port == 3333);
}

void sessionTest() {
    handheld::Session session;
    CHECK(!session.sendChat("offline", 1));
    session.begin("  handheld user  ");
    auto outgoing = session.takeOutgoing();
    CHECK(outgoing.size() == 1);
    CHECK(outgoing[0].type == MsgType::Login);
    CHECK((outgoing[0].fields == std::vector<std::string>{"handheld_user", "0", ""}));
    session.receive({MsgType::LoginOk, {"handheld_user-2", "welcome"}}, 100);
    CHECK(session.view.connected && session.view.historyLoading);
    outgoing = session.takeOutgoing();
    CHECK(outgoing.size() == 1 && outgoing[0].type == MsgType::FetchHistory);
    CHECK(session.sendChat(" hello\n3DS ", 123));
    outgoing = session.takeOutgoing();
    CHECK(outgoing.size() == 2);
    CHECK(outgoing[0].type == MsgType::PeerChat && outgoing[1].type == MsgType::Store);
    CHECK((outgoing[0].fields == std::vector<std::string>{"handheld_user-2", "123", "hello 3DS", "mint"}));
    session.receive({MsgType::History, {"123", "handheld_user-2", "hello 3DS", "mint"}}, 200);
    session.receive({MsgType::History, {"100", "desktop", "older", "pink"}}, 200);
    session.receive({MsgType::PeerChat, {"desktop", "101", "live", "aqua"}}, 200);
    session.receive({MsgType::History, {"101", "desktop", "live", "aqua"}}, 200);
    session.receive({MsgType::PeerChat, {"desktop", "bad", "invalid", "aqua"}}, 200);
    CHECK(session.view.messages.size() == 3);
    CHECK(session.view.messages.front().body == "older");
    CHECK(session.view.messages.back().body == "hello 3DS");
    session.receive({MsgType::HistoryEnd, {}}, 200);
    CHECK(!session.view.historyLoading);

    session.receive({MsgType::Peer, {"desktop", "127.0.0.1", "1", "0", "5060"}}, 200);
    session.receive({MsgType::Users, {"desktop", "handheld_user-2"}}, 200);
    CHECK(session.view.members.at("desktop").voice);
    session.setVoice(true);
    outgoing = session.takeOutgoing();
    CHECK(outgoing.size() == 2);
    CHECK(outgoing[0].type == MsgType::VoicePort && outgoing[0].fields[0] == "65535");
    CHECK(outgoing[1].fields[1] == "1"); // Idle PTT is muted on the wire.
    CHECK(!session.view.transmitting);
    handheld::Samples samples;
    samples.fill(1000);
    const auto pcm = handheld::encodePcm(samples);
    session.capture(pcm);
    CHECK(session.takeOutgoing().empty());
    session.controls(false, false, true, true);
    session.capture(pcm);
    outgoing = session.takeOutgoing();
    CHECK(outgoing.size() == 2);
    CHECK(outgoing[0].type == MsgType::VoiceState && outgoing[0].fields[1] == "0");
    CHECK(outgoing[1].type == MsgType::VoiceAudio && outgoing[1].fields[0] == pcm);
    session.controls(false, true, true, true);
    CHECK(!session.view.transmitting);
    session.takeOutgoing();
    int received = 0;
    session.audioReceived = [&](const std::string& name, const std::string& frame) {
        CHECK(name == "desktop" && frame == pcm);
        ++received;
    };
    session.receive({MsgType::VoiceAudio, {"desktop", pcm}}, 300);
    CHECK(received == 0);
    session.controls(false, false, true, false);
    session.receive({MsgType::VoiceAudio, {"desktop", pcm}}, 300);
    session.receive({MsgType::VoiceAudio, {"desktop", "bad PCM"}}, 300);
    CHECK(received == 1 && session.view.members.at("desktop").lastAudio == 300);
    session.receive({MsgType::VoiceState, {"desktop", "1", "0"}}, 300);
    CHECK(session.view.members.at("desktop").muted);
    session.receive({MsgType::PeerLeft, {"desktop"}}, 300);
    CHECK(session.view.members.count("desktop") == 0);
    session.receive({MsgType::LoginOk, {"new_name"}}, 400);
    CHECK(!session.view.voice && !session.view.transmitting);
    session.disconnected("gone");
    CHECK(!session.view.connected && session.view.members.empty());
    CHECK(session.takeOutgoing().empty());

    session.receive({MsgType::LoginOk, {"bounded"}}, 500);
    for (int i = 0; i < 200; ++i) {
        session.receive({MsgType::PeerChat, {"desktop", std::to_string(i + 1000), "message", "pink"}}, 500);
        session.receive({MsgType::Peer, {"user" + std::to_string(i), "localhost", "1", "0"}}, 500);
    }
    CHECK(session.view.messages.size() == handheld::kMessageLimit);
    CHECK(session.view.members.size() == handheld::kMemberLimit);
    session.receive({MsgType::Users, {"replacement", "bounded"}}, 500);
    CHECK(session.view.members.size() == 2 && session.view.members.count("replacement") == 1);
}

void audioTest() {
    handheld::Samples samples{};
    samples[0] = -32768; samples[1] = 32767; samples[2] = -1;
    const auto pcm = handheld::encodePcm(samples);
    CHECK(pcm.size() == 640);
    CHECK(static_cast<unsigned char>(pcm[0]) == 0 && static_cast<unsigned char>(pcm[1]) == 128);
    CHECK(static_cast<unsigned char>(pcm[2]) == 255 && static_cast<unsigned char>(pcm[3]) == 127);
    handheld::Samples decoded{};
    CHECK(handheld::decodePcm(pcm, decoded) && decoded == samples);
    CHECK(!handheld::decodePcm("short", decoded));
    samples.fill(20000);
    handheld::Mixer mixer;
    mixer.receive("one", handheld::encodePcm(samples));
    mixer.receive("two", handheld::encodePcm(samples));
    CHECK(mixer.render()[0] == 32767);
    CHECK(mixer.render()[0] == 0);
    samples.fill(-20000);
    mixer.receive("one", handheld::encodePcm(samples));
    mixer.receive("two", handheld::encodePcm(samples));
    CHECK(mixer.render()[0] == -32768);
    for (int i = 1; i <= 20; ++i) {
        samples.fill(static_cast<std::int16_t>(i));
        mixer.receive("one", handheld::encodePcm(samples));
    }
    CHECK(mixer.render()[0] == 15); // Drop old frames beyond the six-frame limit.
    mixer.remove("one");
    CHECK(mixer.render()[0] == 0);
    samples.fill(450);
    CHECK(!handheld::speaking(samples));
    samples.fill(451);
    CHECK(handheld::speaking(samples));

    handheld::Resampler resampler;
    int frames = 0;
    // Ten seconds from the documented mic clock must produce 16 kHz output
    // without drift, even though capture calls arrive in arbitrary chunks.
    for (int i = 0; i < 163645; ++i) {
        if (resampler.push(1000, samples)) {
            ++frames;
            CHECK(std::all_of(samples.begin(), samples.end(), [](auto s) { return s == 1000; }));
        }
    }
    CHECK(frames == 500);
    resampler.reset();
    frames = 0;
    for (int i = 0; i < 1000; ++i) {
        const auto input = static_cast<std::int16_t>(12000 * std::sin(i * 6.283185307179586 * 1000 / 16364.479));
        if (resampler.push(input, samples)) {
            ++frames;
            CHECK(handheld::speaking(samples));
            // The 1 kHz tone crosses upward about 20 times in each 20 ms frame.
            int crossings = 0;
            for (std::size_t j = 1; j < samples.size(); ++j)
                if (samples[j - 1] <= 0 && samples[j] > 0) ++crossings;
            CHECK(crossings >= 19 && crossings <= 21);
        }
    }
    CHECK(frames == 3);
}

void transportTest() {
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(listener >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    CHECK(listen(listener, 1) == 0);
    socklen_t size = sizeof(address);
    CHECK(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size) == 0);
    handheld::Transport transport;
    CHECK(transport.open("127.0.0.1", ntohs(address.sin_port), 0));
    const int peer = accept(listener, nullptr, nullptr);
    CHECK(peer >= 0);
    std::vector<chat::Message> received;
    for (int i = 0; i < 100 && transport.state() != handheld::Transport::State::Connected; ++i)
        transport.poll(1, received);
    CHECK(transport.state() == handheld::Transport::State::Connected);
    const auto frame = chat::encode({MsgType::LoginOk, {"fragmented"}});
    CHECK(send(peer, frame.data(), 3, 0) == 3);
    transport.poll(2, received);
    CHECK(received.empty());
    const auto rest = frame.substr(3) + chat::encode({MsgType::HistoryEnd, {}});
    CHECK(send(peer, rest.data(), rest.size(), 0) == static_cast<ssize_t>(rest.size()));
    for (int i = 0; i < 1000 && received.size() < 2; ++i) {
        transport.poll(3, received);
        usleep(1000);
    }
    CHECK(received.size() == 2 && received[0].fields[0] == "fragmented");
    for (int i = 0; i < 100; ++i)
        CHECK(transport.queue({MsgType::VoiceAudio, {std::string(640, '\0')}}));
    const chat::Message text{MsgType::Store, {"1", "priority", "mint"}};
    CHECK(transport.queue(text)); // Text discards queued audio that hasn't started.
    transport.poll(4, received);
    const auto expected = chat::encode(text);
    std::string sent;
    while (sent.size() < expected.size()) {
        char bytes[512];
        const auto count = recv(peer, bytes, sizeof(bytes), 0);
        CHECK(count > 0);
        sent.append(bytes, static_cast<std::size_t>(count));
    }
    CHECK(sent == expected);
    const char bad[4] = {0, 0, 0, 0};
    CHECK(send(peer, bad, 4, 0) == 4);
    for (int i = 0; i < 1000 && transport.state() != handheld::Transport::State::Failed; ++i) {
        transport.poll(4, received);
        usleep(1000);
    }
    CHECK(transport.state() == handheld::Transport::State::Failed);
    close(peer);

    // A blocked relay may lose disposable audio, but a full reliable queue
    // must fail explicitly rather than consume unbounded console memory.
    CHECK(transport.open("127.0.0.1", ntohs(address.sin_port), 10));
    const int slow = accept(listener, nullptr, nullptr);
    CHECK(slow >= 0);
    transport.poll(11, received);
    CHECK(transport.state() == handheld::Transport::State::Connected);
    bool accepted = true;
    for (int i = 0; i < 100 && accepted; ++i)
        accepted = transport.queue({MsgType::Store, {"1", std::string(2000, 'x'), "mint"}});
    CHECK(!accepted && transport.state() == handheld::Transport::State::Failed);
    close(slow);

    CHECK(transport.open("127.0.0.1", ntohs(address.sin_port), 20));
    const int departing = accept(listener, nullptr, nullptr);
    CHECK(departing >= 0);
    close(departing);
    for (int i = 0; i < 1000 && transport.state() != handheld::Transport::State::Failed; ++i) {
        transport.poll(21, received);
        usleep(1000);
    }
    CHECK(transport.state() == handheld::Transport::State::Failed);
    close(listener);
}

} // namespace

int main() {
    settingsTest(); sessionTest(); audioTest(); transportTest();
    std::puts("handheld_test: ok");
}
