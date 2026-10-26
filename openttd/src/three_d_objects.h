/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file three_d_objects.h Buildings as real 3D objects (glTF binary, .glb), drawn by the game itself in the turned
 * view and the cab view.
 *
 * The player (13. 10.): "the next houses will be real 3D objects, not eight renders, to save megabytes"; the
 * colleague made every temperate house of the original game so (grrrrf hra/3d/objekty/, the format in
 * hra/3d/OBJEKTY-3D.md): one .glb a building, triangles with one texture, metres, the origin at the north corner of
 * its north tile, and an index objekty.json saying which house and which of its colour variants each one is. They
 * are read from baseset/decouple/objekty/<set>/ the first time they are wanted. The view facing north keeps the
 * sprite: an object is drawn only where the view is 3D.
 *
 * Vehicles too (14. 10.), the player: "we will never render anything again"; the colleague's Sergej first
 * (hra/3d/PROSBA-VOZIDLA-3D.md): entries "druh": "vozidlo" of an index, an engine of a set as one .glb. A vehicle
 * has no sprites of its own then: the game draws its sprites from the object (three_d_vehicle_object.cpp).
 */

#ifndef THREE_D_OBJECTS_H
#define THREE_D_OBJECTS_H

#include "house_type.h"
#include "slope_type.h"
#include "industry_type.h"
#include "engine_type.h"
#include "tile_type.h"
#include "vehicle_type.h"
#include "zoom_type.h"
#include "sound_type.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

/** World units along x and y to a metre of the objects: a tile, 16 units, is 14.83765 m (the renders' scale). */
static constexpr double OBJECT_UNITS_PER_METRE = 16.0 / 14.83765;
/**
 * The creatures on the map -- the witches, the dragons, the ghosts, the animals and the dogs -- not in the view from the
 * eyes: three times as big as they were drawn (the player, 23. 10.: "the witches at night 3x bigger, at the other
 * angles too, and not only the witches, the dragons too, everything we enlarge when it is not the first person view").
 */
static constexpr float THREE_D_MAP_CREATURE_SCALE = 3.0f;
/** The people in the view from the eyes (the girls, Mat, the sellers): a head taller than true (the player, 23. 10.: "I want to look them in the eyes"). */
static constexpr float THREE_D_EYE_PEOPLE_SCALE = 1.15f;
/** Units of height to a metre: a height level, 8 units, is 3 m (hra/3d/OBJEKTY-3D.md). */
static constexpr double OBJECT_HEIGHT_PER_METRE = 8.0 / 3.0;

/** The glow of a lone light of an object ("svetlo_NN", a lit window's), its radius, world units. */
static constexpr double THREE_D_GLOW_UNITS = 1.2;
float ThreeDGlowShare(const std::vector<float> &lights, size_t i);
/**
 * How far the walker of the view from the first person keeps from a wall, metres: half a body across and a little
 * (first_person_gui.cpp; ThreeDObject::Blocks() shuts a door narrower than that). It was 1.4 m, a walker three metres
 * wide: it did not get through an alley between two houses (the player, 17. 10.).
 */
static constexpr double THREE_D_WALKER_RADIUS_M = 0.4;
/** A body's half-width: the walker indoors and up on stairs, and how near a wall it can be reached from outside. */
static constexpr double THREE_D_WALKER_BODY_M = 0.2;

/**
 * A number of its own for each shape and texture made: what the graphics card keeps of it is found by it
 * (three_d_gpu.cpp) -- an address may be another thing's once the first is gone. A copy is another thing, with a number
 * of its own; a thing moved keeps its number, and the one it was moved from takes a new one.
 */
struct ThreeDUniqueId {
	uint64_t value = Next();
	ThreeDUniqueId() = default;
	ThreeDUniqueId(const ThreeDUniqueId &) : value(Next()) {}
	ThreeDUniqueId(ThreeDUniqueId &&other) noexcept : value(other.value) { other.value = Next(); }
	ThreeDUniqueId &operator=(const ThreeDUniqueId &)
	{
		this->value = Next();
		return *this;
	}
	ThreeDUniqueId &operator=(ThreeDUniqueId &&other) noexcept
	{
		this->value = other.value;
		other.value = Next();
		return *this;
	}
	static uint64_t Next();
};

/** A texture of an object at one level of detail: 32bpp pixels in the blitter's colours, 0 where it is transparent. */
struct ThreeDObjectTexture {
	int w = 0;
	int h = 0;
	std::vector<uint32_t> px;
	ThreeDUniqueId uid; ///< for the graphics card (three_d_gpu.cpp)
};

/**
 * The bytes of a texture's picture as they are in its file (a PNG), one copy for every material, shape and object that
 * has the same picture (InternThreeDTextureBytes()): an add-on of the colleague's lorries is one picture of sixteen
 * million pixels for tens of materials, in every record of the package.
 */
using ThreeDTextureBytes = std::shared_ptr<const std::vector<uint8_t>>;
/**
 * A texture halved for each level of detail, made once for all that have the same picture (ThreeDTextureLevelsOf()):
 * the levels finer than any drawn are without their pixels (their sizes kept), made when one is drawn.
 */
using ThreeDTextureLevels = std::shared_ptr<const std::vector<ThreeDObjectTexture>>;
ThreeDTextureBytes InternThreeDTextureBytes(std::span<const uint8_t> bytes);

/**
 * A part of a building painted a colour by number, each house on the map its own ("barvy", the colleague's
 * hra/3d/PROSBA-DOMY-Z-BUDOVY3D.md): the roof, the plaster, the fence.
 */
struct ThreeDObjectColourPart {
	std::string name;               ///< "cast"
	std::vector<uint8_t> mask_png;  ///< "maska": a grey PNG of the texture's layout, 255 where it is the part
	int start = 255;                ///< "zacatek": the texture's lightness the colour is at its own
	std::vector<uint32_t> palette;  ///< "paleta": the colours it may be (Colour::data), the first as it is built
	mutable std::vector<ThreeDObjectTexture> levels; ///< the mask, halved for each level of detail (its grey in the red)
};

/** A figure standing on a building ("postavy" of a house): College Girl smoking by the parasols of the hotel's roof. */
struct ThreeDObjectFigure {
	std::string figure;   ///< "postava": a figure of the indices
	float at[3] = {};     ///< "poloha": where its feet stand, metres in the building's frame (from its north corner, z up)
	float facing = 0;     ///< "otoceni": degrees in the world, 0 along +x, 90 along +y
	float scale = 1;      ///< "meritko": its size against true metres; the building is in true metres
	float stretch = 1;    ///< its height against its width (a plant's "meritko" [x, y, z]: z / x)
};

/** A door of a building ("dvere"): people come out of it and go in (three_d_walkers.h). */
struct ThreeDObjectDoor {
	float at[2] = {};     ///< "poloha": where it is, metres in the building's frame
	float out[2] = {};    ///< "smer": the way out of it
	float width = 1;      ///< "sirka": metres
};

/** A place on a building's flat roof ("strechy", the colleague's desert houses): a monkey sits there (three_d_animals.h). */
struct ThreeDObjectRoof {
	float at[3] = {};     ///< "poloha": metres in the building's frame, z up
	float out[2] = {};    ///< "smer": the way it looks, out to the street
};

/**
 * The shape of a building simplified for a level of detail (ThreeDObject::GetShape()), as its own: its corners
 * closer than the level's cell merged into one, the triangles left without area gone.
 */
struct ThreeDObjectShape {
	std::vector<float> pos;
	std::vector<float> uv;
	std::vector<uint16_t> light;
	std::vector<uint32_t> tri;
	std::vector<int8_t> facing;
	std::vector<uint8_t> tri_tex;    ///< as ThreeDObject's
	std::vector<uint8_t> corner_tex; ///< as ThreeDObject's
	ThreeDUniqueId uid;              ///< for the graphics card (three_d_gpu.cpp)
};

/** A shape of a building to draw: its own (level 0) or simplified (ThreeDObject::GetShape()). */
struct ThreeDObjectShapeView {
	std::span<const float> pos;
	std::span<const float> uv;
	std::span<const uint16_t> light;
	std::span<const uint32_t> tri;
	std::span<const int8_t> facing;
	std::span<const uint8_t> tri_tex;    ///< each triangle's texture (ThreeDObject::GetTextureOf()); empty all the building's own
	std::span<const uint8_t> corner_tex; ///< each corner's texture, the texture its u, v are of; empty all the building's own
	uint64_t uid = 0;                    ///< the shape's number, for the graphics card (ThreeDUniqueId); 0 none

	/** The texture of a triangle (its first index) and of a corner: 0 the building's own, n its add-on n's (ThreeDObject::GetTextureOf()). */
	uint8_t TriangleTexture(size_t t) const { return t / 3 < this->tri_tex.size() ? this->tri_tex[t / 3] : 0; }
	uint8_t CornerTexture(size_t i) const { return i < this->corner_tex.size() ? this->corner_tex[i] : 0; }
};

/** The levels of the shapes: 0 the building's own, then its corners merged in cells of 5 cm, 10 cm, ... 3.2 m. */
static constexpr int THREE_D_SHAPE_LEVELS = 8;
int ThreeDShapeLevel(double pixels_per_metre, double shift);

/** A state kept as an atomic and copied as its value (ThreeDObject::glb_state: the objects are copied as they are read). */
struct ThreeDCopyableState {
	std::atomic<uint8_t> value{0};
	ThreeDCopyableState() = default;
	ThreeDCopyableState(const ThreeDCopyableState &other) : value(other.value.load()) {}
	ThreeDCopyableState &operator=(const ThreeDCopyableState &other)
	{
		this->value.store(other.value.load());
		return *this;
	}
};

/** A building as a 3D object. */
struct ThreeDObject {
	std::string name;         ///< its name in the index
	std::string file;         ///< the .glb it came from
	/* A house's shape and texture are read the first time it is drawn (Read()), not all the houses at the start of the
	 * game (the player, 17. 10.: 170 files, 178 MB): till then pos, uv, tri, facing, light, png, the night and top
	 * are empty. */
	std::string glb_path;     ///< its .glb to read so; empty: read with the index (an industry)
	std::string snow_path;    ///< the snowy texture over it (a house above the snow line), read with it
	mutable ThreeDCopyableState glb_state; ///< 0 not read yet, 1 read, 2 cannot be read
	HouseID house = UINT16_MAX; ///< the house of the original game it is (its north tile's); UINT16_MAX for a set's house
	uint32_t grfid = 0;       ///< a house of a set (NewGRF): the set's id as the game keeps it, 0 for an original house
	uint16_t grf_house = 0;   ///< and the house's number in the set (its Action 0 id, the north tile's)
	uint8_t variants = 0x0F;  ///< the colour variants (bit per slot, TileHash2Bit()) it is
	uint8_t theme = 0;        ///< the towns it is drawn in ("tema", TownTheme): 0 every town, else only the towns of that theme
	int tiles_x = 1;          ///< its size in tiles
	int tiles_y = 1;
	std::vector<float> pos;   ///< x, y, z of each corner: world units along x and y from the north corner, units of height up
	/* Its night (the colleague's node "noc", hra/3d/PROSBA-MESTO-HLINENE.md): the lit windows and lanterns, drawn over
	 * it at night (gui.night) and not darkened, in its texture; and where a glow is ("svetlo_NN"). As pos and uv. */
	std::vector<float> night_pos;
	std::vector<float> night_uv;
	std::vector<uint32_t> night_tri;
	std::vector<uint8_t> night_tex; ///< each triangle's texture, as tri_tex (an add-on's lamp: its own, in its colour at the place); empty all its own
	std::vector<float> lights; ///< x, y, z of each light, as pos
	std::vector<float> uv;    ///< u, v of each corner in the texture, 0..1
	std::vector<uint32_t> tri; ///< three corners a triangle
	std::vector<int8_t> facing; ///< of each triangle, its outer side: 1 as its corners go round (counter-clockwise seen from outside), -1 the other way, 0 seen from both sides
	float top = 0;            ///< its height, units of height
	std::vector<uint8_t> png; ///< the texture as it is in the file, read into pixels the first time it is drawn
	mutable std::vector<ThreeDObjectTexture> levels; ///< the texture, halved for each level of detail
	std::vector<uint16_t> light; ///< the light of each corner from its normal, 256 for 1 (lit as the vehicles); empty when the file has no normals, its light in its texture
	std::vector<ThreeDObjectColourPart> colours; ///< "barvy": parts painted a colour by number
	std::vector<ThreeDObjectFigure> figures;     ///< "postavy": figures standing on it, drawn among its triangles
	std::vector<ThreeDObjectFigure> girls;       ///< "postavy_s_holkami" of an industry: drawn so too while its girls are there (HasGirls(): the statue, the school, the machine)
	std::array<std::vector<ThreeDObjectFigure>, 2> hut_girls; ///< "postavy_stoji" [0] and "postavy_sedi" [1] of the hut: drawn as HutGirlsAt() says
	std::vector<ThreeDObjectFigure> plants;      ///< "rostliny": plants standing on it ("druh": "rostlina" of the index, a figure 1 m high), drawn so too
	/* Its add-ons (hra/3d/DOPLNKY-DOMU-A-POSTAVY.md: a parasol, a bench, a bicycle, each once for all the buildings):
	 * laid into its shape when it is read (Read()), their triangles with their own textures. */
	std::vector<ThreeDObjectFigure> addon_places; ///< "doplnky": each add-on's name ("jmeno", as figure), place, turn and size
	std::vector<size_t> addon_objects; ///< the add-ons of its textures 1, 2, ... (objects of the add-ons' index, "druh": "doplnek")
	/** By its textures 1, 2, ... as addon_objects: the place of an add-on painted a colour by number ("barvy" of its record,
	 * the player, 18. 10.: "the parasols, the bicycles a colour by number") -- a texture of its own at each place, each
	 * its own colour (AddonPaintAt()); UINT16_MAX a texture shared by all its places. */
	std::vector<uint16_t> addon_places_of;
	std::vector<uint8_t> tri_tex;     ///< each triangle's texture: 0 its own, n the add-on addon_objects[n - 1]'s, past those its other materials'; empty all its own
	std::vector<uint8_t> corner_tex;  ///< each corner's texture so
	/* Its file's other materials (a scene of several: the forest's parts, the grass and the ferns beside the ground and
	 * the atlas of its stumps), each its own texture after its add-ons': texture 1 + addon_objects.size() + n is n's. */
	std::vector<ThreeDTextureBytes> material_pngs; ///< their pictures as in the file; nullptr: only the material's colour
	std::vector<uint32_t> material_flats;          ///< their colours where they have no picture
	mutable std::vector<ThreeDTextureLevels> material_levels; ///< their textures, made the first time drawn (MaterialTexture())
	bool addon = false;               ///< an add-on itself ("druh": "doplnek"): never a house of its own
	float trunk_r = 0;                ///< an add-on's "kmen_r" (a tree), metres at size 1: only its trunk stands in the way, not its crown; 0 all of it
	std::vector<uint32_t> passable_tris; ///< the triangles of its laid trees that stand in no one's way (trunk_r), first and end of each run
	std::vector<float> trunks;           ///< the trunks of its laid trees instead: x, y and radius of each, world units (Blocks())
	/* A house with an interior (the colleague's hra/3d/PROSBA-INTERIERY.md): the interior an object of its own in the
	 * house's frame, the wings of the doors objects of their own, each in its three states. */
	size_t interior = SIZE_MAX;               ///< "interier": its interior's object, SIZE_MAX none
	size_t interior_unlit = SIZE_MAX;         ///< and that interior with its lamp taken (TakeThreeDLamp())
	bool unlit = false;                       ///< an interior with its lamp taken: nothing of its night, nor its nodes "lucerna..."
	std::vector<std::array<size_t, 3>> door_wings; ///< the wings of its doors and windows ("dvere_NN", "okno_NN" of its file, then of its interior's): shut, open one way, the other
	bool is_interior = false;                 ///< an interior: read with all its nodes, its floors ("podlaha") walked on, nothing solid but its walls
	int8_t scene_turns = -1;                  ///< a part of the forest with a clearing: its whole scene read, turned by so many quarter turns (ReadGlbScene())
	/* An industry: every node of its file read, not only the first (the colleague's dragon castle: "zem", "hrad",
	 * "podlaha"; the vending machine's weeds under "krovi"), but the night's, the wings of doors and these. */
	bool all_roots = false;
	std::vector<std::string> left_out;        ///< with all_roots, the nodes (and their children) read on their own: the heaps of hay ("seno"), the day's ("den")
	/* An industry with an interior (the colleague's dragon castle, its hall): itself without what lies in its
	 * interior's box, drawn near with the interior, which has the whole hall. */
	size_t outside = SIZE_MAX;                ///< that object, SIZE_MAX none
	std::string outside_of;                   ///< of that object: the interior's file whose box is left out
	std::string door_node;                    ///< a door's wing: its node in the file, turned about the node's origin (the hinge)
	int8_t door_turn = 0;                     ///< by 0, +90 or -90 degrees
	std::vector<float> floor;                 ///< an interior's floors: x, y, z of each corner of each triangle, world units (FloorAt())
	std::vector<ThreeDObjectFigure> dragon_places; ///< "mista_draku" (the dragon farm): where each of its dragons stands, its creature ("drak") as figure
	std::vector<uint8_t> dragon_places_in;     ///< and of each where: 1 in a hall ("kde": "sin"), its dragon flies in through the nearest door; 2 in a cave ("jeskyne"), it walks in; 0 out
	std::vector<size_t> hay;                  ///< "seno" "uzly" (the dragon farm): its heaps of hay, each an object of its node alone, shown as many as it has (DragonFarmHay())
	std::optional<double> FloorAt(double x, double y, double up_to) const;
	bool street = false;              ///< a street lamp (GetThreeDStreetLamp()): sorted among the vehicles by its own extent, not its tile's
	float extent[4] = {};             ///< x0, x1, y0, y1 of its corners as read, world units
	/** Night triangle t's texture (night_tex): 0 its own, n its add-on n's. */
	uint8_t NightTexture(size_t t) const { return t / 3 < this->night_tex.size() ? this->night_tex[t / 3] : 0; }
	std::vector<float> nrm;           ///< an add-on's normals, metres, to light it as it is turned at a place
	std::vector<ThreeDObjectFigure> event_figures; ///< "akce": the figures standing about it on its days (GetThreeDHouseEvent(): the new stadium's)
	std::vector<ThreeDObjectDoor> doors;         ///< "dvere": its doors
	std::vector<ThreeDObjectRoof> roofs;         ///< "strechy": places on its roof
	std::string title_cs;     ///< "nazev": its name in the land area information, Czech ("cs") ...
	std::string title_en;     ///< ... and in any other language ("en"); empty for the house's own name
	bool has_stop = false;    ///< "stani": where a car of the game's own that drives to it stops (ThreeDVehicleObject::drive_house)
	float stop[3] = {};       ///< there, world units along x and y from the north corner, units of height up
	float stop_turn = 0;      ///< facing, degrees, 0 along +x
	mutable std::map<std::pair<int, uint32_t>, ThreeDObjectTexture> painted; ///< the texture painted, by level and paint (PaintAt())
	mutable std::array<std::shared_ptr<const ThreeDObjectShape>, THREE_D_SHAPE_LEVELS> shapes; ///< its shape simplified, by level, made the first time it is asked for (GetShape())
	ThreeDUniqueId shape_uid; ///< its own shape's number, for the graphics card (ThreeDObjectShapeView::uid)
	IndustryType industry = 0xFF; ///< "prumysl" ("druh": "prumysl"): the game's own industry it is, 0xFF none or one the game has not yet
	std::string industry_name; ///< "prumysl" as the index writes it ("IT_GYMNASIUM", "lihovar")
	int8_t industry_variant = -1; ///< "varianta": the statue of stone 0 ("kamen") or bronze 1 ("bronz", the industry's random bit 0); -1 any
	std::array<int8_t, 2> variant_paint = {-1, -1}; ///< "varianty": one object for both (the colleague's socha.glb), the colour of its palette its first part takes for stone 0 and bronze 1; -1 by the place
	std::vector<std::string> shared_with; ///< "sdili": the game's industries of another kind that show it too ("IT_OIL_REFINERY": the refinery the distillery), on a square of their tiles as big as it
	std::string field_part;    ///< "dil" of a field ("druh": "pole"): "zem", "cesta", "bouda", "seno", "mari_male", "brambory_vzrostle", ...
	std::array<std::vector<ThreeDObjectFigure>, 2> work;    ///< "prace" of the soil: the girls at work on a tile of marijuana [0], of potatoes [1]
	std::array<std::vector<ThreeDObjectFigure>, 2> by_road; ///< "u_cesty" of the field road: the girls by its north-west edge [0], its south-east one [1]
	size_t texture_of = SIZE_MAX; ///< a part of the original industries placed on a tile ("dily"): the object of the part, whose texture it shares

	bool Read() const;
	bool TryRead() const;
	/** Whether its shape and texture are read (Read()): what is read of it may be looked at then, on any thread. */
	/** Whether it is read (Read()): one made in memory (an industry's part) is from the first; one of add-ons alone (a street
	 * lamp, the trees of a tile of the forest) when they are laid -- before, the forest's trees were never laid in. */
	bool IsRead() const { return (this->glb_path.empty() && !this->street && this->addon_places.empty()) || this->glb_state.value.load(std::memory_order_acquire) == 1; }
	const ThreeDObjectTexture *GetTexture(int level, uint32_t paint = 0, bool may_drop = true) const;
	const ThreeDObjectTexture *GetTextureOf(uint8_t texture, int level, uint32_t paint = 0) const;
	const ThreeDObjectTexture *TryGetTextureOf(uint8_t texture, int level, uint32_t paint = 0) const;
	uint32_t AddonPaintAt(TileIndex origin, uint8_t texture) const;
	/** How many textures its triangles have: its own, its add-ons' and its other materials'. */
	size_t Textures() const { return 1 + this->addon_objects.size() + this->material_pngs.size(); }
	const ThreeDObjectTexture *MaterialTexture(size_t material, int level, bool now) const;
	ThreeDObjectShapeView GetShape(int level) const;
	const ThreeDObjectTexture *TryGetTexture(int level, uint32_t paint = 0) const;
	ThreeDObjectShapeView TryGetShape(int level) const;
	bool Blocks(double x, double y, double feet = 0) const;
	void Forget() const;
	ThreeDObjectTexture Painted(const ThreeDObjectTexture &plain, uint32_t paint) const;

	mutable uint32_t used = 0;              ///< when it was last drawn (ForgetUnusedThreeDObjects()), seconds of the clock of the objects

	mutable std::vector<uint8_t> footprint; ///< what of it stands as high as a body, cells of a 64th of a tile (Blocks()), made the first time
	mutable int footprint_w = 0;            ///< the cells across x, 0 not made yet
	mutable std::map<int, std::vector<uint8_t>> footprints_up; ///< up on it: what stands as high as a body over feet so many steps up, cells of a 128th of a tile (Blocks())
	uint32_t PaintAt(TileIndex origin) const;
};

/**
 * The parts of a vehicle's object a state shows (ThreeDVehicleObject::part_names, PartsOf()): bit n the part n, part 0
 * always drawn, bit 0 the record's driver. 128 bits: the colleague's Tatra of all its bodies has 65 parts.
 */
struct ThreeDParts {
	static constexpr int COUNT = 126; ///< the parts an object may have, part 0 among them (a part at the driver's seat, PART_SEAT + n, fits a byte)
	std::array<uint64_t, 2> bits{};

	constexpr ThreeDParts() = default;
	/** The parts of the bits of the first 64 (1: part 0 alone). */
	constexpr ThreeDParts(uint64_t low) : bits{low, 0} {}
	static constexpr ThreeDParts All()
	{
		ThreeDParts p;
		p.bits = {UINT64_MAX, UINT64_MAX};
		return p;
	}
	constexpr bool Has(int n) const { return n >= 0 && n < 128 && ((this->bits[n >> 6] >> (n & 63)) & 1) != 0; }
	constexpr void Set(int n) { if (n >= 0 && n < 128) this->bits[n >> 6] |= uint64_t{1} << (n & 63); }
	constexpr void Clear(int n) { if (n >= 0 && n < 128) this->bits[n >> 6] &= ~(uint64_t{1} << (n & 63)); }
	/** Whether it has all the parts of another. */
	constexpr bool Contains(const ThreeDParts &o) const { return (this->bits[0] & o.bits[0]) == o.bits[0] && (this->bits[1] & o.bits[1]) == o.bits[1]; }
	constexpr ThreeDParts &operator|=(const ThreeDParts &o)
	{
		this->bits[0] |= o.bits[0];
		this->bits[1] |= o.bits[1];
		return *this;
	}
	constexpr ThreeDParts operator|(const ThreeDParts &o) const { ThreeDParts p = *this; return p |= o; }
	constexpr bool operator==(const ThreeDParts &) const = default;
	constexpr auto operator<=>(const ThreeDParts &) const = default;
};

/**
 * The shape of a vehicle as a 3D object, shared by its paints (the colleague's hra/3d/PROSBA-VOZIDLA-3D.md): metres in
 * the vehicle's frame -- x forward, y to its left, z up, the origin in the middle of the vehicle on the top of the
 * rail -- with the normals, so the game lights it itself.
 */
struct ThreeDVehicleShape {
	ThreeDUniqueId uid;        ///< for the graphics card (three_d_gpu.cpp)
	std::vector<float> pos;    ///< x, y, z of each corner, metres
	std::vector<float> nrm;    ///< its normal; empty when the file has none (drawn unlit)
	std::vector<float> uv;     ///< u, v of each corner in its texture
	std::vector<uint32_t> tri; ///< three corners a triangle
	std::vector<uint8_t> tri_tex; ///< the texture of each triangle: 0 the body's (the one painted, "textura", and the one the paint mask is for), then the others of the file and of the wheels
	std::vector<ThreeDTextureBytes> pngs; ///< the textures, as they are in the files (nullptr: none, the material's colour)
	std::vector<uint32_t> flats; ///< for each texture, the colour where a material has no picture
	std::vector<std::string> tex_names; ///< for each texture, the name of its material
	/* The materials' own light and see-through (the colleague's hra/3d/PROSBA-NOC-ZBRANE-TRABANT-LAMPY.md: the fire
	 * Tatra's beacon, the ghost "BLEND 0.7, it shines by itself"; a material's "emissiveFactor" and "alphaMode"). */
	static constexpr uint8_t GLOW_TEXTURED = 1; ///< tex_glow's alpha: its light the texel times the colour ("emissiveTexture")
	static constexpr uint8_t GLOW_FLAT = 2;     ///< tex_glow's alpha: its light the colour alone
	std::vector<uint32_t> tex_glow;     ///< for each texture, its own light (Colour::data, the alpha GLOW_*), 0 none: added to it unshaded, and at night it is not darkened
	std::vector<uint8_t> tex_alpha;     ///< for each texture, how much of what is behind it hides (a material "BLEND", times its texels' alpha), 255 all
	float x0 = 0, x1 = 0, y0 = 0, y1 = 0, z0 = 0, z1 = 0; ///< its extent, metres
	float all[6] = {};          ///< the extent of all its corners, what the states show among them: x0, x1, y0, y1, z0, z1, metres; all 0 not known
	/* Its lights at night (the colleague's hra/3d/PROSBA-SVETLA-VOZIDEL-UZLY.md): the glass of the node "noc" of its
	 * file, never drawn by day, and the places "svetlo_predni_NN" and "svetlo_zadni_NN" the light shines from. */
	std::vector<float> night_pos;       ///< x, y, z of each corner of the glass, metres
	std::vector<uint32_t> night_tri;    ///< three corners a triangle
	std::vector<uint32_t> night_colour; ///< each triangle's colour, its material's (Colour::data)
	std::vector<uint8_t> night_kind;    ///< which lights a triangle is of (NightLightKind()): 0 the headlights, 1 the red lights at the back, 2 the headlights on the back end, 3 the red lights on the front end
	std::vector<float> front_lights;    ///< x, y, z of each headlight, metres
	std::vector<float> back_lights;     ///< x, y, z of each red light at the back, metres
	/* Going back first (the colleague's hra/3d/VAGONY-3D-V5-PRO-HRU.md: the unit 810, the 753, the Strela): the
	 * headlights on its back end ("svetlo_predni_vzad_NN"), the red lights on its front end ("svetlo_zadni_vpred_NN"). */
	std::vector<float> front_back_lights; ///< x, y, z of each headlight on the back end, metres
	std::vector<float> back_front_lights; ///< x, y, z of each red light on the front end, metres
	/* A witch's lights in her frame (the player, 18. 10.: "light all round the lantern, and from the hand torch a beam";
	 * the colleague's hra/3d/PROSBA-CARODEJNICE.md): the places "lucerna_NN", "svitilna_NN" and "paprsek_NN" of the
	 * frame NN, as her hand and broom have them there. */
	std::vector<float> lanterns;        ///< x, y, z of each lantern's flame, metres
	std::vector<float> torches;         ///< x, y, z of each hand torch's glass, metres
	std::vector<float> torch_aims;      ///< x, y, z of the point a metre along each one's beam, metres
	bool paint_whole = false;          ///< the body's texture (the first) painted all over in the state's colour, without a mask (a car of BeamNG)
	/* Its parts (the colleague's hra/3d/PROSBA-V3S-3D.md): what is drawn only as the state shows it, the glass, the
	 * driver, the wheels that turn, the crew drawn bigger than they are. */
	static constexpr uint8_t PART_DRIVER = 254; ///< tri_part: the driver, not drawn from the own cab's eye
	static constexpr uint8_t PART_GLASS = 255;  ///< tri_part: the glass, drawn see-through after the rest
	std::vector<uint8_t> tri_part;      ///< each triangle's part: 0 always, n the part n of the object's states (ThreeDVehicleObject::part_names), or PART_*; empty all 0
	std::vector<int8_t> tex_addon;      ///< each texture's add-on (ThreeDVehicleObject::addons), -1 none
	std::vector<uint8_t> corner_wheel;  ///< each corner's wheel that turns, n the wheel n - 1 of ThreeDVehicleObject::model_wheels, 0 none; empty none
	float glass_alpha = 0.15f;          ///< how much of the glass's colour a layer of it lays over
	/* The parts of its add-ons (the colleague's hra/3d/PROSBA-NAKLADAKY-TATRA-3D.md, points 5, 6 and 10): the windows,
	 * the lamps and the wheels are in each part's node, shown with it -- the cab's windows and its headlights only
	 * where the cab is. */
	std::vector<uint8_t> tri_glass;     ///< each triangle: 1 of the glass of an add-on's node ("<dil>_sklo"), drawn see-through with its part (tri_part); empty none
	std::vector<uint8_t> night_part;    ///< each triangle of the glass of the lights: its part (0 always: the body's "noc")
	std::vector<uint8_t> front_part;    ///< each headlight: its part (0 always)
	std::vector<uint8_t> back_part;     ///< each light at the back: its part (0 always)
	bool HasPart(uint8_t part) const { return std::ranges::find(this->tri_part, part) != this->tri_part.end(); }
	bool HasLights() const { return !this->night_tri.empty() || !this->front_lights.empty() || !this->back_lights.empty() || this->HasLightsGoingBack(); }
	/** Whether it has lights of its own for going back first (front_back_lights). */
	bool HasLightsGoingBack() const { return !this->front_back_lights.empty() || !this->back_front_lights.empty() || std::ranges::any_of(this->night_kind, [](uint8_t k) { return k >= 2; }); }
	/** Whether a triangle is of the glass: the body's ("sklo") or an add-on's node's. */
	bool IsGlass(size_t triangle) const
	{
		return (triangle < this->tri_part.size() && this->tri_part[triangle] == PART_GLASS) || (triangle < this->tri_glass.size() && this->tri_glass[triangle] != 0);
	}
	/** A texture's own light (tex_glow), 0 none. */
	uint32_t Glow(size_t texture) const { return texture < this->tex_glow.size() ? this->tex_glow[texture] : 0; }
	/** How much of what is behind a texture hides (tex_alpha), 255 all. */
	uint8_t Alpha(size_t texture) const { return texture < this->tex_alpha.size() ? this->tex_alpha[texture] : 255; }
	/** Whether any of its textures has a light of its own. */
	bool Glows() const { return std::ranges::any_of(this->tex_glow, [](uint32_t g) { return g != 0; }); }
	/** Whether a triangle is drawn see-through after the rest: of the glass, or of a material "BLEND". */
	bool IsSeeThrough(size_t triangle) const
	{
		return this->IsGlass(triangle) || this->Alpha(triangle < this->tri_tex.size() ? this->tri_tex[triangle] : 0) < 255;
	}
	/** Whether a see-through triangle is of a material "BLEND": its texels' alpha too (SeeThroughAlpha() times it). */
	bool BlendsByTexel(size_t triangle) const { return this->Alpha(triangle < this->tri_tex.size() ? this->tri_tex[triangle] : 0) < 255; }
	/** How much of what is behind a see-through triangle hides: its material's, else the glass's. */
	float SeeThroughAlpha(size_t triangle) const
	{
		const uint8_t a = this->Alpha(triangle < this->tri_tex.size() ? this->tri_tex[triangle] : 0);
		return a < 255 ? a / 255.0f : this->glass_alpha;
	}
	/** Whether a part of the object is shown with these parts (ThreeDVehicleObject::PartsOf()): 0 always. */
	static constexpr uint8_t PART_SEAT = 128;   ///< tri_part PART_SEAT + n: of the part n, sitting at the driver's seat ("misto_ridicky"), not drawn from the own cab's eye
	/**
	 * Whether a triangle of a part is drawn where the parts are shown (ThreeDVehicleObject::PartsOf()): part 0 always,
	 * the record's driver while bit 0 is set (not where a group of the state takes the driver's seat), else its bit.
	 */
	static bool Shown(uint8_t part, const ThreeDParts &parts)
	{
		if (part == PART_DRIVER) return parts.Has(0);
		if (part >= PART_SEAT && part < PART_SEAT + ThreeDParts::COUNT) part -= PART_SEAT;
		return part == 0 || (part < ThreeDParts::COUNT && parts.Has(part));
	}
	/** Whether a triangle of a part sits at the driver's seat: not drawn from the own cab's eye. */
	static bool AtDriverSeat(uint8_t part) { return part == PART_DRIVER || (part >= PART_SEAT && part < PART_SEAT + ThreeDParts::COUNT); }

	/** How many simpler shapes of it there may be (Simpler()): its corners joined in cells of 2, 4, ... 64 cm. */
	static constexpr int SIMPLER_LEVELS = 6;
	const ThreeDVehicleShape *Simpler(double cell_m) const;
	const ThreeDVehicleShape *TrySimpler(double cell_m) const;
	size_t Bytes() const;
private:
	/** Its simpler shapes, made the first time each is asked for; a copy of the shape has none of its own yet. */
	struct SimplerShapes {
		mutable std::array<std::shared_ptr<const ThreeDVehicleShape>, SIMPLER_LEVELS> shapes;
		SimplerShapes() = default;
		SimplerShapes(const SimplerShapes &) {}
		SimplerShapes &operator=(const SimplerShapes &)
		{
			this->shapes = {};
			return *this;
		}
	};
	SimplerShapes simpler;
};

/** The paint of a vehicle object ("lak"): which pixels of the body's texture are paint, how they fade with age, a colour by number. */
struct ThreeDVehiclePaint {
	std::string mask_file;      ///< "maska": a grey PNG of the body's texture's layout, 255 where it is paint
	bool fades = false;         ///< "cil" given: the paint fades to it with age
	uint32_t target = 0;        ///< "cil": the colour it fades to (Colour::data)
	bool coloured = false;      ///< "barva" given: the paint is that colour, at the lightness of the texture
	uint32_t colour = 0;        ///< "barva" (Colour::data)
	int start = 0;              ///< "zacatek": the texture's lightness the colour is at its own (0: the brightest of the mask)
};

/**
 * The rust of a vehicle object ("rez", the colleague's hra/3d/PROSBA-VOZIDLA-3D.md): a second grey mask of the body's
 * texture's layout, its pixels rusting to a colour with age, over the faded paint.
 */
struct ThreeDVehicleRust {
	std::string mask_file;      ///< "maska"
	uint32_t target = 0;        ///< "cil": the rust's colour (Colour::data)
	bool gradual = false;       ///< "postupne": a pixel's value is the moment it starts rusting (1 at once, 255 at the end of "let", 0 never), else how much it rusts
	float ramp = 0.25f;         ///< "postupne": "nabeh", the share of "let" a pixel takes from bare to all rust
};

/**
 * The size a set's vehicle objects are drawn at as a parameter of the set ("velikost", grrrrf PROSBA-VOZIDLA-3D.md):
 * a value of the parameter, the scale it makes. Without it the game's setting decides.
 */
struct ThreeDVehicleSize {
	int param = -1;             ///< "parametr": the set's parameter; -1 for none
	std::vector<std::pair<uint32_t, float>> values; ///< "hodnoty": a value of the parameter and the scale it draws at

	/** The scale a value of the parameter makes, if it names one. */
	std::optional<float> ScaleOf(uint32_t value) const
	{
		for (const auto &[v, scale] : this->values) {
			if (v == value) return scale;
		}
		return std::nullopt;
	}
};

/**
 * A state of a vehicle object chosen by its GRF ("stavy", grrrrf hra/3d/ROZHRANI-GRF-STAVY.md): the GRF picks a set of
 * its sprites for the vehicle by its own rules (random bits, age, breakdown, load …) and a set whose first sprite is
 * numbered within [first, last] in the GRF draws the object so. The game decides nothing of it by itself (the player,
 * 15. 10.: "the colour, the lid and the beacon the GRF must do; when it does not, nothing, it does not fade").
 */
struct ThreeDVehicleState {
	uint32_t first = 0;         ///< "sprity": the first ...
	uint32_t last = 0;          ///< ... and the last sprite number in the GRF
	uint32_t colour = 0;        ///< "barva": the paint's colour (Colour::data), over its mask; 0 the texture's own
	float fade = 0;             ///< "lak": how far the paint has faded to its target, 0 to 1
	float rust = 0;             ///< "rez": how far it has rusted, 0 to 1
	ThreeDParts parts;          ///< the parts it shows ("doplnky" its nodes, "posadka" a group of the crew): bit n the part n (ThreeDVehicleObject::part_names)
	std::vector<uint32_t> addon_colours; ///< "barvy_doplnku": each add-on's colour (Colour::data), 0 its texture's own
	/* A state of layers (the colleague's hra/3d/PROSBA-SPECIAL-3D.md): the GRF picks a set for each layer of its
	 * sprites, as for its pictures, and the states of the layers make one (ThreeDVehicleObject::ComposeState()). */
	int layer = 0;              ///< "vrstva": the layer of the GRF's sprites it is of
	int8_t piece = -1;          ///< "clanek": the piece whose sets it is of, 0 the first ("hlava"), 1 the anchor ("stred"), 2 the last ("zad"); -1 any
	float turn = 0;             ///< "otoceni": the whole object turned so many degrees about z at its origin (a cart pushed: the cart pulled turned round)
	uint8_t given = 0;          ///< the keys it gives (GIVEN_*): those of a later layer replace an earlier one's
	std::vector<int16_t> addon_parts; ///< "doplnky": the part of each add-on it shows (its node), -1 where it names none of the add-on
	int crew_part = -1;         ///< "posadka": the part of the group of the crew it shows, -1 none named
	std::string set_name;       ///< "sada": the GRF's set it is drawn for, by its name ("nastavba hasici"): a vehicle standing as a building's event (StateNamed())
	bool empty = false;         ///< standing empty: none of its crew drawn, the record's nor its groups' (EmptyStateOf(): the fire Tatra on the pitch)
	/**
	 * The colours of the vehicle's set, detail by detail (true_colour.h, callback 1C1: the configurator, the colour by
	 * the load, by chance, faded with age), its masks of details ("maska_podrobnosti") painted in them; 0 none. Not of
	 * the index: the game puts the vehicle's own into the state it draws (ThreeDVehicleObject::ComposeState()).
	 */
	uint16_t true_colours = 0;
	static constexpr uint8_t GIVEN_COLOUR = 1, GIVEN_FADE = 2, GIVEN_RUST = 4, GIVEN_TURN = 8;
};

/** A figure riding in a vehicle object ("posadka"): a figure of an index ("druh": "postava"), where its feet stand, turned. */
struct ThreeDVehicleCrew {
	std::string figure;         ///< "postava": the figure's name
	float at[3] = {};           ///< "poloha": where its feet stand, metres in the vehicle's frame
	float turn = 0;             ///< "otoceni": degrees about z, 0 facing forward as the vehicle
	float scale = 1.0f;         ///< "meritko": its size against its true one (the girls on a lorry's back twice, as at a stop)
	bool driver_seat = false;   ///< "misto_ridicky": of a group, sitting at the driver's seat -- the record's driver not drawn where its state shows it
};

/**
 * A figure of the crew that walks as the vehicle goes (its figure's "animace" with "chuze", the colleague's
 * hra/3d/PROSBA-SPECIAL-3D.md: the girls pulling and pushing the cart): not in the vehicle's shape, drawn with it on
 * its own, its frame by the way gone (ThreeDVehicleObject::WalkingCrew()).
 */
struct ThreeDWalkingCrew {
	const struct ThreeDVehicleObject *figure = nullptr; ///< the figure, its frames the steps of a round of "krok_m"
	float at[3] = {};           ///< "poloha": where its feet stand, metres in the vehicle's frame
	float turn = 0;             ///< "otoceni": degrees about z
	float scale = 1.0f;         ///< "meritko"
	uint8_t part = 0;           ///< the part that shows it: 0 always ("posadka"), else its group of the crew ("posadky")
};

/**
 * Things laid on a vehicle object from a file of their own ("doplnky", the colleague's hra/3d/ROZHRANI-GRF-STAVY.md):
 * of its nodes only the one the state its GRF picks names, with its children; without a state none.
 */
struct ThreeDVehicleAddon {
	std::string key;            ///< "soubor" as the states name it
	std::string file;           ///< its path
	float at[3] = {};           ///< "poloha": shifted so in the vehicle's frame, metres
	std::vector<std::string> nodes; ///< "uzly": the nodes the states pick from
	std::string mask_file;      ///< "barva" "maska": where it takes the state's colour ("barvy_doplnku"), its path; empty nowhere
	int start = 0;              ///< "barva" "zacatek": the texture's lightness where the colour is its own
	std::string detail_file;    ///< "maska_podrobnosti": its mask of details (ThreeDDetailMask), its path; empty none
};

/**
 * A mask of details of a texture ("maska_podrobnosti", the colleague's hra/3d/PROSBA-NAKLADAKY-TATRA-3D.md, point 2):
 * a grey PNG of the texture's layout whose value is the mask index of the vehicle's 2D pictures there -- the cab 0xC6,
 * the body 0x60, the load 0xB0, 0 none --, painted as a 32bpp sprite's mask is (true_colour.h): the colour of the
 * detail its set gives by callback 1C1, at the texture's lightness. Read whole, never halved: a mask index is not a
 * colour, a mean of two is neither.
 */
struct ThreeDDetailMask {
	int w = 0;
	int h = 0;
	std::vector<uint8_t> index; ///< each pixel's mask index
	ThreeDUniqueId uid; ///< for the graphics card (three_d_gpu.cpp)

	/** The mask index at u, v of the texture, the nearest pixel's. */
	uint8_t At(float u, float v) const
	{
		if (this->w <= 0 || this->h <= 0) return 0;
		const int x = std::clamp(static_cast<int>(u * this->w), 0, this->w - 1), y = std::clamp(static_cast<int>(v * this->h), 0, this->h - 1);
		return this->index[static_cast<size_t>(y) * this->w + x];
	}
};

/** A wheel of the vehicle's own model that turns as it goes ("kola_v_modelu"), the front ones steering. */
struct ThreeDModelWheel {
	std::string node;           ///< "uzel": the wheel's node
	std::string addon;          ///< "soubor": the add-on the node is in (the colleague's Tatras: the front wheels in the cab, the back ones in the chassis of the body); empty the vehicle's own file
	float centre[3] = {};       ///< "stred": its middle, metres in the vehicle's frame
	float radius = 0.5f;        ///< "polomer_m"
	bool steers = false;        ///< "rizeni"
	float max_angle = 35;       ///< "max_uhel": how far it steers at most, degrees
};

/** Wheels of a vehicle object from a file of their own ("kola"): one wheel, at every place it is. */
struct ThreeDVehicleWheels {
	std::string file;           ///< "soubor": the wheel, its centre at the origin, its axle along y, its outer face to +y (the left)
	std::vector<std::array<float, 3>> at; ///< "polohy": the centres, metres in the vehicle's frame
	bool both_sides = true;     ///< "oba_boky": each also mirrored to the other side
};

/**
 * A vehicle as a 3D object: one engine of a set (NewGRF) by the set's id and the engine's number in it, drawn by the
 * game from the object instead of the set's sprites -- as sprites of the eight directions it makes itself, and at
 * any angle in the turned view. One rigid object over the pieces it names, drawn by its anchor piece about the middle
 * of the chain; the other pieces draw nothing.
 */
struct ThreeDVehicleObject {
	std::string name;               ///< its name in the index
	std::string package;            ///< the folder of the sets it was read from: the game's own, or a GRF's package
	std::string file;               ///< the .glb, its path
	std::string paint_file;         ///< a texture over the same layout instead of the file's ("textura"), its path; empty for the file's
	VehicleType type = VehicleType::Invalid;
	uint32_t grfid = 0;             ///< the set's id as the game keeps it
	std::vector<uint16_t> pieces;   ///< the engines (their numbers in the set, Action 0) of the pieces it covers, front first
	int anchor = 0;                 ///< the piece it is drawn by: the middle one ("kotva": "stred")
	bool states_of_anchor = false;  ///< its states name their piece ("clanek"): only the anchor's sets pick them (the colleague's wagons), else every piece's (StateStep())
	bool has_cab = false;           ///< "kabina": where the eye of the cab view is
	float cab[3] = {};              ///< x, y, z, metres
	ThreeDVehiclePaint paint;       ///< "lak"
	ThreeDVehicleRust rust;         ///< "rez"
	ThreeDVehicleSize size;         ///< "velikost"
	std::vector<ThreeDVehicleState> states; ///< "stavy": what the sets its GRF picks mean; none, the object as it is
	bool paint_random = false;      ///< a car of a mod of BeamNG (beamng.h): its states the paints of the mod, one of them each car by its random bits
	uint8_t paint_years = 1;        ///< and of each paint so many states, one a year of its age, the paint fading (BEAMNG_FADE_YEARS + 1)
	HouseID drive_house = UINT16_MAX; ///< a car of the game's own ("auto" with "jizda"): drives between the houses of this original house, from stop ("stani") to stop
	float drive_kmh = 60;           ///< "rychlost_kmh"
	int drive_stand = 400;          ///< "stoji_tiku": ticks it stands at a stop
	std::vector<ThreeDVehicleWheels> wheels; ///< "kola"
	std::vector<ThreeDVehicleCrew> crew;     ///< "posadka": never fading, lit as the vehicle
	std::vector<std::pair<std::string, std::vector<ThreeDVehicleCrew>>> crew_groups; ///< "posadky": named groups of the crew, each shown by the states naming it ("posadka")
	ThreeDParts seat_groups;  ///< by part, the groups of the crew one of which sits at the driver's seat ("misto_ridicky"): the record's driver not drawn where they are shown
	int crew_record_part = 0; ///< the part of the record's own crew ("posadka" of the record), shown but where the state stands empty; 0 none (always drawn)
	mutable std::map<int, int> empty_steps; ///< the step of each state standing empty (EmptyStateOf()), made the first time
	std::vector<ThreeDVehicleAddon> addons;  ///< "doplnky"
	std::vector<std::string> part_names;     ///< the parts the states show: each add-on's nodes ("<soubor>/<uzel>"), then the crew's groups ("posadky/<jmeno>"); part n is bit n of a state's parts
	std::vector<ThreeDModelWheel> model_wheels; ///< "kola_v_modelu"
	std::string glass_node;         ///< "sklo" "uzel": the windows, drawn see-through over what is behind them
	float glass_alpha = 0.15f;      ///< "sklo" "alfa": how much of the glass's colour a layer of it lays over
	std::string frame_prefix;       ///< a figure that moves ("animace"): its frames are the nodes whose names start so ("uzly"), followed by their number; empty for none
	int frames = 0;                 ///< "snimku": how many frames
	int frame_ticks = 1;            ///< "tiku": game ticks a frame is held
	float walk_step = 0;            ///< "chuze": "krok_m", metres a round of its frames walks; 0 when it does not walk
	bool Walks() const { return this->walk_step > 0 && this->frames > 1 && !this->frame_prefix.empty(); }
	bool loop = true;               ///< "smycka": the frames go round; false, they run once and stay on the last (a creature's attack, its death)
	bool thing = false;             ///< a thing among the figures ("druh": "predmet", the spit): as big as the building it is on, also in the view from the eyes
	bool frame_lights = false;      ///< a witch's ("druh": "carodejnice"): the glass of her lights under "noc_NN" and their places read from each frame (ThreeDVehicleShape::lanterns)
	int spin_ticks = 0;             ///< a thing that turns ("predmet" with "otaceni", the spit of the pub): game ticks a turn ("tiku_na_otacku"), 0 for none
	float spin_axis[3] = {1, 0, 0}; ///< "osa": the axis it turns about, in its frame
	float spin_centre[3] = {};      ///< "stred": a point of the axis, metres
	std::string skeleton_file;      ///< a creature's "kostra": its mesh with bones and animations; its frames are posed from them, a frame a tick
	std::string skeleton_anim;      ///< the animation of it this figure is
	const ThreeDVehicleObject *blend_from = nullptr; ///< a passage from another animation of the creature (GetThreeDFigure("<jmeno>/<a>@<snimek>><b>")): the figure it comes from ...
	const ThreeDVehicleObject *blend_to = nullptr;   ///< ... and the one it goes to
	int blend_from_frame = 0;       ///< the frame of the one it comes from
	int blend_ticks = 0;            ///< the passage's ticks: a frame of a passage is the frame of the one it goes to times them, plus its tick
	std::string tint_material;      ///< an effect's "barvy": the material the game colours ("material")
	std::vector<uint32_t> tint_palette; ///< and the colours it may be ("paleta", Colour::data)
	std::string only_node;          ///< a figure of one node of a file of several ("uzel", the colleague's girls of the lorries: a pose a node, the textures once): only that node of it is drawn
	std::vector<std::string> skip_nodes; ///< nodes of it left out (the rocket launcher shot, "<jmeno>/prazdny": its "raketa_v_hlavni"; a saucer's frames: its beam and its sheep)
	bool night_glass = false;       ///< a light's thing ("kategorie": "svetlo", the lantern): its glass ("noc") part of it, lit by its own light
	uint32_t light_colour = 0;      ///< a light's thing: "svetlo" "barva", 0xRRGGBB; 0 the game's own for it
	float light_reach_m = 0;        ///< and "dosah_m", how far it lights; 0 the game's own
	bool light_flickers = false;    ///< and "blika": its light flickers with its flame (the torch's fire)
	/* An articulated vehicle of several objects, one an article (the colleague's hra/3d/PROSBA-NAKLADAKY-TATRA-3D.md,
	 * point 3): where an article is coupled to the one before it and the one after, the next one drawn with its front
	 * coupling on the back one of the one before, turned as its own article. */
	bool has_couple_front = false;  ///< "spojeni" "vpredu": the kingpin of a semi-trailer, the eye of a trailer's drawbar
	float couple_front[3] = {};     ///< there, metres in its frame
	bool has_couple_back = false;   ///< "spojeni" "vzadu": the fifth wheel of a tractor, the hook of a lorry
	float couple_back[3] = {};      ///< there, metres in its frame
	std::vector<int> grf_lengths;   ///< "delky_clanku_grf": its pieces' lengths in its set the object's length is for, eighths of a tile; empty: as the set says now
	float length_m = 0;             ///< "rozmer_m" "delka": its length, metres; 0 from its shape (Length())
	std::string detail_file;        ///< "maska_podrobnosti": the mask of details of its body's texture (ThreeDDetailMask), its path; empty none

	const ThreeDVehicleShape *GetShape() const;
	const ThreeDVehicleShape *GetShape(int frame) const;
	const ThreeDVehicleShape *GetShape(int frame, const ThreeDParts &parts) const;
	const ThreeDVehicleShape *TryGetShape(int frame = 0) const;
	const ThreeDVehicleShape *TryGetShape(int frame, const ThreeDParts &parts) const;
	bool ShapeKnown() const;
	bool KnownExtent(float extent[6], uint32_t *version = nullptr) const;
	int FrameAt(uint64_t tick, uint32_t phase) const;
	const ThreeDObjectTexture *GetTexture(int level, int index = 0) const;
	const ThreeDObjectTexture *TryGetTexture(int level, int index = 0) const;
	bool TextureSize(int index, int &w, int &h) const;
	const ThreeDObjectTexture *GetPaintMask(int level) const;
	const ThreeDObjectTexture *GetRustMask(int level) const;
	const ThreeDObjectTexture *GetAddonMask(size_t addon, int level) const;
	const ThreeDDetailMask *GetDetailMask(int addon) const;
	bool HasDetails() const;
	float Length() const;
	ThreeDParts PartsOf(int state) const;
	const ThreeDVehicleState *State(int step) const;
	int ComposeState(const std::vector<int> &layers, uint16_t true_colours = 0) const;
	int StateNamed(std::string_view set) const;
	int EmptyStateOf(int step) const;
	const std::vector<ThreeDWalkingCrew> &WalkingCrew() const;
	void AddCrewFigure(ThreeDVehicleShape &made, const ThreeDVehicleCrew &c, uint8_t part) const;
	void Forget() const;
	size_t TrimTextures(std::set<const void *> &seen) const;

	mutable uint32_t used = 0;      ///< when it was last drawn (ForgetUnusedThreeDObjects()), seconds of the clock of the objects

	mutable std::shared_ptr<const ThreeDVehicleShape> shape; ///< read the first time it is drawn
	mutable bool shape_tried = false;
	mutable ThreeDParts shape_parts; ///< the parts of its states the shape has (PartsOf()): only those of the states drawn, the others added as they are drawn
	mutable float extent[6] = {};   ///< its shape's extent of all its corners (ThreeDVehicleShape::all), kept when the shape is forgotten
	mutable bool extent_known = false;
	mutable uint32_t extent_version = 0; ///< each time the extent grew (a part added to the shape)
	mutable std::vector<std::shared_ptr<const ThreeDVehicleShape>> frame_shapes; ///< the frames of a figure that moves, read as drawn
	mutable std::deque<ThreeDVehicleState> composed; ///< the states made of the states of layers (ComposeState()), steps after those of "stavy"
	mutable std::map<std::pair<std::vector<int>, uint16_t>, int> composed_steps; ///< the step each was made for, by the steps of its layers and the vehicle's colours
	mutable std::vector<ThreeDWalkingCrew> walking; ///< its crew that walks (WalkingCrew()), found the first time asked for
	mutable bool walking_found = false;

private:
	std::shared_ptr<const ThreeDVehicleShape> MakeShape(int frame, const ThreeDParts &parts = ThreeDParts::All()) const;
	std::shared_ptr<const ThreeDVehicleShape> MakePosedShape(int frame) const;
	mutable std::vector<ThreeDTextureLevels> levels; ///< the textures, each halved for each level of detail
	mutable ThreeDTextureLevels mask_levels;         ///< the paint mask likewise (its grey in the red)
	mutable bool mask_tried = false;
	mutable ThreeDTextureLevels rust_levels;         ///< the rust mask likewise
	mutable bool rust_tried = false;
	mutable std::vector<ThreeDTextureLevels> addon_mask_levels; ///< each add-on's colour mask likewise
	mutable std::vector<bool> addon_mask_tried;
	mutable std::vector<std::shared_ptr<const ThreeDDetailMask>> detail_masks; ///< the masks of details: [0] the body's, [1 + n] the add-on n's (GetDetailMask())
	mutable std::vector<bool> detail_tried;
};

const ThreeDVehicleObject *GetThreeDVehicleObject(EngineID engine, int *piece = nullptr);
const ThreeDVehicleObject *ThreeDVehicleObjectOf(const Vehicle *v, int *piece = nullptr);
const ThreeDVehicleObject *GetThreeDFigure(std::string_view name, std::string_view package = {});
const ThreeDVehicleObject *GetThreeDVehicleObjectNamed(std::string_view name);
std::optional<std::array<float, 3>> GetThreeDThingPlace(std::string_view thing, std::string_view node);
std::optional<SoundID> GetThreeDThingSound(std::string_view thing, std::string_view sound);

/**
 * A creature of the indices ("druh": "tvor", the colleague's dragons, hra/3d/PROSBA-DRACI.md): its animations, each
 * read as a figure of its own named "<jmeno>/<animace>" (frames as nodes, "uzly", "snimku", "tiku", "smycka", "krok_m").
 */
struct ThreeDCreature {
	std::string name;                    ///< "jmeno"
	std::vector<std::string> animations; ///< the names of its animations ("klid", "chuze", "utok", "smrt", "let")
	float size[3] = {};                  ///< "rozmer_m": length, width and height, metres

	bool Has(std::string_view animation) const { return std::ranges::find(this->animations, animation) != this->animations.end(); }
	std::string Figure(std::string_view animation) const { return this->name + "/" + std::string(animation); }
};
const std::vector<ThreeDCreature> &GetThreeDCreatures();

/**
 * A witch of the indices ("druh": "carodejnice", the player, 18. 10.: "witches flying at night, as big as the girls at
 * a stop"; the colleague's hra/3d/PROSBA-CARODEJNICE.md): her animations as a creature's, "<jmeno>/<animace>" (she
 * flies "let"), how big she is drawn, and the light of her lantern and of her hand torch ("svetla"), what they are like
 * the index's: none where it gives none.
 */
struct ThreeDWitch {
	std::string name;                    ///< "jmeno"
	std::vector<std::string> animations; ///< the names of her animations ("let")
	float size[3] = {};                  ///< "rozmer_m": length, width and height, metres
	float scale = 2.0f;                  ///< "meritko": so many times her true size on the map (2 as the girls at a stop)
	uint32_t lantern_colour = 0;         ///< "svetla" "lucerna" "barva" (Colour::data); 0 no light of a lantern
	float lantern_reach_m = 0;           ///< its "dosah_m", true metres
	uint32_t torch_colour = 0;           ///< "svetla" "svitilna" "barva"; 0 no beam of a torch
	float torch_reach_m = 0;             ///< its "dosah_m", true metres
	float torch_angle = 0;               ///< its "uhel": the cone's whole angle, degrees

	bool Has(std::string_view animation) const { return std::ranges::find(this->animations, animation) != this->animations.end(); }
	std::string Figure(std::string_view animation) const { return this->name + "/" + std::string(animation); }
};
const std::vector<ThreeDWitch> &GetThreeDWitches();

/**
 * A flying saucer of the indices ("druh": "ufo", the colleague's hra/3d/PROSBA-UFO.md): the small and the big UFO of
 * the disasters, and flying over the towns at night, breaking nothing. Its figures: "<jmeno>/let" (its frames, without
 * its beam and its sheep), "<jmeno>/paprsek" (the beam alone, from the hole in its bottom down) and "<jmeno>/ovce".
 */
struct ThreeDUfo {
	std::string name;           ///< "jmeno"
	float size[3] = {};         ///< "rozmer_m": length, width and height, metres
	float small_scale = 1.0f;   ///< "katastrofa" "male_ufo" "meritko": the small UFO, so many times its true size
	float big_scale = 2.0f;     ///< "katastrofa" "velke_ufo" "meritko"
	bool disaster = false;      ///< "kdy" has "katastrofa": drawn for the disasters' UFOs
	bool night = false;         ///< "kdy" has "noc": flies over the towns at night
	float night_scale = 1.0f;   ///< "meritko": its size flying at night
	float night_lift_m = 13.0f; ///< "noc" "vyska_m": its middle over the ground at night, true metres
	float beam_len_m = 12.0f;   ///< "paprsek" "delka_m": the beam's length in the model
	float beam_from_z = -1.0f;  ///< "paprsek" "od_z": where the beam starts under its middle, metres
	bool has_beam = false;      ///< it has "paprsek"
	bool has_sheep = false;     ///< it has "ovce"
	uint32_t beam_colour = 0;   ///< "svetla" "paprsek" "barva" (Colour::data); 0 no light of it at night
	float beam_reach_m = 0;     ///< its "dosah_m"
	float beam_angle = 14.0f;   ///< its "uhel", degrees
	uint32_t rim_colour = 0;    ///< "svetla" "lem" "barva": the light all round it at night
	float rim_reach_m = 0;      ///< its "dosah_m"
	std::string crew;           ///< "posadka" "postava": who gets out where it lands at night

	std::string Figure(std::string_view part) const { return this->name + "/" + std::string(part); }
};
const std::vector<ThreeDUfo> &GetThreeDUfos();

/**
 * A dog, an animal, a ghost or a seller of dogs of the indices (the colleague's hra/3d/PSI-ZVIRATA-HOUBY-PRO-HRU.md;
 * "druh" "pes", "zvire", "strasidlo", or "postava" with "prodavac"): its poses figures of their own,
 * "<jmeno>/<poza>" ("pozy" with bones, or "animace" of frames), the figure "<jmeno>" as it stands; where it is found,
 * how it takes the walker, what it costs. What it does is the index's (the player: "the index decides, not the game").
 */
struct ThreeDAnimal {
	enum class Kind : uint8_t {
		Dog,    ///< "pes": a dog, a cat, a fox, a wolf
		Animal, ///< "zvire": about the towns, in the desert towns' streets and on their roofs
		Ghost,  ///< "strasidlo": out at night
		Seller, ///< a seller of dogs ("prodavac": "psi")
	};
	enum class Temper : uint8_t {
		Peaceful, ///< "mirumilovne": never fights, runs from a blow
		Provoked, ///< "vyprovokovat": peaceful till struck, then fights
		Attacks,  ///< "utoci": goes for the walker
	};
	/* Where it is found ("kde"), bits. */
	static constexpr uint8_t STREET = 1; ///< "ulice": in the streets of the towns
	static constexpr uint8_t DOOR = 2;   ///< "dvere": before the doors of the houses
	static constexpr uint8_t ROUND = 4;  ///< "okoli": in the land about the towns
	static constexpr uint8_t ROOF = 8;   ///< "strecha": on the flat roofs ("strechy")
	static constexpr uint8_t RAIL = 16;  ///< "koleje": along the railway lines (where the game puts any ghost too)
	/* The towns it is found at ("klima", one or a list): bits, 0 any. */
	static constexpr uint8_t TEMPERATE = 1; ///< "mirne"
	static constexpr uint8_t DESERT = 2;    ///< "poust": the desert towns
	static constexpr uint8_t ARCTIC = 4;    ///< "subarkticke"

	std::string name;            ///< "jmeno"
	std::string title_cs, title_en; ///< "nazev"
	Kind kind = Kind::Dog;
	Temper temper = Temper::Peaceful; ///< "povaha"
	uint8_t places = 0;          ///< "kde", bits
	uint8_t climate = 0;         ///< "klima", bits
	uint8_t theme = 0;           ///< "tema": only about the towns of the theme (TownTheme), 0 any
	bool night = false;          ///< "kdy": "noc": out only at night
	bool day = false;            ///< "kdy": "den": out only by day (the colleague's gnome, PAR14-PRO-HRU.md)
	float hover_m = 0;           ///< "vznaseni_m" (or "vyska_m"): its origin floats so high over the ground, true metres (the ghosts of the forest)

	/** Whether it is out at this time of day ("kdy"; none: always). */
	bool OutNow(bool is_night) const { return is_night ? !this->day : !this->night; }
	bool sold = false;           ///< "prodej": a seller sells it, it walks with the walker
	bool flies = false;          ///< "leta": flies beside the walker
	int price = 0;               ///< "cena", pounds
	float size[3] = {};          ///< "rozmer_m": length, width and height, metres
	float scale = 2.0f;          ///< "meritko": so many times its true size on the map (2 as the girls)
	std::vector<std::string> poses; ///< its poses ("klid", "chuze", "beh", "utok", "smrt", "lezi"...)

	bool Has(std::string_view pose) const { return std::ranges::find(this->poses, pose) != this->poses.end(); }
	std::string Figure(std::string_view pose) const { return pose.empty() || !this->Has(pose) ? this->name : this->name + "/" + std::string(pose); }
	const std::string &Title() const;
};
const std::vector<ThreeDAnimal> &GetThreeDAnimals();
const ThreeDAnimal *GetThreeDAnimal(std::string_view name);

/** A mushroom to pick of the indices ("predmet" of "kategorie" "houba"): where, and its glowing one for the night ("kdy": "noc"). */
struct ThreeDMushroom {
	std::string name;            ///< "jmeno", a figure
	std::string night;           ///< the one glowing at night, "<jmeno>_svitici"; empty none
	std::string title_cs, title_en; ///< "nazev"
	uint8_t theme = 0;           ///< "tema": about the towns of the theme
	float height_m = 0.1f;       ///< "vyska_m"
};
const std::vector<ThreeDMushroom> &GetThreeDMushrooms();
const ThreeDVehicleObject *GetThreeDEffect(std::string_view name);
const std::vector<ThreeDVehicleObject> &GetThreeDDrivingCars();
void ResetThreeDVehicleObjects();
const ThreeDVehicleSize *GetThreeDVehicleSize(uint32_t grfid);
std::string DescribeThreeDVehicleObjects();

const ThreeDObject *GetThreeDHouseObject(HouseID north_house, uint colour_slot, uint8_t theme = 0);
std::span<const ThreeDObjectFigure> GetThreeDHouseEvent(const ThreeDObject *obj, TileIndex origin);
extern bool _three_d_event_forced;
const ThreeDObject *GetThreeDIndustryObject(IndustryType type, bool variant);
std::string ThreeDPackagePlaces();
struct Industry;
const ThreeDObject *GetThreeDIndustryObjectOf(const Industry *ind);
TileIndex GetThreeDIndustryObjectOrigin(const Industry *ind);
std::span<const ThreeDObjectFigure> GetThreeDIndustryGirls(const Industry *ind, const ThreeDObject *obj);
const ThreeDObject *GetThreeDFieldPart(std::string_view part);
const ThreeDObject *GetThreeDStreetLamp(int dx, int dy);
const ThreeDObject *GetThreeDObjectByName(std::string_view name);
const ThreeDObject *GetThreeDObjectNumbered(size_t index);
const ThreeDObject *GetThreeDInterior(const ThreeDObject *house);
const ThreeDObject *GetThreeDObjectToStandOnAt(TileIndex tile, TileIndex &origin);
const ThreeDObject *GetThreeDObjectWithRoomsAt(TileIndex tile, TileIndex &origin);
const ThreeDObject *GetThreeDOutsideOfInterior(const ThreeDObject *obj);
bool ThreeDInsideInterior(const ThreeDObject *house, double x, double y);
const ThreeDObject *GetThreeDDoorWing(const ThreeDObject *house, TileIndex origin, size_t door);
std::optional<size_t> ThreeDDoorNear(const ThreeDObject *house, TileIndex origin, double x, double y, double reach);
void ToggleThreeDDoor(const ThreeDObject *house, TileIndex origin, size_t door, double x, double y);
bool ThreeDDoorOpen(TileIndex origin, size_t door);
bool ThreeDDoorIsWindow(const ThreeDObject *house, size_t door);
const ThreeDObject *GetThreeDInteriorAt(const ThreeDObject *house, TileIndex origin);
bool ThreeDLampNear(const ThreeDObject *house, TileIndex origin, double x, double y, double reach);
void TakeThreeDLamp(TileIndex origin);
/**
 * The pasture of the farms (the colleague's pv_pole_pastvina, hra/3d/PASTVINY-OKNA-ROZHLEDNA-PRO-HRU.md; the game's
 * pastures, IsPasture()): what stands on a tile of it, each an object placed in the tile's frame from its north corner.
 */
struct ThreeDPasture {
	const ThreeDObject *ground = nullptr;              ///< "podklad": the grass grazed
	std::array<const ThreeDObject *, 4> fence{};       ///< "plot" "hrana": the fence along a side of the tile, by DiagDirection
	std::array<const ThreeDObject *, 4> gate{};        ///< "plot" "otvor": so, with the gap of the way in
	std::vector<const ThreeDObject *> extras;          ///< "doplnky_pole": the shelter, the trough; each on a tile of its own
	std::vector<ThreeDObjectFigure> places;            ///< "zvirata": where the animals of a tile stand (the figure the caller's)
	std::array<std::string, 3> kinds;                  ///< "druhy_zvirat": the animals by PastureKind (cows, sheep, pigs)
	std::array<int, 3> count{};                        ///< "zvirat_na_policko": so many of each on a tile
};
const ThreeDPasture *GetThreeDPasture();
const ThreeDObject *GetThreeDSlopedPlants(const ThreeDObject *plants, Slope slope);

/**
 * A tile of an industry of the original game drawn from the parts the industries share (the colleague's
 * hra/3d/PROSBA-PRUMYSL-VANILLA-3D.md, set prumysl_vanilla): the ground of the tile, the parts standing on it (of the
 * whole building when the tile is the anchor of a block of tiles), and the plants (the forest, the plantations).
 */
struct ThreeDIndustryTileParts {
	const ThreeDObject *ground = nullptr;          ///< "podklad": the yard, the soil
	std::vector<const ThreeDObject *> parts;       ///< "dily", each placed where it stands
	const ThreeDObject *plants = nullptr;          ///< "rostliny": an object of the plants alone
	bool member = false;                           ///< a tile of a block its anchor draws ("soucast_bloku"): its ground only
	int anchor_dx = 0, anchor_dy = 0;              ///< "posun": this tile's place from the anchor's, tiles
	bool at_once = false;                          ///< trees and plants alone, no building: they stand from the first, the industry being built or not (the forest)
};
const ThreeDIndustryTileParts *GetThreeDOriginalIndustryTile(IndustryGfx gfx);
bool ThreeDOriginalIndustryTileStands(TileIndex tile, const ThreeDIndustryTileParts &parts);
bool ThreeDForestTile(TileIndex tile);

/** A place of the forest with a clearing where the game or the ghosts come ("zvirata", "noc_postavy"), with their kinds. */
struct ThreeDForestPlace {
	ThreeDObjectFigure at;           ///< where (figure empty), facing
	std::vector<std::string> kinds;  ///< "druhy" / "postavy": of the animals and ghosts of the indices, by name
};

/**
 * A part of the forest with a clearing (the colleague's "druh": "les", hra/3d/LES-PRO-HRU.md) as it lies on a tile,
 * turned by so many quarter turns about the tile's middle: its ground (the forest floor, the ferns, the path, the
 * forester's log cabin), its trees and bushes, and where its girls, mushrooms, game and ghosts are -- metres from the
 * north corner of the tile.
 */
struct ThreeDForestPart {
	const ThreeDObject *ground = nullptr;     ///< the part's model with its trees and bushes ("doplnky") laid in; its night the cabin's windows, lantern, embers
	std::vector<ThreeDObjectFigure> girls;    ///< "holky": after a delivery of studentky
	std::vector<ThreeDObjectFigure> mushrooms; ///< "houby": where they grow, all the time (the kind drawn by the place)
	std::vector<ThreeDForestPlace> animals;   ///< "zvirata"
	std::vector<ThreeDForestPlace> ghosts;    ///< "noc_postavy", at night
};
const ThreeDForestPart *ThreeDGameForestPartAt(TileIndex tile);
const ThreeDIndustryTileParts *GetThreeDIndustryYard(const Industry *ind, TileIndex tile);
std::optional<bool> ThreeDOriginalIndustryBlocks(TileIndex tile, double x, double y);

struct ThreeDPlantationTile;
/**
 * A tile of a field laid from the parts of the fields (GetThreeDFieldPart(), the colleague's
 * hra/3d/PROSBA-PRUMYSL-3D.md): the soil with the plants of the stage on it and the girl at work, and from the tile
 * before the field road the road tile behind it with the road, the shed and the haystack and the girls by its edges.
 */
struct ThreeDFieldTile {
	const ThreeDObject *soil = nullptr;     ///< the soil ("zem")
	const ThreeDObject *plants = nullptr;   ///< the plants of the stage, nullptr bare
	std::span<const ThreeDObjectFigure> worker; ///< the girl at work on it, of the soil's "prace"
	const ThreeDObject *road = nullptr;     ///< on the road tile behind: the field road ("cesta"), nullptr no road tile behind
	const ThreeDObject *shed = nullptr;     ///< the shed ("bouda")
	const ThreeDObject *hay = nullptr;      ///< the haystack ("seno")
	std::span<const ThreeDObjectFigure> girls[2]; ///< the girls by the road's north-west edge and its south-east one
};
bool GetThreeDFieldTile(const ThreeDPlantationTile &tile, ThreeDFieldTile &out);

/** A 3D object laid in a picture (RenderThreeDObjectsPicture()): where, world units from the picture's origin, and what stands on it besides. */
struct ThreeDObjectPlacement {
	const ThreeDObject *obj = nullptr;      ///< nullptr: only the figures
	const ThreeDObject *ground = nullptr;   ///< drawn first under it, whole
	std::span<const ThreeDObjectFigure> more; ///< figures besides its own
	double x = 0, y = 0, z = 0;             ///< its north corner
	uint32_t paint = 0;                     ///< its colours by number (ThreeDObject::PaintAt())
};
struct ThreeDModelPicture;
bool RenderThreeDObjectsPicture(std::span<const ThreeDObjectPlacement> parts, float yaw, ZoomLevel zoom, ThreeDModelPicture &out);
const ThreeDObject *GetThreeDHouseObjectAny(HouseID north_house, uint colour_slot, uint8_t theme = 0);
const ThreeDObject *GetThreeDHouseObjectAt(HouseID north_house, TileIndex origin);
void NoteThreeDHouseBoxed(HouseID house);
/** A texture of an object wanted for drawing: the object, the level of detail and the paint (PrepareThreeDObjectTextures()). */
struct ThreeDTextureWanted {
	const ThreeDObject *obj;
	int level;
	uint32_t paint;
};
void PrepareThreeDObjectTextures(std::span<const ThreeDTextureWanted> wanted);
void StartReadingThreeDObjects();
void WaitForReadingThreeDObjects();
bool ThreeDObjectsIndexRead();
bool ThreeDObjectReadyToDraw(const ThreeDObject *o, int level, uint32_t paint);
uint64_t ThreeDObjectsReadied();
void PrepareThreeDObjectShapes(std::span<const std::pair<const ThreeDObject *, int>> wanted);

/*
 * Loading in the background (the player, 17. 10., 4): what the view from the eyes has not got yet -- a shape, a
 * texture, a simpler shape, a painted texture -- it asks for (the Try... getters above: nullptr, or what stands in
 * meanwhile) and draws without it this frame; the thread making the pictures of the vehicles
 * (three_d_vehicle_object.cpp) makes it, and the next frame has it.
 */
void ThreeDLoadInBackground(std::function<void()> job);
extern bool _three_d_load_at_once; ///< the rig: a job is run the moment it is asked for, so a picture is of the moment
std::span<const std::string> GetThreeDThemeTrees(uint8_t theme, bool night = false);
std::optional<std::string> GetThreeDHouseTitle(TileIndex tile);
std::string DescribeThreeDObjects();

#endif /* THREE_D_OBJECTS_H */
