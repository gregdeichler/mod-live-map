#include "LiveMapMgr.h"
#include "Config.h"
#include "Log.h"
#include <nlohmann/json.hpp>
#include <sstream>
#include <vector>
#include <chrono>

static std::once_flag curlInitFlag;

static uint64_t now_ms()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

LiveMapMgr* LiveMapMgr::instance() { static LiveMapMgr i; return &i; }

// Try to reconnect to Redis. Thread-safe and idempotent.
bool LiveMapMgr::TryRedisReconnect()
{
    // simple backoff: only attempt reconnect if enough time passed since last attempt
    uint32_t last = _lastReconnectAttempt.load(std::memory_order_relaxed);
    uint32_t now = static_cast<uint32_t>(now_ms() / 1000);
    if (now - last < 2) // 2s between attempts
        return _redis != nullptr && !_redis->err;

    // mark attempt time
    _lastReconnectAttempt.store(now, std::memory_order_relaxed);

    std::lock_guard<std::mutex> lk(_redisLock);
    if (_redis && !_redis->err)
        return true;

    if (_redis) {
        redisFree(_redis);
        _redis = nullptr;
    }

    LOG_INFO("module", "LiveMap: attempting redis reconnect to {}:{}", _redisHost, _redisPort);
    _redis = redisConnect(_redisHost.c_str(), _redisPort);
    if (!_redis || _redis->err) {
        LOG_ERROR("module", "LiveMap Redis reconnect failed: {}", _redis ? _redis->errstr : "no context");
        if (_redis) { redisFree(_redis); _redis = nullptr; }
        return false;
    }
    LOG_INFO("module", "LiveMap: redis reconnect successful");
    return true;
}

void LiveMapMgr::Init()
{
    _useRedis = sConfigMgr->GetOption<bool>("LiveMap.UseRedis", true);
    _useHttp = sConfigMgr->GetOption<bool>("LiveMap.UseHttp", false);
    _apiUrl = sConfigMgr->GetOption<std::string>("LiveMap.ApiUrl", "http://127.0.0.1:8000/api/positions/batch");
    _redisHost = sConfigMgr->GetOption<std::string>("LiveMap.RedisHost", "127.0.0.1");
    _redisPort = sConfigMgr->GetOption<int>("LiveMap.RedisPort", 6379);
    _intervalMs = sConfigMgr->GetOption<uint32>("LiveMap.UpdateIntervalMs", 1000);
    _botIntervalMs = sConfigMgr->GetOption<uint32>("LiveMap.BotUpdateIntervalMs", 3000);
    _secret = sConfigMgr->GetOption<std::string>("LiveMap.Secret", "local-secret-123");
    _trackBots = sConfigMgr->GetOption<bool>("LiveMap.TrackBots", true);
    _trackBotsOnly = sConfigMgr->GetOption<bool>("LiveMap.TrackBotsOnly", false);
    std::string mf = sConfigMgr->GetOption<std::string>("LiveMap.MapFilter", "0");
    _hasFilter = (mf != "" && mf != "all");
    _mapFilter.clear();
    if (_hasFilter) {
        std::stringstream ss(mf);
        std::string t;
        while (std::getline(ss, t, ',')) {
            try {
                t.erase(0, t.find_first_not_of(" \t"));
                t.erase(t.find_last_not_of(" \t") + 1);
                if (!t.empty()) _mapFilter.insert(static_cast<uint32>(std::stoul(t)));
            } catch (...) {}
        }
    }

    if (_useRedis) {
        std::lock_guard<std::mutex> lk(_redisLock);
        _redis = redisConnect(_redisHost.c_str(), _redisPort);
        if (!_redis || _redis->err) {
            LOG_ERROR("module", "LiveMap Redis {}:{} connect failed: {}", _redisHost, _redisPort, _redis ? _redis->errstr : "no context");
            if (_redis) { redisFree(_redis); _redis = nullptr; }
        }
    }

    // curl global init once
    std::call_once(curlInitFlag, [](){ curl_global_init(CURL_GLOBAL_ALL); });

    LOG_INFO("module", "LiveMap Init Maps={} PInt={} BInt={} Bots={} + Behavior + Flight + Inspector", mf, _intervalMs, _botIntervalMs, _trackBots);
}

void LiveMapMgr::Shutdown()
{
    // stop HTTP worker first
    _httpWorker.Stop();

    {
        std::lock_guard<std::mutex> g(_lock);
        _queue.clear();
        _lastMapForChar.clear();
    }

    {
        std::lock_guard<std::mutex> g(_redisLock);
        if (_redis) { redisFree(_redis); _redis = nullptr; }
    }

    // cleanup libcurl global
    curl_global_cleanup();
}

bool LiveMapMgr::IsMapTracked(uint32 id) const { if (!_hasFilter) return true; return _mapFilter.count(id) > 0; }

void LiveMapMgr::QueueUpdate(const PendingUpdate& upd)
{
    if (!IsMapTracked(upd.mapId)) return;
    if (upd.isBot && !_trackBots) return;
    if (!upd.isBot && _trackBotsOnly) return;
    std::lock_guard<std::mutex> g(_lock);
    _queue.push_back(upd);
    if (_queue.size() > _maxQueue) { _queue.pop_front(); _dropped++; if ((_dropped % 1000) == 0) LOG_WARNING("module","LiveMap dropped {} updates", _dropped); }
}

void LiveMapMgr::QueueUpdate(uint32 charId, const std::string& name, uint32 mapId, float x, float y, float z, uint32 zone, uint8 level, uint8 cls, bool isBot, uint32 team, uint8 race, const std::string& guild,
                             bool isDead, bool inCombat, const std::string& behaviorState, bool isInFlight, uint32 taxiOrigin, uint32 taxiDest, uint32 totalPlayed, uint32 levelPlayed, uint32 money, float healthPct, float manaPct, uint32 latency)
{
    PendingUpdate u;
    u.charId = charId; u.name = name; u.mapId = mapId; u.x = x; u.y = y; u.z = z; u.zone = zone; u.level = level; u.playerClass = cls; u.isBot = isBot; u.team = team; u.race = race; u.guild = guild; u.isDead = isDead; u.inCombat = inCombat;
    u.behaviorState = behaviorState; u.isInFlight = isInFlight; u.taxiOrigin = taxiOrigin; u.taxiDest = taxiDest; u.totalPlayed = totalPlayed; u.levelPlayed = levelPlayed; u.money = money; u.healthPct = healthPct; u.manaPct = manaPct; u.latency = latency; u.ts = now_ms();
    QueueUpdate(u);
}

void LiveMapMgr::RemovePlayer(uint32 charId)
{
    // remove pending updates for this char from queue
    {
        std::lock_guard<std::mutex> g(_lock);
        for (auto it = _queue.begin(); it != _queue.end(); ) {
            if (it->charId == charId) it = _queue.erase(it);
            else ++it;
        }
    }

    // find last map for char
    uint32 mapId = 0;
    {
        std::lock_guard<std::mutex> g(_redisLock);
        auto it = _lastMapForChar.find(charId);
        if (it != _lastMapForChar.end()) { mapId = it->second; _lastMapForChar.erase(it); }
    }

    if (_useRedis) {
        std::lock_guard<std::mutex> g(_redisLock);
        if (!_redis || _redis->err) {
            // try reconnect outside of lock
        }
    }

    nlohmann::json j;
    j["charId"] = charId;
    j["remove"] = true;
    j["ts"] = now_ms();
    std::string payload = j.dump();

    if (_useRedis) {
        bool have = false;
        {
            std::lock_guard<std::mutex> g(_redisLock);
            have = (_redis && !_redis->err);
        }
        if (have) {
            // publish removal to last known map
            std::string channel = std::string("map:") + std::to_string(mapId);
            // use binary-safe publish
            redisAppendCommand(_redis, "PUBLISH %b %b", channel.c_str(), (size_t)channel.size(), payload.c_str(), (size_t)payload.size());
            // also delete snapshot
            redisAppendCommand(_redis, "DEL %b", (std::string("live:char:") + std::to_string(charId)).c_str(), (size_t)(std::string("live:char:") + std::to_string(charId)).size());
            // attempt to drain replies
            int expected = 2;
            redisReply* rep = nullptr;
            for (int i = 0; i < expected; ++i) {
                if (redisGetReply(_redis, (void**)&rep) != REDIS_OK) {
                    LOG_ERROR("module", "LiveMap redisGetReply failed during RemovePlayer");
                    if (rep) freeReplyObject(rep);
                    redisFree(_redis); _redis = nullptr;
                    break;
                }
                if (rep) freeReplyObject(rep);
                rep = nullptr;
            }
        }
    }

    // Also send via HTTP worker if configured
    if (_useHttp) {
        _httpWorker.Enqueue(payload);
    }
}

void LiveMapMgr::Flush()
{
    // swap queue out under lock
    std::deque<PendingUpdate> local;
    {
        std::lock_guard<std::mutex> g(_lock);
        if (_queue.empty()) return;
        local.swap(_queue);
    }

    // group by map
    std::unordered_map<uint32, std::vector<PendingUpdate>> byMap;
    for (auto& u : local) byMap[u.mapId].push_back(u);

    // Attempt redis work if enabled
    if (_useRedis) {
        // ensure redis connected; do not hold _redisLock while calling TryRedisReconnect
        bool need = false;
        {
            std::lock_guard<std::mutex> g(_redisLock);
            need = (!_redis || _redis->err);
        }
        if (need) TryRedisReconnect();

        std::lock_guard<std::mutex> g(_redisLock);
        if (_redis && !_redis->err) {
            for (auto& kv : byMap) {
                uint32 mapId = kv.first;
                auto& vec = kv.second;

                // build batch json (for HTTP fallback if needed)
                nlohmann::json batch = nlohmann::json::array();

                // append SETEX for each char
                for (auto& u : vec) {
                    nlohmann::json j;
                    j["charId"] = u.charId; j["name"] = u.name; j["mapId"] = u.mapId;
                    j["world_x"] = u.x; j["world_y"] = u.y; j["world_z"] = u.z; j["zone"] = u.zone;
                    j["level"] = u.level; j["class"] = u.playerClass;
                    j["behavior"] = u.behaviorState; j["isInFlight"] = u.isInFlight; j["taxiOrigin"] = u.taxiOrigin; j["taxiDest"] = u.taxiDest;
                    j["totalPlayed"] = u.totalPlayed; j["levelPlayed"] = u.levelPlayed; j["money"] = u.money; j["healthPct"] = u.healthPct; j["manaPct"] = u.manaPct; j["latency"] = u.latency;
                    j["ts"] = u.ts;
                    batch.push_back(j);

                    std::string single = j.dump();
                    // binary-safe SETEX
                    std::string key = std::string("live:char:") + std::to_string(u.charId);
                    redisAppendCommand(_redis, "SETEX %b %d %b", key.c_str(), (size_t)key.size(), 30, single.c_str(), (size_t)single.size());

                    // track last map for removals
                    {
                        std::lock_guard<std::mutex> lm(_redisLock);
                        _lastMapForChar[u.charId] = mapId;
                    }
                }

                // SADD live:map:<mapId> <ids...>
                std::vector<std::string> argv;
                argv.push_back("SADD");
                std::string key = std::string("live:map:") + std::to_string(mapId);
                argv.push_back(key);
                for (auto& u : vec) argv.push_back(std::to_string(u.charId));
                std::vector<const char*> argv_c; argv_c.reserve(argv.size());
                std::vector<size_t> argv_len; argv_len.reserve(argv.size());
                for (auto& s : argv) { argv_c.push_back(s.c_str()); argv_len.push_back(s.size()); }
                redisAppendCommandArgv(_redis, (int)argv_c.size(), argv_c.data(), argv_len.data());

                // Publish batch to channel map:<mapId>
                std::string ch = std::string("map:") + std::to_string(mapId);
                std::string payload = batch.dump();
                redisAppendCommand(_redis, "PUBLISH %b %b", ch.c_str(), (size_t)ch.size(), payload.c_str(), (size_t)payload.size());
            }

            // Drain replies: compute an approximate expected count: for each char we appended SETEX (1), plus one SADD and one PUBLISH per map
            int expected = 0;
            for (auto& kv : byMap) expected += (int)kv.second.size() + 2;

            redisReply* rep = nullptr;
            for (int i = 0; i < expected; ++i) {
                if (redisGetReply(_redis, (void**)&rep) != REDIS_OK) {
                    LOG_ERROR("module", "LiveMap redisGetReply failed during Flush at reply {}/{}", i, expected);
                    if (rep) freeReplyObject(rep);
                    redisFree(_redis); _redis = nullptr;
                    break;
                }
                if (rep) freeReplyObject(rep);
                rep = nullptr;
            }
        }
    }

    // HTTP fallback: send batches for maps if enabled
    if (_useHttp) {
        for (auto& kv : byMap) {
            nlohmann::json batch = nlohmann::json::array();
            for (auto& u : kv.second) {
                nlohmann::json j;
                j["charId"] = u.charId; j["name"] = u.name; j["mapId"] = u.mapId;
                j["world_x"] = u.x; j["world_y"] = u.y; j["world_z"] = u.z; j["zone"] = u.zone;
                j["level"] = u.level; j["class"] = u.playerClass; j["behavior"] = u.behaviorState; j["ts"] = u.ts;
                batch.push_back(j);
            }
            std::string payload = batch.dump();
            _httpWorker.Enqueue(payload);
        }
    }
}
