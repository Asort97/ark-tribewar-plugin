#include <API/ARK/Ark.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <mutex>
#include <optional>
#include <atomic>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "json.hpp"
#include <filesystem>

#pragma comment(lib, "ArkApi.lib")

namespace
{
#define TRIBEWAR_ENABLE_RADIAL 0
#define TRIBEWAR_ENABLE_CHAT_COMMANDS 1
constexpr int kMenuRootId = 910000;
constexpr int kMenuDeclareId = 910001;
constexpr int kMenuStatusId = 910002;
constexpr int kMenuCancelId = 910003;
constexpr int kMenuAcceptCancelId = 910004;
constexpr int kMenuDeclareListBaseId = 910100;
constexpr int kMenuDeclareListMax = 64;

// MultiUse wheel identifiers MUST be small on some versions (often treated as a byte).
// Using huge values (910xxx) can cause options to not render or not be selectable.
constexpr int kMuDeclareListBaseId = 180; // 180..243 (64 entries)
constexpr int kMuStatusId = 244;
constexpr int kMuCancelId = 245;
constexpr int kMuAcceptCancelId = 246;

#if TRIBEWAR_ENABLE_RADIAL
// ArkApi headers in this workspace do not provide a native definition for the tribe radial menu entry.
// We provide a minimal definition that matches the fields we need to populate.
// If the game expects a different layout, this will need adjustment.
struct UTexture2D;

struct FTribeRadialMenuEntry
{
    FString EntryName;
    FString EntryDescription;
    UTexture2D* EntryIcon = nullptr;
    int EntryID = 0;
    int ParentID = 0;
    bool bIsSubmenu = false;
};
#endif
struct Config
{
    int32_t war_delay_seconds = 12 * 60 * 60;
    int32_t cooldown_seconds = 48 * 60 * 60;

    // Structure damage
    // Applied only when damage is allowed because of an active war (opposing sides).
    // 1.0 = normal damage, 0.5 = half damage, 0.0 = no structure damage during war.
    float structure_damage_multiplier = 1.0f;
    std::vector<std::string> excluded_structure_blueprints;

    // Abandoned tribes (tribe deleted / zero members)
    // If a tribe has zero members, its structures become attackable by anyone for this duration.
    bool enable_abandoned_structure_window = false;
    int32_t abandoned_structure_window_seconds = 12 * 60 * 60;
    float abandoned_structure_damage_multiplier = 1.0f;

    // UI integration
    // enable_multiuse_menu: adds actions to the existing MultiUse wheel (server-side, no client mod).
    // enable_tribe_radial_menu: experimental, depends on client/game version.
    bool enable_multiuse_menu = true;
    bool multiuse_require_owned_structure = true;
    bool multiuse_require_leader = true;
    int32_t multiuse_max_targets = 24;
    bool enable_tribe_radial_menu = false;

    // Diagnostics
    bool debug_multiuse_log = false;

    // Self-test mode: creates a synthetic war and drives it through phases
    // so that functionality can be validated without any players.
    bool self_test = false;
    int64_t self_test_tribe_a = 111111;
    int64_t self_test_tribe_b = 222222;
    int32_t self_test_active_seconds = 15;
};

struct WarRecord
{
    int64_t war_id = 0;
    int64_t tribe_a = 0;
    int64_t tribe_b = 0;
    int64_t declared_at = 0;
    int64_t start_at = 0;
    int64_t ended_at = 0;
    int64_t cooldown_end_a = 0;
    int64_t cooldown_end_b = 0;
    bool cancel_requested_by_a = false;
    bool cancel_requested_by_b = false;
    bool start_notified = false;
    bool cooldown_notified = false;
};

enum class WarPhase
{
    None,
    Pending,
    Active,
    Cooldown
};

// std::mutex has been observed to crash in this environment (inside MSVCP140.dll).
// Use a WinAPI SRWLOCK-based mutex to avoid STL runtime mutex internals.
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

using DataMutex = WinMutex;
using DataLockGuard = std::lock_guard<DataMutex>;

DataMutex data_mutex;
Config config;
std::unordered_map<int64_t, WarRecord> wars_by_id;
std::unordered_map<int64_t, int64_t> tribe_to_war_id;
std::unordered_map<uint64_t, std::unordered_map<int, int64_t>> declare_targets;
std::unordered_map<int64_t, int64_t> abandoned_tribe_until; // tribe_id -> unix_ts
std::unordered_map<int64_t, FString> tribe_name_cache;
std::atomic<bool> tribe_name_dirty { false };

// Dynamic UseIndex mapping: for each player, store what UseIndex maps to what action.
// action codes: 1=status, 2=cancel, 3=accept_cancel, 100+N=declare_target[N]
std::unordered_map<uint64_t, std::unordered_map<int, int>> multiuse_action_map;

int64_t next_war_id = 1;
bool auto_timers_enabled = true;
bool plugin_initialized = false;
std::atomic<bool> need_save { false };
struct PendingNotification
{
    int64_t side_tribe_id = 0;
    FString message;
    bool styled = false;
    FLinearColor color = FLinearColor(1.0f, 0.85f, 0.1f, 1.0f);
    float scale = 1.0f;
    float time = 6.0f;
};

std::vector<PendingNotification> pending_notifications;
DataMutex notification_mutex;


std::string GetPluginDir()
{
    return ArkApi::Tools::GetCurrentDir() + "/ArkApi/Plugins/TribeWarSystem";
}

std::string GetConfigPath()
{
    return GetPluginDir() + "/config.json";
}

std::string GetDataPath()
{
    return GetPluginDir() + "/data.json";
}

std::string GetTribeNameCachePath()
{
    return GetPluginDir() + "/tribe_names.json";
}

AShooterPlayerState* GetPlayerState(AShooterPlayerController* pc);
int64_t CanonicalTribeId(int64_t raw_id);
int64_t GetTribeIdFromPlayer(AShooterPlayerController* pc);
bool TryGetPathNameSafe(UObject* obj, FString* out_path);
bool TryGetTribeNameSafe(FTribeData* data, FString* out_name);
int32_t TryGetTribeMemberCount(FTribeData& data, int32_t& out_tribe_id);

std::string ToLowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string NormalizeBlueprintPath(const std::string& path)
{
    std::string lower = ToLowerAscii(path);
    const std::string prefix = "blueprint'";
    if (lower.rfind(prefix, 0) == 0)
        lower = lower.substr(prefix.size());
    if (!lower.empty() && lower.back() == '\'')
        lower.pop_back();
    return lower;
}

bool TryGetPathNameSafe(UObject* obj, FString* out_path)
{
    if (!obj || !out_path)
        return false;

    if (!obj->IsValidLowLevelFast(true))
        return false;

    __try
    {
        obj->GetPathName(out_path, nullptr);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool TryGetTribeNameSafe(FTribeData* data, FString* out_name)
{
    if (!data || !out_name)
        return false;

    __try
    {
        *out_name = data->TribeNameField();
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

std::string GetSelfTestLogPath()
{
    return GetPluginDir() + "/self_test.log";
}

std::string GetMultiUseDebugLogPath()
{
    return GetPluginDir() + "/multiuse_debug.log";
}

bool FileExists(const std::string& path)
{
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::path(path), ec);
}

void LoadTribeNameCache()
{
    try
    {
        if (!FileExists(GetTribeNameCachePath()))
            return;

        std::ifstream file(GetTribeNameCachePath(), std::ios::in | std::ios::binary);
        if (!file.is_open())
            return;

        const std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        const auto json = nlohmann::json::parse(content, nullptr, false);
        if (json.is_discarded())
            return;

        if (json.find("names") == json.end() || !json["names"].is_object())
            return;

        DataLockGuard lock(data_mutex);
        tribe_name_cache.clear();
        for (auto it = json["names"].begin(); it != json["names"].end(); ++it)
        {
            if (!it.value().is_string())
                continue;
            const int64_t id = CanonicalTribeId(std::stoll(it.key()));
            tribe_name_cache[id] = FString(it.value().get<std::string>().c_str());
        }
    }
    catch (...)
    {
        // ignore
    }
}

void SaveTribeNameCache()
{
    if (!tribe_name_dirty.exchange(false))
        return;

    try
    {
        nlohmann::json json;
        nlohmann::json names = nlohmann::json::object();
        {
            DataLockGuard lock(data_mutex);
            for (const auto& it : tribe_name_cache)
                names[std::to_string(it.first)] = it.second.ToString();
        }
        json["names"] = std::move(names);

        std::filesystem::create_directories(std::filesystem::path(GetTribeNameCachePath()).parent_path());
        std::ofstream file(GetTribeNameCachePath(), std::ios::trunc);
        file << json.dump(2);
    }
    catch (...)
    {
        // ignore
    }
}

FString GetCachedTribeName(int64_t tribe_id)
{
    tribe_id = CanonicalTribeId(tribe_id);
    DataLockGuard lock(data_mutex);
    auto it = tribe_name_cache.find(tribe_id);
    if (it == tribe_name_cache.end())
        return FString();
    return it->second;
}

void CacheTribeName(int64_t tribe_id, const FString& name)
{
    if (name.IsEmpty())
        return;

    tribe_id = CanonicalTribeId(tribe_id);
    bool updated = false;
    {
        DataLockGuard lock(data_mutex);
        auto it = tribe_name_cache.find(tribe_id);
        if (it == tribe_name_cache.end() || it->second != name)
        {
            tribe_name_cache[tribe_id] = name;
            updated = true;
        }
    }

    if (updated)
        tribe_name_dirty.store(true);
}

bool TryResolveTribeName(int64_t tribe_id, FString& out_name)
{
    tribe_id = CanonicalTribeId(tribe_id);
    if (tribe_id == 0)
        return false;

    out_name = GetCachedTribeName(tribe_id);
    if (!out_name.IsEmpty())
        return true;

    if (ArkApi::GetApiUtils().GetStatus() == ArkApi::ServerStatus::Ready)
    {
        if (auto* world = ArkApi::GetApiUtils().GetWorld())
        {
            auto& players = world->PlayerControllerListField();
            for (TWeakObjectPtr<APlayerController>& player : players)
            {
                auto* pc = static_cast<AShooterPlayerController*>(player.Get());
                if (!pc || GetTribeIdFromPlayer(pc) != tribe_id)
                    continue;

                if (auto* ps = GetPlayerState(pc))
                {
                    if (auto* data = ps->MyTribeDataField())
                        TryGetTribeNameSafe(data, &out_name);
                }

                if (out_name.IsEmpty())
                {
                    if (auto* ch = pc->GetPlayerCharacter())
                        out_name = ch->TribeNameField();
                }

                if (!out_name.IsEmpty())
                {
                    CacheTribeName(tribe_id, out_name);
                    return true;
                }
            }
        }

        if (auto* game_mode = ArkApi::GetApiUtils().GetShooterGameMode())
        {
            const auto& tribes = game_mode->TribesDataField();
            for (int i = 0; i < tribes.Num(); ++i)
            {
                auto& data = const_cast<FTribeData&>(tribes[i]);
                int32_t tid = 0;
                if (TryGetTribeMemberCount(data, tid) < 0)
                    continue;
                if (CanonicalTribeId(static_cast<int64_t>(tid)) != tribe_id)
                    continue;
                TryGetTribeNameSafe(&data, &out_name);
                if (!out_name.IsEmpty())
                {
                    CacheTribeName(tribe_id, out_name);
                    return true;
                }
            }
        }
    }

    return false;
}

FString GetTribeDisplayName(int64_t tribe_id)
{
    FString name;
    if (!TryResolveTribeName(tribe_id, name))
        name = GetCachedTribeName(tribe_id);

    if (!name.IsEmpty())
    {
        CacheTribeName(tribe_id, name);
        return FString::Format(L"{} (ID: {})", *name, tribe_id);
    }
    return FString::Format(L"ID: {}", CanonicalTribeId(tribe_id));
}

void UpdateTribeNameCache()
{
    if (ArkApi::GetApiUtils().GetStatus() != ArkApi::ServerStatus::Ready)
        return;

    auto* world = ArkApi::GetApiUtils().GetWorld();
    if (!world)
        return;

    auto* game_mode = ArkApi::GetApiUtils().GetShooterGameMode();

    auto& players = world->PlayerControllerListField();
    for (TWeakObjectPtr<APlayerController>& player : players)
    {
        auto* pc = static_cast<AShooterPlayerController*>(player.Get());
        if (!pc)
            continue;

        const int64_t tribe_id = GetTribeIdFromPlayer(pc);
        if (tribe_id == 0)
            continue;

        FString name;
        if (auto* ps = GetPlayerState(pc))
        {
            if (auto* data = ps->MyTribeDataField())
                TryGetTribeNameSafe(data, &name);
        }

        if (name.IsEmpty())
        {
            auto* ch = pc->GetPlayerCharacter();
            if (ch)
                name = ch->TribeNameField();
        }

        if (name.IsEmpty())
            continue;

        bool updated = false;
        {
            DataLockGuard lock(data_mutex);
            auto it = tribe_name_cache.find(tribe_id);
            if (it == tribe_name_cache.end() || it->second != name)
            {
                tribe_name_cache[tribe_id] = name;
                updated = true;
            }
        }

        if (updated)
            tribe_name_dirty.store(true);
    }

    // Fallback: best-effort fill from TribesDataField (covers offline tribes).
    if (game_mode)
    {
        const auto& tribes = game_mode->TribesDataField();
        for (int i = 0; i < tribes.Num(); ++i)
        {
            auto& data = const_cast<FTribeData&>(tribes[i]);
            int32_t tid = 0;
            const int32_t members = TryGetTribeMemberCount(data, tid);
            if (tid <= 0 || members < 0)
                continue;

            const int64_t tribe_id = CanonicalTribeId(static_cast<int64_t>(tid));
            FString name;
            if (!TryGetTribeNameSafe(&data, &name) || name.IsEmpty())
                continue;

            bool updated = false;
            {
                DataLockGuard lock(data_mutex);
                auto it = tribe_name_cache.find(tribe_id);
                if (it == tribe_name_cache.end() || it->second != name)
                {
                    tribe_name_cache[tribe_id] = name;
                    updated = true;
                }
            }

            if (updated)
                tribe_name_dirty.store(true);
        }
    }
}

int64_t Now();

void AppendSelfTestLog(const std::string& message)
{
    if (!config.self_test)
        return;

    try
    {
        std::filesystem::create_directories(std::filesystem::path(GetSelfTestLogPath()).parent_path());
        std::ofstream f(GetSelfTestLogPath(), std::ios::app);
        if (!f.is_open())
            return;
        f << Now() << " " << message << "\n";
    }
    catch (...)
    {
    }
}

void AppendMultiUseDebugLog(const std::string& message)
{
    if (!config.debug_multiuse_log)
        return;

    try
    {
        std::filesystem::create_directories(std::filesystem::path(GetMultiUseDebugLogPath()).parent_path());
        std::ofstream f(GetMultiUseDebugLogPath(), std::ios::app);
        if (!f.is_open())
            return;
        f << Now() << " " << message << "\n";
    }
    catch (...)
    {
    }
}

int64_t Now()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

int64_t CanonicalTribeId(int64_t raw_id)
{
    // ARK tribe/team IDs are effectively 32-bit values.
    // Normalize to unsigned 32-bit to avoid negative IDs in UI and comparisons.
    const auto as_i32 = static_cast<int32_t>(raw_id);
    const auto as_u32 = static_cast<uint32_t>(as_i32);
    return static_cast<int64_t>(as_u32);
}

WarPhase GetPhase(const WarRecord& war, int64_t now)
{
    if (war.war_id == 0)
        return WarPhase::None;

    if (war.ended_at == 0)
    {
        if (now < war.start_at)
            return WarPhase::Pending;
        return WarPhase::Active;
    }

    if (now < war.cooldown_end_a || now < war.cooldown_end_b)
        return WarPhase::Cooldown;

    return WarPhase::None;
}

bool IsActiveWar(const WarRecord& war, int64_t now)
{
    return GetPhase(war, now) == WarPhase::Active;
}

void RebuildTribeIndexLocked(int64_t now)
{
    tribe_to_war_id.clear();
    for (const auto& it : wars_by_id)
    {
        const auto& war = it.second;
        const auto phase = GetPhase(war, now);
        if (phase == WarPhase::None)
            continue;
        tribe_to_war_id[war.tribe_a] = war.war_id;
        tribe_to_war_id[war.tribe_b] = war.war_id;
    }
}

void SaveData()
{
    try
    {
        int64_t snapshot_next_war_id = 1;
        std::vector<WarRecord> snapshot_wars;
        {
            DataLockGuard lock(data_mutex);
            snapshot_next_war_id = next_war_id;
            snapshot_wars.reserve(wars_by_id.size());
            for (const auto& it : wars_by_id)
                snapshot_wars.push_back(it.second);
        }

        const auto path = GetDataPath();
        std::filesystem::create_directories(
            std::filesystem::path(path).parent_path()
        );

        std::ofstream file(path, std::ios::trunc);
        if (!file.is_open())
        {
            AppendSelfTestLog("SaveData: failed to open data.json");
            return;
        }

        nlohmann::json json;
        json["next_war_id"] = snapshot_next_war_id;
        json["wars"] = nlohmann::json::array();

        for (const auto& war : snapshot_wars)
        {
            nlohmann::json item;
            item["war_id"] = war.war_id;
            item["tribe_a"] = war.tribe_a;
            item["tribe_b"] = war.tribe_b;
            item["declared_at"] = war.declared_at;
            item["start_at"] = war.start_at;
            item["ended_at"] = war.ended_at;
            item["cooldown_end_a"] = war.cooldown_end_a;
            item["cooldown_end_b"] = war.cooldown_end_b;
            item["cancel_requested_by_a"] = war.cancel_requested_by_a;
            item["cancel_requested_by_b"] = war.cancel_requested_by_b;
            item["start_notified"] = war.start_notified;
            item["cooldown_notified"] = war.cooldown_notified;
            json["wars"].push_back(item);
        }

        file << json.dump(2);
        AppendSelfTestLog("SaveData: wrote data.json (wars=" + std::to_string(snapshot_wars.size()) + ")");
    }
    catch (...)
    {
        // Silent fail to avoid crash
        AppendSelfTestLog("SaveData: exception");
    }
}

void LoadData()
{
    try
    {
        std::ifstream file(GetDataPath(), std::ios::in | std::ios::binary);
        if (!file.is_open())
            return;

        const std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        const auto json = nlohmann::json::parse(content, nullptr, false);
        if (json.is_discarded())
            return;

        int64_t loaded_next_war_id = json.value("next_war_id", static_cast<int64_t>(1));

        std::vector<WarRecord> loaded_wars;
        const int64_t now = Now();
        const int64_t max_cooldown = config.cooldown_seconds * 2 + 3600; // allow 2x cooldown + 1hr buffer

        if (json.find("wars") != json.end())
        {
            for (const auto& item : json["wars"])
            {
                WarRecord war;
                war.war_id = item.value("war_id", 0);
                war.tribe_a = CanonicalTribeId(item.value("tribe_a", static_cast<int64_t>(0)));
                war.tribe_b = CanonicalTribeId(item.value("tribe_b", static_cast<int64_t>(0)));
                war.declared_at = item.value("declared_at", 0);
                war.start_at = item.value("start_at", 0);
                war.ended_at = item.value("ended_at", 0);
                war.cooldown_end_a = item.value("cooldown_end_a", 0);
                war.cooldown_end_b = item.value("cooldown_end_b", 0);
                war.cancel_requested_by_a = item.value("cancel_requested_by_a", false);
                war.cancel_requested_by_b = item.value("cancel_requested_by_b", false);
                war.start_notified = item.value("start_notified", false);
                war.cooldown_notified = item.value("cooldown_notified", false);
                
                if (war.war_id <= 0)
                    continue;
                if (war.tribe_a == 0 || war.tribe_b == 0)
                    continue;
                    
                // BUGFIX: Ignore expired wars (avoid stale data from file)
                // If war is ended and both cooldowns are expired, skip it
                if (war.ended_at != 0)
                {
                    if (war.cooldown_end_a > 0 && war.cooldown_end_b > 0 &&
                        now >= war.cooldown_end_a && now >= war.cooldown_end_b)
                    {
                        // War is completely done, discard it
                        continue;
                    }
                    // Also discard if war is WAY too old (data corruption safety)
                    if (war.declared_at > 0 && now - war.declared_at > max_cooldown)
                        continue;
                }
                
                loaded_wars.push_back(war);
            }
        }

        {
            DataLockGuard lock(data_mutex);
            wars_by_id.clear();
            tribe_to_war_id.clear();
            next_war_id = loaded_next_war_id;
            for (const auto& war : loaded_wars)
                wars_by_id[war.war_id] = war;
            RebuildTribeIndexLocked(Now());
        }
    }
    catch (...)
    {
        DataLockGuard lock(data_mutex);
        wars_by_id.clear();
        tribe_to_war_id.clear();
        next_war_id = 1;
    }
}

void FlushSaveIfNeeded()
{
    static int64_t last_save = 0;
    const auto now = Now();
    if (!need_save.exchange(false))
        return;
    if (now - last_save < 30)
        return;
    SaveData();
    last_save = now;
}

void SaveConfig()
{
    std::ofstream file(GetConfigPath(), std::ios::trunc);
    nlohmann::json json;
    json["war_delay_seconds"] = config.war_delay_seconds;
    json["cooldown_seconds"] = config.cooldown_seconds;
    json["structure_damage_multiplier"] = config.structure_damage_multiplier;
    json["excluded_structure_blueprints"] = config.excluded_structure_blueprints;
    json["enable_abandoned_structure_window"] = config.enable_abandoned_structure_window;
    json["abandoned_structure_window_seconds"] = config.abandoned_structure_window_seconds;
    json["abandoned_structure_damage_multiplier"] = config.abandoned_structure_damage_multiplier;

    json["enable_multiuse_menu"] = config.enable_multiuse_menu;
    json["multiuse_require_owned_structure"] = config.multiuse_require_owned_structure;
    json["multiuse_require_leader"] = config.multiuse_require_leader;
    json["multiuse_max_targets"] = config.multiuse_max_targets;
    json["enable_tribe_radial_menu"] = config.enable_tribe_radial_menu;

    json["debug_multiuse_log"] = config.debug_multiuse_log;

    json["self_test"] = config.self_test;
    json["self_test_tribe_a"] = config.self_test_tribe_a;
    json["self_test_tribe_b"] = config.self_test_tribe_b;
    json["self_test_active_seconds"] = config.self_test_active_seconds;

    file << json.dump(2);
}

void LoadConfig()
{
    try
    {
        std::ifstream file(GetConfigPath());
        if (!file.is_open())
        {
            SaveConfig();
            return;
        }

        nlohmann::json json;
        file >> json;
        config.war_delay_seconds = json.value("war_delay_seconds", config.war_delay_seconds);
        config.cooldown_seconds = json.value("cooldown_seconds", config.cooldown_seconds);
        config.structure_damage_multiplier = json.value("structure_damage_multiplier", config.structure_damage_multiplier);
        config.excluded_structure_blueprints.clear();
        if (json.find("excluded_structure_blueprints") != json.end() && json["excluded_structure_blueprints"].is_array())
        {
            for (const auto& item : json["excluded_structure_blueprints"])
            {
                if (!item.is_string())
                    continue;
                const auto normalized = NormalizeBlueprintPath(item.get<std::string>());
                if (!normalized.empty())
                    config.excluded_structure_blueprints.push_back(normalized);
            }
        }
        config.enable_abandoned_structure_window = json.value("enable_abandoned_structure_window", config.enable_abandoned_structure_window);
        config.abandoned_structure_window_seconds = json.value("abandoned_structure_window_seconds", config.abandoned_structure_window_seconds);
        config.abandoned_structure_damage_multiplier = json.value("abandoned_structure_damage_multiplier", config.abandoned_structure_damage_multiplier);

        config.enable_multiuse_menu = json.value("enable_multiuse_menu", config.enable_multiuse_menu);
        config.multiuse_require_owned_structure = json.value("multiuse_require_owned_structure", config.multiuse_require_owned_structure);
        config.multiuse_require_leader = json.value("multiuse_require_leader", config.multiuse_require_leader);
        config.multiuse_max_targets = json.value("multiuse_max_targets", config.multiuse_max_targets);
        config.enable_tribe_radial_menu = json.value("enable_tribe_radial_menu", config.enable_tribe_radial_menu);

        config.debug_multiuse_log = json.value("debug_multiuse_log", config.debug_multiuse_log);

        config.self_test = json.value("self_test", config.self_test);
        config.self_test_tribe_a = json.value("self_test_tribe_a", config.self_test_tribe_a);
        config.self_test_tribe_b = json.value("self_test_tribe_b", config.self_test_tribe_b);
        config.self_test_active_seconds = json.value("self_test_active_seconds", config.self_test_active_seconds);
    }
    catch (...)
    {
        // Silent fail, use defaults
    }
}

void SeedSelfTestWarIfNeeded()
{
    if (!config.self_test)
        return;

    const auto now = Now();

    DataLockGuard lock(data_mutex);
    if (!wars_by_id.empty())
        return;

    const auto a = config.self_test_tribe_a;
    const auto b = config.self_test_tribe_b;
    if (a == 0 || b == 0 || a == b)
        return;

    WarRecord war;
    war.war_id = next_war_id++;
    war.tribe_a = a;
    war.tribe_b = b;
    war.declared_at = now;
    war.start_at = now + config.war_delay_seconds;
    wars_by_id[war.war_id] = war;
    RebuildTribeIndexLocked(now);

    AppendSelfTestLog("SeedSelfTestWar: created war_id=" + std::to_string(war.war_id) +
                      " a=" + std::to_string(a) + " b=" + std::to_string(b) +
                      " start_in=" + std::to_string(config.war_delay_seconds) + "s");
}

int64_t GetTribeIdFromActor(AActor* actor)
{
    if (!actor)
        return 0;

    if (actor->IsA(AShooterPlayerController::StaticClass()))
    {
        auto* pc = static_cast<AShooterPlayerController*>(actor);
        return CanonicalTribeId(pc->TargetingTeamField());
    }

    if (actor->IsA(APrimalCharacter::StaticClass()))
    {
        auto* ch = static_cast<APrimalCharacter*>(actor);
        return CanonicalTribeId(ch->TargetingTeamField());
    }

    if (actor->IsA(APrimalDinoCharacter::StaticClass()))
    {
        auto* dino = static_cast<APrimalDinoCharacter*>(actor);
        return CanonicalTribeId(dino->TargetingTeamField());
    }

    if (actor->IsA(AController::StaticClass()))
    {
        auto* controller = static_cast<AController*>(actor);
        return CanonicalTribeId(controller->TargetingTeamField());
    }

    return 0;
}

int64_t GetTribeIdFromPlayer(AShooterPlayerController* pc)
{
    if (!pc)
        return 0;
    const auto raw = pc->TargetingTeamField();
    const auto canonical = CanonicalTribeId(raw);
    return canonical;
}

int32_t TryGetTribeMemberCount(FTribeData& data, int32_t& out_tribe_id)
{
    __try
    {
        out_tribe_id = data.TribeIDField();
        int32_t count = data.MembersPlayerDataIDField().Num();
        if (count <= 0)
            count = data.MembersPlayerNameField().Num();
        return count;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        out_tribe_id = 0;
        return -1;
    }
}

void UpdateAbandonedTribes(int64_t now)
{
    if (!config.enable_abandoned_structure_window)
        return;

    if (ArkApi::GetApiUtils().GetStatus() != ArkApi::ServerStatus::Ready)
        return;

    auto* game_mode = ArkApi::GetApiUtils().GetShooterGameMode();
    if (!game_mode)
        return;

    const int32_t window = (std::max)(0, config.abandoned_structure_window_seconds);
    if (window <= 0)
        return;

    try
    {
        const auto& tribes = game_mode->TribesDataField();
        DataLockGuard lock(data_mutex);

        // Cleanup expired entries (keep map small).
        for (auto it = abandoned_tribe_until.begin(); it != abandoned_tribe_until.end();)
        {
            if (it->second <= now)
                it = abandoned_tribe_until.erase(it);
            else
                ++it;
        }

        for (int i = 0; i < tribes.Num(); ++i)
        {
            auto& data = const_cast<FTribeData&>(tribes[i]);
            int32_t tid = 0;
            const int32_t members = TryGetTribeMemberCount(data, tid);
            if (tid <= 0 || members < 0)
                continue;

            if (members == 0)
            {
                // Start/refresh the window from "now" when tribe is observed as empty.
                abandoned_tribe_until[CanonicalTribeId(static_cast<int64_t>(tid))] = now + window;
            }
            else
            {
                abandoned_tribe_until.erase(CanonicalTribeId(static_cast<int64_t>(tid)));
            }
        }
    }
    catch (...)
    {
        // ignore
    }
}

bool IsAbandonedStructureVulnerable(int64_t target_tribe_id, int64_t now, float& out_multiplier)
{
    out_multiplier = 1.0f;
    if (!config.enable_abandoned_structure_window)
        return false;
    target_tribe_id = CanonicalTribeId(target_tribe_id);
    if (target_tribe_id == 0)
        return false;

    const float mult = config.abandoned_structure_damage_multiplier;
    DataLockGuard lock(data_mutex);
    auto it = abandoned_tribe_until.find(target_tribe_id);
    if (it == abandoned_tribe_until.end())
        return false;
    if (it->second <= now)
        return false;

    out_multiplier = mult;
    return true;
}

bool IsExcludedStructure(APrimalStructure* structure)
{
    if (!structure)
        return false;
    if (config.excluded_structure_blueprints.empty())
        return false;

    auto* cls = structure->ClassField();
    if (!cls)
        return false;
    if (!cls->IsValidLowLevelFast(true))
        return false;

    FString path;
    if (!TryGetPathNameSafe(cls, &path))
        return false;
    const auto normalized = NormalizeBlueprintPath(path.ToString());
    if (normalized.empty())
        return false;

    for (const auto& excluded : config.excluded_structure_blueprints)
    {
        if (normalized.find(excluded) != std::string::npos)
            return true;
    }

    return false;
}

AShooterPlayerState* GetPlayerState(AShooterPlayerController* pc)
{
    if (!pc)
        return nullptr;
    return pc->GetShooterPlayerState();
}

bool IsTribeLeaderOrAdmin(AShooterPlayerController* pc)
{
    if (!pc)
        return false;

    if (pc->IsTribeAdmin())
        return true;

    auto* ps = GetPlayerState(pc);
    if (!ps)
        return false;

    if (ps->IsTribeFounder())
        return true;

    auto* data_struct = ps->MyPlayerDataStructField();
    if (!data_struct)
        return false;

    const auto player_data_id = data_struct->PlayerDataIDField();
    
    // BUGFIX: Check multiple conditions for tribe owner status
    // In case founder/owner flags don't sync properly after tribe recreation
    if (ps->IsTribeOwner(static_cast<unsigned int>(player_data_id)))
        return true;
    
    // FALLBACK: If player has valid MyTribeData, they're in a tribe
    // Allow them to use war commands (especially after tribe recreation)
    auto* my_tribe = ps->MyTribeDataField();
    if (my_tribe)
        return true;

    return false;
}

bool IsTribeLeaderOrAdminOnline(int64_t tribe_id)
{
    if (tribe_id == 0)
        return false;

    if (ArkApi::GetApiUtils().GetStatus() != ArkApi::ServerStatus::Ready)
        return false;

    auto* world = ArkApi::GetApiUtils().GetWorld();
    if (!world)
        return false;

    auto& players = world->PlayerControllerListField();
    
    // BUGFIX: Simply check if ANY member of the tribe is online
    // Don't require strict leader/admin status verification (can be out of sync after tribe creation)
    // ALSO: Verify PlayerController is actually valid (not disconnected/pending)
    for (TWeakObjectPtr<APlayerController>& player : players)
    {
        auto* pc = static_cast<AShooterPlayerController*>(player.Get());
        if (!pc)
            continue;
        
        // CRITICAL: Check validity - avoid disconnected/pending controllers
        if (!pc->IsValidLowLevelFast(true))
            continue;
        if (!pc->IsA(AShooterPlayerController::StaticClass()))
            continue;
            
        const int64_t player_tribe = GetTribeIdFromPlayer(pc);
        if (player_tribe == tribe_id)
        {
            // Found at least one VALID online member of this tribe
            // Tribe can receive war declarations
            return true;
        }
    }

    return false;
}

void SendPlayerMessage(AShooterPlayerController* pc, const FString& message)
{
    if (!pc)
        return;
    const FString sender_name(L"Mega Tribe War");
    ArkApi::GetApiUtils().SendChatMessage(pc, sender_name, L"{}", *message);
    ArkApi::GetApiUtils().SendNotification(pc, FLinearColor(1.0f, 0.85f, 0.1f, 1.0f), 1.0f, 6.0f, nullptr, L"{}", *message);
}

void SendPlayerMessageStyled(AShooterPlayerController* pc, const FString& message, const FLinearColor& color, float scale, float time)
{
    if (!pc)
        return;
    const FString sender_name(L"Mega Tribe War");
    ArkApi::GetApiUtils().SendChatMessage(pc, sender_name, L"{}", *message);
    ArkApi::GetApiUtils().SendNotification(pc, color, scale, time, nullptr, L"{}", *message);
}

// Notify all online players who are on the given side: the tribe itself + allied tribes.
void NotifySide(int64_t side_tribe_id, const FString& message)
{
    if (ArkApi::GetApiUtils().GetStatus() != ArkApi::ServerStatus::Ready)
        return;

    auto* world = ArkApi::GetApiUtils().GetWorld();
    if (!world)
        return;

    auto* game_mode = ArkApi::GetApiUtils().GetShooterGameMode();

    auto& players = world->PlayerControllerListField();
    for (TWeakObjectPtr<APlayerController>& player : players)
    {
        auto* pc = static_cast<AShooterPlayerController*>(player.Get());
        if (!pc)
            continue;

        const auto player_tribe_id = GetTribeIdFromPlayer(pc);
        if (player_tribe_id == 0 || side_tribe_id == 0)
            continue;

        if (player_tribe_id == side_tribe_id)
        {
            SendPlayerMessage(pc, message);
            continue;
        }

        if (game_mode && game_mode->AreTribesAllied(static_cast<int>(player_tribe_id), static_cast<int>(side_tribe_id)))
        {
            SendPlayerMessage(pc, message);
            continue;
        }
    }
}

void NotifySideStyled(int64_t side_tribe_id, const FString& message, const FLinearColor& color, float scale, float time)
{
    if (ArkApi::GetApiUtils().GetStatus() != ArkApi::ServerStatus::Ready)
        return;

    auto* world = ArkApi::GetApiUtils().GetWorld();
    if (!world)
        return;

    auto* game_mode = ArkApi::GetApiUtils().GetShooterGameMode();

    auto& players = world->PlayerControllerListField();
    for (TWeakObjectPtr<APlayerController>& player : players)
    {
        auto* pc = static_cast<AShooterPlayerController*>(player.Get());
        if (!pc)
            continue;

        const auto player_tribe_id = GetTribeIdFromPlayer(pc);
        if (player_tribe_id == 0 || side_tribe_id == 0)
            continue;

        if (player_tribe_id == side_tribe_id)
        {
            SendPlayerMessageStyled(pc, message, color, scale, time);
            continue;
        }

        if (game_mode && game_mode->AreTribesAllied(static_cast<int>(player_tribe_id), static_cast<int>(side_tribe_id)))
        {
            SendPlayerMessageStyled(pc, message, color, scale, time);
            continue;
        }
    }
}

FString FormatDuration(int64_t seconds)
{
    const int64_t hours = seconds / 3600;
    const int64_t minutes = (seconds % 3600) / 60;
    const int64_t secs = seconds % 60;
    return FString::Format(L"{}ч {}м {}с", hours, minutes, secs);
}

WarRecord* GetWarForTribeLocked(int64_t tribe_id)
{
    auto it = tribe_to_war_id.find(tribe_id);
    if (it == tribe_to_war_id.end())
        return nullptr;

    auto war_it = wars_by_id.find(it->second);
    if (war_it == wars_by_id.end())
        return nullptr;

    return &war_it->second;
}

std::optional<WarRecord> GetWarForTribeCopy(int64_t tribe_id)
{
    tribe_id = CanonicalTribeId(tribe_id);
    DataLockGuard lock(data_mutex);
    if (auto* war = GetWarForTribeLocked(tribe_id))
    {
        if (GetPhase(*war, Now()) == WarPhase::None)
            return std::nullopt;
        return *war;
    }
    return std::nullopt;
}

struct WarView
{
    WarRecord war;
    int64_t side_root = 0; // war.tribe_a or war.tribe_b that this tribe belongs to (directly or via alliance)
};

std::optional<WarView> GetWarForSideCopy(int64_t tribe_id)
{
    if (tribe_id == 0)
        return std::nullopt;

    // Fast path: direct participant.
    if (auto direct = GetWarForTribeCopy(tribe_id))
    {
        const auto& war = *direct;
        const int64_t side_root = (tribe_id == war.tribe_a) ? war.tribe_a : war.tribe_b;
        return WarView{ war, side_root };
    }

    auto* game_mode = ArkApi::GetApiUtils().GetShooterGameMode();
    if (!game_mode)
        return std::nullopt;

    const auto now = Now();
    DataLockGuard lock(data_mutex);
    for (const auto& it : wars_by_id)
    {
        const auto& war = it.second;
        if (GetPhase(war, now) == WarPhase::None)
            continue;

        const bool on_a = (tribe_id == war.tribe_a) ||
                          game_mode->AreTribesAllied(static_cast<int>(tribe_id), static_cast<int>(war.tribe_a));
        const bool on_b = (tribe_id == war.tribe_b) ||
                          game_mode->AreTribesAllied(static_cast<int>(tribe_id), static_cast<int>(war.tribe_b));

        if (on_a == on_b)
            continue;

        return WarView{ war, on_a ? war.tribe_a : war.tribe_b };
    }

    return std::nullopt;
}

bool IsTribeInCooldown(int64_t tribe_id, int64_t now)
{
    tribe_id = CanonicalTribeId(tribe_id);
    DataLockGuard lock(data_mutex);
    auto* war = GetWarForTribeLocked(tribe_id);
    if (!war)
        return false;
    if (war->ended_at == 0)
        return false;

    if (GetPhase(*war, now) != WarPhase::Cooldown)
        return false;

    if (tribe_id == war->tribe_a)
        return now < war->cooldown_end_a;
    if (tribe_id == war->tribe_b)
        return now < war->cooldown_end_b;
    return false;
}

void RemoveWarLocked(int64_t war_id, int64_t now)
{
    wars_by_id.erase(war_id);
    RebuildTribeIndexLocked(now);
}

bool CleanupExpiredWarsLocked(int64_t now)
{
    std::vector<int64_t> to_remove;
    for (const auto& it : wars_by_id)
    {
        const auto& war = it.second;
        if (war.ended_at == 0)
            continue;
        if (war.cooldown_end_a <= 0 || war.cooldown_end_b <= 0)
            continue;
        if (now >= war.cooldown_end_a && now >= war.cooldown_end_b)
            to_remove.push_back(it.first);
    }

    for (const auto war_id : to_remove)
        RemoveWarLocked(war_id, now);

    return !to_remove.empty();
}

bool IsWarAllowed(int64_t tribe_a, int64_t tribe_b, int64_t now, FString& reason)
{
    tribe_a = CanonicalTribeId(tribe_a);
    tribe_b = CanonicalTribeId(tribe_b);
    if (tribe_a == 0 || tribe_b == 0)
    {
        reason = L"Вы должны состоять в племени.";
        return false;
    }

    if (tribe_a == tribe_b)
    {
        reason = L"Нельзя объявить войну своему племени.";
        return false;
    }

    if (ArkApi::GetApiUtils().GetStatus() == ArkApi::ServerStatus::Ready)
    {
        auto* game_mode = ArkApi::GetApiUtils().GetShooterGameMode();
        if (game_mode && game_mode->AreTribesAllied(static_cast<int>(tribe_a), static_cast<int>(tribe_b)))
        {
            reason = L"Нельзя объявить войну союзному племени. Сначала разорвите альянс.";
            return false;
        }
    }

    if (GetWarForTribeCopy(tribe_a).has_value() || GetWarForTribeCopy(tribe_b).has_value())
    {
        reason = L"У одного из племён уже есть активная война или откат.";
        return false;
    }

    if (!IsTribeLeaderOrAdminOnline(tribe_b))
    {
        reason = L"Лидер/администратор целевого племени должен быть в сети.";
        return false;
    }

    if (IsTribeInCooldown(tribe_a, now) || IsTribeInCooldown(tribe_b, now))
    {
        reason = L"Сейчас действует откат.";
        return false;
    }

    return true;
}

void DeclareWar(int64_t tribe_a, int64_t tribe_b)
{
    tribe_a = CanonicalTribeId(tribe_a);
    tribe_b = CanonicalTribeId(tribe_b);
    WarRecord war;
    {
        DataLockGuard lock(data_mutex);
        war.war_id = next_war_id++;
        war.tribe_a = tribe_a;
        war.tribe_b = tribe_b;
        war.declared_at = Now();
        war.start_at = war.declared_at + config.war_delay_seconds;
        wars_by_id[war.war_id] = war;
        RebuildTribeIndexLocked(war.declared_at);
    }
    need_save.store(true);

    const FString delay = FormatDuration(config.war_delay_seconds);
    const FString tribe_a_name = GetTribeDisplayName(tribe_a);
    const FString tribe_b_name = GetTribeDisplayName(tribe_b);
    NotifySide(tribe_a, FString::Format(L"Вы объявили войну племени {}. Начало через {}.", *tribe_b_name, *delay));
    NotifySide(tribe_b, FString::Format(L"Племя {} объявило вам войну. Начало через {}.", *tribe_a_name, *delay));
    // Logging disabled to avoid crashes in early init
}

void RequestCancelWar(int64_t tribe_id)
{
    tribe_id = CanonicalTribeId(tribe_id);
    int64_t other = 0;
    int64_t war_id = 0;
    {
        DataLockGuard lock(data_mutex);
        auto* war = GetWarForTribeLocked(tribe_id);
        if (!war)
            return;
        if (war->ended_at != 0)
            return;

        war_id = war->war_id;
        if (tribe_id == war->tribe_a)
            war->cancel_requested_by_a = true;
        else if (tribe_id == war->tribe_b)
            war->cancel_requested_by_b = true;

        other = tribe_id == war->tribe_a ? war->tribe_b : war->tribe_a;
    }
    need_save.store(true);
    NotifySide(other, L"Противник запросил отмену войны. Чтобы подтвердить, введите /accept.");
    NotifySide(tribe_id, L"Запрос на отмену войны отправлен. Ожидайте подтверждения /accept от противника.");
    // Logging disabled to avoid crashes in early init
}

void AcceptCancelWar(int64_t tribe_id)
{
    tribe_id = CanonicalTribeId(tribe_id);
    WarRecord snapshot;
    bool canceled = false;

    {
        DataLockGuard lock(data_mutex);
        auto* war = GetWarForTribeLocked(tribe_id);
        if (!war)
            return;
        if (war->ended_at != 0)
            return;

        if (tribe_id == war->tribe_a)
            war->cancel_requested_by_a = true;
        else if (tribe_id == war->tribe_b)
            war->cancel_requested_by_b = true;

        if (!(war->cancel_requested_by_a && war->cancel_requested_by_b))
            return;

        const auto now = Now();
        war->ended_at = now;
        war->cooldown_end_a = now + config.cooldown_seconds;
        war->cooldown_end_b = now + config.cooldown_seconds;
        war->cancel_requested_by_a = false;
        war->cancel_requested_by_b = false;
        war->cooldown_notified = false;
        snapshot = *war;
        canceled = true;
        RebuildTribeIndexLocked(now);
    }
    need_save.store(true);

    if (!canceled)
        return;

    const FString cooldown = FormatDuration(config.cooldown_seconds);
    const FString msg = FString::Format(L"Война отменена. Начался откат ({}).", *cooldown);
    NotifySideStyled(snapshot.tribe_a, msg, FLinearColor(0.2f, 1.0f, 0.2f, 1.0f), 1.4f, 8.0f);
    NotifySideStyled(snapshot.tribe_b, msg, FLinearColor(0.2f, 1.0f, 0.2f, 1.0f), 1.4f, 8.0f);
    // Logging disabled to avoid crashes in early init
}

bool HasIncomingCancel(int64_t tribe_id)
{
    tribe_id = CanonicalTribeId(tribe_id);
    DataLockGuard lock(data_mutex);
    auto* war = GetWarForTribeLocked(tribe_id);
    if (!war)
        return false;

    if (tribe_id == war->tribe_a)
        return war->cancel_requested_by_b;
    if (tribe_id == war->tribe_b)
        return war->cancel_requested_by_a;
    return false;
}

std::vector<PendingNotification> ProcessTimers()
{
    std::vector<PendingNotification> notifications_out;
    
    if (!auto_timers_enabled)
        return notifications_out;

    if (!plugin_initialized)
        return notifications_out;

    try
    {
        const auto now = Now();
        bool changed = false;
        {
            DataLockGuard lock(data_mutex);
            if (wars_by_id.empty())
                return notifications_out;

            // IMPORTANT: never erase from unordered_map while iterating it.
            // Collect IDs to remove first, then erase after the loop.
            std::vector<int64_t> war_ids_to_remove;

            for (auto& it : wars_by_id)
            {
                auto& war = it.second;
                if (war.war_id == 0 || war.tribe_a == 0 || war.tribe_b == 0)
                    continue;
                if (war.start_at == 0 && war.declared_at != 0)
                    war.start_at = war.declared_at + config.war_delay_seconds;
                if (war.start_at == 0)
                    war.start_at = now + config.war_delay_seconds;
                if (war.ended_at == 0 && now >= war.start_at && !war.start_notified)
                {
                    PendingNotification n;
                    n.styled = true;
                    n.color = FLinearColor(1.0f, 0.15f, 0.15f, 1.0f);
                    n.scale = 2.2f;
                    n.time = 12.0f;
                    n.message = FString(L"Война началась!");

                    n.side_tribe_id = war.tribe_a;
                    notifications_out.push_back(n);
                    n.side_tribe_id = war.tribe_b;
                    notifications_out.push_back(n);
                    war.start_notified = true;
                    changed = true;

                    if (config.self_test)
                        AppendSelfTestLog("ProcessTimers: war started war_id=" + std::to_string(war.war_id));
                }

                // Self-test: keep war Active for N seconds, then end and start cooldown.
                if (config.self_test && war.ended_at == 0 && war.start_notified)
                {
                    const auto active_seconds = std::max<int32_t>(1, config.self_test_active_seconds);
                    if (now >= war.start_at + active_seconds)
                    {
                        war.ended_at = now;
                        war.cooldown_end_a = now + config.cooldown_seconds;
                        war.cooldown_end_b = now + config.cooldown_seconds;
                        war.cooldown_notified = false;
                        changed = true;
                        AppendSelfTestLog("ProcessTimers: war ended war_id=" + std::to_string(war.war_id) +
                                          " cooldown=" + std::to_string(config.cooldown_seconds) + "s");
                    }
                }

                if (war.ended_at != 0)
                {
                    if (!war.cooldown_notified && now >= war.cooldown_end_a && now >= war.cooldown_end_b)
                    {
                        notifications_out.push_back(PendingNotification{ war.tribe_a, FString(L"Откат закончился.") });
                        notifications_out.push_back(PendingNotification{ war.tribe_b, FString(L"Откат закончился.") });
                        war.cooldown_notified = true;
                        changed = true;

                        if (config.self_test)
                            AppendSelfTestLog("ProcessTimers: cooldown ended war_id=" + std::to_string(war.war_id));
                    }

                    // War can be cleaned up after both cooldowns ended.
                    if (war.cooldown_end_a > 0 && war.cooldown_end_b > 0 &&
                        now >= war.cooldown_end_a && now >= war.cooldown_end_b)
                    {
                        war_ids_to_remove.push_back(it.first);
                    }
                }
            }

            if (!war_ids_to_remove.empty())
            {
                for (const auto war_id : war_ids_to_remove)
                    wars_by_id.erase(war_id);

                RebuildTribeIndexLocked(now);
                changed = true;

                if (config.self_test)
                {
                    AppendSelfTestLog("ProcessTimers: cleaned up wars count=" + std::to_string(war_ids_to_remove.size()));
                }
            }
        }

        if (changed)
            need_save.store(true);
    }
    catch (...)
    {
        auto_timers_enabled = false;
    }
    
    return notifications_out;
}

void EnqueueNotifications(const std::vector<PendingNotification>& notes)
{
    if (notes.empty())
        return;
    DataLockGuard lock(notification_mutex);
    pending_notifications.insert(pending_notifications.end(), notes.begin(), notes.end());
}

void FlushNotificationQueue()
{
    std::vector<PendingNotification> local;
    {
        DataLockGuard lock(notification_mutex);
        if (pending_notifications.empty())
            return;
        local.swap(pending_notifications);
    }

    for (const auto& note : local)
    {
        if (note.styled)
            NotifySideStyled(note.side_tribe_id, note.message, note.color, note.scale, note.time);
        else
            NotifySide(note.side_tribe_id, note.message);
    }
}

void TimerCallback()
{
    if (!plugin_initialized)
        return;

    UpdateTribeNameCache();
    UpdateAbandonedTribes(Now());

    auto notifications = ProcessTimers();
    EnqueueNotifications(notifications);

    if (ArkApi::GetApiUtils().GetStatus() == ArkApi::ServerStatus::Ready)
        FlushNotificationQueue();

    FlushSaveIfNeeded();
    SaveTribeNameCache();
}

std::vector<WarRecord> GetActiveWarsSnapshot(int64_t now)
{
    std::vector<WarRecord> result;
    DataLockGuard lock(data_mutex);
    result.reserve(wars_by_id.size());
    for (const auto& it : wars_by_id)
    {
        const auto& war = it.second;
        if (IsActiveWar(war, now))
            result.push_back(war);
    }
    return result;
}

bool IsStructureDamageAllowed(APrimalStructure* structure, AController* instigator, AActor* causer, float& out_multiplier)
{
    out_multiplier = 1.0f;
    if (!structure)
        return true;

    if (ArkApi::GetApiUtils().GetStatus() != ArkApi::ServerStatus::Ready)
        return false;

    if (IsExcludedStructure(structure))
        return true;

    const auto now = Now();
    const auto target_tribe = CanonicalTribeId(structure->TargetingTeamField());
    int64_t attacker_tribe = 0;

    if (instigator)
        attacker_tribe = GetTribeIdFromActor(instigator);
    if (attacker_tribe == 0 && causer)
        attacker_tribe = GetTribeIdFromActor(causer);

    if (target_tribe == 0 || attacker_tribe == 0)
    {
        // If attacker has no tribe, only allow against abandoned tribes (optional feature).
        if (target_tribe != 0)
        {
            float abandoned_mult = 1.0f;
            if (IsAbandonedStructureVulnerable(target_tribe, now, abandoned_mult))
            {
                out_multiplier = abandoned_mult;
                return true;
            }
        }
        return false;
    }

    if (target_tribe == attacker_tribe)
        return true;

    // Abandoned tribe window: structures can be damaged by anyone.
    float abandoned_mult = 1.0f;
    if (IsAbandonedStructureVulnerable(target_tribe, now, abandoned_mult))
    {
        out_multiplier = abandoned_mult;
        return true;
    }

    auto* game_mode = ArkApi::GetApiUtils().GetShooterGameMode();
    if (!game_mode)
        return false;

    const auto IsAlliedWith = [&](int64_t tribe_id, int64_t other_id) -> bool {
        if (tribe_id == 0 || other_id == 0)
            return false;
        return game_mode->AreTribesAllied(static_cast<int>(tribe_id), static_cast<int>(other_id));
    };

    const auto IsOnSide = [&](int64_t tribe_id, int64_t side_tribe) -> bool {
        if (tribe_id == side_tribe)
            return true;
        return IsAlliedWith(tribe_id, side_tribe);
    };

    const auto active_wars = GetActiveWarsSnapshot(now);
    for (const auto& war : active_wars)
    {
        const bool attacker_side_a = IsOnSide(attacker_tribe, war.tribe_a);
        const bool attacker_side_b = IsOnSide(attacker_tribe, war.tribe_b);
        const bool target_side_a = IsOnSide(target_tribe, war.tribe_a);
        const bool target_side_b = IsOnSide(target_tribe, war.tribe_b);

        if ((attacker_side_a && target_side_b) || (attacker_side_b && target_side_a))
        {
            out_multiplier = config.structure_damage_multiplier;
            // Log::GetLog()->debug("[TribeWarSystem] Structure damage ALLOWED: War #{}, Attacker tribe {} vs Target tribe {}", war.war_id, attacker_tribe, target_tribe);
            return true;
        }
    }

    // Log::GetLog()->debug("[TribeWarSystem] Structure damage BLOCKED: Attacker tribe {} vs Target tribe {} (no active war)", attacker_tribe, target_tribe);
    return false;
}

FString GetStatusText(const WarRecord* war, int64_t tribe_id)
{
    if (!war)
        return FString(L"Войны нет.");

    const auto now = Now();
    const auto phase = GetPhase(*war, now);
    if (phase == WarPhase::Pending)
    {
        const auto remain = std::max<int64_t>(0, war->start_at - now);
        const FString remain_text = FormatDuration(remain);
        return FString::Format(L"Ожидание начала: {}", *remain_text);
    }
    if (phase == WarPhase::Active)
        return FString(L"Война активна.");
    if (phase == WarPhase::Cooldown)
    {
        int64_t end = 0;
        if (tribe_id == war->tribe_a)
            end = war->cooldown_end_a;
        else if (tribe_id == war->tribe_b)
            end = war->cooldown_end_b;
        const auto remain = std::max<int64_t>(0, end - now);
        const FString remain_text = FormatDuration(remain);
        return FString::Format(L"Откат: {}", *remain_text);
    }

    return FString(L"Войны нет.");
}

uint64_t GetPlayerKey(AShooterPlayerController* pc)
{
    if (!pc)
        return 0;
    return ArkApi::IApiUtils::GetSteamIdFromController(pc);
}

void HandleMenuAction(AShooterPlayerController* pc, int entry_id)
{
    if (!pc)
        return;

    const auto tribe_id = GetTribeIdFromPlayer(pc);
    if (tribe_id == 0)
        return;
    if (config.multiuse_require_leader && !IsTribeLeaderOrAdmin(pc))
        return;

    const auto now = Now();
    const auto player_key = GetPlayerKey(pc);

    // Check radial menu constants first (backward compat)
    if (entry_id == kMenuStatusId || entry_id == kMuStatusId)
    {
        const auto war_view = GetWarForSideCopy(tribe_id);
        const int64_t side_root = war_view ? war_view->side_root : tribe_id;
        SendPlayerMessage(pc, GetStatusText(war_view ? &war_view->war : nullptr, side_root));
        return;
    }

    if (entry_id == kMenuCancelId || entry_id == kMuCancelId)
    {
        if (!GetWarForTribeCopy(tribe_id).has_value())
        {
            SendPlayerMessage(pc, L"Нет активной войны.");
            return;
        }
        RequestCancelWar(tribe_id);
        return;
    }

    if (entry_id == kMenuAcceptCancelId || entry_id == kMuAcceptCancelId)
    {
        if (!GetWarForTribeCopy(tribe_id).has_value())
        {
            SendPlayerMessage(pc, L"Нет активной войны.");
            return;
        }
        if (!HasIncomingCancel(tribe_id))
        {
            SendPlayerMessage(pc, L"Нет запроса на отмену.");
            return;
        }
        AcceptCancelWar(tribe_id);
        return;
    }

    const bool is_radial_declare = (entry_id >= kMenuDeclareListBaseId && entry_id < kMenuDeclareListBaseId + kMenuDeclareListMax);
    if (is_radial_declare)
    {
        auto target_it = declare_targets.find(player_key);
        if (target_it == declare_targets.end())
            return;
        auto entry_it = target_it->second.find(entry_id);
        if (entry_it == target_it->second.end())
            return;
        const auto target_id = entry_it->second;
        FString reason;
        if (!IsWarAllowed(tribe_id, target_id, now, reason))
        {
            SendPlayerMessage(pc, reason);
            return;
        }
        DeclareWar(tribe_id, target_id);
        return;
    }

    // Dynamic MultiUse: lookup action from multiuse_action_map
    if (player_key == 0)
        return;
    auto action_it = multiuse_action_map.find(player_key);
    if (action_it == multiuse_action_map.end())
        return;
    auto action_entry = action_it->second.find(entry_id);
    if (action_entry == action_it->second.end())
        return;

    const int action = action_entry->second;
    if (action == 1) // status
    {
        const auto war_view = GetWarForSideCopy(tribe_id);
        const int64_t side_root = war_view ? war_view->side_root : tribe_id;
        SendPlayerMessage(pc, GetStatusText(war_view ? &war_view->war : nullptr, side_root));
    }
    else if (action == 2) // cancel
    {
        if (!GetWarForTribeCopy(tribe_id).has_value())
        {
            SendPlayerMessage(pc, L"Нет активной войны.");
            return;
        }
        RequestCancelWar(tribe_id);
    }
    else if (action == 3) // accept_cancel
    {
        if (!GetWarForTribeCopy(tribe_id).has_value())
        {
            SendPlayerMessage(pc, L"Нет активной войны.");
            return;
        }
        if (!HasIncomingCancel(tribe_id))
        {
            SendPlayerMessage(pc, L"Нет запроса на отмену.");
            return;
        }
        AcceptCancelWar(tribe_id);
    }
    else if (action >= 100) // declare target
    {
        auto target_it = declare_targets.find(player_key);
        if (target_it == declare_targets.end())
            return;
        auto entry_it = target_it->second.find(entry_id);
        if (entry_it == target_it->second.end())
            return;
        const auto target_id = entry_it->second;
        FString reason;
        if (!IsWarAllowed(tribe_id, target_id, now, reason))
        {
            SendPlayerMessage(pc, reason);
            return;
        }
        DeclareWar(tribe_id, target_id);
    }
}

#if TRIBEWAR_ENABLE_RADIAL
void BuildTribeWarMenu(AShooterPlayerController* pc, TArray<FTribeRadialMenuEntry>* entries)
{
    if (!pc || !entries)
        return;

    if (GetTribeIdFromPlayer(pc) == 0 || !IsTribeLeaderOrAdmin(pc))
        return;

    FTribeRadialMenuEntry root;
    root.EntryName = FString(L"Mega Tribe War");
    root.EntryDescription = FString(L"Управление войнами племён");
    root.EntryIcon = nullptr;
    root.EntryID = kMenuRootId;
    root.ParentID = 0;
    root.bIsSubmenu = true;
    entries->Add(root);

    FTribeRadialMenuEntry declare;
    declare.EntryName = FString(L"Объявить войну");
    declare.EntryDescription = FString(L"Объявить войну племени");
    declare.EntryID = kMenuDeclareId;
    declare.ParentID = kMenuRootId;
    entries->Add(declare);

    FTribeRadialMenuEntry status;
    status.EntryName = FString(L"Статус войны");
    status.EntryDescription = FString(L"Показать статус войны");
    status.EntryID = kMenuStatusId;
    status.ParentID = kMenuRootId;
    entries->Add(status);

    FTribeRadialMenuEntry cancel;
    cancel.EntryName = FString(L"Отменить войну");
    cancel.EntryDescription = FString(L"Запросить отмену");
    cancel.EntryID = kMenuCancelId;
    cancel.ParentID = kMenuRootId;
    entries->Add(cancel);

    if (HasIncomingCancel(GetTribeIdFromPlayer(pc)))
    {
        FTribeRadialMenuEntry accept;
        accept.EntryName = FString(L"Принять отмену");
        accept.EntryDescription = FString(L"Принять запрос на отмену");
        accept.EntryID = kMenuAcceptCancelId;
        accept.ParentID = kMenuRootId;
        entries->Add(accept);
    }
}

void BuildDeclareListMenu(AShooterPlayerController* pc, TArray<FTribeRadialMenuEntry>* entries)
{
    if (!pc || !entries)
        return;

    const auto tribe_id = GetTribeIdFromPlayer(pc);
    if (tribe_id == 0 || !IsTribeLeaderOrAdmin(pc))
        return;

    const auto now = Now();
    if (GetWarForTribeCopy(tribe_id).has_value() || IsTribeInCooldown(tribe_id, now))
        return;

    int list_count = 0;
    const auto player_key = GetPlayerKey(pc);
    if (player_key == 0)
        return;
    declare_targets[player_key].clear();
    auto* game_mode = ArkApi::GetApiUtils().GetShooterGameMode();
    if (!game_mode)
        return;
    const auto& tribes = game_mode->TribesDataField();
    std::unordered_set<int64_t> seen_ids;
    for (int i = 0; i < tribes.Num(); ++i)
    {
        auto& data = const_cast<FTribeData&>(tribes[i]);
        if (list_count >= kMenuDeclareListMax)
            break;
        int32_t tid = 0;
        const int32_t members = TryGetTribeMemberCount(data, tid);
        if (members <= 0)
            continue;
        const int64_t other_id = CanonicalTribeId(static_cast<int64_t>(tid));
        if (other_id == 0)
            continue;
        if (other_id == tribe_id)
            continue;
        if (!seen_ids.insert(other_id).second)
            continue;
        if (GetWarForTribeCopy(other_id).has_value() || IsTribeInCooldown(other_id, now))
            continue;

        FTribeRadialMenuEntry item;
        const FString entry_label = GetTribeDisplayName(other_id);
        if (!entry_label.IsEmpty())
            item.EntryName = entry_label;
        else
            item.EntryName = FString::Format(L"ID: {}", other_id);
        item.EntryDescription = FString(L"Объявить войну");
        item.EntryID = kMenuDeclareListBaseId + list_count;
        item.ParentID = kMenuDeclareId;
        entries->Add(item);
        declare_targets[player_key][item.EntryID] = other_id;
        ++list_count;
    }
}
#endif

// === MultiUse wheel integration (server-side radial) ===

static void DumpMultiUseEntries(const char* prefix, const TArray<FMultiUseEntry>* entries)
{
    if (!config.debug_multiuse_log)
        return;
    if (!entries)
        return;

    const int count = entries->Num();
    AppendMultiUseDebugLog(std::string(prefix) + ": count=" + std::to_string(count));

    const int limit = (std::min)(count, 40);
    for (int i = 0; i < limit; ++i)
    {
        const auto& e = (*entries)[i];
        AppendMultiUseDebugLog(
            std::string(prefix) + " [" + std::to_string(i) + "]" +
            " idx=" + std::to_string(e.UseIndex) +
            " prio=" + std::to_string(e.Priority) +
            " cat=" + std::to_string(e.WheelCategory) +
            " hideUI=" + std::to_string(static_cast<int>(e.bHideFromUI)) +
            " disable=" + std::to_string(static_cast<int>(e.bDisableUse)) +
            " inv=" + std::to_string(static_cast<int>(e.bDisplayOnInventoryUI)) +
            " inv2=" + std::to_string(static_cast<int>(e.bDisplayOnInventoryUISecondary)) +
            " inv3=" + std::to_string(static_cast<int>(e.bDisplayOnInventoryUITertiary)) +
            " sec=" + std::to_string(static_cast<int>(e.bIsSecondaryUse)) +
            " clientOnly=" + std::to_string(static_cast<int>(e.bClientSideOnly)));
    }

    if (count > limit)
        AppendMultiUseDebugLog(std::string(prefix) + ": (truncated, total=" + std::to_string(count) + ")");
}

static void AddMultiUseEntry(TArray<FMultiUseEntry>* entries, int use_index, const FString& text, int priority = 0)
{
    if (!entries)
        return;
    FMultiUseEntry e;
    memset(&e, 0, sizeof(e));
    e.ForComponent = nullptr; // CRITICAL: must be valid pointer or nullptr
    e.UseString = text;
    e.UseIndex = use_index;
    e.Priority = priority;
    e.bHideFromUI = 0;
    e.bDisableUse = 0;
    e.WheelCategory = 0;
    e.DisableUseColor = FColor(0, 0, 0, 0);
    e.UseTextColor = FColor(255, 255, 255, 255);
    e.EntryActivationTimer = 0.0f;
    e.DefaultEntryActivationTimer = 0.0f;
    e.ActivationSound = nullptr;
    e.UseInventoryButtonStyleOverrideIndex = 0;
    entries->Add(e);
}

static void BuildDeclareListMultiUse(AShooterPlayerController* pc, int64_t tribe_id, TArray<FMultiUseEntry>* entries, int& next_index)
{
    if (!pc || !entries)
        return;

    const auto now = Now();
    if (GetWarForTribeCopy(tribe_id).has_value() || IsTribeInCooldown(tribe_id, now))
        return;

    const int max_targets = std::min<int>(kMenuDeclareListMax, config.multiuse_max_targets);
    if (max_targets <= 0)
        return;

    const auto player_key = GetPlayerKey(pc);
    if (player_key == 0)
        return;
    declare_targets[player_key].clear();

    auto* game_mode = ArkApi::GetApiUtils().GetShooterGameMode();
    if (!game_mode)
        return;
    const auto& tribes = game_mode->TribesDataField();

    int list_count = 0;
    std::unordered_set<int64_t> seen_ids;
    for (int i = 0; i < tribes.Num(); ++i)
    {
        if (list_count >= max_targets)
            break;
        auto& data = const_cast<FTribeData&>(tribes[i]);
        int32_t tid = 0;
        const int32_t members = TryGetTribeMemberCount(data, tid);
        if (members <= 0)
            continue;
        const int64_t other_id = CanonicalTribeId(static_cast<int64_t>(tid));
        if (other_id == 0)
            continue;
        if (other_id == tribe_id)
            continue;
        if (!seen_ids.insert(other_id).second)
            continue;
        if (GetWarForTribeCopy(other_id).has_value() || IsTribeInCooldown(other_id, now))
            continue;

        const int entry_id = next_index++;
        const FString display_name = GetTribeDisplayName(other_id);
        const auto label = display_name.IsEmpty()
            ? FString::Format(L"Объявить войну: ID {}", other_id)
            : FString::Format(L"Объявить войну: {}", *display_name);
        AddMultiUseEntry(entries, entry_id, label);
        declare_targets[player_key][entry_id] = other_id;
        multiuse_action_map[player_key][entry_id] = 100 + list_count; // action=declare target #N
        ++list_count;
    }
}

DECLARE_HOOK(APrimalStructure_GetMultiUseEntries, void, APrimalStructure*, APlayerController*, TArray<FMultiUseEntry>*);

DECLARE_HOOK(APrimalStructure_TryMultiUse, bool, APrimalStructure*, APlayerController*, int);

static void MaybeAddMultiUseMenu(APrimalStructure* structure, APlayerController* for_pc, TArray<FMultiUseEntry>* entries, const char* hook_name)
{
    if (!plugin_initialized)
        return;
    if (!config.enable_multiuse_menu)
        return;
    if (!structure || !for_pc || !entries)
        return;

    auto* pc = static_cast<AShooterPlayerController*>(for_pc);
    if (!pc)
        return;

    const auto tribe_id = GetTribeIdFromPlayer(pc);
    if (tribe_id == 0)
    {
        AppendMultiUseDebugLog(std::string(hook_name) + ": skip (tribe_id=0)");
        return;
    }

    const bool is_leader = IsTribeLeaderOrAdmin(pc);
    if (config.multiuse_require_leader && !is_leader)
    {
        AppendMultiUseDebugLog(std::string(hook_name) + ": skip (not leader/admin) tribe_id=" + std::to_string(tribe_id));
        return;
    }

    const bool owned_ok = !config.multiuse_require_owned_structure || structure->IsOfTribe(static_cast<int>(tribe_id));
    if (!owned_ok)
    {
        AppendMultiUseDebugLog(std::string(hook_name) + ": skip (not owned structure) tribe_id=" + std::to_string(tribe_id));
        return;
    }

    const int before = entries->Num();

    DumpMultiUseEntries((std::string(hook_name) + ": before").c_str(), entries);

    const auto player_key = GetPlayerKey(pc);
    if (player_key == 0)
        return;

    // Clear previous mappings for this player
    multiuse_action_map[player_key].clear();

    // Find max UseIndex in existing entries to avoid conflicts
    int max_index = 0;
    for (int i = 0; i < before; ++i)
    {
        const int idx = (*entries)[i].UseIndex;
        if (idx > max_index)
            max_index = idx;
    }
    // Start adding from max+1 (or minimum 100 if no entries exist)
    int next_index = (std::max)(max_index + 1, 100);

    // Always add Status (always valid)
    const int status_idx = next_index++;
    AddMultiUseEntry(entries, status_idx, FString(L"Mega Tribe War: Статус"), 10);
    multiuse_action_map[player_key][status_idx] = 1; // action=status

    // Cancel and Accept only if war is active
    const auto war = GetWarForTribeCopy(tribe_id);
    if (war.has_value())
    {
        const int cancel_idx = next_index++;
        AddMultiUseEntry(entries, cancel_idx, FString(L"Mega Tribe War: Отмена"), 10);
        multiuse_action_map[player_key][cancel_idx] = 2; // action=cancel

        if (HasIncomingCancel(tribe_id))
        {
            const int accept_idx = next_index++;
            AddMultiUseEntry(entries, accept_idx, FString(L"Mega Tribe War: Принять отмену"), 10);
            multiuse_action_map[player_key][accept_idx] = 3; // action=accept_cancel
        }
    }

    BuildDeclareListMultiUse(pc, tribe_id, entries, next_index);
    const int after = entries->Num();

    DumpMultiUseEntries((std::string(hook_name) + ": after").c_str(), entries);

    AppendMultiUseDebugLog(std::string(hook_name) + ": added entries before=" + std::to_string(before) + " after=" + std::to_string(after) +
                           " tribe_id=" + std::to_string(tribe_id) + " leader=" + std::string(is_leader ? "1" : "0") +
                           " owned_ok=" + std::string(owned_ok ? "1" : "0"));
}

static bool MaybeHandleMultiUse(APrimalStructure* structure, APlayerController* for_pc, int use_index, const char* hook_name)
{
    if (!plugin_initialized)
        return false;
    if (!config.enable_multiuse_menu)
        return false;
    if (!for_pc)
        return false;

// Check if this use_index belongs to our plugin (via action_map or radial constants)
    const auto player_key = GetPlayerKey(static_cast<AShooterPlayerController*>(for_pc));
    const bool is_our_multiuse = (player_key != 0 && multiuse_action_map.count(player_key) && multiuse_action_map[player_key].count(use_index));
    const bool is_radial_action = (use_index == kMenuStatusId || use_index == kMenuCancelId || use_index == kMenuAcceptCancelId ||
                                   (use_index >= kMenuDeclareListBaseId && use_index < kMenuDeclareListBaseId + kMenuDeclareListMax));
    if (!is_our_multiuse && !is_radial_action)
        return false;

    auto* pc = static_cast<AShooterPlayerController*>(for_pc);
    if (!pc)
        return false;

    const auto tribe_id = GetTribeIdFromPlayer(pc);
    if (tribe_id == 0)
    {
        AppendMultiUseDebugLog(std::string(hook_name) + ": deny use_index=" + std::to_string(use_index) + " (tribe_id=0)");
        return false;
    }

    const bool is_leader = IsTribeLeaderOrAdmin(pc);
    if (config.multiuse_require_leader && !is_leader)
    {
        AppendMultiUseDebugLog(std::string(hook_name) + ": deny use_index=" + std::to_string(use_index) + " (not leader/admin) tribe_id=" + std::to_string(tribe_id));
        return false;
    }

    if (config.multiuse_require_owned_structure)
    {
        if (!structure || !structure->IsOfTribe(static_cast<int>(tribe_id)))
        {
            AppendMultiUseDebugLog(std::string(hook_name) + ": deny use_index=" + std::to_string(use_index) + " (not owned structure) tribe_id=" + std::to_string(tribe_id));
            return false;
        }
    }

    AppendMultiUseDebugLog(std::string(hook_name) + ": handle use_index=" + std::to_string(use_index) + " tribe_id=" + std::to_string(tribe_id));
    HandleMenuAction(pc, use_index);
    return true;
}

void Hook_APrimalStructure_GetMultiUseEntries(APrimalStructure* structure, APlayerController* for_pc, TArray<FMultiUseEntry>* entries)
{
    APrimalStructure_GetMultiUseEntries_original(structure, for_pc, entries);
    MaybeAddMultiUseMenu(structure, for_pc, entries, "APrimalStructure.GetMultiUseEntries");
}

bool Hook_APrimalStructure_TryMultiUse(APrimalStructure* structure, APlayerController* for_pc, int use_index)
{
    if (MaybeHandleMultiUse(structure, for_pc, use_index, "APrimalStructure.TryMultiUse"))
        return true;
    return APrimalStructure_TryMultiUse_original(structure, for_pc, use_index);
}

// Some versions/modded structures route MultiUse through Blueprint events.
// Hook these too for compatibility and better diagnostics.
DECLARE_HOOK(APrimalStructure_BPGetMultiUseEntries, void, APrimalStructure*, APlayerController*, TArray<FMultiUseEntry>*);
void Hook_APrimalStructure_BPGetMultiUseEntries(APrimalStructure* structure, APlayerController* for_pc, TArray<FMultiUseEntry>* entries)
{
    APrimalStructure_BPGetMultiUseEntries_original(structure, for_pc, entries);
    MaybeAddMultiUseMenu(structure, for_pc, entries, "APrimalStructure.BPGetMultiUseEntries");
}

DECLARE_HOOK(APrimalStructure_BPTryMultiUse, bool, APrimalStructure*, APlayerController*, int);
bool Hook_APrimalStructure_BPTryMultiUse(APrimalStructure* structure, APlayerController* for_pc, int use_index)
{
    if (MaybeHandleMultiUse(structure, for_pc, use_index, "APrimalStructure.BPTryMultiUse"))
        return true;
    return APrimalStructure_BPTryMultiUse_original(structure, for_pc, use_index);
}

DECLARE_HOOK(APrimalStructureItemContainer_GetMultiUseEntries, void, APrimalStructure*, APlayerController*, TArray<FMultiUseEntry>*);
void Hook_APrimalStructureItemContainer_GetMultiUseEntries(APrimalStructure* structure, APlayerController* for_pc, TArray<FMultiUseEntry>* entries)
{
    APrimalStructureItemContainer_GetMultiUseEntries_original(structure, for_pc, entries);
    MaybeAddMultiUseMenu(structure, for_pc, entries, "APrimalStructureItemContainer.GetMultiUseEntries");
}

DECLARE_HOOK(APrimalStructureItemContainer_TryMultiUse, bool, APrimalStructure*, APlayerController*, int);
bool Hook_APrimalStructureItemContainer_TryMultiUse(APrimalStructure* structure, APlayerController* for_pc, int use_index)
{
    if (MaybeHandleMultiUse(structure, for_pc, use_index, "APrimalStructureItemContainer.TryMultiUse"))
        return true;
    return APrimalStructureItemContainer_TryMultiUse_original(structure, for_pc, use_index);
}

DECLARE_HOOK(APrimalStructureItemContainer_BPGetMultiUseEntries, void, APrimalStructure*, APlayerController*, TArray<FMultiUseEntry>*);
void Hook_APrimalStructureItemContainer_BPGetMultiUseEntries(APrimalStructure* structure, APlayerController* for_pc, TArray<FMultiUseEntry>* entries)
{
    APrimalStructureItemContainer_BPGetMultiUseEntries_original(structure, for_pc, entries);
    MaybeAddMultiUseMenu(structure, for_pc, entries, "APrimalStructureItemContainer.BPGetMultiUseEntries");
}

DECLARE_HOOK(APrimalStructureItemContainer_BPTryMultiUse, bool, APrimalStructure*, APlayerController*, int);
bool Hook_APrimalStructureItemContainer_BPTryMultiUse(APrimalStructure* structure, APlayerController* for_pc, int use_index)
{
    if (MaybeHandleMultiUse(structure, for_pc, use_index, "APrimalStructureItemContainer.BPTryMultiUse"))
        return true;
    return APrimalStructureItemContainer_BPTryMultiUse_original(structure, for_pc, use_index);
}

void InitPlugin();
void TimerCallback();

DECLARE_HOOK(AShooterGameMode_Tick, void, AShooterGameMode*, float);
void Hook_AShooterGameMode_Tick(AShooterGameMode* game_mode, float delta_seconds)
{
    AShooterGameMode_Tick_original(game_mode, delta_seconds);
    
    if (!plugin_initialized &&
        ArkApi::GetApiUtils().GetStatus() == ArkApi::ServerStatus::Ready)
    {
        InitPlugin();
    }
}

DECLARE_HOOK(APrimalStructure_TakeDamage, float, APrimalStructure*, float, FDamageEvent*, AController*, AActor*);
float Hook_APrimalStructure_TakeDamage(APrimalStructure* structure, float damage, FDamageEvent* event, AController* instigator, AActor* causer)
{
    if (damage <= 0.0f)
        return APrimalStructure_TakeDamage_original(structure, damage, event, instigator, causer);

    float mult = 1.0f;
    if (!IsStructureDamageAllowed(structure, instigator, causer, mult))
        return 0.0f;

    mult = (std::max)(0.0f, (std::min)(mult, 10.0f));
    return APrimalStructure_TakeDamage_original(structure, damage * mult, event, instigator, causer);
}

#if TRIBEWAR_ENABLE_RADIAL
DECLARE_HOOK(AShooterPlayerController_GetTribeRadialMenuEntries, void, AShooterPlayerController*, TArray<FTribeRadialMenuEntry>*);
void Hook_AShooterPlayerController_GetTribeRadialMenuEntries(AShooterPlayerController* pc, TArray<FTribeRadialMenuEntry>* entries)
{
    AShooterPlayerController_GetTribeRadialMenuEntries_original(pc, entries);
    BuildTribeWarMenu(pc, entries);
    BuildDeclareListMenu(pc, entries);
}

DECLARE_HOOK(AShooterPlayerController_OnTribeRadialMenuSelection, void, AShooterPlayerController*, int);
void Hook_AShooterPlayerController_OnTribeRadialMenuSelection(AShooterPlayerController* pc, int entry_id)
{
    HandleMenuAction(pc, entry_id);
    AShooterPlayerController_OnTribeRadialMenuSelection_original(pc, entry_id);
}
#endif

// === Chat Commands ===
#if TRIBEWAR_ENABLE_CHAT_COMMANDS

void CmdWarStatus(AShooterPlayerController* pc, FString*, EChatSendMode::Type)
{
    if (!pc)
        return;

    const auto tribe_id = GetTribeIdFromPlayer(pc);
    if (tribe_id == 0)
    {
        SendPlayerMessage(pc, L"Вы должны состоять в племени.");
        return;
    }

    if (!IsTribeLeaderOrAdmin(pc))
    {
        SendPlayerMessage(pc, L"Только лидер/администратор племени может использовать эту команду.");
        return;
    }

    const auto war_view = GetWarForSideCopy(tribe_id);
    const int64_t side_root = war_view ? war_view->side_root : tribe_id;
    const FString status = GetStatusText(war_view ? &war_view->war : nullptr, side_root);
    SendPlayerMessage(pc, status);
}

void CmdWarDeclare(AShooterPlayerController* pc, FString*, EChatSendMode::Type)
{
    if (!pc)
        return;

    UpdateTribeNameCache();

    const auto tribe_id = GetTribeIdFromPlayer(pc);
    if (tribe_id == 0)
    {
        SendPlayerMessage(pc, L"Вы должны состоять в племени.");
        return;
    }

    if (!IsTribeLeaderOrAdmin(pc))
    {
        SendPlayerMessage(pc, L"Только лидер/администратор племени может использовать эту команду.");
        return;
    }

    const auto now = Now();
    if (GetWarForTribeCopy(tribe_id).has_value() || IsTribeInCooldown(tribe_id, now))
    {
        SendPlayerMessage(pc, L"У вашего племени уже есть активная война или откат.");
        return;
    }

    if (ArkApi::GetApiUtils().GetStatus() != ArkApi::ServerStatus::Ready)
        return;

    auto* world = ArkApi::GetApiUtils().GetWorld();
    if (!world)
        return;

    // BUGFIX: Build tribe list from currently online players (more reliable than TribesDataField)
    // This avoids race conditions where TribesDataField may be out of sync with PlayerControllerList
    std::unordered_set<int64_t> available_tribes;
    auto& players = world->PlayerControllerListField();
    for (TWeakObjectPtr<APlayerController>& player : players)
    {
        auto* check_pc = static_cast<AShooterPlayerController*>(player.Get());
        
        // CRITICAL BUGFIX: Validate player is actually active (not disconnected/pending)
        if (!check_pc)
            continue;
        if (!check_pc->IsValidLowLevelFast(true))
            continue;
        if (check_pc->IsA(AShooterPlayerController::StaticClass()) == false)
            continue;

        const int64_t check_tribe = GetTribeIdFromPlayer(check_pc);
        if (check_tribe == 0 || check_tribe == tribe_id)
            continue;

        // Skip if this tribe has an active war or cooldown
        if (GetWarForTribeCopy(check_tribe).has_value() || IsTribeInCooldown(check_tribe, now))
            continue;

        available_tribes.insert(check_tribe);
    }

    if (available_tribes.empty())
    {
        SendPlayerMessage(pc, L"Нет доступных племён для объявления войны.");
        return;
    }

    FString message(L"Список племён:\n");
    for (const auto other_id : available_tribes)
    {
        const FString display_name = GetTribeDisplayName(other_id);
        if (!display_name.IsEmpty())
            message += FString::Format(L"{}\n", *display_name);
        else
            message += FString::Format(L"ID: {}\n", other_id);
    }

    message += L"\nИспользуйте /war <tribe_id>, чтобы объявить войну.";
    SendPlayerMessage(pc, message);
}

void CmdWarDeclareId(AShooterPlayerController* pc, FString* message, EChatSendMode::Type)
{
    if (!pc || !message)
        return;

    const auto tribe_id = GetTribeIdFromPlayer(pc);
    if (tribe_id == 0)
    {
        SendPlayerMessage(pc, L"Вы должны состоять в племени.");
        return;
    }

    if (!IsTribeLeaderOrAdmin(pc))
    {
        SendPlayerMessage(pc, L"Только лидер/администратор племени может использовать эту команду.");
        return;
    }

    TArray<FString> parsed;
    message->ParseIntoArray(parsed, L" ", true);
    // expected: "/war <tribe_id>" or "<tribe_id>" depending on chat hook
    int arg_index = 0;
    if (parsed.Num() >= 1 && parsed[0].StartsWith(L"/"))
        arg_index = 1;

    if (parsed.Num() <= arg_index)
    {
        SendPlayerMessage(pc, L"Использование: /war <tribe_id>");
        return;
    }

    int64_t target_id = 0;
    try
    {
        const uint64_t raw = std::stoull(parsed[arg_index].ToString());
        if (raw == 0 || raw > 0xFFFFFFFFULL)
            throw std::out_of_range("tribe_id");
        target_id = static_cast<int64_t>(raw);
    }
    catch (...)
    {
        SendPlayerMessage(pc, L"Некорректный ID племени.");
        return;
    }

    const auto now = Now();
    FString reason;
    if (!IsWarAllowed(tribe_id, target_id, now, reason))
    {
        SendPlayerMessage(pc, reason);
        return;
    }

    DeclareWar(tribe_id, target_id);
}

void CmdWarCancel(AShooterPlayerController* pc, FString*, EChatSendMode::Type)
{
    if (!pc)
        return;

    const auto tribe_id = GetTribeIdFromPlayer(pc);
    if (tribe_id == 0)
    {
        SendPlayerMessage(pc, L"Вы должны состоять в племени.");
        return;
    }

    if (!IsTribeLeaderOrAdmin(pc))
    {
        SendPlayerMessage(pc, L"Только лидер/администратор племени может использовать эту команду.");
        return;
    }

    if (!GetWarForTribeCopy(tribe_id).has_value())
    {
        SendPlayerMessage(pc, L"Нет активной войны.");
        return;
    }

    RequestCancelWar(tribe_id);
}

void CmdWarAcceptCancel(AShooterPlayerController* pc, FString*, EChatSendMode::Type)
{
    if (!pc)
        return;

    const auto tribe_id = GetTribeIdFromPlayer(pc);
    if (tribe_id == 0)
    {
        SendPlayerMessage(pc, L"Вы должны состоять в племени.");
        return;
    }

    if (!IsTribeLeaderOrAdmin(pc))
    {
        SendPlayerMessage(pc, L"Только лидер/администратор племени может использовать эту команду.");
        return;
    }

    if (!GetWarForTribeCopy(tribe_id).has_value())
    {
        SendPlayerMessage(pc, L"Нет активной войны.");
        return;
    }

    if (!HasIncomingCancel(tribe_id))
    {
        SendPlayerMessage(pc, L"Запрос на отмену не получен.");
        return;
    }

    AcceptCancelWar(tribe_id);
}

void CmdWarHelp(AShooterPlayerController* pc, FString*, EChatSendMode::Type)
{
    if (!pc)
        return;

    FString help(L"Краткая справка по командам:\n");
    help += L"/info - краткая справка по командам\n";
    help += L"/status - статус текущей войны\n";
    help += L"/war - список доступных племён для объявления\n";
    help += L"/war <tribe_id> - объявить войну выбранному племени\n";
    help += L"/stop - запросить отмену войны\n";
    help += L"/accept - принять запрос на отмену\n";
    SendPlayerMessage(pc, help);
}

void CmdWar(AShooterPlayerController* pc, FString* message, EChatSendMode::Type mode)
{
    if (!pc)
        return;

    // /war => list; /war <tribe_id> => declare
    if (!message)
    {
        CmdWarDeclare(pc, message, mode);
        return;
    }

    TArray<FString> parsed;
    message->ParseIntoArray(parsed, L" ", true);

    if (parsed.Num() <= 1)
    {
        CmdWarDeclare(pc, message, mode);
        return;
    }

    // Try to support both message formats:
    // 1) "/war 123"  => parsed[0]="/war", parsed[1]="123"
    // 2) "123"       => parsed[0]="123"
    int arg_index = 0;
    if (parsed[0].StartsWith(L"/"))
        arg_index = 1;

    if (parsed.Num() <= arg_index)
    {
        CmdWarDeclare(pc, message, mode);
        return;
    }

    // If the next token looks like a number, treat it as tribe_id; otherwise list.
    const FString& arg = parsed[arg_index];
    if (arg.IsNumeric())
        CmdWarDeclareId(pc, message, mode);
    else
        CmdWarDeclare(pc, message, mode);
}

#endif // TRIBEWAR_ENABLE_CHAT_COMMANDS

void InitPlugin()
{
    if (plugin_initialized)
        return;

    std::filesystem::create_directories(GetPluginDir());
    LoadConfig();
    LoadData();
    LoadTribeNameCache();

    AppendMultiUseDebugLog(std::string("InitPlugin: enable_multiuse_menu=") + (config.enable_multiuse_menu ? "true" : "false") +
                           " require_owned=" + (config.multiuse_require_owned_structure ? "true" : "false") +
                           " require_leader=" + (config.multiuse_require_leader ? "true" : "false") +
                           " max_targets=" + std::to_string(config.multiuse_max_targets));

    // Ensure data.json gets created even on empty state and even if the process
    // terminates without a clean plugin unload.
    if (!FileExists(GetDataPath()))
        need_save.store(true);

    SeedSelfTestWarIfNeeded();
    if (config.self_test)
        need_save.store(true);

    ArkApi::GetCommands().AddOnTimerCallback("TribeWarSystem_Timer", &TimerCallback);

    plugin_initialized = true;
}

void Load()
{
    try
    {
        // Initialize early so config (incl. debug_multiuse_log) is loaded before hook logging.
        InitPlugin();

        auto set_hook_logged = [](const char* name, auto hook_fn, auto* original_fn)
        {
            try
            {
                ArkApi::GetHooks().SetHook(name, hook_fn, original_fn);
                AppendMultiUseDebugLog(std::string("SetHook OK: ") + name);
            }
            catch (const std::exception& e)
            {
                AppendMultiUseDebugLog(std::string("SetHook FAIL: ") + name + " ex=" + e.what());
            }
            catch (...)
            {
                AppendMultiUseDebugLog(std::string("SetHook FAIL: ") + name + " ex=unknown");
            }
        };

        ArkApi::GetHooks().SetHook("AShooterGameMode.Tick", &Hook_AShooterGameMode_Tick, &AShooterGameMode_Tick_original);
        ArkApi::GetHooks().SetHook("APrimalStructure.TakeDamage", &Hook_APrimalStructure_TakeDamage, &APrimalStructure_TakeDamage_original);

#if TRIBEWAR_ENABLE_CHAT_COMMANDS
        ArkApi::GetCommands().AddChatCommand("/info", &CmdWarHelp);
        ArkApi::GetCommands().AddChatCommand("/status", &CmdWarStatus);
        ArkApi::GetCommands().AddChatCommand("/war", &CmdWar);
        ArkApi::GetCommands().AddChatCommand("/stop", &CmdWarCancel);
        ArkApi::GetCommands().AddChatCommand("/accept", &CmdWarAcceptCancel);
#endif

        // MultiUse hooks disabled due to FMultiUseEntry structure incompatibility with ARK 361.7
        // Use chat commands instead: /info, /status, /war, /stop, /accept
        /*
        set_hook_logged("APrimalStructure.GetMultiUseEntries", &Hook_APrimalStructure_GetMultiUseEntries, &APrimalStructure_GetMultiUseEntries_original);
        set_hook_logged("APrimalStructure.TryMultiUse", &Hook_APrimalStructure_TryMultiUse, &APrimalStructure_TryMultiUse_original);
        set_hook_logged("APrimalStructure.BPGetMultiUseEntries", &Hook_APrimalStructure_BPGetMultiUseEntries, &APrimalStructure_BPGetMultiUseEntries_original);
        set_hook_logged("APrimalStructure.BPTryMultiUse", &Hook_APrimalStructure_BPTryMultiUse, &APrimalStructure_BPTryMultiUse_original);

        set_hook_logged("APrimalStructureItemContainer.GetMultiUseEntries", &Hook_APrimalStructureItemContainer_GetMultiUseEntries, &APrimalStructureItemContainer_GetMultiUseEntries_original);
        set_hook_logged("APrimalStructureItemContainer.TryMultiUse", &Hook_APrimalStructureItemContainer_TryMultiUse, &APrimalStructureItemContainer_TryMultiUse_original);
        set_hook_logged("APrimalStructureItemContainer.BPGetMultiUseEntries", &Hook_APrimalStructureItemContainer_BPGetMultiUseEntries, &APrimalStructureItemContainer_BPGetMultiUseEntries_original);
        set_hook_logged("APrimalStructureItemContainer.BPTryMultiUse", &Hook_APrimalStructureItemContainer_BPTryMultiUse, &APrimalStructureItemContainer_BPTryMultiUse_original);
        */
        
#if TRIBEWAR_ENABLE_RADIAL
        ArkApi::GetHooks().SetHook("AShooterPlayerController.GetTribeRadialMenuEntries", &Hook_AShooterPlayerController_GetTribeRadialMenuEntries, &AShooterPlayerController_GetTribeRadialMenuEntries_original);
        ArkApi::GetHooks().SetHook("AShooterPlayerController.OnTribeRadialMenuSelection", &Hook_AShooterPlayerController_OnTribeRadialMenuSelection, &AShooterPlayerController_OnTribeRadialMenuSelection_original);
#endif
    }
    catch (...)
    {
        // Silent fail
    }
}

void Unload()
{
    try
    {
        if (plugin_initialized)
        {
            SaveData();
            SaveTribeNameCache();
        }

#if TRIBEWAR_ENABLE_CHAT_COMMANDS
        ArkApi::GetCommands().RemoveChatCommand("/info");
        ArkApi::GetCommands().RemoveChatCommand("/status");
        ArkApi::GetCommands().RemoveChatCommand("/war");
        ArkApi::GetCommands().RemoveChatCommand("/stop");
        ArkApi::GetCommands().RemoveChatCommand("/accept");
#endif

        ArkApi::GetCommands().RemoveOnTimerCallback("TribeWarSystem_Timer");
        
        ArkApi::GetHooks().DisableHook("AShooterGameMode.Tick", &Hook_AShooterGameMode_Tick);
        ArkApi::GetHooks().DisableHook("APrimalStructure.TakeDamage", &Hook_APrimalStructure_TakeDamage);
// MultiUse hooks were disabled (see Load())
        /*
        
        ArkApi::GetHooks().DisableHook("APrimalStructure.GetMultiUseEntries", &Hook_APrimalStructure_GetMultiUseEntries);
        ArkApi::GetHooks().DisableHook("APrimalStructure.TryMultiUse", &Hook_APrimalStructure_TryMultiUse);

        ArkApi::GetHooks().DisableHook("APrimalStructure.BPGetMultiUseEntries", &Hook_APrimalStructure_BPGetMultiUseEntries);
        ArkApi::GetHooks().DisableHook("APrimalStructure.BPTryMultiUse", &Hook_APrimalStructure_BPTryMultiUse);

        ArkApi::GetHooks().DisableHook("APrimalStructureItemContainer.GetMultiUseEntries", &Hook_APrimalStructureItemContainer_GetMultiUseEntries);
        ArkApi::GetHooks().DisableHook("APrimalStructureItemContainer.BPGetMultiUseEntries", &Hook_APrimalStructureItemContainer_BPGetMultiUseEntries);
        ArkApi::GetHooks().DisableHook("APrimalStructureItemContainer.BPTryMultiUse", &Hook_APrimalStructureItemContainer_BPTryMultiUse);
        */

#if TRIBEWAR_ENABLE_RADIAL
        ArkApi::GetHooks().DisableHook("AShooterPlayerController.GetTribeRadialMenuEntries", &Hook_AShooterPlayerController_GetTribeRadialMenuEntries);
        ArkApi::GetHooks().DisableHook("AShooterPlayerController.OnTribeRadialMenuSelection", &Hook_AShooterPlayerController_OnTribeRadialMenuSelection);
#endif
    }
    catch (...)
    {
        // Silent fail
    }
}

} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        Load();
        break;
    case DLL_PROCESS_DETACH:
        Unload();
        break;
    }
    return TRUE;
}
