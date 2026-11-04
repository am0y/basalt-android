"""Build the pinned Lucide atlas and transparent Basalt brand artwork."""
import argparse
import urllib.request
import xml.etree.ElementTree as ET
from pathlib import Path

import cairosvg
from PIL import Image, ImageChops

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "scripts/assets"
VERSION = "0.468.0"
ICONS = ["house", "square-terminal", "gamepad-2", "cloud", "settings", "folder",
         "file-code-2", "x", "play", "eraser", "clipboard", "copy", "battery",
         "zap", "clock", "hourglass", "arrow-down", "plus", "check", "trash-2"]
HUB_ICONS = ["search", "compass", "flame", "rotate-cw", "arrow-right", "arrow-left",
             "badge-check", "key-round", "lock-keyhole-open", "user-round", "circle-alert", "loader-circle"]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--logo", type=Path)
    args = parser.parse_args()
    vendor = ASSETS / "lucide"
    vendor.mkdir(exist_ok=True)
    for name in ["LICENSE", *[f"icons/{icon}.svg" for icon in ICONS + HUB_ICONS]]:
        destination = vendor / name.rsplit("/", 1)[-1]
        if not destination.exists():
            with urllib.request.urlopen(f"https://unpkg.com/lucide-static@{VERSION}/{name}", timeout=45) as response:
                destination.write_bytes(response.read())
    brands = ASSETS / "simple-icons"
    brands.mkdir(exist_ok=True)
    for name in ["icons/discord.svg", "LICENSE.md"]:
        destination = brands / name.rsplit("/", 1)[-1]
        if not destination.exists():
            with urllib.request.urlopen(f"https://raw.githubusercontent.com/simple-icons/simple-icons/14.0.0/{name}", timeout=45) as response:
                destination.write_bytes(response.read())
    rows = (len(ICONS) + 1 + len(HUB_ICONS) + 4) // 5
    svg = ET.Element("svg", xmlns="http://www.w3.org/2000/svg", width="320", height=str(rows * 64), viewBox=f"0 0 120 {rows * 24}")
    for index, name in [*enumerate(ICONS), *enumerate(HUB_ICONS, start=21)]:
        tile = ET.fromstring((vendor / f"{name}.svg").read_text())
        tile.set("x", str(index % 5 * 24))
        tile.set("y", str(index // 5 * 24))
        tile.set("stroke", "white")
        svg.append(tile)
    discord = ET.fromstring((brands / "discord.svg").read_text())
    discord.attrib.update(x="0", y="96", width="24", height="24", fill="white")
    svg.append(discord)
    cairosvg.svg2png(bytestring=ET.tostring(svg), write_to=str(ASSETS / "glass-icons.png"))
    if args.logo:
        original = Image.open(args.logo).convert("RGBA")
        # The supplied white mark has a black matte; preserve its antialiased contour.
        alpha = ImageChops.multiply(original.convert("L"), original.getchannel("A"))
        mark = Image.new("RGBA", original.size, "white")
        mark.putalpha(alpha)
        mark = mark.crop(alpha.getbbox())
        mark.thumbnail((192, 192), Image.Resampling.LANCZOS)
        canvas = Image.new("RGBA", (256, 256))
        canvas.alpha_composite(mark, ((256 - mark.width) // 2, (256 - mark.height) // 2))
        canvas.save(ASSETS / "basalt-logo.png")
    print(f"Lucide {VERSION}: {len(ICONS) + len(HUB_ICONS)} icons; assets in {ASSETS}")


if __name__ == "__main__":
    main()
