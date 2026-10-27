#include "common/recent_messages.h"

#include <sqlite3.h>

#include <algorithm>
#include <memory>
#include <set>
#include <stdexcept>

namespace chat {
namespace {

using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

void execute(sqlite3* db, const char* sql) {
    if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(db));
    }
}

Statement prepare(sqlite3* db, const char* sql) {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &raw, nullptr) != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(db));
    }
    return Statement(raw, sqlite3_finalize);
}

void bindText(sqlite3* db, sqlite3_stmt* statement, int index, const std::string& value) {
    if (sqlite3_bind_text(statement, index, value.data(), static_cast<int>(value.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(db));
    }
}

std::string columnText(sqlite3_stmt* statement, int column) {
    const char* bytes = reinterpret_cast<const char*>(sqlite3_column_text(statement, column));
    const int length = sqlite3_column_bytes(statement, column);
    return bytes == nullptr ? "" : std::string(bytes, static_cast<std::size_t>(length));
}

} // namespace

RecentMessages::RecentMessages(const std::string& path, std::optional<std::int64_t> maximum) :
    maximum_(maximum) {
    if (path.empty() || (maximum_ && *maximum_ <= 0)) {
        throw std::invalid_argument("message file and maximum must be valid");
    }
    const int opened = sqlite3_open_v2(path.c_str(), &db_,
                                       SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if (opened != SQLITE_OK) {
        const std::string detail = db_ ? sqlite3_errmsg(db_) : "unknown error";
        sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("cannot open " + path + ": " + detail);
    }
    try {
        sqlite3_busy_timeout(db_, 5000);
        execute(db_, "PRAGMA secure_delete = ON");
        execute(db_, "CREATE TABLE IF NOT EXISTS saved_messages ("
                     "id INTEGER PRIMARY KEY, timestamp INTEGER NOT NULL, "
                     "sender TEXT NOT NULL, body TEXT NOT NULL, color TEXT NOT NULL)");
        // Empty scope identifies the legacy room in upgraded local files.
        std::set<std::string> columns;
        {
            auto info = prepare(db_, "PRAGMA table_info(saved_messages)");
            while (sqlite3_step(info.get()) == SQLITE_ROW)
                columns.insert(columnText(info.get(), 1));
        }
        execute(db_, "BEGIN IMMEDIATE");
        try {
            for (const auto* column : {"community_id", "channel_id", "message_id", "sender_id"}) {
                if (!columns.contains(column))
                    execute(db_, (std::string("ALTER TABLE saved_messages ADD COLUMN ") + column +
                                  " TEXT NOT NULL DEFAULT ''")
                                     .c_str());
            }
            execute(db_,
                    "CREATE UNIQUE INDEX IF NOT EXISTS saved_message_identity ON saved_messages "
                    "(community_id, channel_id, message_id) WHERE message_id != ''");
            execute(db_, "CREATE INDEX IF NOT EXISTS saved_message_channel ON saved_messages "
                         "(community_id, channel_id, id)");
            execute(db_, "COMMIT");
        }
        catch (...) {
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
            throw;
        }
        prune();
    }
    catch (...) {
        sqlite3_close(db_);
        db_ = nullptr;
        throw;
    }
}

RecentMessages::~RecentMessages() {
    sqlite3_close(db_);
}

void RecentMessages::prune() {
    if (!maximum_)
        return;
    auto statement = prepare(db_, "DELETE FROM saved_messages WHERE id NOT IN "
                                  "(SELECT id FROM saved_messages ORDER BY id DESC LIMIT ?)");
    if (sqlite3_bind_int64(statement.get(), 1, *maximum_) != SQLITE_OK ||
        sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(sqlite3_errmsg(db_));
    }
}

void RecentMessages::append(const RecentMessage& message) {
    if (message.communityId.empty() != message.channelId.empty())
        throw std::invalid_argument("community and channel must both be specified");
    try {
        execute(db_, "BEGIN IMMEDIATE");
        {
            auto statement =
                prepare(db_, "INSERT OR IGNORE INTO saved_messages "
                             "(timestamp, sender, body, color, community_id, "
                             "channel_id, message_id, sender_id) VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
            if (sqlite3_bind_int64(statement.get(), 1, message.timestamp) != SQLITE_OK) {
                throw std::runtime_error(sqlite3_errmsg(db_));
            }
            bindText(db_, statement.get(), 2, message.sender);
            bindText(db_, statement.get(), 3, message.body);
            bindText(db_, statement.get(), 4, message.color);
            bindText(db_, statement.get(), 5, message.communityId);
            bindText(db_, statement.get(), 6, message.channelId);
            bindText(db_, statement.get(), 7, message.messageId);
            bindText(db_, statement.get(), 8, message.senderId);
            if (sqlite3_step(statement.get()) != SQLITE_DONE) {
                throw std::runtime_error(sqlite3_errmsg(db_));
            }
        }
        prune();
        execute(db_, "COMMIT");
    }
    catch (...) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
}

std::vector<RecentMessage> RecentMessages::recent(std::size_t limit) const {
    return recent({}, {}, limit);
}

std::vector<RecentMessage> RecentMessages::recent(const std::string& communityId,
                                                  const std::string& channelId,
                                                  std::size_t limit) const {
    auto statement = prepare(
        db_, "SELECT timestamp, sender, body, color, community_id, channel_id, message_id, "
             "sender_id FROM "
             "saved_messages WHERE community_id = ? AND channel_id = ? ORDER BY id DESC LIMIT ?");
    bindText(db_, statement.get(), 1, communityId);
    bindText(db_, statement.get(), 2, channelId);
    if (sqlite3_bind_int64(statement.get(), 3, static_cast<sqlite3_int64>(limit)) != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(db_));
    }
    std::vector<RecentMessage> messages;
    for (;;) {
        const int result = sqlite3_step(statement.get());
        if (result == SQLITE_DONE)
            break;
        if (result != SQLITE_ROW)
            throw std::runtime_error(sqlite3_errmsg(db_));
        messages.push_back({sqlite3_column_int64(statement.get(), 0),
                            columnText(statement.get(), 1), columnText(statement.get(), 2),
                            columnText(statement.get(), 3), columnText(statement.get(), 4),
                            columnText(statement.get(), 5), columnText(statement.get(), 6),
                            columnText(statement.get(), 7)});
    }
    std::reverse(messages.begin(), messages.end());
    return messages;
}

} // namespace chat
