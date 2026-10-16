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
#include "industry_type.h"
#include "engine_type.h"
#include "tile_type.h"
#include "vehicle_type.h"
#include "zoom_type.h"

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

/** World units along x and y to a metre of the objects: a tile, 16 units, is 14.83765 m (the renders' scale). */
static constexpr double OBJECT_UNITS_PER_METRE = 16.0 / 14.83765;
/** Units of height to a metre: a height level, 8 units, is 3 m (hra/3d/OBJEKTY-3D.md). */
static constexpr double OBJECT_HEIGHT_PER_METRE = 8.0 / 3.0;

/** A texture of an object at one level of detail: 32bpp pixels in the blitter's colours, 0 where it is transparent. */
struct ThreeDObjectTexture {
	int w = 0;
	int h = 0;
	std::vector<uint32_t> px;
};

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
};

/** A shape of a building to draw: its own (level 0) or simplified (ThreeDObject::GetShape()). */
struct ThreeDObjectShapeView {
	std::span<const float> pos;
	std::span<const float> uv;
	std::span<const uint16_t> light;
	std::span<const uint32_t> tri;
	std::span<const int8_t> facing;
};

/** The levels of the shapes: 0 the building's own, then its corners merged in cells of 5 cm, 10 cm, ... 3.2 m. */
static constexpr int THREE_D_SHAPE_LEVELS = 8;
int ThreeDShapeLevel(double pixels_per_metre);

/** A building as a 3D object. */
struct ThreeDObject {
	std::string name;         ///< its name in the index
	std::string file;         ///< the .glb it came from
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
	std::vector<ThreeDObjectFigure> plants;      ///< "rostliny": plants standing on it ("druh": "rostlina" of the index, a figure 1 m high), drawn so too
	std::vector<ThreeDObjectDoor> doors;         ///< "dvere": its doors
	std::string title_cs;     ///< "nazev": its name in the land area information, Czech ("cs") ...
	std::string title_en;     ///< ... and in any other language ("en"); empty for the house's own name
	bool has_stop = false;    ///< "stani": where a car of the game's own that drives to it stops (ThreeDVehicleObject::drive_house)
	float stop[3] = {};       ///< there, world units along x and y from the north corner, units of height up
	float stop_turn = 0;      ///< facing, degrees, 0 along +x
	mutable std::map<std::pair<int, uint32_t>, ThreeDObjectTexture> painted; ///< the texture painted, by level and paint (PaintAt())
	mutable std::array<std::shared_ptr<const ThreeDObjectShape>, THREE_D_SHAPE_LEVELS> shapes; ///< its shape simplified, by level, made the first time it is asked for (GetShape())
	IndustryType industry = 0xFF; ///< "prumysl" ("druh": "prumysl"): the game's own industry it is, 0xFF none or one the game has not yet
	std::string industry_name; ///< "prumysl" as the index writes it ("IT_GYMNASIUM", "lihovar")
	int8_t industry_variant = -1; ///< "varianta": the statue of stone 0 ("kamen") or bronze 1 ("bronz", the industry's random bit 0); -1 any
	std::string field_part;    ///< "dil" of a field ("druh": "pole"): "zem", "cesta", "bouda", "seno", "mari_male", "brambory_vzrostle", ...
	std::array<std::vector<ThreeDObjectFigure>, 2> work;    ///< "prace" of the soil: the girls at work on a tile of marijuana [0], of potatoes [1]
	std::array<std::vector<ThreeDObjectFigure>, 2> by_road; ///< "u_cesty" of the field road: the girls by its north-west edge [0], its south-east one [1]

	const ThreeDObjectTexture *GetTexture(int level, uint32_t paint = 0, bool may_drop = true) const;
	ThreeDObjectShapeView GetShape(int level) const;
	uint32_t PaintAt(TileIndex origin) const;
};

/**
 * The shape of a vehicle as a 3D object, shared by its paints (the colleague's hra/3d/PROSBA-VOZIDLA-3D.md): metres in
 * the vehicle's frame -- x forward, y to its left, z up, the origin in the middle of the vehicle on the top of the
 * rail -- with the normals, so the game lights it itself.
 */
struct ThreeDVehicleShape {
	std::vector<float> pos;    ///< x, y, z of each corner, metres
	std::vector<float> nrm;    ///< its normal; empty when the file has none (drawn unlit)
	std::vector<float> uv;     ///< u, v of each corner in its texture
	std::vector<uint32_t> tri; ///< three corners a triangle
	std::vector<uint8_t> tri_tex; ///< the texture of each triangle: 0 the body's (the one painted, "textura", and the one the paint mask is for), then the others of the file and of the wheels
	std::vector<std::vector<uint8_t>> pngs; ///< the textures, as they are in the files
	std::vector<uint32_t> flats; ///< for each texture, the colour where a material has no picture
	std::vector<std::string> tex_names; ///< for each texture, the name of its material
	float x0 = 0, x1 = 0, y0 = 0, y1 = 0, z0 = 0, z1 = 0; ///< its extent, metres
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
};

/** A figure riding in a vehicle object ("posadka"): a figure of an index ("druh": "postava"), where its feet stand, turned. */
struct ThreeDVehicleCrew {
	std::string figure;         ///< "postava": the figure's name
	float at[3] = {};           ///< "poloha": where its feet stand, metres in the vehicle's frame
	float turn = 0;             ///< "otoceni": degrees about z, 0 facing forward as the vehicle
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
	bool has_cab = false;           ///< "kabina": where the eye of the cab view is
	float cab[3] = {};              ///< x, y, z, metres
	ThreeDVehiclePaint paint;       ///< "lak"
	ThreeDVehicleRust rust;         ///< "rez"
	ThreeDVehicleSize size;         ///< "velikost"
	std::vector<ThreeDVehicleState> states; ///< "stavy": what the sets its GRF picks mean; none, the object as it is
	HouseID drive_house = UINT16_MAX; ///< a car of the game's own ("auto" with "jizda"): drives between the houses of this original house, from stop ("stani") to stop
	float drive_kmh = 60;           ///< "rychlost_kmh"
	int drive_stand = 400;          ///< "stoji_tiku": ticks it stands at a stop
	std::vector<ThreeDVehicleWheels> wheels; ///< "kola"
	std::vector<ThreeDVehicleCrew> crew;     ///< "posadka": never fading, lit as the vehicle
	std::string frame_prefix;       ///< a figure that moves ("animace"): its frames are the nodes whose names start so ("uzly"), followed by their number; empty for none
	int frames = 0;                 ///< "snimku": how many frames
	int frame_ticks = 1;            ///< "tiku": game ticks a frame is held
	float walk_step = 0;            ///< "chuze": "krok_m", metres a round of its frames walks; 0 when it does not walk
	bool loop = true;               ///< "smycka": the frames go round; false, they run once and stay on the last (a creature's attack, its death)
	int spin_ticks = 0;             ///< a thing that turns ("predmet" with "otaceni", the spit of the pub): game ticks a turn ("tiku_na_otacku"), 0 for none
	float spin_axis[3] = {1, 0, 0}; ///< "osa": the axis it turns about, in its frame
	float spin_centre[3] = {};      ///< "stred": a point of the axis, metres
	std::string tint_material;      ///< an effect's "barvy": the material the game colours ("material")
	std::vector<uint32_t> tint_palette; ///< and the colours it may be ("paleta", Colour::data)

	const ThreeDVehicleShape *GetShape() const;
	const ThreeDVehicleShape *GetShape(int frame) const;
	int FrameAt(uint64_t tick, uint32_t phase) const;
	const ThreeDObjectTexture *GetTexture(int level, int index = 0) const;
	const ThreeDObjectTexture *GetPaintMask(int level) const;
	const ThreeDObjectTexture *GetRustMask(int level) const;

	mutable std::shared_ptr<const ThreeDVehicleShape> shape; ///< read the first time it is drawn
	mutable bool shape_tried = false;
	mutable std::vector<std::shared_ptr<const ThreeDVehicleShape>> frame_shapes; ///< the frames of a figure that moves, read as drawn

private:
	std::shared_ptr<const ThreeDVehicleShape> MakeShape(int frame) const;
	mutable std::vector<std::vector<ThreeDObjectTexture>> levels; ///< the textures, each halved for each level of detail
	mutable std::vector<ThreeDObjectTexture> mask_levels;         ///< the paint mask likewise (its grey in the red)
	mutable bool mask_tried = false;
	mutable std::vector<ThreeDObjectTexture> rust_levels;         ///< the rust mask likewise
	mutable bool rust_tried = false;
};

const ThreeDVehicleObject *GetThreeDVehicleObject(EngineID engine, int *piece = nullptr);
const ThreeDVehicleObject *GetThreeDFigure(std::string_view name, std::string_view package = {});

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
const ThreeDVehicleObject *GetThreeDEffect(std::string_view name);
const std::vector<ThreeDVehicleObject> &GetThreeDDrivingCars();
void ResetThreeDVehicleObjects();
const ThreeDVehicleSize *GetThreeDVehicleSize(uint32_t grfid);
std::string DescribeThreeDVehicleObjects();

const ThreeDObject *GetThreeDHouseObject(HouseID north_house, uint colour_slot, uint8_t theme = 0);
const ThreeDObject *GetThreeDIndustryObject(IndustryType type, bool variant);
const ThreeDObject *GetThreeDFieldPart(std::string_view part);
const ThreeDObject *GetThreeDObjectByName(std::string_view name);

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
/** A texture of an object wanted for drawing: the object, the level of detail and the paint (PrepareThreeDObjectTextures()). */
struct ThreeDTextureWanted {
	const ThreeDObject *obj;
	int level;
	uint32_t paint;
};
void PrepareThreeDObjectTextures(std::span<const ThreeDTextureWanted> wanted);
std::span<const std::string> GetThreeDThemeTrees(uint8_t theme);
std::optional<std::string> GetThreeDHouseTitle(TileIndex tile);
std::string DescribeThreeDObjects();

#endif /* THREE_D_OBJECTS_H */
