#pragma once
#include "db_service.h"
#include "../../vendor/sqlite3.h"
#include <string>
#include <mutex>

class SqliteDbService : public DbService {
public:
    explicit SqliteDbService(const std::string& db_path);
    ~SqliteDbService();

    void Login(const std::string& username, const std::string& password,
               std::function<void(bool, PlayerId)> cb) override;
    void Register(const std::string& username, const std::string& password,
                  std::function<void(bool, std::string)> cb) override;
    void GetCharList(PlayerId pid,
                     std::function<void(std::vector<CharInfo>)> cb) override;
    void CreateChar(PlayerId pid, const std::string& name,
                    std::function<void(bool, std::string, CharInfo)> cb) override;
    void LoadInventory(CharId cid,
                       std::function<void(std::vector<ItemData>)> cb) override;
    void LoadCurrency(CharId cid,
                      std::function<void(CurrencyData)> cb) override;

    void SaveCurrency(CharId cid, int64_t gold);
    void SaveInventorySlot(CharId cid, int64_t instance_id, int32_t item_def_id,
                           int32_t slot, int32_t qty);

private:
    void InitSchema();
    void Exec(const char* sql);
    sqlite3* db_ = nullptr;
    std::mutex mutex_;
};
