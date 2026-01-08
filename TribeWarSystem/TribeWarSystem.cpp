#include <API/ARK/Ark.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <mutex>
#include <optional>
#include <atomic>
#include <string>
#include <unordered_map>
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

// Dynamic UseIndex mapping: for each player, store what UseIndex maps to what action.
// action codes: 1=status, 2=cancel, 3=accept_cancel, 100+N=declare_target[N]
std::unordered_map<uint64_t, std::unordered_map<int, int>> multiuse_action_map;

int64_t next_war_id = 1;
bool auto_timers_enabled = true;
bool plugin_initialized = false;
std::atomic<bool> need_save { false };
std::vector<std::pair<int64_t, FString>> pending_notifications;
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

        if (json.find("wars") != json.end())
        {
            for (const auto& item : json["wars"])
            {
                WarRecord war;
                war.war_id = item.value("war_id", 0);
                war.tribe_a = item.value("tribe_a", 0);
                war.tribe_b = item.value("tribe_b", 0);
                war.declared_at = item.value("declared_at", 0);
                war.start_at = item.value("start_at", 0);
                war.ended_at = item.value("ended_at", 0);
                war.cooldown_end_a = item.value("cooldown_end_a", 0);
                war.cooldown_end_b = item.value("cooldown_end_b", 0);
                war.cancel_requested_by_a = item.value("cancel_requested_by_a", false);
                war.cancel_requested_by_b = item.value("cancel_requested_by_b", false);
                war.start_notified = item.value("start_notified", false);
                war.cooldown_notified = item.value("cooldown_notified", false);
                if (war.war_id > 0)
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
        return pc->TargetingTeamField();
    }

    if (actor->IsA(APrimalCharacter::StaticClass()))
    {
        auto* ch = static_cast<APrimalCharacter*>(actor);
        return ch->TargetingTeamField();
    }

    if (actor->IsA(APrimalDinoCharacter::StaticClass()))
    {
        auto* dino = static_cast<APrimalDinoCharacter*>(actor);
        return dino->TargetingTeamField();
    }

    if (actor->IsA(AController::StaticClass()))
    {
        auto* controller = static_cast<AController*>(actor);
        return controller->TargetingTeamField();
    }

    return 0;
}

int64_t GetTribeIdFromPlayer(AShooterPlayerController* pc)
{
    if (!pc)
        return 0;
    return pc->TargetingTeamField();
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
    return ps->IsTribeOwner(static_cast<unsigned int>(player_data_id));
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
    for (TWeakObjectPtr<APlayerController>& player : players)
    {
        auto* pc = static_cast<AShooterPlayerController*>(player.Get());
        if (pc && GetTribeIdFromPlayer(pc) == tribe_id && IsTribeLeaderOrAdmin(pc))
            return true;
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

void NotifyTribe(int64_t tribe_id, const FString& message)
{
    if (ArkApi::GetApiUtils().GetStatus() != ArkApi::ServerStatus::Ready)
        return;

    auto* world = ArkApi::GetApiUtils().GetWorld();
    if (!world)
        return;

    auto& players = world->PlayerControllerListField();
    for (TWeakObjectPtr<APlayerController>& player : players)
    {
        auto* pc = static_cast<AShooterPlayerController*>(player.Get());
        if (pc && GetTribeIdFromPlayer(pc) == tribe_id)
            SendPlayerMessage(pc, message);
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
    DataLockGuard lock(data_mutex);
    if (auto* war = GetWarForTribeLocked(tribe_id))
    {
        if (GetPhase(*war, Now()) == WarPhase::None)
            return std::nullopt;
        return *war;
    }
    return std::nullopt;
}

bool IsTribeInCooldown(int64_t tribe_id, int64_t now)
{
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
    NotifyTribe(tribe_a, FString::Format(L"Война объявлена. Начало через {}.", *delay));
    NotifyTribe(tribe_b, FString::Format(L"Вашему племени объявили войну. Начало через {}.", *delay));
    // Logging disabled to avoid crashes in early init
}

void RequestCancelWar(int64_t tribe_id)
{
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
    NotifyTribe(other, L"Получен запрос на отмену войны. Чтобы отменить введите /accept");
    NotifyTribe(tribe_id, L"Запрос на отмену войны отправлен.");
    // Logging disabled to avoid crashes in early init
}

void AcceptCancelWar(int64_t tribe_id)
{
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
    NotifyTribe(snapshot.tribe_a, FString::Format(L"Война отменена. Начался откат ({}).", *cooldown));
    NotifyTribe(snapshot.tribe_b, FString::Format(L"Война отменена. Начался откат ({}).", *cooldown));
    // Logging disabled to avoid crashes in early init
}

bool HasIncomingCancel(int64_t tribe_id)
{
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

std::vector<std::pair<int64_t, FString>> ProcessTimers()
{
    std::vector<std::pair<int64_t, FString>> notifications_out;
    
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
                    notifications_out.emplace_back(war.tribe_a, FString(L"Война началась."));
                    notifications_out.emplace_back(war.tribe_b, FString(L"Война началась."));
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
                        notifications_out.emplace_back(war.tribe_a, FString(L"Откат закончился."));
                        notifications_out.emplace_back(war.tribe_b, FString(L"Откат закончился."));
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

void EnqueueNotifications(const std::vector<std::pair<int64_t, FString>>& notes)
{
    if (notes.empty())
        return;
    DataLockGuard lock(notification_mutex);
    pending_notifications.insert(pending_notifications.end(), notes.begin(), notes.end());
}

void FlushNotificationQueue()
{
    std::vector<std::pair<int64_t, FString>> local;
    {
        DataLockGuard lock(notification_mutex);
        if (pending_notifications.empty())
            return;
        local.swap(pending_notifications);
    }

    for (const auto& note : local)
        NotifyTribe(note.first, note.second);
}

void TimerCallback()
{
    if (!plugin_initialized)
        return;

    auto notifications = ProcessTimers();
    EnqueueNotifications(notifications);

    if (ArkApi::GetApiUtils().GetStatus() == ArkApi::ServerStatus::Ready)
        FlushNotificationQueue();

    FlushSaveIfNeeded();
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

    const auto target_tribe = structure->TargetingTeamField();
    int64_t attacker_tribe = 0;

    if (instigator)
        attacker_tribe = GetTribeIdFromActor(instigator);
    if (attacker_tribe == 0 && causer)
        attacker_tribe = GetTribeIdFromActor(causer);

    if (target_tribe == 0 || attacker_tribe == 0)
        return false;

    if (target_tribe == attacker_tribe)
        return true;

    const auto now = Now();
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
        const auto war = GetWarForTribeCopy(tribe_id);
        SendPlayerMessage(pc, GetStatusText(war ? &(*war) : nullptr, tribe_id));
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
        const auto war = GetWarForTribeCopy(tribe_id);
        SendPlayerMessage(pc, GetStatusText(war ? &(*war) : nullptr, tribe_id));
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
    for (int i = 0; i < tribes.Num(); ++i)
    {
        auto& data = const_cast<FTribeData&>(tribes[i]);
        if (list_count >= kMenuDeclareListMax)
            break;
        if (data.TribeIDField() == tribe_id)
            continue;
        if (GetWarForTribeCopy(data.TribeIDField()).has_value() || IsTribeInCooldown(data.TribeIDField(), now))
            continue;

        FTribeRadialMenuEntry item;
        item.EntryName = data.TribeNameField();
        item.EntryDescription = FString(L"Объявить войну");
        item.EntryID = kMenuDeclareListBaseId + list_count;
        item.ParentID = kMenuDeclareId;
        entries->Add(item);
        declare_targets[player_key][item.EntryID] = data.TribeIDField();
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
    for (int i = 0; i < tribes.Num(); ++i)
    {
        if (list_count >= max_targets)
            break;
        auto& data = const_cast<FTribeData&>(tribes[i]);
        if (data.TribeIDField() == tribe_id)
            continue;
        if (GetWarForTribeCopy(data.TribeIDField()).has_value() || IsTribeInCooldown(data.TribeIDField(), now))
            continue;

        const int entry_id = next_index++;
        const auto label = FString::Format(L"Объявить войну: {}", *data.TribeNameField());
        AddMultiUseEntry(entries, entry_id, label);
        declare_targets[player_key][entry_id] = data.TribeIDField();
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

    const auto war = GetWarForTribeCopy(tribe_id);
    const FString status = GetStatusText(war ? &(*war) : nullptr, tribe_id);
    SendPlayerMessage(pc, status);
}

void CmdWarDeclare(AShooterPlayerController* pc, FString*, EChatSendMode::Type)
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

    const auto now = Now();
    if (GetWarForTribeCopy(tribe_id).has_value() || IsTribeInCooldown(tribe_id, now))
    {
        SendPlayerMessage(pc, L"У вашего племени уже есть активная война или откат.");
        return;
    }

    auto* game_mode = ArkApi::GetApiUtils().GetShooterGameMode();
    if (!game_mode)
        return;

    FString message(L"Список племён:\n");
    int count = 0;
    const auto& tribes = game_mode->TribesDataField();
    for (int i = 0; i < tribes.Num(); ++i)
    {
        auto& data = const_cast<FTribeData&>(tribes[i]);
        if (data.TribeIDField() == tribe_id)
            continue;
        if (GetWarForTribeCopy(data.TribeIDField()).has_value() || IsTribeInCooldown(data.TribeIDField(), now))
            continue;

        message += FString::Format(L"{} (ID: {})\n", *data.TribeNameField(), data.TribeIDField());
        count++;
    }

    if (count == 0)
    {
        SendPlayerMessage(pc, L"Нет доступных племён для объявления войны.");
        return;
    }

    message += L"\nИспользуйте /war <tribe_id>, чтобы объявить войну.\n";
    message += L"Если лидер/админ выбранного племени не в сети, объявить войну не получится.";
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
        target_id = std::stoll(parsed[arg_index].ToString());
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
