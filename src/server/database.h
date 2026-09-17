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
    std::string colour;
};

// Thin wrapper around the handful of SQLite calls the server needs. The
// connection is opened in full-mutex mode so it is safe to hand to other
// threads later if the server ever grows one.
class Database {
public:
    explicit Database(const std::string& path);
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    ~Database();

    void add(std::int64_t timestamp, const std::string& sender, const std::string& body,
             const std::string& colour);

    // Most recent `limit` messages, oldest first (ready to replay to a client).
    std::vector<StoredMessage> recent(std::size_t limit);

private:
    sqlite3* db_ = nullptr;
    sqlite3_stmt* insert_ = nullptr;
    sqlite3_stmt* select_ = nullptr;
};

}  // namespace chat
