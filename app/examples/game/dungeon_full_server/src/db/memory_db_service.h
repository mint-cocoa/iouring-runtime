#pragma once

#include "db_service.h"
#include <unordered_map>
#include <shared_mutex>
#include <mutex>

class MemoryDbService : public DbService {
public:
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

private:
    struct Account {
        PlayerId id;
        std::string password;
    };

    std::shared_mutex mutex_;
    std::unordered_map<std::string, Account> accounts_;
    std::unordered_map<PlayerId, std::vector<CharInfo>> characters_;
    PlayerId next_player_id_ = 1;
    CharId next_char_id_ = 1;
};
