#include <API/ARK/Ark.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>

#include "json.hpp"

#pragma comment(lib, "ArkApi.lib")

namespace
{
struct WinMutex
{
    WinMutex() noexcept
    {
        InitializeSRWLock(&lock_);
    }

    void lock() noexcept
    {
        AcquireSRWLockExclusive(&lock_);
    }

    void unlock() noexcept
    {
        ReleaseSRWLockExclusive(&lock_);
    }

private:
    SRWLOCK lock_{};
};

using DataLock = std::lock_guard<WinMutex>;

struct PromoEntry
{
    std::string code;
    std::string blueprint;
    int quantity = 1;
    float quality = 1.0f;
    bool force_blueprint = false;
    bool one_time_per_player = true;
    int max_total_uses = 0; // 0 = unlimited
};

struct Config
{
    std::string command = "/promo";
    bool case_sensitive = false;
    std::vector<PromoEntry> promos;
};

WinMutex data_mutex;
Config config;
// redeemed[normalized_code][steam_id_str] = unix_ts
std::unordered_map<std::string, std::unordered_map<std::string, int64_t>> redeemed;
bool need_save = false;

std::string GetPluginDir()
{
    return ArkApi::Tools::GetCurrentDir() + "/ArkApi/Plugins/PromoCodeReward";
}

std::string GetConfigPath()
{
    return GetPluginDir() + "/config.json";
}

std::string GetDataPath()
{
    return GetPluginDir() + "/data.json";
}

bool FileExists(const std::string& path)
{
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::path(path), ec);
}

int64_t NowUnix()
{
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

std::string NormalizeCode(std::string code)
{
    if (config.case_sensitive)
        return code;

    std::transform(code.begin(), code.end(), code.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return code;
}

FString Msg(const std::string& utf8)
{
    return FString(ArkApi::Tools::Utf8Decode(utf8).c_str());
}

void Send(AShooterPlayerController* pc, const std::string& utf8)
{
    if (!pc)
        return;
    const FString sender(L"Promo");
    const FString text = Msg(utf8);
    ArkApi::GetApiUtils().SendChatMessage(pc, sender, L"{}", *text);
    ArkApi::GetApiUtils().SendNotification(pc, FLinearColor(1.0f, 0.85f, 0.1f, 1.0f), 1.0f, 6.0f, nullptr, L"{}", *text);
}

void SaveDefaultConfig()
{
    std::filesystem::create_directories(std::filesystem::path(GetPluginDir()));

    nlohmann::json json;
    json["command"] = "/promo";
    json["case_sensitive"] = false;

    nlohmann::json promo;
    promo["code"] = "OPEN2026";
    promo["blueprint"] = "Blueprint'/Game/Mods/KsMissions/Items/PrimalItem_Goldcoin.PrimalItem_Goldcoin'";
    promo["quantity"] = 1;
    promo["quality"] = 1.0;
    promo["force_blueprint"] = false;
    promo["one_time_per_player"] = true;
    promo["max_total_uses"] = 0;
    json["promos"] = nlohmann::json::array({ promo });

    std::ofstream file(GetConfigPath(), std::ios::trunc);
    file << json.dump(2);
}

void LoadConfig()
{
    try
    {
        if (!FileExists(GetConfigPath()))
            SaveDefaultConfig();

        std::ifstream file(GetConfigPath());
        if (!file.is_open())
            return;

        nlohmann::json json;
        file >> json;

        config.command = json.value("command", config.command);
        config.case_sensitive = json.value("case_sensitive", config.case_sensitive);
        config.promos.clear();

        if (json.find("promos") != json.end() && json["promos"].is_array())
        {
            for (const auto& item : json["promos"])
            {
                PromoEntry p;
                p.code = item.value("code", "");
                p.blueprint = item.value("blueprint", "");
                p.quantity = item.value("quantity", 1);
                p.quality = item.value("quality", 1.0f);
                p.force_blueprint = item.value("force_blueprint", false);
                p.one_time_per_player = item.value("one_time_per_player", true);
                p.max_total_uses = item.value("max_total_uses", 0);

                if (!p.code.empty() && !p.blueprint.empty())
                    config.promos.push_back(std::move(p));
            }
        }
    }
    catch (...)
    {
        // Use defaults
    }
}

void SaveData()
{
    try
    {
        std::filesystem::create_directories(std::filesystem::path(GetPluginDir()));
        nlohmann::json json;

        nlohmann::json redeemed_json = nlohmann::json::object();
        {
            DataLock lock(data_mutex);
            for (const auto& code_it : redeemed)
            {
                nlohmann::json players = nlohmann::json::object();
                for (const auto& player_it : code_it.second)
                    players[player_it.first] = player_it.second;
                redeemed_json[code_it.first] = std::move(players);
            }
        }

        json["redeemed"] = std::move(redeemed_json);

        std::ofstream file(GetDataPath(), std::ios::trunc);
        file << json.dump(2);
        need_save = false;
    }
    catch (...)
    {
        // ignore
    }
}

void LoadData()
{
    try
    {
        if (!FileExists(GetDataPath()))
            return;

        std::ifstream file(GetDataPath());
        if (!file.is_open())
            return;

        nlohmann::json json;
        file >> json;
        if (json.find("redeemed") == json.end() || !json["redeemed"].is_object())
            return;

        DataLock lock(data_mutex);
        redeemed.clear();
        for (auto it = json["redeemed"].begin(); it != json["redeemed"].end(); ++it)
        {
            if (!it.value().is_object())
                continue;

            std::unordered_map<std::string, int64_t> players;
            for (auto pit = it.value().begin(); pit != it.value().end(); ++pit)
            {
                if (pit.value().is_number_integer())
                    players[pit.key()] = pit.value().get<int64_t>();
            }

            redeemed[it.key()] = std::move(players);
        }
    }
    catch (...)
    {
        // ignore
    }
}

const PromoEntry* FindPromo(const std::string& normalized_code)
{
    for (const auto& p : config.promos)
    {
        if (NormalizeCode(p.code) == normalized_code)
            return &p;
    }
    return nullptr;
}

int GetTotalUsesForCodeLocked(const std::string& normalized_code)
{
    auto it = redeemed.find(normalized_code);
    if (it == redeemed.end())
        return 0;
    return static_cast<int>(it->second.size());
}

bool HasRedeemedLocked(const std::string& normalized_code, const std::string& steam_id)
{
    auto it = redeemed.find(normalized_code);
    if (it == redeemed.end())
        return false;
    return it->second.find(steam_id) != it->second.end();
}

void MarkRedeemedLocked(const std::string& normalized_code, const std::string& steam_id)
{
    redeemed[normalized_code][steam_id] = NowUnix();
    need_save = true;
}

void CmdPromo(AShooterPlayerController* pc, FString* message, EChatSendMode::Type)
{
    if (!pc)
        return;

    if (!message)
    {
        Send(pc, "Использование: /promo <код>");
        return;
    }

    TArray<FString> parsed;
    message->ParseIntoArray(parsed, L" ", true);

    // expected: "/promo CODE" or "CODE"
    int arg_index = 0;
    if (parsed.Num() >= 1 && parsed[0].StartsWith(L"/"))
        arg_index = 1;

    if (parsed.Num() <= arg_index)
    {
        Send(pc, "Использование: /promo <код>");
        return;
    }

    const std::string raw_code = parsed[arg_index].ToString();
    const std::string normalized_code = NormalizeCode(raw_code);

    const PromoEntry* promo = FindPromo(normalized_code);
    if (!promo)
    {
        Send(pc, "Неверный промокод.");
        return;
    }

    const uint64 steam_id_u64 = ArkApi::IApiUtils::GetSteamIdFromController(pc);
    if (steam_id_u64 == 0)
    {
        Send(pc, "Не удалось определить ваш SteamID.");
        return;
    }
    const std::string steam_id = std::to_string(steam_id_u64);

    {
        DataLock lock(data_mutex);
        if (promo->one_time_per_player && HasRedeemedLocked(normalized_code, steam_id))
        {
            Send(pc, "Вы уже использовали этот промокод.");
            return;
        }

        if (promo->max_total_uses > 0 && GetTotalUsesForCodeLocked(normalized_code) >= promo->max_total_uses)
        {
            Send(pc, "Лимит использований промокода исчерпан.");
            return;
        }
    }

    FString bp(promo->blueprint.c_str());
    TArray<UPrimalItem*> out_items;
    const bool ok = pc->GiveItem(&out_items, &bp, promo->quantity, promo->quality, promo->force_blueprint, false, 0.0f);
    if (!ok)
    {
        Send(pc, "Не удалось выдать предмет (проверьте blueprint в конфиге).");
        return;
    }

    {
        DataLock lock(data_mutex);
        MarkRedeemedLocked(normalized_code, steam_id);
    }

    SaveData();
    Send(pc, "Промокод принят. Предмет выдан!");
}

void Load()
{
    LoadConfig();
    LoadData();

    if (config.command.empty())
        config.command = "/promo";

    ArkApi::GetCommands().AddChatCommand(config.command.c_str(), &CmdPromo);
}

void Unload()
{
    SaveData();
    if (!config.command.empty())
        ArkApi::GetCommands().RemoveChatCommand(config.command.c_str());
}

} // namespace

BOOL APIENTRY DllMain(HMODULE, DWORD ul_reason_for_call, LPVOID)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        Load();
        break;
    case DLL_PROCESS_DETACH:
        Unload();
        break;
    default:
        break;
    }
    return TRUE;
}
