#include "LiveMapMgr.h"
#include "Config.h"
#include "Log.h"
#include <nlohmann/json.hpp>

LiveMapMgr* LiveMapMgr::instance() { static LiveMapMgr i; return &i; }

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
    if (_hasFilter) { std::stringstream ss(mf); std::string t; while(std::getline(ss,t,',')){ try{ t.erase(0,t.find_first_not_of(" \t")); t.erase(t.find_last_not_of(" \t")+1); if(!t.empty()) _mapFilter.insert((uint32)std::stoul(t)); }catch(...){} } }
    if (_useRedis) { _redis = redisConnect(_redisHost.c_str(), _redisPort); if(!_redis || _redis->err){ if(_redis){ LOG_ERROR("module","LiveMap Redis {}:{} {}",_redisHost,_redisPort,_redis->errstr); redisFree(_redis); _redis=nullptr; } } else LOG_INFO("module","LiveMap Redis {}:{}",_redisHost,_redisPort); }
    LOG_INFO("module","LiveMap Init Maps={} PInt={} BInt={} Bots={} + Behavior + Flight + Inspector", mf, _intervalMs, _botIntervalMs, _trackBots);
}
void LiveMapMgr::Shutdown(){ std::lock_guard<std::mutex> g(_lock); _queue.clear(); if(_redis){ redisFree(_redis); _redis=nullptr; } }
bool LiveMapMgr::IsMapTracked(uint32 id) const { if(!_hasFilter) return true; return _mapFilter.count(id)>0; }

void LiveMapMgr::QueueUpdate(const PendingUpdate& upd)
{
    if(!IsMapTracked(upd.mapId)) return;
    if(upd.isBot && !_trackBots) return;
    if(!upd.isBot && _trackBotsOnly) return;
    std::lock_guard<std::mutex> g(_lock);
    _queue.push_back(upd);
    if(_queue.size()>8000) _queue.pop_front();
}

void LiveMapMgr::QueueUpdate(uint32 charId, const std::string& name, uint32 mapId, float x,float y,float z, uint32 zone, uint8 level, uint8 cls, bool isBot, uint32 team, uint8 race, const std::string& guild, bool isDead, bool inCombat,
                     const std::string& behaviorState, bool isInFlight, uint32 taxiOrigin, uint32 taxiDest, uint32 totalPlayed, uint32 levelPlayed, uint32 money, float healthPct, float manaPct, uint32 latency)
{
    PendingUpdate u;
    u.charId=charId; u.name=name; u.mapId=mapId; u.x=x; u.y=y; u.z=z; u.zone=zone; u.level=level; u.playerClass=cls; u.isBot=isBot; u.team=team; u.race=race; u.guild=guild; u.isDead=isDead; u.inCombat=inCombat;
    u.behaviorState=behaviorState; u.isInFlight=isInFlight; u.taxiOrigin=taxiOrigin; u.taxiDest=taxiDest; u.totalPlayed=totalPlayed; u.levelPlayed=levelPlayed; u.money=money; u.healthPct=healthPct; u.manaPct=manaPct; u.latency=latency; u.ts=(uint64)time(nullptr);
    QueueUpdate(u);
}

void LiveMapMgr::RemovePlayer(uint32 charId){ std::lock_guard<std::mutex> g(_lock); if(_redis && !_redis->err){ nlohmann::json j; j["charId"]=charId; j["remove"]=true; j["ts"]=(uint64)time(nullptr); auto s=j.dump(); redisReply* r=(redisReply*)redisCommand(_redis,"PUBLISH map:0 %s",s.c_str()); if(r) freeReplyObject(r); r=(redisReply*)redisCommand(_redis,"DEL live:char:%d",charId); if(r) freeReplyObject(r); } _queue.erase(std::remove_if(_queue.begin(),_queue.end(),[charId](auto& u){return u.charId==charId;}), _queue.end()); }

void LiveMapMgr::Flush()
{
    std::deque<PendingUpdate> local; { std::lock_guard<std::mutex> g(_lock); if(_queue.empty()) return; local.swap(_queue); }
    std::unordered_map<uint32,std::vector<PendingUpdate>> byMap; for(auto& u: local) byMap[u.mapId].push_back(u);
    std::lock_guard<std::mutex> g(_lock);
    if(_redis && !_redis->err){
        for(auto& [mapId, vec]: byMap){
            nlohmann::json batch=nlohmann::json::array();
            for(auto& u: vec){
                nlohmann::json j;
                j["charId"]=u.charId; j["name"]=u.name; j["mapId"]=u.mapId; j["world_x"]=u.x; j["world_y"]=u.y; j["world_z"]=u.z; j["zone"]=u.zone; j["level"]=u.level; j["class"]=u.playerClass; j["isBot"]=u.isBot; j["team"]=u.team; j["race"]=u.race; j["guild"]=u.guild; j["isDead"]=u.isDead; j["inCombat"]=u.inCombat;
                // NEW fields
                j["behavior"]=u.behaviorState; j["isInFlight"]=u.isInFlight; j["taxiOrigin"]=u.taxiOrigin; j["taxiDest"]=u.taxiDest;
                j["totalPlayed"]=u.totalPlayed; j["levelPlayed"]=u.levelPlayed; j["money"]=u.money; j["healthPct"]=u.healthPct; j["manaPct"]=u.manaPct; j["latency"]=u.latency;
                j["ts"]=u.ts;
                batch.push_back(j);
                std::string single=j.dump();
                redisAppendCommand(_redis,"SETEX live:char:%d %d %s",u.charId,30,single.c_str());
            }
            std::string sadd="SADD live:map:"+std::to_string(mapId); for(auto& u: vec) sadd+=" "+std::to_string(u.charId); redisAppendCommand(_redis,"%s",sadd.c_str()); redisAppendCommand(_redis,"EXPIRE live:map:%d %d",mapId,35); std::string bs=batch.dump(); redisAppendCommand(_redis,"PUBLISH map:%d %s",mapId,bs.c_str());
        }
        int total=0; for(auto& kv: byMap) total+=kv.second.size()+3; redisReply* rep=nullptr; for(int i=0;i<total;++i){ if(redisGetReply(_redis,(void**)&rep)==REDIS_OK && rep) freeReplyObject(rep); }
    }
    if(_useHttp && !local.empty()){
        nlohmann::json batch=nlohmann::json::array();
        for(auto& u: local){ nlohmann::json j; j["charId"]=u.charId; j["name"]=u.name; j["mapId"]=u.mapId; j["world_x"]=u.x; j["world_y"]=u.y; j["world_z"]=u.z; j["zone"]=u.zone; j["level"]=u.level; j["class"]=u.playerClass; j["isBot"]=u.isBot; j["team"]=u.team; j["race"]=u.race; j["guild"]=u.guild; j["isDead"]=u.isDead; j["inCombat"]=u.inCombat; j["behavior"]=u.behaviorState; j["isInFlight"]=u.isInFlight; j["taxiOrigin"]=u.taxiOrigin; j["taxiDest"]=u.taxiDest; j["totalPlayed"]=u.totalPlayed; j["levelPlayed"]=u.levelPlayed; j["money"]=u.money; j["healthPct"]=u.healthPct; j["manaPct"]=u.manaPct; j["latency"]=u.latency; j["ts"]=u.ts; batch.push_back(j); }
        std::string payload=batch.dump(); std::string esc; for(char c: payload){ if(c==''') esc+="'\''"; else esc+=c; } std::string cmd="curl -s -X POST -H 'Content-Type: application/json' -H 'X-LiveMap-Secret: "+_secret+"' -d '"+esc+"' "+_apiUrl+" > /dev/null 2>&1 &"; std::system(cmd.c_str());
    }
}
