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
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
            ".db");
}

sqlite3* open(const std::filesystem::path& file) {
    sqlite3* db = nullptr;
    CHECK(sqlite3_open_v2(file.string().c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) ==
          SQLITE_OK);
    return db;
}

int count(sqlite3* db) {
    sqlite3_stmt* statement = nullptr;
    CHECK(sqlite3_prepare_v2(db, "SELECT count(*) FROM saved_messages", -1, &statement,
                             nullptr) == SQLITE_OK);
    CHECK(sqlite3_step(statement) == SQLITE_ROW);
    const int result = sqlite3_column_int(statement, 0);
    sqlite3_finalize(statement);
    return result;
}

void testSnapshot() {
    const auto file = path();
    chat::RecentMessages recent;
    recent.add({123, "alice", "hello 'world'", "mint"});
    recent.add({124, "bob", "line one\nline two", "pink"});
    recent.save(file.string());

    sqlite3* db = open(file);
    CHECK(count(db) == 2);
    sqlite3_stmt* statement = nullptr;
    CHECK(sqlite3_prepare_v2(db,
                             "SELECT timestamp, sender, body, color FROM saved_messages "
                             "ORDER BY id", -1, &statement, nullptr) == SQLITE_OK);
    CHECK(sqlite3_step(statement) == SQLITE_ROW);
    CHECK(sqlite3_column_int64(statement, 0) == 123);
    CHECK(std::string(reinterpret_cast<const char*>(sqlite3_column_text(statement, 1))) ==
          "alice");
    CHECK(std::string(reinterpret_cast<const char*>(sqlite3_column_text(statement, 2))) ==
          "hello 'world'");
    CHECK(std::string(reinterpret_cast<const char*>(sqlite3_column_text(statement, 3))) ==
          "mint");
    CHECK(sqlite3_step(statement) == SQLITE_ROW);
    CHECK(std::string(reinterpret_cast<const char*>(sqlite3_column_text(statement, 2))) ==
          "line one\nline two");
    CHECK(sqlite3_step(statement) == SQLITE_DONE);
    sqlite3_finalize(statement);
    sqlite3_close(db);

    recent.clear();
    recent.add({125, "carol", "new snapshot", "aqua"});
    recent.save(file.string());
    db = open(file);
    CHECK(count(db) == 1);
    sqlite3_close(db);
    std::filesystem::remove(file);
}

void testLimitAndRollback() {
    const auto file = path();
    chat::RecentMessages recent;
    for (std::size_t index = 0; index <= chat::RecentMessages::limit; ++index) {
        recent.add({static_cast<std::int64_t>(index), "alice", "body", "mint"});
    }
    CHECK(recent.size() == chat::RecentMessages::limit);
    recent.save(file.string());
    sqlite3* db = open(file);
    CHECK(count(db) == static_cast<int>(chat::RecentMessages::limit));
    sqlite3_stmt* statement = nullptr;
    CHECK(sqlite3_prepare_v2(db, "SELECT min(timestamp) FROM saved_messages", -1,
                             &statement, nullptr) == SQLITE_OK);
    CHECK(sqlite3_step(statement) == SQLITE_ROW);
    CHECK(sqlite3_column_int64(statement, 0) == 1);
    sqlite3_finalize(statement);
    CHECK(sqlite3_exec(db, "CREATE TRIGGER fail_insert BEFORE INSERT ON saved_messages "
                           "BEGIN SELECT RAISE(FAIL, 'blocked'); END", nullptr, nullptr,
                       nullptr) == SQLITE_OK);
    sqlite3_close(db);

    bool failed = false;
    try {
        recent.save(file.string());
    } catch (const std::runtime_error&) {
        failed = true;
    }
    CHECK(failed);
    db = open(file);
    CHECK(count(db) == static_cast<int>(chat::RecentMessages::limit));
    sqlite3_close(db);
    std::filesystem::remove(file);
}

} // namespace

int main() {
    testSnapshot();
    testLimitAndRollback();
    std::puts("recent_messages_test: ok");
}
