#include "sqlite_db_service.h"
#include <spdlog/spdlog.h>

SqliteDbService::SqliteDbService(const std::string& db_path) {
    int rc = sqlite3_open(db_path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        spdlog::error("SQLite open failed: {}", sqlite3_errmsg(db_));
        db_ = nullptr;
        return;
    }
    Exec("PRAGMA journal_mode=WAL");
    Exec("PRAGMA synchronous=NORMAL");
    InitSchema();
    spdlog::info("SQLite DB opened: {}", db_path);
}

SqliteDbService::~SqliteDbService() {
    if (db_) sqlite3_close(db_);
}

void SqliteDbService::Exec(const char* sql) {
    char* err = nullptr;
    sqlite3_exec(db_, sql, nullptr, nullptr, &err);
    if (err) {
        spdlog::error("SQLite exec error: {}", err);
        sqlite3_free(err);
    }
}

void SqliteDbService::InitSchema() {
    Exec("CREATE TABLE IF NOT EXISTS accounts ("
         "id INTEGER PRIMARY KEY AUTOINCREMENT,"
         "username TEXT UNIQUE NOT NULL,"
         "password TEXT NOT NULL)");
    Exec("CREATE TABLE IF NOT EXISTS characters ("
         "id INTEGER PRIMARY KEY AUTOINCREMENT,"
         "player_id INTEGER NOT NULL,"
         "name TEXT UNIQUE NOT NULL,"
         "level INTEGER DEFAULT 1)");
    Exec("CREATE TABLE IF NOT EXISTS currency ("
         "char_id INTEGER PRIMARY KEY,"
         "gold INTEGER DEFAULT 1000,"
         "gems INTEGER DEFAULT 50,"
         "tokens INTEGER DEFAULT 10)");
    Exec("CREATE TABLE IF NOT EXISTS inventory ("
         "instance_id INTEGER PRIMARY KEY AUTOINCREMENT,"
         "char_id INTEGER NOT NULL,"
         "item_def_id INTEGER NOT NULL,"
         "slot INTEGER NOT NULL,"
         "quantity INTEGER DEFAULT 1,"
         "durability INTEGER DEFAULT 100)");
}

void SqliteDbService::Login(const std::string& username, const std::string& password,
                             std::function<void(bool, PlayerId)> cb) {
    bool success = false;
    PlayerId result_id = 0;
    {
        std::lock_guard lock(mutex_);

        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_, "SELECT id, password FROM accounts WHERE username = ?", -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);

        if (sqlite3_step(stmt) == SQLITE_ROW) {
            PlayerId id = sqlite3_column_int64(stmt, 0);
            const char* pw = (const char*)sqlite3_column_text(stmt, 1);
            bool ok = (pw && password == pw);
            sqlite3_finalize(stmt);
            success = ok;
            result_id = ok ? id : 0;
            cb(success, result_id);  // existing account — safe, no nested DB calls from this path
            return;
        }
        sqlite3_finalize(stmt);

        // Auto-register
        sqlite3_prepare_v2(db_, "INSERT INTO accounts (username, password) VALUES (?, ?)", -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, password.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        result_id = sqlite3_last_insert_rowid(db_);

        success = true;
    }
    // mutex released — safe to call back
    cb(success, result_id);
}

void SqliteDbService::Register(const std::string& username, const std::string& password,
                                std::function<void(bool, std::string)> cb) {
    bool success = false;
    std::string error;
    {
        std::lock_guard lock(mutex_);

        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_, "INSERT INTO accounts (username, password) VALUES (?, ?)", -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, password.c_str(), -1, SQLITE_TRANSIENT);

        if (sqlite3_step(stmt) != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            success = false;
            error = "Username already exists";
        } else {
            sqlite3_finalize(stmt);
            success = true;
        }
    }
    cb(success, error);
}

void SqliteDbService::GetCharList(PlayerId pid,
                                   std::function<void(std::vector<CharInfo>)> cb) {
    std::vector<CharInfo> chars;
    {
        std::lock_guard lock(mutex_);

        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_, "SELECT id, name, level FROM characters WHERE player_id = ?", -1, &stmt, nullptr);
        sqlite3_bind_int64(stmt, 1, pid);

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            CharInfo ci;
            ci.id = sqlite3_column_int64(stmt, 0);
            ci.name = (const char*)sqlite3_column_text(stmt, 1);
            ci.level = sqlite3_column_int(stmt, 2);
            chars.push_back(ci);
        }
        sqlite3_finalize(stmt);
    }
    cb(chars);
}

void SqliteDbService::CreateChar(PlayerId pid, const std::string& name,
                                  std::function<void(bool, std::string, CharInfo)> cb) {
    bool success = false;
    std::string error;
    CharInfo ci{};
    {
        std::lock_guard lock(mutex_);

        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_, "INSERT INTO characters (player_id, name, level) VALUES (?, ?, 1)", -1, &stmt, nullptr);
        sqlite3_bind_int64(stmt, 1, pid);
        sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);

        if (sqlite3_step(stmt) != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            error = "Character name already exists";
        } else {
            sqlite3_finalize(stmt);

            ci.id = sqlite3_last_insert_rowid(db_);
            ci.name = name;
            ci.level = 1;

            // Default currency for new char
            sqlite3_prepare_v2(db_, "INSERT OR IGNORE INTO currency (char_id, gold) VALUES (?, 1000)", -1, &stmt, nullptr);
            sqlite3_bind_int64(stmt, 1, ci.id);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);

            success = true;
        }
    }
    cb(success, error, ci);
}

void SqliteDbService::LoadInventory(CharId cid,
                                     std::function<void(std::vector<ItemData>)> cb) {
    std::vector<ItemData> items;
    {
        std::lock_guard lock(mutex_);

        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_, "SELECT instance_id, item_def_id, slot, quantity, durability FROM inventory WHERE char_id = ?", -1, &stmt, nullptr);
        sqlite3_bind_int64(stmt, 1, cid);

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            ItemData it;
            it.instance_id = sqlite3_column_int64(stmt, 0);
            it.item_def_id = sqlite3_column_int(stmt, 1);
            it.slot = sqlite3_column_int(stmt, 2);
            it.quantity = sqlite3_column_int(stmt, 3);
            it.durability = sqlite3_column_int(stmt, 4);
            items.push_back(it);
        }
        sqlite3_finalize(stmt);
    }
    cb(items);
}

void SqliteDbService::LoadCurrency(CharId cid,
                                    std::function<void(CurrencyData)> cb) {
    CurrencyData result{1000};
    {
        std::lock_guard lock(mutex_);

        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_, "SELECT gold FROM currency WHERE char_id = ?", -1, &stmt, nullptr);
        sqlite3_bind_int64(stmt, 1, cid);

        if (sqlite3_step(stmt) == SQLITE_ROW) {
            result.gold = sqlite3_column_int64(stmt, 0);
            sqlite3_finalize(stmt);
        } else {
            sqlite3_finalize(stmt);

            // Insert defaults
            sqlite3_prepare_v2(db_, "INSERT INTO currency (char_id, gold) VALUES (?, 1000)", -1, &stmt, nullptr);
            sqlite3_bind_int64(stmt, 1, cid);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }
    cb(result);
}

void SqliteDbService::SaveCurrency(CharId cid, int64_t gold) {
    std::lock_guard lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_, "INSERT OR REPLACE INTO currency (char_id, gold) VALUES (?, ?)", -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, cid);
    sqlite3_bind_int64(stmt, 2, gold);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void SqliteDbService::SaveInventorySlot(CharId cid, int64_t instance_id, int32_t item_def_id,
                                         int32_t slot, int32_t qty) {
    std::lock_guard lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_, "INSERT OR REPLACE INTO inventory (instance_id, char_id, item_def_id, slot, quantity) VALUES (?, ?, ?, ?, ?)", -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, instance_id);
    sqlite3_bind_int64(stmt, 2, cid);
    sqlite3_bind_int(stmt, 3, item_def_id);
    sqlite3_bind_int(stmt, 4, slot);
    sqlite3_bind_int(stmt, 5, qty);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}
