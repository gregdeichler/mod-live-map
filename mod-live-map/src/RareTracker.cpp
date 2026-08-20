#include "RareTracker.h"
#include "Config.h"
#include "Log.h"
#include <nlohmann/json.hpp>
#include <hiredis/hiredis.h>
#include "LiveMapMgr.h"

extern redisContext* GetLiveMapRedis(); // we will share

RareTracker* RareTracker::instance(){ static RareTracker i; return &i; }

void RareTracker::Init(){
    _rareEntries.clear();
    std::string rareList = sConfigMgr->GetOption<std::string>("LiveMap.RareEntries", "");
    // If empty, auto-detect: all creatures with rank 1/2/3 and elite flag via DB? For now use list
    // Example rares Eastern Kingdoms: 2740, 2741, etc
    if(!rareList.empty()){
        std::stringstream ss(rareList); std::string t;
        while(std::getline(ss,t,',')){ try{ t.erase(0,t.find_first_not_of(" \t")); t.erase(t.find_last_not_of(" \t")+1); if(!t.empty()) _rareEntries.insert((uint32)std::stoul(t)); }catch(...){} }
    }
    // If track all rares enabled
    if(sConfigMgr->GetOption<bool>("LiveMap.TrackAllRares", true)){
        // _rareEntries will be ignored, IsRareEntry will check rank at runtime
    }
    LOG_INFO("module","[LiveMap-Rare] Tracking {} explicit rares, TrackAll={}", _rareEntries.size(), sConfigMgr->GetOption<bool>("LiveMap.TrackAllRares", true));
}

bool RareTracker::IsRareEntry(uint32 entry) const {
    if(sConfigMgr->GetOption<bool>("LiveMap.TrackAllRares", true)) return true; // let caller filter by rank
    return _rareEntries.count(entry)>0;
}

void RareTracker::UpdateCreature(uint32 entry, uint32 guidLow, const std::string& name, uint32 mapId, float x,float y,float z, uint32 zone, bool isDead, uint32 respawn){
    if(!sLiveMap->IsMapTracked(mapId)) return;
    RareInfo info{entry,guidLow,name,mapId,x,y,z,zone,isDead,respawn,(uint64)time(nullptr)};
    {
        std::lock_guard<std::mutex> g(_lock);
        _rares[guidLow]=info;
    }
    // Publish to redis channel rare:mapId
    extern redisContext* g_redis;
    if(g_redis && !g_redis->err){
        nlohmann::json j;
        j["type"]="rare"; j["entry"]=entry; j["guid"]=guidLow; j["name"]=name; j["mapId"]=mapId;
        j["world_x"]=x; j["world_y"]=y; j["world_z"]=z; j["zone"]=zone; j["isDead"]=isDead; j["respawn"]=respawn; j["ts"]=info.ts;
        std::string s=j.dump();
        redisReply* r=(redisReply*)redisCommand(g_redis,"PUBLISH rare:%d %s", mapId, s.c_str());
        if(r) freeReplyObject(r);
        r=(redisReply*)redisCommand(g_redis,"SETEX rare:char:%d %d %s", guidLow, 120, s.c_str());
        if(r) freeReplyObject(r);
    }
}

void RareTracker::RemoveCreature(uint32 guidLow){
    std::lock_guard<std::mutex> g(_lock);
    _rares.erase(guidLow);
}

std::vector<RareInfo> RareTracker::GetRaresForMap(uint32 mapId){
    std::lock_guard<std::mutex> g(_lock);
    std::vector<RareInfo> out;
    for(auto& kv: _rares) if(kv.second.mapId==mapId) out.push_back(kv.second);
    return out;
}
