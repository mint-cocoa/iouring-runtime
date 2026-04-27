#pragma once

#include "../types.h"
#include <string>
#include <vector>
#include <functional>
#include <optional>
#include <cstdint>

struct CharInfo {
    CharId id;
    std::string name;
    int32_t level;
};

struct ItemData {
    int64_t instance_id;
    int32_t item_def_id;
    int32_t slot;
    int32_t quantity;
    int32_t durability;
};

struct CurrencyData {
    int64_t gold = 0;
};

class DbService {
public:
    virtual ~DbService() = default;

    virtual void Login(const std::string& username, const std::string& password,
                       std::function<void(bool success, PlayerId player_id)> cb) = 0;

    virtual void Register(const std::string& username, const std::string& password,
                          std::function<void(bool success, std::string error)> cb) = 0;

    virtual void GetCharList(PlayerId pid,
                             std::function<void(std::vector<CharInfo>)> cb) = 0;

    virtual void CreateChar(PlayerId pid, const std::string& name,
                            std::function<void(bool success, std::string error, CharInfo ch)> cb) = 0;

    virtual void LoadInventory(CharId cid,
                               std::function<void(std::vector<ItemData>)> cb) = 0;

    virtual void LoadCurrency(CharId cid,
                              std::function<void(CurrencyData)> cb) = 0;
};
