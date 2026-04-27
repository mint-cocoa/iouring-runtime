#include "memory_db_service.h"

void MemoryDbService::Login(const std::string& username, const std::string& password,
                            std::function<void(bool, PlayerId)> cb) {
    // 자동 등록: 계정이 없으면 생성 (LoadTest 봇 호환)
    {
        std::unique_lock lock(mutex_);
        auto it = accounts_.find(username);
        if (it == accounts_.end()) {
            auto id = next_player_id_++;
            accounts_[username] = {id, password};
            // 기본 캐릭터도 자동 생성
            CharInfo ch{next_char_id_++, username, 1};
            characters_[id].push_back(ch);
            cb(true, id);
            return;
        }
        if (it->second.password != password) {
            cb(false, 0);
            return;
        }
        cb(true, it->second.id);
    }
}

void MemoryDbService::Register(const std::string& username, const std::string& password,
                               std::function<void(bool, std::string)> cb) {
    std::unique_lock lock(mutex_);
    if (accounts_.contains(username)) {
        cb(false, "Username already exists");
        return;
    }
    auto id = next_player_id_++;
    accounts_[username] = {id, password};
    cb(true, "");
}

void MemoryDbService::GetCharList(PlayerId pid,
                                  std::function<void(std::vector<CharInfo>)> cb) {
    std::shared_lock lock(mutex_);
    auto it = characters_.find(pid);
    if (it == characters_.end()) {
        cb({});
        return;
    }
    cb(it->second);
}

void MemoryDbService::CreateChar(PlayerId pid, const std::string& name,
                                 std::function<void(bool, std::string, CharInfo)> cb) {
    std::unique_lock lock(mutex_);

    for (auto& [_, chars] : characters_) {
        for (auto& c : chars) {
            if (c.name == name) {
                cb(false, "Character name already exists", {});
                return;
            }
        }
    }

    CharInfo ch{next_char_id_++, name, 1};
    characters_[pid].push_back(ch);
    cb(true, "", ch);
}

void MemoryDbService::LoadInventory(CharId cid,
                                    std::function<void(std::vector<ItemData>)> cb) {
    cb({});
}

void MemoryDbService::LoadCurrency(CharId cid,
                                   std::function<void(CurrencyData)> cb) {
    cb({1000});
}
