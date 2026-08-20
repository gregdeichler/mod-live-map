#pragma once
#include <string>
#include <set>
#include <mutex>
#include <unordered_map>
#include <deque>
#include <queue>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <hiredis/hiredis.h>

struct PendingUpdate
{
    uint32 charId; std::string name; uint32 mapId; float x,y,z; uint32 zone; uint8 level; uint8 playerClass;
    bool isBot; uint32 team; uint8 race; std::string guild; bool isDead; bool inCombat;
    std::string behaviorState; bool isInFlight; uint32 taxiOrigin, taxiDest;
    uint32 totalPlayed, levelPlayed, money; float healthPct, manaPct; uint32 latency; uint64 ts;
};

class HttpWorker
{
public:
    void Start(const std::string& url, const std::string& secret);
    void Stop();
    void Enqueue(std::string payload);
    bool IsRunning() const { return _running.load(); }
private:
    void Run();
    std::string _url,_secret;
    std::mutex _m; std::condition_variable _cv; std::queue<std::string> _q;
    std::atomic<bool> _running{false}; std::thread _thread; static constexpr size_t MAX_Q=256;
};

class LiveMapMgr
{
public:
    static LiveMapMgr* instance(); void Init(); void Shutdown();
    void QueueUpdate(const PendingUpdate& upd);
    void QueueUpdate(uint32 charId, const std::string& name, uint32 mapId, float x,float y,float z, uint32 zone, uint8 level, uint8 playerClass, bool isBot, uint32 team, uint8 race, const std::string& guild, bool isDead, bool inCombat, const std::string& behaviorState, bool isInFlight, uint32 taxiOrigin, uint32 taxiDest, uint32 totalPlayed, uint32 levelPlayed, uint32 money, float healthPct, float manaPct, uint32 latency);
    void RemovePlayer(uint32 charId); bool IsMapTracked(uint32 mapId) const; void Flush();
private:
    bool TryRedisReconnect();
    redisContext* _redis=nullptr; std::mutex _lock; std::mutex _redisLock;
    std::deque<PendingUpdate> _queue; std::unordered_map<uint32,uint32> _lastMapForChar;
    bool _useRedis=true; bool _useHttp=false; std::string _apiUrl,_redisHost; int _redisPort=6379, _redisTimeoutMs=500;
    uint32 _intervalMs=1000,_botIntervalMs=3000,_queueMax=8000; std::set<uint32> _mapFilter; std::string _secret;
    bool _hasFilter=false,_trackBots=true,_trackBotsOnly=false;
    HttpWorker _httpWorker; std::atomic<uint64> _dropped{0}; uint32 _lastReconnectAttempt=0;
};
#define sLiveMap LiveMapMgr::instance()
