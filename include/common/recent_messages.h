#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>

namespace chat {

struct RecentMessage {
    std::int64_t timestamp = 0;
    std::string sender;
    std::string body;
    std::string color;
};

// Chat messages seen in this client session. Nothing is written until save()
// is called. System notices and connection details are not kept here.
class RecentMessages {
public:
    static constexpr std::size_t limit = 1000;

    void add(RecentMessage message);
    void clear();
    std::size_t size() const;

    // Replaces the saved_messages table in the chosen SQLite file with this
    // snapshot. Throws std::runtime_error on failure.
    void save(const std::string& path) const;

private:
    std::deque<RecentMessage> messages_;
};

} // namespace chat
