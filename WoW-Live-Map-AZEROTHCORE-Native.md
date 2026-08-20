# WoW Live Map — AzerothCore Native (No Addon) — 100% Local
## Mapgenie.io Clone + Live Map from AzerothCore Directly
### wowlib for tiles + AzerothCore hook for live positions

**Goal:** Self-hosted Mapgenie clone of Eastern Kingdoms + live character dots pulled straight from your AzerothCore worldserver. No addon, no companion app, no client mod.

**Stack:** wowlib (tile bake) + AzerothCore module + FastAPI + Redis + Leaflet, all local docker-compose.

---

### 1. Architecture — Addon-Free

```
[D:\WoW 3.3.5a Client Folder]  (for tile baking only, not live)
        |
        v
[ extractor ] python + wowlib-py -> ./data/tiles/azeroth/{z}/{x}/{y}.webp + poi.json

[AzerothCore Worldserver]
  acore_worldserver + mod-live-map
        |
        |-- OnPlayerUpdatePosition / OnPlayerMove -> 
        |-- redis.publish("map:0", {charId, name, mapId, world_x, world_y, zone, level, class})
        |   OR direct HTTP POST to api (no redis needed for <100 players)
        |
        v
[ redis:6379 ] (local container, ephemeral)
        |
        v
[ api:8000 ] FastAPI
  - /api/positions (GET current snapshot for page load)
  - /ws/map/{mapId} (broadcast live)
  - converts world_x/y -> pixel px/py for Leaflet
        |
        v
[ nginx:80 ] serves
  / -> web (Vite + Leaflet)
  /tiles/azeroth/... -> static tiles from ./data/tiles
  /api/ /ws/ -> proxy to api
```

No client touches WoW client files at runtime. Live data comes from server memory.

### 2. AzerothCore Module — The Core of Live (No Addon)

You have 3 options, from cleanest to quickest:

#### Option A: C++ Module (Recommended — Production)

Create a new AzerothCore module: `modules/mod-live-map/`

**File structure:**
```
modules/mod-live-map/
  CMakeLists.txt
  src/
    mod_live_map.cpp
    LiveMapMgr.h
    LiveMapMgr.cpp
    HttpClient.h (simple curl POST)
  conf/
    mod_live_map.conf.dist
```

**CMakeLists.txt:**
```cmake
CU_ADD_MODULE(mod-live-map)
```

**mod_live_map.conf.dist:**
```
LiveMap.Enable = 1
LiveMap.ApiUrl = http://api:8000/api/position
LiveMap.RedisHost = redis
LiveMap.RedisPort = 6379
LiveMap.UseRedis = 1
LiveMap.UseHttp = 0
LiveMap.UpdateIntervalMs = 1000
LiveMap.MapFilter = 0,1,530,571 # only track these maps, 0 = Eastern Kingdoms
LiveMap.GMInvisible = 0 # include GM invisible?
```

**LiveMapMgr.h:**
```cpp
#pragma once
#include <string>
#include <mutex>
#include <hiredis/hiredis.h>

class LiveMapMgr {
public:
  static LiveMapMgr* instance();
  void Init();
  void UpdatePlayer(uint32 charId, std::string name, uint32 mapId, float x, float y, float z, uint32 zone, uint8 level, uint8 cls);
  void Shutdown();
private:
  redisContext* _redis = nullptr;
  std::mutex _lock;
  bool _useRedis;
  std::string _apiUrl;
  uint32 _intervalMs;
  std::set<uint32> _mapFilter;
  std::unordered_map<uint32, uint32> _lastUpdate; // charId -> timestamp ms
};
#define sLiveMap LiveMapMgr::instance()
```

**mod_live_map.cpp — Hook:**
```cpp
#include "ScriptMgr.h"
#include "Player.h"
#include "Map.h"
#include "LiveMapMgr.h"
#include "Config.h"

class LiveMap_PlayerScript : public PlayerScript {
public:
  LiveMap_PlayerScript() : PlayerScript("LiveMap_PlayerScript") {}

  void OnPlayerUpdate(Player* player, uint32 diff) override {
    if (!sConfigMgr->GetOption<bool>("LiveMap.Enable", true))
      return;

    // throttle: 1000ms per player
    uint32 now = getMSTime();
    uint32 guid = player->GetGUID().GetCounter();
    static std::unordered_map<uint32, uint32> last;
    if (last[guid] && now - last[guid] < sConfigMgr->GetOption<uint32>("LiveMap.UpdateIntervalMs", 1000))
      return;
    last[guid] = now;

    uint32 mapId = player->GetMapId();
    // filter maps if configured
    if (!sLiveMap->IsMapTracked(mapId))
      return;

    if (!player->IsInWorld() || player->IsBeingTeleported())
      return;

    // Optional: skip GM in .gm on if configured
    if (player->IsGameMaster() && !sConfigMgr->GetOption<bool>("LiveMap.GMInvisible", false))
      if (player->isGMVisible() == false)
        return;

    float x = player->GetPositionX();
    float y = player->GetPositionY();
    float z = player->GetPositionZ();
    uint32 zone = player->GetZoneId();
    sLiveMap->UpdatePlayer(guid, player->GetName(), mapId, x, y, z, zone, player->getLevel(), player->getClass());
  }

  void OnPlayerLogout(Player* player) override {
    sLiveMap->RemovePlayer(player->GetGUID().GetCounter());
  }
};

class LiveMap_WorldScript : public WorldScript {
public:
  LiveMap_WorldScript() : WorldScript("LiveMap_WorldScript") {}
  void OnStartup() override { sLiveMap->Init(); }
  void OnShutdown() override { sLiveMap->Shutdown(); }
};

void Addmod_live_mapScripts() {
  new LiveMap_PlayerScript();
  new LiveMap_WorldScript();
}
```

**LiveMapMgr.cpp — Redis Publish (no addon needed):**
```cpp
#include "LiveMapMgr.h"
#include "Config.h"
#include <nlohmann/json.hpp>

void LiveMapMgr::Init() {
  _useRedis = sConfigMgr->GetOption<bool>("LiveMap.UseRedis", true);
  _apiUrl = sConfigMgr->GetOption<std::string>("LiveMap.ApiUrl", "http://api:8000/api/position");
  if (_useRedis) {
    std::string host = sConfigMgr->GetOption<std::string>("LiveMap.RedisHost", "127.0.0.1");
    int port = sConfigMgr->GetOption<int>("LiveMap.RedisPort", 6379);
    _redis = redisConnect(host.c_str(), port);
    if (_redis && _redis->err) {
      LOG_ERROR("module", "LiveMap Redis connect failed: {}", _redis->errstr);
      _redis = nullptr;
    }
  }
}

void LiveMapMgr::UpdatePlayer(uint32 charId, std::string name, uint32 mapId, float x, float y, float z, uint32 zone, uint8 level, uint8 cls) {
  nlohmann::json j = {
    {"charId", charId},
    {"name", name},
    {"mapId", mapId},
    {"world_x", x},
    {"world_y", y},
    {"world_z", z},
    {"zone", zone},
    {"level", level},
    {"class", cls},
    {"ts", (uint64)time(nullptr)}
  };
  std::string payload = j.dump();

  std::lock_guard<std::mutex> guard(_lock);
  if (_redis) {
    // publish to map channel + setex for snapshot
    redisCommand(_redis, "PUBLISH map:%d %s", mapId, payload.c_str());
    redisCommand(_redis, "SETEX live:char:%d 30 %s", charId, payload.c_str());
    redisCommand(_redis, "SADD live:map:%d %d", mapId, charId);
    redisCommand(_redis, "EXPIRE live:map:%d 35", mapId);
  }
}

void LiveMapMgr::RemovePlayer(uint32 charId) {
  if (_redis) {
    redisCommand(_redis, "DEL live:char:%d", charId);
    // publish removal
    nlohmann::json j = {{"charId", charId}, {"remove", true}, {"ts", (uint64)time(nullptr)}};
    redisCommand(_redis, "PUBLISH map:0 %s", j.dump().c_str());
  }
}
```

Build: put in `modules/`, re-run cmake, build worldserver. No DB changes.

**Result:** Every moving player automatically appears on redis channel `map:0` etc. No client mod.

#### Option B: Eluna Lua (No C++ Compile, 5 Minute Test)

If you already have mod-eluna enabled in AzerothCore:

Create `lua_scripts/LiveMap.lua`:

```lua
-- Requires: lua-socket + hiredis lua lib or http
-- For local testing without redis, use simple TCP to api

local UPDATE_MS = 1000
local lastUpdate = {}

local function isTrackedMap(mapId)
  -- 0 = Eastern Kingdoms, 1 = Kalimdor, 530 = Outland, 571 = Northrend
  return mapId == 0 -- only track EK for Mapgenie clone
end

CreateLuaEvent(function()
  local players = GetPlayersInWorld() -- or GetPlayersInMap(0)
  for _, p in ipairs(players) do
    if p:IsInWorld() and isTrackedMap(p:GetMapId()) then
      local guid = p:GetGUIDLow()
      local now = os.time() * 1000
      if not lastUpdate[guid] or now - lastUpdate[guid] >= UPDATE_MS then
        lastUpdate[guid] = now
        local x,y,z,o = p:GetLocation()
        local mapId = p:GetMapId()
        local zone = p:GetZoneId()
        local name = p:GetName()
        -- Send to api via external http - use luasocket
        -- If luasocket not available, write to file that api watches
        -- Simplest for local: use io.popen curl
        local json = string.format('{"charId":%d,"name":"%s","mapId":%d,"world_x":%f,"world_y":%f,"zone":%d,"ts":%d}',
          guid, name, mapId, x, y, zone, os.time())
        os.execute(string.format("curl -s -X POST -H 'Content-Type: application/json' -d '%s' http://api:8000/api/position > /dev/null &", json))
      end
    end
  end
end, UPDATE_MS, 0)

print("[LiveMap] Eluna live map running, tracking map 0")
```

Enable: place in `lua_scripts/`, restart worldserver, `reload eluna`. No module compile needed.

Cons: curl per player per second is heavier, but fine for <100 players local.

#### Option C: Database Polling (No Module At All, Easiest)

If you don't want to compile anything:

AzerothCore stores player position in `characters` table? Actually live position is not persisted constantly, only on save. But you can enable `CharacterDB` update on move via config.

Simpler: external python poller queries worldserver via SOAP or directly reads `characters` if you set `PlayerSave.Interval = 1000`.

But best is: python script that connects to worldserver via SOAP `server info` or via direct `SELECT` from `characters`? Not real-time.

**So use Option A or B.** Option A is proper.

### 3. API — AzerothCore Native Version

No token needed, server is trusted. But keep simple auth via shared secret between worldserver and api.

**api/main.py:**
```python
from fastapi import FastAPI, WebSocket, WebSocketDisconnect, Request
import redis.asyncio as redis
import json, time, os

app = FastAPI()
r = redis.from_url(os.getenv("REDIS_URL", "redis://redis:6379"), decode_responses=True)

# In-memory fallback if no redis
live_state = {}

WORLD_MIN, WORLD_MAX = -17000, 17000
TILE_PX = 4096

def wow_to_pixel(wx, wy):
    px = (wy - WORLD_MIN) / (WORLD_MAX - WORLD_MIN) * TILE_PX
    py = (WORLD_MAX - wx) / (WORLD_MAX - WORLD_MIN) * TILE_PX
    return px, py

@app.post("/api/position")
async def ingest(req: Request):
    data = await req.json()
    # optional shared secret check
    # if req.headers.get("X-LiveMap-Secret") != os.getenv("SECRET"): raise 403

    wx, wy = data['world_x'], data['world_y']
    px, py = wow_to_pixel(wx, wy)
    data['px'] = px
    data['py'] = py
    data['ts'] = time.time()

    charId = data['charId']
    mapId = data['mapId']

    # store snapshot 30s TTL
    await r.setex(f"live:char:{charId}", 30, json.dumps(data))
    await r.sadd(f"live:map:{mapId}", charId)
    await r.expire(f"live:map:{mapId}", 35)
    await r.publish(f"map:{mapId}", json.dumps(data))

    # also in-memory for GET
    live_state[charId] = data
    return {"ok": True}

@app.get("/api/positions/{map_id}")
async def get_positions(map_id: int):
    # return all live chars for map
    keys = await r.smembers(f"live:map:{map_id}")
    if not keys:
        # fallback to scan live:char:*
        vals = await r.mget([f"live:char:{k}" for k in live_state.keys()])
        return [json.loads(v) for v in vals if v]
    vals = await r.mget([f"live:char:{k}" for k in keys])
    return [json.loads(v) for v in vals if v]

@app.websocket("/ws/map/{map_id}")
async def ws_map(ws: WebSocket, map_id: int):
    await ws.accept()
    # send current snapshot first
    snap = await get_positions(map_id)
    await ws.send_text(json.dumps({"type": "snapshot", "data": snap}))

    pubsub = r.pubsub()
    await pubsub.subscribe(f"map:{map_id}")
    try:
        async for msg in pubsub.listen():
            if msg['type'] == 'message':
                await ws.send_text(msg['data'])
    except WebSocketDisconnect:
        pass
    finally:
        await pubsub.unsubscribe(f"map:{map_id}")
```

### 4. Frontend — No Change Except No Auth

Leaflet page `http://localhost`:

```js
// initial load
const res = await fetch('/api/positions/0');
const snapshot = await res.json();
snapshot.forEach(addOrUpdateMarker);

const ws = new WebSocket(`ws://${location.hostname}:8000/ws/map/0`);
ws.onmessage = e => {
  const data = JSON.parse(e.data);
  if (data.type === 'snapshot') {
    data.data.forEach(addOrUpdateMarker);
  } else {
    if (data.remove) removeMarker(data.charId);
    else addOrUpdateMarker(data);
  }
};

function addOrUpdateMarker(d) {
  // d: charId, name, level, class, px, py, world_x, world_y, zone, ts
  // Convert px/py to Leaflet latlng: L.CRS.Simple uses [y, x]
  const latlng = [d.py, d.px];
  // ...
}
```

Add class icons: use local icons from `Interface/Glues/...` extracted via wowlib BLP -> webp.

### 5. Tile Baking — Same As Before (Local Only)

No change, but you only need map 0:

```bash
docker compose --profile tools run extractor python bake_tiles.py --client /wow --map Azeroth --zoom 0-7 --out /data/tiles/azeroth
```

`bake_tiles.py` uses wowlib as before. No API keys.

### 6. Docker Compose — Updated for AzerothCore

```yaml
services:
  redis:
    image: redis:7-alpine
    ports: ["6379:6379"]

  api:
    build: ./api
    ports: ["8000:8000"]
    environment:
      REDIS_URL: redis://redis:6379
      SECRET: local-secret-123
    depends_on: [redis]
    volumes: ["./data:/data"]

  nginx:
    image: nginx:alpine
    ports: ["80:80"]
    volumes:
      - ./nginx.conf:/etc/nginx/nginx.conf:ro
      - ./data/tiles:/usr/share/nginx/html/tiles:ro
      - ./web/dist:/usr/share/nginx/html:ro

  # Your existing AzerothCore services - add mod-live-map to worldserver build
  # ac-worldserver:
  #   build: ./azerothcore
  #   environment:
  #     LiveMap.RedisHost: redis
  #     LiveMap.ApiUrl: http://api:8000/api/position
```

Your AzerothCore `worldserver.conf` should have `modules/mod-live-map.conf` mounted.

If you run AzerothCore natively (not docker), just set in `mod_live_map.conf`:
```
LiveMap.RedisHost = 127.0.0.1
LiveMap.RedisPort = 6379
```

And make sure redis is reachable.

### 7. Testing Locally

1. Build module: copy `mod-live-map` to `AzerothCore/modules/`, re-run cmake, build worldserver
2. Start redis: `docker compose up redis -d` or `redis-server`
3. Start api: `cd api && uvicorn main:app --host 0.0.0.0 --port 8000 --reload`
4. Start nginx + web: `docker compose up nginx`
5. Login with 2 WoW clients to your local AzerothCore, move around Elwynn Forest
6. Open `http://localhost` — you should see 2 dots moving at 1Hz, no addon
7. Bake tiles once: `python extractor/bake_tiles.py`

### 8. Performance Notes (Local)

- 1Hz per player is fine. 100 players = 100 redis PUBLISH/sec = trivial
- Memory: live:char:* 30s TTL, so only online players stored
- If you don't want redis, change module to POST HTTP directly to api (set UseRedis=0, UseHttp=1) and api keeps in-memory dict. One less container.
- No Warden, no ToS issue — you're reading server memory you own.

### 9. Next Steps / Extensions

- Dungeon floors: WMO groups for Deadmines — parse WMO root + groups via wowlib, render interior map tiles
- Instance maps: mapId 36 = Deadmines, same pipeline
- GM view: add flag to show stealthed / GM, or hide
- Trail: store last 100 positions per char in redis list for breadcrumb trail
- Fog of war per character: track explored ADT tiles from movement history

This version is 100% server-authoritative, zero client mod, zero companion app, zero cloud.
