#include <sqlite3.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>

#include "check.h"
#include "common/recent_messages.h"

namespace {

std::filesystem::path path() {
    return std::filesystem::temp_directory_path() /
           ("disquisition-messages-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
}

sqlite3* open(const std::filesystem::path& file) {
    sqlite3* db = nullptr;
    CHECK(sqlite3_open_v2(file.string().c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK);
    return db;
}

int count(sqlite3* db) {
    sqlite3_stmt* statement = nullptr;
    CHECK(sqlite3_prepare_v2(db, "SELECT count(*) FROM saved_messages", -1, &statement, nullptr) ==
          SQLITE_OK);
    CHECK(sqlite3_step(statement) == SQLITE_ROW);
    const int result = sqlite3_column_int(statement, 0);
    sqlite3_finalize(statement);
    return result;
}

void testContinuousSavingAndReload() {
    const auto file = path();
    {
        chat::RecentMessages store(file.string());
        store.append({123, "alice", "hello 'world'", "mint"});
        sqlite3* db = open(file);
        CHECK(count(db) == 1); // the write is committed before append returns
        sqlite3_close(db);
        store.append({124, "bob", "line one\nline two", "pink"});
    }
    {
        chat::RecentMessages store(file.string());
        const auto loaded = store.recent();
        CHECK(loaded.size() == 2);
        CHECK(loaded[0].timestamp == 123);
        CHECK(loaded[0].sender == "alice");
        CHECK(loaded[0].body == "hello 'world'");
        CHECK(loaded[0].color == "mint");
        CHECK(loaded[1].body == "line one\nline two");
        store.append({125, "carol", "new message", "aqua"});
    }
    sqlite3* db = open(file);
    CHECK(count(db) == 3); // reopening appends; it does not replace the file
    sqlite3_close(db);
    std::filesystem::remove(file);
}

void testUnlimitedAndDisplayLimit() {
    const auto file = path();
    {
        chat::RecentMessages store(file.string());
        for (int index = 0; index <= 1000; ++index) {
            store.append({index, "alice", "body", "mint"});
        }
        const auto loaded = store.recent();
        CHECK(loaded.size() == chat::RecentMessages::displayLimit);
        CHECK(loaded.front().timestamp == 1);
        CHECK(loaded.back().timestamp == 1000);
    }
    sqlite3* db = open(file);
    CHECK(count(db) == 1001); // no maximum means no pruning
    sqlite3_close(db);
    std::filesystem::remove(file);
}

void testMaximumAndRollback() {
    const auto file = path();
    {
        chat::RecentMessages store(file.string());
        for (int index = 0; index < 5; ++index) {
            store.append({index, "alice", "body", "mint"});
        }
    }
    {
        chat::RecentMessages store(file.string(), 2);
        auto loaded = store.recent();
        CHECK(loaded.size() == 2);
        CHECK(loaded[0].timestamp == 3);
        store.append({5, "bob", "body", "pink"});
        loaded = store.recent();
        CHECK(loaded.size() == 2);
        CHECK(loaded[0].timestamp == 4);
        CHECK(loaded[1].timestamp == 5);

        sqlite3* db = open(file);
        CHECK(sqlite3_exec(db,
                           "CREATE TRIGGER fail_prune BEFORE DELETE ON saved_messages "
                           "BEGIN SELECT RAISE(FAIL, 'blocked'); END",
                           nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(db);

        bool failed = false;
        try {
            store.append({6, "bob", "must roll back", "pink"});
        }
        catch (const std::runtime_error&) {
            failed = true;
        }
        CHECK(failed);
        loaded = store.recent();
        CHECK(loaded.size() == 2);
        CHECK(loaded[1].timestamp == 5);
    }
    std::filesystem::remove(file);
}

void testChannelIsolationAndLegacyMigration() {
    const auto file = path();
    sqlite3* legacy = nullptr;
    CHECK(sqlite3_open(file.string().c_str(), &legacy) == SQLITE_OK);
    CHECK(sqlite3_exec(
              legacy,
              "CREATE TABLE saved_messages (id INTEGER PRIMARY KEY, timestamp INTEGER NOT NULL, "
              "sender TEXT NOT NULL, body TEXT NOT NULL, color TEXT NOT NULL); "
              "INSERT INTO saved_messages VALUES (1, 1, 'old user', 'old message', 'mint')",
              nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(legacy);
    {
        chat::RecentMessages store(file.string());
        store.append({2, "alice", "community one general", "mint", "one", "general", "message-1",
                      "alice-id"});
        store.append({2, "alice", "duplicate", "mint", "one", "general", "message-1"});
        store.append({3, "alice", "community one music", "mint", "one", "music", "message-1"});
        store.append({4, "bob", "community two general", "mint", "two", "general", "message-1"});
        CHECK(store.recent().size() == 1);
        CHECK(store.recent()[0].body == "old message");
        CHECK(store.recent()[0].senderId.empty());
        const auto general = store.recent("one", "general");
        CHECK(general.size() == 1 && general[0].body == "community one general");
        CHECK(general[0].communityId == "one" && general[0].channelId == "general" &&
              general[0].messageId == "message-1");
        CHECK(general[0].senderId == "alice-id");
        CHECK(store.recent("one", "music")[0].body == "community one music");
        CHECK(store.recent("two", "general")[0].sender == "bob");
        CHECK(store.recent("missing", "general").empty());
    }
    {
        chat::RecentMessages reopened(file.string());
        CHECK(reopened.recent("two", "general").size() == 1);
        CHECK(reopened.recent("one", "general")[0].senderId == "alice-id");
        CHECK(reopened.recent().size() == 1);
    }
    std::filesystem::remove(file);
}

} // namespace

int main() {
    testContinuousSavingAndReload();
    testUnlimitedAndDisplayLimit();
    testMaximumAndRollback();
    testChannelIsolationAndLegacyMigration();
    std::puts("recent_messages_test: ok");
}
