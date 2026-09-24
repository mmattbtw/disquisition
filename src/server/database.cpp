#include "server/database.h"

#include <sqlite3.h>

#include <algorithm>
#include <stdexcept>

namespace chat {
namespace {

constexpr const char* kInsertSql =
    "INSERT INTO messages (timestamp, sender, body, color) VALUES (?, ?, ?, ?)";
constexpr const char* kSelectRecentSql =
    "SELECT timestamp, sender, body, color FROM messages ORDER BY id DESC "
    "LIMIT ?";

[[noreturn]] void fail(sqlite3* db, const std::string& what) {
    const char* detail = db != nullptr ? sqlite3_errmsg(db) : "unknown error";
    throw std::runtime_error(what + ": " + detail);
}

void exec(sqlite3* db, const char* sql) {
    char* error = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &error) != SQLITE_OK) {
        const std::string detail = error != nullptr ? error : "unknown error";
        sqlite3_free(error);
        throw std::runtime_error(std::string("sqlite: ") + detail);
    }
}

bool hasColumn(sqlite3* db, const char* table, const char* column) {
    sqlite3_stmt* statement = nullptr;
    const std::string query = "PRAGMA table_info(" + std::string(table) + ")";
    if (sqlite3_prepare_v2(db, query.c_str(), -1, &statement, nullptr) != SQLITE_OK) {
        fail(db, "cannot inspect table");
    }
    bool found = false;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        const auto* name = reinterpret_cast<const char*>(sqlite3_column_text(statement, 1));
        if (name != nullptr && column == std::string(name)) {
            found = true;
            break;
        }
    }
    sqlite3_finalize(statement);
    return found;
}

} // namespace

Database::Database(const std::string& path) {
    if (sqlite3_open_v2(path.c_str(), &db_,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        const std::string detail = db_ != nullptr ? sqlite3_errmsg(db_) : "cannot open file";
        if (db_ != nullptr) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        throw std::runtime_error("cannot open database " + path + ": " + detail);
    }

    sqlite3_busy_timeout(db_, 5000);
    exec(db_, "PRAGMA journal_mode = WAL");
    exec(db_, "PRAGMA synchronous = NORMAL");
    exec(db_, "CREATE TABLE IF NOT EXISTS messages ("
              "  id INTEGER PRIMARY KEY,"
              "  timestamp INTEGER NOT NULL,"
              "  sender TEXT NOT NULL,"
              "  body TEXT NOT NULL,"
              "  color TEXT NOT NULL DEFAULT 'pink')");
    if (!hasColumn(db_, "messages", "color")) {
        exec(db_, "ALTER TABLE messages ADD COLUMN color TEXT NOT NULL DEFAULT 'pink'");
    }

    if (sqlite3_prepare_v2(db_, kInsertSql, -1, &insert_, nullptr) != SQLITE_OK) {
        fail(db_, "cannot prepare insert");
    }
    if (sqlite3_prepare_v2(db_, kSelectRecentSql, -1, &select_, nullptr) != SQLITE_OK) {
        fail(db_, "cannot prepare select");
    }
}

Database::~Database() {
    if (insert_ != nullptr) {
        sqlite3_finalize(insert_);
    }
    if (select_ != nullptr) {
        sqlite3_finalize(select_);
    }
    if (db_ != nullptr) {
        sqlite3_close(db_);
    }
}

void Database::add(std::int64_t timestamp, const std::string& sender, const std::string& body,
                   const std::string& color) {
    sqlite3_reset(insert_);
    sqlite3_clear_bindings(insert_);
    sqlite3_bind_int64(insert_, 1, timestamp);
    sqlite3_bind_text(insert_, 2, sender.c_str(), static_cast<int>(sender.size()), SQLITE_STATIC);
    sqlite3_bind_text(insert_, 3, body.c_str(), static_cast<int>(body.size()), SQLITE_STATIC);
    sqlite3_bind_text(insert_, 4, color.c_str(), static_cast<int>(color.size()), SQLITE_STATIC);
    if (sqlite3_step(insert_) != SQLITE_DONE) {
        fail(db_, "cannot store message");
    }
}

std::vector<StoredMessage> Database::recent(std::size_t limit) {
    std::vector<StoredMessage> messages;
    if (limit == 0) {
        return messages;
    }

    sqlite3_reset(select_);
    sqlite3_clear_bindings(select_);
    sqlite3_bind_int64(select_, 1, static_cast<sqlite3_int64>(limit));

    int rc = SQLITE_ROW;
    while ((rc = sqlite3_step(select_)) == SQLITE_ROW) {
        StoredMessage message;
        message.timestamp = sqlite3_column_int64(select_, 0);
        const auto* sender = reinterpret_cast<const char*>(sqlite3_column_text(select_, 1));
        const auto* body = reinterpret_cast<const char*>(sqlite3_column_text(select_, 2));
        const auto* color = reinterpret_cast<const char*>(sqlite3_column_text(select_, 3));
        message.sender = sender != nullptr ? sender : "";
        message.body = body != nullptr ? body : "";
        message.color = color != nullptr ? color : "pink";
        messages.push_back(std::move(message));
    }
    if (rc != SQLITE_DONE) {
        fail(db_, "cannot read history");
    }

    std::reverse(messages.begin(), messages.end());
    return messages;
}

} // namespace chat
