#pragma once
#include <string>
#include <set>
#include <unordered_map>
#include <mutex>

struct RareInfo {
    uint32 entry;
    uint32 guidLow;
    std::string name;
    uint32 mapId;
    float x,y,z;
    uint32 zone;
    bool isDead;
    uint32 respawnTime;
    uint64 ts;
};

class RareTracker {
public:
    static RareTracker* instance();
    void Init();
    void UpdateCreature(uint32 entry, uint32 guidLow, const std::string& name, uint32 mapId, float x,float y,float z, uint32 zone, bool isDead, uint32 respawn);
    void RemoveCreature(uint32 guidLow);
    std::vector<RareInfo> GetRaresForMap(uint32 mapId);
    bool IsRareEntry(uint32 entry) const;
private:
    std::set<uint32> _rareEntries;
    std::unordered_map<uint32, RareInfo> _rares; // guid -> info
    std::mutex _lock;
};

#define sRareTracker RareTracker::instance()
