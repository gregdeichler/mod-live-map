"""
Export WoW class icons + faction icons using wowlib-py
Run: python export_icons.py --client "D:/WoW 3.3.5a" --out "../../data/icons"
"""
import argparse
from pathlib import Path
from wowlib.filesystem import CASCClient, MPQClient
from wowlib.formats import blp

CLASS_MAP = {
    "Warrior": 1, "Paladin": 2, "Hunter": 3, "Rogue": 4, "Priest": 5,
    "DeathKnight": 6, "Shaman": 7, "Mage": 8, "Warlock": 9, "Druid": 11
}

ICON_SOURCES = [
    # WotLK path
    "Interface/Icons/ClassIcon_{name}.blp",
    # Fallback sheet
    "Interface/Glues/CharacterCreate/UI-CharacterCreate-Classes.blp",
]

def try_read(fs, path):
    try:
        return fs.read(path)
    except:
        return None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--client", required=True, help="Path to WoW client")
    ap.add_argument("--out", default="./data/icons", help="Output dir")
    args = ap.parse_args()

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    client = Path(args.client)
    if (client / "Data" / "data.000").exists() or (client / "Data" / "common.MPQ").exists():
        fs = MPQClient(str(client), patch_chain=True)
    else:
        fs = CASCClient(str(client))

    # Export class icons
    for name, classId in CLASS_MAP.items():
        blp_path = f"Interface/Icons/ClassIcon_{name}.blp"
        data = try_read(fs, blp_path)
        if not data:
            # Try lowercase
            data = try_read(fs, f"Interface/Icons/ClassIcon_{name.lower()}.blp")
        if data:
            img = blp.BLP.read(data).to_pillow()
            img = img.resize((64,64))
            img.save(out / f"class_{classId}_{name.lower()}.webp", "WEBP", quality=90)
            img.save(out / f"class_{classId}_{name.lower()}.png")
            print(f"Exported {name} -> class_{classId}")
        else:
            print(f"Missing {blp_path}")

    # Also export faction
    for faction_path in ["Interface/Icons/INV_Banner_01.blp", "Interface/PVPFrame/PVP-Currency-Alliance.blp", "Interface/PVPFrame/PVP-Currency-Horde.blp"]:
        data = try_read(fs, faction_path)
        if data:
            img = blp.BLP.read(data).to_pillow()
            img.save(out / f"{Path(faction_path).stem}.webp")

    print(f"Done -> {out}")

if __name__ == "__main__":
    main()
