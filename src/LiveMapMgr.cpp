#include "LiveMapMgr.h"
#include "Config.h"
#include "Log.h"
#include <nlohmann/json.hpp>
#include <curl/curl.h>

// HttpWorker - libcurl, no shell, bounded queue
void HttpWorker::Start(const std::string& url, const std::string& secret)
{
    _url = url;
    _secret = secret;
    _running = true;
    _thread = std::thread(&HttpWorker::Run, this);
}
void HttpWorker::Stop()
{
    {
        std::lock_guard<std::mutex> g(_m);
        _running = false;
    }
    _cv.notify_all();
    if (_thread.joinable()) _thread.join();
    std::lock_guard<std::mutex> g(_m);
    while (!_q.empty()) _q.pop();
}
void HttpWorker::Enqueue(std::string payload)
{
    std::lock_guard<std::mutex> g(_m);
    if (_q.size() >= MAX_Q) {
        _q.pop(); // drop oldest - backpressure
    }
    _q.push(std::move(payload));
    _cv.notify_one();
}
void HttpWorker::Run()
{
    curl_global_init(CURL_GLOBAL_DEFAULT);
    CURL* curl = curl_easy_init();
    if (!curl) return;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    std::string secretHeader = "X-LiveMap-Secret: " + _secret;
    headers = curl_slist_append(headers, secretHeader.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 2L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    while (true)
    {
        std::string payload;
        {
            std::unique_lock<std::mutex> lk(_m);
            _cv.wait(lk, [&]{ return !_q.empty() || !_running; });
            if (!_running && _q.empty()) break;
            if (_q.empty()) continue;
            payload = std::move(_q.front());
            _q.pop();
        }
        curl_easy_setopt(curl, CURLOPT_URL, _url.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)payload.size());
        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            LOG_ERROR("module", "LiveMap HTTP POST failed: {} url={}", curl_easy_strerror(res), _url);
        }
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
}

// LiveMapMgr

LiveMapMgr* LiveMapMgr::instance(){ static LiveMapMgr i; return &i; }

void LiveMapMgr::Init()
{
    _useRedis = sConfigMgr->GetOption<bool>("LiveMap.UseRedis", true);
    _useHttp = sConfigMgr->GetOption<bool>("LiveMap.UseHttp", false);
    _apiUrl = sConfigMgr->GetOption<std::string>("LiveMap.ApiUrl", "http://127.0.0.1:8000/api/positions/batch");
    _redisHost = sConfigMgr->GetOption<std::string>("LiveMap.RedisHost", "127.0.0.1");
    _redisPort = sConfigMgr->GetOption<int>("LiveMap.RedisPort", 6379);
    _redisTimeoutMs = sConfigMgr->GetOption<int>("LiveMap.RedisTimeoutMs", 500);
    _intervalMs = sConfigMgr->GetOption<uint32>("LiveMap.UpdateIntervalMs", 1000);
    _botIntervalMs = sConfigMgr->GetOption<uint32>("LiveMap.BotUpdateIntervalMs", 3000);
    _secret = sConfigMgr->GetOption<std::string>("LiveMap.Secret", "local-secret-123");
    _trackBots = sConfigMgr->GetOption<bool>("LiveMap.TrackBots", true);
    _trackBotsOnly = sConfigMgr->GetOption<bool>("LiveMap.TrackBotsOnly", false);
    _queueMax = sConfigMgr->GetOption<uint32>("LiveMap.QueueMax", 8000);
    std::string mf = sConfigMgr->GetOption<std::string>("LiveMap.MapFilter", "0");
    _hasFilter = (mf != "" && mf != "all");
    _mapFilter.clear();
    if(_hasFilter){
        std::stringstream ss(mf); std::string t;
        while(std::getline(ss,t,',')){
            try{
                t.erase(0,t.find_first_not_of(" \t"));
                t.erase(t.find_last_not_of(" \t")+1);
                if(!t.empty()) _mapFilter.insert((uint32)std::stoul(t));
            }catch(...){}
        }
    }
    if(_useRedis){
        timeval tv = { _redisTimeoutMs / 1000, (_redisTimeoutMs % 1000) * 1000 };
        _redis = redisConnectWithTimeout(_redisHost.c_str(), _redisPort, tv);
        if(!_redis || _redis->err){
            if(_redis){ LOG_ERROR("module","LiveMap Redis {}:{} err {}",_redisHost,_redisPort,_redis->errstr); redisFree(_redis); _redis=nullptr; }
        } else {
            LOG_INFO("module","LiveMap Redis {}:{}",_redisHost,_redisPort);
        }
    }
    if(_useHttp){
        _httpWorker.Start(_apiUrl, _secret);
    }
    LOG_INFO("module","LiveMap Init secure version Maps={} Bots={} QueueMax={}", mf, _trackBots, _queueMax);
}

bool LiveMapMgr::TryRedisReconnect()
{
    uint32 now = getMSTime();
    if (now - _lastReconnectAttempt < 5000) return false; // backoff 5s
    _lastReconnectAttempt = now;
    std::lock_guard<std::mutex> g(_redisLock);
    if (_redis) {
        redisFree(_redis);
        _redis = nullptr;
    }
    timeval tv = { _redisTimeoutMs / 1000, (_redisTimeoutMs % 1000) * 1000 };
    _redis = redisConnectWithTimeout(_redisHost.c_str(), _redisPort, tv);
    if (!_redis || _redis->err) {
        if (_redis) { LOG_ERROR("module","LiveMap Redis reconnect failed {}:{} {}",_redisHost,_redisPort,_redis->errstr); redisFree(_redis); _redis=nullptr; }
        return false;
    }
    LOG_INFO("module","LiveMap Redis reconnected {}:{}",_redisHost,_redisPort);
    return true;
}

void LiveMapMgr::Shutdown(){
    _httpWorker.Stop();
    std::lock_guard<std::mutex> g1(_lock);
    std::lock_guard<std::mutex> g2(_redisLock);
    _queue.clear();
    _lastMapForChar.clear();
    if(_redis){ redisFree(_redis); _redis=nullptr; }
}

bool LiveMapMgr::IsMapTracked(uint32 id) const { if(!_hasFilter) return true; return _mapFilter.count(id)>0; }

void LiveMapMgr::QueueUpdate(const PendingUpdate& upd)
{
    if(!IsMapTracked(upd.mapId)) return;
    if(upd.isBot && !_trackBots) return;
    if(!upd.isBot && _trackBotsOnly) return;
    std::lock_guard<std::mutex> g(_lock);
    if (_queue.size() >= _queueMax) {
        _queue.pop_front();
        _dropped++;
        if (_dropped % 1000 == 1) {
            LOG_WARN("module","LiveMap queue overflow, dropped {} total", _dropped.load());
        }
    }
    _queue.push_back(upd);
    _lastMapForChar[upd.charId] = upd.mapId;
}

void LiveMapMgr::QueueUpdate(uint32 charId, const std::string& name, uint32 mapId, float x,float y,float z, uint32 zone, uint8 level, uint8 cls, bool isBot, uint32 team, uint8 race, const std::string& guild, bool isDead, bool inCombat, const std::string& behaviorState, bool isInFlight, uint32 taxiOrigin, uint32 taxiDest, uint32 totalPlayed, uint32 levelPlayed, uint32 money, float healthPct, float manaPct, uint32 latency)
{
    PendingUpdate u;
    u.charId=charId; u.name=name; u.mapId=mapId; u.x=x; u.y=y; u.z=z; u.zone=zone; u.level=level; u.playerClass=cls; u.isBot=isBot; u.team=team; u.race=race; u.guild=guild; u.isDead=isDead; u.inCombat=inCombat; u.behaviorState=behaviorState; u.isInFlight=isInFlight; u.taxiOrigin=taxiOrigin; u.taxiDest=taxiDest; u.totalPlayed=totalPlayed; u.levelPlayed=levelPlayed; u.money=money; u.healthPct=healthPct; u.manaPct=manaPct; u.latency=latency; u.ts=(uint64)time(nullptr);
    QueueUpdate(u);
}

void LiveMapMgr::RemovePlayer(uint32 charId)
{
    uint32 lastMap = 0;
    {
        std::lock_guard<std::mutex> g(_lock);
        auto it = _lastMapForChar.find(charId);
        if (it != _lastMapForChar.end()) { lastMap = it->second; _lastMapForChar.erase(it); }
        _queue.erase(std::remove_if(_queue.begin(),_queue.end(),[charId](auto& u){return u.charId==charId;}), _queue.end());
    }
    // publish removal to correct map, plus generic map:0 for backward compat
    std::lock_guard<std::mutex> g(_redisLock);
    if(_redis && !_redis->err){
        nlohmann::json j; j["charId"]=charId; j["remove"]=true; j["ts"]=(uint64)time(nullptr); j["mapId"]=lastMap;
        std::string s=j.dump();
        // binary-safe publish
        redisReply* r = (redisReply*)redisCommand(_redis, "PUBLISH %b %b", 
            ("map:"+std::to_string(lastMap ? lastMap : 0)).c_str(), ("map:"+std::to_string(lastMap ? lastMap : 0)).size(),
            s.c_str(), s.size());
        if(r) freeReplyObject(r);
        if (lastMap != 0) {
            r = (redisReply*)redisCommand(_redis, "PUBLISH %b %b", "map:0", (size_t)5, s.c_str(), s.size());
            if(r) freeReplyObject(r);
        }
        r=(redisReply*)redisCommand(_redis, "DEL live:char:%d", charId);
        if(r) freeReplyObject(r);
    }
}

void LiveMapMgr::Flush()
{
    std::deque<PendingUpdate> local;
    {
        std::lock_guard<std::mutex> g(_lock);
        if(_queue.empty()) return;
        local.swap(_queue);
    }

    // Group by map
    std::unordered_map<uint32,std::vector<PendingUpdate>> byMap;
    for(auto& u: local) byMap[u.mapId].push_back(u);

    // ---- Redis pipelined, binary-safe, no lock held during IO ----
    if(_useRedis){
        std::lock_guard<std::mutex> g(_redisLock);
        if(!_redis || _redis->err){
            if(!TryRedisReconnect()){
                // drop or keep for http fallback, don't hold lock
            }
        }
        if(_redis && !_redis->err){
            int expectedReplies = 0;
            for(auto& [mapId, vec] : byMap){
                // SETEX per char binary-safe
                for(auto& u : vec){
                    nlohmann::json j;
                    j["charId"]=u.charId; j["name"]=u.name; j["mapId"]=u.mapId; j["world_x"]=u.x; j["world_y"]=u.y; j["world_z"]=u.z; j["zone"]=u.zone; j["level"]=u.level; j["class"]=u.playerClass; j["isBot"]=u.isBot; j["team"]=u.team; j["race"]=u.race; j["guild"]=u.guild; j["isDead"]=u.isDead; j["inCombat"]=u.inCombat;
                    j["behavior"]=u.behaviorState; j["isInFlight"]=u.isInFlight; j["taxiOrigin"]=u.taxiOrigin; j["taxiDest"]=u.taxiDest;
                    j["totalPlayed"]=u.totalPlayed; j["levelPlayed"]=u.levelPlayed; j["money"]=u.money; j["healthPct"]=u.healthPct; j["manaPct"]=u.manaPct; j["latency"]=u.latency; j["ts"]=u.ts;
                    std::string single=j.dump();
                    // %b for binary-safe JSON
                    if(redisAppendCommand(_redis, "SETEX live:char:%d %d %b", u.charId, 30, single.c_str(), single.size()) != REDIS_OK){
                        LOG_ERROR("module","LiveMap SETEX append failed");
                    } else expectedReplies++;
                }
                // SADD using argv - binary-safe, no concatenation
                // Build argv: SADD key member1 member2 ...
                std::string saddKey = "live:map:" + std::to_string(mapId);
                std::vector<std::string> idStrs;
                idStrs.reserve(vec.size());
                for(auto& u: vec) idStrs.push_back(std::to_string(u.charId));

                std::vector<const char*> argv;
                std::vector<size_t> argvlen;
                argv.reserve(2 + idStrs.size());
                argvlen.reserve(2 + idStrs.size());
                argv.push_back("SADD"); argvlen.push_back(4);
                argv.push_back(saddKey.c_str()); argvlen.push_back(saddKey.size());
                for(auto& s : idStrs){ argv.push_back(s.c_str()); argvlen.push_back(s.size()); }

                if(redisAppendCommandArgv(_redis, (int)argv.size(), argv.data(), argvlen.data()) != REDIS_OK){
                    LOG_ERROR("module","LiveMap SADD append failed map {}", mapId);
                } else expectedReplies++;

                // EXPIRE
                if(redisAppendCommand(_redis, "EXPIRE %b %d", saddKey.c_str(), saddKey.size(), 35) != REDIS_OK){
                    LOG_ERROR("module","LiveMap EXPIRE append failed");
                } else expectedReplies++;

                // PUBLISH batch per map - binary-safe
                nlohmann::json batch = nlohmann::json::array();
                for(auto& u: vec){
                    nlohmann::json j;
                    j["charId"]=u.charId; j["name"]=u.name; j["mapId"]=u.mapId; j["world_x"]=u.x; j["world_y"]=u.y; j["world_z"]=u.z; j["zone"]=u.zone; j["level"]=u.level; j["class"]=u.playerClass; j["isBot"]=u.isBot; j["team"]=u.team; j["race"]=u.race; j["guild"]=u.guild; j["isDead"]=u.isDead; j["inCombat"]=u.inCombat;
                    j["behavior"]=u.behaviorState; j["isInFlight"]=u.isInFlight; j["taxiOrigin"]=u.taxiOrigin; j["taxiDest"]=u.taxiDest;
                    j["totalPlayed"]=u.totalPlayed; j["levelPlayed"]=u.levelPlayed; j["money"]=u.money; j["healthPct"]=u.healthPct; j["manaPct"]=u.manaPct; j["latency"]=u.latency; j["ts"]=u.ts;
                    batch.push_back(j);
                }
                std::string bs = batch.dump();
                std::string chan = "map:" + std::to_string(mapId);
                if(redisAppendCommand(_redis, "PUBLISH %b %b", chan.c_str(), chan.size(), bs.c_str(), bs.size()) != REDIS_OK){
                    LOG_ERROR("module","LiveMap PUBLISH append failed");
                } else expectedReplies++;
            }

            // Drain replies robustly
            redisReply* rep = nullptr;
            for(int i=0;i<expectedReplies;++i){
                if(redisGetReply(_redis, (void**)&rep) != REDIS_OK){
                    LOG_ERROR("module","LiveMap redisGetReply failed at {}/{}", i, expectedReplies);
                    if(_redis) { _redis->err = 1; } // force reconnect next time
                    break;
                }
                if(!rep){ LOG_ERROR("module","LiveMap null reply {}/{}", i, expectedReplies); break; }
                if(rep->type == REDIS_REPLY_ERROR){
                    LOG_ERROR("module","LiveMap Redis error reply: {}", rep->str ? rep->str : "<empty>");
                }
                freeReplyObject(rep);
                rep = nullptr;
            }
        }
    }

    // HTTP via worker - no shell, no injection
    if(_useHttp && !local.empty()){
        nlohmann::json batch=nlohmann::json::array();
        for(auto& u: local){
            nlohmann::json j;
            j["charId"]=u.charId; j["name"]=u.name; j["mapId"]=u.mapId; j["world_x"]=u.x; j["world_y"]=u.y; j["world_z"]=u.z; j["zone"]=u.zone; j["level"]=u.level; j["class"]=u.playerClass; j["isBot"]=u.isBot; j["team"]=u.team; j["race"]=u.race; j["guild"]=u.guild; j["isDead"]=u.isDead; j["inCombat"]=u.inCombat;
            j["behavior"]=u.behaviorState; j["isInFlight"]=u.isInFlight; j["taxiOrigin"]=u.taxiOrigin; j["taxiDest"]=u.taxiDest;
            j["totalPlayed"]=u.totalPlayed; j["levelPlayed"]=u.levelPlayed; j["money"]=u.money; j["healthPct"]=u.healthPct; j["manaPct"]=u.manaPct; j["latency"]=u.latency; j["ts"]=u.ts;
            batch.push_back(j);
        }
        _httpWorker.Enqueue(batch.dump());
    }
}
