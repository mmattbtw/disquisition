#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace chat {

struct RecentMessage {
    std::int64_t timestamp = 0;
    std::string sender;
    std::string body;
    std::string color;
};

// Each append is committed before returning. An absent maximum keeps every
// message in the database; recent() only limits what the UI loads at once.
class RecentMessages {
public:
    static constexpr std::size_t displayLimit = 1000;

    explicit RecentMessages(const std::string& path,
                            std::optional<std::int64_t> maximum = std::nullopt);
    ~RecentMessages();
    RecentMessages(const RecentMessages&) = delete;
    RecentMessages& operator=(const RecentMessages&) = delete;

    void append(const RecentMessage& message);
    std::vector<RecentMessage> recent(std::size_t limit = displayLimit) const;

private:
    void prune();

    sqlite3* db_ = nullptr;
    std::optional<std::int64_t> maximum_;
};

} // namespace chat
