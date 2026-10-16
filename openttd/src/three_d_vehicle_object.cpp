/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file three_d_vehicle_object.cpp Vehicles drawn from 3D objects (three_d_vehicle_object.h): their sprites made from the object, and the object in the turned view. */

#include "stdafx.h"
#include "three_d_vehicle_object.h"
#include "three_d_objects.h"
#include "three_d.h"
#include "engine_base.h"
#include "vehicle_base.h"
#include "vehicle_func.h"
#include "train.h"
#include "spritecache.h"
#include "zoom_func.h"
#include "landscape.h"
#include "viewport_func.h"
#include "direction_func.h"
#include "round_track.h"
#include "climate_industries.h"
#include "debug.h"
#include "settings_type.h"
#include "newgrf/newgrf_internal.h"
#include "timer/timer_game_calendar.h"
#include "timer/timer_game_tick.h"
#include "timer/timer.h"

#include <bit>
#include <chrono>
#include <cmath>
#include <map>
#include <set>

#include "safeguards.h"

/** Pixels to a metre at 16x: the camera of the sprites of this game's sets. */
static constexpr float PXM_16X = 48.8f;
/** The camera of the sprites: 30 degrees above the horizon. */
static constexpr float CAM_SIN = 0.5f;
static constexpr float CAM_COS = 0.86602540f;
/** World units along x and y to a metre: a tile, 16 units, is 14.84 m (CZTR's scale). */
static constexpr double UNITS_PER_METRE = 16.0 / 14.83765;
/** The foreshortening along a vehicle in the four directions of straight track, as the sets draw it. */
static constexpr float SQUEEZE = 0.70710678f;
/** The pictures are drawn at this many times the size and shrunk: soft edges. */
static constexpr int SUPERSAMPLE = 2;
/** The pictures of a turn of a thing that turns (ThreeDVehicleObject::spin_ticks, the spit of the pub). */
static constexpr int SPIN_STEPS = 24;
/** The pictures of the turned view kept: the vehicles ask for the same few over and over. */
static constexpr size_t PICTURES_KEPT = 1024;

/** The game's unit step of a direction (Direction: N, NE, E, SE, S, SW, W, NW), world units. */
static const int8_t DIR_DX[8] = {-1, -1, -1, 0, 1, 1, 1, 0};
static const int8_t DIR_DY[8] = {-1, 0, 1, 1, 1, 0, -1, -1};

/** The angle a vehicle facing a direction is drawn at by the camera of the sprites, degrees (as the 3D likeness, three_d_vehicle.cpp). */
static inline float ViewAzimuth(int d)
{
	return 90.0f - 45.0f * d;
}

/** A sprite to make from an object: which object, facing where, about which point. */
struct ObjectSpriteRequest {
	const ThreeDVehicleObject *obj = nullptr; ///< nullptr: the empty picture of the pieces that draw nothing
	int dir = 0;                              ///< the direction it faces
	float off_x = 0, off_y = 0;               ///< world units from the point the game draws the sprite about to the object's origin
	float scale = 1.0f;                       ///< the object's metres to the game's
	int fade = 0;                             ///< its state (StateStep())
};
static std::vector<ObjectSpriteRequest> _object_sprite_requests;
/** The sprites made: by the object, the piece it is hung from, the direction, its scale (thousandths) and its state (StateStep()). */
static std::map<std::tuple<const ThreeDVehicleObject *, int, int, int, int>, SpriteID> _object_sprites;
static SpriteID _object_empty_sprite = 0;

/** A picture of an object in the turned view, kept. */
struct ObjectPictureEntry {
	uint64_t used;
	ThreeDModelPicture picture;
};
static std::map<std::tuple<const ThreeDVehicleObject *, int, int, int, int, int, int, int>, ObjectPictureEntry> _object_pictures;
static uint64_t _object_pictures_clock = 0;

/** Forget the sprites made: the sprite cache they were in is gone (GfxInitSpriteMem()). */
void ResetThreeDVehicleObjectSprites()
{
	_object_sprite_requests.clear();
	_object_sprites.clear();
	_object_empty_sprite = 0;
	_object_pictures.clear();
	/* The GRFs are loaded anew: their packages read again the next time they are looked at, one put into the NewGRF
	 * folders meanwhile among them (three_d_objects.h). */
	ResetThreeDVehicleObjects();
}

/** Whether a vehicle of the type is foreshortened along itself on straight track, as the sets draw trains and road vehicles. */
static inline bool SqueezedType(VehicleType type)
{
	return type == VehicleType::Train || type == VehicleType::Road;
}

/**
 * The scale an object is drawn at: what its set's size parameter makes ("velikost": the player sets it, all the 3D
 * sets at once, at the bottom of the active list of the NewGRF window), else the size of the game's setting -- the
 * original game's, as the bigger sets draw (BRYLE), 1.2, or CZTR's, 1 (the player, 14. 10.: "the game can make the
 * object bigger, so there need not be two files"). The objects are in true metres.
 */
static float ObjectScale(const ThreeDVehicleObject *obj)
{
	if (obj != nullptr && obj->size.param >= 0) {
		const GRFFile *file = GetFileByGRFID(obj->grfid);
		if (file != nullptr) {
			auto scale = obj->size.ScaleOf(file->GetParam(obj->size.param));
			if (scale.has_value()) return *scale;
		}
	}
	return _settings_game.vehicle.object_original_size ? 1.2f : 1.0f;
}

/**
 * The state of a vehicle's object its GRF picks ("stavy", grrrrf hra/3d/ROZHRANI-GRF-STAVY.md): the state whose range
 * holds the number in the GRF of the first sprite of the set the GRF chooses for the vehicle, by its own rules.
 * @return the state's index plus one; 0 none: the object as it is, its paint new, no rust
 */
static int StateStep(const ThreeDVehicleObject *obj, EngineID engine, const Vehicle *v, EngineImageType image_type)
{
	if (obj->states.empty()) return 0;
	uint32_t id = GetVehicleSetSpriteLocalID(engine, v, image_type);
	if (id == UINT32_MAX) return 0;
	for (size_t i = 0; i < obj->states.size(); i++) {
		if (id >= obj->states[i].first && id <= obj->states[i].last) return static_cast<int>(i) + 1;
	}
	return 0;
}

/** The state of a step (StateStep()), nullptr for none. */
static const ThreeDVehicleState *StateOf(const ThreeDVehicleObject *obj, int step)
{
	return step > 0 && static_cast<size_t>(step) <= obj->states.size() ? &obj->states[step - 1] : nullptr;
}

/** The colour of the paint a state gives (StateStep()): 0 none, the texture's own. */
static uint32_t PaletteColour(const ThreeDVehicleObject *obj, int step)
{
	const ThreeDVehicleState *s = StateOf(obj, step);
	return s != nullptr ? s->colour : 0;
}

/** The fading of the paint in a state: 0 new, 1 the target's colour. */
static float FadeAmount(const ThreeDVehicleObject *obj, int step)
{
	const ThreeDVehicleState *s = StateOf(obj, step);
	return s != nullptr && obj->paint.fades ? s->fade : 0.0f;
}

/** How far a state has rusted: 0 new, 1 all of it. */
static float RustTime(const ThreeDVehicleObject *obj, int step)
{
	const ThreeDVehicleState *s = StateOf(obj, step);
	return s != nullptr && !obj->rust.mask_file.empty() ? s->rust : 0.0f;
}

/** The lengths of the object's pieces as its set says, eighths of a tile; a piece the game has not got counts as a whole vehicle. */
static std::vector<int> SetPieceLengths(const ThreeDVehicleObject *obj)
{
	std::vector<int> lengths;
	for (uint16_t local : obj->pieces) {
		EngineID e = _engine_mngr.GetID(obj->type, local, obj->grfid);
		lengths.push_back(e == EngineID::Invalid() ? static_cast<int>(VEHICLE_LENGTH) : ThreeDEngineLength(e));
	}
	return lengths;
}

/**
 * The lengths of the object's pieces on the map, eighths of a tile: the object's length at its scale, a step of
 * the chain a step of diagonal track (where a vehicle is not foreshortened), shared by the pieces in the
 * proportions of its set, each a whole vehicle at most (the player: "do it from the object"). A ship or an
 * aircraft keeps its set's.
 */
static std::vector<int> PieceLengths(const ThreeDVehicleObject *obj)
{
	std::vector<int> lengths = SetPieceLengths(obj);
	const ThreeDVehicleShape *shape = SqueezedType(obj->type) ? obj->GetShape() : nullptr;
	if (shape == nullptr || lengths.empty() || shape->x1 - shape->x0 < 0.1f) return lengths;
	const int n = static_cast<int>(lengths.size()), most = static_cast<int>(VEHICLE_LENGTH);
	int total = static_cast<int>(std::lround((shape->x1 - shape->x0) * ObjectScale(obj) * UNITS_PER_METRE / std::sqrt(2.0)));
	total = std::clamp(total, n, n * most);
	int set_total = 0;
	for (int l : lengths) set_total += l;
	int sum = 0;
	for (int &l : lengths) {
		l = std::clamp(static_cast<int>(std::lround(static_cast<double>(l) * total / std::max(set_total, 1))), 1, most);
		sum += l;
	}
	/* What the rounding left over: on the anchor piece first, then on the others from the middle out. */
	for (int k = 0; sum != total && k < 2 * n; k++) {
		int i = (obj->anchor + ((k & 1) ? (k + 1) / 2 : -(k / 2)) + n * 2) % n;
		while (sum < total && lengths[i] < most) { lengths[i]++; sum++; }
		while (sum > total && lengths[i] > 1) { lengths[i]--; sum--; }
	}
	return lengths;
}

/**
 * The length of a piece of a vehicle on the map, eighths of a tile (Train::ConsistChanged(), GetRoadVehLength()):
 * a piece of a vehicle drawn from its 3D object is as long as the object makes it (PieceLengths()).
 * @param set_length its length as its set says
 */
int ThreeDVehicleObjectPieceLength(EngineID engine, int set_length)
{
	int piece = 0;
	const ThreeDVehicleObject *obj = GetThreeDVehicleObject(engine, &piece);
	if (obj == nullptr || !SqueezedType(obj->type)) return set_length;
	std::vector<int> lengths = PieceLengths(obj);
	return piece >= 0 && piece < static_cast<int>(lengths.size()) ? lengths[piece] : set_length;
}

/** Where the object's origin is from a piece's position, steps forward along the vehicle: the middle of the chain. */
static double MiddleFromPiece(const ThreeDVehicleObject *obj, const std::vector<int> &lengths, int piece)
{
	std::vector<double> pos;
	double middle = ThreeDChainPositions(obj->type, lengths, pos);
	if (piece < 0 || static_cast<size_t>(piece) >= pos.size()) return 0.0;
	return middle - pos[piece];
}

/** The empty picture of the pieces that draw nothing. */
static SpriteID EmptyObjectSprite()
{
	if (_object_empty_sprite != 0) return _object_empty_sprite;
	SpriteID made = AllocateDerivedSpriteID();
	if (made == 0) return 0;
	_object_sprite_requests.push_back({});
	if (!SetThreeDVehicleObjectSprite(made, static_cast<uint32_t>(_object_sprite_requests.size()))) return 0;
	_object_empty_sprite = made;
	return made;
}

/** The sprite of an object facing a direction, hung from one of its pieces: made when it is first drawn. */
static SpriteID ObjectSprite(const ThreeDVehicleObject *obj, int piece, int dir, int fade)
{
	auto key = std::make_tuple(obj, piece, dir, static_cast<int>(std::lround(ObjectScale(obj) * 1000)), fade);
	auto it = _object_sprites.find(key);
	if (it != _object_sprites.end()) return it->second;

	std::vector<int> lengths = PieceLengths(obj);
	ObjectSpriteRequest req;
	req.obj = obj;
	req.dir = dir;
	req.scale = ObjectScale(obj);
	req.fade = fade;
	double along = MiddleFromPiece(obj, lengths, piece);
	Point anchor = ThreeDSpriteAnchor(obj->type, lengths[std::clamp<int>(piece, 0, static_cast<int>(lengths.size()) - 1)], static_cast<Direction>(dir));
	req.off_x = static_cast<float>(along * DIR_DX[dir] - anchor.x);
	req.off_y = static_cast<float>(along * DIR_DY[dir] - anchor.y);

	SpriteID made = AllocateDerivedSpriteID();
	if (made == 0) return 0;
	_object_sprite_requests.push_back(req);
	if (!SetThreeDVehicleObjectSprite(made, static_cast<uint32_t>(_object_sprite_requests.size()))) return 0;
	_object_sprites.emplace(key, made);
	return made;
}

/**
 * The sprite of an engine drawn as an object (GetCustomEngineSprite()): on the map, in the depot and the lists the
 * object hung from its anchor piece, the other pieces an empty picture; in the purchase window and the news, where
 * only the first piece is drawn, the whole object from the first piece.
 * @return whether the engine is drawn as an object
 */
bool GetThreeDVehicleObjectSprite(EngineID engine, const Vehicle *v, Direction direction, EngineImageType image_type, VehicleSpriteSeq *result)
{
	int piece = 0;
	const ThreeDVehicleObject *obj = GetThreeDVehicleObject(engine, &piece);
	if (obj == nullptr || direction == Direction::Invalid) return false;
	int from = (image_type == EngineImageType::Purchase || image_type == EngineImageType::Preview) ? 0 : obj->anchor;
	SpriteID sprite = piece == from ? ObjectSprite(obj, from, to_underlying(direction) & 7, StateStep(obj, engine, v, image_type)) : EmptyObjectSprite();
	if (sprite == 0) return false;
	result->Set(sprite);
	return true;
}

/**
 * An engine of a set drawn from the set's own pictures, as no 3D object was found for it: when those pictures are
 * empty -- a set whose vehicles are drawn from the objects of its package -- the package was not found, and nothing at
 * all would be seen. Said once a set in the log, at level 0, with where the game looked (the colleague's
 * hra/3d/PROSBA-TATRA-V6.md: "v5 was seen neither in the purchase list nor on the road"). Only by the pictures of the
 * purchase list and of the front of a vehicle: a part of an articulated vehicle no one is meant to see has empty ones.
 */
void NoteThreeDVehicleWithoutObject(EngineID engine, const Vehicle *v, EngineImageType image_type, const VehicleSpriteSeq &seq)
{
	if (image_type != EngineImageType::Purchase && (v == nullptr || !v->IsPrimaryVehicle())) return;
	const Engine *e = Engine::GetIfValid(engine);
	if (e == nullptr || e->grf_prop.grffile == nullptr) return;
	static std::set<uint32_t> told;
	static std::set<std::pair<uint32_t, uint16_t>> looked;
	const uint32_t grfid = e->grf_prop.grfid;
	if (told.contains(grfid) || !looked.insert({grfid, e->grf_prop.local_id}).second) return;
	for (uint i = 0; i < seq.count; i++) {
		/* A pixel at the normal zoom: ZOOM_BASE square as the cache keeps it, at the finest. */
		const Sprite *s = GetSprite(seq.seq[i].sprite, SpriteType::Normal);
		if (s != nullptr && (s->width > ZOOM_BASE || s->height > ZOOM_BASE)) return;
	}
	told.insert(grfid);
	Debug(misc, 0, "3D vehicles: the set {} ({:08X}) draws its engine {} with empty pictures, and no package of its 3D objects was found (looked in {}: <package>/objekty/<set>/objekty.json, a folder or a tar, with \"grf\": \"{:08X}\")",
			e->grf_prop.grffile->filename, std::byteswap(grfid), e->grf_prop.local_id, ThreeDPackagePlaces(), std::byteswap(grfid));
}

/** An object drawn into pixels about its origin. */
struct ObjectRaster {
	int left = 0, top = 0, width = 0, height = 0;
	std::vector<Colour> px;
};

/** The texels to a metre of a shape's texture at its full size: how fine a level of it a picture needs. */
static float TexelsPerMetre(const ThreeDVehicleObject &obj, const ThreeDVehicleShape &shape)
{
	static std::map<const ThreeDVehicleShape *, float> known;
	auto it = known.find(&shape);
	if (it != known.end()) return it->second;
	const ThreeDObjectTexture *t = obj.GetTexture(0);
	double uv_area = 0, area = 0;
	for (size_t i = 0; i + 2 < shape.tri.size(); i += 3) {
		const uint32_t a = shape.tri[i], b = shape.tri[i + 1], c = shape.tri[i + 2];
		double ux = shape.uv[b * 2] - shape.uv[a * 2], uy = shape.uv[b * 2 + 1] - shape.uv[a * 2 + 1];
		double vx = shape.uv[c * 2] - shape.uv[a * 2], vy = shape.uv[c * 2 + 1] - shape.uv[a * 2 + 1];
		uv_area += std::abs(ux * vy - uy * vx) / 2 * t->w * t->h;
		double px = shape.pos[b * 3] - shape.pos[a * 3], py = shape.pos[b * 3 + 1] - shape.pos[a * 3 + 1], pz = shape.pos[b * 3 + 2] - shape.pos[a * 3 + 2];
		double qx = shape.pos[c * 3] - shape.pos[a * 3], qy = shape.pos[c * 3 + 1] - shape.pos[a * 3 + 1], qz = shape.pos[c * 3 + 2] - shape.pos[a * 3 + 2];
		double cx = py * qz - pz * qy, cy = pz * qx - px * qz, cz = px * qy - py * qx;
		area += std::sqrt(cx * cx + cy * cy + cz * cz) / 2;
	}
	float density = area > 0 ? static_cast<float>(std::sqrt(uv_area / area)) : 1.0f;
	known.emplace(&shape, density);
	return density;
}

/**
 * Draw an object with the camera of the sprites, turned: the triangles through a depth buffer, each pixel the
 * texture's, lit from the corners' normals by one light fixed to the world (the colleague's: from the left above
 * as the sprites of the original game and the sets, the brightness 0.55 + 0.45 max(0, n . L), L = (-0.55, 0.35,
 * 0.75) in the world's axes), at SUPERSAMPLE times the size and shrunk, the alpha from the cover.
 * @param scale the object's metres to the game's
 * @param cam_az the angle the camera sees it at, degrees (ViewAzimuth(): its front to the right of the picture at 0)
 * @param world_az the angle it stands at in the world, the same way counted: the light is the world's
 * @param along the foreshortening along it
 * @param pxm pixels to a metre of the picture
 * @param fade how far its paint has faded to the target (FadeAmount())
 * @param rust_time how far into its rusting it is (RustTime())
 * @param tint the colour of the material an effect is coloured in ("barvy"), 0 none
 * @param paint_colour the vehicle's colour of its palette ("barvy", PaletteColour()), 0 the paint's own
 * @param stretch its height against its width (a plant of a list, "meritko" [x, y, z])
 * @param spin how far a thing that turns has turned about its axis, degrees (ThreeDVehicleObject::spin_axis)
 */
static bool RasterObject(const ThreeDVehicleObject &obj, float scale, float cam_az, float world_az, float along, float pxm, float fade, ObjectRaster &out, int frame = 0, float rust_time = 0, uint32_t tint = 0, uint32_t paint_colour = 0, float stretch = 1.0f, float spin = 0.0f, float pitch = 0.0f, float roll = 0.0f)
{
	const ThreeDVehicleShape *shape = obj.GetShape(frame);
	if (shape == nullptr) return false;
	const int ss = SUPERSAMPLE;
	const float p = pxm * ss;
	const float ca = std::cos(cam_az * static_cast<float>(M_PI) / 180.0f), sa = std::sin(cam_az * static_cast<float>(M_PI) / 180.0f);
	/* The light: the object's front and left in the world, the normals turned into it. */
	const float wa = (world_az + 135.0f) * static_cast<float>(M_PI) / 180.0f;
	const float fx = std::cos(wa), fy = std::sin(wa), lx = -fy, ly = fx;
	static const float L[3] = {-0.55f / 1.0f, 0.35f, 0.75f};
	const float ll = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
	const bool lit = !shape->nrm.empty();

	const size_t n = shape->pos.size() / 3;
	std::vector<float> sx(n), sy(n), depth(n), light(n, 1.0f);
	float minx = std::numeric_limits<float>::infinity(), miny = minx, maxx = -minx, maxy = -minx;
	/* Turned about its axis (Rodrigues): v' = v cos + (k x v) sin + k (k . v)(1 - cos), about the axis's point. */
	const float sp = spin * static_cast<float>(M_PI) / 180.0f, sc = std::cos(sp), sn = std::sin(sp);
	const float *k = obj.spin_axis, *kc = obj.spin_centre;
	auto turn = [&](float &vx, float &vy, float &vz) {
		const float d = k[0] * vx + k[1] * vy + k[2] * vz;
		const float cx = k[1] * vz - k[2] * vy, cy = k[2] * vx - k[0] * vz, cz = k[0] * vy - k[1] * vx;
		vx = vx * sc + cx * sn + k[0] * d * (1 - sc);
		vy = vy * sc + cy * sn + k[1] * d * (1 - sc);
		vz = vz * sc + cz * sn + k[2] * d * (1 - sc);
	};
	/* Up a hill its front higher (pitch), in a curve leaning in (roll, its left side down): about its middle on the rail. */
	const float pp = pitch * static_cast<float>(M_PI) / 180.0f, pc = std::cos(pp), ps = std::sin(pp);
	const float rp = roll * static_cast<float>(M_PI) / 180.0f, rc = std::cos(rp), rs = std::sin(rp);
	auto lean = [&](float &vx, float &vy, float &vz) {
		if (pitch != 0.0f) {
			const float x = vx * pc - vz * ps, z = vx * ps + vz * pc;
			vx = x;
			vz = z;
		}
		if (roll != 0.0f) {
			const float y = vy * rc + vz * rs, z = vz * rc - vy * rs;
			vy = y;
			vz = z;
		}
	};
	for (size_t i = 0; i < n; i++) {
		float px = shape->pos[i * 3], py = shape->pos[i * 3 + 1], pz = shape->pos[i * 3 + 2];
		if (spin != 0.0f) {
			px -= kc[0]; py -= kc[1]; pz -= kc[2];
			turn(px, py, pz);
			px += kc[0]; py += kc[1]; pz += kc[2];
		}
		lean(px, py, pz);
		float x = px * scale * along, y = py * scale, z = pz * scale * stretch;
		float xr = x * ca - y * sa, yr = x * sa + y * ca;
		sx[i] = xr * p;
		sy[i] = (-yr * CAM_SIN - z * CAM_COS) * p;
		depth[i] = yr * CAM_COS - z * CAM_SIN;
		minx = std::min(minx, sx[i]); maxx = std::max(maxx, sx[i]);
		miny = std::min(miny, sy[i]); maxy = std::max(maxy, sy[i]);
		if (lit) {
			float nx = shape->nrm[i * 3], ny = shape->nrm[i * 3 + 1], nz = shape->nrm[i * 3 + 2];
			if (spin != 0.0f) turn(nx, ny, nz);
			lean(nx, ny, nz);
			float wx = nx * fx + ny * lx, wy = nx * fy + ny * ly;
			float d = (wx * L[0] + wy * L[1] + nz * L[2]) / ll;
			light[i] = 0.55f + 0.45f * std::max(0.0f, d);
		}
	}
	if (minx > maxx) return false;
	int left = static_cast<int>(std::floor(minx)), top = static_cast<int>(std::floor(miny));
	left -= ((left % ss) + ss) % ss;
	top -= ((top % ss) + ss) % ss;
	int width = static_cast<int>(std::ceil(maxx)) - left + 1, height = static_cast<int>(std::ceil(maxy)) - top + 1;
	width += (ss - width % ss) % ss;
	height += (ss - height % ss) % ss;
	if (width <= 0 || height <= 0 || static_cast<size_t>(width) * height > 64u * 1024 * 1024) return false;

	/* The level of the textures: about a texel to a pixel. */
	int level = 0;
	for (float d = TexelsPerMetre(obj, *shape); d >= 2.0f * p && level < 12; d /= 2.0f) level++;
	std::vector<const ThreeDObjectTexture *> texs(std::max<size_t>(1, shape->pngs.size()));
	for (size_t i = 0; i < texs.size(); i++) texs[i] = obj.GetTexture(level, static_cast<int>(i));
	/* The paint of the body's texture: a colour by number, faded with age (the colleague's point 6). */
	const ThreeDVehiclePaint &paint = obj.paint;
	const bool coloured = paint.coloured || paint_colour != 0;
	const ThreeDObjectTexture *mask = (paint.fades || coloured) ? obj.GetPaintMask(level) : nullptr;
	const Colour target(paint.target), colour(paint_colour != 0 ? paint_colour : paint.colour);
	const float start = paint.start > 0 ? static_cast<float>(paint.start) : 255.0f;
	/* The rust, after the paint has faded (the colleague: the paint fades, the rust grows over it). */
	const ThreeDObjectTexture *rust_mask = rust_time > 0 ? obj.GetRustMask(level) : nullptr;
	const Colour rust_colour(obj.rust.target);
	/* The textures an effect is coloured in: those of its material ("barvy"), the texture times the colour. */
	const Colour tint_colour(tint);
	std::vector<bool> tinted(texs.size(), false);
	if (tint != 0) {
		for (size_t i = 0; i < texs.size() && i < shape->tex_names.size(); i++) tinted[i] = shape->tex_names[i] == obj.tint_material;
	}

	std::vector<float> zb(static_cast<size_t>(width) * height, std::numeric_limits<float>::infinity());
	std::vector<Colour> buf(static_cast<size_t>(width) * height, Colour(0, 0, 0, 0));
	for (size_t t = 0; t + 2 < shape->tri.size(); t += 3) {
		const uint32_t a = shape->tri[t], b = shape->tri[t + 1], c = shape->tri[t + 2];
		const int ti = t / 3 < shape->tri_tex.size() ? shape->tri_tex[t / 3] : 0;
		const ThreeDObjectTexture *tex = texs[static_cast<size_t>(ti) < texs.size() ? ti : 0];
		const ThreeDObjectTexture *tri_mask = ti == 0 ? mask : nullptr;
		const ThreeDObjectTexture *tri_rust = ti == 0 ? rust_mask : nullptr;
		const float ax = sx[a] - left, ay = sy[a] - top, bx = sx[b] - left, by = sy[b] - top, cx = sx[c] - left, cy = sy[c] - top;
		const float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
		if (std::abs(area) < 1e-6f) continue;
		int x0 = std::max(0, static_cast<int>(std::floor(std::min({ax, bx, cx})))), x1 = std::min(width - 1, static_cast<int>(std::ceil(std::max({ax, bx, cx}))));
		int y0 = std::max(0, static_cast<int>(std::floor(std::min({ay, by, cy})))), y1 = std::min(height - 1, static_cast<int>(std::ceil(std::max({ay, by, cy}))));
		const float inv = 1.0f / area;
		for (int py = y0; py <= y1; py++) {
			const float qy = py + 0.5f;
			for (int px = x0; px <= x1; px++) {
				const float qx = px + 0.5f;
				float w0 = ((cx - bx) * (qy - by) - (cy - by) * (qx - bx)) * inv;
				float w1 = ((ax - cx) * (qy - cy) - (ay - cy) * (qx - cx)) * inv;
				float w2 = 1.0f - w0 - w1;
				if (w0 < 0 || w1 < 0 || w2 < 0) continue;
				size_t at = static_cast<size_t>(py) * width + px;
				float dz = w0 * depth[a] + w1 * depth[b] + w2 * depth[c];
				if (dz >= zb[at]) continue;
				float u = w0 * shape->uv[a * 2] + w1 * shape->uv[b * 2] + w2 * shape->uv[c * 2];
				float v = w0 * shape->uv[a * 2 + 1] + w1 * shape->uv[b * 2 + 1] + w2 * shape->uv[c * 2 + 1];
				u -= std::floor(u);
				v -= std::floor(v);
				int tx = std::min(tex->w - 1, static_cast<int>(u * tex->w)), ty = std::min(tex->h - 1, static_cast<int>(v * tex->h));
				uint32_t texel = tex->px[static_cast<size_t>(ty) * tex->w + tx];
				if (texel == 0) continue; // a hole in the texture
				float k = w0 * light[a] + w1 * light[b] + w2 * light[c];
				Colour col(texel);
				float cr = col.r, cg = col.g, cb = col.b;
				if (tri_mask != nullptr) {
					int mx = std::min(tri_mask->w - 1, static_cast<int>(u * tri_mask->w)), my = std::min(tri_mask->h - 1, static_cast<int>(v * tri_mask->h));
					float m = Colour(tri_mask->px[static_cast<size_t>(my) * tri_mask->w + mx]).r / 255.0f;
					if (m > 0) {
						if (coloured) {
							/* The colour at the texture's lightness: its own where the texture is "zacatek" light. */
							float l = std::max({cr, cg, cb}) / start;
							cr += (std::min(255.0f, colour.r * l) - cr) * m;
							cg += (std::min(255.0f, colour.g * l) - cg) * m;
							cb += (std::min(255.0f, colour.b * l) - cb) * m;
						}
						if (fade > 0) {
							float f = fade * m;
							cr += (target.r - cr) * f;
							cg += (target.g - cg) * f;
							cb += (target.b - cb) * f;
						}
					}
				}
				if (tint != 0 && static_cast<size_t>(ti) < tinted.size() && tinted[ti]) {
					cr = cr * tint_colour.r / 255.0f;
					cg = cg * tint_colour.g / 255.0f;
					cb = cb * tint_colour.b / 255.0f;
				}
				if (tri_rust != nullptr) {
					int rx = std::min(tri_rust->w - 1, static_cast<int>(u * tri_rust->w)), ry = std::min(tri_rust->h - 1, static_cast<int>(v * tri_rust->h));
					float m = Colour(tri_rust->px[static_cast<size_t>(ry) * tri_rust->w + rx]).r;
					if (m > 0) {
						/* Gradual: the value is the moment it starts, from nothing to all rust in "nabeh"; else how much. */
						float f = obj.rust.gradual ? std::clamp((rust_time - (m - 1) / 254.0f) / obj.rust.ramp, 0.0f, 1.0f) : m / 255.0f * rust_time;
						cr += (rust_colour.r - cr) * f;
						cg += (rust_colour.g - cg) * f;
						cb += (rust_colour.b - cb) * f;
					}
				}
				zb[at] = dz;
				buf[at] = Colour(static_cast<uint8_t>(std::min(255.0f, cr * k)), static_cast<uint8_t>(std::min(255.0f, cg * k)), static_cast<uint8_t>(std::min(255.0f, cb * k)));
			}
		}
	}

	/* Shrink: the mean of the covered subpixels, the alpha from how many are. */
	out.width = width / ss;
	out.height = height / ss;
	out.left = left / ss;
	out.top = top / ss;
	out.px.assign(static_cast<size_t>(out.width) * out.height, Colour(0, 0, 0, 0));
	for (int y = 0; y < out.height; y++) {
		for (int x = 0; x < out.width; x++) {
			uint r = 0, g = 0, bl = 0, cnt = 0;
			for (int dy = 0; dy < ss; dy++) {
				for (int dx = 0; dx < ss; dx++) {
					Colour cc = buf[static_cast<size_t>(y * ss + dy) * width + (x * ss + dx)];
					if (cc.a == 0) continue;
					r += cc.r; g += cc.g; bl += cc.b; cnt++;
				}
			}
			if (cnt == 0) continue;
			out.px[static_cast<size_t>(y) * out.width + x] = Colour(r / cnt, g / cnt, bl / cnt, static_cast<uint8_t>(cnt * 255 / (ss * ss)));
		}
	}
	return true;
}

/**
 * Make a sprite of an object (ReadSprite()): the object drawn at 16x with the camera of the sprites, facing the
 * direction asked, about the point the game draws the sprite about; the coarser levels are made from it as from a
 * set's zin16 (ResizeSprites()).
 * @param request the sprite's request, counted from 1
 * @return the levels made: 16x
 */
ZoomLevels MakeThreeDVehicleObjectSprite(uint32_t request, SpriteLoader::SpriteCollection &sprite)
{
	ZoomLevels avail;
	avail.Set(ZoomLevel::Min);
	SpriteLoader::Sprite &s = sprite[ZoomLevel::Min];
	s.colours = SpriteComponents{SpriteComponent::RGB, SpriteComponent::Alpha};

	ObjectRaster r;
	auto start = std::chrono::steady_clock::now();
	const ObjectSpriteRequest *req = (request >= 1 && request <= _object_sprite_requests.size()) ? &_object_sprite_requests[request - 1] : nullptr;
	bool drawn = false;
	if (req != nullptr && req->obj != nullptr) {
		float along = (SqueezedType(req->obj->type) && (req->dir & 1)) ? SQUEEZE : 1.0f;
		float az = ViewAzimuth(req->dir);
		drawn = RasterObject(*req->obj, req->scale, az, az, along, PXM_16X, FadeAmount(req->obj, req->fade), r, 0, RustTime(req->obj, req->fade), 0, PaletteColour(req->obj, req->fade)) && r.width <= UINT16_MAX && r.height <= UINT16_MAX;
	}
	if (!drawn) {
		/* The empty picture, or an object that cannot be drawn: a transparent pixel. */
		s.width = 1;
		s.height = 1;
		s.x_offs = 0;
		s.y_offs = 0;
		s.AllocateData(ZoomLevel::Min, 1);
		return avail;
	}

	/* The object's origin from the point the sprite is drawn about, in pixels of 16x (RemapCoords()). */
	double ox = (req->off_y - req->off_x) * 2.0 * ZOOM_BASE, oy = (req->off_x + req->off_y) * static_cast<double>(ZOOM_BASE);
	s.width = static_cast<uint16_t>(r.width);
	s.height = static_cast<uint16_t>(r.height);
	s.x_offs = static_cast<int16_t>(r.left + std::lround(ox));
	s.y_offs = static_cast<int16_t>(r.top + std::lround(oy));
	s.AllocateData(ZoomLevel::Min, static_cast<size_t>(r.width) * r.height);
	for (size_t i = 0; i < r.px.size(); i++) {
		SpriteLoader::CommonPixel &d = s.data[i];
		d.r = r.px[i].r;
		d.g = r.px[i].g;
		d.b = r.px[i].b;
		d.a = r.px[i].a;
		d.m = 0;
	}
	Debug(sprite, 3, "3D vehicle {}: sprite of direction {}, faded {}, made, {}x{} at 16x, {} ms", req->obj->name, req->dir, req->fade, r.width, r.height,
			std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
	return avail;
}

/** Time spent making pictures of objects for the turned view, and how many, for the rig (test3d casy). */
double _object_picture_ms = 0;
uint64_t _object_pictures_made = 0;

/** A picture of an object in the turned view at an angle and zoom, kept (RasterObject()). */
/** The pictures asked for since this clock stay (PinThreeDPictures()). */
static uint64_t _pictures_pinned_from = UINT64_MAX;

/**
 * Keep the pictures of the 3D objects asked for from now on until unpinned: a view that collects its triangles and
 * draws them at once on every core (the view from the eyes) draws their pixels after it asked for more of them.
 */
void PinThreeDPictures(bool pin)
{
	_pictures_pinned_from = pin ? _object_pictures_clock + 1 : UINT64_MAX;
}

static const ThreeDModelPicture *ObjectPicture(const ThreeDVehicleObject *obj, float scale, ZoomLevel zoom, float cam_az, float world_az, bool squeeze, int fade, int frame = 0, int tint_index = -1, float stretch = 1.0f, int spin_step = 0, int pitch = 0, int roll = 0)
{
	int cam_step = static_cast<int>(std::lround(std::fmod(cam_az + 720.0f, 360.0f))) % 360;
	int world_step = static_cast<int>(std::lround(std::fmod(world_az + 720.0f, 360.0f))) % 360;
	pitch = std::clamp(pitch, -60, 60);
	roll = std::clamp(roll, -60, 60);
	auto key = std::make_tuple(obj, to_underlying(zoom), cam_step, world_step, static_cast<int>(std::lround(scale * 1000)), (fade & 0xFFFF) | (pitch + 64) << 16 | (roll + 64) << 24, frame | spin_step << 8 | (tint_index + 1) << 16, static_cast<int>(std::lround(stretch * 1000)));
	const uint32_t tint = tint_index >= 0 && static_cast<size_t>(tint_index) < obj->tint_palette.size() ? obj->tint_palette[tint_index] : 0;
	auto it = _object_pictures.find(key);
	if (it != _object_pictures.end()) {
		it->second.used = ++_object_pictures_clock;
		return it->second.picture.width > 0 ? &it->second.picture : nullptr;
	}
	if (_object_pictures.size() >= PICTURES_KEPT) {
		/* The older half goes. */
		std::vector<uint64_t> ages;
		for (const auto &e : _object_pictures) ages.push_back(e.second.used);
		std::nth_element(ages.begin(), ages.begin() + ages.size() / 2, ages.end());
		/* Never those of the picture being drawn (PinThreeDPictures()): their pixels are still to be drawn. */
		uint64_t limit = std::min(ages[ages.size() / 2], _pictures_pinned_from - 1);
		for (auto i = _object_pictures.begin(); i != _object_pictures.end();) {
			if (i->second.used <= limit) {
				i = _object_pictures.erase(i);
			} else {
				++i;
			}
		}
	}
	ObjectPictureEntry &e = _object_pictures[key];
	e.used = ++_object_pictures_clock;
	/* On the track foreshortened along itself as the map does, smoothly between the directions (as the likeness, RenderThreeDVehicle()). */
	float f = cam_step * static_cast<float>(M_PI) / 180.0f;
	float along = squeeze ? 1.0f - (1.0f - SQUEEZE) * std::fabs(std::sin(2.0f * f)) : 1.0f;
	ObjectRaster r;
	auto start = std::chrono::steady_clock::now();
	_object_pictures_made++;
	auto timed = [&]() { _object_picture_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(); };
	if (RasterObject(*obj, scale, static_cast<float>(cam_step), static_cast<float>(world_step), along, PXM_16X / (1 << to_underlying(zoom)), FadeAmount(obj, fade), r, frame, RustTime(obj, fade), tint, PaletteColour(obj, fade), stretch, spin_step * 360.0f / SPIN_STEPS, static_cast<float>(pitch), static_cast<float>(roll))) {
		Debug(sprite, 4, "3D vehicle {}: picture at {} degrees, zoom {}: {}x{}, {} ms", obj->name, cam_step, to_underlying(zoom), r.width, r.height,
				std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
		e.picture.width = r.width;
		e.picture.height = r.height;
		e.picture.left = r.left;
		e.picture.top = r.top;
		e.picture.pixels.resize(r.px.size());
		for (size_t i = 0; i < r.px.size(); i++) e.picture.pixels[i] = r.px[i].a == 0 ? 0 : r.px[i].data;
	}
	timed();
	return e.picture.width > 0 ? &e.picture : nullptr;
}

/**
 * Put a vehicle drawn as an object on the turned map (AddThreeDVehicleToViewport()): the object at the angle of
 * the view, hung from the middle of its chain, sorted by the box of its pieces; its other pieces draw nothing.
 * @param pose where and at what angle to draw it instead (a round curve, round_track.h), or nullptr
 * @return whether the vehicle is drawn as an object (a piece drawn with its anchor piece counts)
 */
bool AddThreeDVehicleObjectToViewport(const Vehicle *v, ZoomLevel zoom, const RoundTrackPose *pose)
{
	int piece = 0;
	const ThreeDVehicleObject *obj = GetThreeDVehicleObject(v->engine_type, &piece);
	if (obj == nullptr) return false;
	if (piece != obj->anchor) return true;

	/* The way it faces: a train turned round, the rear head of a double-headed engine, the other way. */
	bool reversed = false;
	if (v->type == VehicleType::Train) {
		const Train *t = Train::From(v);
		if (t->flags.Test(VehicleRailFlag::Flipped)) reversed = !reversed;
		if (t->spritenum == CUSTOM_VEHICLE_SPRITENUM_REVERSED) reversed = !reversed;
	}
	Direction dir = reversed ? ReverseDir(v->direction) : v->direction;
	int d = to_underlying(dir) & 7;

	std::vector<int> lengths = PieceLengths(obj);
	float scale = ObjectScale(obj);
	double along = MiddleFromPiece(obj, lengths, piece);
	float world_az = pose != nullptr ? pose->azimuth + (reversed ? 180.0f : 0.0f) : ViewAzimuth(d);
	const ThreeDModelPicture *pic = ObjectPicture(obj, scale, zoom, world_az + GetViewYaw(), world_az, SqueezedType(obj->type), StateStep(obj, v->engine_type, v, EngineImageType::OnMap));
	if (pic == nullptr) return true;

	/* The box of the pieces for the sorting. */
	const Vehicle *front = v;
	while (front->IsArticulatedPart() && front->Previous() != nullptr) front = front->Previous();
	int xmin = INT32_MAX, ymin = INT32_MAX, zmin = INT32_MAX, xmax = INT32_MIN, ymax = INT32_MIN, zmax = INT32_MIN;
	for (const Vehicle *u = front; u != nullptr; u = u->HasArticulatedPart() ? u->GetNextArticulatedPart() : nullptr) {
		xmin = std::min(xmin, u->x_pos + u->bounds.origin.x);
		ymin = std::min(ymin, u->y_pos + u->bounds.origin.y);
		zmin = std::min(zmin, u->z_pos + u->bounds.origin.z);
		xmax = std::max(xmax, u->x_pos + u->bounds.origin.x + u->bounds.extent.x - 1);
		ymax = std::max(ymax, u->y_pos + u->bounds.origin.y + u->bounds.extent.y - 1);
		zmax = std::max(zmax, u->z_pos + u->bounds.origin.z + u->bounds.extent.z - 1);
	}

	/* Hung from the middle of the chain: from the anchor piece that far along. */
	int ax = v->x_pos + static_cast<int>(std::lround(along * DIR_DX[d])), ay = v->y_pos + static_cast<int>(std::lround(along * DIR_DY[d]));
	if (pose != nullptr) {
		int nx = static_cast<int>(std::lround(pose->x)), ny = static_cast<int>(std::lround(pose->y));
		xmin += nx - v->x_pos; xmax += nx - v->x_pos; ymin += ny - v->y_pos; ymax += ny - v->y_pos;
		ax = nx;
		ay = ny;
	}
	AddTurnedVehiclePicture(pic, ax, ay, v->z_pos, xmin, ymin, zmin, xmax, ymax, zmax);
	return true;
}

/**
 * The picture of an engine drawn from its 3D object at an angle, for the turning preview beside the purchase window:
 * the object itself, as the turned map draws it (not the likeness made from its sprites); nullptr for an engine
 * without an object.
 * @param azimuth the angle, as the likeness counts it (ViewAzimuth())
 */
const ThreeDModelPicture *GetThreeDVehicleObjectPreviewPicture(EngineID engine, ZoomLevel zoom, float azimuth)
{
	const ThreeDVehicleObject *obj = GetThreeDVehicleObject(engine);
	if (obj == nullptr) return nullptr;
	return ObjectPicture(obj, ObjectScale(obj), zoom, azimuth, azimuth, SqueezedType(obj->type), StateStep(obj, engine, nullptr, EngineImageType::Purchase));
}

/** The huts whose fireworks this drawing of the turned view puts on (QueueThreeDFireworks()). */
static std::vector<TileIndex> _turned_fireworks;

/** A hut's fireworks to be drawn as the 3D object, after the tiles of the turned view (they would go with the hut's own sprites, left out where the hut is drawn from its renders). */
void QueueThreeDFireworks(TileIndex hut_front)
{
	if (std::ranges::find(_turned_fireworks, hut_front) == _turned_fireworks.end()) _turned_fireworks.push_back(hut_front);
}

/**
 * Put the fireworks over the huts of this drawing on the turned view, from the colleague's object (the player: "and
 * then fireworks 3D, use the changes of colour of one and the same object, let it be all colours"): the launch and the
 * three phases of the ball as its frames, the phase as the hut's sprites go (HutFireworkPhase()), each phase of each
 * firework its colour of the palette; over the middle of the hut's front tile, at true size, unlit (it glows).
 */
void AddThreeDEffectsToViewport(ZoomLevel zoom)
{
	const ThreeDVehicleObject *fx = _turned_fireworks.empty() ? nullptr : GetThreeDEffect("ohnostroj");
	for (TileIndex tile : _turned_fireworks) {
		if (fx == nullptr) break;
		uint part = 0;
		uint32_t hash = 0;
		if (!HutFireworkPhase(tile, TimerGameTick::counter, part, hash) || part >= 4) continue;
		int frame = fx->frames > 0 ? static_cast<int>(std::min<uint>(part, fx->frames - 1)) : 0;
		int tint = part == 0 || fx->tint_palette.empty() ? -1 : static_cast<int>(hash % fx->tint_palette.size());
		const ThreeDVehicleShape *shape = fx->GetShape(frame);
		if (shape == nullptr) continue;
		const ThreeDModelPicture *pic = ObjectPicture(fx, 1.0f, zoom, GetViewYaw(), 0.0f, false, 0, frame, tint);
		if (pic == nullptr) continue;
		int x = TileX(tile) * TILE_SIZE + TILE_SIZE / 2, y = TileY(tile) * TILE_SIZE + TILE_SIZE / 2, z = GetTileMaxPixelZ(tile);
		int height = std::max(1, static_cast<int>(std::lround(shape->z1 * 8.0 / 3.0)));
		AddTurnedVehiclePicture(pic, x, y, z, x - 1, y - 1, z, x + 1, y + 1, z + height);
	}
	_turned_fireworks.clear();
}

/** A figure that moves drawn on the map: its tile is drawn again when its frame changes (AddThreeDFigureToViewport()). */
struct MovingFigure {
	TileIndex tile;
	int ticks;     ///< the ticks a frame of it is held
	uint64_t step; ///< the frame's step it was drawn in: the game's tick counter / ticks
	int z0, z1;    ///< the heights it is between, units; z0 INT_MIN not known: its tile as high as a building may be
	double x0, y0, x1, y1; ///< the ground it is over, world units, when its heights are known
};
static std::vector<MovingFigure> _moving_figures;

/** Draw the tiles of the figures that move again when their frames change, every tick of the game (only the
 * screen, nothing of the game); a figure no longer drawn drops out. */
static const IntervalTimer<TimerGameTick> _moving_figures_timer({TimerGameTick::Priority::None, 1}, [](auto) {
	for (auto it = _moving_figures.begin(); it != _moving_figures.end();) {
		if (TimerGameTick::counter / it->ticks == it->step) {
			++it;
			continue;
		}
		if (it->z0 == INT_MIN) {
			MarkTileDirtyByTile(it->tile);
		} else {
			MarkWorldBoxDirty(it->x0, it->y0, it->x1, it->y1, it->z0, it->z1);
		}
		it = _moving_figures.erase(it);
	}
});

/**
 * A figure standing on the turned map (the girls at a bus stop): drawn from its object at the angle of the view,
 * lit as the vehicles, hung from where its feet stand, sorted by a small box about them. A figure that moves
 * ("animace") is drawn in its frame of the game's tick, and its tile is drawn again when the frame changes.
 * @param name the figure's name in the index ("druh": "postava")
 * @param scale its size against true metres (the girls of a stop are drawn twice theirs, as their sprites are)
 * @param x,y,z where its feet stand, world units
 * @param facing the way it faces, degrees in the world: 0 along +x, 90 along +y (the colleague's renders' angles)
 * @param zoom the zoom of the view
 * @param tile the tile it stands on, drawn again when it moves
 * @param phase how many frames on it is from the others (a hash of its tile: the girls of a map do not all move at once)
 * @return whether it was drawn so; else the caller draws its sprite
 */
/**
 * A vehicle object of no vehicle on the turned map (a car of the game's own driving about, three_d_walkers.h): at a
 * place, facing a way (degrees, 0 along +x), sorted by a box about it as long as it is.
 */
bool AddThreeDObjectToViewport(const ThreeDVehicleObject *obj, float scale, int x, int y, int z, float facing, ZoomLevel zoom)
{
	const ThreeDVehicleShape *shape = obj != nullptr ? obj->GetShape() : nullptr;
	if (shape == nullptr) return false;
	float world_az = facing - 135.0f;
	const ThreeDModelPicture *pic = ObjectPicture(obj, scale, zoom, world_az + GetViewYaw(), world_az, false, 0);
	if (pic == nullptr) return false;
	int half = std::max(1, static_cast<int>(std::lround(std::max(shape->x1 - shape->x0, shape->y1 - shape->y0) * scale * OBJECT_UNITS_PER_METRE / 2)));
	int height = std::max(1, static_cast<int>(std::lround(shape->z1 * scale * 8.0 / 3.0)));
	AddTurnedVehiclePicture(pic, x, y, z, x - half, y - half, z, x + half, y + half, z + height);
	return true;
}

bool AddThreeDFigureToViewport(std::string_view name, float scale, int x, int y, int z, float facing, ZoomLevel zoom, TileIndex tile, uint32_t phase, int frame)
{
	int height = 0;
	const ThreeDModelPicture *pic = GetThreeDFigurePicture(name, scale, facing, zoom, tile, phase, &height, frame, NAN, z, x, y);
	if (pic == nullptr) return false;
	AddTurnedVehiclePicture(pic, x, y, z, x - 1, y - 1, z, x + 1, y + 1, z + height);
	return true;
}

/**
 * The picture of a figure of the turned map at the angle of the view (AddThreeDFigureToViewport(); a figure on a
 * building, drawn among its triangles): its frame of the game's tick when it moves, its tile then drawn again as the
 * frame changes.
 * @param[out] height how tall it is, units of height
 * @param frame the frame to draw (one walking: by the way walked, three_d_walkers.h); -1 by the game's tick
 * @param yaw the angle of the view it is seen from (the preview beside an industry's window), NaN the map's
 * @param z the height it stands at, units, INT_MIN not known: its tile drawn again as high as a building may be
 * @param x,y where it stands, world units, when z is known
 * @return the picture, or nullptr when the figure is not there
 */
const ThreeDModelPicture *GetThreeDFigurePicture(std::string_view name, float scale, float facing, ZoomLevel zoom, TileIndex tile, uint32_t phase, int *height, int frame, float yaw, int z, int x, int y)
{
	if (std::isnan(yaw)) yaw = GetViewYaw();
	const ThreeDVehicleObject *fig = GetThreeDFigure(name);
	if (fig == nullptr) return nullptr;
	const bool by_tick = frame < 0;
	if (by_tick) frame = fig->FrameAt(TimerGameTick::counter, phase);
	const ThreeDVehicleShape *shape = fig->GetShape(frame);
	if (shape == nullptr) return nullptr;
	/* A thing that turns: where in its turn it is, by the game's tick (its frames change after so many turns). */
	const int spin_step = fig->spin_ticks > 0 ? static_cast<int>((TimerGameTick::counter % fig->spin_ticks) * SPIN_STEPS / fig->spin_ticks) : 0;
	/* The angle as the vehicles count it: the front to the right of the picture at 0, along +x of the world at 135. */
	float world_az = facing - 135.0f;
	const ThreeDModelPicture *pic = ObjectPicture(fig, scale, zoom, world_az + yaw, world_az, false, 0, frame, -1, 1.0f, spin_step);
	if (pic == nullptr) return nullptr;
	const int tall = std::max(1, static_cast<int>(std::lround(shape->z1 * scale * 8.0 / 3.0)));
	if (height != nullptr) *height = tall;
	if (by_tick && (fig->frames > 1 || fig->spin_ticks > 0) && tile != INVALID_TILE) {
		/* Its tile is drawn again as the picture changes: a frame, or a step of a turn. */
		const int ticks = fig->spin_ticks > 0 ? std::max(1, fig->spin_ticks / SPIN_STEPS) : fig->frame_ticks;
		uint64_t step = TimerGameTick::counter / ticks;
		bool known = false;
		/* Where it stands, as far as it reaches about (drawn bigger on the map than it is, its scale), a little below
		 * its feet and above its head: where it reaches over as it moves. */
		const double rx = std::max(std::abs(shape->x0), std::abs(shape->x1)), ry = std::max(std::abs(shape->y0), std::abs(shape->y1));
		const double reach = std::hypot(rx, ry) * scale * OBJECT_UNITS_PER_METRE + 2;
		MovingFigure f{tile, ticks, step, INT_MIN, INT_MIN, x - reach, y - reach, x + reach, y + reach};
		if (z != INT_MIN) {
			f.z0 = z - TILE_HEIGHT;
			f.z1 = z + tall + TILE_HEIGHT;
		}
		for (MovingFigure &m : _moving_figures) {
			if (m.tile != tile) continue;
			if (m.ticks != ticks) continue;
			m.step = step;
			/* Two figures on one tile: the box of both, or the tile as high as a building when one is not known. */
			if (m.z0 == INT_MIN || f.z0 == INT_MIN) {
				m.z0 = INT_MIN;
			} else {
				m.z0 = std::min(m.z0, f.z0);
				m.z1 = std::max(m.z1, f.z1);
				m.x0 = std::min(m.x0, f.x0);
				m.y0 = std::min(m.y0, f.y0);
				m.x1 = std::max(m.x1, f.x1);
				m.y1 = std::max(m.y1, f.y1);
			}
			known = true;
		}
		if (!known) _moving_figures.push_back(f);
	}
	return pic;
}

/**
 * The picture of a plant standing on a building or a field ("rostliny", three_d_objects.h) at the angle of the view:
 * a figure that never moves, its angles to the view and to the light in steps of PLANT_STEP degrees, so that the
 * hundreds of plants of a field share a few pictures (a plant looks about the same from a few degrees aside).
 * @param stretch its height against its width
 * @param[out] height how tall it is, units of height
 * @param yaw the angle of the view it is seen from, NaN the map's
 */
const ThreeDModelPicture *GetThreeDPlantPicture(std::string_view name, float scale, float stretch, float facing, ZoomLevel zoom, int *height, float yaw)
{
	static constexpr float PLANT_STEP = 10.0f;
	const ThreeDVehicleObject *plant = GetThreeDFigure(name);
	if (plant == nullptr) return nullptr;
	const ThreeDVehicleShape *shape = plant->GetShape(0);
	if (shape == nullptr) return nullptr;
	if (std::isnan(yaw)) yaw = GetViewYaw();
	float world_az = std::round((facing - 135.0f) / PLANT_STEP) * PLANT_STEP;
	float cam_az = std::round((facing - 135.0f + yaw) / PLANT_STEP) * PLANT_STEP;
	/* Sizes in steps of a twentieth: the plants of a list are each a little other. */
	scale = std::max(0.05f, std::round(scale * 20.0f) / 20.0f);
	stretch = std::max(0.05f, std::round(stretch * 20.0f) / 20.0f);
	const ThreeDModelPicture *pic = ObjectPicture(plant, scale, zoom, cam_az, world_az, false, 0, 0, -1, stretch);
	if (pic == nullptr) return nullptr;
	if (height != nullptr) *height = std::max(1, static_cast<int>(std::lround(shape->z1 * scale * stretch * 8.0 / 3.0)));
	return pic;
}

/**
 * A thing in the hands of the walker of the view from the first person ("predmet" of the indices, the colleague's
 * hammer, hra/3d/PROSBA-PREDMETY-KLADIVO-TRUHLA.md; the player: "the game swings it, we need only the models"): drawn
 * over the picture from its eyes at the bottom right, as a right hand holds it, in perspective and lit from its
 * normals. The thing's origin is the lower end of its handle, the handle along +z, its striking faces to +-x; the
 * hand holds it a little above the end. A swing turns it about the line through the hand across the view: back and
 * up as it is raised, forward and down as it strikes.
 * @param name the thing ("jmeno" of the index)
 * @param swing how far it is swung, degrees: 0 at rest, below 0 raised back, above 0 struck forward and down
 * @param raise how far up in the hand it is: 0 put away under the picture, 1 held
 * @param dst the picture, width by height
 * @param zb its depth, 1 over the distance ahead in world units (larger nearer): drawn where the thing is nearer,
 *           and kept there, so that the night's lights behind it stay behind it
 * @param focal pixels of the picture across, for a unit across at a unit ahead
 * @return whether it was drawn
 */
bool DrawThreeDHeldThing(std::string_view name, float swing, float raise, uint32_t *dst, float *zb, int width, int height, double focal)
{
	if (raise <= 0.0f || width <= 0 || height <= 0) return false;
	const ThreeDVehicleObject *obj = GetThreeDFigure(name);
	if (obj == nullptr) return false;
	const ThreeDVehicleShape *shape = obj->GetShape(0);
	if (shape == nullptr || shape->tri.empty()) return false;

	/* The eye: x to the right, y up, z ahead, metres. */
	struct V { float x, y, z; };
	auto unit = [](V v) {
		const float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
		return V{v.x / l, v.y / l, v.z / l};
	};
	/* At rest the handle leans forward and a little to the left, a striking face ahead; the hand at the bottom
	 * right, lower while the thing is being put away or taken out. */
	const V hz = unit({-0.12f, 0.62f, 0.75f});
	V hx{0.1f, 0.0f, 1.0f};
	{
		const float d = hx.x * hz.x + hx.y * hz.y + hx.z * hz.z;
		hx = unit({hx.x - d * hz.x, hx.y - d * hz.y, hx.z - d * hz.z});
	}
	const V hy{hz.y * hx.z - hz.z * hx.y, hz.z * hx.x - hz.x * hx.z, hz.x * hx.y - hz.y * hx.x};
	/* Striking, the arms go forward and up with it, so that the head comes down before the eyes. */
	const float reach = std::clamp(swing / 70.0f, 0.0f, 1.0f);
	const V hand{0.40f - 0.12f * reach, -0.50f - (1.0f - raise) * 0.7f + 0.22f * reach, 0.52f + 0.30f * reach};
	static constexpr float GRIP = 0.22f; ///< where the hand holds it, metres up the handle
	const float a = swing * static_cast<float>(M_PI) / 180.0f, ca = std::cos(a), sa = std::sin(a);
	auto place = [&](float px, float py, float pz, bool point) {
		if (point) pz -= GRIP;
		V v{hx.x * px + hy.x * py + hz.x * pz, hx.y * px + hy.y * py + hz.y * pz, hx.z * px + hy.z * py + hz.z * pz};
		/* The swing: about the hand's line across, from up towards ahead. */
		v = V{v.x, v.y * ca - v.z * sa, v.y * sa + v.z * ca};
		if (point) v = V{v.x + hand.x, v.y + hand.y, v.z + hand.z};
		return v;
	};

	const size_t n = shape->pos.size() / 3;
	std::vector<float> sx(n), sy(n), iz(n), light(n, 1.0f);
	const bool lit = shape->nrm.size() == shape->pos.size();
	/* The light from above, a little left and behind the eye. */
	const V sun = unit({-0.45f, 0.75f, -0.5f});
	const float cx = width / 2.0f, cy = height / 2.0f, f = static_cast<float>(focal);
	for (size_t i = 0; i < n; i++) {
		const V p = place(shape->pos[i * 3], shape->pos[i * 3 + 1], shape->pos[i * 3 + 2], true);
		iz[i] = p.z > 0.02f ? 1.0f / p.z : -1.0f;
		sx[i] = cx + f * p.x * std::max(iz[i], 0.0f);
		sy[i] = cy - f * p.y * std::max(iz[i], 0.0f);
		if (lit) {
			const V q = place(shape->nrm[i * 3], shape->nrm[i * 3 + 1], shape->nrm[i * 3 + 2], false);
			light[i] = 0.45f + 0.55f * std::max(0.0f, q.x * sun.x + q.y * sun.y + q.z * sun.z);
		}
	}
	const float to_world = 1.0f / static_cast<float>(OBJECT_UNITS_PER_METRE);
	bool drawn = false;
	for (size_t t = 0; t + 2 < shape->tri.size(); t += 3) {
		const uint32_t i0 = shape->tri[t], i1 = shape->tri[t + 1], i2 = shape->tri[t + 2];
		if (iz[i0] <= 0 || iz[i1] <= 0 || iz[i2] <= 0) continue; // behind the eye
		const int ti = t / 3 < shape->tri_tex.size() ? shape->tri_tex[t / 3] : 0;
		const ThreeDObjectTexture *tex = obj->GetTexture(0, ti);
		const float ax = sx[i0], ay = sy[i0], bx = sx[i1], by = sy[i1], qx = sx[i2], qy = sy[i2];
		const float area = (bx - ax) * (qy - ay) - (by - ay) * (qx - ax);
		if (std::abs(area) < 1e-6f) continue;
		const int x0 = std::max(0, static_cast<int>(std::floor(std::min({ax, bx, qx})))), x1 = std::min(width - 1, static_cast<int>(std::ceil(std::max({ax, bx, qx}))));
		const int y0 = std::max(0, static_cast<int>(std::floor(std::min({ay, by, qy})))), y1 = std::min(height - 1, static_cast<int>(std::ceil(std::max({ay, by, qy}))));
		const float inv = 1.0f / area;
		for (int py = y0; py <= y1; py++) {
			const float ry = py + 0.5f;
			for (int px = x0; px <= x1; px++) {
				const float rx = px + 0.5f;
				const float w0 = ((qx - bx) * (ry - by) - (qy - by) * (rx - bx)) * inv;
				const float w1 = ((ax - qx) * (ry - qy) - (ay - qy) * (rx - qx)) * inv;
				const float w2 = 1.0f - w0 - w1;
				if (w0 < 0 || w1 < 0 || w2 < 0) continue;
				/* Perspective: 1/z goes evenly across the picture, the texture's place over z with it. */
				const float z_inv = w0 * iz[i0] + w1 * iz[i1] + w2 * iz[i2];
				const float near = z_inv * to_world;
				const size_t at = static_cast<size_t>(py) * width + px;
				if (near <= zb[at]) continue;
				const float u = (w0 * shape->uv[i0 * 2] * iz[i0] + w1 * shape->uv[i1 * 2] * iz[i1] + w2 * shape->uv[i2 * 2] * iz[i2]) / z_inv;
				const float v = (w0 * shape->uv[i0 * 2 + 1] * iz[i0] + w1 * shape->uv[i1 * 2 + 1] * iz[i1] + w2 * shape->uv[i2 * 2 + 1] * iz[i2]) / z_inv;
				const float uu = u - std::floor(u), vv = v - std::floor(v);
				const int tx = std::min(tex->w - 1, static_cast<int>(uu * tex->w)), ty = std::min(tex->h - 1, static_cast<int>(vv * tex->h));
				const uint32_t texel = tex->px[static_cast<size_t>(ty) * tex->w + tx];
				if (texel == 0) continue; // a hole in the texture
				const float k = w0 * light[i0] + w1 * light[i1] + w2 * light[i2];
				const Colour c(texel);
				dst[at] = Colour(static_cast<uint8_t>(std::min(255.0f, c.r * k)), static_cast<uint8_t>(std::min(255.0f, c.g * k)), static_cast<uint8_t>(std::min(255.0f, c.b * k))).data;
				zb[at] = near;
				drawn = true;
			}
		}
	}
	return drawn;
}

/** The rig: the objects' sprites made. */
std::string DescribeThreeDVehicleObjectSprites()
{
	std::string out = fmt::format("{} sprites asked for, {} pictures of the turned view", _object_sprites.size(), _object_pictures.size());
	for (const auto &[key, sprite] : _object_sprites) {
		out += fmt::format("\n  {} piece {} direction {}: sprite {}", std::get<0>(key)->name, std::get<1>(key), std::get<2>(key), sprite);
	}
	return out;
}
