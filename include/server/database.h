#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace chat {

struct StoredMessage {
    std::int64_t timestamp = 0;
    std::string sender;
    std::string body;
    std::string color;
};

// Message history in SQLite.
class Database {
public:
    explicit Database(const std::string& path);
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    ~Database();

    void add(std::int64_t timestamp, const std::string& sender, const std::string& body,
             const std::string& color);

    // The most recent `limit` messages, oldest first.
    std::vector<StoredMessage> recent(std::size_t limit);

private:
    sqlite3* db_ = nullptr;
    sqlite3_stmt* insert_ = nullptr;
    sqlite3_stmt* select_ = nullptr;
};

} // namespace chat
