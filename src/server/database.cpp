#include "server/database.h"

#include <sqlite3.h>

#include <stdexcept>

namespace chat {
namespace {

void exec(sqlite3* db, const char* sql) {
    char* error = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &error) != SQLITE_OK) {
        const std::string detail = error != nullptr ? error : "unknown error";
        sqlite3_free(error);
        throw std::runtime_error(std::string("sqlite: ") + detail);
    }
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

    try {
        sqlite3_busy_timeout(db_, 5000);
        exec(db_, "PRAGMA journal_mode = WAL");
        exec(db_, "PRAGMA synchronous = NORMAL");
    }
    catch (...) {
        sqlite3_close(db_);
        db_ = nullptr;
        throw;
    }
}

Database::~Database() {
    if (db_ != nullptr) {
        sqlite3_close(db_);
    }
}

} // namespace chat
