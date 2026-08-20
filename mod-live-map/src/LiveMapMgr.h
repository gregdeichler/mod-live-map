#pragma once
#include <string>
#include <set>
#include <mutex>
#include <unordered_map>
#include <deque>
#include <hiredis/hiredis.h>

struct PendingUpdate
{
    uint32 charId;
    std::string name;
    uint32 mapId;
    float x, y, z;
    uint32 zone;
    uint8 level;
    uint8 playerClass;
    bool isBot;
    uint32 team; // 0 ally 1 horde
    uint8 race;
    std::string guild;
    bool isDead;
    bool inCombat;
    // --- NEW for 3 features ---
    std::string behaviorState; // Combat, Dead, Looting, Traveling, Fleeing, Grinding, Idle, Flight, etc
    bool isInFlight;
    uint32 taxiOrigin; // TaxiNodes.dbc ID
    uint32 taxiDest;
    uint32 totalPlayed; // seconds
    uint32 levelPlayed;
    uint32 money; // copper
    float healthPct;
    float manaPct;
    uint32 latency; // ms if available
    uint64 ts;
};

class LiveMapMgr
{
public:
    static LiveMapMgr* instance();
    void Init();
    void Shutdown();
    void QueueUpdate(const PendingUpdate& upd);
    void QueueUpdate(uint32 charId, const std::string& name, uint32 mapId, float x, float y, float z, uint32 zone, uint8 level, uint8 playerClass, bool isBot, uint32 team, uint8 race, const std::string& guild, bool isDead, bool inCombat,
                     const std::string& behaviorState, bool isInFlight, uint32 taxiOrigin, uint32 taxiDest, uint32 totalPlayed, uint32 levelPlayed, uint32 money, float healthPct, float manaPct, uint32 latency);
    void RemovePlayer(uint32 charId);
    bool IsMapTracked(uint32 mapId) const;
    void Flush();
private:
    void FlushInternal();
    redisContext* _redis = nullptr;
    std::mutex _lock;
    std::deque<PendingUpdate> _queue;
    bool _useRedis = true;
    bool _useHttp = false;
    std::string _apiUrl;
    std::string _redisHost;
    int _redisPort = 6379;
    uint32 _intervalMs = 1000;
    uint32 _botIntervalMs = 3000;
    std::set<uint32> _mapFilter;
    std::string _secret;
    bool _hasFilter = false;
    bool _trackBots = true;
    bool _trackBotsOnly = false;
};

#define sLiveMap LiveMapMgr::instance()
