#include "server/database.h"

#include <sqlite3.h>

#include <algorithm>
#include <stdexcept>

namespace chat {
namespace {

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

}  // namespace

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
    exec(db_,
         "CREATE TABLE IF NOT EXISTS messages ("
         "  id INTEGER PRIMARY KEY,"
         "  timestamp INTEGER NOT NULL,"
         "  sender TEXT NOT NULL,"
         "  body TEXT NOT NULL)");

    if (sqlite3_prepare_v2(db_, "INSERT INTO messages (timestamp, sender, body) VALUES (?, ?, ?)", -1,
                           &insert_, nullptr) != SQLITE_OK) {
        fail(db_, "cannot prepare insert");
    }
    if (sqlite3_prepare_v2(db_,
                           "SELECT timestamp, sender, body FROM messages ORDER BY id DESC LIMIT ?", -1,
                           &select_, nullptr) != SQLITE_OK) {
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

void Database::add(std::int64_t timestamp, const std::string& sender, const std::string& body) {
    sqlite3_reset(insert_);
    sqlite3_clear_bindings(insert_);
    sqlite3_bind_int64(insert_, 1, timestamp);
    sqlite3_bind_text(insert_, 2, sender.c_str(), static_cast<int>(sender.size()), SQLITE_STATIC);
    sqlite3_bind_text(insert_, 3, body.c_str(), static_cast<int>(body.size()), SQLITE_STATIC);
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
        message.sender = sender != nullptr ? sender : "";
        message.body = body != nullptr ? body : "";
        messages.push_back(std::move(message));
    }
    if (rc != SQLITE_DONE) {
        fail(db_, "cannot read history");
    }

    std::reverse(messages.begin(), messages.end());
    return messages;
}

}  // namespace chat
