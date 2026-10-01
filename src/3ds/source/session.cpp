#include "session.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace handheld {

void Session::begin(const std::string& name) {
    disconnected("Signing in through relay...");
    outgoing_.push_back({chat::MsgType::Login, {chat::sanitizeName(name), "0", ""}});
}

void Session::disconnected(const std::string& reason) {
    outgoing_.clear();
    view.connected = false;
    view.historyLoading = false;
    view.voice = false;
    view.transmitting = false;
    view.name.clear();
    view.members.clear();
    view.status = reason;
    held_ = false;
}

Member* Session::member(const std::string& name) {
    if (name.empty() || name.size() > chat::kMaxNameLength + 16) return nullptr;
    auto found = view.members.find(name);
    if (found != view.members.end()) return &found->second;
    if (view.members.size() >= kMemberLimit) return nullptr;
    return &view.members.emplace(name, Member{name}).first->second;
}

void Session::append(chat::ChatPayload message) {
    for (const auto& existing : view.messages) {
        if (existing.sender == message.sender && existing.timestamp == message.timestamp &&
            existing.body == message.body) return;
    }
    const auto position = std::upper_bound(view.messages.begin(), view.messages.end(),
        message.timestamp, [](std::int64_t time, const chat::ChatPayload& entry) {
            return time < entry.timestamp;
        });
    view.messages.insert(position, std::move(message));
    if (view.messages.size() > kMessageLimit) view.messages.erase(view.messages.begin());
    ++view.revision;
}

void Session::receive(const chat::Message& message, std::uint64_t now) {
    using chat::MsgType;
    const auto& f = message.fields;
    if (message.type == MsgType::LoginOk && !f.empty() && !f[0].empty()) {
        // A relay can report a new LoginOk after its upstream server reconnects.
        view.members.clear();
        view.name = f[0];
        view.connected = true;
        view.voice = false;
        view.transmitting = false;
        view.historyLoading = true;
        view.status = "Joined as " + view.name;
        member(view.name);
        outgoing_.push_back({MsgType::FetchHistory, {}});
    } else if (message.type == MsgType::Error && !f.empty()) {
        view.status = "Server: " + chat::sanitizeBody(f[0]);
    } else if (!view.connected) {
        return;
    } else if (message.type == MsgType::HistoryEnd) {
        view.historyLoading = false;
    } else if (message.type == MsgType::Users) {
        std::map<std::string, Member> roster;
        for (const auto& name : f) {
            if (roster.size() >= kMemberLimit) break;
            if (name.empty() || name.size() > chat::kMaxNameLength + 16) continue;
            const auto existing = view.members.find(name);
            roster.emplace(name, existing == view.members.end() ? Member{name} : existing->second);
        }
        view.members = std::move(roster);
    } else if (message.type == MsgType::Peer || message.type == MsgType::PeerJoined) {
        chat::PeerAddress peer;
        if (chat::parsePeerAddress(message, peer)) {
            if (Member* entry = member(peer.name)) entry->voice = peer.voicePort != 0;
        }
    } else if (message.type == MsgType::PeerLeft && !f.empty()) {
        view.members.erase(f[0]);
    } else if (message.type == MsgType::VoicePort && f.size() == 2) {
        std::uint16_t port = 0;
        if (chat::parsePort(f[1], port, true)) {
            if (Member* entry = member(f[0])) {
                entry->voice = port != 0;
                if (!entry->voice) {
                    entry->muted = entry->deafened = false;
                    entry->lastAudio = 0;
                }
            }
        }
    } else if (message.type == MsgType::VoiceState && f.size() == 3 &&
               (f[1] == "0" || f[1] == "1") && (f[2] == "0" || f[2] == "1")) {
        if (Member* entry = member(f[0])) {
            entry->muted = f[1] == "1";
            entry->deafened = f[2] == "1";
        }
    } else if (message.type == MsgType::VoiceAudio && f.size() == 2 &&
               view.voice && !view.deafened && f[0] != view.name) {
        Samples samples;
        if (decodePcm(f[1], samples)) {
            if (Member* entry = member(f[0])) {
                if (!entry->voice) return;
                if (speaking(samples)) entry->lastAudio = now;
                if (audioReceived) audioReceived(f[0], f[1]);
            }
        }
    } else if (message.type == MsgType::PeerChat) {
        chat::ChatPayload payload;
        if (chat::parsePeerChat(message, payload)) append(std::move(payload));
    } else if (message.type == MsgType::History && f.size() >= 3) {
        const std::string color = f.size() >= 4 ? f[3] : "pink";
        chat::ChatPayload payload;
        if (chat::parsePeerChat({MsgType::PeerChat, {f[1], f[0], f[2], color}}, payload))
            append(std::move(payload));
    } else if (message.type == MsgType::System && !f.empty()) {
        view.status = chat::sanitizeBody(f[0]);
    }
}

bool Session::sendChat(const std::string& text, std::int64_t timestamp) {
    const std::string body = chat::sanitizeBody(text);
    if (!view.connected || body.empty() || timestamp <= 0) return false;
    outgoing_.push_back({chat::MsgType::PeerChat,
        {view.name, std::to_string(timestamp), body, view.color}});
    outgoing_.push_back({chat::MsgType::Store,
        {std::to_string(timestamp), body, view.color}});
    append({view.name, timestamp, body, view.color});
    return true;
}

void Session::setVoice(bool enabled) {
    enabled = enabled && view.connected;
    if (view.voice == enabled) return;
    view.voice = enabled;
    held_ = false;
    outgoing_.push_back({chat::MsgType::VoicePort, {enabled ? "65535" : "0"}});
    voiceState(true);
}

void Session::controls(bool muted, bool deafened, bool pushToTalk, bool held) {
    view.muted = muted;
    view.deafened = deafened;
    view.pushToTalk = pushToTalk;
    held_ = held;
    voiceState();
}

void Session::voiceState(bool force) {
    const bool muted = !view.voice || view.muted || view.deafened ||
                       (view.pushToTalk && !held_);
    view.transmitting = !muted;
    if (view.connected && (force || muted != wireMuted_ || view.deafened != wireDeafened_)) {
        outgoing_.push_back({chat::MsgType::VoiceState,
            {view.name, muted ? "1" : "0", view.deafened ? "1" : "0"}});
    }
    wireMuted_ = muted;
    wireDeafened_ = view.deafened;
    if (Member* self = view.connected ? member(view.name) : nullptr) {
        self->voice = view.voice;
        self->muted = muted;
        self->deafened = view.deafened;
    }
}

void Session::capture(const std::string& pcm) {
    if (view.connected && view.transmitting && pcm.size() == kPcmBytes)
        outgoing_.push_back({chat::MsgType::VoiceAudio, {pcm}});
}

std::vector<chat::Message> Session::takeOutgoing() {
    std::vector<chat::Message> result;
    result.swap(outgoing_);
    return result;
}

std::string encodePcm(const Samples& samples) {
    std::string pcm(kPcmBytes, '\0');
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const auto value = static_cast<std::uint16_t>(samples[i]);
        pcm[2 * i] = static_cast<char>(value & 255);
        pcm[2 * i + 1] = static_cast<char>(value >> 8);
    }
    return pcm;
}

bool decodePcm(const std::string& pcm, Samples& samples) {
    if (pcm.size() != kPcmBytes) return false;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const unsigned value = static_cast<unsigned char>(pcm[2 * i]) |
            (static_cast<unsigned char>(pcm[2 * i + 1]) << 8);
        samples[i] = static_cast<std::int16_t>(value < 32768 ? int(value) : int(value) - 65536);
    }
    return true;
}

bool speaking(const Samples& samples) {
    std::int64_t sum = 0;
    for (const auto sample : samples) sum += static_cast<std::int64_t>(sample) * sample;
    return sum > static_cast<std::int64_t>(kSamples) * 450 * 450;
}

void Mixer::receive(const std::string& sender, const std::string& pcm) {
    Samples samples;
    if (!decodePcm(pcm, samples)) return;
    if (!streams_.count(sender) && streams_.size() >= 8) return;
    auto& queue = streams_[sender];
    if (queue.size() >= 6) queue.pop_front();
    queue.push_back(samples);
}

Samples Mixer::render() {
    std::array<std::int32_t, kSamples> mixed{};
    for (auto it = streams_.begin(); it != streams_.end();) {
        auto& queue = it->second;
        if (queue.empty()) {
            it = streams_.erase(it);
            continue;
        }
        for (std::size_t i = 0; i < kSamples; ++i) mixed[i] += queue.front()[i];
        queue.pop_front();
        ++it;
    }
    Samples result;
    for (std::size_t i = 0; i < kSamples; ++i)
        result[i] = static_cast<std::int16_t>(std::clamp<std::int32_t>(mixed[i], -32768, 32767));
    return result;
}

void Resampler::reset() {
    first_ = true;
    phase_ = 0;
    count_ = 0;
}

bool Resampler::push(std::int16_t sample, Samples& frame) {
    if (first_) {
        previous_ = sample;
        first_ = false;
        return false;
    }
    bool ready = false;
    if (phase_ <= 1.0) {
        const double value = previous_ + (sample - previous_) * phase_;
        pending_[count_++] = static_cast<std::int16_t>(std::lround(value));
        phase_ += 16364.479 / 16000.0;
        if (count_ == kSamples) {
            frame = pending_;
            count_ = 0;
            ready = true;
        }
    }
    phase_ -= 1.0;
    previous_ = sample;
    return ready;
}

} // namespace handheld
