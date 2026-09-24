#include "common/recent_messages.h"

#include <sqlite3.h>

#include <memory>
#include <stdexcept>
#include <utility>

namespace chat {
namespace {

using Database = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

void execute(sqlite3* db, const char* sql) {
    if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(db));
    }
}

void bindText(sqlite3* db, sqlite3_stmt* statement, int index, const std::string& value) {
    if (sqlite3_bind_text(statement, index, value.data(), static_cast<int>(value.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(db));
    }
}

} // namespace

void RecentMessages::add(RecentMessage message) {
    if (messages_.size() == limit) {
        messages_.pop_front();
    }
    messages_.push_back(std::move(message));
}

void RecentMessages::clear() {
    messages_.clear();
}

std::size_t RecentMessages::size() const {
    return messages_.size();
}

void RecentMessages::save(const std::string& path) const {
    sqlite3* raw = nullptr;
    const int opened = sqlite3_open_v2(path.c_str(), &raw,
                                       SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    Database db(raw, sqlite3_close);
    if (opened != SQLITE_OK) {
        throw std::runtime_error("cannot open " + path + ": " +
                                 (db ? sqlite3_errmsg(db.get()) : "unknown error"));
    }

    try {
        execute(db.get(), "PRAGMA secure_delete = ON");
        execute(db.get(), "BEGIN IMMEDIATE");
        execute(db.get(), "CREATE TABLE IF NOT EXISTS saved_messages ("
                          "id INTEGER PRIMARY KEY, timestamp INTEGER NOT NULL, "
                          "sender TEXT NOT NULL, body TEXT NOT NULL, color TEXT NOT NULL)");
        execute(db.get(), "DELETE FROM saved_messages");

        sqlite3_stmt* rawStatement = nullptr;
        if (sqlite3_prepare_v2(db.get(),
                               "INSERT INTO saved_messages(timestamp, sender, body, color) "
                               "VALUES (?, ?, ?, ?)", -1, &rawStatement, nullptr) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(db.get()));
        }
        Statement statement(rawStatement, sqlite3_finalize);
        for (const RecentMessage& message : messages_) {
            if (sqlite3_bind_int64(statement.get(), 1, message.timestamp) != SQLITE_OK) {
                throw std::runtime_error(sqlite3_errmsg(db.get()));
            }
            bindText(db.get(), statement.get(), 2, message.sender);
            bindText(db.get(), statement.get(), 3, message.body);
            bindText(db.get(), statement.get(), 4, message.color);
            if (sqlite3_step(statement.get()) != SQLITE_DONE) {
                throw std::runtime_error(sqlite3_errmsg(db.get()));
            }
            sqlite3_reset(statement.get());
            sqlite3_clear_bindings(statement.get());
        }
        statement.reset();
        execute(db.get(), "COMMIT");
    }
    catch (const std::exception& error) {
        const std::string detail = error.what();
        sqlite3_exec(db.get(), "ROLLBACK", nullptr, nullptr, nullptr);
        throw std::runtime_error("cannot save recent messages to " + path + ": " + detail);
    }
}

} // namespace chat
