#include "ScriptMgr.h"
#include "Player.h"
#include "Map.h"
#include "Config.h"
#include "Log.h"
#include "LiveMapMgr.h"
#include "GuildMgr.h"
#include <unordered_map>
#include <mutex>

static std::unordered_map<uint32, uint32> _lastPlayerUpdate;
static std::unordered_map<uint32, uint32> _lastBotUpdate;
static std::mutex _lastUpdateMutex;

static bool IsBotPlayer(Player* p){
    if(!p) return false;
    if(!p->GetSession()) return true;
    if(p->GetSession()->GetRemoteAddress().empty()) return true;
    return false;
}

// Try to get playerbot AI state if mod-playerbots is present
static std::string GetBehaviorState(Player* player, bool isBot, bool isDead, bool inCombat, bool isInFlight)
{
    if (isDead) return "Dead";
    if (isInFlight) return "Flight";
    if (inCombat) return "Combat";
    if (!player) return "Unknown";

    // If not bot, use movement states
    if (!isBot)
    {
        if (player->IsFalling()) return "Falling";
        if (player->IsInWater() && player->GetPositionZ() < player->GetMap()->GetWaterLevel(player->GetPhaseMask(), player->GetPositionX(), player->GetPositionY(), player->GetPositionZ()))
            return "Swimming";
        if (player->isMoving()) return "Traveling";
        return "Idle";
    }

    // Bot-specific: try to detect playerbots engine state via string in AI
    // We use safe checks without hard dependency - if mod-playerbots exposes GetBotState
    // Fallback to movement-based heuristic that works for all bots
    if (player->IsInCombat()) return "Combat";
    if (player->HasUnitState(UNIT_STATE_LOOTING)) return "Looting";
    if (player->GetMotionMaster() && player->GetMotionMaster()->GetCurrentMovementGeneratorType() == FLIGHT_MOTION_TYPE) return "Flight";
    if (player->GetMotionMaster() && player->GetMotionMaster()->GetCurrentMovementGeneratorType() != IDLE_MOTION_TYPE)
    {
        // If moving to target
        if (player->GetVictim()) return "Combat";
        return "Traveling";
    }
    if (player->IsFalling()) return "Falling";
    if (player->GetSession() && player->GetSession()->GetSecurity() == SEC_PLAYER) // bot has loot?
    {
        // Check if recently looted - use aura check for rest
        if (player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_RESTING)) return "Resting";
    }
    // Default grinding
    return "Grinding";
}

class LiveMap_PlayerScript : public PlayerScript {
public:
    LiveMap_PlayerScript() : PlayerScript("LiveMap_PlayerScript") {}
    void OnPlayerUpdate(Player* player, uint32) override {
        if(!sConfigMgr->GetOption<bool>("LiveMap.Enable", true)) return;
        if(!player || !player->IsInWorld() || player->IsBeingTeleported()) return;
        uint32 mapId = player->GetMapId();
        if(!sLiveMap->IsMapTracked(mapId)) return;
        bool isBot = IsBotPlayer(player);
        if(player->IsGameMaster() && !isBot){
            if(!sConfigMgr->GetOption<bool>("LiveMap.TrackGM", false)) return;
            if(!player->isGMVisible() && !sConfigMgr->GetOption<bool>("LiveMap.TrackGMInvisible", false)) return;
        }
        uint32 guidLow = player->GetGUID().GetCounter();
        uint32 now = getMSTime();
        uint32 pInt = sConfigMgr->GetOption<uint32>("LiveMap.UpdateIntervalMs", 1000);
        uint32 bInt = sConfigMgr->GetOption<uint32>("LiveMap.BotUpdateIntervalMs", 3000);
        uint32 interval = isBot ? bInt : pInt;
        { std::lock_guard<std::mutex> g(_lastUpdateMutex); auto& m = isBot ? _lastBotUpdate : _lastPlayerUpdate; auto it=m.find(guidLow); if(it!=m.end() && (now-it->second)<interval) return; m[guidLow]=now; }

        // Character Inspector fields
        std::string guildName = "";
        if (Guild* g = sGuildMgr->GetGuildById(player->GetGuildId())) guildName = g->GetName();
        bool isDead = !player->IsAlive();
        bool inCombat = player->IsInCombat();
        bool isInFlight = player->IsInFlight();
        uint32 taxiOrigin = 0, taxiDest = 0;
        if (isInFlight)
        {
            // Get taxi path: first and last nodes
            if (player->m_taxi.GetTaxiSource()) taxiOrigin = player->m_taxi.GetTaxiSource();
            if (player->m_taxi.GetTaxiDestination()) taxiDest = player->m_taxi.GetTaxiDestination();
        }
        std::string behavior = GetBehaviorState(player, isBot, isDead, inCombat, isInFlight);
        uint32 totalPlayed = player->GetTotalPlayedTime();
        uint32 levelPlayed = player->GetLevelPlayedTime();
        uint32 money = player->GetMoney();
        float healthPct = player->GetMaxHealth() > 0 ? (float)player->GetHealth() * 100.0f / (float)player->GetMaxHealth() : 0.0f;
        float manaPct = player->GetMaxPower(POWER_MANA) > 0 ? (float)player->GetPower(POWER_MANA) * 100.0f / (float)player->GetMaxPower(POWER_MANA) : 0.0f;
        uint32 latency = 0;
        if (player->GetSession()) latency = player->GetSession()->GetLatency();

        PendingUpdate upd;
        upd.charId = guidLow;
        upd.name = player->GetName();
        upd.mapId = mapId;
        upd.x = player->GetPositionX(); upd.y = player->GetPositionY(); upd.z = player->GetPositionZ();
        upd.zone = player->GetZoneId();
        upd.level = player->getLevel();
        upd.playerClass = player->getClass();
        upd.isBot = isBot;
        upd.team = player->GetTeamId();
        upd.race = player->getRace();
        upd.guild = guildName;
        upd.isDead = isDead;
        upd.inCombat = inCombat;
        upd.behaviorState = behavior;
        upd.isInFlight = isInFlight;
        upd.taxiOrigin = taxiOrigin;
        upd.taxiDest = taxiDest;
        upd.totalPlayed = totalPlayed;
        upd.levelPlayed = levelPlayed;
        upd.money = money;
        upd.healthPct = healthPct;
        upd.manaPct = manaPct;
        upd.latency = latency;
        upd.ts = (uint64)time(nullptr);

        sLiveMap->QueueUpdate(upd);
    }
    void OnPlayerLogout(Player* p) override { if(!p) return; uint32 id=p->GetGUID().GetCounter(); { std::lock_guard<std::mutex> g(_lastUpdateMutex); _lastPlayerUpdate.erase(id); _lastBotUpdate.erase(id);} sLiveMap->RemovePlayer(id); }
    void OnPlayerLogin(Player* p) override { if(!p) return; uint32 id=p->GetGUID().GetCounter(); std::lock_guard<std::mutex> g(_lastUpdateMutex); _lastPlayerUpdate.erase(id); _lastBotUpdate.erase(id); }
};

class LiveMap_WorldScript : public WorldScript {
public:
    LiveMap_WorldScript() : WorldScript("LiveMap_WorldScript") {}
    void OnStartup() override { sLiveMap->Init(); LOG_INFO("module","[LiveMap] Started with CharacterInspector+BehaviorState+FlightPath"); }
    void OnShutdown() override { sLiveMap->Shutdown(); }
    void OnAfterUpdate(uint32 diff) override { static uint32 t=0; t+=diff; if(t>=500){ sLiveMap->Flush(); t=0; } }
};

void Addmod_live_mapScripts(){ new LiveMap_PlayerScript(); new LiveMap_WorldScript(); }
