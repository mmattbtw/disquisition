#pragma once

#include <string>

struct sqlite3;

namespace chat {

// Owns the server's SQLite connection. No message history is stored.
class Database {
public:
    explicit Database(const std::string& path);
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    ~Database();

private:
    sqlite3* db_ = nullptr;
};

} // namespace chat
