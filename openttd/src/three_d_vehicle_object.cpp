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
#include "debug.h"
#include "settings_type.h"
#include "timer/timer_game_calendar.h"

#include <chrono>
#include <cmath>
#include <map>

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
	int fade = 0;                             ///< the step of its paint's fading (FadeStep())
};
static std::vector<ObjectSpriteRequest> _object_sprite_requests;
/** The sprites made: by the object, the piece it is hung from and the direction. */
static std::map<std::tuple<const ThreeDVehicleObject *, int, int, bool, int>, SpriteID> _object_sprites;
static SpriteID _object_empty_sprite = 0;

/** A picture of an object in the turned view, kept. */
struct ObjectPictureEntry {
	uint64_t used;
	ThreeDModelPicture picture;
};
static std::map<std::tuple<const ThreeDVehicleObject *, int, int, int, int, int>, ObjectPictureEntry> _object_pictures;
static uint64_t _object_pictures_clock = 0;

/** Forget the sprites made: the sprite cache they were in is gone (GfxInitSpriteMem()). */
void ResetThreeDVehicleObjectSprites()
{
	_object_sprite_requests.clear();
	_object_sprites.clear();
	_object_empty_sprite = 0;
	_object_pictures.clear();
}

/** Whether a vehicle of the type is foreshortened along itself on straight track, as the sets draw trains and road vehicles. */
static inline bool SqueezedType(VehicleType type)
{
	return type == VehicleType::Train || type == VehicleType::Road;
}

/**
 * The scale the objects are drawn at, all of them: the size of the setting -- the original game's, as the bigger
 * sets draw (BRYLE), 1.2, or CZTR's, 1 (the player, 14. 10.: "the game can make the object bigger, so there need
 * not be two files"). The objects are in true metres.
 */
static float ObjectScale(const ThreeDVehicleObject *)
{
	return _settings_game.vehicle.object_original_size ? 1.2f : 1.0f;
}

/** How far a vehicle's paint has faded, in steps of its "krok" years: by its age, to the target in "let" years; 0 new or without fading. */
static int FadeStep(const ThreeDVehicleObject *obj, const Vehicle *v)
{
	if (!obj->paint.fades || v == nullptr) return 0;
	int years = std::max(0, static_cast<int>(v->age.base() / CalendarTime::DAYS_IN_YEAR));
	return std::min(years / obj->paint.step, obj->paint.years / obj->paint.step);
}

/** The fading of a step: 0 new, 1 the target's colour. */
static float FadeAmount(const ThreeDVehicleObject *obj, int step)
{
	return std::min(1.0f, static_cast<float>(step * obj->paint.step) / obj->paint.years);
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
	int total = static_cast<int>(std::lround((shape->x1 - shape->x0) * ObjectScale(obj) * UNITS_PER_METRE / M_SQRT2));
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
	auto key = std::make_tuple(obj, piece, dir, _settings_game.vehicle.object_original_size, fade);
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
	SpriteID sprite = piece == from ? ObjectSprite(obj, from, to_underlying(direction) & 7, FadeStep(obj, v)) : EmptyObjectSprite();
	if (sprite == 0) return false;
	result->Set(sprite);
	return true;
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
 */
static bool RasterObject(const ThreeDVehicleObject &obj, float scale, float cam_az, float world_az, float along, float pxm, float fade, ObjectRaster &out)
{
	const ThreeDVehicleShape *shape = obj.GetShape();
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
	for (size_t i = 0; i < n; i++) {
		float x = shape->pos[i * 3] * scale * along, y = shape->pos[i * 3 + 1] * scale, z = shape->pos[i * 3 + 2] * scale;
		float xr = x * ca - y * sa, yr = x * sa + y * ca;
		sx[i] = xr * p;
		sy[i] = (-yr * CAM_SIN - z * CAM_COS) * p;
		depth[i] = yr * CAM_COS - z * CAM_SIN;
		minx = std::min(minx, sx[i]); maxx = std::max(maxx, sx[i]);
		miny = std::min(miny, sy[i]); maxy = std::max(maxy, sy[i]);
		if (lit) {
			float nx = shape->nrm[i * 3], ny = shape->nrm[i * 3 + 1], nz = shape->nrm[i * 3 + 2];
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
	const ThreeDObjectTexture *mask = (paint.fades || paint.coloured) ? obj.GetPaintMask(level) : nullptr;
	const Colour target(paint.target), colour(paint.colour);
	const float start = paint.start > 0 ? static_cast<float>(paint.start) : 255.0f;

	std::vector<float> zb(static_cast<size_t>(width) * height, std::numeric_limits<float>::infinity());
	std::vector<Colour> buf(static_cast<size_t>(width) * height, Colour(0, 0, 0, 0));
	for (size_t t = 0; t + 2 < shape->tri.size(); t += 3) {
		const uint32_t a = shape->tri[t], b = shape->tri[t + 1], c = shape->tri[t + 2];
		const int ti = t / 3 < shape->tri_tex.size() ? shape->tri_tex[t / 3] : 0;
		const ThreeDObjectTexture *tex = texs[static_cast<size_t>(ti) < texs.size() ? ti : 0];
		const ThreeDObjectTexture *tri_mask = ti == 0 ? mask : nullptr;
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
						if (paint.coloured) {
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
		drawn = RasterObject(*req->obj, req->scale, az, az, along, PXM_16X, FadeAmount(req->obj, req->fade), r) && r.width <= UINT16_MAX && r.height <= UINT16_MAX;
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

/** A picture of an object in the turned view at an angle and zoom, kept (RasterObject()). */
static const ThreeDModelPicture *ObjectPicture(const ThreeDVehicleObject *obj, float scale, ZoomLevel zoom, float cam_az, float world_az, bool squeeze, int fade)
{
	int cam_step = static_cast<int>(std::lround(std::fmod(cam_az + 720.0f, 360.0f))) % 360;
	int world_step = static_cast<int>(std::lround(std::fmod(world_az + 720.0f, 360.0f))) % 360;
	auto key = std::make_tuple(obj, to_underlying(zoom), cam_step, world_step, static_cast<int>(std::lround(scale * 1000)), fade);
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
		uint64_t limit = ages[ages.size() / 2];
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
	if (RasterObject(*obj, scale, static_cast<float>(cam_step), static_cast<float>(world_step), along, PXM_16X / (1 << to_underlying(zoom)), FadeAmount(obj, fade), r)) {
		Debug(sprite, 4, "3D vehicle {}: picture at {} degrees, zoom {}: {}x{}, {} ms", obj->name, cam_step, to_underlying(zoom), r.width, r.height,
				std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
		e.picture.width = r.width;
		e.picture.height = r.height;
		e.picture.left = r.left;
		e.picture.top = r.top;
		e.picture.pixels.resize(r.px.size());
		for (size_t i = 0; i < r.px.size(); i++) e.picture.pixels[i] = r.px[i].a == 0 ? 0 : r.px[i].data;
	}
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
	const ThreeDModelPicture *pic = ObjectPicture(obj, scale, zoom, world_az + GetViewYaw(), world_az, SqueezedType(obj->type), FadeStep(obj, v));
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

/** The rig: the objects' sprites made. */
std::string DescribeThreeDVehicleObjectSprites()
{
	std::string out = fmt::format("{} sprites asked for, {} pictures of the turned view", _object_sprites.size(), _object_pictures.size());
	for (const auto &[key, sprite] : _object_sprites) {
		out += fmt::format("\n  {} piece {} direction {}: sprite {}", std::get<0>(key)->name, std::get<1>(key), std::get<2>(key), sprite);
	}
	return out;
}
