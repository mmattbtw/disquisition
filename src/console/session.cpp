#include "console/session.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace console {
namespace {
std::string key(const std::string& community, const std::string& channel) {
    return community + '/' + channel;
}
std::string folded(std::string text) {
    for (auto& ch : text)
        if (ch >= 'A' && ch <= 'Z')
            ch = char(ch - 'A' + 'a');
    return text;
}
} // namespace

Session::Session(Storage& storage, Clock& clock) :
    clock_(clock), client_(storage, clock) {}

void Session::status(const std::string& text) {
    if (view.status != text) {
        view.status = text;
        ++view.revision;
    }
}

bool Session::start(const std::string& name, const std::string& relay, const std::string& color) {
    stop("Connecting...");
    view.color = color.empty() ? std::string("mint") : color;
    try {
        client_.load();
    }
    catch (const std::exception& error) {
        status(std::string("Saved community data is unreadable: ") + error.what());
        return false;
    }
    if (!client_.start(name, relay)) {
        for (const auto& event : client_.takeEvents())
            if (event.kind == Event::Kind::Error)
                status(event.message);
        return false;
    }
    view.started = true;
    view.name = client_.name();
    view.peerId = client_.peerId();
    status("Connecting to relay...");
    dirty_ = true;
    poll();
    return true;
}

void Session::stop(const std::string& text) {
    if (!view.voiceChannel.empty())
        leaveVoice();
    client_.stop();
    view.started = false;
    view.relayConnected = false;
    speakers_.clear();
    speaking_.clear();
    lastAudio_.clear();
    status(text);
    dirty_ = true;
}

void Session::join(const std::string& link) {
    try {
        client_.joinCommunity(link);
        status("Joining community...");
    }
    catch (const std::exception& error) {
        status(error.what());
    }
    dirty_ = true;
}

void Session::leave(const std::string& community) {
    try {
        if (view.voiceCommunity == community)
            leaveVoice();
        client_.leaveCommunity(community);
        if (view.community == community) {
            view.community.clear();
            view.channel.clear();
        }
        status("Left the community.");
    }
    catch (const std::exception& error) {
        status(error.what());
    }
    dirty_ = true;
}

void Session::choose(const std::string& community, const std::string& channel) {
    view.community = community;
    view.channel.clear();
    const auto model = client_.communities().find(community);
    if (model != client_.communities().end()) {
        const auto chosen = model->second.channels.find(channel);
        if (chosen != model->second.channels.end() && chosen->second.type == "text")
            view.channel = channel;
        else {
            // Default to the first text channel by name, as listed in the view.
            std::string best;
            for (const auto& [id, entry] : model->second.channels)
                if (entry.type == "text" &&
                    (best.empty() ||
                     folded(entry.name) < folded(model->second.channels.at(best).name)))
                    best = id;
            view.channel = best;
        }
    }
    unread_.erase(key(view.community, view.channel));
}

void Session::select(const std::string& community, const std::string& channel) {
    choose(community, channel);
    dirty_ = true;
}

void Session::append(const std::string& at, ChatLine line) {
    auto& lines = history_[at];
    lines.push_back(std::move(line));
    while (lines.size() > kMessageLimit)
        lines.pop_front();
    if (at != key(view.community, view.channel))
        unread_.insert(at);
    dirty_ = true;
}

bool Session::sendChat(const std::string& text) {
    std::string body = text;
    body.erase(0, body.find_first_not_of(" \t\r\n"));
    const auto last = body.find_last_not_of(" \t\r\n");
    body.erase(last == std::string::npos ? 0 : last + 1);
    if (!view.started || view.community.empty() || view.channel.empty() || body.empty())
        return false;
    try {
        const auto timestamp = clock_.wallMs();
        const int recipients = client_.sendPacket(
            view.community, view.channel, "chat",
            {{"body", body}, {"color", view.color}, {"timestamp", json::Value(timestamp)}});
        // Messages are live-only: nobody online means nobody received it.
        append(key(view.community, view.channel),
               {client_.peerId(), view.name, timestamp, body, view.color, true, recipients == 0});
        ++view.sentRevision;
        return true;
    }
    catch (const std::exception& error) {
        status(error.what());
        return false;
    }
}

bool Session::joinVoice(const std::string& community, const std::string& channel) {
    try {
        client_.setVoice(community, channel, view.muted || view.deafened, view.deafened);
    }
    catch (const std::exception& error) {
        status(error.what());
        return false;
    }
    view.voiceCommunity = community;
    view.voiceChannel = channel;
    held_ = false;
    publishVoice();
    dirty_ = true;
    return true;
}

void Session::leaveVoice() {
    if (!view.voiceCommunity.empty()) {
        try {
            client_.setVoice(view.voiceCommunity, {});
        }
        catch (const std::exception&) {
        }
    }
    view.voiceCommunity.clear();
    view.voiceChannel.clear();
    view.transmitting = false;
    for (const auto& peer : speakers_)
        if (speakerLeft)
            speakerLeft(peer);
    speakers_.clear();
    dirty_ = true;
}

void Session::controls(bool muted, bool deafened, bool pushToTalk, bool held) {
    const bool changed = muted != view.muted || deafened != view.deafened;
    view.muted = muted;
    view.deafened = deafened;
    view.pushToTalk = pushToTalk;
    held_ = held;
    if (changed && !view.voiceChannel.empty()) {
        try {
            // Push-to-talk stays local; the wire carries only the user's
            // own mute and deafen choices.
            client_.setVoice(view.voiceCommunity, view.voiceChannel, muted || deafened, deafened);
        }
        catch (const std::exception& error) {
            status(error.what());
        }
    }
    publishVoice();
}

void Session::publishVoice() {
    const bool transmitting = !view.voiceChannel.empty() && !view.muted && !view.deafened &&
                              (!view.pushToTalk || held_);
    if (transmitting != view.transmitting) {
        view.transmitting = transmitting;
        dirty_ = true;
    }
}

void Session::capture(const std::string& pcm) {
    if (!view.transmitting || pcm.size() != kPcmBytes)
        return;
    try {
        client_.sendPacket(view.voiceCommunity, view.voiceChannel, "audio",
                           {{"bytes", base64(pcm)}});
    }
    catch (const std::exception&) {
    }
    Samples samples;
    if (decodePcm(pcm, samples) && speaking(samples))
        lastAudio_[client_.peerId()] = clock_.monotonicMs();
}

void Session::handle(const Event& event) {
    const auto& communities = client_.communities();
    const auto community = communities.find(event.community);
    const auto senderName = [&](const std::string& peer) {
        if (community != communities.end()) {
            const auto member = community->second.members.find(peer);
            if (member != community->second.members.end())
                return member->second.name;
        }
        return peer.substr(0, 8);
    };
    switch (event.kind) {
    case Event::Kind::Changed: dirty_ = true; break;
    case Event::Kind::Error: status(event.message); break;
    case Event::Kind::Joined:
        status(community != communities.end() ? "Joined " + community->second.name
                                              : std::string("Joined community"));
        if (view.community.empty())
            select(event.community);
        dirty_ = true;
        break;
    case Event::Kind::JoinRejected:
        status("The invite was rejected. Ask for a new invite.");
        dirty_ = true;
        break;
    case Event::Kind::VoiceMoved:
        if (event.community == view.voiceCommunity) {
            view.voiceChannel = event.channel;
            status("An admin moved you to another voice channel.");
            dirty_ = true;
        }
        break;
    case Event::Kind::Packet: {
        if (event.type == "chat") {
            append(key(event.community, event.channel),
                   {event.peer, senderName(event.peer), event.data["timestamp"].toInteger(),
                    event.data["body"].toString(), event.data["color"].toString(), false, false});
        }
        else if (event.type == "image" && event.data["index"].toInt(-1) == 0) {
            append(key(event.community, event.channel),
                   {event.peer, senderName(event.peer), event.data["timestamp"].toInteger(),
                    "[sent an image]", event.data["color"].toString(), false, false});
        }
        else if (event.type == "audio" && event.community == view.voiceCommunity &&
                 event.channel == view.voiceChannel && !view.deafened) {
            const auto pcm = fromBase64(event.data["bytes"].toString());
            Samples samples;
            if (decodePcm(pcm, samples)) {
                if (speaking(samples))
                    lastAudio_[event.peer] = clock_.monotonicMs();
                speakers_.insert(event.peer);
                if (audioReceived)
                    audioReceived(event.peer, pcm);
            }
        }
        break;
    }
    }
}

void Session::rebuild() {
    const auto me = client_.peerId();
    const auto& communities = client_.communities();
    // Leave voice if its community or channel disappeared.
    if (!view.voiceChannel.empty()) {
        const auto model = communities.find(view.voiceCommunity);
        if (model == communities.end() || !model->second.isMember(me) ||
            !model->second.channels.count(view.voiceChannel))
            leaveVoice();
    }
    if (!view.community.empty()) {
        const auto model = communities.find(view.community);
        if (model == communities.end() || !model->second.isMember(me)) {
            view.community.clear();
            view.channel.clear();
        }
        else if (!model->second.channels.count(view.channel) ||
                 model->second.channels.at(view.channel).type != "text")
            choose(view.community); // marks nothing dirty: may have no text channel
    }
    view.communities.clear();
    for (const auto& [id, model] : communities) {
        if (!model.isMember(me))
            continue;
        CommunityView entry;
        entry.id = id;
        entry.name = model.name;
        const auto online = client_.online(id);
        for (const auto& [channelId, channel] : model.channels) {
            const bool unread = unread_.count(key(id, channelId)) != 0;
            entry.channels.push_back({channelId, channel.name, channel.type, channel.topic, unread});
            entry.unread = entry.unread || unread;
        }
        std::sort(entry.channels.begin(), entry.channels.end(), [](const auto& a, const auto& b) {
            if (a.type != b.type)
                return a.type == "text";
            const auto left = folded(a.name), right = folded(b.name);
            return left == right ? a.id < b.id : left < right;
        });
        const auto now = clock_.monotonicMs();
        for (const auto& [peer, member] : model.members) {
            MemberView person;
            person.id = peer;
            person.name = member.name;
            person.self = peer == me;
            person.online = online.count(peer) != 0;
            person.admin = model.canAdmin(peer);
            const auto presence = client_.presence(id, peer);
            if (person.online) {
                person.voiceChannel = presence.voiceChannel;
                person.muted = presence.muted;
                person.deafened = presence.deafened;
            }
            const auto heard = lastAudio_.find(peer);
            person.speaking = person.online && !person.voiceChannel.empty() &&
                              heard != lastAudio_.end() && now - heard->second < 300;
            entry.members.push_back(std::move(person));
        }
        std::sort(entry.members.begin(), entry.members.end(), [](const auto& a, const auto& b) {
            if (a.online != b.online)
                return a.online;
            const auto left = folded(a.name), right = folded(b.name);
            return left == right ? a.id < b.id : left < right;
        });
        if (entry.members.size() > kMemberLimit)
            entry.members.resize(kMemberLimit);
        view.communities.push_back(std::move(entry));
    }
    for (const auto& id : client_.pendingCommunities()) {
        CommunityView entry;
        entry.id = id;
        entry.name = "Joining...";
        entry.pending = true;
        view.communities.push_back(std::move(entry));
    }
    view.messages.clear();
    const auto found = history_.find(key(view.community, view.channel));
    if (found != history_.end())
        view.messages.assign(found->second.begin(), found->second.end());
    // Peers who stopped sending voice no longer need a jitter queue.
    if (!view.voiceChannel.empty()) {
        const auto online = client_.online(view.voiceCommunity);
        for (auto it = speakers_.begin(); it != speakers_.end();) {
            const auto presence = client_.presence(view.voiceCommunity, *it);
            if (!online.count(*it) || presence.voiceChannel != view.voiceChannel) {
                if (speakerLeft)
                    speakerLeft(*it);
                it = speakers_.erase(it);
            }
            else
                ++it;
        }
    }
    ++view.revision;
}

void Session::poll() {
    if (!view.started)
        return;
    client_.poll();
    for (const auto& event : client_.takeEvents())
        handle(event);
    if (view.relayConnected != client_.relayConnected()) {
        view.relayConnected = client_.relayConnected();
        status(view.relayConnected ? "Connected through the relay." : client_.relayError());
        dirty_ = true;
    }
    else if (!view.relayConnected && !client_.relayError().empty())
        status(client_.relayError());
    publishVoice();
    // Speaking indicators fade without any packet arriving.
    const auto now = clock_.monotonicMs();
    if (now - activityCheck_ >= 100) {
        activityCheck_ = now;
        std::set<std::string> active;
        for (const auto& [peer, heard] : lastAudio_)
            if (now - heard < 300)
                active.insert(peer);
        if (active != speaking_) {
            speaking_ = std::move(active);
            dirty_ = true;
        }
    }
    if (dirty_) {
        dirty_ = false;
        rebuild();
    }
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
    if (pcm.size() != kPcmBytes)
        return false;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const unsigned value = static_cast<unsigned char>(pcm[2 * i]) |
                               (static_cast<unsigned char>(pcm[2 * i + 1]) << 8);
        samples[i] = static_cast<std::int16_t>(value < 32768 ? int(value) : int(value) - 65536);
    }
    return true;
}

bool speaking(const Samples& samples) {
    std::int64_t sum = 0;
    for (const auto sample : samples)
        sum += static_cast<std::int64_t>(sample) * sample;
    return sum > static_cast<std::int64_t>(kSamples) * 450 * 450;
}

void Mixer::receive(const std::string& sender, const std::string& pcm) {
    Samples samples;
    if (!decodePcm(pcm, samples))
        return;
    if (!streams_.count(sender) && streams_.size() >= 8)
        return;
    auto& queue = streams_[sender];
    if (queue.size() >= 6)
        queue.pop_front();
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
        for (std::size_t i = 0; i < kSamples; ++i)
            mixed[i] += queue.front()[i];
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
        phase_ += step_;
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

} // namespace console
