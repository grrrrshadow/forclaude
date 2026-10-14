/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file three_d_vehicle.cpp The 3D likeness of a vehicle made from its eight
 * sprites (ThreeDVehicleModel, three_d.h), for the preview beside the
 * purchase window that turns smoothly (three_d_preview_gui.cpp). The
 * buildings of this game come with renders that carry depth; a vehicle has
 * only its eight sprites, so its depth is made here, from the eight outlines,
 * the way it was tried outside the game on Sergej (M62) and on figures and
 * settled with the player:
 *
 * 1. The eight views: the sprites of every part of the vehicle (articulated
 *    parts, the two heads of a double-headed engine) for a direction, laid
 *    together as the map lays them (Train::UpdateDeltaXY(),
 *    Train::CalcNextVehicleOffset()), at the finest zoom the sprites have, in
 *    the company's colours. The anchor is the track point under the middle
 *    of the chain.
 * 2. The camera of the sprites: orthographic, 30 degrees above the horizon,
 *    48.8 pixels to a metre at 16x; direction d has the vehicle turned
 *    90 - 45 d degrees, and in the four directions along the track its
 *    sprite is foreshortened by 1/sqrt 2, as the sets draw vehicles.
 * 3. The visual hull: a grid of voxels (a pixel each), cut away wherever a
 *    voxel falls outside the outline in any of the eight views (outline:
 *    alpha 40 and more). The anchor of each view is first moved by up to a
 *    few pixels to where the hull's outline fits the sprite's best.
 * 4. The surface of the hull, each point with the colour the sprite gives it
 *    in every view that sees it (a z-buffer), weighted by how squarely the
 *    camera looks at it.
 * 5. A picture at any angle: the points, turned, drawn as 2x2 dots from the
 *    farthest in, at twice the size and shrunk with their alpha, so the
 *    outline is soft as a sprite's; the colour of a point is a mix of the
 *    two sprites nearest the angle, so in a sprite's own direction it is the
 *    sprite's pixels, and between two there is no jump.
 */

#include "stdafx.h"
#include "three_d.h"
#include "articulated_vehicles.h"
#include "engine_base.h"
#include "engine_gui.h"
#include "newgrf_engine.h"
#include "newgrf_properties.h"
#include "newgrf_callbacks.h"
#include "spritecache.h"
#include "spriteloader/spriteloader.hpp"
#include "landscape.h"
#include "zoom_func.h"
#include "palette_func.h"
#include "vehicle_type.h"
#include "vehicle_base.h"
#include "vehicle_func.h"
#include "train.h"
#include "roadveh.h"
#include "viewport_func.h"
#include "direction_func.h"
#include "fileio_type.h"
#include "core/math_func.hpp"
#include "core/backup_type.hpp"
#include "debug.h"

#include "table/sprites.h"

#include <chrono>
#include <cmath>
#include <map>

#include "safeguards.h"

/** Pixels to a metre of a sprite at 16x: the sets of this game draw a metre as 48.8 pixels there (24.4 at 8x). */
static constexpr float PXM_16X = 48.8f;
/** The camera of the sprites: 30 degrees above the horizon. */
static constexpr float CAM_SIN = 0.5f;
static constexpr float CAM_COS = 0.86602540f;
/** A pixel is part of the outline from this alpha on (thin parts were cut off at 128). */
static constexpr uint8_t OUTLINE_ALPHA = 40;
/** The outline the hull is fitted to when the views are aligned: the firmer pixels. */
static constexpr uint8_t FIT_ALPHA = 128;
/** The voxel grid is kept to about this many voxels. */
static constexpr double MAX_VOXELS = 3e6;
/** How far a view's anchor may be moved to fit, pixels. */
static constexpr int FIT_SHIFT = 16;
/** The pictures are drawn at this many times the size and shrunk. */
static constexpr int SUPERSAMPLE = 2;
/** The models kept: the last engines looked at, up to this many surface points in all (about 48 bytes a point). */
static constexpr size_t MODEL_POINTS_KEPT = 2000000;
/** The pictures kept (GetThreeDVehiclePictureAt()): the vehicles on the turned map ask for the same few over and over. */
static constexpr size_t PICTURES_KEPT = 2048;
/** How finely the angle of a cached picture is counted, degrees. */
static constexpr float PICTURE_ANGLE_STEP = 1.0f;

/** One of the eight views of a vehicle: its sprites of a direction composed as the map draws them, about the anchor. */
struct ThreeDVehicleView {
	int width = 0;
	int height = 0;
	int anchor_x = 0; ///< the track point under the middle of the chain, pixels from the left
	int anchor_y = 0; ///< and from the top
	int shift_x = 0;  ///< the move of the anchor that fits the hull to the sprite (BuildVehicleModel())
	int shift_y = 0;
	std::vector<Colour> pixels;

	Colour At(int x, int y) const
	{
		if (x < 0 || y < 0 || x >= this->width || y >= this->height) return Colour(0, 0, 0, 0);
		return this->pixels[static_cast<size_t>(y) * this->width + x];
	}
};

/** The 3D likeness of a vehicle: the surface points of its hull with their colours in every view. */
struct ThreeDVehicleModel {
	EngineID engine = EngineID::Invalid();
	PaletteID pal = PAL_NONE;
	ZoomLevel zoom = ZoomLevel::Normal; ///< the views' zoom
	float pxm = 0.0f;                   ///< pixels to a metre at that zoom
	float vox = 0.0f;                   ///< the voxel, metres
	bool squeeze = true;                ///< the sprites of the directions along the track are foreshortened 1/sqrt 2
	size_t count = 0;                   ///< surface points
	std::vector<float> xyz;             ///< three to a point, metres: x forward, y to the left, z up, the anchor at the origin
	std::vector<uint8_t> col8;          ///< r, g, b of the point in each of the eight views, 24 to a point
	std::vector<uint8_t> w8;            ///< the weight of each view's colour, eight to a point (0: the view does not see the point)
	std::vector<Colour> fallback;       ///< the colour where neither of the two views nearest the angle sees the point
	std::vector<uint8_t> rank;          ///< a point's place in a random order, 0..255: a coarser zoom draws the points of the lower ranks only
	std::map<ZoomLevel, Rect> frames;   ///< the union of the picture's rectangles over the angles, about the anchor, by zoom (ThreeDVehicleFrame())
	std::vector<float> part_x;          ///< where each part's position lies along x, metres, front first: a part is a piece with a picture and the pieces without one that go with it (the cab painted on the piece behind it); one for a vehicle of one part
	std::vector<float> part_cut;        ///< the x where part i ends and part i+1 begins, one fewer than the parts
	std::vector<int8_t> piece_part;     ///< the part each piece of the chain belongs to, front first: the pieces of an articulated vehicle, the two heads of a double-headed engine
	std::vector<int8_t> part_piece;     ///< the piece each part is hung from: the one with the picture
	uint64_t look = 0;                  ///< the look of the vehicle the model was made from (VehicleLook()): 0 for one made from the engine's own pictures (the purchase window)
	int build_ms = 0;
	int voxels = 0;
	std::string note;                   ///< what the rig says about it
};

static std::vector<std::unique_ptr<ThreeDVehicleModel>> _vehicle_models;

/** A picture of a vehicle's likeness kept for the map (GetThreeDVehiclePictureAt()). */
struct VehiclePictureEntry {
	const ThreeDVehicleModel *model;
	uint64_t used;
	ThreeDModelPicture picture;
};
static std::map<uint64_t, VehiclePictureEntry> _vehicle_pictures;
static uint64_t _vehicle_pictures_clock = 0;

/** Forget the pictures of a model that goes. */
static void ForgetVehiclePictures(const ThreeDVehicleModel *model)
{
	for (auto it = _vehicle_pictures.begin(); it != _vehicle_pictures.end();) {
		if (it->second.model == model) {
			it = _vehicle_pictures.erase(it);
		} else {
			++it;
		}
	}
}

/** The game's unit step of a direction (Direction: N, NE, E, SE, S, SW, W, NW), world units. */
static const int8_t DIR_DX[8] = {-1, -1, -1, 0, 1, 1, 1, 0};
static const int8_t DIR_DY[8] = {-1, 0, 1, 1, 1, 0, -1, -1};

/** The angle the vehicle is turned by in a view, degrees: the front points along +x of the model at 90. */
static inline float ViewAzimuth(int d)
{
	return 90.0f - 45.0f * d;
}

/**
 * Where a part's sprite goes against the part's position: bounds.origin plus
 * bounds.offset of the vehicle's UpdateDeltaXY() for a part of a length, world units.
 */
static Point SpriteAnchor(VehicleType type, int length, Direction dir)
{
	int d = to_underlying(dir);
	switch (type) {
		case VehicleType::Train:
			if (!IsDiagonalDirection(dir)) {
				/* N, E, S, W: the box moved to the middle of the vehicle. */
				static const int8_t sx[8] = {-1, 0, -1, 0, 1, 0, 1, 0};
				static const int8_t sy[8] = {-1, 0, 1, 0, 1, 0, -1, 0};
				int hs = (static_cast<int>(VEHICLE_LENGTH) - length) / 2;
				return {-1 - hs * sx[d], -1 - hs * sy[d]};
			}
			switch (dir) {
				case Direction::NE: return {-(length + 1) / 2 + 1, -1};
				case Direction::NW: return {-1, -(length + 1) / 2 + 1};
				case Direction::SW: return {-length / 2 + 1 - (static_cast<int>(VEHICLE_LENGTH) - length), -1};
				default:            return {-1, -length / 2 + 1 - (static_cast<int>(VEHICLE_LENGTH) - length)}; // SE
			}

		case VehicleType::Road:
			/* RoadVehicle::UpdateDeltaXY(): the box of a whole vehicle, whatever the length. */
			switch (dir) {
				case Direction::NE: case Direction::SW: return {-2, -1};
				case Direction::NW: case Direction::SE: return {-1, -2};
				default: return {-1, -1};
			}

		case VehicleType::Ship: {
			static const Point bounds[8] = {{-3, -3}, {-16, -3}, {-3, -3}, {-3, -16}, {-3, -3}, {-16, -3}, {-3, -3}, {-3, -16}};
			return bounds[d];
		}

		default:
			return {-1, -1};
	}
}

/** A part of a vehicle as the purchase window knows it: its engine, its length, and whether its sprite faces the other way. */
struct VehiclePart {
	EngineID engine;
	bool mirrored; ///< an articulated part turned round
	bool rear;     ///< the rear head of a double-headed engine
	int length;    ///< eighths of a tile
	const Vehicle *veh = nullptr; ///< the piece itself when the model is made from a vehicle on the map: its sprites are the vehicle's own
};

/** The length of an engine's part for the purchase window, eighths of a tile: its shortening property or callback. */
static int PartLength(const Engine *e)
{
	int shorten = 0;
	if (e->type == VehicleType::Train) {
		shorten = e->VehInfo<RailVehicleInfo>().shorten_factor;
		if (e->info.callback_mask.Test(VehicleCallbackMask::Length)) {
			uint16_t cb = GetVehicleCallback(CBID_VEHICLE_LENGTH, 0, 0, e->index, nullptr);
			if (cb != CALLBACK_FAILED && cb < VEHICLE_LENGTH) shorten = cb;
		}
		shorten = GetEngineProperty(e->index, PROP_TRAIN_SHORTEN_FACTOR, shorten);
	} else if (e->type == VehicleType::Road) {
		shorten = e->VehInfo<RoadVehicleInfo>().shorten_factor;
		if (e->info.callback_mask.Test(VehicleCallbackMask::Length)) {
			uint16_t cb = GetVehicleCallback(CBID_VEHICLE_LENGTH, 0, 0, e->index, nullptr);
			if (cb != CALLBACK_FAILED && cb < VEHICLE_LENGTH) shorten = cb;
		}
		shorten = GetEngineProperty(e->index, PROP_ROADVEH_SHORTEN_FACTOR, shorten);
	}
	return static_cast<int>(VEHICLE_LENGTH) - Clamp(shorten, 0, static_cast<int>(VEHICLE_LENGTH) - 1);
}

/** The parts of an engine, front first. */
static std::vector<VehiclePart> VehicleParts(EngineID engine)
{
	const Engine *e = Engine::Get(engine);
	std::vector<VehiclePart> parts;
	parts.push_back({engine, false, false, PartLength(e)});
	if (e->type == VehicleType::Train && e->VehInfo<RailVehicleInfo>().railveh_type == RailVehicleType::Multihead) {
		parts.push_back({engine, false, true, PartLength(e)});
		return parts;
	}
	std::vector<bool> mirrored;
	std::vector<EngineID> engines = GetArticulatedPartEngines(engine, &mirrored);
	for (size_t i = 0; i < engines.size(); i++) {
		if (!Engine::IsValidID(engines[i])) continue;
		parts.push_back({engines[i], i < mirrored.size() && mirrored[i], false, PartLength(Engine::Get(engines[i]))});
	}
	return parts;
}

/** The length of a piece on the map, eighths of a tile. */
static int PieceLength(const Vehicle *u)
{
	switch (u->type) {
		case VehicleType::Train: return Train::From(u)->gcache.cached_veh_length;
		case VehicleType::Road:  return RoadVehicle::From(u)->gcache.cached_veh_length;
		default:                 return static_cast<int>(VEHICLE_LENGTH);
	}
}

/**
 * The pieces of a vehicle on the map, front first: the vehicle and its articulated parts, and the rear head
 * of a double-headed engine. Each piece's sprites are its own (Vehicle::GetImage()), the way the map draws
 * it: a set picks a vehicle's pictures by what it carries, its age, its livery, which the engine's own
 * pictures (the purchase window's) know nothing of.
 * @param v the front piece
 */
static std::vector<VehiclePart> VehiclePiecesOf(const Vehicle *v)
{
	std::vector<VehiclePart> parts;
	for (const Vehicle *u = v; u != nullptr; u = u->HasArticulatedPart() ? u->GetNextArticulatedPart() : nullptr) {
		parts.push_back({u->engine_type, false, false, PieceLength(u), u});
	}
	if (v->type == VehicleType::Train && Train::From(v)->IsMultiheaded() && Train::From(v)->other_multiheaded_part != nullptr) {
		const Train *rear = Train::From(v)->other_multiheaded_part;
		parts.push_back({rear->engine_type, false, true, PieceLength(rear), rear});
	}
	return parts;
}

/**
 * What a vehicle on the map looks like, as a number: its pieces' sprites of two directions, so two vehicles of
 * one engine that a set draws alike share a model and two it draws differently (a load, a livery) do not. The
 * set is told the vehicle faces the direction asked (_resolve_vehicle_direction), as when the views are composed,
 * so the look does not change as the vehicle turns.
 */
static uint64_t VehicleLook(const Vehicle *v)
{
	uint64_t h = 1469598103934665603ULL;
	auto mix = [&h](uint64_t x) { h = (h ^ x) * 1099511628211ULL; };
	for (const VehiclePart &part : VehiclePiecesOf(v)) {
		for (Direction d : {Direction::NE, Direction::E}) {
			VehicleSpriteSeq seq;
			AutoRestoreBackup facing(_resolve_vehicle_direction, d);
			part.veh->GetImage(d, EngineImageType::OnMap, &seq);
			mix(seq.count);
			for (uint k = 0; k < seq.count; k++) {
				mix(seq.seq[k].sprite);
				mix(seq.seq[k].pal);
			}
		}
	}
	return h == 0 ? 1 : h;
}

/**
 * The pieces along the chain, in steps of the direction, the first at 0, each next one behind as the game
 * spaces them: a train's by Train::CalcNextVehicleOffset() (half the length of the one before and half, rounded
 * up, of its own, the pieces being centred on their positions), a road vehicle's by the whole length of the one
 * before (the next part leaves the shed when the one before has gone its length, IndividualRoadVehicleController()),
 * so a piece of a road vehicle reaches from its position back to the next piece's.
 * @return the middle of the first and the last, the anchor
 */
static double PartPositions(VehicleType type, const std::vector<VehiclePart> &parts, std::vector<double> &pos)
{
	pos.assign(parts.size(), 0.0);
	for (size_t i = 1; i < parts.size(); i++) {
		int behind = type == VehicleType::Road ? parts[i - 1].length : parts[i - 1].length / 2 + (parts[i].length + 1) / 2;
		pos[i] = pos[i - 1] - behind;
	}
	return (pos.front() + pos.back()) / 2.0;
}

/** Where a piece ends along the chain, behind its position: a train's half its length (rounded down), a road vehicle's its whole length. */
static int PieceEnd(VehicleType type, const VehiclePart &part)
{
	return type == VehicleType::Road ? part.length : part.length / 2;
}

/** A piece's position along the chain, in steps from the anchor, whole: all the pieces rounded the same way, so their spacing stays the game's. */
static inline int PieceAlong(double pos, double middle)
{
	return static_cast<int>(std::floor(pos - middle + 0.5));
}

/**
 * The sprites of a part for a direction, as the map draws it. The set is told the vehicle faces that direction
 * and runs straight (_resolve_vehicle_direction): a set that places a layer by the direction the vehicle really
 * has -- the player's lorry set shifts the cab so -- gave the picture of one direction the shift of another.
 */
static bool PartSprites(const VehiclePart &part, VehicleType type, Direction dir, VehicleSpriteSeq &seq)
{
	seq.Clear();
	AutoRestoreBackup facing(_resolve_vehicle_direction, dir);
	if (part.veh != nullptr) {
		/* A piece on the map: its own sprites, as the map draws them (a flipped engine's, a mirror piece's, the rear head's: all inside). */
		part.veh->GetImage(dir, EngineImageType::OnMap, &seq);
		return seq.IsValid();
	}
	Direction d = (part.mirrored || part.rear) ? ReverseDir(dir) : dir;
	int pitch = 0;
	switch (type) {
		case VehicleType::Train:    GetTrainIcon(part.engine, part.rear, d, pitch, EngineImageType::OnMap, &seq); break;
		case VehicleType::Road:     GetRoadVehIcon(part.engine, d, EngineImageType::OnMap, &seq); break;
		case VehicleType::Ship:     GetShipIcon(part.engine, d, EngineImageType::OnMap, &seq); break;
		case VehicleType::Aircraft: GetAircraftIcon(part.engine, d, EngineImageType::OnMap, &seq); break;
		default: return false;
	}
	return seq.IsValid();
}

/** A pixel of a sprite as the blitter would draw it: the company's colours put in, the palette looked up. */
static inline Colour PixelColour(const SpriteLoader::CommonPixel &px, bool is_32bpp, const uint8_t *remap)
{
	if (!is_32bpp) {
		if (px.m == 0) return Colour(0, 0, 0, 0);
		Colour c = _cur_palette.palette[remap != nullptr ? remap[px.m] : px.m];
		c.a = 255;
		return c;
	}
	if (px.m == 0) return Colour(px.r, px.g, px.b, px.a);
	/* A pixel with a colour index: the palette's colour of that index, or the remapped one, at the brightness
	 * of its r, g, b -- as Blitter_32bppOptimized::Encode() pre-converts it (without the remap the index's own
	 * colour, not the grey the r, g, b spell). */
	uint8_t brightness = std::max({px.r, px.g, px.b});
	if (brightness == 0) brightness = DEFAULT_BRIGHTNESS;
	Colour c = AdjustBrightness(_cur_palette.palette[remap != nullptr ? remap[px.m] : px.m], brightness);
	c.a = px.a;
	return c;
}

/** A sprite's pixels at its finest zoom, with its offsets at that zoom. */
struct RawPiece {
	ZoomLevel zoom;
	int width = 0, height = 0;
	int x_offs = 0, y_offs = 0;
	std::vector<Colour> pixels;
	int world_x = 0, world_y = 0; ///< where it goes, world units from the anchor
	int order = 0;                ///< drawn in this order: the farther first
};

/** Shrink a piece's pixels to a coarser zoom, 2x2 means weighted by alpha, as the game makes its coarser levels. */
static void ShrinkPiece(RawPiece &piece, ZoomLevel to)
{
	while (piece.zoom < to) {
		int w = (piece.width + 1) / 2, h = (piece.height + 1) / 2;
		std::vector<Colour> out(static_cast<size_t>(w) * h);
		for (int y = 0; y < h; y++) {
			for (int x = 0; x < w; x++) {
				uint r = 0, g = 0, b = 0, a = 0, n = 0;
				for (int dy = 0; dy < 2; dy++) {
					for (int dx = 0; dx < 2; dx++) {
						int sx = 2 * x + dx, sy = 2 * y + dy;
						if (sx >= piece.width || sy >= piece.height) continue;
						Colour c = piece.pixels[static_cast<size_t>(sy) * piece.width + sx];
						r += c.r * c.a; g += c.g * c.a; b += c.b * c.a; a += c.a; n++;
					}
				}
				out[static_cast<size_t>(y) * w + x] = a == 0 ? Colour(0, 0, 0, 0) : Colour(r / a, g / a, b / a, a / std::max(n, 1u));
			}
		}
		piece.pixels = std::move(out);
		piece.width = w;
		piece.height = h;
		piece.x_offs = piece.x_offs >= 0 ? piece.x_offs / 2 : -((-piece.x_offs + 1) / 2);
		piece.y_offs = piece.y_offs >= 0 ? piece.y_offs / 2 : -((-piece.y_offs + 1) / 2);
		++piece.zoom;
	}
}

/**
 * Compose the eight views of an engine: every part's sprite of the direction at the finest zoom they all have.
 * @param[out] has_picture which of the parts have a picture at all (a set builds a vehicle of pieces and paints some of them on others)
 * @return whether there is a picture in every direction
 */
static bool ComposeVehicleViews(EngineID engine, PaletteID pal, std::array<ThreeDVehicleView, 8> &views, ZoomLevel &zoom, std::string &note, const Vehicle *veh, std::vector<bool> &has_picture)
{
	const Engine *e = Engine::Get(engine);
	std::vector<VehiclePart> parts = veh != nullptr ? VehiclePiecesOf(veh) : VehicleParts(engine);
	has_picture.assign(parts.size(), false);

	/* The parts along the chain; the middle at the anchor. */
	std::vector<double> pos;
	double middle = PartPositions(e->type, parts, pos);

	const uint8_t *remap_engine = pal == PAL_NONE ? nullptr : GetNonSprite(GB(pal, 0, PALETTE_WIDTH), SpriteType::Recolour) + 1;

	std::array<std::vector<RawPiece>, 8> pieces;
	zoom = ZoomLevel::Min;
	for (int d = 0; d < 8; d++) {
		Direction dir = static_cast<Direction>(d);
		for (size_t i = 0; i < parts.size(); i++) {
			VehicleSpriteSeq seq;
			if (!PartSprites(parts[i], e->type, dir, seq)) continue;
			/* Where the part stands, in whole steps. */
			int along = PieceAlong(pos[i], middle);
			Point anchor = SpriteAnchor(e->type, parts[i].length, dir);
			int wx = along * DIR_DX[d] + anchor.x, wy = along * DIR_DY[d] + anchor.y;
			for (uint k = 0; k < seq.count; k++) {
				SpriteLoader::SpriteCollection collection;
				bool is_32bpp;
				ZoomLevels levels = ReadRawSprite(seq.seq[k].sprite & SPRITE_MASK, collection, is_32bpp);
				if (levels.None()) continue;
				ZoomLevel finest = ZoomLevel::End;
				for (ZoomLevel z : levels) {
					finest = z;
					break;
				}
				if (finest == ZoomLevel::End) continue;
				const SpriteLoader::Sprite &s = collection[finest];
				if (s.width <= 4 && s.height <= 4) continue; // the empty sprite a set gives a direction it has no picture for
				PaletteID p = (pal == PALETTE_CRASH || seq.seq[k].pal == PAL_NONE) ? pal : seq.seq[k].pal;
				const uint8_t *remap = p == pal ? remap_engine : (p == PAL_NONE ? nullptr : GetNonSprite(GB(p, 0, PALETTE_WIDTH), SpriteType::Recolour) + 1);
				RawPiece piece;
				piece.zoom = finest;
				piece.width = s.width;
				piece.height = s.height;
				piece.x_offs = s.x_offs;
				piece.y_offs = s.y_offs;
				piece.pixels.resize(static_cast<size_t>(s.width) * s.height);
				for (size_t j = 0; j < piece.pixels.size(); j++) piece.pixels[j] = PixelColour(s.data[j], is_32bpp, remap);
				if (d == 2) {
					/* The rig: what the pieces of the E view are made of (the colours of a set's vehicle go wrong here first). */
					size_t with_m = 0;
					for (size_t j = 0; j < piece.pixels.size(); j++) if (s.data[j].m != 0 && s.data[j].a != 0) with_m++;
					note += fmt::format("E piece {} sprite {} {}x{} {} seq pal {:#x} pal {:#x} remap {} m-pixels {}; ", i, seq.seq[k].sprite & SPRITE_MASK, s.width, s.height, is_32bpp ? "32bpp" : "8bpp",
							seq.seq[k].pal, pal, remap != nullptr ? "yes" : "no", with_m);
					/* The first few pixels with a colour index, from the middle row: what they are made of. */
					int shown = 0;
					for (int xx = 0; xx < s.width && shown < 3; xx += 7) {
						const SpriteLoader::CommonPixel &sp = s.data[static_cast<size_t>(s.height / 2) * s.width + xx];
						if (sp.m == 0 || sp.a == 0) continue;
						uint8_t idx = remap != nullptr ? remap[sp.m] : sp.m;
						Colour base = _cur_palette.palette[idx];
						Colour got = piece.pixels[static_cast<size_t>(s.height / 2) * s.width + xx];
						note += fmt::format("px {} rgba {},{},{},{} m {:#x} -> idx {:#x} base {},{},{} got {},{},{}; ", xx, sp.r, sp.g, sp.b, sp.a, sp.m, idx, base.r, base.g, base.b, got.r, got.g, got.b);
						shown++;
					}
				}
				piece.world_x = wx;
				piece.world_y = wy;
				piece.order = wx + wy;
				if (finest > zoom) zoom = finest;
				pieces[d].push_back(std::move(piece));
				has_picture[i] = true;
			}
		}
		if (pieces[d].empty()) {
			note = fmt::format("no picture for direction {}", d);
			return false;
		}
	}

	/* Lay them on a canvas, the farther first, about the anchor. */
	for (int d = 0; d < 8; d++) {
		std::sort(pieces[d].begin(), pieces[d].end(), [](const RawPiece &a, const RawPiece &b) { return a.order < b.order; });
		int left = INT32_MAX, top = INT32_MAX, right = INT32_MIN, bottom = INT32_MIN;
		std::vector<Point> at;
		for (RawPiece &piece : pieces[d]) {
			ShrinkPiece(piece, zoom);
			Point screen = RemapCoords(piece.world_x, piece.world_y, 0);
			int px = UnScaleByZoom(screen.x, zoom) + piece.x_offs, py = UnScaleByZoom(screen.y, zoom) + piece.y_offs;
			at.push_back({px, py});
			left = std::min(left, px);
			top = std::min(top, py);
			right = std::max(right, px + piece.width);
			bottom = std::max(bottom, py + piece.height);
		}
		ThreeDVehicleView &v = views[d];
		v.width = right - left;
		v.height = bottom - top;
		v.anchor_x = -left;
		v.anchor_y = -top;
		v.pixels.assign(static_cast<size_t>(v.width) * v.height, Colour(0, 0, 0, 0));
		for (size_t i = 0; i < pieces[d].size(); i++) {
			const RawPiece &piece = pieces[d][i];
			for (int y = 0; y < piece.height; y++) {
				for (int x = 0; x < piece.width; x++) {
					Colour s = piece.pixels[static_cast<size_t>(y) * piece.width + x];
					if (s.a == 0) continue;
					Colour &dst = v.pixels[static_cast<size_t>(at[i].y - top + y) * v.width + (at[i].x - left + x)];
					if (s.a == 255 || dst.a == 0) {
						dst = s;
					} else {
						uint al = s.a, na = 255 - al;
						dst = Colour((s.r * al + dst.r * na + 127) / 255, (s.g * al + dst.g * na + 127) / 255, (s.b * al + dst.b * na + 127) / 255, std::max(s.a, dst.a));
					}
				}
			}
		}
	}
	return true;
}

/** The projection of a view: the vehicle turned and foreshortened, the camera 30 degrees up. */
struct ViewCamera {
	float c, s;  ///< cosine and sine of the azimuth
	float sq;    ///< the foreshortening along the vehicle
	float cam[3]; ///< the direction to the camera in the model's frame

	void Set(int d, bool squeeze)
	{
		float f = ViewAzimuth(d) * static_cast<float>(M_PI) / 180.0f;
		this->c = std::cos(f);
		this->s = std::sin(f);
		this->sq = (squeeze && (d & 1)) ? 0.70710678f : 1.0f;
		this->cam[0] = -this->s * CAM_COS;
		this->cam[1] = -this->c * CAM_COS;
		this->cam[2] = CAM_SIN;
	}

	/** A model point to the view's pixel about the anchor (x right, y down) and its depth (the smaller the nearer). */
	inline void Project(float x, float y, float z, float pxm, float &sx, float &sy, float &depth) const
	{
		float px = x * this->sq;
		float xr = px * this->c - y * this->s;
		float yr = px * this->s + y * this->c;
		sx = xr * pxm;
		sy = (-yr * CAM_SIN - z * CAM_COS) * pxm;
		depth = yr * CAM_COS - z * CAM_SIN;
	}
};

/** The voxel grid of the hull. */
struct VoxelGrid {
	int nx, ny, nz;
	float x0, y0, z0, vox;
	std::vector<uint8_t> occ;

	inline size_t Index(int x, int y, int z) const { return (static_cast<size_t>(z) * this->ny + y) * this->nx + x; }
	inline bool At(int x, int y, int z) const
	{
		if (x < 0 || y < 0 || z < 0 || x >= this->nx || y >= this->ny || z >= this->nz) return false;
		return this->occ[this->Index(x, y, z)] != 0;
	}
	inline float X(int i) const { return this->x0 + (i + 0.5f) * this->vox; }
	inline float Y(int i) const { return this->y0 + (i + 0.5f) * this->vox; }
	inline float Z(int i) const { return this->z0 + (i + 0.5f) * this->vox; }
};

/** A view's outline widened by r pixels (a box), so a sprite a few pixels off the others cuts nothing it should not. */
static std::vector<uint8_t> WidenedOutline(const ThreeDVehicleView &v, int r)
{
	std::vector<uint8_t> a(static_cast<size_t>(v.width) * v.height, 0), b(a.size(), 0);
	for (size_t i = 0; i < a.size(); i++) a[i] = v.pixels[i].a >= OUTLINE_ALPHA;
	for (int y = 0; y < v.height; y++) {
		for (int x = 0; x < v.width; x++) {
			uint8_t any = 0;
			for (int dx = -r; dx <= r && !any; dx++) {
				int xx = x + dx;
				if (xx >= 0 && xx < v.width) any = a[static_cast<size_t>(y) * v.width + xx];
			}
			b[static_cast<size_t>(y) * v.width + x] = any;
		}
	}
	for (int y = 0; y < v.height; y++) {
		for (int x = 0; x < v.width; x++) {
			uint8_t any = 0;
			for (int dy = -r; dy <= r && !any; dy++) {
				int yy = y + dy;
				if (yy >= 0 && yy < v.height) any = b[static_cast<size_t>(yy) * v.width + x];
			}
			a[static_cast<size_t>(y) * v.width + x] = any;
		}
	}
	return a;
}

/**
 * Cut the grid to the hull: a voxel stays where every view has it inside the outline.
 * @param widened the views' outlines widened (WidenedOutline()), or nullptr for the outlines as they are
 */
static void CarveHull(VoxelGrid &g, const std::array<ThreeDVehicleView, 8> &views, const std::array<ViewCamera, 8> &cams, float pxm, const std::array<std::vector<uint8_t>, 8> *widened)
{
	g.occ.assign(static_cast<size_t>(g.nx) * g.ny * g.nz, 1);
	for (int d = 0; d < 8; d++) {
		const ThreeDVehicleView &v = views[d];
		const ViewCamera &cam = cams[d];
		const std::vector<uint8_t> *wide = widened != nullptr ? &(*widened)[d] : nullptr;
		float ax = v.anchor_x + v.shift_x + 0.5f, ay = v.anchor_y + v.shift_y + 0.5f;
		for (int iz = 0; iz < g.nz; iz++) {
			float z = g.Z(iz);
			for (int iy = 0; iy < g.ny; iy++) {
				float y = g.Y(iy);
				uint8_t *row = &g.occ[g.Index(0, iy, iz)];
				for (int ix = 0; ix < g.nx; ix++) {
					if (row[ix] == 0) continue;
					float sx, sy, depth;
					cam.Project(g.X(ix), y, z, pxm, sx, sy, depth);
					int px = static_cast<int>(std::floor(ax + sx)), py = static_cast<int>(std::floor(ay + sy));
					bool inside;
					if (wide != nullptr) {
						inside = px >= 0 && py >= 0 && px < v.width && py < v.height && (*wide)[static_cast<size_t>(py) * v.width + px] != 0;
					} else {
						inside = v.At(px, py).a >= OUTLINE_ALPHA;
					}
					if (!inside) row[ix] = 0;
				}
			}
		}
	}
}

/** The outline of the hull as a view sees it, closed by a pixel, to fit it to the sprite. */
static std::vector<uint8_t> HullOutline(const VoxelGrid &g, const ThreeDVehicleView &v, const ViewCamera &cam, float pxm)
{
	std::vector<uint8_t> mask(static_cast<size_t>(v.width) * v.height, 0);
	float ax = v.anchor_x + 0.5f, ay = v.anchor_y + 0.5f;
	for (int iz = 0; iz < g.nz; iz++) {
		for (int iy = 0; iy < g.ny; iy++) {
			const uint8_t *row = &g.occ[g.Index(0, iy, iz)];
			for (int ix = 0; ix < g.nx; ix++) {
				if (row[ix] == 0) continue;
				float sx, sy, depth;
				cam.Project(g.X(ix), g.Y(iy), g.Z(iz), pxm, sx, sy, depth);
				int px = static_cast<int>(std::floor(ax + sx)), py = static_cast<int>(std::floor(ay + sy));
				if (px >= 0 && py >= 0 && px < v.width && py < v.height) mask[static_cast<size_t>(py) * v.width + px] = 1;
			}
		}
	}
	/* Closing: dilate, then erode, 3x3. */
	auto pass = [&](const std::vector<uint8_t> &in, bool grow) {
		std::vector<uint8_t> out(in.size(), 0);
		for (int y = 0; y < v.height; y++) {
			for (int x = 0; x < v.width; x++) {
				bool any = false, all = true;
				for (int dy = -1; dy <= 1; dy++) {
					for (int dx = -1; dx <= 1; dx++) {
						int xx = x + dx, yy = y + dy;
						bool s = xx >= 0 && yy >= 0 && xx < v.width && yy < v.height && in[static_cast<size_t>(yy) * v.width + xx] != 0;
						any |= s;
						all &= s;
					}
				}
				out[static_cast<size_t>(y) * v.width + x] = grow ? any : all;
			}
		}
		return out;
	};
	return pass(pass(mask, true), false);
}

/** How well the hull's outline, moved by (tx, ty), covers the sprite's: intersection over union. */
static float OutlineFit(const std::vector<uint8_t> &mask, const ThreeDVehicleView &v, int tx, int ty)
{
	uint both = 0, either = 0;
	for (int y = 0; y < v.height; y++) {
		for (int x = 0; x < v.width; x++) {
			bool sprite = v.pixels[static_cast<size_t>(y) * v.width + x].a >= FIT_ALPHA;
			int mx = x - tx, my = y - ty;
			bool hull = mx >= 0 && my >= 0 && mx < v.width && my < v.height && mask[static_cast<size_t>(my) * v.width + mx] != 0;
			both += sprite && hull;
			either += sprite || hull;
		}
	}
	return either == 0 ? 0.0f : static_cast<float>(both) / either;
}

/** Erode and dilate the hull by a voxel (6 neighbours), taking off the specks a lone outline leaves. */
static void OpenHull(VoxelGrid &g)
{
	auto pass = [&](bool grow) {
		std::vector<uint8_t> out(g.occ.size(), 0);
		for (int z = 0; z < g.nz; z++) {
			for (int y = 0; y < g.ny; y++) {
				for (int x = 0; x < g.nx; x++) {
					bool c = g.At(x, y, z);
					bool n = g.At(x - 1, y, z) || g.At(x + 1, y, z) || g.At(x, y - 1, z) || g.At(x, y + 1, z) || g.At(x, y, z - 1) || g.At(x, y, z + 1);
					bool a = g.At(x - 1, y, z) && g.At(x + 1, y, z) && g.At(x, y - 1, z) && g.At(x, y + 1, z) && g.At(x, y, z - 1) && g.At(x, y, z + 1);
					out[g.Index(x, y, z)] = grow ? (c || n) : (c && a);
				}
			}
		}
		g.occ = std::move(out);
	};
	pass(false);
	pass(true);
}

/**
 * Build the likeness of an engine from its eight sprites.
 * @return the model, or nothing when the engine has no picture in some direction
 */
static std::unique_ptr<ThreeDVehicleModel> BuildVehicleModel(EngineID engine, PaletteID pal, const Vehicle *veh)
{
	auto t0 = std::chrono::steady_clock::now();
	auto model = std::make_unique<ThreeDVehicleModel>();
	model->engine = engine;
	model->pal = pal;
	model->look = veh != nullptr ? VehicleLook(veh) : 0;

	std::array<ThreeDVehicleView, 8> views;
	std::vector<bool> has_picture;
	if (!ComposeVehicleViews(engine, pal, views, model->zoom, model->note, veh, has_picture)) return nullptr;
	{
		/* Where the parts lie along the likeness, for the map to draw each at its own place (a curve): the chain's
		 * spacing in steps of the direction as the views were composed, a step of the E and W views being four
		 * pixels at the normal zoom. A part is a piece with a picture and the pieces without one that go with it:
		 * a set paints the cab on the piece behind the front one, the tail of a trailer over the pieces behind it,
		 * so a piece without a picture belongs to the piece with one before it, and the pieces before the first
		 * picture to that one. The likeness is cut where the last piece of a part ends (PieceEnd()). */
		const VehicleType type = Engine::Get(engine)->type;
		std::vector<VehiclePart> parts = veh != nullptr ? VehiclePiecesOf(veh) : VehicleParts(engine);
		std::vector<double> pos;
		double middle = PartPositions(type, parts, pos);
		const float step_m = 64.0f / PXM_16X;
		int current = -1;
		for (size_t i = 0; i < parts.size(); i++) {
			if (has_picture[i]) {
				current = static_cast<int>(model->part_x.size());
				model->part_x.push_back(PieceAlong(pos[i], middle) * step_m);
				model->part_piece.push_back(static_cast<int8_t>(i));
			}
			model->piece_part.push_back(static_cast<int8_t>(current));
		}
		for (int8_t &p : model->piece_part) if (p < 0) p = 0;
		for (size_t i = 0; i + 1 < parts.size(); i++) {
			const VehiclePart &part = parts[i];
			model->note += fmt::format("part {}{}{} len {}{}; ", part.engine.base(), part.mirrored ? " mirrored" : "", part.rear ? " rear" : "", part.length, has_picture[i] ? " picture" : "");
			if (model->piece_part[i] != model->piece_part[i + 1]) model->part_cut.push_back((PieceAlong(pos[i], middle) - PieceEnd(type, part)) * step_m);
		}
		const VehiclePart &last = parts.back();
		model->note += fmt::format("part {}{}{} len {}{}; ", last.engine.base(), last.mirrored ? " mirrored" : "", last.rear ? " rear" : "", last.length, has_picture.back() ? " picture" : "");
	}
	for (int d = 0; d < 8; d++) model->note += fmt::format("v{} {}x{} anchor {},{}; ", d, views[d].width, views[d].height, views[d].anchor_x, views[d].anchor_y);
	model->pxm = PXM_16X / (1 << to_underlying(model->zoom));
	const float pxm = model->pxm;

	std::array<ViewCamera, 8> cams;
	for (int d = 0; d < 8; d++) cams[d].Set(d, model->squeeze);

	/* The grid from the views' extents: the length from the views along the screen (E, W), the width from
	 * those along the depth (N, S), the height from the tallest. */
	auto extent = [&](int d, float &lo, float &hi, float &top, float &bottom) {
		const ThreeDVehicleView &v = views[d];
		int x0 = INT32_MAX, x1 = INT32_MIN, y0 = INT32_MAX, y1 = INT32_MIN;
		for (int y = 0; y < v.height; y++) {
			for (int x = 0; x < v.width; x++) {
				if (v.pixels[static_cast<size_t>(y) * v.width + x].a < OUTLINE_ALPHA) continue;
				x0 = std::min(x0, x); x1 = std::max(x1, x);
				y0 = std::min(y0, y); y1 = std::max(y1, y);
			}
		}
		if (x0 > x1) { lo = hi = top = bottom = 0.0f; return; }
		lo = (x0 - v.anchor_x) / pxm;
		hi = (x1 + 1 - v.anchor_x) / pxm;
		top = (v.anchor_y - y0) / pxm;
		bottom = (v.anchor_y - (y1 + 1)) / pxm;
	};
	float lo, hi, top, bottom;
	float xmin = 0, xmax = 0, ymin = 0, ymax = 0, zmax = 0, zmin = 0;
	extent(2, lo, hi, top, bottom); xmin = std::min(xmin, lo); xmax = std::max(xmax, hi); zmax = std::max(zmax, top); zmin = std::min(zmin, bottom);
	extent(6, lo, hi, top, bottom); xmin = std::min(xmin, -hi); xmax = std::max(xmax, -lo); zmax = std::max(zmax, top); zmin = std::min(zmin, bottom);
	extent(0, lo, hi, top, bottom); ymin = std::min(ymin, -hi); ymax = std::max(ymax, -lo); zmax = std::max(zmax, top); zmin = std::min(zmin, bottom);
	extent(4, lo, hi, top, bottom); ymin = std::min(ymin, lo); ymax = std::max(ymax, hi); zmax = std::max(zmax, top); zmin = std::min(zmin, bottom);
	for (int d = 1; d < 8; d += 2) { extent(d, lo, hi, top, bottom); zmax = std::max(zmax, top); zmin = std::min(zmin, bottom); }
	const float margin = 0.3f;
	VoxelGrid g;
	float x0 = xmin - margin, x1 = xmax + margin, y0 = ymin - margin, y1 = ymax + margin;
	float z0 = std::min(-0.2f, zmin / CAM_COS - 0.1f), z1 = zmax / CAM_COS + margin;
	double volume = static_cast<double>(x1 - x0) * (y1 - y0) * (z1 - z0);
	float vox = std::max(1.0f / pxm, static_cast<float>(std::cbrt(volume / MAX_VOXELS)));
	g.vox = vox;
	g.x0 = x0; g.y0 = y0; g.z0 = z0;
	g.nx = std::max(1, static_cast<int>(std::ceil((x1 - x0) / vox)));
	g.ny = std::max(1, static_cast<int>(std::ceil((y1 - y0) / vox)));
	g.nz = std::max(1, static_cast<int>(std::ceil((z1 - z0) / vox)));
	model->vox = vox;

	/* The extents of an outline, pixels about the anchor. */
	auto extents = [&](const std::vector<uint8_t> &mask, const ThreeDVehicleView &v, int &x0, int &y0, int &x1, int &y1) {
		x0 = INT32_MAX; y0 = INT32_MAX; x1 = INT32_MIN; y1 = INT32_MIN;
		for (int y = 0; y < v.height; y++) {
			for (int x = 0; x < v.width; x++) {
				if (mask[static_cast<size_t>(y) * v.width + x] == 0) continue;
				x0 = std::min(x0, x - v.anchor_x); x1 = std::max(x1, x - v.anchor_x);
				y0 = std::min(y0, y - v.anchor_y); y1 = std::max(y1, y - v.anchor_y);
			}
		}
	};

	/* First a loose hull, the outlines widened by FIT_SHIFT, so that a sprite that sits a few pixels off the
	 * others (the sets are drawn by hand; the player's set has its N and S pictures 16 pixels to the right of
	 * the rest at 16x) cuts nothing it should not; each view's anchor is then moved so that the middle of the
	 * hull's outline is at the middle of the sprite's. */
	std::array<std::vector<uint8_t>, 8> widened;
	for (int d = 0; d < 8; d++) widened[d] = WidenedOutline(views[d], FIT_SHIFT);
	CarveHull(g, views, cams, pxm, &widened);
	for (int d = 0; d < 8; d++) {
		ThreeDVehicleView &v = views[d];
		std::vector<uint8_t> mask = HullOutline(g, v, cams[d], pxm);
		std::vector<uint8_t> sprite(mask.size(), 0);
		for (size_t i = 0; i < sprite.size(); i++) sprite[i] = v.pixels[i].a >= FIT_ALPHA;
		int hx0, hy0, hx1, hy1, sx0, sy0, sx1, sy1;
		extents(mask, v, hx0, hy0, hx1, hy1);
		extents(sprite, v, sx0, sy0, sx1, sy1);
		if (hx0 > hx1 || sx0 > sx1) continue;
		v.shift_x = Clamp(((sx0 + sx1) - (hx0 + hx1)) / 2, -FIT_SHIFT, FIT_SHIFT);
		v.shift_y = Clamp(((sy0 + sy1) - (hy0 + hy1)) / 2, -FIT_SHIFT, FIT_SHIFT);
	}

	/* Then the hull from the outlines as they are, each anchor refined by a pixel or two to where the hull's
	 * outline covers the sprite's best. */
	CarveHull(g, views, cams, pxm, nullptr);
	for (int d = 0; d < 8; d++) {
		ThreeDVehicleView &v = views[d];
		std::vector<uint8_t> mask = HullOutline(g, v, cams[d], pxm);
		int best_x = v.shift_x, best_y = v.shift_y;
		float best = -1.0f;
		for (int step : {2, 1}) {
			int cx = best_x, cy = best_y, range = step * 2;
			for (int ty = cy - range; ty <= cy + range; ty += step) {
				for (int tx = cx - range; tx <= cx + range; tx += step) {
					float fit = OutlineFit(mask, v, tx, ty);
					if (fit > best) { best = fit; best_x = tx; best_y = ty; }
				}
			}
		}
		v.shift_x = best_x;
		v.shift_y = best_y;
		{
			/* The extents of the sprite's outline and the hull's, pixels about the anchor (the rig). */
			std::vector<uint8_t> sprite(mask.size(), 0);
			for (size_t i = 0; i < sprite.size(); i++) sprite[i] = v.pixels[i].a >= FIT_ALPHA;
			int hx0, hy0, hx1, hy1, sx0, sy0, sx1, sy1;
			extents(mask, v, hx0, hy0, hx1, hy1);
			extents(sprite, v, sx0, sy0, sx1, sy1);
			model->note += fmt::format("d{} fit {:.3f} shift {},{} sprite {}..{},{}..{} hull {}..{},{}..{}; ", d, best, v.shift_x, v.shift_y, sx0, sx1, sy0, sy1, hx0 + v.shift_x, hx1 + v.shift_x, hy0 + v.shift_y, hy1 + v.shift_y);
		}
		if (_debug_misc_level >= 5) {
			/* The rig: the view and the hull's outline over it, as a PPM in the working directory. */
			std::string name = fmt::format("three_d_view_{}_{}.ppm", engine.base(), d);
			auto f = FileHandle::Open(name, "wb");
			if (f.has_value()) {
				std::string head = fmt::format("P6\n{} {}\n255\n", v.width, v.height);
				fwrite(head.data(), 1, head.size(), *f);
				for (int y = 0; y < v.height; y++) {
					for (int x = 0; x < v.width; x++) {
						Colour c = v.pixels[static_cast<size_t>(y) * v.width + x];
						uint8_t rgb[3] = {c.r, c.g, c.b};
						if (c.a < OUTLINE_ALPHA) { rgb[0] = 40; rgb[1] = 40; rgb[2] = 40; }
						int mx = x - best_x, my = y - best_y; // the hull's outline moved by the shift onto the sprite
						bool hull = mx >= 0 && my >= 0 && mx < v.width && my < v.height && mask[static_cast<size_t>(my) * v.width + mx] != 0;
						if (hull) { rgb[0] = static_cast<uint8_t>((rgb[0] + 255) / 2); rgb[1] = rgb[1] / 2; rgb[2] = rgb[2] / 2; }
						if (x == v.anchor_x || y == v.anchor_y) { rgb[0] = 0; rgb[1] = 255; rgb[2] = 0; }
						fwrite(rgb, 1, 3, *f);
					}
				}
			}
		}
	}
	CarveHull(g, views, cams, pxm, nullptr);
	OpenHull(g);
	int voxels = 0;
	for (uint8_t o : g.occ) voxels += o;
	model->voxels = voxels;
	if (voxels == 0) {
		model->note += "empty hull";
		return nullptr;
	}

	/* The surface: voxels with an empty neighbour; the normal towards the empty side. */
	std::vector<int32_t> point_at(g.occ.size(), -1);
	std::vector<float> nrm;
	for (int z = 0; z < g.nz; z++) {
		for (int y = 0; y < g.ny; y++) {
			for (int x = 0; x < g.nx; x++) {
				if (!g.At(x, y, z)) continue;
				if (g.At(x - 1, y, z) && g.At(x + 1, y, z) && g.At(x, y - 1, z) && g.At(x, y + 1, z) && g.At(x, y, z - 1) && g.At(x, y, z + 1)) continue;
				float nx = 0, ny = 0, nz = 0;
				for (int dz = -2; dz <= 2; dz++) {
					for (int dy = -2; dy <= 2; dy++) {
						for (int dx = -2; dx <= 2; dx++) {
							if (dx == 0 && dy == 0 && dz == 0) continue;
							if (g.At(x + dx, y + dy, z + dz)) continue;
							nx += dx; ny += dy; nz += dz;
						}
					}
				}
				float len = std::sqrt(nx * nx + ny * ny + nz * nz);
				if (len < 1e-6f) { nx = 0; ny = 0; nz = 1; len = 1; }
				point_at[g.Index(x, y, z)] = static_cast<int32_t>(model->count);
				model->xyz.push_back(g.X(x)); model->xyz.push_back(g.Y(y)); model->xyz.push_back(g.Z(z));
				model->rank.push_back(static_cast<uint8_t>(((static_cast<uint32_t>(x) * 73856093u) ^ (static_cast<uint32_t>(y) * 19349663u) ^ (static_cast<uint32_t>(z) * 83492791u)) >> 13));
				nrm.push_back(nx / len); nrm.push_back(ny / len); nrm.push_back(nz / len);
				model->count++;
			}
		}
	}
	const size_t n = model->count;
	model->col8.assign(n * 24, 0);
	model->w8.assign(n * 8, 0);
	std::vector<float> wsum(n, 0.0f), rsum(n, 0.0f), gsum(n, 0.0f), bsum(n, 0.0f);

	/* The colour of each point in each view that sees it. */
	std::vector<float> sx(n), sy(n), depth(n);
	for (int d = 0; d < 8; d++) {
		const ThreeDVehicleView &v = views[d];
		const ViewCamera &cam = cams[d];
		float ax = v.anchor_x + v.shift_x + 0.5f, ay = v.anchor_y + v.shift_y + 0.5f;
		std::vector<float> zb(static_cast<size_t>(v.width) * v.height, std::numeric_limits<float>::infinity());
		for (size_t i = 0; i < n; i++) {
			cam.Project(model->xyz[i * 3], model->xyz[i * 3 + 1], model->xyz[i * 3 + 2], pxm, sx[i], sy[i], depth[i]);
			int px = static_cast<int>(std::floor(ax + sx[i])), py = static_cast<int>(std::floor(ay + sy[i]));
			if (px < 0 || py < 0 || px >= v.width || py >= v.height) continue;
			float &z = zb[static_cast<size_t>(py) * v.width + px];
			if (depth[i] < z) z = depth[i];
		}
		for (size_t i = 0; i < n; i++) {
			int px = static_cast<int>(std::floor(ax + sx[i])), py = static_cast<int>(std::floor(ay + sy[i]));
			if (px < 0 || py < 0 || px >= v.width || py >= v.height) continue;
			if (depth[i] > zb[static_cast<size_t>(py) * v.width + px] + 2.5f * vox) continue;
			Colour c = v.pixels[static_cast<size_t>(py) * v.width + px];
			if (c.a < OUTLINE_ALPHA) continue;
			float facing = nrm[i * 3] * cam.cam[0] + nrm[i * 3 + 1] * cam.cam[1] + nrm[i * 3 + 2] * cam.cam[2];
			if (facing < 0) facing = 0;
			float w = (facing * facing + 0.02f) * (c.a / 255.0f);
			model->col8[i * 24 + d * 3] = c.r;
			model->col8[i * 24 + d * 3 + 1] = c.g;
			model->col8[i * 24 + d * 3 + 2] = c.b;
			model->w8[i * 8 + d] = static_cast<uint8_t>(Clamp(static_cast<int>(std::lround(w * 250.0f)), 1, 255));
			wsum[i] += w; rsum[i] += w * c.r; gsum[i] += w * c.g; bsum[i] += w * c.b;
		}
	}

	/* The colour where no view sees a point: the mean of the views that do, else the neighbours'. */
	model->fallback.assign(n, Colour(90, 90, 90, 255));
	std::vector<uint8_t> coloured(n, 0);
	size_t uncoloured = 0;
	for (size_t i = 0; i < n; i++) {
		if (wsum[i] > 0) {
			model->fallback[i] = Colour(static_cast<uint8_t>(rsum[i] / wsum[i] + 0.5f), static_cast<uint8_t>(gsum[i] / wsum[i] + 0.5f), static_cast<uint8_t>(bsum[i] / wsum[i] + 0.5f));
			coloured[i] = 1;
		} else {
			uncoloured++;
		}
	}
	for (int round = 0; round < 16 && uncoloured > 0; round++) {
		std::vector<uint8_t> now = coloured;
		for (int z = 0; z < g.nz; z++) {
			for (int y = 0; y < g.ny; y++) {
				for (int x = 0; x < g.nx; x++) {
					int32_t i = point_at[g.Index(x, y, z)];
					if (i < 0 || coloured[i]) continue;
					uint r = 0, gg = 0, b = 0, cnt = 0;
					for (int dz = -1; dz <= 1; dz++) {
						for (int dy = -1; dy <= 1; dy++) {
							for (int dx = -1; dx <= 1; dx++) {
								int xx = x + dx, yy = y + dy, zz = z + dz;
								if (xx < 0 || yy < 0 || zz < 0 || xx >= g.nx || yy >= g.ny || zz >= g.nz) continue;
								int32_t j = point_at[g.Index(xx, yy, zz)];
								if (j < 0 || !coloured[j]) continue;
								r += model->fallback[j].r; gg += model->fallback[j].g; b += model->fallback[j].b; cnt++;
							}
						}
					}
					if (cnt == 0) continue;
					model->fallback[i] = Colour(r / cnt, gg / cnt, b / cnt);
					now[i] = 1;
					uncoloured--;
				}
			}
		}
		coloured = std::move(now);
	}

	if (_debug_misc_level >= 5) {
		/* The rig: how many points each view colours and the mean of its colours. */
		for (int d = 0; d < 8; d++) {
			size_t seen = 0; uint64_t r = 0, gg = 0, b = 0;
			for (size_t i = 0; i < n; i++) {
				if (model->w8[i * 8 + d] == 0) continue;
				seen++; r += model->col8[i * 24 + d * 3]; gg += model->col8[i * 24 + d * 3 + 1]; b += model->col8[i * 24 + d * 3 + 2];
			}
			model->note += fmt::format("c{} seen {} mean {},{},{}; ", d, seen, seen ? r / seen : 0, seen ? gg / seen : 0, seen ? b / seen : 0);
		}
		uint64_t r = 0, gg = 0, b = 0;
		for (const Colour &c : model->fallback) { r += c.r; gg += c.g; b += c.b; }
		model->note += fmt::format("fallback mean {},{},{}; ", r / n, gg / n, b / n);
	}
	model->build_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
	model->note = fmt::format("zoom {} pxm {:.1f} vox {:.3f} grid {}x{}x{} voxels {} points {} unseen {} built in {} ms; ", to_underlying(model->zoom), model->pxm, model->vox, g.nx, g.ny, g.nz, voxels, n, uncoloured, model->build_ms) + model->note;
	Debug(misc, 4, "three_d vehicle model of engine {} pal {:#x} look {:#x}: {}", engine.base(), pal, model->look, model->note);
	Debug(misc, 1, "3D vehicle model of engine {}: {}", engine.base(), model->note);
	return model;
}

/**
 * The likeness of an engine in a palette: built the first time it is asked for, kept for a while.
 * @return the model, or nullptr when the engine has no picture in some direction
 */
const ThreeDVehicleModel *GetThreeDVehicleModel(EngineID engine, PaletteID pal, const Vehicle *veh)
{
	uint64_t look = veh != nullptr ? VehicleLook(veh) : 0;
	for (auto it = _vehicle_models.begin(); it != _vehicle_models.end(); ++it) {
		if ((*it)->engine == engine && (*it)->pal == pal && (*it)->look == look) {
			std::unique_ptr<ThreeDVehicleModel> model = std::move(*it);
			_vehicle_models.erase(it);
			_vehicle_models.push_back(std::move(model));
			return _vehicle_models.back()->count == 0 ? nullptr : _vehicle_models.back().get();
		}
	}
	if (engine == EngineID::Invalid() || !Engine::IsValidID(engine)) return nullptr;
	std::unique_ptr<ThreeDVehicleModel> model = BuildVehicleModel(engine, pal, veh);
	if (model == nullptr) {
		/* Remember that there is none, so it is not tried at every frame. */
		model = std::make_unique<ThreeDVehicleModel>();
		model->engine = engine;
		model->pal = pal;
		model->look = look;
	}
	/* The oldest go when there are too many points kept; their pictures with them. */
	size_t points = model->count;
	for (const auto &m : _vehicle_models) points += m->count;
	while (points > MODEL_POINTS_KEPT && !_vehicle_models.empty()) {
		points -= _vehicle_models.front()->count;
		ForgetVehiclePictures(_vehicle_models.front().get());
		_vehicle_models.erase(_vehicle_models.begin());
	}
	_vehicle_models.push_back(std::move(model));
	return _vehicle_models.back()->count == 0 ? nullptr : _vehicle_models.back().get();
}

/** Forget the models (the sprites are reloaded). */
void ClearThreeDVehicleModels()
{
	_vehicle_pictures.clear();
	_vehicle_models.clear();
}

/** The zoom a model's views are at. */
ZoomLevel ThreeDVehicleModelZoom(const ThreeDVehicleModel *model)
{
	return model->zoom;
}

/**
 * Draw a model at an angle and zoom.
 * @param azimuth degrees; ViewAzimuth(d) shows the vehicle as its sprite of direction d does
 * @param out the picture, its left and top from the anchor
 * @param on_track foreshorten the vehicle along itself as the map does on straight track, smoothly between
 *                 the directions: in a sprite's own direction the picture is the sprite's size (the preview
 *                 shows the vehicle's true proportions instead)
 * @param part one part of a vehicle of several (ThreeDVehicleModel::part_x): the points between its cuts, about
 *             the position of the piece it is hung from, so the map draws each part where the game has it (a
 *             curve); -1 the whole about the anchor (the purchase preview)
 * @return whether there is a picture
 */
bool RenderThreeDVehicle(const ThreeDVehicleModel *model, ZoomLevel zoom, float azimuth, ThreeDModelPicture &out, bool on_track, int part)
{
	out = {};
	if (model == nullptr || model->count == 0) return false;
	const size_t n = model->count;
	const int ss = SUPERSAMPLE;
	float pxm = PXM_16X / (1 << to_underlying(zoom)) * ss;
	float f = azimuth * static_cast<float>(M_PI) / 180.0f;
	float c = std::cos(f), s = std::sin(f);
	/* On the track: the sprites of the directions along the track are foreshortened 1/sqrt 2 along the vehicle,
	 * those across not at all; between them smoothly, so the picture never jumps. */
	float along = on_track ? 1.0f - (1.0f - 0.70710678f) * std::fabs(std::sin(2.0f * f)) : 1.0f;
	/* A coarser zoom than the views' needs fewer points: the lower ranks only, about one to a pixel. */
	int coarser = std::max(0, to_underlying(zoom) - to_underlying(model->zoom));
	int keep = coarser >= 2 ? std::max(1, 256 >> (2 * coarser - 3)) : 256;

	/* One part of several: the points between its cuts, about the position of its piece. */
	float x_lo = -std::numeric_limits<float>::infinity(), x_hi = std::numeric_limits<float>::infinity(), x_at = 0.0f;
	if (part >= 0) {
		if (static_cast<size_t>(part) >= model->part_x.size()) return false;
		x_at = model->part_x[part];
		if (part > 0) x_hi = model->part_cut[part - 1];
		if (static_cast<size_t>(part) < model->part_cut.size()) x_lo = model->part_cut[part];
	}
	auto skip = [&](size_t i) {
		if (model->rank[i] >= keep) return true;
		float x = model->xyz[i * 3];
		return x < x_lo || x >= x_hi;
	};

	/* The mix of the two views nearest the angle. */
	float k[8];
	for (int d = 0; d < 8; d++) {
		float du = std::fabs(std::fmod(azimuth - ViewAzimuth(d) + 540.0f, 360.0f) - 180.0f);
		k[d] = du < 45.0f ? 1.0f - du / 45.0f : 0.0f;
	}

	/* Where the points land. */
	std::vector<float> sx(n), sy(n), depth(n);
	float minx = std::numeric_limits<float>::infinity(), miny = minx, maxx = -minx, maxy = -minx;
	for (size_t i = 0; i < n; i++) {
		if (skip(i)) continue;
		float x = (model->xyz[i * 3] - x_at) * along, y = model->xyz[i * 3 + 1], z = model->xyz[i * 3 + 2];
		float xr = x * c - y * s, yr = x * s + y * c;
		sx[i] = xr * pxm;
		sy[i] = (-yr * CAM_SIN - z * CAM_COS) * pxm;
		depth[i] = yr * CAM_COS - z * CAM_SIN;
		minx = std::min(minx, sx[i]); maxx = std::max(maxx, sx[i]);
		miny = std::min(miny, sy[i]); maxy = std::max(maxy, sy[i]);
	}
	/* A dot covers a voxel: 2x2 at the views' own zoom, bigger when the picture is finer than the views (a set
	 * with its sprites at the normal zoom only, looked at zoomed in). */
	int dot = std::max(2, static_cast<int>(std::ceil(model->vox * pxm * 1.05f)));
	int left = static_cast<int>(std::floor(minx)), top = static_cast<int>(std::floor(miny));
	left -= ((left % ss) + ss) % ss;
	top -= ((top % ss) + ss) % ss;
	int width = static_cast<int>(std::ceil(maxx)) - left + dot, height = static_cast<int>(std::ceil(maxy)) - top + dot;
	width += (ss - width % ss) % ss;
	height += (ss - height % ss) % ss;
	if (width <= 0 || height <= 0 || minx > maxx || static_cast<size_t>(width) * height > 64u * 1024 * 1024) return false;

	std::vector<float> zb(static_cast<size_t>(width) * height, std::numeric_limits<float>::infinity());
	std::vector<Colour> buf(static_cast<size_t>(width) * height, Colour(0, 0, 0, 0));
	for (size_t i = 0; i < n; i++) {
		if (skip(i)) continue;
		/* The colour at this angle. */
		float wsum = 0, r = 0, g = 0, b = 0;
		for (int d = 0; d < 8; d++) {
			if (k[d] == 0.0f) continue;
			float w = model->w8[i * 8 + d] * k[d];
			if (w == 0.0f) continue;
			wsum += w;
			r += w * model->col8[i * 24 + d * 3];
			g += w * model->col8[i * 24 + d * 3 + 1];
			b += w * model->col8[i * 24 + d * 3 + 2];
		}
		Colour col = wsum > 0 ? Colour(static_cast<uint8_t>(r / wsum + 0.5f), static_cast<uint8_t>(g / wsum + 0.5f), static_cast<uint8_t>(b / wsum + 0.5f)) : model->fallback[i];
		int px = static_cast<int>(std::lround(sx[i])) - left, py = static_cast<int>(std::lround(sy[i])) - top;
		for (int dy = 0; dy < dot; dy++) {
			for (int dx = 0; dx < dot; dx++) {
				int xx = px + dx, yy = py + dy;
				if (xx < 0 || yy < 0 || xx >= width || yy >= height) continue;
				size_t at = static_cast<size_t>(yy) * width + xx;
				if (depth[i] < zb[at]) {
					zb[at] = depth[i];
					buf[at] = col;
				}
			}
		}
	}

	/* Shrink: the mean of the dots' colours, the alpha from how many of the subpixels have one. */
	out.width = width / ss;
	out.height = height / ss;
	out.left = left / ss;
	out.top = top / ss;
	out.pixels.assign(static_cast<size_t>(out.width) * out.height, 0);
	for (int y = 0; y < out.height; y++) {
		for (int x = 0; x < out.width; x++) {
			uint r = 0, g = 0, b = 0, cnt = 0;
			for (int dy = 0; dy < ss; dy++) {
				for (int dx = 0; dx < ss; dx++) {
					Colour cc = buf[static_cast<size_t>(y * ss + dy) * width + (x * ss + dx)];
					if (cc.a == 0) continue;
					r += cc.r; g += cc.g; b += cc.b; cnt++;
				}
			}
			if (cnt == 0) continue;
			out.pixels[static_cast<size_t>(y) * out.width + x] = Colour(r / cnt, g / cnt, b / cnt, static_cast<uint8_t>(cnt * 255 / (ss * ss))).data;
		}
	}
	if (_debug_misc_level >= 6) {
		/* The rig: the picture as a PPM in the working directory. */
		auto f = FileHandle::Open(fmt::format("three_d_render_{}_{}.ppm", model->engine.base(), static_cast<int>(azimuth)), "wb");
		if (f.has_value()) {
			std::string head = fmt::format("P6\n{} {}\n255\n", out.width, out.height);
			fwrite(head.data(), 1, head.size(), *f);
			for (uint32_t p : out.pixels) {
				Colour c(p);
				uint8_t rgb[3] = {c.r, c.g, c.b};
				if (c.a == 0) { rgb[0] = 40; rgb[1] = 40; rgb[2] = 40; }
				fwrite(rgb, 1, 3, *f);
			}
		}
	}
	return true;
}

/** The rectangle the pictures of a model at a zoom all fit in, about the anchor: the union over eight angles. */
Rect ThreeDVehicleFrame(const ThreeDVehicleModel *model, ZoomLevel zoom)
{
	ThreeDVehicleModel *m = const_cast<ThreeDVehicleModel *>(model);
	auto it = m->frames.find(zoom);
	if (it != m->frames.end()) return it->second;
	Rect frame{INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
	ThreeDModelPicture pic;
	for (int i = 0; i < 8; i++) {
		if (!RenderThreeDVehicle(model, zoom, i * 45.0f + 22.5f, pic, false)) continue;
		frame.left = std::min(frame.left, pic.left);
		frame.top = std::min(frame.top, pic.top);
		frame.right = std::max(frame.right, pic.left + pic.width);
		frame.bottom = std::max(frame.bottom, pic.top + pic.height);
	}
	for (int i = 0; i < 8; i++) {
		if (!RenderThreeDVehicle(model, zoom, i * 45.0f, pic, false)) continue;
		frame.left = std::min(frame.left, pic.left);
		frame.top = std::min(frame.top, pic.top);
		frame.right = std::max(frame.right, pic.left + pic.width);
		frame.bottom = std::max(frame.bottom, pic.top + pic.height);
	}
	if (frame.right <= frame.left) frame = {};
	m->frames[zoom] = frame;
	return frame;
}


/**
 * A picture of a model at an angle and zoom, kept: the vehicles of the turned map ask for the same few over
 * and over (eight directions at the one angle of the view), and a turn by a degree asks for them anew.
 * @param azimuth degrees, counted in steps of PICTURE_ANGLE_STEP
 * @return the picture, or nullptr; it lives until PICTURES_KEPT others have been asked for
 */
const ThreeDModelPicture *GetThreeDVehiclePictureAt(const ThreeDVehicleModel *model, ZoomLevel zoom, float azimuth, bool on_track, int part)
{
	if (model == nullptr) return nullptr;
	int step = static_cast<int>(std::lround(std::fmod(azimuth + 720.0f, 360.0f) / PICTURE_ANGLE_STEP)) % static_cast<int>(std::lround(360.0f / PICTURE_ANGLE_STEP));
	uint64_t key = (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(model)) << 20) ^ (static_cast<uint64_t>(part + 1) << 14) ^ (static_cast<uint64_t>(step) << 5) ^ (static_cast<uint64_t>(to_underlying(zoom)) << 1) ^ (on_track ? 1 : 0);
	auto it = _vehicle_pictures.find(key);
	if (it != _vehicle_pictures.end()) {
		it->second.used = ++_vehicle_pictures_clock;
		return it->second.picture.width > 0 ? &it->second.picture : nullptr;
	}
	if (_vehicle_pictures.size() >= PICTURES_KEPT) {
		/* The oldest third goes. */
		std::vector<std::pair<uint64_t, uint64_t>> ages;
		for (const auto &e : _vehicle_pictures) ages.emplace_back(e.second.used, e.first);
		std::sort(ages.begin(), ages.end());
		for (size_t i = 0; i < ages.size() / 3; i++) _vehicle_pictures.erase(ages[i].second);
	}
	VehiclePictureEntry &e = _vehicle_pictures[key];
	e.model = model;
	e.used = ++_vehicle_pictures_clock;
	if (!RenderThreeDVehicle(model, zoom, step * PICTURE_ANGLE_STEP, e.picture, on_track, part)) e.picture = {};
	return e.picture.width > 0 ? &e.picture : nullptr;
}

/**
 * Put a vehicle on the turned map as its likeness at the angle of the view (DoDrawVehicle(), viewport.cpp).
 * A vehicle of several pieces -- an articulated one, a double-headed engine -- has one likeness, made from its
 * pieces composed as the chain spaces them; it is cut into parts, one for each piece that has a picture of its
 * own (the pieces without one -- a set paints the cab on the piece behind the front one -- go with it), and each
 * part is drawn about the position of its piece, in that piece's direction, so the vehicle bends on a curve as
 * its sprites do, the front sits where the game has the front, the trailer where it has the trailer, and the two
 * heads of a double-headed engine stand at either end of the train.
 * @param v the vehicle, or any piece of one
 * @param pal the palette of its sprites (the company's colours)
 * @param zoom the zoom of the view
 * @return whether it was put there (a piece drawn with another of its part counts); else the caller draws the sprites
 */
bool AddThreeDVehicleToViewport(const Vehicle *v, PaletteID pal, ZoomLevel zoom)
{
	if (v->direction == Direction::Invalid) return false;

	/* The vehicle this is a piece of, and which piece: the chain of the front and its articulated parts, then
	 * the rear head of a double-headed engine (VehiclePiecesOf()). */
	const Vehicle *front = v;
	int piece = 0;
	if (v->type == VehicleType::Train && Train::From(v)->IsRearDualheaded()) {
		front = Train::From(v)->other_multiheaded_part;
		if (front == nullptr) return false;
		piece = 1;
		for (const Vehicle *u = front; u->HasArticulatedPart(); u = u->GetNextArticulatedPart()) piece++;
	} else {
		while (front->IsArticulatedPart()) {
			front = front->Previous();
			if (front == nullptr) return false;
			piece++;
		}
	}
	/* The model from the vehicle's own sprites: an engine coupled turned round, a flipped piece drawn as its
	 * mirror piece, a load, a livery -- all as the map draws them, so the direction is the vehicle's own. */
	const ThreeDVehicleModel *model = GetThreeDVehicleModel(front->engine_type, pal, front);
	if (model == nullptr) return false;
	if (static_cast<size_t>(piece) >= model->piece_part.size()) return false;
	int part = model->piece_part[piece];
	/* A piece without a picture of its own: drawn with the piece of its part, nothing here. */
	if (model->part_piece[part] != piece) return true;

	/* The box of the part for the sorting: the boxes of its pieces together. */
	int xmin = INT32_MAX, ymin = INT32_MAX, zmin = INT32_MAX, xmax = INT32_MIN, ymax = INT32_MIN, zmax = INT32_MIN;
	{
		std::vector<VehiclePart> pieces = VehiclePiecesOf(front);
		for (size_t i = 0; i < pieces.size() && i < model->piece_part.size(); i++) {
			if (model->piece_part[i] != part) continue;
			const Vehicle *u = pieces[i].veh;
			xmin = std::min(xmin, u->x_pos + u->bounds.origin.x);
			ymin = std::min(ymin, u->y_pos + u->bounds.origin.y);
			zmin = std::min(zmin, u->z_pos + u->bounds.origin.z);
			xmax = std::max(xmax, u->x_pos + u->bounds.origin.x + u->bounds.extent.x - 1);
			ymax = std::max(ymax, u->y_pos + u->bounds.origin.y + u->bounds.extent.y - 1);
			zmax = std::max(zmax, u->z_pos + u->bounds.origin.z + u->bounds.extent.z - 1);
		}
	}

	/* The angle: the sprite of direction d shows at ViewAzimuth(d); the map, turned, shows the direction so many
	 * steps back, so the angle goes on by the view's turn. */
	float azimuth = ViewAzimuth(to_underlying(v->direction)) + GetViewYaw();
	const ThreeDModelPicture *pic = GetThreeDVehiclePictureAt(model, zoom, azimuth, true, part);
	if (pic == nullptr) return false;
	AddTurnedVehiclePicture(pic, v->x_pos, v->y_pos, v->z_pos, xmin, ymin, zmin, xmax, ymax, zmax);
	return true;
}

/** What the rig says about a model. */
std::string DescribeThreeDVehicleModel(const ThreeDVehicleModel *model)
{
	if (model == nullptr) return "no model";
	return model->note;
}
