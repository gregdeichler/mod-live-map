// Drop this into your Leaflet map component
// Uses class ID from mod-live-map payload

export const CLASS_COLOR = {
  1: '#C79C6E', // Warrior
  2: '#F58CBA', // Paladin
  3: '#ABD473', // Hunter
  4: '#FFF569', // Rogue
  5: '#FFFFFF', // Priest
  6: '#C41F3B', // DK
  7: '#0070DE', // Shaman
  8: '#69CCF0', // Mage
  9: '#9482C9', // Warlock
  11: '#FF7D0A', // Druid
};

export const CLASS_ICON = {
  1: '/icons/class_1_warrior.webp',
  2: '/icons/class_2_paladin.webp',
  3: '/icons/class_3_hunter.webp',
  4: '/icons/class_4_rogue.webp',
  5: '/icons/class_5_priest.webp',
  6: '/icons/class_6_deathknight.webp',
  7: '/icons/class_7_shaman.webp',
  8: '/icons/class_8_mage.webp',
  9: '/icons/class_9_warlock.webp',
  11: '/icons/class_11_druid.webp',
};

export function createLiveMarker(d) {
  // d = {charId, name, mapId, world_x, world_y, px, py, level, class, isBot, team}
  const isBot = d.isBot;
  const color = CLASS_COLOR[d.class] || '#fff';
  const iconUrl = CLASS_ICON[d.class];

  if (isBot) {
    // For 1200 bots: tiny colored dot, no image, high perf
    return L.circleMarker([d.py, d.px], {
      radius: 4,
      fillColor: color,
      color: '#000',
      weight: 0.5,
      opacity: 0.8,
      fillOpacity: 0.5
    }).bindTooltip(`${d.name} [${d.level}]`, {permanent: false});
  } else {
    // Real player: class icon + level badge + pulse
    return L.marker([d.py, d.px], {
      icon: L.divIcon({
        html: `
          <div class="live-player" style="--class-color:${color}">
            <img src="${iconUrl}" class="class-icon" />
            <span class="level-badge">${d.level}</span>
            <span class="name-label">${d.name}</span>
          </div>`,
        className: '',
        iconSize: [36, 36],
        iconAnchor: [18, 18]
      })
    });
  }
}

// CSS to add to your site:
/*
.live-player { position: relative; filter: drop-shadow(0 0 4px var(--class-color)); }
.class-icon { width: 28px; height: 28px; border-radius: 50%; border: 2px solid var(--class-color); }
.level-badge { position: absolute; bottom: -2px; right: -2px; background: #000; color: #fff; font-size: 10px; padding: 1px 3px; border-radius: 4px; }
.name-label { position: absolute; top: -18px; left: 50%; transform: translateX(-50%); white-space: nowrap; background: rgba(0,0,0,0.7); color: #fff; font-size: 11px; padding: 1px 4px; border-radius: 3px; }
.bot-dot { opacity: 0.5; }
*/
