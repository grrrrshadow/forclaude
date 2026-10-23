/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file three_d_preview_gui.cpp The 3D previews that turn (three_d.h,
 * gui.three_d): beside the purchase window the vehicle chosen there, big, in
 * the company's colours, turning smoothly as its 3D likeness made from its
 * eight sprites (three_d_vehicle.cpp); and beside the window of one of the
 * game's own industries the building as its 3D object (three_d_objects.h),
 * turning smoothly, degree by degree, drawn the way the map draws it.
 *
 * There is no window: the preview is a picture on a see-through background,
 * drawn over the map beside the window it belongs to (the player: a window
 * and buttons are not needed, it starts turning on a transparent background).
 * The main window draws it after its viewport (DrawThreeDPreviews() from
 * MainWindow::OnPaint()), a timer turns it and marks the screen under it
 * dirty, and it goes when its window goes. Any window over it is drawn over
 * it, as over the map.
 *
 * Each preview turns about its own axis: a frame is found once, the
 * rectangle its pictures at every angle fit in about the axis (the middle of
 * a vehicle, the middle of a building's plot), the frame is placed beside
 * the window, and every picture is drawn at the axis within it; so the thing
 * turns on the spot and the frame never moves (the player: the turning was
 * off its axis).
 */

#include "stdafx.h"
#include "three_d.h"
#include "three_d_objects.h"
#include "three_d_vehicle_object.h"
#include "window_gui.h"
#include "window_func.h"
#include "gfx_func.h"
#include "engine_base.h"
#include "engine_func.h"
#include "engine_gui.h"
#include "vehicle_base.h"
#include "vehicle_func.h"
#include "industry.h"
#include "industry_map.h"
#include "industrytype.h"
#include "spritecache.h"
#include "zoom_func.h"
#include "landscape.h"
#include "company_func.h"
#include "blitter/factory.hpp"
#include "timer/timer.h"
#include "timer/timer_window.h"

#include "table/sprites.h"

#include <map>
#include <tuple>

#include "safeguards.h"

/** How often the previews are looked at: turned on, and their window checked. */
static constexpr int PREVIEW_TICK_MS = 50;
/** A vehicle without a 3D likeness shows the next of its eight sprites this often. */
static constexpr int PREVIEW_VEHICLE_STEP_MS = 400;
/** A preview turns this many degrees a second. */
static constexpr float PREVIEW_DEG_PER_S = 30.0f;
/** A vehicle's preview stays this long in each direction the set has a picture for, showing that picture, and
 * turns between them this fast showing the likeness (the player: "use the pictures you have, the drawn ones only
 * where you have not got that angle"). */
static constexpr int PREVIEW_HOLD_MS = 1500;
static constexpr float PREVIEW_TURN_DEG_PER_S = 60.0f;
/** A vehicle drawn from its 3D object turns this many degrees a frame (the player, 17. 10.: "the turning is too jerky,
 * leave out every third frame, it will turn faster": 4.5 for 3), each frame made in the background beforehand. */
static constexpr float PREVIEW_OBJECT_STEP = 4.5f;
/** And this many frames ahead of the one shown are asked of the thread. */
static constexpr int PREVIEW_OBJECT_AHEAD = 6;
/** The gap between the window and its preview, pixels. */
static constexpr int PREVIEW_GAP = 40; // the player: the things turning almost touched the purchase window
/** A preview whose frame is wider or taller than this is drawn at the next coarser zoom (the player: a long wagon at 2x, not 4x). */
static constexpr int PREVIEW_MAX_SIZE = 360;

/** A preview beside a window. */
struct ThreeDPreview {
	bool vehicle;               ///< a vehicle beside the purchase window, else an industry beside its window
	WindowNumber parent;        ///< the window it belongs to: the purchase window's number, or the industry
	EngineID engine = EngineID::Invalid(); ///< the vehicle shown
	float yaw = 0.0f;           ///< the angle it is turned by, degrees, the way the map counts it
	int elapsed = 0;            ///< milliseconds since a vehicle without a likeness last turned on
	int hold = 0;               ///< milliseconds a vehicle stays yet in the direction of a picture of its set
	Rect drawn{};               ///< where it was last drawn on the screen, to mark dirty
	bool was_drawn = false;
	bool frame_ok = false;      ///< the frame is known
	Rect frame{};               ///< the rectangle the pictures fit in, about the axis
	ZoomLevel zoom = ZoomLevel::Normal; ///< the zoom the pictures are drawn at
	EngineID frame_engine = EngineID::Invalid(); ///< the engine the frame is for
	bool object_frame_made = false; ///< a vehicle drawn from its 3D object: the frame of its angle is made (drawn)
};

static std::vector<ThreeDPreview> _previews;
static std::unique_ptr<IntervalTimer<TimerWindow>> _preview_timer;

static Window *PreviewParent(const ThreeDPreview &p)
{
	return FindWindowById(p.vehicle ? WindowClass::BuildVehicle : WindowClass::IndustryView, p.parent);
}

/** Mark the screen under a preview dirty, so the main window draws it anew. */
static void DirtyPreview(ThreeDPreview &p)
{
	if (!p.was_drawn) return;
	AddDirtyBlock(p.drawn.left, p.drawn.top, p.drawn.right, p.drawn.bottom);
}

/**
 * Draw a picture with its alpha onto the screen, through the clipping of the window being drawn. Its pixels are no
 * more of the palette's animation: the water of the map under it was drawn over it again as the palette went round
 * (the player, 17. 10.: "the water on the ground is shown over the car of the preview").
 */
static void BlitPicture(const ThreeDModelPicture &pic, int px, int py)
{
	const DrawPixelInfo *dpi = _cur_dpi;
	Blitter *blitter = BlitterFactory::GetCurrentBlitter();
	int x0 = std::max(px, dpi->left), x1 = std::min(px + pic.width, dpi->left + dpi->width);
	int y0 = std::max(py, dpi->top), y1 = std::min(py + pic.height, dpi->top + dpi->height);
	/* Under it plain colours first (Blitter::RealizeAnimation()): what it is blended with is the colour shown. */
	if (x0 < x1 && y0 < y1) blitter->RealizeAnimation(blitter->MoveTo(dpi->dst_ptr, x0 - dpi->left, y0 - dpi->top), x1 - x0, y1 - y0);
	for (int y = y0; y < y1; y++) {
		const uint32_t *src = &pic.pixels[static_cast<size_t>(y - py) * pic.width];
		uint32_t *dst = static_cast<uint32_t *>(blitter->MoveTo(dpi->dst_ptr, x0 - dpi->left, y - dpi->top));
		for (int x = x0; x < x1; x++, dst++) {
			Colour sc(src[x - px]);
			if (sc.a == 0) continue;
			uint32_t out;
			if (sc.a == 255) {
				out = Colour(sc.r, sc.g, sc.b).data;
			} else {
				Colour d(*dst);
				uint al = sc.a, na = 255 - al;
				out = Colour(static_cast<uint8_t>((sc.r * al + d.r * na + 127) / 255), static_cast<uint8_t>((sc.g * al + d.g * na + 127) / 255), static_cast<uint8_t>((sc.b * al + d.b * na + 127) / 255)).data;
			}
			*dst = out;
		}
	}
}

/** The vehicle's sprites for the direction, as the map draws it (the purchase icon of a set is often one picture whatever the direction). */
static bool PreviewVehicleSprites(EngineID engine, Direction dir, VehicleSpriteSeq &front, VehicleSpriteSeq &rear, bool &two, int &pitch)
{
	two = false;
	pitch = 0;
	if (engine == EngineID::Invalid() || !Engine::IsValidID(engine)) return false;
	const Engine *e = Engine::Get(engine);
	switch (e->type) {
		case VehicleType::Train:
			GetTrainIcon(engine, false, dir, pitch, EngineImageType::OnMap, &front);
			if (e->VehInfo<RailVehicleInfo>().railveh_type == RailVehicleType::Multihead) {
				int pitch_rear = 0;
				GetTrainIcon(engine, true, ReverseDir(dir), pitch_rear, EngineImageType::OnMap, &rear);
				two = rear.IsValid();
			}
			break;
		case VehicleType::Road: GetRoadVehIcon(engine, dir, EngineImageType::OnMap, &front); break;
		case VehicleType::Ship: GetShipIcon(engine, dir, EngineImageType::OnMap, &front); break;
		case VehicleType::Aircraft: GetAircraftIcon(engine, dir, EngineImageType::OnMap, &front); break;
		default: return false;
	}
	return front.IsValid();
}

/** The bounds of sprites at a zoom, pixels about the point they are drawn at. */
struct PreviewBounds {
	int left = 0, top = 0, right = 0, bottom = 0;
	bool any = false;

	void Add(SpriteID sprite, ZoomLevel zoom, int dx = 0, int dy = 0)
	{
		const Sprite *spr = GetSprite(sprite & SPRITE_MASK, SpriteType::Normal);
		int l = UnScaleByZoom(spr->x_offs, zoom) + dx, t = UnScaleByZoom(spr->y_offs, zoom) + dy;
		int r = l + UnScaleByZoom(spr->width, zoom), b = t + UnScaleByZoom(spr->height, zoom);
		if (!this->any) {
			this->left = l; this->top = t; this->right = r; this->bottom = b;
			this->any = true;
		} else {
			this->left = std::min(this->left, l); this->top = std::min(this->top, t);
			this->right = std::max(this->right, r); this->bottom = std::max(this->bottom, b);
		}
	}
};

/** Where the two heads of a multiheaded engine stand, pixels of the zoom about the middle: half a vehicle along the direction each. */
static void PreviewHeadOffsets(Direction dir, ZoomLevel zoom, int &fx, int &fy)
{
	static const int dx[8] = {-1, 0, 1, 1, 1, 0, -1, -1}, dy[8] = {-1, -1, -1, 0, 1, 1, 1, 0};
	double len = (to_underlying(dir) & 1) ? 1.0 : std::sqrt(2.0);
	double half = TILE_SIZE / 4.0 / len;
	double wx = dx[to_underlying(dir)] * half, wy = dy[to_underlying(dir)] * half;
	double scale = 1.0 / (1 << to_underlying(zoom));
	fx = static_cast<int>(std::lround((wy - wx) * 2 * ZOOM_BASE * scale));
	fy = static_cast<int>(std::lround((wy + wx) * ZOOM_BASE * scale));
}

/**
 * The direction a vehicle without a likeness shows: by its own turning alone. The previews have nothing of the map's
 * view (the player, 17. 10.: "I turn the map to the left and they turn to the left; what has the preview got in common
 * with the rest, it is only to turn there").
 */
static Direction PreviewDirection(const ThreeDPreview &p)
{
	int steps = static_cast<int>(std::lround(p.yaw / 45.0f));
	return static_cast<Direction>((to_underlying(Direction::W) + steps) & 7);
}

/** The angle a vehicle's likeness is drawn at at a turn of the preview: the sprite of direction d shows at 90 - 45 d degrees. */
static float PreviewAzimuthAt(float yaw)
{
	return std::fmod(180.0f - yaw + 720.0f, 360.0f);
}

/** The angle a vehicle's likeness is drawn at: by the preview's own turning alone (PreviewDirection()). */
static float PreviewAzimuth(const ThreeDPreview &p)
{
	return PreviewAzimuthAt(p.yaw);
}

/**
 * Whether a vehicle has a picture for a direction: a set whose chain wants a
 * vehicle to choose from (the straight directions of a set that has only
 * the four along the track) gives an empty sprite, which the preview skips.
 */
static bool PreviewVehicleHasPicture(EngineID engine, Direction dir)
{
	VehicleSpriteSeq front, rear;
	bool two;
	int pitch;
	if (!PreviewVehicleSprites(engine, dir, front, rear, two, pitch)) return false;
	for (uint i = 0; i < front.count; i++) {
		const Sprite *spr = GetSprite(front.seq[i].sprite & SPRITE_MASK, SpriteType::Normal);
		if (spr->width > 4 * ZOOM_BASE || spr->height > 4 * ZOOM_BASE) return true;
	}
	return false;
}

/** The zoom a picture of this size at 16x fits PREVIEW_MAX_SIZE at: 4x, else 2x, else the normal. */
static ZoomLevel PreviewZoomFor(int width16, int height16)
{
	for (ZoomLevel zoom : {ZoomLevel::In4x, ZoomLevel::In2x}) {
		if (UnScaleByZoom(width16, zoom) <= PREVIEW_MAX_SIZE && UnScaleByZoom(height16, zoom) <= PREVIEW_MAX_SIZE) return zoom;
	}
	return ZoomLevel::Normal;
}

/**
 * Where a preview goes and how big it is: beside its window, to the right,
 * or to the left when there is no room there; the picture's middle at the
 * middle of the window's height.
 * @param p the preview
 * @param width,height the picture
 * @return the rectangle on the screen, or an empty one when the window is gone
 */
static Rect PreviewPlace(const ThreeDPreview &p, int width, int height)
{
	const Window *w = PreviewParent(p);
	if (w == nullptr) return {};
	int x = w->left + w->width + PREVIEW_GAP;
	if (x + width > _screen.width) x = std::max(0, w->left - PREVIEW_GAP - width);
	int y = Clamp(w->top + (w->height - height) / 2, 0, std::max(0, _screen.height - height));
	return {x, y, x + width, y + height};
}

/** The palette a vehicle preview is drawn in: the company's colours. */
static PaletteID PreviewPalette(const ThreeDPreview &p)
{
	return GetEnginePalette(p.engine, _local_company);
}

/** The likeness of the vehicle shown, when the blitter can draw it and the engine has its eight pictures. */
static const ThreeDVehicleModel *PreviewVehicleModel(const ThreeDPreview &p)
{
	if (p.engine == EngineID::Invalid() || !Engine::IsValidID(p.engine)) return nullptr;
	if (BlitterFactory::GetCurrentBlitter()->GetScreenDepth() != 32) return nullptr;
	return GetThreeDVehicleModel(p.engine, PreviewPalette(p));
}

/** Draw a vehicle without a likeness: its sprite of the direction; returns where it was drawn. */
static Rect DrawVehicleSprites(const ThreeDPreview &p, bool draw)
{
	VehicleSpriteSeq front, rear;
	bool two;
	int pitch;
	Direction dir = PreviewDirection(p);
	if (!PreviewVehicleSprites(p.engine, dir, front, rear, two, pitch)) return {};

	/* The size at 16x decides the zoom: the sprite's bounds, with the two heads apart. */
	PreviewBounds at16;
	int fx16 = 0, fy16 = 0;
	if (two) PreviewHeadOffsets(dir, ZoomLevel::In16x, fx16, fy16);
	for (uint i = 0; i < front.count; i++) at16.Add(front.seq[i].sprite, ZoomLevel::In16x, fx16, fy16);
	if (two) for (uint i = 0; i < rear.count; i++) at16.Add(rear.seq[i].sprite, ZoomLevel::In16x, -fx16, -fy16);
	if (!at16.any) return {};
	ZoomLevel zoom = PreviewZoomFor(at16.right - at16.left, at16.bottom - at16.top);

	PreviewBounds bounds;
	int fx = 0, fy = 0;
	if (two) PreviewHeadOffsets(dir, zoom, fx, fy);
	for (uint i = 0; i < front.count; i++) bounds.Add(front.seq[i].sprite, zoom, fx, fy);
	if (two) for (uint i = 0; i < rear.count; i++) bounds.Add(rear.seq[i].sprite, zoom, -fx, -fy);
	int width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
	Rect r = PreviewPlace(p, width, height);
	if (r.right <= r.left) return {};
	if (!draw) return r;

	PaletteID pal = PreviewPalette(p);
	int x = r.left - bounds.left, y = r.top - bounds.top + pitch;
	auto draw_seq = [&](const VehicleSpriteSeq &seq, int sx, int sy) {
		for (uint i = 0; i < seq.count; i++) {
			PaletteID pal_i = (pal == PALETTE_CRASH || seq.seq[i].pal == PAL_NONE) ? pal : seq.seq[i].pal;
			DrawSprite(seq.seq[i].sprite, pal_i, sx, sy, nullptr, zoom);
		}
	};
	/* The head further back is drawn first. */
	if (two && -fy < fy) {
		draw_seq(rear, x - fx, y - fy);
		draw_seq(front, x + fx, y + fy);
	} else {
		draw_seq(front, x + fx, y + fy);
		if (two) draw_seq(rear, x - fx, y - fy);
	}
	return r;
}

/**
 * The side of the square beside the purchase window a vehicle of a 3D object fills (the player, 17. 10.: "imagine the
 * window is there"): as tall as the window, as wide as there is room beside it on the screen.
 */
static int PreviewObjectSide(const ThreeDPreview &p)
{
	const Window *w = PreviewParent(p);
	if (w == nullptr) return 0;
	const int room = std::max(_screen.width - (w->left + w->width) - PREVIEW_GAP, w->left - PREVIEW_GAP);
	return std::min({std::max(w->height, 160), room, _screen.height});
}

/** Draw a vehicle preview: its likeness at the angle, turning about its middle; returns where it was drawn. */
static Rect DrawVehiclePreview(ThreeDPreview &p, bool draw)
{
	/* A vehicle of a 3D object: the object itself, filling the square beside the window, turning about its middle. */
	if (GetThreeDVehicleObject(p.engine) != nullptr && BlitterFactory::GetCurrentBlitter()->GetScreenDepth() == 32) {
		const int side = PreviewObjectSide(p);
		if (side > 0) {
			Rect r = PreviewPlace(p, side, side);
			if (r.right <= r.left || !draw) return r;
			int cx, cy;
			if (const ThreeDModelPicture *op = GetThreeDVehicleObjectPreviewFit(p.engine, side, PreviewAzimuth(p), cx, cy); op != nullptr) {
				BlitPicture(*op, r.left + side / 2 - cx, r.top + side / 2 - cy);
			}
			return r;
		}
	}
	const ThreeDVehicleModel *model = PreviewVehicleModel(p);
	if (model == nullptr) return DrawVehicleSprites(p, draw);

	if (!p.frame_ok || p.frame_engine != p.engine) {
		/* The zoom: 4x, or coarser until the frame fits. */
		ZoomLevel zoom = std::max(ZoomLevel::In4x, ThreeDVehicleModelZoom(model));
		for (;;) {
			Rect frame = ThreeDVehicleFrame(model, zoom);
			if (frame.right <= frame.left) return {};
			if ((frame.right - frame.left <= PREVIEW_MAX_SIZE && frame.bottom - frame.top <= PREVIEW_MAX_SIZE) || zoom >= ZoomLevel::Normal) {
				p.frame = frame;
				p.zoom = zoom;
				break;
			}
			++zoom;
		}
		p.frame_ok = true;
		p.frame_engine = p.engine;
	}
	Rect r = PreviewPlace(p, p.frame.right - p.frame.left, p.frame.bottom - p.frame.top);
	if (r.right <= r.left) return {};
	if (!draw) return r;

	int ax = r.left - p.frame.left, ay = r.top - p.frame.top;
	/* A vehicle of a 3D object: the object itself at the angle, as the turned map draws it. */
	if (const ThreeDModelPicture *op = GetThreeDVehicleObjectPreviewPicture(p.engine, p.zoom, PreviewAzimuth(p)); op != nullptr) {
		BlitPicture(*op, ax + op->left, ay + op->top);
		return r;
	}
	ThreeDModelPicture pic;
	/* On the track as the map draws it, so that the set's own pictures, shown in their directions, fit the likeness between. */
	if (!RenderThreeDVehicle(model, p.zoom, PreviewAzimuth(p), pic, true)) return r;
	BlitPicture(pic, ax + pic.left, ay + pic.top);
	return r;
}

/** A piece of an industry preview: a picture and where it goes, pixels about the industry's north corner. */
struct IndustryPiece {
	const ThreeDModelPicture *pic;
	int dx, dy;
	double near;
};

/**
 * The parts of a tile of an industry of the original game, as the map draws it from the parts the industries share
 * (GetThreeDOriginalIndustryTile()); nullptr for a tile not the industry's, of a set (NewGRF), or one the sets have none for.
 */
static const ThreeDIndustryTileParts *OriginalIndustryTileOf(const Industry *ind, TileIndex tile)
{
	if (!IsTileType(tile, TileType::Industry) || GetIndustryIndex(tile) != ind->index) return nullptr;
	const IndustryGfx gfx = GetIndustryGfx(tile);
	if (gfx >= NEW_INDUSTRYTILEOFFSET || GetIndustryTileSpec(gfx)->grf_prop.HasGrfFile()) return nullptr;
	return GetThreeDOriginalIndustryTile(gfx);
}

/** Whether an industry of the original game has tiles drawn from the parts the industries share (the forest). */
static bool HasOriginalIndustryParts(const Industry *ind)
{
	for (TileIndex tile : ind->location) {
		if (OriginalIndustryTileOf(ind, tile) != nullptr) return true;
	}
	return false;
}

/** The pictures of the industries' objects at the angles the previews were at, by the industry, the angle (tenths of a degree) and the zoom. */
static std::map<std::tuple<uint, int, int, size_t>, ThreeDModelPicture> _preview_object_pictures;

/**
 * The 3D objects an industry of the game's own is laid from at an angle (three_d_objects.h): its building, or the
 * plantation's tiles from the parts of the fields, far first, world units about its north corner.
 * @param[out] tiles_x,tiles_y the size of its plot
 * @param[out] top how tall it is, units of height
 * @return whether it has objects
 */
static bool IndustryPlacements(const Industry *ind, float yaw, std::vector<ThreeDObjectPlacement> &parts, int &tiles_x, int &tiles_y, int &top)
{
	parts.clear();
	tiles_x = ind->location.w;
	tiles_y = ind->location.h;
	top = 0;
	const int base_z = GetTileMaxPixelZ(ind->location.tile);
	const ThreeDObject *obj = GetThreeDIndustryObjectOf(ind);
	const TileIndex origin = obj != nullptr ? GetThreeDIndustryObjectOrigin(ind) : INVALID_TILE;
	if (obj != nullptr && origin == ind->location.tile && obj->tiles_x == ind->location.w && obj->tiles_y == ind->location.h) {
		ThreeDObjectPlacement &p = parts.emplace_back();
		p.obj = obj;
		p.paint = obj->PaintAt(ind->location.tile);
		p.more = GetThreeDIndustryGirls(ind, obj);
		tiles_x = obj->tiles_x;
		tiles_y = obj->tiles_y;
		top = static_cast<int>(std::ceil(obj->top));
		return true;
	}
	const double a = yaw * M_PI / 180.0, c = std::cos(a), s = std::sin(a);
	std::vector<std::pair<double, ThreeDObjectPlacement>> laid;
	auto lay = [&](TileIndex at, const ThreeDObject *o, const ThreeDObject *ground, std::span<const ThreeDObjectFigure> more, double bias) {
		ThreeDObjectPlacement p;
		p.obj = o;
		p.ground = ground;
		p.more = more;
		p.x = (static_cast<int>(TileX(at)) - static_cast<int>(TileX(ind->location.tile))) * TILE_SIZE;
		p.y = (static_cast<int>(TileY(at)) - static_cast<int>(TileY(ind->location.tile))) * TILE_SIZE;
		p.z = GetTilePixelZ(at) - base_z;
		double mx = p.x + TILE_SIZE / 2.0, my = p.y + TILE_SIZE / 2.0;
		laid.emplace_back((c * mx - s * my) + (s * mx + c * my) + bias, p);
		if (o != nullptr) top = std::max(top, static_cast<int>(std::ceil(o->top)));
	};
	if (obj != nullptr) {
		/* The refinery as the distillery (GetThreeDIndustryYard()): the distillery on its square, the refinery's tanks and columns on the other tiles. */
		for (TileIndex tile : ind->location) {
			if (!IsTileType(tile, TileType::Industry) || GetIndustryIndex(tile) != ind->index) continue;
			const ThreeDIndustryTileParts *yard = GetThreeDIndustryYard(ind, tile);
			if (yard == nullptr) continue;
			if (yard->ground != nullptr) lay(tile, yard->ground, nullptr, {}, -1e6);
			for (const ThreeDObject *part : yard->parts) lay(tile, part, nullptr, {}, 0);
		}
		lay(origin, obj, nullptr, GetThreeDIndustryGirls(ind, obj), 0);
		laid.back().second.paint = obj->PaintAt(origin);
	} else if (ind->type != IT_MARIJUANA_PLANTATION && ind->type != IT_POTATO_FIELD) {
		/* An industry of the original game from the parts the industries share, as the map lays it
		 * (AddOriginalIndustryTile() of viewport.cpp): each tile's ground, first; the parts and the plants of each
		 * built tile that is no member of a block (a block's anchor stands for all of it). The forest's trees on each
		 * of its tiles: before, the preview knew only an industry of one object and the two fields, and the forest's
		 * did not open (the player, 23. 10.: "the 3D preview by the window of the forest did not work"). */
		for (TileIndex tile : ind->location) {
			const ThreeDIndustryTileParts *tp = OriginalIndustryTileOf(ind, tile);
			if (tp == nullptr) continue;
			if (tp->ground != nullptr) lay(tile, tp->ground, nullptr, {}, -1e6);
			if (tp->member || !IsIndustryCompleted(tile)) continue;
			for (const ThreeDObject *part : tp->parts) lay(tile, part, nullptr, {}, 0);
			if (tp->plants != nullptr) lay(tile, tp->plants, nullptr, {}, 0);
		}
	}
	for (TileIndex tile : ind->location) {
		ThreeDPlantationTile pt;
		ThreeDFieldTile ft;
		if (ind->type != IT_MARIJUANA_PLANTATION && ind->type != IT_POTATO_FIELD) break;
		if (!IsTileType(tile, TileType::Industry) || GetIndustryIndex(tile) != ind->index || !GetThreeDPlantationTile(tile, pt) || !GetThreeDFieldTile(pt, ft)) continue;
		lay(tile, ft.plants != nullptr ? ft.plants : ft.soil, ft.plants != nullptr ? ft.soil : nullptr, ft.worker, 0);
		if (ft.road != nullptr) {
			const TileIndex road_tile = TileAddXY(tile, 0, 1);
			lay(road_tile, ft.road, nullptr, {}, -static_cast<double>(TILE_SIZE));
			if (ft.shed != nullptr) lay(road_tile, ft.shed, nullptr, {}, 0);
			if (ft.hay != nullptr) lay(road_tile, ft.hay, nullptr, {}, 0);
			for (int edge = 0; edge < 2; edge++) {
				if (!ft.girls[edge].empty()) lay(road_tile, nullptr, nullptr, ft.girls[edge], 0.5);
			}
		}
	}
	if (laid.empty()) return false;
	std::stable_sort(laid.begin(), laid.end(), [](const auto &l, const auto &r) { return l.first < r.first; });
	for (const auto &[near, p] : laid) parts.push_back(p);
	top = std::max(top, 2 * static_cast<int>(TILE_HEIGHT));
	return true;
}

/**
 * The pieces of an industry preview at an angle: its 3D objects laid in one picture (RenderThreeDObjectsPicture()),
 * about the industry's north corner.
 * @param[out] axis where the middle of the plot is at the angle, pixels about the north corner
 * @return whether there is anything to draw
 */
static bool IndustryPieces(const Industry *ind, float yaw, ZoomLevel &zoom, std::vector<IndustryPiece> &pieces, Point &axis)
{
	pieces.clear();
	double a = yaw * M_PI / 180.0, c = std::cos(a), s = std::sin(a);
	std::vector<ThreeDObjectPlacement> parts;
	int tiles_x, tiles_y, top;
	if (!IndustryPlacements(ind, yaw, parts, tiles_x, tiles_y, top)) return false;
	/* The zoom it fits the preview at: its plot across, and its height. */
	const int span = (tiles_x + tiles_y) * TILE_SIZE;
	zoom = PreviewZoomFor(span * 2 * ZOOM_BASE, (span + top) * ZOOM_BASE);
	/* Which figures are there (the girls come and go with the deliveries), so a picture without them is not drawn for one with them. */
	size_t figures = 0;
	for (const ThreeDObjectPlacement &p : parts) figures = figures * 31 + reinterpret_cast<uintptr_t>(p.more.data()) + p.more.size();
	const auto key = std::make_tuple(ind->index.base(), static_cast<int>(std::lround(std::fmod(yaw + 720.0f, 360.0f) * 10)), to_underlying(zoom), figures);
	auto it = _preview_object_pictures.find(key);
	if (it == _preview_object_pictures.end()) {
		if (_preview_object_pictures.size() > 64) _preview_object_pictures.clear();
		ThreeDModelPicture pic;
		RenderThreeDObjectsPicture(parts, yaw, zoom, pic);
		it = _preview_object_pictures.emplace(key, std::move(pic)).first;
	}
	if (it->second.width <= 0) return false;
	pieces.push_back({&it->second, 0, 0, 0.0});

	/* The axis: the middle of the plot, turned with it. */
	double scale = 1.0 / (1 << to_underlying(zoom));
	double cx = tiles_x * TILE_SIZE / 2.0, cy = tiles_y * TILE_SIZE / 2.0;
	double xr = c * cx - s * cy, yr = s * cx + c * cy;
	axis = {static_cast<int>(std::lround((yr - xr) * 2 * ZOOM_BASE * scale)), static_cast<int>(std::lround((yr + xr) * ZOOM_BASE * scale))};
	return true;
}

/** The rectangle pieces fill, about the axis. */
static Rect PiecesRect(const std::vector<IndustryPiece> &pieces, Point axis)
{
	Rect r{INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
	for (const IndustryPiece &pc : pieces) {
		r.left = std::min(r.left, pc.dx + pc.pic->left - axis.x);
		r.top = std::min(r.top, pc.dy + pc.pic->top - axis.y);
		r.right = std::max(r.right, pc.dx + pc.pic->left + pc.pic->width - axis.x);
		r.bottom = std::max(r.bottom, pc.dy + pc.pic->top + pc.pic->height - axis.y);
	}
	return r;
}

/** Draw an industry preview: the building's picture at the preview's angle, turning about the middle of its plot; returns where. */
static Rect DrawIndustryPreview(ThreeDPreview &p, bool draw)
{
	IndustryID id = static_cast<IndustryID>(p.parent);
	if (!Industry::IsValidID(id)) return {};
	const Industry *ind = Industry::Get(id);
	if (BlitterFactory::GetCurrentBlitter()->GetScreenDepth() != 32) return {};

	std::vector<IndustryPiece> pieces;
	Point axis;
	ZoomLevel zoom;
	if (!p.frame_ok) {
		/* The frame: the pictures of eight angles about the axis. */
		Rect frame{INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
		for (int i = 0; i < 8; i++) {
			if (!IndustryPieces(ind, i * 45.0f + 22.5f, zoom, pieces, axis)) return {};
			Rect r = PiecesRect(pieces, axis);
			frame.left = std::min(frame.left, r.left); frame.top = std::min(frame.top, r.top);
			frame.right = std::max(frame.right, r.right); frame.bottom = std::max(frame.bottom, r.bottom);
		}
		p.frame = frame;
		p.zoom = zoom;
		p.frame_ok = true;
	}
	Rect r = PreviewPlace(p, p.frame.right - p.frame.left, p.frame.bottom - p.frame.top);
	if (r.right <= r.left) return {};
	if (!draw) return r;

	/* The angle: the preview's own turn, nothing of the map's view (PreviewDirection()). */
	float yaw = std::fmod(p.yaw, 360.0f);
	if (!IndustryPieces(ind, yaw, zoom, pieces, axis)) return r;
	int ax = r.left - p.frame.left - axis.x, ay = r.top - p.frame.top - axis.y;
	for (const IndustryPiece &pc : pieces) BlitPicture(*pc.pic, ax + pc.dx + pc.pic->left, ay + pc.dy + pc.pic->top);
	return r;
}

/** Where a preview is now, without drawing it. */
static Rect PreviewRect(ThreeDPreview &p)
{
	return p.vehicle ? DrawVehiclePreview(p, false) : DrawIndustryPreview(p, false);
}

/** The timer: every PREVIEW_TICK_MS each preview turns on, follows its window and goes when the window has gone. */
static void PreviewTick()
{
	for (auto it = _previews.begin(); it != _previews.end();) {
		ThreeDPreview &p = *it;
		if (PreviewParent(p) == nullptr || !ThreeDEnabled()) {
			DirtyPreview(p);
			it = _previews.erase(it);
			/* No vehicle shown: the frames of its turn go. */
			if (std::ranges::none_of(_previews, [](const ThreeDPreview &q) { return q.vehicle; })) ForgetThreeDVehicleObjectPreviews();
			continue;
		}
		bool changed = false;
		if (p.vehicle) {
			EngineID engine = GetBuildVehicleSelectedEngine(p.parent);
			if (engine != p.engine) {
				p.engine = engine;
				p.frame_ok = false;
				p.object_frame_made = false;
				changed = true;
			}
			const int side = BlitterFactory::GetCurrentBlitter()->GetScreenDepth() == 32 && GetThreeDVehicleObject(p.engine) != nullptr ? PreviewObjectSide(p) : 0;
			if (side > 0) {
				/* A vehicle drawn from its 3D object (DrawVehiclePreview()): no likeness made of its sprites (it was,
				 * a second and its eight sprites at 16x each time one was chosen, for nothing drawn). Its frames are
				 * made in the background a few ahead; it turns on to the next when it is there, and waits for it
				 * else -- never the game standing while one is drawn. */
				if (changed) p.yaw = std::fmod(PREVIEW_OBJECT_STEP * std::round(p.yaw / PREVIEW_OBJECT_STEP), 360.0f);
				bool here = false, next = false;
				for (int k = 0; k <= PREVIEW_OBJECT_AHEAD; k++) {
					const bool made = ThreeDVehicleObjectPreviewFitMade(p.engine, side, PreviewAzimuthAt(p.yaw + k * PREVIEW_OBJECT_STEP));
					if (k == 0) here = made;
					if (k == 1) next = made;
				}
				if (here && !p.object_frame_made) {
					p.object_frame_made = true;
					changed = true;
				}
				if (here && next) {
					p.yaw = std::fmod(p.yaw + PREVIEW_OBJECT_STEP, 360.0f);
					changed = true;
				}
			} else if (PreviewVehicleModel(p) != nullptr) {
				/* Held in each direction of a sprite (its own picture), turned on to the next (the likeness). The
				 * angle drawn falls as the preview turns, so the next direction is the multiple of 45 below it. */
				if (p.hold > 0) {
					p.hold -= PREVIEW_TICK_MS;
				} else {
					float turn = PREVIEW_TURN_DEG_PER_S * PREVIEW_TICK_MS / 1000.0f;
					float off = std::fmod(PreviewAzimuth(p), 45.0f);
					float to_next = off < 0.01f ? 45.0f : off;
					if (turn >= to_next - 0.01f) {
						p.yaw = std::fmod(p.yaw + to_next, 360.0f);
						p.hold = PREVIEW_HOLD_MS;
					} else {
						p.yaw = std::fmod(p.yaw + turn, 360.0f);
					}
					changed = true;
				}
			} else {
				p.elapsed += PREVIEW_TICK_MS;
				if (p.elapsed >= PREVIEW_VEHICLE_STEP_MS || changed) {
					p.elapsed = 0;
					/* The next direction that has a picture, so a set with only the four along the track still turns. */
					p.yaw = 45.0f * std::lround(p.yaw / 45.0f);
					for (int tries = 0; tries < 8; tries++) {
						if (p.elapsed == 0 && tries == 0 && changed && PreviewVehicleHasPicture(p.engine, PreviewDirection(p))) break;
						p.yaw = std::fmod(p.yaw + 45.0f, 360.0f);
						if (PreviewVehicleHasPicture(p.engine, PreviewDirection(p))) break;
					}
					changed = true;
				}
			}
		} else {
			p.yaw = std::fmod(p.yaw + PREVIEW_DEG_PER_S * PREVIEW_TICK_MS / 1000.0f, 360.0f);
			changed = true;
		}
		Rect now = PreviewRect(p);
		if (changed || now.left != p.drawn.left || now.top != p.drawn.top || now.right != p.drawn.right || now.bottom != p.drawn.bottom) {
			DirtyPreview(p);
			if (now.right > now.left) AddDirtyBlock(now.left, now.top, now.right, now.bottom);
		}
		++it;
	}
	if (_previews.empty()) _preview_timer.reset();
}

/** Draw every preview there is, over the main window's viewport (MainWindow::OnPaint()). */
void DrawThreeDPreviews()
{
	if (_game_mode != GameMode::Normal) return;
	for (ThreeDPreview &p : _previews) {
		Rect r = p.vehicle ? DrawVehiclePreview(p, true) : DrawIndustryPreview(p, true);
		if (r.right > r.left) {
			p.drawn = r;
			p.was_drawn = true;
		}
	}
}

static void AddPreview(ThreeDPreview preview)
{
	for (ThreeDPreview &p : _previews) {
		if (p.vehicle == preview.vehicle && p.parent == preview.parent) return;
	}
	_previews.push_back(preview);
	if (_preview_timer == nullptr) {
		_preview_timer = std::make_unique<IntervalTimer<TimerWindow>>(std::chrono::milliseconds(PREVIEW_TICK_MS), [](auto) { PreviewTick(); });
	}
	PreviewTick();
}

/**
 * Show the preview beside a purchase window, when the 3D is on.
 * @param parent_number the purchase window's number
 */
void ShowThreeDVehiclePreview(WindowNumber parent_number)
{
	if (!ThreeDEnabled()) return;
	AddPreview({true, parent_number});
}

/**
 * Show the preview beside an industry's window, when the 3D is on and the
 * industry is one of the game's own drawn as a 3D object, a field, or one of the original game's laid from the parts.
 * @param industry the industry
 */
void ShowThreeDIndustryPreview(IndustryID industry)
{
	if (!ThreeDEnabled() || !Industry::IsValidID(industry)) return;
	const Industry *ind = Industry::Get(industry);
	const bool field = (ind->type == IT_MARIJUANA_PLANTATION || ind->type == IT_POTATO_FIELD) && GetThreeDFieldPart("zem") != nullptr;
	if (GetThreeDIndustryObjectOf(ind) == nullptr && !field && !HasOriginalIndustryParts(ind)) return;
	AddPreview({false, static_cast<WindowNumber>(industry)});
}

/** Rig: let the previews look at their windows now (the null driver's timers run only between ticks). */
void ThreeDPreviewsTick()
{
	PreviewTick();
}

/** Rig: turn every preview to an angle, and have it drawn anew. */
void SetThreeDPreviewYaw(float yaw)
{
	for (ThreeDPreview &p : _previews) {
		p.yaw = std::fmod(yaw + 720.0f, 360.0f);
		DirtyPreview(p);
		Rect now = PreviewRect(p);
		if (now.right > now.left) AddDirtyBlock(now.left, now.top, now.right, now.bottom);
	}
}

/** Rig: the previews there are, where they are drawn. */
std::string DescribeThreeDPreviews()
{
	std::string s;
	for (ThreeDPreview &p : _previews) {
		Rect r = PreviewRect(p);
		s += fmt::format("{}{} at {},{}..{},{} yaw {:.0f} zoom {}; ", p.vehicle ? "vehicle " : "industry ", p.vehicle ? static_cast<int32_t>(p.engine.base()) : static_cast<int32_t>(p.parent), r.left, r.top, r.right, r.bottom, p.yaw, to_underlying(p.zoom));
		if (p.vehicle && p.engine != EngineID::Invalid()) {
			/* (A vehicle drawn from its 3D object has no likeness: none made for it here either.) */
			if (GetThreeDVehicleObject(p.engine) != nullptr) {
				s += "3D object; ";
				continue;
			}
			s += fmt::format("model: {}; ", DescribeThreeDVehicleModel(PreviewVehicleModel(p)));
			/* The sprites of every direction, with their size: a set whose chain needs a vehicle gives empty ones. */
			for (int d = 0; d < 8; d++) {
				VehicleSpriteSeq front, rear;
				bool two;
				int pitch;
				if (!PreviewVehicleSprites(p.engine, static_cast<Direction>(d), front, rear, two, pitch)) { s += fmt::format("d{} none; ", d); continue; }
				s += fmt::format("d{}:", d);
				for (uint i = 0; i < front.count; i++) {
					const Sprite *spr = GetSprite(front.seq[i].sprite & SPRITE_MASK, SpriteType::Normal);
					s += fmt::format(" {}({}x{})", front.seq[i].sprite & SPRITE_MASK, spr->width, spr->height);
				}
				s += "; ";
			}
		}
	}
	return s.empty() ? "no previews" : s;
}
