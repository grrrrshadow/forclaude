# -*- coding: utf-8 -*-
"""The rig's set for the game's configurator (vehicle_config.h), written in yagl.

A road vehicle whose set names two details for the player to choose -- a crew
of three and a cart of two -- through callback 1C0, and draws one picture for
the first choice of both and another for anything else, by variable 5C. The
set asks for the feature 'decouple_vehicle_config' first and gives up with an
error where nobody answers, as the colleague's sets do (special/pack_special.py
in grrrrf), and its pictures are zin8 only, as his are; the game works the
smaller zooms out. Written in the colleague's yagl dialect on purpose, so that
the test is the same recipe his sets use (docs/decouple_vehicle_config.md).

    python3 grf/konfig_yagl.py <output directory>
    yagl -e konfig.grf              (in the output directory; it reads sprites/konfig.yagl)
    cp konfig.grf <rig home>/.openttd/newgrf/
"""
import os
import sys

from PIL import Image

OUT = sys.argv[1]
SPRITES = os.path.join(OUT, "sprites")
os.makedirs(SPRITES, exist_ok=True)

VEH = 0x0058  # the vehicle's id in the set

# The pictures: a 1x1 transparent c8bpp placeholder line, as the colleague's sets
# carry (the game draws from zin8), and two zin8 blocks that tell the sets apart.
pal = Image.new("P", (16, 16), 0)
pal.putpalette([0, 0, 255] + [0, 0, 0] * 255)
pal.save(os.path.join(SPRITES, "konfig-8bpp.png"))
sheet = Image.new("RGBA", (96, 48), (0, 0, 0, 0))
for x, colour in ((8, (200, 40, 40, 255)), (48, (40, 160, 40, 255))):
    for dx in range(32):
        for dy in range(32):
            sheet.putpixel((x + dx, 8 + dy), colour)
sheet.save(os.path.join(SPRITES, "konfig-zin8.png"))

Y = ['yagl_version: "";', "grf_format: Container2;",
     "optional_info // Action14: the feature test, answered on bit 9 of 0x9D", "{", "    FTST: ", "    {",
     '        NAME: default, "decouple_vehicle_config";', "        MINV: [ 0x01 0x00 ];", "        SETP: [ 0x09 ];", "    }", "}",
     "grf // Action08", "{", '    grf_id: "RIGk";', "    version: GRF8;", '    name: "Rig: konfig";',
     '    description: "Details for the configurator: crew and cart";', "}",
     "if_act7 (is_bit_set(global_var[0x9D] & 0xFF, 1 << 9)) // Action07: the game answered, skip the error", "{",
     "    skip_sprites: 0x01;", "}",
     "error_message<Fatal, default, 0xFF> // Action0B", "{",
     '    message: "Tento GRF patri ke hre OpenTTD decouple by Karel Macha a jinde nefunguje.";', "}",
     f"properties<RoadVehicles, 0x{VEH:04X}> // Action00", "{", "    {",
     "        long_introduction_date: date(1920/1/1);", "        model_life_years: 255;", "        vehicle_life_years: 30;",
     "        reliability_decay_speed: 20;", "        climate_availability: Temperate | Arctic | Tropical | Toyland;",
     "        cargo_type: 0x00;", "        cargo_capacity: 0x10;", "        loading_speed: 0x02;", "        sprite_id: 0xFF;",
     "        speed_2_kmh: 0x30;", "        power_10_hp: 0x01;", "        weight_quarter_tons: 0x04;",
     "        cost_factor: 0x08;", "        running_cost_factor: 0x04;", "        running_cost_base: 0x00004C48;",
     "        callback_flags_mask: 0x00;", "    }", "}",
     f"strings<RoadVehicles, default, 0x{VEH:04X}> // Action04: the vehicle's name", "{", f'    /* 0x{VEH:04X} */ "Rig: konfig vuz";', "}",
     "strings<RoadVehicles, default, 0xD000*> // Action04: the configurator's texts, D000..D006", "{"]
TEXTS = ["Posadka", "College Girl", "Female Girl", "Ufon", "Vozik", "tahnout", "tlacit"]
for i, t in enumerate(TEXTS):
    Y.append(f'    /* 0x{0xD000 + i:04X} */ "{t}";')
Y += ["}"]

sid = 1
Y += ["sprite_sets<RoadVehicles, 0x0000> // Action01: set 0 the first choices, set 1 anything else", "{"]
for s, x in enumerate((8, 48)):
    Y += [f"    sprite_set // 0x{s:04X}", "    {"]
    for d in range(8):
        Y += [f"        sprite_id<0x{sid:08X}>", "        {",
              '            [1, 1, 0, 0], normal, c8bpp, "konfig-8bpp.png", [4, 4];',
              f'            [32, 32, -16, -16], zin8, c32bpp | chunked, "konfig-zin8.png", [{x}, 8];',
              "        }"]
        sid += 1
    Y += ["    }"]
Y += ["}"]
for g in range(2):
    Y += [f"sprite_groups<RoadVehicles, 0x{g:02X}> // Action02 basic: set {g}", "{",
          f"    primary_spritesets: [ 0x{g:04X} ];", f"    secondary_spritesets: [ 0x{g:04X} ];", "}"]

def switch(cid, popis, var, ranges, default):
    r = [f"switch<RoadVehicles, 0x{cid:02X}, PrimaryDWord> // {popis}", "{", "    expression:", "    {",
         f"        value1 = variable[0x{var:02X}] & 0x0000FFFF;", "    };", "    ranges:", "    {"]
    for low, high, result in ranges:
        r.append(f"        0x{low:08X}: 0x{result:04X};" if low == high else f"        0x{low:08X}..0x{high:08X}: 0x{result:04X};")
    r += ["    };", f"    default: 0x{default:04X};", "}"]
    return r

# The picture by variable 5C: byte 0 the crew, byte 1 the cart; both first -> set 0.
Y += switch(0x02, "the picture by the player's choices (variable 5C)", 0x5C, [(0, 0, 0x0000)], 0x0001)
# The texts by variable 10: detail << 8 | option, FF the detail's name; 8400 = none.
Y += switch(0x03, "the texts of the configurator (callback 1C0, variable 10)", 0x10,
            [(0x00FF, 0x00FF, 0x8000), (0x0000, 0x0000, 0x8001), (0x0001, 0x0001, 0x8002), (0x0002, 0x0002, 0x8003),
             (0x01FF, 0x01FF, 0x8004), (0x0100, 0x0100, 0x8005), (0x0101, 0x0101, 0x8006)], 0x8400)
# The callbacks by variable 0C: 1C0 to the texts, anything else to the picture.
Y += switch(0x04, "the callbacks", 0x0C, [(0x01C0, 0x01C0, 0x0003)], 0x0002)
Y += ["feature_graphics<RoadVehicles> // Action03", "{", "    livery_override: false;", "    default_set_id: 0x0004;",
      f"    feature_ids: [ 0x{VEH:04X} ];", "    cargo_types:", "    {", "        0xFF: 0x0004;", "    };", "}"]

open(os.path.join(SPRITES, "konfig.yagl"), "w").write("\n".join(Y) + "\n")
print("konfig.yagl:", len(Y), "lines,", sid - 1, "sprites")
