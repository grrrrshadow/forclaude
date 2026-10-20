/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file viewport.cpp Handling of all viewports.
 *
 * \verbatim
 * The in-game coordinate system looks like this *
 *                                               *
 *                    ^ Z                        *
 *                    |                          *
 *                    |                          *
 *                    |                          *
 *                    |                          *
 *                 /     \                       *
 *              /           \                    *
 *           /                 \                 *
 *        /                       \              *
 *   X <                             > Y         *
 * \endverbatim
 */

/**
 * @defgroup vp_column_row Rows and columns in the viewport
 *
 * Columns are vertical sections of the viewport that are half a tile wide.
 * The origin, i.e. column 0, is through the northern and southern most tile.
 * This means that the column of e.g. Tile(0, 0) and Tile(100, 100) are in
 * column number 0. The negative columns are towards the left of the screen,
 * or towards the west, whereas the positive ones are towards respectively
 * the right and east.
 * With half a tile wide is meant that the next column of tiles directly west
 * or east of the centre line are respectively column -1 and 1. Their tile
 * centers are only half a tile from the center of their adjoining tile when
 * looking only at the X-coordinate.
 *
 * \verbatim
 *        ╳        *
 *       ╱ ╲       *
 *      ╳ 0 ╳      *
 *     ╱ ╲ ╱ ╲     *
 *    ╳-1 ╳ 1 ╳    *
 *   ╱ ╲ ╱ ╲ ╱ ╲   *
 *  ╳-2 ╳ 0 ╳ 2 ╳  *
 *   ╲ ╱ ╲ ╱ ╲ ╱   *
 *    ╳-1 ╳ 1 ╳    *
 *     ╲ ╱ ╲ ╱     *
 *      ╳ 0 ╳      *
 *       ╲ ╱       *
 *        ╳        *
 * \endverbatim
 *
 *
 * Rows are horizontal sections of the viewport, also half a tile wide.
 * This time the northern most tile on the map defines 0 and
 * everything south of that has a positive number.
 */

#include "stdafx.h"
#include "core/backup_type.hpp"
#include "blueprint_gui.h"
#include "landscape.h"
#include "viewport_func.h"
#include "station_base.h"
#include "waypoint_base.h"
#include "town.h"
#include "signs_base.h"
#include "signs_func.h"
#include "vehicle_base.h"
#include "aircraft.h"
#include "train.h"
#include "timer/timer_game_tick.h"
#include "vehicle_gui.h"
#include "blitter/factory.hpp"
#include "strings_func.h"
#include "zoom_func.h"
#include "vehicle_func.h"
#include "company_func.h"
#include "track_func.h"
#include "waypoint_func.h"
#include "window_func.h"
#include "tilehighlight_func.h"
#include "window_gui.h"
#include "linkgraph/linkgraph_gui.h"
#include "viewport_kdtree.h"
#include "town_kdtree.h"
#include "viewport_sprite_sorter.h"
#include "bridge_map.h"
#include "tunnel_map.h"
#include "tunnelbridge_map.h"
#include "rail.h"
#include "rail_map.h"
#include "road_map.h"
#include "water_map.h"
#include "clear_map.h"
#include "tree_map.h"
#include "newgrf_railtype.h"
#include "company_base.h"
#include "command_func.h"
#include "network/network_func.h"
#include "framerate_type.h"
#include "viewport_cmd.h"
#include "three_d.h"
#include "three_d_objects.h"
#include "three_d_gpu.h"
#include "three_d_vehicle_object.h"
#include "three_d_dragons.h"
#include "three_d_animals.h"
#include "three_d_witches.h"
#include "three_d_walkers.h"
#include "cab_view.h"
#include "cab_drive.h"
#include "thread.h"
#include "round_track.h"

extern bool _draw_fences; ///< clear_cmd.cpp: the fences of fields and rails, off while the 3D views collect
#include "fileio_type.h"
#include "transparency.h"
#include "console_func.h"
#include "industry.h"
#include "industry_map.h"
#include "industrytype.h"

#include <chrono>
#include <cmath>
#include <numbers>
#include <forward_list>
#include <optional>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <map>
#include <tuple>
#include <atomic>
#include <functional>
#include <stack>
#include <deque>
#include <memory>

#include "widgets/vehicle_widget.h"

#include "table/strings.h"
#include "table/string_colours.h"

#include "safeguards.h"

Point _tile_fract_coords;


ViewportSignKdtree _viewport_sign_kdtree{};
static int _viewport_sign_maxwidth = 0;


static const int MAX_TILE_EXTENT_LEFT   = ZOOM_BASE * TILE_PIXELS;                     ///< Maximum left   extent of tile relative to north corner.
static const int MAX_TILE_EXTENT_RIGHT  = ZOOM_BASE * TILE_PIXELS;                     ///< Maximum right  extent of tile relative to north corner.
static const int MAX_TILE_EXTENT_TOP    = ZOOM_BASE * MAX_BUILDING_PIXELS;             ///< Maximum top    extent of tile relative to north corner (not considering bridges).
static const int MAX_TILE_EXTENT_BOTTOM = ZOOM_BASE * (TILE_PIXELS + 2 * TILE_HEIGHT); ///< Maximum bottom extent of tile relative to north corner (worst case: #SLOPE_STEEP_N).

struct StringSpriteToDraw {
	std::string string;
	uint16_t width;
	Colours colour;
	ViewportStringFlags flags;
	int32_t x;
	int32_t y;
};

struct TileSpriteToDraw {
	SpriteID image;
	PaletteID pal;
	const SubSprite *sub;           ///< only draw a rectangular part of the sprite
	int32_t x;                        ///< screen X coordinate of sprite
	int32_t y;                        ///< screen Y coordinate of sprite
};

struct ChildScreenSpriteToDraw {
	SpriteID image;
	PaletteID pal;
	const SubSprite *sub;           ///< only draw a rectangular part of the sprite
	int32_t x;
	int32_t y;
	bool relative;
	int next;                       ///< next child to draw (-1 at the end)
};

/** Enumeration of multi-part foundations */
enum class FoundationPart : uint8_t {
	None = 0xFF, ///< Neither foundation nor groundsprite drawn yet.
	Normal = 0, ///< First part (normal foundation or no foundation)
	Halftile = 1, ///< Second part (halftile foundation)
	End, ///< End marker.
};

/**
 * Mode of "sprite combining"
 * @see StartSpriteCombine
 */
enum class SpriteCombineMode : uint8_t {
	None, ///< Every #AddSortableSpriteToDraw start its own bounding box
	Pending, ///< %Sprite combining will start with the next unclipped sprite.
	Active, ///< %Sprite combining is active. #AddSortableSpriteToDraw outputs child sprites.
};

typedef std::vector<TileSpriteToDraw> TileSpriteToDrawVector;
typedef std::vector<StringSpriteToDraw> StringSpriteToDrawVector;
typedef std::vector<ParentSpriteToDraw> ParentSpriteToDrawVector;
typedef std::vector<ChildScreenSpriteToDraw> ChildScreenSpriteToDrawVector;

constexpr int LAST_CHILD_NONE = -1; ///< There is no last_child to fill.
constexpr int LAST_CHILD_PARENT = -2; ///< Fill last_child of the most recent parent sprite.

/** Data structure storing rendering information */
struct ViewportDrawer {
	DrawPixelInfo dpi;

	StringSpriteToDrawVector string_sprites_to_draw;
	TileSpriteToDrawVector tile_sprites_to_draw;
	ParentSpriteToDrawVector parent_sprites_to_draw;
	ParentSpriteToSortVector parent_sprites_to_sort; ///< Parent sprite pointer array used for sorting
	ChildScreenSpriteToDrawVector child_screen_sprites_to_draw;

	int last_child;

	SpriteCombineMode combine_sprites; ///< Current mode of "sprite combining". @see StartSpriteCombine

	FoundationPart foundation_part; ///< Currently active foundation for ground sprite drawing.
	EnumIndexArray<int, FoundationPart, FoundationPart::End> foundation; ///< Foundation sprites (index into parent_sprites_to_draw).
	EnumIndexArray<int, FoundationPart, FoundationPart::End> last_foundation_child; ///< Tail of ChildSprite list of the foundations. (index into child_screen_sprites_to_draw)
	EnumIndexArray<Point, FoundationPart, FoundationPart::End> foundation_offset; ///< Pixel offset for ground sprites on the foundations.
};

static bool MarkViewportDirty(const Viewport &vp, int left, int top, int right, int bottom);

static ViewportDrawer _vd;

TileHighlightData _thd;
TileHighlightData _thd_drawn;
static TileInfo _cur_ti;
bool _draw_bounding_boxes = false;
bool _draw_dirty_blocks = false;
uint _dirty_block_colour = 0;
static VpSpriteSorter _vp_sprite_sorter = nullptr;

static Point MapXYZToViewport(const Viewport &vp, int x, int y, int z)
{
	Point p = IsViewportTurned(vp) ? ProjectTurned(x, y, z) : RemapCoords(x, y, z);
	p.x -= vp.virtual_width / 2;
	p.y -= vp.virtual_height / 2;
	return p;
}

/**
 * Initialize viewport of the window for use.
 * @param w Window to use/display the viewport in
 * @param x Offset of left edge of viewport with respect to left edge window \a w
 * @param y Offset of top edge of viewport with respect to top edge window \a w
 * @param width Width of the viewport
 * @param height Height of the viewport
 * @param focus Either the tile index or vehicle ID to focus.
 * @param zoom Zoomlevel to display
 */
void InitializeWindowViewport(Window *w, int x, int y,
	int width, int height, std::variant<TileIndex, VehicleID> focus, ZoomLevel zoom)
{
	assert(w->viewport == nullptr);

	auto vp = std::make_unique<ViewportData>();

	vp->left = x + w->left;
	vp->top = y + w->top;
	vp->width = width;
	vp->height = height;

	vp->zoom = Clamp(zoom, _settings_client.gui.zoom_min, _settings_client.gui.zoom_max);

	vp->virtual_width = ScaleByZoom(width, zoom);
	vp->virtual_height = ScaleByZoom(height, zoom);

	Point pt;

	if (std::holds_alternative<VehicleID>(focus)) {
		const Vehicle *veh;

		vp->follow_vehicle = std::get<VehicleID>(focus);
		veh = Vehicle::Get(vp->follow_vehicle)->GetMovingFront();
		pt = MapXYZToViewport(*vp, veh->x_pos, veh->y_pos, veh->z_pos);
	} else {
		TileIndex tile = std::get<TileIndex>(focus);
		if (tile == INVALID_TILE) {
			/* No tile? Use center of main viewport. */
			const Window *mw = GetMainWindow();

			/* center on same place as main window (zoom is maximum, no adjustment needed) */
			pt.x = mw->viewport->scrollpos_x + mw->viewport->virtual_width / 2;
			pt.x -= vp->virtual_width / 2;
			pt.y = mw->viewport->scrollpos_y + mw->viewport->virtual_height / 2;
			pt.y -= vp->virtual_height / 2;
		} else {
			x = TileX(tile) * TILE_SIZE;
			y = TileY(tile) * TILE_SIZE;
			pt = MapXYZToViewport(*vp, x, y, GetSlopePixelZ(x, y));
		}
		vp->follow_vehicle = VehicleID::Invalid();
	}

	vp->scrollpos_x = pt.x;
	vp->scrollpos_y = pt.y;
	vp->dest_scrollpos_x = pt.x;
	vp->dest_scrollpos_y = pt.y;

	vp->overlay = nullptr;

	vp->virtual_left = 0;
	vp->virtual_top = 0;

	w->viewport = std::move(vp);
}

static Point _vp_move_offs;

static void DoSetViewportPosition(Window::IteratorToFront it, int left, int top, int width, int height)
{
	for (; !it.IsEnd(); ++it) {
		const Window *w = *it;
		if (left + width > w->left &&
				w->left + w->width > left &&
				top + height > w->top &&
				w->top + w->height > top) {

			if (left < w->left) {
				DoSetViewportPosition(it, left, top, w->left - left, height);
				DoSetViewportPosition(it, left + (w->left - left), top, width - (w->left - left), height);
				return;
			}

			if (left + width > w->left + w->width) {
				DoSetViewportPosition(it, left, top, (w->left + w->width - left), height);
				DoSetViewportPosition(it, left + (w->left + w->width - left), top, width - (w->left + w->width - left), height);
				return;
			}

			if (top < w->top) {
				DoSetViewportPosition(it, left, top, width, (w->top - top));
				DoSetViewportPosition(it, left, top + (w->top - top), width, height - (w->top - top));
				return;
			}

			if (top + height > w->top + w->height) {
				DoSetViewportPosition(it, left, top, width, (w->top + w->height - top));
				DoSetViewportPosition(it, left, top + (w->top + w->height - top), width, height - (w->top + w->height - top));
				return;
			}

			return;
		}
	}

	{
		int xo = _vp_move_offs.x;
		int yo = _vp_move_offs.y;

		if (abs(xo) >= width || abs(yo) >= height) {
			/* fully_outside */
			RedrawScreenRect(left, top, left + width, top + height);
			return;
		}

		GfxScroll(left, top, width, height, xo, yo);

		if (xo > 0) {
			RedrawScreenRect(left, top, xo + left, top + height);
			left += xo;
			width -= xo;
		} else if (xo < 0) {
			RedrawScreenRect(left + width + xo, top, left + width, top + height);
			width += xo;
		}

		if (yo > 0) {
			RedrawScreenRect(left, top, width + left, top + yo);
		} else if (yo < 0) {
			RedrawScreenRect(left, top + height + yo, width + left, top + height);
		}
	}
}

static void SetViewportPosition(Window *w, int x, int y)
{
	Viewport &vp = *w->viewport;
	int old_left = vp.virtual_left;
	int old_top = vp.virtual_top;
	int i;
	int left, top, width, height;

	vp.virtual_left = x;
	vp.virtual_top = y;

	/* Viewport is bound to its left top corner, so it must be rounded down (UnScaleByZoomLower)
	 * else glitch described in FS#1412 will happen (offset by 1 pixel with zoom level > NORMAL)
	 */
	old_left = UnScaleByZoomLower(old_left, vp.zoom);
	old_top = UnScaleByZoomLower(old_top, vp.zoom);
	x = UnScaleByZoomLower(x, vp.zoom);
	y = UnScaleByZoomLower(y, vp.zoom);

	old_left -= x;
	old_top -= y;

	if (old_top == 0 && old_left == 0) return;

	_vp_move_offs.x = old_left;
	_vp_move_offs.y = old_top;

	left = vp.left;
	top = vp.top;
	width = vp.width;
	height = vp.height;

	if (left < 0) {
		width += left;
		left = 0;
	}

	i = left + width - _screen.width;
	if (i >= 0) width -= i;

	if (width > 0) {
		if (top < 0) {
			height += top;
			top = 0;
		}

		i = top + height - _screen.height;
		if (i >= 0) height -= i;

		if (height > 0) {
			Window::IteratorToFront it(w);
			++it;
			DoSetViewportPosition(it, left, top, width, height);
		}
	}
}

/**
 * Is a xy position inside the viewport of the window?
 * @param w Window to examine its viewport
 * @param x X coordinate of the xy position
 * @param y Y coordinate of the xy position
 * @return Pointer to the viewport if the xy position is in the viewport of the window,
 *         otherwise \c nullptr is returned.
 */
Viewport *IsPtInWindowViewport(const Window *w, int x, int y)
{
	if (w->viewport == nullptr) return nullptr;

	const Viewport &vp = *w->viewport;
	if (IsInsideMM(x, vp.left, vp.left + vp.width) && IsInsideMM(y, vp.top, vp.top + vp.height)) return w->viewport.get();

	return nullptr;
}

/**
 * Translate screen coordinate in a viewport to underlying tile coordinate.
 *
 * Returns exact point of the map that is visible in the given place
 * of the viewport (3D perspective), height of tiles and foundations matter.
 *
 * @param vp  Viewport that contains the (\a x, \a y) screen coordinate
 * @param x   Screen x coordinate, distance in pixels from the left edge of viewport frame
 * @param y   Screen y coordinate, distance in pixels from the top edge of viewport frame
 * @param clamp_to_map Clamp the coordinate outside of the map to the closest, non-void tile within the map
 * @return Tile coordinate or (-1, -1) if given x or y is not within viewport frame
 */
Point TranslateXYToTileCoord(const Viewport &vp, int x, int y, bool clamp_to_map)
{
	if (!IsInsideBS(x, vp.left, vp.width) || !IsInsideBS(y, vp.top, vp.height)) {
		Point pt = { -1, -1 };
		return pt;
	}

	int vx = ScaleByZoom(x - vp.left, vp.zoom) + vp.virtual_left;
	int vy = ScaleByZoom(y - vp.top, vp.zoom) + vp.virtual_top;
	if (IsViewportTurned(vp)) return InverseRemapCoordsTurned(vx, vy, clamp_to_map);
	return InverseRemapCoords2(vx, vy, clamp_to_map);
}

/* When used for zooming, check area below current coordinates (x,y)
 * and return the tile of the zoomed out/in position (zoom_x, zoom_y)
 * when you just want the tile, make x = zoom_x and y = zoom_y */
static Point GetTileFromScreenXY(int x, int y, int zoom_x, int zoom_y)
{
	if (Window *w = FindWindowFromPt(x, y); w != nullptr) {
		if (Viewport *vp = IsPtInWindowViewport(w, x, y); vp != nullptr) {
			return TranslateXYToTileCoord(*vp, zoom_x, zoom_y);
		}
	}

	return {-1, -1};
}

Point GetTileBelowCursor()
{
	return GetTileFromScreenXY(_cursor.pos.x, _cursor.pos.y, _cursor.pos.x, _cursor.pos.y);
}


Point GetTileZoomCenterWindow(bool in, Window * w)
{
	int x, y;
	const Viewport &vp = *w->viewport;

	if (in) {
		x = ((_cursor.pos.x - vp.left) >> 1) + (vp.width >> 2);
		y = ((_cursor.pos.y - vp.top) >> 1) + (vp.height >> 2);
	} else {
		x = vp.width - (_cursor.pos.x - vp.left);
		y = vp.height - (_cursor.pos.y - vp.top);
	}
	/* Get the tile below the cursor and center on the zoomed-out center */
	return GetTileFromScreenXY(_cursor.pos.x, _cursor.pos.y, x + vp.left, y + vp.top);
}

/**
 * Update the status of the zoom-buttons according to the zoom-level
 * of the viewport. This will update their status and invalidate accordingly
 * @param w Window pointer to the window that has the zoom buttons
 * @param vp pointer to the viewport whose zoom-level the buttons represent
 * @param widget_zoom_in widget index for window with zoom-in button
 * @param widget_zoom_out widget index for window with zoom-out button
 */
void HandleZoomMessage(Window *w, const Viewport &vp, WidgetID widget_zoom_in, WidgetID widget_zoom_out)
{
	w->SetWidgetDisabledState(widget_zoom_in, vp.zoom <= _settings_client.gui.zoom_min);
	w->SetWidgetDirty(widget_zoom_in);

	w->SetWidgetDisabledState(widget_zoom_out, vp.zoom >= _settings_client.gui.zoom_max);
	w->SetWidgetDirty(widget_zoom_out);
}

/**
 * Schedules a tile sprite for drawing.
 *
 * @param image the image to draw.
 * @param pal the provided palette.
 * @param x position x (world coordinates) of the sprite.
 * @param y position y (world coordinates) of the sprite.
 * @param z position z (world coordinates) of the sprite.
 * @param sub Only draw a part of the sprite.
 * @param extra_offs_x Pixel X offset for the sprite position.
 * @param extra_offs_y Pixel Y offset for the sprite position.
 */
static void AddTileSpriteToDraw(SpriteID image, PaletteID pal, int32_t x, int32_t y, int z, const SubSprite *sub = nullptr, int extra_offs_x = 0, int extra_offs_y = 0)
{
	assert((image & SPRITE_MASK) < MAX_SPRITES);

	TileSpriteToDraw &ts = _vd.tile_sprites_to_draw.emplace_back();
	ts.image = image;
	ts.pal = pal;
	ts.sub = sub;
	Point pt = RemapCoords(x, y, z);
	ts.x = pt.x + extra_offs_x;
	ts.y = pt.y + extra_offs_y;
}

/**
 * Adds a child sprite to the active foundation.
 *
 * The pixel offset of the sprite relative to the ParentSprite is the sum of the offset passed to OffsetGroundSprite() and extra_offs_?.
 *
 * @param image the image to draw.
 * @param pal the provided palette.
 * @param sub Only draw a part of the sprite.
 * @param foundation_part Foundation part.
 * @param extra_offs_x Pixel X offset for the sprite position.
 * @param extra_offs_y Pixel Y offset for the sprite position.
 */
static void AddChildSpriteToFoundation(SpriteID image, PaletteID pal, const SubSprite *sub, FoundationPart foundation_part, int extra_offs_x, int extra_offs_y)
{
	assert(foundation_part < FoundationPart::End);
	assert(_vd.foundation[foundation_part] != -1);
	Point offs = _vd.foundation_offset[foundation_part];

	/* Change the active ChildSprite list to the one of the foundation */
	AutoRestoreBackup backup(_vd.last_child, _vd.last_foundation_child[foundation_part]);
	AddChildSpriteScreen(image, pal, offs.x + extra_offs_x, offs.y + extra_offs_y, false, sub, false, false);
}

/**
 * Draws a ground sprite at a specific world-coordinate relative to the current tile.
 * If the current tile is drawn on top of a foundation the sprite is added as child sprite to the "foundation"-ParentSprite.
 *
 * @param image the image to draw.
 * @param pal the provided palette.
 * @param x position x (world coordinates) of the sprite relative to current tile.
 * @param y position y (world coordinates) of the sprite relative to current tile.
 * @param z position z (world coordinates) of the sprite relative to current tile.
 * @param sub Only draw a part of the sprite.
 * @param extra_offs_x Pixel X offset for the sprite position.
 * @param extra_offs_y Pixel Y offset for the sprite position.
 */
void DrawGroundSpriteAt(SpriteID image, PaletteID pal, int32_t x, int32_t y, int z, const SubSprite *sub, int extra_offs_x, int extra_offs_y)
{
	/* Switch to first foundation part, if no foundation was drawn */
	if (_vd.foundation_part == FoundationPart::None) _vd.foundation_part = FoundationPart::Normal;

	if (_vd.foundation[_vd.foundation_part] != -1) {
		Point pt = RemapCoords(x, y, z);
		AddChildSpriteToFoundation(image, pal, sub, _vd.foundation_part, pt.x + extra_offs_x * ZOOM_BASE, pt.y + extra_offs_y * ZOOM_BASE);
	} else {
		AddTileSpriteToDraw(image, pal, _cur_ti.x + x, _cur_ti.y + y, _cur_ti.z + z, sub, extra_offs_x * ZOOM_BASE, extra_offs_y * ZOOM_BASE);
	}
}

/**
 * Draws a ground sprite for the current tile.
 * If the current tile is drawn on top of a foundation the sprite is added as child sprite to the "foundation"-ParentSprite.
 *
 * @param image the image to draw.
 * @param pal the provided palette.
 * @param sub Only draw a part of the sprite.
 * @param extra_offs_x Pixel X offset for the sprite position.
 * @param extra_offs_y Pixel Y offset for the sprite position.
 */
void DrawGroundSprite(SpriteID image, PaletteID pal, const SubSprite *sub, int extra_offs_x, int extra_offs_y)
{
	DrawGroundSpriteAt(image, pal, 0, 0, 0, sub, extra_offs_x, extra_offs_y);
}

/**
 * Get the next foundation part
 * @param foundation_part The current foundation part.
 * @return The next foundation part.
 */
static FoundationPart GetNextFoundationPart(FoundationPart foundation_part)
{
	switch (foundation_part) {
		case FoundationPart::None: return FoundationPart::Normal;
		case FoundationPart::Normal: return FoundationPart::Halftile;
		default: NOT_REACHED();
	}
}

/**
 * Called when a foundation has been drawn for the current tile.
 * Successive ground sprites for the current tile will be drawn as child sprites of the "foundation"-ParentSprite, not as TileSprites.
 *
 * @param x sprite x-offset (screen coordinates) of ground sprites relative to the "foundation"-ParentSprite.
 * @param y sprite y-offset (screen coordinates) of ground sprites relative to the "foundation"-ParentSprite.
 */
void OffsetGroundSprite(int x, int y)
{
	/* Switch to next foundation part */
	_vd.foundation_part = GetNextFoundationPart(_vd.foundation_part);

	/* _vd.last_child is LAST_CHILD_NONE if foundation sprite was clipped by the viewport bounds */
	if (_vd.last_child != LAST_CHILD_NONE) _vd.foundation[_vd.foundation_part] = static_cast<uint>(_vd.parent_sprites_to_draw.size()) - 1;

	_vd.foundation_offset[_vd.foundation_part].x = x * ZOOM_BASE;
	_vd.foundation_offset[_vd.foundation_part].y = y * ZOOM_BASE;
	_vd.last_foundation_child[_vd.foundation_part] = _vd.last_child;
}

/**
 * Adds a child sprite to a parent sprite.
 * In contrast to "AddChildSpriteScreen()" the sprite position is in world coordinates
 *
 * @param image the image to draw.
 * @param pal the provided palette.
 * @param x position x of the sprite.
 * @param y position y of the sprite.
 * @param z position z of the sprite.
 * @param sub Only draw a part of the sprite.
 */
static void AddCombinedSprite(SpriteID image, PaletteID pal, int x, int y, int z, const SubSprite *sub, Point pixel_offset)
{
	Point pt = RemapCoords(x, y, z);
	pt.x += pixel_offset.x;
	pt.y += pixel_offset.y;
	const Sprite *spr = GetSprite(image & SPRITE_MASK, SpriteType::Normal);

	if (pt.x + spr->x_offs >= _vd.dpi.left + _vd.dpi.width ||
			pt.x + spr->x_offs + spr->width <= _vd.dpi.left ||
			pt.y + spr->y_offs >= _vd.dpi.top + _vd.dpi.height ||
			pt.y + spr->y_offs + spr->height <= _vd.dpi.top)
		return;

	const ParentSpriteToDraw &pstd = _vd.parent_sprites_to_draw.back();
	AddChildSpriteScreen(image, pal, pt.x - pstd.left, pt.y - pstd.top, false, sub, false);
}

/**
 * Draw a (transparent) sprite at given coordinates with a given bounding box.
 * The bounding box extends from (x + bb_offset_x, y + bb_offset_y, z + bb_offset_z) to (x + w - 1, y + h - 1, z + dz - 1), both corners included.
 * Bounding boxes with bb_offset_x == w or bb_offset_y == h or bb_offset_z == dz are allowed and produce thin slices.
 *
 * @note Bounding boxes are normally specified with bb_offset_x = bb_offset_y = bb_offset_z = 0. The extent of the bounding box in negative direction is
 *       defined by the sprite offset in the grf file.
 *       However if modifying the sprite offsets is not suitable (e.g. when using existing graphics), the bounding box can be tuned by bb_offset.
 *
 * @pre w >= bb_offset_x, h >= bb_offset_y, dz >= bb_offset_z. Else w, h or dz are ignored.
 *
 * @param image the image to combine and draw,
 * @param pal the provided palette,
 * @param x position X (world) of the sprite,
 * @param y position Y (world) of the sprite,
 * @param z position Z (world) of the sprite,
 * @param bounds Bounding box extent towards positive X/Y/Z (world).
 * @param transparent if true, switch the palette between the provided palette and the transparent palette,
 * @param sub Only draw a part of the sprite.
 */
void AddSortableSpriteToDraw(SpriteID image, PaletteID pal, int x, int y, int z, const SpriteBounds &bounds, bool transparent, const SubSprite *sub, Point pixel_offset)
{
	int32_t left, right, top, bottom;

	assert((image & SPRITE_MASK) < MAX_SPRITES);

	/* Move to bounding box. */
	x += bounds.origin.x;
	y += bounds.origin.y;
	z += bounds.origin.z;

	/* make the sprites transparent with the right palette */
	if (transparent) {
		SetBit(image, PALETTE_MODIFIER_TRANSPARENT);
		pal = PALETTE_TO_TRANSPARENT;
	}

	if (_vd.combine_sprites == SpriteCombineMode::Active) {
		AddCombinedSprite(image, pal, x + bounds.offset.x, y + bounds.offset.y, z + bounds.offset.z, sub, pixel_offset);
		return;
	}

	_vd.last_child = LAST_CHILD_NONE;

	Point pt = RemapCoords(x + bounds.offset.x, y + bounds.offset.y, z + bounds.offset.z);
	/* Straight across the screen, where the grid the world is laid out on
	 * cannot reach (Vehicle::draw_offs). The sorting box below is left where
	 * the vehicle really is, so a load shifted across its wagon still sorts
	 * against the wagon the way it did. */
	pt.x += pixel_offset.x;
	pt.y += pixel_offset.y;
	int tmp_left, tmp_top, tmp_x = pt.x, tmp_y = pt.y;

	/* Compute screen extents of sprite */
	if (image == SPR_EMPTY_BOUNDING_BOX) {
		left = tmp_left = RemapCoords(x + bounds.extent.x, y, z).x;
		right           = RemapCoords(x, y + bounds.extent.y, z).x + 1;
		top  = tmp_top  = RemapCoords(x, y, z + bounds.extent.z).y;
		bottom          = RemapCoords(x + bounds.extent.x, y + bounds.extent.y, z).y + 1;
	} else {
		const Sprite *spr = GetSprite(image & SPRITE_MASK, SpriteType::Normal);
		left = tmp_left = (pt.x += spr->x_offs);
		right           = (pt.x +  spr->width );
		top  = tmp_top  = (pt.y += spr->y_offs);
		bottom          = (pt.y +  spr->height);
	}

	if (_draw_bounding_boxes && (image != SPR_EMPTY_BOUNDING_BOX)) {
		/* Compute maximal extents of sprite and its bounding box */
		left   = std::min(left  , RemapCoords(x + bounds.extent.x, y, z).x);
		right  = std::max(right , RemapCoords(x, y + bounds.extent.y, z).x + 1);
		top    = std::min(top   , RemapCoords(x, y, z + bounds.extent.z).y);
		bottom = std::max(bottom, RemapCoords(x + bounds.extent.x, y + bounds.extent.y, z).y + 1);
	}

	/* Do not add the sprite to the viewport, if it is outside */
	if (left   >= _vd.dpi.left + _vd.dpi.width ||
	    right  <= _vd.dpi.left                 ||
	    top    >= _vd.dpi.top + _vd.dpi.height ||
	    bottom <= _vd.dpi.top) {
		return;
	}

	ParentSpriteToDraw &ps = _vd.parent_sprites_to_draw.emplace_back();
	ps.x = tmp_x;
	ps.y = tmp_y;

	ps.left = tmp_left;
	ps.top  = tmp_top;

	ps.image = image;
	ps.pal = pal;
	ps.sub = sub;
	ps.xmin = x;
	ps.xmax = x + bounds.extent.x - 1;

	ps.ymin = y;
	ps.ymax = y + bounds.extent.y - 1;

	ps.zmin = z;
	ps.zmax = z + bounds.extent.z - 1;

	ps.first_child = LAST_CHILD_NONE;

	ps.ax = x + bounds.offset.x;
	ps.ay = y + bounds.offset.y;
	ps.az = z + bounds.offset.z;

	_vd.last_child = LAST_CHILD_PARENT;

	if (_vd.combine_sprites == SpriteCombineMode::Pending) _vd.combine_sprites = SpriteCombineMode::Active;
}

/**
 * Starts a block of sprites, which are "combined" into a single bounding box.
 *
 * Subsequent calls to #AddSortableSpriteToDraw will be drawn into the same bounding box.
 * That is: The first sprite that is not clipped by the viewport defines the bounding box, and
 * the following sprites will be child sprites to that one.
 *
 * That implies:
 *  - The drawing order is definite. No other sprites will be sorted between those of the block.
 *  - You have to provide a valid bounding box for all sprites,
 *    as you won't know which one is the first non-clipped one.
 *    Preferable you use the same bounding box for all.
 *  - You cannot use #AddChildSpriteScreen inside the block, as its result will be indefinite.
 *
 * The block is terminated by #EndSpriteCombine.
 *
 * You cannot nest "combined" blocks.
 */
void StartSpriteCombine()
{
	assert(_vd.combine_sprites == SpriteCombineMode::None);
	_vd.combine_sprites = SpriteCombineMode::Pending;
}

/**
 * Terminates a block of sprites started by #StartSpriteCombine.
 * Take a look there for details.
 */
void EndSpriteCombine()
{
	assert(_vd.combine_sprites != SpriteCombineMode::None);
	_vd.combine_sprites = SpriteCombineMode::None;
}

/**
 * Check if the parameter "check" is inside the interval between
 * begin and end, including both begin and end.
 * @note Whether \c begin or \c end is the biggest does not matter.
 *       This method will account for that.
 * @param begin The begin of the interval.
 * @param end   The end of the interval.
 * @param check The value to check.
 * @return \c true iff check is between begin and end, including both begin and end.
 */
static bool IsInRangeInclusive(int begin, int end, int check)
{
	if (begin > end) std::swap(begin, end);
	return begin <= check && check <= end;
}

/**
 * Checks whether a point is inside the selected a diagonal rectangle given by _thd.size and _thd.pos
 * @param x The x coordinate of the point to be checked.
 * @param y The y coordinate of the point to be checked.
 * @return True if the point is inside the rectangle, else false.
 */
bool IsInsideRotatedRectangle(int x, int y)
{
	int dist_a = (_thd.size.x + _thd.size.y);      // Rotated coordinate system for selected rectangle.
	int dist_b = (_thd.size.x - _thd.size.y);      // We don't have to divide by 2. It's all relative!
	int a = ((x - _thd.pos.x) + (y - _thd.pos.y)); // Rotated coordinate system for the point under scrutiny.
	int b = ((x - _thd.pos.x) - (y - _thd.pos.y));

	/* Check if a and b are between 0 and dist_a or dist_b respectively. */
	return IsInRangeInclusive(dist_a, 0, a) && IsInRangeInclusive(dist_b, 0, b);
}

/**
 * Add a child sprite to a parent sprite.
 *
 * @param image the image to draw.
 * @param pal the provided palette.
 * @param x sprite x-offset (screen coordinates) relative to parent sprite.
 * @param y sprite y-offset (screen coordinates) relative to parent sprite.
 * @param transparent if true, switch the palette between the provided palette and the transparent palette,
 * @param sub Only draw a part of the sprite.
 * @param scale if true, scale offsets to base zoom level.
 * @param relative if true, draw sprite relative to parent sprite offsets.
 */
void AddChildSpriteScreen(SpriteID image, PaletteID pal, int x, int y, bool transparent, const SubSprite *sub, bool scale, bool relative)
{
	assert((image & SPRITE_MASK) < MAX_SPRITES);

	/* If the ParentSprite was clipped by the viewport bounds, do not draw the ChildSprites either */
	if (_vd.last_child == LAST_CHILD_NONE) return;

	/* make the sprites transparent with the right palette */
	if (transparent) {
		SetBit(image, PALETTE_MODIFIER_TRANSPARENT);
		pal = PALETTE_TO_TRANSPARENT;
	}

	int32_t child_id = static_cast<int32_t>(_vd.child_screen_sprites_to_draw.size());
	if (_vd.last_child != LAST_CHILD_PARENT) {
		_vd.child_screen_sprites_to_draw[_vd.last_child].next = child_id;
	} else {
		_vd.parent_sprites_to_draw.back().first_child = child_id;
	}

	ChildScreenSpriteToDraw &cs = _vd.child_screen_sprites_to_draw.emplace_back();
	cs.image = image;
	cs.pal = pal;
	cs.sub = sub;
	cs.x = scale ? x * ZOOM_BASE : x;
	cs.y = scale ? y * ZOOM_BASE : y;
	cs.relative = relative;
	cs.next = LAST_CHILD_NONE;

	/* Append the sprite to the active ChildSprite list.
	 * If the active ParentSprite is a foundation, update last_foundation_child as well.
	 * Note: ChildSprites of foundations are NOT sequential in the vector, as selection sprites are added at last. */
	if (_vd.last_foundation_child[FoundationPart::Normal] == _vd.last_child) _vd.last_foundation_child[FoundationPart::Normal] = child_id;
	if (_vd.last_foundation_child[FoundationPart::Halftile] == _vd.last_child) _vd.last_foundation_child[FoundationPart::Halftile] = child_id;
	_vd.last_child = child_id;
}

/**
 * Add a string to draw to a viewport.
 * @param x Left position of string.
 * @param y Top position of string.
 * @param colour Colour of string.
 * @param flags ViewportStringFlags to control the string's appearance.
 * @param width Width of the string.
 * @returns Reference to raw string which should be filled in by the caller.
 */
static std::string &AddStringToDraw(int x, int y, Colours colour, ViewportStringFlags flags, uint16_t width)
{
	assert(width != 0);
	StringSpriteToDraw &ss = _vd.string_sprites_to_draw.emplace_back();
	ss.colour = colour;
	ss.flags = flags;
	ss.x = x;
	ss.y = y;
	ss.width = width;

	return ss.string;
}


/**
 * Draws sprites between ground sprite and everything above.
 *
 * The sprite is either drawn as TileSprite or as ChildSprite of the active foundation.
 *
 * @param image the image to draw.
 * @param pal the provided palette.
 * @param ti TileInfo Tile that is being drawn
 * @param z_offset Z offset relative to the groundsprite. Only used for the sprite position, not for sprite sorting.
 * @param foundation_part Foundation part the sprite belongs to.
 * @param extra_offs_x Pixel X offset for the sprite position.
 * @param extra_offs_y Pixel Y offset for the sprite position.
 */
static void DrawSelectionSprite(SpriteID image, PaletteID pal, const TileInfo *ti, int z_offset, FoundationPart foundation_part, int extra_offs_x = 0, int extra_offs_y = 0)
{
	if (_vd.foundation[foundation_part] == -1) {
		/* draw on real ground */
		AddTileSpriteToDraw(image, pal, ti->x, ti->y, ti->z + z_offset, nullptr, extra_offs_x, extra_offs_y);
	} else {
		/* draw on top of foundation */
		AddChildSpriteToFoundation(image, pal, nullptr, foundation_part, extra_offs_x, extra_offs_y - z_offset * ZOOM_BASE);
	}
}

/**
 * Draws a selection rectangle on a tile.
 *
 * @param ti TileInfo Tile that is being drawn
 * @param pal Palette to apply.
 */
static void DrawTileSelectionRect(const TileInfo *ti, PaletteID pal)
{
	if (!IsValidTile(ti->tile)) return;

	SpriteID sel;
	if (IsHalftileSlope(ti->tileh)) {
		Corner halftile_corner = GetHalftileSlopeCorner(ti->tileh);
		SpriteID sel2 = SPR_HALFTILE_SELECTION_FLAT + halftile_corner;
		DrawSelectionSprite(sel2, pal, ti, 7 + TILE_HEIGHT, FoundationPart::Halftile);

		Corner opposite_corner = OppositeCorner(halftile_corner);
		if (IsSteepSlope(ti->tileh)) {
			sel = SPR_HALFTILE_SELECTION_DOWN;
		} else {
			sel = ((ti->tileh & SlopeWithOneCornerRaised(opposite_corner)) != 0 ? SPR_HALFTILE_SELECTION_UP : SPR_HALFTILE_SELECTION_FLAT);
		}
		sel += opposite_corner;
	} else {
		sel = SPR_SELECT_TILE + SlopeToSpriteOffset(ti->tileh);
	}
	DrawSelectionSprite(sel, pal, ti, 7, FoundationPart::Normal);
}

static bool IsPartOfAutoLine(int px, int py)
{
	px -= _thd.selstart.x;
	py -= _thd.selstart.y;

	if ((_thd.drawstyle & HT_DRAG_MASK) != HT_LINE) return false;

	switch (_thd.drawstyle & HT_DIR_MASK) {
		case HT_DIR_X:  return py == 0; // x direction
		case HT_DIR_Y:  return px == 0; // y direction
		case HT_DIR_HU: return px == -py || px == -py - 16; // horizontal upper
		case HT_DIR_HL: return px == -py || px == -py + 16; // horizontal lower
		case HT_DIR_VL: return px == py || px == py + 16; // vertical left
		case HT_DIR_VR: return px == py || px == py - 16; // vertical right
		default:
			NOT_REACHED();
	}
}

/* [direction][side] */
static const HighLightStyle _autorail_type[6][2] = {
	{ HT_DIR_X,  HT_DIR_X },
	{ HT_DIR_Y,  HT_DIR_Y },
	{ HT_DIR_HU, HT_DIR_HL },
	{ HT_DIR_HL, HT_DIR_HU },
	{ HT_DIR_VL, HT_DIR_VR },
	{ HT_DIR_VR, HT_DIR_VL }
};

#include "table/autorail.h"

/**
 * Draws autorail highlights.
 *
 * @param *ti TileInfo Tile that is being drawn
 * @param highlight_style Highlight to draw
 */
static void DrawAutorailSelection(const TileInfo *ti, HighLightStyle highlight_style)
{
	SpriteID image;
	PaletteID pal;

	FoundationPart foundation_part = FoundationPart::Normal;
	Slope slope = RemoveHalftileSlope(ti->tileh);
	if (IsHalftileSlope(ti->tileh)) {
		static constexpr CornerIndexArray<HighLightStyle> lower_rail{HT_DIR_VR, HT_DIR_HU, HT_DIR_VL, HT_DIR_HL};
		Corner halftile_corner = GetHalftileSlopeCorner(ti->tileh);
		if ((highlight_style & HT_DIR_MASK) != lower_rail[halftile_corner]) {
			foundation_part = FoundationPart::Halftile;
			/* Here we draw the highlights of the "three-corners-raised"-slope. That looks ok to me. */
			slope = SlopeWithThreeCornersRaised(OppositeCorner(halftile_corner));
		}
	}

	int offset = _autorail_slope_sprite_offsets[slope][highlight_style & HT_DIR_MASK];
	if (offset >= 0) {
		image = SPR_AUTORAIL_BASE + offset;
		pal = PAL_NONE;
	} else {
		image = SPR_AUTORAIL_BASE - offset;
		pal = PALETTE_SEL_TILE_RED;
	}

	DrawSelectionSprite(image, _thd.make_square_red ? PALETTE_SEL_TILE_RED : pal, ti, 7, foundation_part);
}

/**
 * Draw the blueprint paste preview overlays on a tile inside the selection.
 * @param ti Tile that is being drawn.
 */
static void DrawBlueprintPastePreview(const TileInfo *ti)
{
	const BlueprintPastePreview *preview = GetBlueprintPastePreview(TileVirtXY(_thd.pos.x, _thd.pos.y));
	if (preview == nullptr) return;

	int dx = (ti->x - _thd.pos.x) / static_cast<int>(TILE_SIZE);
	int dy = (ti->y - _thd.pos.y) / static_cast<int>(TILE_SIZE);
	if (dx < 0 || dy < 0 || dx >= preview->width || dy >= preview->height) return;

	size_t index = static_cast<size_t>(dy) * preview->width + dx;
	uint8_t overlay = preview->tiles[index];
	if (index < preview->blocked.size() && preview->blocked[index]) {
		/* This part cannot be built at the current position. */
		DrawTileSelectionRect(ti, PALETTE_SEL_TILE_RED);
	} else if (HasBit(overlay, BLUEPRINT_PREVIEW_HIGHLIGHT_BIT)) {
		DrawTileSelectionRect(ti, PALETTE_SEL_TILE_BLUE);
	}

	for (Track track : TrackBits(static_cast<uint8_t>(GB(overlay, 0, 6)))) {
		DrawAutorailSelection(ti, _autorail_type[to_underlying(track)][0]);
	}
}

/** Types of tile highlight. */
enum class TileHighlightType : uint8_t {
	None, ///< No tile highlight.
	White, ///< Indicates a tile which is part of the highlighted station, or a station tile within the highlighted town.
	Blue, ///< Indicates a tile which is in the catchment area of the highlighted station.
	Red, ///< Indicates a house tile which is not in any station catchment area of the highlighted town.
};

const Station *_viewport_highlight_station; ///< Currently selected station for coverage area highlight
const Station *_viewport_highlight_station_rect; ///< Currently selected station for rectangle highlight
const Waypoint *_viewport_highlight_waypoint; ///< Currently selected waypoint for coverage area highlight
const Waypoint *_viewport_highlight_waypoint_rect; ///< Currently selected waypoint for rectangle highlight
const Town *_viewport_highlight_town; ///< Currently selected town for coverage area highlight

/**
 * Get tile highlight type of coverage area for a given tile.
 * @param t Tile that is being drawn
 * @return Tile highlight type to draw
 */
static TileHighlightType GetTileHighlightType(TileIndex t)
{
	if (_viewport_highlight_station != nullptr) {
		if (IsTileType(t, TileType::Station) && GetStationIndex(t) == _viewport_highlight_station->index) return TileHighlightType::White;
		if (_viewport_highlight_station->TileIsInCatchment(t)) return TileHighlightType::Blue;
	}

	if (_viewport_highlight_station_rect != nullptr) {
		if (IsTileType(t, TileType::Station) && GetStationIndex(t) == _viewport_highlight_station_rect->index) return TileHighlightType::White;
		const StationRect *r = &_viewport_highlight_station_rect->rect;
		if (r->PtInExtendedRect(TileX(t), TileY(t))) return TileHighlightType::Blue;
	}

	if (_viewport_highlight_waypoint != nullptr) {
		if (IsTileType(t, TileType::Station) && GetStationIndex(t) == _viewport_highlight_waypoint->index) return TileHighlightType::Blue;
	}

	if (_viewport_highlight_waypoint_rect != nullptr) {
		if (IsTileType(t, TileType::Station) && GetStationIndex(t) == _viewport_highlight_waypoint_rect->index) return TileHighlightType::White;
		const StationRect *r = &_viewport_highlight_waypoint_rect->rect;
		if (r->PtInExtendedRect(TileX(t), TileY(t))) return TileHighlightType::Blue;
	}

	if (_viewport_highlight_town != nullptr) {
		if (IsTileType(t, TileType::House)) {
			if (GetTownIndex(t) == _viewport_highlight_town->index) {
				for (const Station *st : _viewport_highlight_town->stations_near) {
					if (st->owner != _current_company) continue;
					if (st->TileIsInCatchment(t)) return TileHighlightType::Blue;
				}
				return TileHighlightType::Red;
			}
		} else if (IsTileType(t, TileType::Station)) {
			for (const Station *st : _viewport_highlight_town->stations_near) {
				if (st->owner != _current_company) continue;
				if (GetStationIndex(t) == st->index) return TileHighlightType::White;
			}
		}
	}

	return TileHighlightType::None;
}

/**
 * Draw tile highlight for coverage area highlight.
 * @param *ti TileInfo Tile that is being drawn
 * @param tht Highlight type to draw.
 */
static void DrawTileHighlightType(const TileInfo *ti, TileHighlightType tht)
{
	switch (tht) {
		default:
		case TileHighlightType::None: break;
		case TileHighlightType::White: DrawTileSelectionRect(ti, PAL_NONE); break;
		case TileHighlightType::Blue: DrawTileSelectionRect(ti, PALETTE_SEL_TILE_BLUE); break;
		case TileHighlightType::Red: DrawTileSelectionRect(ti, PALETTE_SEL_TILE_RED); break;
	}
}

/**
 * Highlights tiles inside local authority of selected towns.
 * @param *ti TileInfo Tile that is being drawn
 */
static void HighlightTownLocalAuthorityTiles(const TileInfo *ti)
{
	/* Going through cases in order of computational time. */

	if (_town_local_authority_kdtree.Count() == 0) return;

	/* Tile belongs to town regardless of distance from town. */
	if (GetTileType(ti->tile) == TileType::House) {
		if (!Town::GetByTile(ti->tile)->show_zone) return;

		DrawTileSelectionRect(ti, PALETTE_CRASH);
		return;
	}

	/* If the closest town in the highlighted list is far, we can stop searching. */
	TownID tid = _town_local_authority_kdtree.FindNearest(TileX(ti->tile), TileY(ti->tile));
	Town *closest_highlighted_town = Town::Get(tid);

	if (DistanceManhattan(ti->tile, closest_highlighted_town->xy) >= _settings_game.economy.dist_local_authority) return;

	/* Tile is inside of the local autrhority distance of a highlighted town,
	   but it is possible that a non-highlighted town is even closer. */
	Town *closest_town = ClosestTownFromTile(ti->tile, _settings_game.economy.dist_local_authority);

	if (closest_town->show_zone) {
		DrawTileSelectionRect(ti, PALETTE_CRASH);
	}

}

/**
 * Checks if the specified tile is selected and if so draws selection using correct selectionstyle.
 * @param *ti TileInfo Tile that is being drawn
 */
static void DrawTileSelection(const TileInfo *ti)
{
	/* Highlight tiles inside local authority of selected towns. */
	HighlightTownLocalAuthorityTiles(ti);

	/* Draw a red error square? */
	bool is_redsq = _thd.redsq == ti->tile;
	if (is_redsq) DrawTileSelectionRect(ti, PALETTE_TILE_RED_PULSATING);

	TileHighlightType tht = GetTileHighlightType(ti->tile);
	DrawTileHighlightType(ti, tht);

	/* No tile selection active? */
	if ((_thd.drawstyle & HT_DRAG_MASK) == HT_NONE) return;

	if (_thd.diagonal) { // We're drawing a 45 degrees rotated (diagonal) rectangle
		if (IsInsideRotatedRectangle((int)ti->x, (int)ti->y)) goto draw_inner;
		return;
	}

	/* Inside the inner area? */
	if (IsInsideBS(ti->x, _thd.pos.x, _thd.size.x) &&
			IsInsideBS(ti->y, _thd.pos.y, _thd.size.y)) {
draw_inner:
		if (_thd.drawstyle & HT_RECT) {
			if (!is_redsq) DrawTileSelectionRect(ti, _thd.make_square_red ? PALETTE_SEL_TILE_RED : PAL_NONE);
			DrawBlueprintPastePreview(ti);
		} else if (_thd.drawstyle & HT_POINT) {
			/* Figure out the Z coordinate for the single dot. */
			int z = 0;
			FoundationPart foundation_part = FoundationPart::Normal;
			if (ti->tileh & SLOPE_N) {
				z += TILE_HEIGHT;
				if (RemoveHalftileSlope(ti->tileh) == SLOPE_STEEP_N) z += TILE_HEIGHT;
			}
			if (IsHalftileSlope(ti->tileh)) {
				Corner halftile_corner = GetHalftileSlopeCorner(ti->tileh);
				if ((halftile_corner == CORNER_W) || (halftile_corner == CORNER_E)) z += TILE_HEIGHT;
				if (halftile_corner != CORNER_S) {
					foundation_part = FoundationPart::Halftile;
					if (IsSteepSlope(ti->tileh)) z -= TILE_HEIGHT;
				}
			}
			DrawSelectionSprite(SPR_DOT, PAL_NONE, ti, z, foundation_part);
		} else if (_thd.drawstyle & HT_RAIL) {
			/* autorail highlight piece under cursor */
			HighLightStyle type = _thd.drawstyle & HT_DIR_MASK;
			assert(type < HT_DIR_END);
			DrawAutorailSelection(ti, _autorail_type[type][0]);
		} else if (IsPartOfAutoLine(ti->x, ti->y)) {
			/* autorail highlighting long line */
			HighLightStyle dir = _thd.drawstyle & HT_DIR_MASK;
			uint side;

			if (dir == HT_DIR_X || dir == HT_DIR_Y) {
				side = 0;
			} else {
				TileIndex start = TileVirtXY(_thd.selstart.x, _thd.selstart.y);
				side = Delta(Delta(TileX(start), TileX(ti->tile)), Delta(TileY(start), TileY(ti->tile)));
			}

			DrawAutorailSelection(ti, _autorail_type[dir][side]);
		}
		return;
	}

	/* Check if it's inside the outer area? */
	if (!is_redsq && (tht == TileHighlightType::None || tht == TileHighlightType::Red) && _thd.outersize.x > 0 &&
			IsInsideBS(ti->x, _thd.pos.x + _thd.offs.x, _thd.size.x + _thd.outersize.x) &&
			IsInsideBS(ti->y, _thd.pos.y + _thd.offs.y, _thd.size.y + _thd.outersize.y)) {
		/* Draw a blue rect. */
		DrawTileSelectionRect(ti, PALETTE_SEL_TILE_BLUE);
		return;
	}
}

/**
 * Returns the y coordinate in the viewport coordinate system where the given
 * tile is painted.
 * @param tile Any tile.
 * @return The viewport y coordinate where the tile is painted.
 */
static int GetViewportY(Point tile)
{
	/* Each increment in X or Y direction moves down by half a tile, i.e. TILE_PIXELS / 2. */
	return (tile.y * (int)(TILE_PIXELS / 2) + tile.x * (int)(TILE_PIXELS / 2) - TilePixelHeightOutsideMap(tile.x, tile.y)) << ZOOM_BASE_SHIFT;
}

/**
 * Add the landscape to the viewport, i.e. all ground tiles and buildings.
 */
static bool _north_house_objects = false;
static void AddNorthHouseObject(size_t first_parent);
static void AddIndustryObjects(size_t first_parent, bool north);
static void AddStreetLamps(size_t first_parent);
static void AddFieldWheat();
static const ThreeDIndustryTileParts *OriginalIndustryTileParts(TileIndex tile);
static TileIndex OriginalIndustryAnchor(TileIndex tile, const ThreeDIndustryTileParts *&parts);
static void AddOriginalIndustryTile(TileIndex tile, const ThreeDIndustryTileParts &parts, bool built);

static void ViewportAddLandscape()
{
	assert(_vd.dpi.top <= _vd.dpi.top + _vd.dpi.height);
	assert(_vd.dpi.left <= _vd.dpi.left + _vd.dpi.width);

	Point upper_left = InverseRemapCoords(_vd.dpi.left, _vd.dpi.top);
	Point upper_right = InverseRemapCoords(_vd.dpi.left + _vd.dpi.width, _vd.dpi.top);

	/* Transformations between tile coordinates and viewport rows/columns: See vp_column_row
	 *   column = y - x
	 *   row    = x + y
	 *   x      = (row - column) / 2
	 *   y      = (row + column) / 2
	 * Note: (row, columns) pairs are only valid, if they are both even or both odd.
	 */

	/* Columns overlap with neighbouring columns by a half tile.
	 *  - Left column is column of upper_left (rounded down) and one column to the left.
	 *  - Right column is column of upper_right (rounded up) and one column to the right.
	 * Note: Integer-division does not round down for negative numbers, so ensure rounding with another increment/decrement.
	 */
	int left_column = (upper_left.y - upper_left.x) / (int)TILE_SIZE - 2;
	int right_column = (upper_right.y - upper_right.x) / (int)TILE_SIZE + 2;

	int potential_bridge_height = ZOOM_BASE * TILE_HEIGHT * _settings_game.construction.max_bridge_height;

	/* Rows overlap with neighbouring rows by a half tile.
	 * The first row that could possibly be visible is the row above upper_left (if it is at height 0).
	 * Due to integer-division not rounding down for negative numbers, we need another decrement.
	 */
	int row = (upper_left.x + upper_left.y) / (int)TILE_SIZE - 2;
	bool last_row = false;
	for (; !last_row; row++) {
		last_row = true;
		for (int column = left_column; column <= right_column; column++) {
			/* Valid row/column? */
			if ((row + column) % 2 != 0) continue;

			Point tilecoord;
			tilecoord.x = (row - column) / 2;
			tilecoord.y = (row + column) / 2;
			assert(column == tilecoord.y - tilecoord.x);
			assert(row == tilecoord.y + tilecoord.x);

			TileType tile_type;
			_cur_ti.x = tilecoord.x * TILE_SIZE;
			_cur_ti.y = tilecoord.y * TILE_SIZE;

			if (IsInsideBS(tilecoord.x, 0, Map::SizeX()) && IsInsideBS(tilecoord.y, 0, Map::SizeY())) {
				/* This includes the south border at Map::MaxX / Map::MaxY. When terraforming we still draw tile selections there. */
				_cur_ti.tile = TileXY(tilecoord.x, tilecoord.y);
				tile_type = GetTileType(_cur_ti.tile);
			} else {
				_cur_ti.tile = INVALID_TILE;
				tile_type = TileType::Void;
			}

			if (tile_type != TileType::Void) {
				/* We are inside the map => paint landscape. */
				std::tie(_cur_ti.tileh, _cur_ti.z) = GetTilePixelSlope(_cur_ti.tile);
			} else {
				/* We are outside the map => paint black. */
				std::tie(_cur_ti.tileh, _cur_ti.z) = GetTilePixelSlopeOutsideMap(tilecoord.x, tilecoord.y);
			}

			int viewport_y = GetViewportY(tilecoord);

			if (viewport_y + MAX_TILE_EXTENT_BOTTOM < _vd.dpi.top) {
				/* The tile in this column is not visible yet.
				 * Tiles in other columns may be visible, but we need more rows in any case. */
				last_row = false;
				continue;
			}

			int min_visible_height = viewport_y - (_vd.dpi.top + _vd.dpi.height);
			bool tile_visible = min_visible_height <= 0;

			if (tile_type != TileType::Void) {
				/* Is tile with buildings visible? */
				if (min_visible_height < MAX_TILE_EXTENT_TOP) tile_visible = true;

				if (IsBridgeAbove(_cur_ti.tile)) {
					/* Is the bridge visible? */
					TileIndex bridge_tile = GetNorthernBridgeEnd(_cur_ti.tile);
					int bridge_height = ZOOM_BASE * (GetBridgePixelHeight(bridge_tile) - TilePixelHeight(_cur_ti.tile));
					if (min_visible_height < bridge_height + MAX_TILE_EXTENT_TOP) tile_visible = true;
				}

				/* Would a higher bridge on a more southern tile be visible?
				 * If yes, we need to loop over more rows to possibly find one. */
				if (min_visible_height < potential_bridge_height + MAX_TILE_EXTENT_TOP) last_row = false;
			} else {
				/* Outside of map. If we are on the north border of the map, there may still be a bridge visible,
				 * so we need to loop over more rows to possibly find one. */
				if ((tilecoord.x <= 0 || tilecoord.y <= 0) && min_visible_height < potential_bridge_height + MAX_TILE_EXTENT_TOP) last_row = false;
			}

			if (tile_visible) {
				last_row = false;
				_vd.foundation_part = FoundationPart::None;
				_vd.foundation[FoundationPart::Normal] = -1;
				_vd.foundation[FoundationPart::Halftile] = -1;
				_vd.last_foundation_child[FoundationPart::Normal] = LAST_CHILD_NONE;
				_vd.last_foundation_child[FoundationPart::Halftile] = LAST_CHILD_NONE;

				size_t first_parent = _vd.parent_sprites_to_draw.size();
				_tile_type_procs[tile_type]->draw_tile_proc(&_cur_ti);
				if (_north_house_objects && tile_type == TileType::House) AddNorthHouseObject(first_parent);
				if (_north_house_objects && tile_type == TileType::Industry) AddIndustryObjects(first_parent, true);
				if (_north_house_objects && tile_type == TileType::Road) AddStreetLamps(first_parent);
				if (_north_house_objects && tile_type == TileType::Clear) AddFieldWheat();
				if (_cur_ti.tile != INVALID_TILE) DrawTileSelection(&_cur_ti);
			}
		}
	}
}

/**
 * Add a string to draw in the current viewport.
 * @param dpi current viewport area
 * @param sign sign position and dimension
 * @param flags ViewportStringFlags to control the string's appearance.
 * @param colour colour of the sign background; or Colours::Invalid if transparent
 * @returns Pointer to std::string to filled with sign, or nullptr if string would be outside the viewport bounds.
 */
std::string *ViewportAddString(const DrawPixelInfo *dpi, const ViewportSign *sign, ViewportStringFlags flags, Colours colour)
{
	int left   = dpi->left;
	int top    = dpi->top;
	int right  = left + dpi->width;
	int bottom = top + dpi->height;

	bool small = flags.Test(ViewportStringFlag::Small);
	int sign_height     = ScaleByZoom(WidgetDimensions::scaled.fullbevel.top + GetCharacterHeight(small ? FontSize::Small : FontSize::Normal) + WidgetDimensions::scaled.fullbevel.bottom, dpi->zoom);
	int sign_half_width = ScaleByZoom((small ? sign->width_small : sign->width_normal) / 2, dpi->zoom);

	if (bottom < sign->top ||
			top   > sign->top + sign_height ||
			right < sign->center - sign_half_width ||
			left  > sign->center + sign_half_width) {
		return nullptr;
	}

	return &AddStringToDraw(sign->center - sign_half_width, sign->top, colour, flags, small ? sign->width_small : sign->width_normal);
}

static Rect ExpandRectWithViewportSignMargins(Rect r, ZoomLevel zoom)
{
	const int fh = std::max(GetCharacterHeight(FontSize::Normal), GetCharacterHeight(FontSize::Small));
	const int max_tw = _viewport_sign_maxwidth / 2 + 1;
	const int expand_y = ScaleByZoom(WidgetDimensions::scaled.fullbevel.top + fh + WidgetDimensions::scaled.fullbevel.bottom, zoom);
	const int expand_x = ScaleByZoom(WidgetDimensions::scaled.fullbevel.left + max_tw + WidgetDimensions::scaled.fullbevel.right, zoom);

	r.left -= expand_x;
	r.right += expand_x;
	r.top -= expand_y;
	r.bottom += expand_y;

	return r;
}

/**
 * Add town strings to a viewport.
 * @param dpi Current viewport area.
 * @param towns List of towns to add.
 * @param small Add small versions of strings.
 */
static void ViewportAddTownStrings(DrawPixelInfo *dpi, const std::vector<const Town *> &towns, bool small)
{
	ViewportStringFlags flags{};
	if (small) flags.Set({ViewportStringFlag::Small, ViewportStringFlag::Shadow});

	StringID stringid_town = !small && _settings_client.gui.population_in_label ? STR_VIEWPORT_TOWN_POP : STR_TOWN_NAME;
	StringID stringid_town_city = stringid_town;
	if (!small) {
		stringid_town_city = _settings_client.gui.population_in_label ? STR_VIEWPORT_TOWN_CITY_POP : STR_VIEWPORT_TOWN_CITY;
	}

	for (const Town *t : towns) {
		std::string *str = ViewportAddString(dpi, &t->cache.sign, flags, Colours::Invalid);
		if (str == nullptr) continue;

		if (t->larger_town) {
			*str = GetString(stringid_town_city, t->index, t->cache.population);
		} else {
			*str = GetString(stringid_town, t->index, t->cache.population);
		}
	}
}

/**
 * Add sign strings to a viewport.
 * @param dpi Current viewport area.
 * @param signs List of signs to add.
 * @param small Add small versions of strings.
 */
static void ViewportAddSignStrings(DrawPixelInfo *dpi, const std::vector<const Sign *> &signs, bool small)
{
	ViewportStringFlags flags{};
	if (small) flags.Set(ViewportStringFlag::Small);

	/* Signs placed by a game script don't have a frame. */
	ViewportStringFlags deity_flags{ flags };
	deity_flags.Set(ViewportStringFlag::TextColour);

	flags.Set(IsTransparencySet(TransparencyOption::Signs) ? ViewportStringFlag::TransparentRect : ViewportStringFlag::ColourRect);

	for (const Sign *si : signs) {
		/* Workaround to make sure white is actually white. The string drawing logic changes all
		 * colours that are not Colours::Invalid slightly, turning white into a light gray. */
		const Colours deity_colour = si->text_colour == Colours::White ? Colours::Invalid : si->text_colour;

		std::string *str = ViewportAddString(dpi, &si->sign, (si->owner == OWNER_DEITY) ? deity_flags : flags,
			(si->owner == OWNER_NONE) ? Colours::Grey : (si->owner == OWNER_DEITY ? deity_colour : _company_colours[si->owner]));
		if (str == nullptr) continue;

		*str = GetString(STR_SIGN_NAME, si->index);
	}
}

/**
 * Add station strings to a viewport.
 * @param dpi Current viewport area.
 * @param stations List of stations to add.
 * @param small Add small versions of strings.
 */
static void ViewportAddStationStrings(DrawPixelInfo *dpi, const std::vector<const BaseStation *> &stations, bool small)
{
	/* Transparent station signs have colour text instead of a colour panel. */
	ViewportStringFlags flags{IsTransparencySet(TransparencyOption::Signs) ? ViewportStringFlag::TextColour : ViewportStringFlag::ColourRect};
	if (small) flags.Set(ViewportStringFlag::Small);

	for (const BaseStation *st : stations) {
		std::string *str = ViewportAddString(dpi, &st->sign, flags, (st->owner == OWNER_NONE || !st->IsInUse()) ? Colours::Grey : _company_colours[st->owner]);
		if (str == nullptr) continue;

		if (Station::IsExpected(st)) { /* Station */
			*str = GetString(small ? STR_STATION_NAME : STR_VIEWPORT_STATION, st->index, st->facilities);
		} else { /* Waypoint */
			*str = GetString(STR_WAYPOINT_NAME, st->index);
		}
	}
}

static void ViewportAddKdtreeSigns(DrawPixelInfo *dpi)
{
	Rect search_rect{ dpi->left, dpi->top, dpi->left + dpi->width, dpi->top + dpi->height };
	search_rect = ExpandRectWithViewportSignMargins(search_rect, dpi->zoom);

	bool show_stations = _display_opt.Test(DisplayOption::ShowStationNames) && _game_mode != GameMode::Menu;
	bool show_waypoints = _display_opt.Test(DisplayOption::ShowWaypointNames) && _game_mode != GameMode::Menu;
	bool show_towns = _display_opt.Test(DisplayOption::ShowTownNames) && _game_mode != GameMode::Menu;
	bool show_signs = _display_opt.Test(DisplayOption::ShowSigns) && !IsInvisibilitySet(TransparencyOption::Signs);
	bool show_competitors = _display_opt.Test(DisplayOption::ShowCompetitorSigns);

	/* Collect all the items first and draw afterwards, to ensure layering */
	std::vector<const BaseStation *> stations;
	std::vector<const Town *> towns;
	std::vector<const Sign *> signs;

	_viewport_sign_kdtree.FindContained(search_rect.left, search_rect.top, search_rect.right, search_rect.bottom, [&](const ViewportSignKdtreeItem & item) {
		switch (item.type) {
			case ViewportSignKdtreeItem::VKI_STATION: {
				if (!show_stations) break;
				const BaseStation *st = BaseStation::Get(std::get<StationID>(item.id));

				/* If no facilities are present the station is a ghost station. */
				StationFacilities facilities = st->facilities;
				if (facilities.None()) facilities = STATION_FACILITY_GHOST;

				if (!facilities.Any(_facility_display_opt)) break;

				/* Don't draw if station is owned by another company and competitor station names are hidden. Stations owned by none are never ignored. */
				if (!show_competitors && _local_company != st->owner && st->owner != OWNER_NONE) break;

				stations.push_back(st);
				break;
			}

			case ViewportSignKdtreeItem::VKI_WAYPOINT: {
				if (!show_waypoints) break;
				const BaseStation *st = BaseStation::Get(std::get<StationID>(item.id));

				/* Don't draw if station is owned by another company and competitor station names are hidden. Stations owned by none are never ignored. */
				if (!show_competitors && _local_company != st->owner && st->owner != OWNER_NONE) break;

				stations.push_back(st);
				break;
			}

			case ViewportSignKdtreeItem::VKI_TOWN:
				if (!show_towns) break;
				towns.push_back(Town::Get(std::get<TownID>(item.id)));
				break;

			case ViewportSignKdtreeItem::VKI_SIGN: {
				if (!show_signs) break;
				const Sign *si = Sign::Get(std::get<SignID>(item.id));

				/* Don't draw if sign is owned by another company and competitor signs should be hidden.
				 * Note: It is intentional that also signs owned by OWNER_NONE are hidden. Bankrupt
				 * companies can leave OWNER_NONE signs after them. */
				if (!show_competitors && _local_company != si->owner && si->owner != OWNER_DEITY) break;

				signs.push_back(si);
				break;
			}

			default:
				NOT_REACHED();
		}
	});

	/* Small versions of signs are used zoom level 4X and higher. */
	bool small = dpi->zoom >= ZoomLevel::Out4x;

	/* Layering order (bottom to top): Town names, signs, stations */
	ViewportAddTownStrings(dpi, towns, small);

	/* Do not draw signs nor station names if they are set invisible */
	if (IsInvisibilitySet(TransparencyOption::Signs)) return;

	ViewportAddSignStrings(dpi, signs, small);
	ViewportAddStationStrings(dpi, stations, small);
}


/**
 * Update the position of the viewport sign.
 * @param center the (preferred) center of the viewport sign
 * @param top    the new top of the sign
 * @param str    the string to show in the sign
 * @param str_small the string to show when zoomed out. If the string is empty then the \a str is used.
 */
void ViewportSign::UpdatePosition(int center, int top, std::string_view str, std::string_view str_small)
{
	if (this->width_normal != 0) this->MarkDirty();

	this->top = top;

	this->width_normal = WidgetDimensions::scaled.fullbevel.left + Align(GetStringBoundingBox(str).width, 2) + WidgetDimensions::scaled.fullbevel.right;
	this->center = center;

	/* zoomed out version */
	if (str_small.empty()) str_small = str;
	this->width_small = WidgetDimensions::scaled.fullbevel.left + Align(GetStringBoundingBox(str_small, FontSize::Small).width, 2) + WidgetDimensions::scaled.fullbevel.right;

	this->MarkDirty();
}

/**
 * Mark the sign dirty in all viewports.
 * @param maxzoom Maximum %ZoomLevel at which the text is visible.
 *
 * @ingroup dirty
 */
void ViewportSign::MarkDirty(ZoomLevel maxzoom) const
{
	EnumIndexArray<Rect, ZoomLevel, ZoomLevel::End> zoomlevels;

	/* We don't know which size will be drawn, so mark the largest area dirty. */
	const uint half_width = std::max(this->width_normal, this->width_small) / 2 + 1;
	const uint height = WidgetDimensions::scaled.fullbevel.top + std::max(GetCharacterHeight(FontSize::Normal), GetCharacterHeight(FontSize::Small)) + WidgetDimensions::scaled.fullbevel.bottom + 1;

	for (ZoomLevel zoom : EnumRange(ZoomLevel::End)) {
		zoomlevels[zoom].left = this->center - ScaleByZoom(half_width, zoom);
		zoomlevels[zoom].top = this->top - ScaleByZoom(1, zoom);
		zoomlevels[zoom].right = this->center + ScaleByZoom(half_width, zoom);
		zoomlevels[zoom].bottom = this->top + ScaleByZoom(height, zoom);
	}

	for (const Window *w : Window::Iterate()) {
		if (w->viewport == nullptr) continue;

		Viewport &vp = *w->viewport;
		if (vp.zoom <= maxzoom) {
			assert(vp.width != 0);
			Rect &zl = zoomlevels[vp.zoom];
			MarkViewportDirty(vp, zl.left, zl.top, zl.right, zl.bottom);
		}
	}
}

static void ViewportDrawTileSprites(const TileSpriteToDrawVector *tstdv)
{
	for (const TileSpriteToDraw &ts : *tstdv) {
		DrawSpriteViewport(ts.image, ts.pal, ts.x, ts.y, ts.sub);
	}
}

/**
 * Sort parent sprites pointer array replicating the way original sorter did it.
 * @param psdv The sprites to sort.
 */
static void ViewportSortParentSprites(ParentSpriteToSortVector *psdv)
{
	if (psdv->size() < 2) return;

	/* We rely on sprites being, for the most part, already ordered.
	 * So we don't need to move many of them and can keep track of their
	 * order efficiently by using stack. We always move sprites to the front
	 * of the current position, i.e. to the top of the stack.
	 * Also use special constants to indicate sorting state without
	 * adding extra fields to ParentSpriteToDraw structure.
	 */
	const uint32_t ORDER_COMPARED = UINT32_MAX; // Sprite was compared but we still need to compare the ones preceding it
	const uint32_t ORDER_RETURNED = UINT32_MAX - 1; // Mark sorted sprite in case there are other occurrences of it in the stack
	std::stack<ParentSpriteToDraw *> sprite_order;
	uint32_t next_order = 0;

	std::forward_list<std::pair<int64_t, ParentSpriteToDraw *>> sprite_list;  // We store sprites in a list sorted by xmin+ymin

	/* Initialize sprite list and order. */
	for (auto p = psdv->rbegin(); p != psdv->rend(); p++) {
		sprite_list.emplace_front((*p)->xmin + (*p)->ymin, *p);
		sprite_order.push(*p);
		(*p)->order = next_order++;
	}

	sprite_list.sort();

	std::vector<ParentSpriteToDraw *> preceding;  // Temporarily stores sprites that precede current and their position in the list
	auto preceding_prev = sprite_list.begin(); // Store iterator in case we need to delete a single preceding sprite
	auto out = psdv->begin();  // Iterator to output sorted sprites

	while (!sprite_order.empty()) {

		auto s = sprite_order.top();
		sprite_order.pop();

		/* Sprite is already sorted, ignore it. */
		if (s->order == ORDER_RETURNED) continue;

		/* Sprite was already compared, just need to output it. */
		if (s->order == ORDER_COMPARED) {
			*(out++) = s;
			s->order = ORDER_RETURNED;
			continue;
		}

		preceding.clear();

		/* We only need sprites with xmin <= s->xmax && ymin <= s->ymax && zmin <= s->zmax
		 * So by iterating sprites with xmin + ymin <= s->xmax + s->ymax
		 * we get all we need and some more that we filter out later.
		 * We don't include zmin into the sum as there are usually more neighbours on x and y than z
		 * so including it will actually increase the amount of false positives.
		 * Also min coordinates can be > max so using max(xmin, xmax) + max(ymin, ymax)
		 * to ensure that we iterate the current sprite as we need to remove it from the list.
		 */
		auto ssum = std::max(s->xmax, s->xmin) + std::max(s->ymax, s->ymin);
		auto prev = sprite_list.before_begin();
		auto x = sprite_list.begin();
		while (x != sprite_list.end() && x->first <= ssum) {
			auto p = x->second;
			if (p == s) {
				/* We found the current sprite, remove it and move on. */
				x = sprite_list.erase_after(prev);
				continue;
			}

			auto p_prev = prev;
			prev = x++;

			if (s->xmax < p->xmin || s->ymax < p->ymin || s->zmax < p->zmin) continue;
			if (s->xmin <= p->xmax && // overlap in X?
					s->ymin <= p->ymax && // overlap in Y?
					s->zmin <= p->zmax) { // overlap in Z?
				if (s->xmin + s->xmax + s->ymin + s->ymax + s->zmin + s->zmax <=
						p->xmin + p->xmax + p->ymin + p->ymax + p->zmin + p->zmax) {
					continue;
				}
			}
			preceding.push_back(p);
			preceding_prev = p_prev;
		}

		if (preceding.empty()) {
			/* No preceding sprites, add current one to the output */
			*(out++) = s;
			s->order = ORDER_RETURNED;
			continue;
		}

		/* Optimization for the case when we only have 1 sprite to move. */
		if (preceding.size() == 1) {
			auto p = preceding[0];
			/* We can only output the preceding sprite if there can't be any other sprites preceding it. */
			if (p->xmax <= s->xmax && p->ymax <= s->ymax && p->zmax <= s->zmax) {
				p->order = ORDER_RETURNED;
				s->order = ORDER_RETURNED;
				sprite_list.erase_after(preceding_prev);
				*(out++) = p;
				*(out++) = s;
				continue;
			}
		}

		/* Sort all preceding sprites by order and assign new orders in reverse (as original sorter did). */
		std::sort(preceding.begin(), preceding.end(), [](const ParentSpriteToDraw *a, const ParentSpriteToDraw *b) {
			return a->order > b->order;
		});

		s->order = ORDER_COMPARED;
		sprite_order.push(s);  // Still need to output so push it back for now

		for (auto p: preceding) {
			p->order = next_order++;
			sprite_order.push(p);
		}
	}
}


static void DrawTurnedObject(const DrawPixelInfo &frame, const ParentSpriteToDraw &ps);
static void DrawTurnedVehicle(const DrawPixelInfo &frame, const ParentSpriteToDraw &ps, bool north);

static void ViewportDrawParentSprites(const ParentSpriteToSortVector *psd, const ChildScreenSpriteToDrawVector *csstdv)
{
	for (const ParentSpriteToDraw *ps : *psd) {
		/* Facing north, a house that is a 3D object as in the turned view (gui.three_d_north_houses): the object
		 * once, whole, in place of the sprites of its tiles (AddNorthHouseObject()). */
		if (ps->box_kind == 4) continue;
		if (ps->box_kind == 7) {
			DrawTurnedObject(*_cur_dpi, *ps);
			continue;
		}
		/* A figure among them as its likeness (the clay town's mushrooms, ViewportDrawingNorthObjects()). */
		if (ps->box_kind == 5) {
			DrawTurnedVehicle(*_cur_dpi, *ps, true);
			continue;
		}
		if (ps->image != SPR_EMPTY_BOUNDING_BOX) DrawSpriteViewport(ps->image, ps->pal, ps->x, ps->y, ps->sub);

		int child_idx = ps->first_child;
		while (child_idx >= 0) {
			const ChildScreenSpriteToDraw *cs = &(*csstdv)[child_idx];
			child_idx = cs->next;
			if (cs->relative) {
				DrawSpriteViewport(cs->image, cs->pal, ps->left + cs->x, ps->top + cs->y, cs->sub);
			} else {
				DrawSpriteViewport(cs->image, cs->pal, ps->x + cs->x, ps->y + cs->y, cs->sub);
			}
		}
	}
}

/**
 * Draws the bounding boxes of all ParentSprites
 * @param psd Array of ParentSprites
 */
static void ViewportDrawBoundingBoxes(const ParentSpriteToSortVector *psd)
{
	for (const ParentSpriteToDraw *ps : *psd) {
		Point pt1 = RemapCoords(ps->xmax + 1, ps->ymax + 1, ps->zmax + 1); // top front corner
		Point pt2 = RemapCoords(ps->xmin    , ps->ymax + 1, ps->zmax + 1); // top left corner
		Point pt3 = RemapCoords(ps->xmax + 1, ps->ymin    , ps->zmax + 1); // top right corner
		Point pt4 = RemapCoords(ps->xmax + 1, ps->ymax + 1, ps->zmin    ); // bottom front corner

		DrawBox(        pt1.x,         pt1.y,
		        pt2.x - pt1.x, pt2.y - pt1.y,
		        pt3.x - pt1.x, pt3.y - pt1.y,
		        pt4.x - pt1.x, pt4.y - pt1.y);
	}
}

/**
 * Draw/colour the blocks that have been redrawn.
 */
static void ViewportDrawDirtyBlocks()
{
	Blitter *blitter = BlitterFactory::GetCurrentBlitter();
	const DrawPixelInfo *dpi = _cur_dpi;
	void *dst;
	int right =  UnScaleByZoom(dpi->width,  dpi->zoom);
	int bottom = UnScaleByZoom(dpi->height, dpi->zoom);

	PixelColour colour = _string_colourmap[_dirty_block_colour & 0xF];

	dst = dpi->dst_ptr;

	uint8_t bo = UnScaleByZoom(dpi->left + dpi->top, dpi->zoom) & 1;
	do {
		for (int i = (bo ^= 1); i < right; i += 2) blitter->SetPixel(dst, i, 0, colour);
		dst = blitter->MoveTo(dst, 0, 1);
	} while (--bottom > 0);
}

static void ViewportDrawStrings(ZoomLevel zoom, const StringSpriteToDrawVector *sstdv)
{
	for (const StringSpriteToDraw &ss : *sstdv) {
		bool small = ss.flags.Test(ViewportStringFlag::Small);
		int w = ss.width;
		int x = UnScaleByZoom(ss.x, zoom);
		int y = UnScaleByZoom(ss.y, zoom);
		int h = WidgetDimensions::scaled.fullbevel.top + GetCharacterHeight(small ? FontSize::Small : FontSize::Normal) + WidgetDimensions::scaled.fullbevel.bottom;

		ExtendedTextColour colour{TextColour::White};
		if (ss.flags.Test(ViewportStringFlag::ColourRect)) {
			if (ss.colour != Colours::Invalid) DrawFrameRect(x, y, x + w - 1, y + h - 1, ss.colour, {});
			colour = TextColour::Black;
		} else if (ss.flags.Test(ViewportStringFlag::TransparentRect)) {
			DrawFrameRect(x, y, x + w - 1, y + h - 1, ss.colour, FrameFlag::Transparent);
		}

		if (ss.flags.Test(ViewportStringFlag::TextColour)) {
			if (ss.colour != Colours::Invalid) colour = ExtendedTextColour{GetColourGradient(ss.colour, Shade::Lighter)};
		}

		int left = x + WidgetDimensions::scaled.fullbevel.left;
		int right = x + w - 1 - WidgetDimensions::scaled.fullbevel.right;
		int top = y + WidgetDimensions::scaled.fullbevel.top;

		int shadow_offset = 0;
		if (small && ss.flags.Test(ViewportStringFlag::Shadow)) {
			/* Shadow needs to be shifted 1 pixel. */
			shadow_offset = WidgetDimensions::scaled.fullbevel.top;
			DrawString(left + shadow_offset, right + shadow_offset, top, ss.string, TextColour::Black, AlignmentH::Centre, false, FontSize::Small);
		}

		DrawString(left, right, top - shadow_offset, ss.string, colour, AlignmentH::Centre, false, small ? FontSize::Small : FontSize::Normal);
	}
}

/*
 * The turned view: the second renderer (three_d.h).
 *
 * The world is turned about a pivot and drawn by the game's own projection.
 * The ground keeps its shape: every tile's ground sprites are drawn north-up
 * into a small texture and the texture is laid over the four corners of the
 * tile where the turned view puts them (four triangles meeting in the middle
 * of the tile, so slopes keep their folds). Everything that stands -- houses,
 * trees, vehicles, foundations with the ground on them -- is a billboard: the
 * sprite as it is, hung from its anchor point turned, drawn far to near.
 * Vehicles show the picture of the direction they face as seen from the turn
 * (vehicle.cpp, DoDrawVehicle()). Signs and floating texts are left out for
 * now.
 */

/** A corner of a turned ground triangle: where it lands in the block being drawn and where it is in the tile's texture, both in pixels. */
struct TurnedVertex {
	double x, y; ///< pixel in the block being drawn
	double u, v; ///< pixel in the texture
	double l = 256; ///< the light there, 256 the texture as it is (a 3D object's light from its normals, three_d_objects.h)
	double d = 0;   ///< how near it is, for a depth buffer (a 3D object, DrawObjectInto()): larger nearer
};

/** A pixel of a texture in a light: 256 as it is. */
static inline uint32_t ShadedPixel(uint32_t p, uint light)
{
	Colour c(p);
	return Colour(static_cast<uint8_t>(std::min<uint>(255, c.r * light >> 8)), static_cast<uint8_t>(std::min<uint>(255, c.g * light >> 8)), static_cast<uint8_t>(std::min<uint>(255, c.b * light >> 8)), c.a).data;
}

/**
 * Fill a triangle with the pixels of a texture: every pixel inside takes the
 * texture pixel of the same world point. Both maps are affine within a
 * triangle, so the texture coordinate changes linearly across it. Pixels the
 * texture has nothing for (value 0) are left as they are.
 * @param fill whether a pixel the texture has nothing for takes the nearest drawn pixel within three (the edges of
 *             the ground's diamonds and of solid things); not for a thing of thin lines on nothing, a bridge's
 *             cables, which it would fill into a sheet
 * @param zb a depth buffer as large as the pixels, or nullptr: a pixel is drawn only where it is nearer than what is there
 */
/**
 * The night's lit pixels of the block being drawn (the windows of the houses, a 3D object's "noc"): where each is and
 * the colour it was drawn in, kept bright when the night darkens the block (DrawNight()) -- unless something drawn
 * later covered it. And the places of the glows (its "svetlo_NN"), world units. Only while the night is drawn.
 */
static bool _night_windows_now = false;
static std::vector<std::pair<uint32_t *, uint32_t>> _night_lit;
static std::vector<std::array<double, 3>> _night_glows;

static void ParallelRows(int rows, const std::function<void(int, int)> &work, bool worth = true);
static void ParallelBands(int rows, const std::function<void(int, int)> &work, bool worth);

static void DrawTexturedTriangle(uint32_t *dst, int pitch, int w, int h, const uint32_t *tex, int tw, int th, const TurnedVertex &a, const TurnedVertex &b, const TurnedVertex &c, bool fill = true, float *zb = nullptr, std::vector<std::pair<uint32_t *, uint32_t>> *lit_pixels = nullptr, int row_lo = 0, int row_hi = INT_MAX)
{
	/* Its rows first: of a band of the picture (ParallelBands()) most triangles have none. */
	int y0 = std::max(std::max(0, row_lo), static_cast<int>(std::ceil(std::min({a.y, b.y, c.y}) - 0.5)));
	int y1 = std::min(std::min(h, row_hi) - 1, static_cast<int>(std::floor(std::max({a.y, b.y, c.y}) - 0.5)));
	if (y0 > y1) return;
	double area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
	if (std::abs(area) < 1e-9) return;
	double du_dx = ((b.u - a.u) * (c.y - a.y) - (c.u - a.u) * (b.y - a.y)) / area;
	double du_dy = ((c.u - a.u) * (b.x - a.x) - (b.u - a.u) * (c.x - a.x)) / area;
	double dv_dx = ((b.v - a.v) * (c.y - a.y) - (c.v - a.v) * (b.y - a.y)) / area;
	double dv_dy = ((c.v - a.v) * (b.x - a.x) - (b.v - a.v) * (c.x - a.x)) / area;
	const bool lit = a.l != 256 || b.l != 256 || c.l != 256;
	double dl_dx = 0, dl_dy = 0;
	if (lit) {
		dl_dx = ((b.l - a.l) * (c.y - a.y) - (c.l - a.l) * (b.y - a.y)) / area;
		dl_dy = ((c.l - a.l) * (b.x - a.x) - (b.l - a.l) * (c.x - a.x)) / area;
	}
	double dd_dx = 0, dd_dy = 0;
	if (zb != nullptr) {
		dd_dx = ((b.d - a.d) * (c.y - a.y) - (c.d - a.d) * (b.y - a.y)) / area;
		dd_dy = ((c.d - a.d) * (b.x - a.x) - (b.d - a.d) * (c.x - a.x)) / area;
	}
	double sign = area > 0 ? 1.0 : -1.0;
	for (int y = y0; y <= y1; y++) {
		double yc = y + 0.5;
		double xlo = -1e18, xhi = 1e18;
		/* Inside is where E(x) = (q.x - p.x) * (yc - p.y) - (q.y - p.y) * (x - p.x) has the sign of the area, for each edge p -> q. */
		auto edge = [&](const TurnedVertex &p, const TurnedVertex &q) {
			double k = -(q.y - p.y) * sign;
			double e0 = ((q.x - p.x) * (yc - p.y) + (q.y - p.y) * p.x) * sign;
			if (k > 1e-12) {
				xlo = std::max(xlo, -e0 / k);
			} else if (k < -1e-12) {
				xhi = std::min(xhi, -e0 / k);
			} else if (e0 < 0) {
				xlo = 1e18;
			}
		};
		edge(a, b);
		edge(b, c);
		edge(c, a);
		int xs = std::max(0, static_cast<int>(std::ceil(xlo - 0.5)));
		int xe = std::min(w - 1, static_cast<int>(std::floor(xhi - 0.5)));
		if (xs > xe) continue;
		double u = a.u + du_dx * (xs + 0.5 - a.x) + du_dy * (yc - a.y);
		double v = a.v + dv_dx * (xs + 0.5 - a.x) + dv_dy * (yc - a.y);
		double l = a.l + dl_dx * (xs + 0.5 - a.x) + dl_dy * (yc - a.y);
		double d = a.d + dd_dx * (xs + 0.5 - a.x) + dd_dy * (yc - a.y);
		uint32_t *row = dst + y * pitch;
		float *zrow = zb != nullptr ? zb + static_cast<size_t>(y) * w : nullptr;
		for (int x = xs; x <= xe; x++, u += du_dx, v += dv_dx, l += dl_dx, d += dd_dx) {
			/* Behind what is there already, by more than a hair (a road laid over the grass a few centimetres up). */
			if (zrow != nullptr && d < zrow[x] - 0.05) continue;
			int iu = static_cast<int>(std::floor(u)), iv = static_cast<int>(std::floor(v));
			if (iu < 0 || iv < 0 || iu >= tw || iv >= th) continue;
			uint32_t p = tex[iv * tw + iu];
			if (p == 0 && fill) {
				/* The edge of a face falls on the outermost pixels of its diamond, whose sides step two pixels at a
				 * time and whose tips are four apart: the nearest drawn pixel within three is the right one then. */
				static const int near[24][2] = {
					{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {-1, 1}, {1, -1}, {-1, -1},
					{2, 0}, {-2, 0}, {0, 2}, {0, -2}, {2, 1}, {-2, 1}, {2, -1}, {-2, -1},
					{3, 0}, {-3, 0}, {0, 3}, {0, -3}, {3, 1}, {-3, 1}, {3, -1}, {-3, -1},
				};
				for (const auto &[dx, dy] : near) {
					int nu = iu + dx, nv = iv + dy;
					if (nu < 0 || nv < 0 || nu >= tw || nv >= th) continue;
					p = tex[nv * tw + nu];
					if (p != 0) break;
				}
			}
			if (p == 0) continue;
			row[x] = lit ? ShadedPixel(p, static_cast<uint>(std::max(0.0, l))) : p;
			if (zrow != nullptr) zrow[x] = static_cast<float>(d);
			if (lit_pixels != nullptr) lit_pixels->emplace_back(&row[x], row[x]);
		}
	}
}

/** RemapCoords() of the view facing north, not rounded: where a world point is in a tile's north-up texture. */
static inline void RemapCoordsF(double x, double y, double z, double &vx, double &vy)
{
	vx = (y - x) * 2 * ZOOM_BASE;
	vy = (y + x - z) * ZOOM_BASE;
}

static std::vector<uint32_t> _turned_texture; ///< The north-up picture of the tile or house being drawn turned.
static std::vector<uint32_t> _turned_big; ///< The north-up picture of the ground of the whole block being drawn turned.
TileIndex _turned_debug_tile = INVALID_TILE; ///< Rig: the tile whose turned drawing is written out (test3d ladeni).

/** Rig: where the time of the turned drawing goes (test3d cas), milliseconds since the last report. */
struct TurnedTimes {
	double texture = 0; ///< drawing the tiles' sprites into the textures
	double widen = 0;   ///< growing the ground over the footprints of buildings
	double warp = 0;    ///< laying the textures over the faces
	double collect = 0; ///< the tiles' draw procedures
	double standing = 0; ///< the billboards
	double models = 0;  ///< the buildings that are 3D objects: drawn
	double models_project = 0; ///< of that: their corners projected and their triangles sorted
	double models_raster = 0;  ///< of that: their pixels
	int models_drawn = 0;      ///< objects drawn (an object in several blocks, once for each)
	int tiles = 0;      ///< tiles looked at
	int drawn = 0;      ///< tiles drawn
};
static TurnedTimes _turned_times;

static inline double TurnedNow()
{
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

/** Rig: print where the time of the turned drawing went since the last call, and start over. */
void TurnedTimingsPrint()
{
	const TurnedTimes &t = _turned_times;
	IConsolePrint(CC_DEFAULT, "test3d: dlazdic {} kresleno {}; ms: sprity do textur {:.1f}, stopy domu {:.1f}, polozeni {:.1f}, sber {:.1f}, plakaty {:.1f}, modely {:.1f} (body a razeni {:.1f}, pixely {:.1f}, {} kresleni)",
			t.tiles, t.drawn, t.texture, t.widen, t.warp, t.collect, t.standing, t.models, t.models_project, t.models_raster, t.models_drawn);
	_turned_times = TurnedTimes();
}

/**
 * The ground under a house or an industry is drawn black where the building
 * stands, for the building's sprite to cover. Turned, the ground turns and the
 * billboard of the building does not, so the black would show beside it.
 * Take the black out and let the ground around grow over it, on the rows x0..x1, y0..y1 of a texture.
 */
static void FillTurnedFootprint(uint32_t *tex, int tw, int x0, int y0, int x1, int y1)
{
	for (int y = y0; y < y1; y++) {
		uint32_t *row = tex + y * tw;
		for (int x = x0; x < x1; x++) {
			Colour c;
			c.data = row[x];
			if (c.data == 0 || c.r > 24 || c.g > 24 || c.b > 24) continue;
			/* A run of black: filled from the drawn pixel next to it on either side, the nearer one for each pixel. */
			int end = x;
			while (end < x1) {
				c.data = row[end];
				if (c.data == 0 || c.r > 24 || c.g > 24 || c.b > 24) break;
				end++;
			}
			uint32_t left = (x > x0) ? row[x - 1] : 0;
			uint32_t right = (end < x1) ? row[end] : 0;
			if (left == 0) left = right;
			if (right == 0) right = left;
			for (int i = x; i < end; i++) row[i] = (i - x < end - 1 - i) ? left : right;
			x = end;
		}
	}
}

/**
 * Draw the ground of one tile turned. The tile sprites it added (from index
 * \a first on), its foundations and the ground drawn on top of them are drawn
 * north-up into a texture. The tile is then a box: its top is the ground
 * (four triangles meeting in the middle, so slopes keep their folds), its
 * sides are the walls of the foundation, from the land below up to the ground
 * on top, wherever the foundation raised the ground. Every face is laid over
 * its corners where the turned view puts them, with the pixels the texture has
 * at the same world points. The two walls the game draws (south-west and
 * south-east) give their pixels to the two it never draws, mirrored through
 * the middle of the tile. The foundation sprites are taken off the billboard
 * list (ViewportDoDrawTurned()), since they are here.
 * @param frame the block of the screen being drawn (virtual coordinates of the turned view)
 * @param first the first tile sprite of this tile in _vd.tile_sprites_to_draw
 * @param end one past its last
 * @param x0,y0 the north corner of the tile (world units)
 * @param top_slope the slope of the ground, on top of the foundation if the tile has one
 * @param top_z the height of the ground's north corner on flat ground, on top of the foundation if the tile has one
 * @param raw_slope the slope of the land itself
 * @param raw_z the height of the land's north corner on flat ground
 * @param building whether a building stands on the tile, with the ground under it drawn black (FillTurnedFootprint())
 * @param foundation_index the tile's foundation sprites (normal and halftile part) in _vd.parent_sprites_to_draw, or -1
 */
static void DrawTurnedTileGround(const DrawPixelInfo &frame, size_t first, size_t end, int x0, int y0, Slope top_slope, int top_z, Slope raw_slope, int raw_z, bool building, const int foundation_index[2])
{
	bool foundation = foundation_index[0] != -1 || foundation_index[1] != -1;
	if (first >= end && !foundation) return;
	ZoomLevel zoom = frame.zoom;
	int mask = ScaleByZoom(-1, zoom);
	Slope st = RemoveHalftileSlope(top_slope), sr = RemoveHalftileSlope(raw_slope);

	/* The corners north, east, south, west and the middle: where they are, the height of the ground on top and of the land below. */
	static const Corner corners[4] = {CORNER_N, CORNER_E, CORNER_S, CORNER_W};
	static const int cxs[4] = {0, 0, TILE_SIZE, TILE_SIZE};
	static const int cys[4] = {0, TILE_SIZE, TILE_SIZE, 0};
	double wx[5], wy[5], wz[5], bz[4];
	for (int i = 0; i < 4; i++) {
		wx[i] = x0 + cxs[i];
		wy[i] = y0 + cys[i];
		wz[i] = top_z + GetSlopePixelZInCorner(st, corners[i]);
		bz[i] = raw_z + GetSlopePixelZInCorner(sr, corners[i]);
	}
	wx[4] = x0 + TILE_SIZE / 2.0;
	wy[4] = y0 + TILE_SIZE / 2.0;
	wz[4] = top_z + GetPartialPixelZ(TILE_SIZE / 2, TILE_SIZE / 2, st);

	/* The texture: the tile north-up, a tile wide, from above the highest corner to below the lowest. */
	double minvy = 1e18, maxvy = -1e18;
	for (int i = 0; i < 5; i++) {
		double vx, vy;
		RemapCoordsF(wx[i], wy[i], wz[i], vx, vy);
		minvy = std::min(minvy, vy);
		maxvy = std::max(maxvy, vy);
		if (i < 4) {
			RemapCoordsF(wx[i], wy[i], bz[i], vx, vy);
			minvy = std::min(minvy, vy);
			maxvy = std::max(maxvy, vy);
		}
	}
	const int margin = TILE_HEIGHT * ZOOM_BASE;
	int tleft = (RemapCoords(x0 + TILE_SIZE, y0, 0).x - margin) & mask;
	int ttop = (static_cast<int>(std::floor(minvy)) - margin) & mask;
	int twidth = (2 * TILE_SIZE * 2 * ZOOM_BASE + 2 * margin + ~mask) & mask;
	int theight = (static_cast<int>(std::ceil(maxvy)) + margin - ttop + ~mask) & mask;
	int tw = UnScaleByZoom(twidth, zoom), th = UnScaleByZoom(theight, zoom);
	if (tw <= 0 || th <= 0) return;
	double t0 = TurnedNow();
	_turned_texture.assign(static_cast<size_t>(tw) * th, 0);
	DrawPixelInfo tdpi{
		.dst_ptr = _turned_texture.data(),
		.left = tleft,
		.top = ttop,
		.width = twidth,
		.height = theight,
		.pitch = tw,
		.zoom = zoom
	};
	{
		AutoRestoreBackup dpi_backup(_cur_dpi, &tdpi);
		AutoRestoreBackup anim_backup(_screen_disable_anim, true);
		for (size_t i = first; i < end; i++) {
			const TileSpriteToDraw &ts = _vd.tile_sprites_to_draw[i];
			DrawSpriteViewport(ts.image, ts.pal, ts.x, ts.y, ts.sub);
		}
		/* The foundation and the ground on top of it, which is its child sprites: drawn here, taken off the billboards. */
		for (int k = 0; k < 2; k++) {
			if (foundation_index[k] == -1) continue;
			ParentSpriteToDraw &ps = _vd.parent_sprites_to_draw[foundation_index[k]];
			if (ps.image != SPR_EMPTY_BOUNDING_BOX) DrawSpriteViewport(ps.image, ps.pal, ps.x, ps.y, ps.sub);
			int child_idx = ps.first_child;
			while (child_idx >= 0) {
				const ChildScreenSpriteToDraw &cs = _vd.child_screen_sprites_to_draw[child_idx];
				child_idx = cs.next;
				if (_turned_debug_tile != INVALID_TILE && _cur_ti.tile == _turned_debug_tile) {
					IConsolePrint(CC_DEFAULT, "ladeni:   dite zakladu {} pal {} na {},{} {}", cs.image, cs.pal, cs.x, cs.y, cs.relative ? "od rohu" : "od kotvy");
				}
				if (cs.relative) {
					DrawSpriteViewport(cs.image, cs.pal, ps.left + cs.x, ps.top + cs.y, cs.sub);
				} else {
					DrawSpriteViewport(cs.image, cs.pal, ps.x + cs.x, ps.y + cs.y, cs.sub);
				}
			}
			ps.first_child = LAST_CHILD_NONE;
			ps.image = SPR_EMPTY_BOUNDING_BOX;
		}
	}
	if (_turned_debug_tile != INVALID_TILE && _cur_ti.tile == _turned_debug_tile) {
		/* Rig: the texture as a picture, textura.ppm next to the executable. */
		auto f = FileHandle::Open(std::string("textura.ppm"), "wb");
		if (f.has_value()) {
			std::string head = fmt::format("P6\n{} {}\n255\n", tw, th);
			fwrite(head.data(), 1, head.size(), *f);
			for (uint32_t p : _turned_texture) {
				Colour c;
				c.data = p;
				uint8_t rgb[3] = {c.r, c.g, c.b};
				fwrite(rgb, 1, 3, *f);
			}
		}
	}
	double t1 = TurnedNow();
	if (building) FillTurnedFootprint(_turned_texture.data(), tw, 0, 0, tw, th);
	double t2 = TurnedNow();
	_turned_times.texture += t1 - t0;
	_turned_times.widen += t2 - t1;

	/* A corner of a face: a world point on the screen, with its pixels taken at another (or the same) world point of the texture. */
	double scale = 1.0 / (1 << to_underlying(zoom));
	auto vertex = [&](double x, double y, double z, double tx, double ty, double tz) {
		TurnedVertex r;
		double fx, fy, cx, cy;
		ProjectTurnedF(x, y, z, fx, fy);
		RemapCoordsF(tx, ty, tz, cx, cy);
		r.x = (fx - frame.left) * scale;
		r.y = (fy - frame.top) * scale;
		r.u = (cx - tleft) * scale;
		r.v = (cy - ttop) * scale;
		return r;
	};
	uint32_t *dst = static_cast<uint32_t *>(frame.dst_ptr);
	int fw = UnScaleByZoom(frame.width, zoom), fh = UnScaleByZoom(frame.height, zoom);
	const uint32_t *tex = _turned_texture.data();

	/* The walls, far first: the edges N-E, E-S, S-W, W-N wherever the ground on top is above the land. */
	struct Wall {
		int a, b;
		double near;
	};
	Wall walls[4];
	int num_walls = 0;
	for (int e = 0; e < 4; e++) {
		int a = e, b = (e + 1) & 3;
		if (wz[a] <= bz[a] + 1e-6 && wz[b] <= bz[b] + 1e-6) continue;
		double xr, yr;
		TurnWorldPoint((wx[a] + wx[b]) / 2, (wy[a] + wy[b]) / 2, xr, yr);
		walls[num_walls++] = {a, b, xr + yr};
	}
	std::sort(walls, walls + num_walls, [](const Wall &p, const Wall &q) { return p.near < q.near; });
	for (int i = 0; i < num_walls; i++) {
		int a = walls[i].a, b = walls[i].b;
		/* The game draws the walls below the edges E-S and S-W; the other two take their pixels mirrored through the middle. */
		bool drawn = (a == 1 && b == 2) || (a == 2 && b == 3);
		int ta = drawn ? a : (a + 2) & 3, tb = drawn ? b : (b + 2) & 3;
		TurnedVertex a_top = vertex(wx[a], wy[a], wz[a], wx[ta], wy[ta], wz[ta]);
		TurnedVertex b_top = vertex(wx[b], wy[b], wz[b], wx[tb], wy[tb], wz[tb]);
		TurnedVertex b_bot = vertex(wx[b], wy[b], bz[b], wx[tb], wy[tb], bz[tb]);
		TurnedVertex a_bot = vertex(wx[a], wy[a], bz[a], wx[ta], wy[ta], bz[ta]);
		if (_turned_debug_tile != INVALID_TILE && _cur_ti.tile == _turned_debug_tile) {
			IConsolePrint(CC_DEFAULT, "ladeni:   stena {}-{} {} blok ({:.1f},{:.1f}) ({:.1f},{:.1f}) ({:.1f},{:.1f}) ({:.1f},{:.1f}) textura ({:.1f},{:.1f}) ({:.1f},{:.1f}) ({:.1f},{:.1f}) ({:.1f},{:.1f})",
					a, b, drawn ? "kreslena" : "zrcadlena", a_top.x, a_top.y, b_top.x, b_top.y, b_bot.x, b_bot.y, a_bot.x, a_bot.y, a_top.u, a_top.v, b_top.u, b_top.v, b_bot.u, b_bot.v, a_bot.u, a_bot.v);
		}
		DrawTexturedTriangle(dst, frame.pitch, fw, fh, tex, tw, th, a_top, b_top, b_bot);
		DrawTexturedTriangle(dst, frame.pitch, fw, fh, tex, tw, th, a_top, b_bot, a_bot);
	}

	/* The top: the ground, four triangles meeting in the middle. */
	TurnedVertex v[5];
	for (int i = 0; i < 5; i++) v[i] = vertex(wx[i], wy[i], wz[i], wx[i], wy[i], wz[i]);
	if (_turned_debug_tile != INVALID_TILE && _cur_ti.tile == _turned_debug_tile) {
		size_t painted = 0;
		for (uint32_t p : _turned_texture) if (p != 0) painted++;
		IConsolePrint(CC_DEFAULT, "ladeni: dlazdice {},{} vrch svah {} z {} zem svah {} z {} spritu {} zaklady {} sten {} textura {}x{} ({},{}) kresleno {}", x0 / TILE_SIZE, y0 / TILE_SIZE,
				to_underlying(top_slope), top_z, to_underlying(raw_slope), raw_z, end - first, foundation ? "ano" : "ne", num_walls, tw, th, tleft, ttop, painted);
		for (int i = 0; i < 5; i++) {
			IConsolePrint(CC_DEFAULT, "ladeni:   roh {} svet {},{},{} zem {} blok {:.1f},{:.1f} textura {:.1f},{:.1f}", i, wx[i], wy[i], wz[i], i < 4 ? bz[i] : wz[i], v[i].x, v[i].y, v[i].u, v[i].v);
		}
	}
	DrawTexturedTriangle(dst, frame.pitch, fw, fh, tex, tw, th, v[0], v[1], v[4]);
	DrawTexturedTriangle(dst, frame.pitch, fw, fh, tex, tw, th, v[1], v[2], v[4]);
	DrawTexturedTriangle(dst, frame.pitch, fw, fh, tex, tw, th, v[2], v[3], v[4]);
	DrawTexturedTriangle(dst, frame.pitch, fw, fh, tex, tw, th, v[3], v[0], v[4]);
	_turned_times.warp += TurnedNow() - t2;
}

/*
 * A house of the original graphics as boxes (ThreeDHouse, table/three_d_houses.h):
 * its one sprite is laid over the faces of its boxes the way the ground is
 * laid over a tile. A face the sprite shows (the walls facing +X and +Y, the
 * roof slopes facing them, a flat top) takes its pixels where the game draws
 * it; a face the sprite never shows takes them from the opposite face of the
 * same box, mirrored, as the hand tables were made (model3d.py). Faces are
 * drawn far to near. The boxes are cut to the tile being drawn, so a house of
 * several tiles is drawn tile by tile, each from its own sprite.
 */

/** A corner of a face of a house box, in the frame of the house (tile units and pixels): the point, and the point the sprite shows it at. */
struct BoxVertex {
	double x, y, z;
	double tx, ty, tz;
};

/** A face of a house box: a convex polygon and how near its middle is in the turned view. */
struct BoxFace {
	std::vector<BoxVertex> v;
	double near;
};

/** Cut a convex polygon to one side of a line x = limit (axis 0) or y = limit (axis 1), the texture point cut along with it. */
static void ClipBoxPolygon(std::vector<BoxVertex> &poly, int axis, double limit, bool keep_greater)
{
	std::vector<BoxVertex> out;
	size_t n = poly.size();
	for (size_t i = 0; i < n; i++) {
		const BoxVertex &a = poly[i], &b = poly[(i + 1) % n];
		double va = axis == 0 ? a.x : a.y, vb = axis == 0 ? b.x : b.y;
		bool ina = keep_greater ? va >= limit : va <= limit;
		bool inb = keep_greater ? vb >= limit : vb <= limit;
		if (ina) out.push_back(a);
		if (ina != inb) {
			double t = (limit - va) / (vb - va);
			out.push_back({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
					a.tx + (b.tx - a.tx) * t, a.ty + (b.ty - a.ty) * t, a.tz + (b.tz - a.tz) * t});
		}
	}
	poly.swap(out);
}

/** The faces of one box of a house (ThreeDPart), each cut to the tile being drawn. */
static void AddBoxFaces(std::vector<BoxFace> &faces, const ThreeDPart &part, bool cut, double cx0, double cx1, double cy0, double cy1, double ox, double oy)
{
	/* The sprite's outline pixels sit half a pixel outside the box's true edges. */
	const double margin = 0.25;
	double x0 = part.x0 - margin, x1 = part.x1 + margin, y0 = part.y0 - margin, y1 = part.y1 + margin;
	double h = part.h + 2 * margin, rise = part.rise;
	double mx = (x0 + x1) / 2, my = (y0 + y1) / 2;

	auto same = [](double x, double y, double z) { return BoxVertex{x, y, z, x, y, z}; };
	auto add = [&](std::vector<BoxVertex> poly) {
		if (cut) {
			ClipBoxPolygon(poly, 0, cx0, true);
			ClipBoxPolygon(poly, 0, cx1, false);
			ClipBoxPolygon(poly, 1, cy0, true);
			ClipBoxPolygon(poly, 1, cy1, false);
		}
		if (poly.size() < 3) return;
		double sx = 0, sy = 0, sz = 0;
		for (const BoxVertex &v : poly) {
			sx += v.x;
			sy += v.y;
			sz += v.z;
		}
		double xr, yr;
		TurnWorldPoint(ox + sx / poly.size(), oy + sy / poly.size(), xr, yr);
		faces.push_back({std::move(poly), xr + yr + 2 * sz / poly.size()});
	};

	/* The walls: +X and +Y are the ones the sprite shows; -X and -Y take theirs mirrored to them. */
	std::vector<BoxVertex> wall;
	for (int side = 0; side < 2; side++) {
		double x = side == 0 ? x1 : x0, tx = x1;
		wall = {{x, y0, 0, tx, y0, 0}, {x, y1, 0, tx, y1, 0}, {x, y1, h, tx, y1, h}};
		if (part.roof == ThreeDRoof::GableX) wall.push_back({x, my, h + rise, tx, my, h + rise});
		wall.push_back({x, y0, h, tx, y0, h});
		add(wall);
	}
	for (int side = 0; side < 2; side++) {
		double y = side == 0 ? y1 : y0, ty = y1;
		wall = {{x0, y, 0, x0, ty, 0}, {x1, y, 0, x1, ty, 0}, {x1, y, h, x1, ty, h}};
		if (part.roof == ThreeDRoof::GableY) wall.push_back({mx, y, h + rise, mx, ty, h + rise});
		wall.push_back({x0, y, h, x0, ty, h});
		add(wall);
	}

	/* The roof. */
	auto mirror_x = [&](double x, double y, double z) { return BoxVertex{x, y, z, x0 + x1 - x, y, z}; };
	auto mirror_y = [&](double x, double y, double z) { return BoxVertex{x, y, z, x, y0 + y1 - y, z}; };
	switch (part.roof) {
		case ThreeDRoof::Flat:
			add({same(x0, y0, h), same(x1, y0, h), same(x1, y1, h), same(x0, y1, h)});
			break;
		case ThreeDRoof::GableX:
			/* the ridge along X at the middle of Y: the slope facing +Y is shown, the one facing -Y mirrored */
			add({same(x0, my, h + rise), same(x1, my, h + rise), same(x1, y1, h), same(x0, y1, h)});
			add({mirror_y(x0, y0, h), mirror_y(x1, y0, h), mirror_y(x1, my, h + rise), mirror_y(x0, my, h + rise)});
			break;
		case ThreeDRoof::GableY:
			add({same(mx, y0, h + rise), same(mx, y1, h + rise), same(x1, y1, h), same(x1, y0, h)});
			add({mirror_x(x0, y0, h), mirror_x(x0, y1, h), mirror_x(mx, y1, h + rise), mirror_x(mx, y0, h + rise)});
			break;
		case ThreeDRoof::Hip: {
			double hx = (x1 - x0) / 2, hy = (y1 - y0) / 2;
			if (hx >= hy) {
				/* the ridge along X, shorter than the box by the hip ends */
				double rx0 = mx - (hx - hy), rx1 = mx + (hx - hy);
				add({same(x0, y1, h), same(x1, y1, h), same(rx1, my, h + rise), same(rx0, my, h + rise)});
				add({mirror_y(x0, y0, h), mirror_y(x1, y0, h), mirror_y(rx1, my, h + rise), mirror_y(rx0, my, h + rise)});
				add({same(x1, y0, h), same(x1, y1, h), same(rx1, my, h + rise)});
				add({mirror_x(x0, y0, h), mirror_x(x0, y1, h), mirror_x(rx0, my, h + rise)});
			} else {
				double ry0 = my - (hy - hx), ry1 = my + (hy - hx);
				add({same(x1, y0, h), same(x1, y1, h), same(mx, ry1, h + rise), same(mx, ry0, h + rise)});
				add({mirror_x(x0, y0, h), mirror_x(x0, y1, h), mirror_x(mx, ry1, h + rise), mirror_x(mx, ry0, h + rise)});
				add({same(x0, y1, h), same(x1, y1, h), same(mx, ry1, h + rise)});
				add({mirror_y(x0, y0, h), mirror_y(x1, y0, h), mirror_y(mx, ry0, h + rise)});
			}
			break;
		}
	}
}

static std::vector<BoxFace> _box_faces; ///< The faces of the house being drawn.

/**
 * Draw a sprite of the turned view as boxes (ParentSpriteToDraw::box_kind):
 * the sprite is drawn north-up into a texture and laid over the faces of the
 * boxes, far to near. Only the front of the thing turns, by up to half the
 * period either way; past that it snaps back by a quarter turn (or a half)
 * and its front turns again, so the back of a thing of one sprite is never on
 * show (the player: it is only background).
 * @param frame the block of the screen being drawn
 * @param ps the sprite, already moved to where the turn puts it
 * @param parts the boxes, in the frame of the thing (tile units from box_ox, box_oy; pixels up from box_oz)
 * @param count how many boxes
 * @param cut whether to cut the boxes to the rectangle cx0..cx1, cy0..cy1 of the frame (the tile of a house of several)
 * @param centre_x,centre_y the middle of the thing in its frame, which the boxes snap about
 * @param period the turn after which the front comes round again: 90 degrees for a square thing, 180 for a longer one
 */
/** Where the sprite of a standing thing lies, drawn north-up into _turned_texture (MakeTurnedTexture()). */
struct TurnedTexture {
	int left, top;     ///< its top left corner, virtual pixels
	int width, height; ///< its size, pixels at the frame's zoom
};

/**
 * Draw the sprite of a standing thing, with its children, north-up into _turned_texture, for its pixels to be laid
 * over faces (DrawTurnedFaces()).
 * @return whether there is anything to lay
 */
static bool MakeTurnedTexture(const DrawPixelInfo &frame, const ParentSpriteToDraw &ps, TurnedTexture &tex)
{
	ZoomLevel zoom = frame.zoom;
	int mask = ScaleByZoom(-1, zoom);
	const Sprite *spr = GetSprite(ps.image & SPRITE_MASK, SpriteType::Normal);

	/* Where the game draws the sprite facing north: the turn's shift taken off again. */
	Point c = RemapCoords(ps.ax, ps.ay, ps.az), t = ProjectTurned(ps.ax, ps.ay, ps.az);
	int shift_x = t.x - c.x, shift_y = t.y - c.y;
	int sx = ps.x - shift_x, sy = ps.y - shift_y, sleft = ps.left - shift_x, stop = ps.top - shift_y;
	/* The texture holds the sprite and its children: a depot's door frame, drawn apart from the shed. */
	int bleft = sleft, btop = stop, bright = sleft + spr->width, bbottom = stop + spr->height;
	for (int child_idx = ps.first_child; child_idx >= 0;) {
		const ChildScreenSpriteToDraw &cs = _vd.child_screen_sprites_to_draw[child_idx];
		child_idx = cs.next;
		const Sprite *cspr = GetSprite(cs.image & SPRITE_MASK, SpriteType::Normal);
		int cx = (cs.relative ? sleft : sx) + cs.x + cspr->x_offs, cy = (cs.relative ? stop : sy) + cs.y + cspr->y_offs;
		bleft = std::min(bleft, cx);
		btop = std::min(btop, cy);
		bright = std::max(bright, cx + cspr->width);
		bbottom = std::max(bbottom, cy + cspr->height);
	}
	const int margin = TILE_HEIGHT * ZOOM_BASE;
	int tleft = (bleft - margin) & mask;
	int ttop = (btop - margin) & mask;
	int twidth = (bright + margin - tleft + ~mask) & mask;
	int theight = (bbottom + margin - ttop + ~mask) & mask;
	int tw = UnScaleByZoom(twidth, zoom), th = UnScaleByZoom(theight, zoom);
	if (tw <= 0 || th <= 0) return false;
	double t0 = TurnedNow();
	_turned_texture.assign(static_cast<size_t>(tw) * th, 0);
	DrawPixelInfo tdpi{
		.dst_ptr = _turned_texture.data(),
		.left = tleft,
		.top = ttop,
		.width = twidth,
		.height = theight,
		.pitch = tw,
		.zoom = zoom
	};
	{
		AutoRestoreBackup dpi_backup(_cur_dpi, &tdpi);
		AutoRestoreBackup anim_backup(_screen_disable_anim, true);
		DrawSpriteViewport(ps.image, ps.pal, sx, sy, ps.sub);
		int child_idx = ps.first_child;
		while (child_idx >= 0) {
			const ChildScreenSpriteToDraw &cs = _vd.child_screen_sprites_to_draw[child_idx];
			child_idx = cs.next;
			if (cs.relative) {
				DrawSpriteViewport(cs.image, cs.pal, sleft + cs.x, stop + cs.y, cs.sub);
			} else {
				DrawSpriteViewport(cs.image, cs.pal, sx + cs.x, sy + cs.y, cs.sub);
			}
		}
	}
	_turned_times.texture += TurnedNow() - t0;
	tex = {tleft, ttop, tw, th};
	return true;
}

/**
 * Lay the texture of a standing thing (MakeTurnedTexture()) over faces turned with the view, far to near.
 * @param faces the faces in the frame of the thing (tile units from box_ox, box_oy; pixels up from box_oz), sorted far to near
 * @param centre_x,centre_y the middle of the thing in its frame, which the faces snap about
 * @param period the turn after which the front comes round again: 90 degrees for a square thing, 180 for a longer one, 360 for one that never snaps
 * @param fill whether gaps in the texture are filled from the pixels about them (DrawTexturedTriangle())
 */
static void DrawTurnedFaces(const DrawPixelInfo &frame, const ParentSpriteToDraw &ps, const std::vector<BoxFace> &faces, double centre_x, double centre_y, double period, const TurnedTexture &tex, bool fill = true)
{
	ZoomLevel zoom = frame.zoom;
	const int tleft = tex.left, ttop = tex.top, tw = tex.width, th = tex.height;

	/* The front turns by up to half the period either way; past that the boxes snap back about the middle. */
	double yaw = GetViewYaw();
	double wrapped = std::fmod(yaw + period / 2, period) - period / 2;
	int quarter = static_cast<int>(std::lround((yaw - wrapped) / 90.0)) & 3;
	auto stand = [&](double x, double y, double &wx, double &wy) {
		double dx = x - centre_x, dy = y - centre_y;
		switch (quarter) {
			default: wx = centre_x + dx; wy = centre_y + dy; break;
			case 1: wx = centre_x + dy; wy = centre_y - dx; break;
			case 2: wx = centre_x - dx; wy = centre_y - dy; break;
			case 3: wx = centre_x - dy; wy = centre_y + dx; break;
		}
	};

	double scale = 1.0 / (1 << to_underlying(zoom));
	auto vertex = [&](const BoxVertex &b) {
		TurnedVertex r;
		double fx, fy, cxv, cyv, wx, wy;
		stand(b.x, b.y, wx, wy);
		ProjectTurnedF(ps.box_ox + wx, ps.box_oy + wy, ps.box_oz + b.z, fx, fy);
		RemapCoordsF(ps.box_ox + b.tx, ps.box_oy + b.ty, ps.box_oz + b.tz, cxv, cyv);
		r.x = (fx - frame.left) * scale;
		r.y = (fy - frame.top) * scale;
		r.u = (cxv - tleft) * scale;
		r.v = (cyv - ttop) * scale;
		return r;
	};
	uint32_t *dst = static_cast<uint32_t *>(frame.dst_ptr);
	int fw = UnScaleByZoom(frame.width, zoom), fh = UnScaleByZoom(frame.height, zoom);
	const uint32_t *pixels = _turned_texture.data();
	double t1 = TurnedNow();
	for (const BoxFace &face : faces) {
		TurnedVertex a = vertex(face.v[0]);
		for (size_t i = 1; i + 1 < face.v.size(); i++) {
			DrawTexturedTriangle(dst, frame.pitch, fw, fh, pixels, tw, th, a, vertex(face.v[i]), vertex(face.v[i + 1]), fill);
		}
	}
	_turned_times.warp += TurnedNow() - t1;
}

static void DrawTurnedBoxes(const DrawPixelInfo &frame, const ParentSpriteToDraw &ps, const ThreeDPart *parts, uint count, bool cut, double cx0, double cx1, double cy0, double cy1, double centre_x, double centre_y, double period, bool fill = true)
{
	TurnedTexture tex;
	if (!MakeTurnedTexture(frame, ps, tex)) return;

	/* The faces of the boxes. */
	_box_faces.clear();
	for (uint i = 0; i < count; i++) AddBoxFaces(_box_faces, parts[i], cut, cx0, cx1, cy0, cy1, ps.box_ox, ps.box_oy);
	std::sort(_box_faces.begin(), _box_faces.end(), [](const BoxFace &a, const BoxFace &b) { return a.near < b.near; });
	DrawTurnedFaces(frame, ps, _box_faces, centre_x, centre_y, period, tex, fill);
}

/** A house sprite of the turned view as the boxes of its hand table (box_kind 1): this tile's share of them, from this tile's sprite. */
static void DrawTurnedHouseBox(const DrawPixelInfo &frame, const ParentSpriteToDraw &ps)
{
	const ThreeDHouse &house = *ps.box;
	bool cut = house.tiles_x > 1 || house.tiles_y > 1;
	double cx0 = ps.box_dx * TILE_SIZE - 0.5, cx1 = cx0 + TILE_SIZE + 1, cy0 = ps.box_dy * TILE_SIZE - 0.5, cy1 = cy0 + TILE_SIZE + 1;
	double period = (house.tiles_x == house.tiles_y) ? 90.0 : 180.0;
	DrawTurnedBoxes(frame, ps, GetThreeDHouseParts(&house), house.count, cut, cx0, cx1, cy0, cy1, house.tiles_x * TILE_SIZE / 2.0, house.tiles_y * TILE_SIZE / 2.0, period);
}

/**
 * A sprite of the turned view as one box with a flat top, its bounding box
 * (box_kind 2): a house without a hand table, an industry's building, an
 * object. The box was narrowed to the sprite's width when the sprite is
 * narrower than the bounding box's footprint on the screen (a tower).
 */
static void DrawTurnedGenericBox(const DrawPixelInfo &frame, const ParentSpriteToDraw &ps)
{
	ThreeDPart part{0.0f, static_cast<float>(ps.box_dx), 0.0f, static_cast<float>(ps.box_dy), static_cast<float>(ps.zmax - ps.zmin + 1), ThreeDRoof::Flat, 0.0f};
	double period = (ps.box_dx == ps.box_dy) ? 90.0 : 180.0;
	DrawTurnedBoxes(frame, ps, &part, 1, false, 0, 0, 0, 0, ps.box_dx / 2.0, ps.box_dy / 2.0, period);
}

/**
 * A low flat thing of a station tile as a slab, its whole bounding box (box_kind 6): a platform, a bench, a
 * heap. It turns for real and never snaps, since its top is all the sprite shows and a flat top turns at any
 * angle; so the platform's edge stays by the track. Its low walls take the sprite's bottom rows.
 */
static void DrawTurnedSlab(const DrawPixelInfo &frame, const ParentSpriteToDraw &ps)
{
	ThreeDPart part{0.0f, static_cast<float>(ps.box_dx), 0.0f, static_cast<float>(ps.box_dy), static_cast<float>(ps.zmax - ps.zmin + 1), ThreeDRoof::Flat, 0.0f};
	DrawTurnedBoxes(frame, ps, &part, 1, false, 0, 0, 0, 0, ps.box_dx / 2.0, ps.box_dy / 2.0, 360.0);
}

/*
 * A bridge in the turned view (the player: "the bridges fall apart when the view turns"): drawn as billboards, the
 * pieces of the deck over each tile stood north-up at their turned tiles and never joined up. So a bridge is built
 * from the sprites the game draws for it, as the cab view builds it (CabDrawBridgeMiddle()): over each tile the
 * deck as a slab at the bridge's height with the floor sprite's pixels on top (box_kind 8), the far and the near
 * railings as thin walls along the deck's edges with the sprites' pylons and cables on them (9), the pillars as
 * columns where the sprites' pixels stand (11), and each end's ramp as a deck rising from the ground to the
 * bridge, dressed in the ramp's sprite (10). None of them ever snaps: a bridge turns for real at every angle.
 */

/** How thick the deck of a bridge is drawn in the turned view, units of height below the track. */
static constexpr int TURNED_DECK = 2;

/** A box of a bridge over a tile, given along the bridge's axis and across it, in the tile's frame. */
static inline ThreeDPart BridgePart(Axis axis, double a0, double a1, double c0, double c1, double h)
{
	if (axis == Axis::X) return {static_cast<float>(a0), static_cast<float>(a1), static_cast<float>(c0), static_cast<float>(c1), static_cast<float>(h), ThreeDRoof::Flat, 0.0f};
	return {static_cast<float>(c0), static_cast<float>(c1), static_cast<float>(a0), static_cast<float>(a1), static_cast<float>(h), ThreeDRoof::Flat, 0.0f};
}

/** The deck of a bridge over a tile (box_kind 8): a slab across the tile at the bridge's height, its top the floor sprite's. */
static void DrawTurnedBridgeDeck(const DrawPixelInfo &frame, const ParentSpriteToDraw &ps)
{
	ThreeDPart part = BridgePart(static_cast<Axis>(ps.box_dx), 0, TILE_SIZE, 1, TILE_SIZE - 1, TURNED_DECK);
	DrawTurnedBoxes(frame, ps, &part, 1, false, 0, 0, 0, 0, TILE_SIZE / 2.0, TILE_SIZE / 2.0, 360.0);
}

/**
 * A railing of a bridge over a tile (box_kind 9): a thin wall along an edge of the deck, the sprite's pylons and
 * cables on it, the gaps between the cables left open (they filled into a sheet with the gaps filled).
 */
static void DrawTurnedBridgeRailing(const DrawPixelInfo &frame, const ParentSpriteToDraw &ps)
{
	int c_min = static_cast<int8_t>(ps.box_dy & 0xFF), c_ext = std::max(1, ps.box_dy >> 8);
	ThreeDPart part = BridgePart(static_cast<Axis>(ps.box_dx), 0, TILE_SIZE, c_min, c_min + c_ext, ps.zmax - ps.zmin + 1);
	DrawTurnedBoxes(frame, ps, &part, 1, false, 0, 0, 0, 0, TILE_SIZE / 2.0, TILE_SIZE / 2.0, 360.0, false);
}

/** A piece of a bridge's pillar (box_kind 11): a column a level high where the sprite's pixels stand (box_dx, box_dy: sixteenths of a tile unit). */
static void DrawTurnedBridgePillar(const DrawPixelInfo &frame, const ParentSpriteToDraw &ps)
{
	const double half = 1.25, fx = ps.box_dx / 16.0, fy = ps.box_dy / 16.0;
	ThreeDPart part{static_cast<float>(fx - half), static_cast<float>(fx + half), static_cast<float>(fy - half), static_cast<float>(fy + half), static_cast<float>(TILE_HEIGHT), ThreeDRoof::Flat, 0.0f};
	DrawTurnedBoxes(frame, ps, &part, 1, false, 0, 0, 0, 0, fx, fy, 90.0);
}

/**
 * The ramp of a bridge (box_kind 10): the deck rising from the ground at its outer end to the bridge's height at
 * the inner one (or level with it, on a slope or a foundation), dressed in the ramp's sprite, its sides down to
 * the ground, the far one in the near one's pixels, the end under the bridge in them too. The road or the rails
 * laid on the ramp come the same way, each its own sprite over the same slope.
 */
static void DrawTurnedBridgeRamp(const DrawPixelInfo &frame, const ParentSpriteToDraw &ps)
{
	TileIndex head = TileXY(ps.box_ox / TILE_SIZE, ps.box_oy / TILE_SIZE);
	if (!IsValidTile(head) || !IsBridgeTile(head)) return;
	const DiagDirection dir = GetTunnelBridgeDirection(head);
	const Axis axis = DiagDirToAxis(dir);
	const int bz = GetBridgePixelHeight(head);
	const int x0 = ps.box_ox, y0 = ps.box_oy;
	/* Along the axis: the end towards the bridge (inner) and the one towards the road or rail (outer). */
	const bool inner_low = dir == DiagDirection::NE || dir == DiagDirection::NW;
	const double a_in = inner_low ? 0 : TILE_SIZE, a_out = inner_low ? TILE_SIZE : 0;
	auto world = [&](double a, double c, double &wx, double &wy) {
		if (axis == Axis::X) {
			wx = a;
			wy = c;
		} else {
			wx = c;
			wy = a;
		}
	};
	double sx, sy;
	world(inner_low ? TILE_SIZE - 1 : 0, TILE_SIZE / 2, sx, sy);
	int z_out = GetSlopePixelZ(x0 + static_cast<int>(sx), y0 + static_cast<int>(sy), true);
	const double deck_out = (bz - z_out >= static_cast<int>(TILE_HEIGHT) / 2) ? bz - static_cast<int>(TILE_HEIGHT) : bz, deck_in = bz;
	auto ground = [&](double a, double c) {
		double wx, wy;
		world(std::clamp(a, 0.0, TILE_SIZE - 1.0), std::clamp(c, 0.0, TILE_SIZE - 1.0), wx, wy);
		return static_cast<double>(GetSlopePixelZ(x0 + static_cast<int>(wx), y0 + static_cast<int>(wy), false));
	};
	/* A corner: along a, across c, height z (absolute: the frame's box_oz is 0); dressed in the pixel of (a, tc, tz). */
	auto pt = [&](double a, double c, double z, double tc, double tz) {
		BoxVertex v;
		world(a, c, v.x, v.y);
		v.z = z;
		world(a, tc, v.tx, v.ty);
		v.tz = tz;
		return v;
	};
	TurnedTexture tex;
	if (!MakeTurnedTexture(frame, ps, tex)) return;
	std::vector<BoxFace> faces;
	auto add = [&](std::vector<BoxVertex> poly) {
		double cx = 0, cy = 0, cz = 0;
		for (const BoxVertex &v : poly) {
			cx += v.x;
			cy += v.y;
			cz += v.z;
		}
		double xr, yr;
		TurnWorldPoint(x0 + cx / poly.size(), y0 + cy / poly.size(), xr, yr);
		faces.push_back({std::move(poly), xr + yr + 2 * cz / poly.size()});
	};
	const double c_far = 1, c_near = TILE_SIZE - 1;
	/* The deck. */
	add({pt(a_out, c_far, deck_out, c_far, deck_out), pt(a_in, c_far, deck_in, c_far, deck_in), pt(a_in, c_near, deck_in, c_near, deck_in), pt(a_out, c_near, deck_out, c_near, deck_out)});
	/* Its sides down to the ground: the near one as the sprite shows it, the far one in the near one's pixels. */
	for (double c : {c_near, c_far}) {
		double g_out = std::min(ground(a_out, c), deck_out), g_in = std::min(ground(a_in, c), deck_in - TURNED_DECK);
		add({pt(a_out, c, g_out, c_near, g_out), pt(a_in, c, g_in, c_near, g_in), pt(a_in, c, deck_in, c_near, deck_in), pt(a_out, c, deck_out, c_near, deck_out)});
	}
	/* The end under the bridge, in the near side's pixels. */
	double g_in = std::min({ground(a_in, c_far), ground(a_in, c_near), deck_in - TURNED_DECK});
	add({pt(a_in, c_far, g_in, c_near, g_in), pt(a_in, c_near, g_in, c_near, g_in), pt(a_in, c_near, deck_in, c_near, deck_in), pt(a_in, c_far, deck_in, c_near, deck_in)});
	std::sort(faces.begin(), faces.end(), [](const BoxFace &a, const BoxFace &b) { return a.near < b.near; });
	DrawTurnedFaces(frame, ps, faces, TILE_SIZE / 2.0, TILE_SIZE / 2.0, 360.0, tex);
}

/**
 * The sprites of a bridge over a tile (DrawBridgeMiddle(), marked by MarkCabBridgeSprites()) given their boxes for
 * the turned view: the floor sprite is the deck, a slab over the whole tile, and its far railing besides, as an
 * entry of its own at the far edge so it sorts there against what stands on the deck; the near part is the near
 * railing; a surface laid on the deck (a rail type's deck, a reservation) a slab a step above it; a pillar piece a
 * column where its pixels stand; anything else of the bridge a box of its bounding box.
 */
static void TurnedBridgeOverTile(TileIndex tile, size_t from, size_t to)
{
	const Axis axis = GetBridgeAxis(tile);
	const int bz = GetBridgePixelHeight(GetSouthernBridgeEnd(tile));
	const int x0 = TileX(tile) * TILE_SIZE, y0 = TileY(tile) * TILE_SIZE;
	const int corner_x = RemapCoords(x0, y0, 0).x;
	std::vector<ParentSpriteToDraw> railings;
	for (size_t i = from; i < to && i < _vd.parent_sprites_to_draw.size(); i++) {
		ParentSpriteToDraw &ps = _vd.parent_sprites_to_draw[i];
		if (ps.image == SPR_EMPTY_BOUNDING_BOX || ps.box_kind != 0) continue;
		int ex = ps.xmax - ps.xmin + 1, ey = ps.ymax - ps.ymin + 1;
		int c_min = axis == Axis::X ? ps.ymin - y0 : ps.xmin - x0;
		int c_ext = axis == Axis::X ? ey : ex;
		ps.box = nullptr;
		if (ps.zmin < bz - TURNED_DECK) {
			/* A piece of a pillar. Its box tells only whether it is the near pillar (on the tile) or the far one (the game
			 * draws that one so many units back that its pixels land at the far edge); where along the bridge it stands is
			 * read off the sprite's middle, the pillar standing a few units in from the deck's edge. */
			const Sprite *spr = GetSprite(ps.image & SPRITE_MASK, SpriteType::Normal);
			double sx_c = ps.left + spr->width / 2.0 - corner_x;
			double c = c_min >= 0 ? TILE_SIZE - 3.0 : 3.0;
			double a = axis == Axis::X ? c - sx_c / (2.0 * ZOOM_BASE) : c + sx_c / (2.0 * ZOOM_BASE);
			a = std::clamp(a, 1.0, TILE_SIZE - 1.0);
			ps.box_kind = 11;
			ps.box_ox = x0;
			ps.box_oy = y0;
			ps.box_oz = ps.zmax + 1 - static_cast<int>(TILE_HEIGHT);
			ps.box_dx = static_cast<int16_t>(std::lround((axis == Axis::X ? a : c) * 16));
			ps.box_dy = static_cast<int16_t>(std::lround((axis == Axis::X ? c : a) * 16));
			continue;
		}
		if (ex >= static_cast<int>(TILE_SIZE) && ey >= static_cast<int>(TILE_SIZE)) {
			/* A surface laid on the deck: a slab a step above it, sorted under what runs on it. */
			ps.box_kind = 6;
			ps.box_ox = ps.xmin;
			ps.box_oy = ps.ymin;
			ps.box_oz = ps.zmin;
			ps.box_dx = static_cast<int16_t>(ex);
			ps.box_dy = static_cast<int16_t>(ey);
			ps.zmax = ps.zmin;
			ps.zmin--;
			continue;
		}
		if (c_ext <= 2) {
			int16_t across = static_cast<int16_t>((c_min & 0xFF) | (c_ext << 8));
			if (c_min >= static_cast<int>(TILE_SIZE) / 2) {
				/* The near railing. */
				ps.box_kind = 9;
				ps.box_ox = x0;
				ps.box_oy = y0;
				ps.box_oz = ps.zmin;
				ps.box_dx = static_cast<int16_t>(axis);
				ps.box_dy = across;
				continue;
			}
			/* The floor with the far railing: the railing as an entry of its own at the far edge, the deck here over the whole tile. */
			ParentSpriteToDraw railing = ps;
			railing.box_kind = 9;
			railing.box_ox = x0;
			railing.box_oy = y0;
			railing.box_oz = ps.zmin;
			railing.box_dx = static_cast<int16_t>(axis);
			railing.box_dy = across;
			railings.push_back(railing);
			ps.box_kind = 8;
			ps.box_ox = x0;
			ps.box_oy = y0;
			ps.box_oz = bz - TURNED_DECK;
			ps.box_dx = static_cast<int16_t>(axis);
			ps.box_dy = 0;
			ps.xmin = x0;
			ps.xmax = x0 + TILE_SIZE - 1;
			ps.ymin = y0;
			ps.ymax = y0 + TILE_SIZE - 1;
			ps.zmin = bz - TURNED_DECK;
			continue;
		}
		/* Anything else of the bridge: a box of its bounding box. */
		ps.box_kind = 2;
		ps.box_ox = ps.xmin;
		ps.box_oy = ps.ymin;
		ps.box_oz = ps.zmin;
		ps.box_dx = static_cast<int16_t>(std::max(2, ex));
		ps.box_dy = static_cast<int16_t>(std::max(2, ey));
	}
	for (const ParentSpriteToDraw &r : railings) _vd.parent_sprites_to_draw.push_back(r);
	if (!railings.empty()) _vd.last_child = LAST_CHILD_NONE;
}

/*
 * A depot in the turned view (the player: "the depots do not turn with the map"): its sprites stood north-up as
 * billboards. A depot draws its shed as one sprite and the frame of its door as another, each with a thin box
 * along a wall so a vehicle inside is drawn between them; neither box is the shed's. So the depot is one box over
 * its tile, the depot's kind telling its shape -- a train shed with a ridge along the track, a road depot and a
 * ship depot flat-topped -- as high as the sprite reaches above it, dressed in the shed's sprite with the door's
 * frame drawn into it (box_kind 12). Any set's depot, a road type's (CZTR's) or a rail type's, is drawn so: the
 * box comes from the tile, the height and the pictures from its sprites. It never snaps: the door stays on the
 * side the track or the road comes in by.
 */

/** The kind of a depot drawn turned (ParentSpriteToDraw::box_dx, low bits). */
enum TurnedDepotKind : uint8_t { TURNED_DEPOT_RAIL = 0, TURNED_DEPOT_ROAD = 1, TURNED_DEPOT_SHIP = 2 };

/** The footprint of a depot's box on its tile, world units: along the depot's axis and across it. */
static void TurnedDepotFootprint(TurnedDepotKind kind, double &a0, double &a1, double &c0, double &c1)
{
	switch (kind) {
		case TURNED_DEPOT_RAIL: a0 = 2; a1 = 15; c0 = 2; c1 = 14; break; // the walls of the original sheds
		case TURNED_DEPOT_SHIP: a0 = 0; a1 = TILE_SIZE; c0 = 0.5; c1 = TILE_SIZE - 0.5; break; // a shed over two tiles
		default: a0 = 0.5; a1 = TILE_SIZE - 0.5; c0 = 0.5; c1 = TILE_SIZE - 0.5; break;
	}
}

/** The depot's box in the tile's frame: along and across its axis to x and y. */
static ThreeDPart TurnedDepotPart(TurnedDepotKind kind, Axis axis, int height)
{
	double a0, a1, c0, c1;
	TurnedDepotFootprint(kind, a0, a1, c0, c1);
	ThreeDPart part = BridgePart(axis, a0, a1, c0, c1, height);
	if (kind == TURNED_DEPOT_RAIL) {
		/* A ridge along the track: about two fifths of the height the roof, the rest the walls. */
		float rise = static_cast<float>(height * 2 / 5);
		part.h = static_cast<float>(height) - rise;
		part.rise = rise;
		part.roof = axis == Axis::X ? ThreeDRoof::GableX : ThreeDRoof::GableY;
	}
	return part;
}

/** A depot (box_kind 12): one box over its tile, its sprites laid over it (TurnedDepot()). */
static void DrawTurnedDepot(const DrawPixelInfo &frame, const ParentSpriteToDraw &ps)
{
	const TurnedDepotKind kind = static_cast<TurnedDepotKind>(ps.box_dx & 0x0F);
	const Axis axis = static_cast<Axis>((ps.box_dx >> 4) & 1);
	ThreeDPart part = TurnedDepotPart(kind, axis, ps.box_dy);
	DrawTurnedBoxes(frame, ps, &part, 1, false, 0, 0, 0, 0, (part.x0 + part.x1) / 2.0, (part.y0 + part.y1) / 2.0, 360.0);
}

/** The sprites of a depot on its tile given their box for the turned view: the tallest the shed, the rest drawn into it. */
static void TurnedDepot(TileIndex tile, size_t first_parent)
{
	TurnedDepotKind kind;
	Axis axis;
	if (IsRailDepotTile(tile)) {
		kind = TURNED_DEPOT_RAIL;
		axis = DiagDirToAxis(GetRailDepotDirection(tile));
	} else if (IsRoadDepotTile(tile)) {
		kind = TURNED_DEPOT_ROAD;
		axis = DiagDirToAxis(GetRoadDepotDirection(tile));
	} else if (IsShipDepotTile(tile)) {
		kind = TURNED_DEPOT_SHIP;
		axis = GetShipDepotAxis(tile);
	} else {
		return;
	}
	/* The shed: the sprite reaching highest. */
	size_t body = SIZE_MAX;
	for (size_t i = first_parent; i < _vd.parent_sprites_to_draw.size(); i++) {
		const ParentSpriteToDraw &ps = _vd.parent_sprites_to_draw[i];
		if (ps.image == SPR_EMPTY_BOUNDING_BOX || ps.box_kind != 0) continue;
		if (body == SIZE_MAX || ps.top < _vd.parent_sprites_to_draw[body].top) body = i;
	}
	if (body == SIZE_MAX) return;

	/* The rest -- the door's frame -- drawn into the shed's texture, where the game draws them. */
	for (size_t i = first_parent; i < _vd.parent_sprites_to_draw.size(); i++) {
		ParentSpriteToDraw &ps = _vd.parent_sprites_to_draw[i];
		if (i == body || ps.image == SPR_EMPTY_BOUNDING_BOX || ps.box_kind != 0) continue;
		ParentSpriteToDraw &shed = _vd.parent_sprites_to_draw[body];
		int idx = static_cast<int>(_vd.child_screen_sprites_to_draw.size());
		_vd.child_screen_sprites_to_draw.push_back({ps.image, ps.pal, ps.sub, ps.x - shed.x, ps.y - shed.y, false, -1});
		if (shed.first_child < 0) {
			shed.first_child = idx;
		} else {
			int last = shed.first_child;
			while (_vd.child_screen_sprites_to_draw[last].next >= 0) last = _vd.child_screen_sprites_to_draw[last].next;
			_vd.child_screen_sprites_to_draw[last].next = idx;
		}
		/* Its own children too, after it. */
		for (int c = ps.first_child; c >= 0;) {
			ChildScreenSpriteToDraw cs = _vd.child_screen_sprites_to_draw[c];
			c = cs.next;
			if (!cs.relative) {
				cs.x += ps.x - shed.x;
				cs.y += ps.y - shed.y;
			} else {
				cs.x += ps.left - shed.left;
				cs.y += ps.top - shed.top;
			}
			cs.next = -1;
			int cidx = static_cast<int>(_vd.child_screen_sprites_to_draw.size());
			_vd.child_screen_sprites_to_draw.push_back(cs);
			int last = shed.first_child;
			while (_vd.child_screen_sprites_to_draw[last].next >= 0) last = _vd.child_screen_sprites_to_draw[last].next;
			_vd.child_screen_sprites_to_draw[last].next = cidx;
		}
		ps.box_kind = 4;
	}

	ParentSpriteToDraw &shed = _vd.parent_sprites_to_draw[body];
	const int x0 = TileX(tile) * TILE_SIZE, y0 = TileY(tile) * TILE_SIZE, z = shed.zmin;
	double a0, a1, c0, c1;
	TurnedDepotFootprint(kind, a0, a1, c0, c1);
	double fx0 = axis == Axis::X ? a0 : c0, fy0 = axis == Axis::X ? c0 : a0;
	double fx1 = axis == Axis::X ? a1 : c1, fy1 = axis == Axis::X ? c1 : a1;
	/* How high: the top of the sprite over the north corner of the box, where the top of a box is highest on the
	 * screen; a ship depot's far half shows only a part of the shed, so it is as high as a ship depot is. */
	int height = static_cast<int>(std::lround(x0 + fx0 + y0 + fy0 - static_cast<double>(shed.top) / ZOOM_BASE)) - z;
	if (kind == TURNED_DEPOT_SHIP) height = std::max<int>(height, 4 * TILE_HEIGHT);
	height = Clamp(height, 2, 8 * static_cast<int>(TILE_HEIGHT));

	shed.box_kind = 12;
	shed.box = nullptr;
	shed.box_ox = x0;
	shed.box_oy = y0;
	shed.box_oz = z;
	shed.box_dx = static_cast<int16_t>(kind | (to_underlying(axis) << 4));
	shed.box_dy = static_cast<int16_t>(height);
	shed.xmin = x0 + static_cast<int>(fx0);
	shed.xmax = x0 + static_cast<int>(std::ceil(fx1)) - 1;
	shed.ymin = y0 + static_cast<int>(fy0);
	shed.ymax = y0 + static_cast<int>(std::ceil(fy1)) - 1;
	shed.zmin = z;
	shed.zmax = z + height - 1;
}

/** The sprites of a bridge's end on its tile: the ramp and whatever lies on it (a road, rails) as the rising deck (box_kind 10). */
static void TurnedBridgeHead(TileIndex head, size_t first_parent)
{
	for (size_t i = first_parent; i < _vd.parent_sprites_to_draw.size(); i++) {
		ParentSpriteToDraw &ps = _vd.parent_sprites_to_draw[i];
		if (ps.image == SPR_EMPTY_BOUNDING_BOX || ps.box_kind != 0) continue;
		if (ps.xmax - ps.xmin + 1 < static_cast<int>(TILE_SIZE) || ps.ymax - ps.ymin + 1 < static_cast<int>(TILE_SIZE)) continue;
		ps.box_kind = 10;
		ps.box = nullptr;
		ps.box_ox = TileX(head) * TILE_SIZE;
		ps.box_oy = TileY(head) * TILE_SIZE;
		ps.box_oz = 0;
		ps.box_dx = 0;
		ps.box_dy = 0;
	}
}

/** The vehicles of the block being drawn as their likeness (box_kind 5, AddTurnedVehiclePicture()); ParentSpriteToDraw::box_dx is the index. */
static std::vector<const ThreeDModelPicture *> _turned_vehicle_pictures;
/** Whether the turned renderer is collecting what to draw (ViewportDoDrawTurned()): the vehicles then come as their likeness. */
static bool _drawing_turned = false;
/** The sprites of the bridges over the tiles collected (MarkCabBridgeSprites()): the tile, and the first and the end of their parent sprites. */
struct CabBridgeSprites {
	TileIndex tile;
	size_t from, to;
};
static std::vector<CabBridgeSprites> _cab_bridge_sprites;

bool ViewportDrawingTurned()
{
	return _drawing_turned;
}

static bool _north_house_objects_now = false;
/**
 * Whether the view facing north being collected draws the houses that are 3D objects as objects
 * (gui.three_d_north_houses): the figures standing among them then come as their likeness too.
 */
bool ViewportDrawingNorthObjects()
{
	return _north_house_objects_now;
}

/**
 * A vehicle of the turned view as its likeness (three_d_vehicle.cpp): the picture at the angle of the view,
 * hung from a world point, sorted by the box of the vehicle's parts.
 * @param pic the picture, its left and top from the point it hangs from
 * @param ax,ay,az the world point it hangs from: the middle of the vehicle's parts
 * @param xmin,ymin,zmin,xmax,ymax,zmax the box of the parts, world units
 */
void AddTurnedVehiclePicture(const ThreeDModelPicture *pic, int ax, int ay, int az, int xmin, int ymin, int zmin, int xmax, int ymax, int zmax)
{
	if (pic == nullptr || pic->width <= 0) return;
	/* Not in the block at all? The picture about its point, in the units of the collecting dpi (16x). */
	Point pt = RemapCoords(ax, ay, az);
	int scale = 1 << to_underlying(_vd.dpi.zoom);
	int left = pt.x + pic->left * scale, top = pt.y + pic->top * scale;
	int right = left + pic->width * scale, bottom = top + pic->height * scale;
	if (left >= _vd.dpi.left + _vd.dpi.width || right <= _vd.dpi.left || top >= _vd.dpi.top + _vd.dpi.height || bottom <= _vd.dpi.top) return;
	if (_turned_vehicle_pictures.size() >= 32000) return;

	ParentSpriteToDraw &ps = _vd.parent_sprites_to_draw.emplace_back();
	ps.x = pt.x;
	ps.y = pt.y;
	ps.left = left;
	ps.top = top;
	ps.image = SPR_EMPTY_BOUNDING_BOX;
	ps.pal = PAL_NONE;
	ps.sub = nullptr;
	ps.xmin = xmin;
	ps.xmax = xmax;
	ps.ymin = ymin;
	ps.ymax = ymax;
	ps.zmin = zmin;
	ps.zmax = zmax;
	ps.first_child = LAST_CHILD_NONE;
	ps.order = 0;
	ps.ax = ax;
	ps.ay = ay;
	ps.az = az;
	ps.box = nullptr;
	ps.box_ox = ax;
	ps.box_oy = ay;
	ps.box_oz = az;
	ps.box_dx = static_cast<int16_t>(_turned_vehicle_pictures.size());
	ps.box_dy = 0;
	ps.box_kind = 5;
	_turned_vehicle_pictures.push_back(pic);
	_vd.last_child = LAST_CHILD_NONE;
}

/**
 * A parent sprite with no picture of its own for a whole articulated road vehicle (vehicle.cpp, DrawRoadVehicleWhole()):
 * its box is the hull of the boxes of all the parts, so other vehicles and buildings sort against the whole
 * consist; the parts' pictures come as its children, far to near. The screen rectangle given is the union of the
 * parts' pictures: the entry is clipped by it, not by the box, since the pictures reach past the box (that is why
 * the consist is drawn this way).
 * @param xmin,ymin,zmin,xmax,ymax,zmax the hull, world units (inclusive)
 * @param ax,ay,az the point of the consist (its first part's)
 * @param anchor where that point is on the screen (RemapCoords() plus the picture's shift): the children hang from it
 * @param screen the union of the parts' pictures on the screen
 * @return whether the entry was added (within the block being drawn): the children may follow
 */
bool AddVehicleHullToDraw(int xmin, int ymin, int zmin, int xmax, int ymax, int zmax, int ax, int ay, int az, Point anchor, const Rect &screen)
{
	_vd.last_child = LAST_CHILD_NONE;
	if (screen.left >= _vd.dpi.left + _vd.dpi.width || screen.right <= _vd.dpi.left || screen.top >= _vd.dpi.top + _vd.dpi.height || screen.bottom <= _vd.dpi.top) return false;
	ParentSpriteToDraw &ps = _vd.parent_sprites_to_draw.emplace_back();
	ps.x = anchor.x;
	ps.y = anchor.y;
	ps.left = screen.left;
	ps.top = screen.top;
	ps.image = SPR_EMPTY_BOUNDING_BOX;
	ps.pal = PAL_NONE;
	ps.sub = nullptr;
	ps.xmin = xmin;
	ps.xmax = xmax;
	ps.ymin = ymin;
	ps.ymax = ymax;
	ps.zmin = zmin;
	ps.zmax = zmax;
	ps.first_child = LAST_CHILD_NONE;
	ps.order = 0;
	ps.ax = ax;
	ps.ay = ay;
	ps.az = az;
	ps.box = nullptr;
	ps.box_ox = ax;
	ps.box_oy = ay;
	ps.box_oz = az;
	ps.box_dx = 0;
	ps.box_dy = 0;
	ps.box_kind = 0;
	_vd.last_child = LAST_CHILD_PARENT;
	return true;
}

/**
 * Make the pixels of a rectangle of a block of the view facing north plain colours, the palette's no more
 * (Blitter::RealizeAnimation()), before a 3D object or a picture is drawn straight into them: else with the OpenGL
 * driver (40bpp-anim) the grass or the original house under it shows through, and is not darkened at night (the
 * player, #287). The turned view clears its blocks of the palette before it draws them.
 * @param frame the block
 * @param x0,y0,x1,y1 the rectangle, pixels of the block, the end out
 */
static void RealizeNorthRect(const DrawPixelInfo &frame, int x0, int y0, int x1, int y1)
{
	const int fw = UnScaleByZoom(frame.width, frame.zoom), fh = UnScaleByZoom(frame.height, frame.zoom);
	x0 = std::max(x0, 0);
	y0 = std::max(y0, 0);
	x1 = std::min(x1, fw);
	y1 = std::min(y1, fh);
	if (x0 >= x1 || y0 >= y1) return;
	BlitterFactory::GetCurrentBlitter()->RealizeAnimation(static_cast<uint32_t *>(frame.dst_ptr) + static_cast<size_t>(y0) * frame.pitch + x0, x1 - x0, y1 - y0);
}

/**
 * Draw a picture of the turned view hung from a world point (a vehicle's likeness, a figure); at night the pixels of
 * its lit glass kept bright (ThreeDModelPicture::lit, DrawNight()).
 * @param north drawn on a view facing north (a figure, a vehicle with lights at night): the point projected as the map
 */
static void BlitTurnedPicture(const DrawPixelInfo &frame, const ThreeDModelPicture &picture, double wx, double wy, double wz, bool north = false)
{
	const ThreeDModelPicture *pic = &picture;
	double vx, vy;
	if (north) {
		RemapCoordsF(wx, wy, wz, vx, vy);
	} else {
		ProjectTurnedF(wx, wy, wz, vx, vy);
	}
	double scale = 1.0 / (1 << to_underlying(frame.zoom));
	int ox = static_cast<int>(std::lround((vx - frame.left) * scale)) + pic->left;
	int oy = static_cast<int>(std::lround((vy - frame.top) * scale)) + pic->top;
	int fw = UnScaleByZoom(frame.width, frame.zoom), fh = UnScaleByZoom(frame.height, frame.zoom);
	int x0 = std::max(0, -ox), x1 = std::min(pic->width, fw - ox);
	int y0 = std::max(0, -oy), y1 = std::min(pic->height, fh - oy);
	if (north) RealizeNorthRect(frame, ox + x0, oy + y0, ox + x1, oy + y1);
	uint32_t *dst = static_cast<uint32_t *>(frame.dst_ptr);
	for (int y = y0; y < y1; y++) {
		const uint32_t *src = &pic->pixels[static_cast<size_t>(y) * pic->width];
		uint32_t *row = dst + static_cast<size_t>(oy + y) * frame.pitch + ox;
		for (int x = x0; x < x1; x++) {
			Colour s(src[x]);
			if (s.a == 0) continue;
			if (s.a == 255) {
				row[x] = Colour(s.r, s.g, s.b).data;
			} else {
				Colour d(row[x]);
				uint a = s.a, na = 255 - a;
				row[x] = Colour(static_cast<uint8_t>((s.r * a + d.r * na + 127) / 255), static_cast<uint8_t>((s.g * a + d.g * na + 127) / 255), static_cast<uint8_t>((s.b * a + d.b * na + 127) / 255)).data;
			}
		}
	}
	if (_night_windows_now) {
		for (uint32_t i : pic->lit) {
			const int x = static_cast<int>(i % pic->width), y = static_cast<int>(i / pic->width);
			if (x < x0 || x >= x1 || y < y0 || y >= y1) continue;
			uint32_t *at = dst + static_cast<size_t>(oy + y) * frame.pitch + ox + x;
			_night_lit.emplace_back(at, *at);
		}
	}
}

/** Draw a vehicle's likeness (box_kind 5): the picture hung from its point, turned (or facing north, north). */
static void DrawTurnedVehicle(const DrawPixelInfo &frame, const ParentSpriteToDraw &ps, bool north)
{
	if (ps.box_dx < 0 || static_cast<size_t>(ps.box_dx) >= _turned_vehicle_pictures.size()) return;
	BlitTurnedPicture(frame, *_turned_vehicle_pictures[ps.box_dx], ps.box_ox, ps.box_oy, ps.box_oz, north);
}

/**
 * A building of the turned view as a real 3D object (box_kind 7, three_d_objects.h), at the north corner of its north
 * tile on the ground; or a part of a field (the soil under the plants, the field road, the girls by it).
 */
struct TurnedObject {
	const ThreeDObject *obj;   ///< nullptr: only the figures of more
	TileIndex origin;
	int z;
	const ThreeDObject *ground = nullptr;     ///< drawn first under it, whole (the soil of a field under its plants)
	std::span<const ThreeDObjectFigure> more; ///< figures on it besides its own (the girl at work, the girls by the road)
	int sort_z = 0;                           ///< added to the bottom of its box (the field road flat and first, under the cars)

	TurnedObject(const ThreeDObject *obj, TileIndex origin, int z, const ThreeDObject *ground = nullptr, std::span<const ThreeDObjectFigure> more = {}, int sort_z = 0) :
			obj(obj), origin(origin), z(z), ground(ground), more(more), sort_z(sort_z) {}
};
/** The objects of the block being drawn; ParentSpriteToDraw::box_dx is the index. */
static std::vector<TurnedObject> _turned_objects;
/** Whether a 3D object's triangles turned away from the view are left out (the rig: 'test3d zadni 0' draws them all). */
bool _turned_cull = true;

/** How a 3D object is laid on a picture: turned about a pivot as the view is, projected as the map, scaled to the zoom's pixels. */
struct ObjectLay {
	double c = 1, s = 0;       ///< the turn
	double px = 0, py = 0;     ///< about this world point
	double left = 0, top = 0;  ///< the picture's corner, virtual screen coordinates
	double scale = 1;          ///< pixels of the picture to a virtual one
	float yaw = 0;             ///< the angle of the view, for the pictures of the figures
	ZoomLevel zoom = ZoomLevel::Min;

	void Turn(double x, double y, double &xr, double &yr) const
	{
		double dx = x - this->px, dy = y - this->py;
		xr = this->px + this->c * dx - this->s * dy;
		yr = this->py + this->s * dx + this->c * dy;
	}
	void Project(double x, double y, double z, double &sx, double &sy) const
	{
		double xr, yr;
		this->Turn(x, y, xr, yr);
		sx = ((yr - xr) * 2 * ZOOM_BASE - this->left) * this->scale;
		sy = ((yr + xr - z) * ZOOM_BASE - this->top) * this->scale;
	}
};

/** The lay of the turned map for a block being drawn. */
static ObjectLay MapLay(const DrawPixelInfo &frame)
{
	ObjectLay lay;
	GetViewTurn(lay.c, lay.s, lay.px, lay.py);
	lay.left = frame.left;
	lay.top = frame.top;
	lay.scale = 1.0 / (1 << to_underlying(frame.zoom));
	lay.yaw = GetViewYaw();
	lay.zoom = frame.zoom;
	return lay;
}

/**
 * A building drawn once for the turned view as it is now, its triangles only (GetTurnedObjectPicture()): drawn
 * again only copied, its figures and its night over it (the technologies of OpenMW, the player, 16. 10.: what does
 * not move, it makes ready once and keeps).
 */
struct TurnedObjectPicture {
	std::vector<uint32_t> pixels; ///< 0 where it is not
	std::vector<float> depth;     ///< how near each pixel is (TurnedVertex::d), -infinity where it is not
	int width = 0, height = 0;
	double left = 0, top = 0;     ///< its top left corner, virtual coordinates of the view
	uint64_t used = 0;            ///< when it was drawn last (_turned_object_clock)
};

/** The shape of an object at the level of detail of the turned view's zoom (ThreeDShapeLevel()): a metre up is the most pixels of the picture. */
static ThreeDObjectShapeView TurnedObjectShape(const ThreeDObject *o, const ObjectLay &lay)
{
	return o->GetShape(ThreeDShapeLevel(ZOOM_BASE * OBJECT_HEIGHT_PER_METRE * lay.scale, 0.75));
}

/**
 * Draw a 3D object into pixels: its triangles turned with the view, far first, each filled with its texture at the
 * level of detail of the zoom -- the texture's 1024 pixels for a tile's width at the closest zoom, halved with every
 * zoom level out -- and the figures and plants standing on it among them by the middle of their height.
 * @param o the object, nullptr for only the figures of more
 * @param ox,oy,oz its north corner, world units
 * @param more figures on it besides its own
 * @param picture the object's triangles already drawn (GetTurnedObjectPicture()): copied, its figures and its night
 *                over them; nullptr to draw them
 * @param triangles_only only its triangles, their depth to depth_out (a picture of it, GetTurnedObjectPicture())
 * @param depth_out the depth of each pixel of the frame, -infinity where it is not
 */
static void DrawObjectInto(uint32_t *dst, int pitch, int fw, int fh, const ObjectLay &lay, const ThreeDObject *o, double ox, double oy, double oz, uint32_t paint, std::span<const ThreeDObjectFigure> more, const TurnedObjectPicture *picture = nullptr, bool triangles_only = false, std::vector<float> *depth_out = nullptr)
{
	static std::vector<TurnedVertex> verts;
	static std::vector<std::pair<double, size_t>> order;
	/* Its textures: its own (painted) and its add-ons' (ThreeDObject::GetTextureOf()), by the triangles' texture. */
	static std::vector<const ThreeDObjectTexture *> texs;
	const double t_start = TurnedNow();
	_turned_times.models_drawn++;
	order.clear();
	verts.clear();
	texs.clear();
	const ThreeDObjectTexture *tex = nullptr;
	/* Its shape as simple as the zoom allows; none of it when it is drawn already. */
	const ThreeDObjectShapeView shape = o != nullptr && picture == nullptr ? TurnedObjectShape(o, lay) : ThreeDObjectShapeView{};
	if (o != nullptr && picture != nullptr) tex = o->GetTexture(to_underlying(lay.zoom), paint);
	if (o != nullptr && !shape.tri.empty()) {
		tex = o->GetTexture(to_underlying(lay.zoom), paint);
		texs.push_back(tex);
		/* Its add-ons' each in its colour at this house's place (ThreeDObject::AddonPaintAt()). */
		const TileIndex at = TileXY(std::clamp<int>(static_cast<int>(ox) / TILE_SIZE, 0, Map::MaxX()), std::clamp<int>(static_cast<int>(oy) / TILE_SIZE, 0, Map::MaxY()));
		for (uint8_t k = 1; k < o->Textures(); k++) texs.push_back(o->GetTextureOf(k, to_underlying(lay.zoom), o->AddonPaintAt(at, k)));
		const bool lit = shape.light.size() == shape.pos.size() / 3;
		const size_t corners = shape.pos.size() / 3;
		verts.resize(corners);
		for (size_t i = 0; i < corners; i++) {
			double sx, sy, xr, yr;
			lay.Project(ox + shape.pos[i * 3], oy + shape.pos[i * 3 + 1], oz + shape.pos[i * 3 + 2], sx, sy);
			lay.Turn(ox + shape.pos[i * 3], oy + shape.pos[i * 3 + 1], xr, yr);
			/* Its place in its own texture: the building's or an add-on's. */
			const uint8_t k = shape.CornerTexture(i);
			const ThreeDObjectTexture *ct = k < texs.size() ? texs[k] : tex;
			verts[i] = {sx, sy, shape.uv[i * 2] * ct->w, shape.uv[i * 2 + 1] * ct->h, lit ? static_cast<double>(shape.light[i]) : 256.0, xr + yr + 2 * shape.pos[i * 3 + 2]};
		}
		/* Far first: by the middle of each triangle, as the faces of the boxes. */
		for (size_t t = 0; t + 2 < shape.tri.size(); t += 3) {
			const TurnedVertex &a = verts[shape.tri[t]], &b = verts[shape.tri[t + 1]], &c = verts[shape.tri[t + 2]];
			/* Not in the block being drawn: nothing to do (an object is drawn again for each block it is in). */
			if (std::max({a.x, b.x, c.x}) < 0 || std::min({a.x, b.x, c.x}) >= fw || std::max({a.y, b.y, c.y}) < 0 || std::min({a.y, b.y, c.y}) >= fh) continue;
			/* A triangle turned away from the view is a back wall: left out, else drawn by the middle it could come
			 * through a front one. On the screen (y down) a triangle seen from outside goes round clockwise. */
			const int8_t facing = _turned_cull && t / 3 < shape.facing.size() ? shape.facing[t / 3] : 0;
			if (facing != 0) {
				double area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
				if (area * facing >= 0) continue;
			}
			double sx = 0, sy = 0, sz = 0;
			for (int k = 0; k < 3; k++) {
				sx += shape.pos[shape.tri[t + k] * 3];
				sy += shape.pos[shape.tri[t + k] * 3 + 1];
				sz += shape.pos[shape.tri[t + k] * 3 + 2];
			}
			double xr, yr;
			lay.Turn(ox + sx / 3, oy + sy / 3, xr, yr);
			order.emplace_back(xr + yr + 2 * sz / 3, t);
		}
	}
	/* The figures standing on it (College Girl by the parasols of the hotel's roof) and its plants: among the
	 * triangles by the middle of their height, so what stands in front of them hides them. */
	struct OnIt {
		const ThreeDModelPicture *pic;
		double x, y, z;
		float near;
	};
	static std::vector<OnIt> on_it;
	on_it.clear();
	const size_t tris = shape.tri.size();
	const TileIndex origin = TileXY(std::clamp<int>(static_cast<int>(ox) / TILE_SIZE, 0, Map::MaxX()), std::clamp<int>(static_cast<int>(oy) / TILE_SIZE, 0, Map::MaxY()));
	auto stand = [&](const ThreeDObjectFigure &f, bool plant) {
		double wx = ox + f.at[0] * OBJECT_UNITS_PER_METRE, wy = oy + f.at[1] * OBJECT_UNITS_PER_METRE, wz = oz + f.at[2] * OBJECT_HEIGHT_PER_METRE;
		int height = 0;
		const ThreeDModelPicture *pic;
		if (plant) {
			pic = GetThreeDPlantPicture(f.figure, f.scale, f.stretch, f.facing, lay.zoom, &height, lay.yaw);
		} else {
			TileIndex on = TileXY(std::clamp<int>(static_cast<int>(wx) / TILE_SIZE, 0, Map::MaxX()), std::clamp<int>(static_cast<int>(wy) / TILE_SIZE, 0, Map::MaxY()));
			pic = GetThreeDFigurePicture(f.figure, f.scale, f.facing, lay.zoom, on, TileX(origin) * 7 + TileY(origin) * 13 + static_cast<uint32_t>(on_it.size()), &height, -1, lay.yaw, static_cast<int>(std::floor(wz)), static_cast<int>(std::lround(wx)), static_cast<int>(std::lround(wy)));
		}
		if (pic == nullptr) return;
		double xr, yr;
		lay.Turn(wx, wy, xr, yr);
		const double near = xr + yr + 2 * (wz - oz + height / 2.0);
		order.emplace_back(near, SIZE_MAX - on_it.size());
		on_it.push_back({pic, wx, wy, wz, static_cast<float>(near)});
	};
	if (!triangles_only) {
		if (o != nullptr) {
			for (const ThreeDObjectFigure &f : o->figures) stand(f, false);
			for (const ThreeDObjectFigure &f : o->plants) stand(f, true);
		}
		for (const ThreeDObjectFigure &f : more) stand(f, false);
	}
	std::sort(order.begin(), order.end());
	/* The depth of its pixels (#45 of the player's list: the flags of the ÚV KSČ building through its front, the
	 * field road through the grass under it): over the rectangle it covers, nothing there yet. */
	static std::vector<float> zb;
	if (zb.size() < static_cast<size_t>(fw) * fh) zb.resize(static_cast<size_t>(fw) * fh);
	int zx0 = fw, zy0 = fh, zx1 = -1, zy1 = -1;
	auto cover = [&](double x0, double y0, double x1, double y1) {
		zx0 = std::min(zx0, std::max(0, static_cast<int>(std::floor(x0)) - 1));
		zy0 = std::min(zy0, std::max(0, static_cast<int>(std::floor(y0)) - 1));
		zx1 = std::max(zx1, std::min(fw - 1, static_cast<int>(std::ceil(x1)) + 1));
		zy1 = std::max(zy1, std::min(fh - 1, static_cast<int>(std::ceil(y1)) + 1));
	};
	for (const auto &[near, t] : order) {
		if (t >= tris) {
			const OnIt &f = on_it[SIZE_MAX - t];
			double sx, sy;
			lay.Project(f.x, f.y, f.z, sx, sy);
			cover(sx + f.pic->left, sy + f.pic->top, sx + f.pic->left + f.pic->width, sy + f.pic->top + f.pic->height);
		} else {
			const TurnedVertex &a = verts[shape.tri[t]], &b = verts[shape.tri[t + 1]], &c = verts[shape.tri[t + 2]];
			cover(std::min({a.x, b.x, c.x}), std::min({a.y, b.y, c.y}), std::max({a.x, b.x, c.x}), std::max({a.y, b.y, c.y}));
		}
	}
	/* Its picture: where it is in the frame, whole pixels (the frame's corner is on a whole pixel of its zoom). */
	int pic_x = 0, pic_y = 0;
	if (picture != nullptr) {
		pic_x = static_cast<int>(std::lround((picture->left - lay.left) * lay.scale));
		pic_y = static_cast<int>(std::lround((picture->top - lay.top) * lay.scale));
		cover(pic_x, pic_y, pic_x + picture->width - 1, pic_y + picture->height - 1);
	}
	if (depth_out != nullptr) depth_out->assign(static_cast<size_t>(fw) * fh, -std::numeric_limits<float>::infinity());
	const double t_projected = TurnedNow();
	_turned_times.models_project += t_projected - t_start;
	if (zx1 < zx0 || zy1 < zy0) return;
	/* Its triangles and figures in their order, a stripe of its rows a core (the technologies of OpenMW, the player,
	 * 16. 10.): each stripe all of them in the same order, only its rows -- the picture as drawn one after the other. */
	auto draw_rows = [&](int row_lo, int row_hi) {
	for (int y = row_lo; y < row_hi; y++) std::fill_n(&zb[static_cast<size_t>(y) * fw + zx0], zx1 - zx0 + 1, -std::numeric_limits<float>::infinity());
	/* Its picture first, its depth with it: the figures go behind its walls as among its triangles. */
	if (picture != nullptr) {
		const int x0 = std::max(0, pic_x), x1 = std::min(fw, pic_x + picture->width);
		for (int y = std::max(row_lo, pic_y); y < std::min(row_hi, pic_y + picture->height); y++) {
			const size_t from = static_cast<size_t>(y - pic_y) * picture->width - pic_x;
			uint32_t *row = dst + static_cast<size_t>(y) * pitch;
			float *zrow = &zb[static_cast<size_t>(y) * fw];
			for (int x = x0; x < x1; x++) {
				const uint32_t p = picture->pixels[from + x];
				if (p == 0) continue;
				row[x] = p;
				zrow[x] = picture->depth[from + x];
			}
		}
	}
	for (const auto &[near, t] : order) {
		if (t >= tris) {
			const OnIt &f = on_it[SIZE_MAX - t];
			double sx, sy;
			lay.Project(f.x, f.y, f.z, sx, sy);
			const ThreeDModelPicture &pic = *f.pic;
			int px = static_cast<int>(std::lround(sx)) + pic.left, py = static_cast<int>(std::lround(sy)) + pic.top;
			int x0 = std::max(0, -px), x1 = std::min(pic.width, fw - px);
			int y0 = std::max(std::max(0, -py), row_lo - py), y1 = std::min(std::min(pic.height, fh - py), row_hi - py);
			for (int y = y0; y < y1; y++) {
				const uint32_t *src = &pic.pixels[static_cast<size_t>(y) * pic.width];
				uint32_t *row = dst + static_cast<size_t>(py + y) * pitch + px;
				float *zrow = &zb[static_cast<size_t>(py + y) * fw + px];
				for (int x = x0; x < x1; x++) {
					Colour c(src[x]);
					if (c.a == 0) continue;
					/* The figure as a whole as near as its middle: behind a wall nearer than that, hidden. */
					if (f.near < zrow[x] - 0.05f) continue;
					if (c.a == 255) {
						row[x] = Colour(c.r, c.g, c.b).data;
					} else {
						Colour d(row[x]);
						uint a = c.a, na = 255 - a;
						row[x] = Colour(static_cast<uint8_t>((c.r * a + d.r * na + 127) / 255), static_cast<uint8_t>((c.g * a + d.g * na + 127) / 255), static_cast<uint8_t>((c.b * a + d.b * na + 127) / 255)).data;
					}
					if (c.a >= 128) zrow[x] = f.near;
				}
			}
			continue;
		}
		const uint8_t k = shape.TriangleTexture(t);
		const ThreeDObjectTexture *tt = k < texs.size() ? texs[k] : tex;
		DrawTexturedTriangle(dst, pitch, fw, fh, tt->px.data(), tt->w, tt->h, verts[shape.tri[t]], verts[shape.tri[t + 1]], verts[shape.tri[t + 2]], true, zb.data(), nullptr, row_lo, row_hi);
	}
	};
	const int rows = zy1 - zy0 + 1;
	ParallelBands(rows, [&](int lo, int hi) { draw_rows(zy0 + lo, zy0 + hi); }, order.size() >= 4096 && rows >= 128);
	if (depth_out != nullptr) {
		for (int y = zy0; y <= zy1; y++) std::copy_n(&zb[static_cast<size_t>(y) * fw + zx0], zx1 - zx0 + 1, &(*depth_out)[static_cast<size_t>(y) * fw + zx0]);
	}
	_turned_times.models_raster += TurnedNow() - t_projected;
	if (triangles_only) return;
	/* At night its lit windows over it, kept bright (DrawNight()), and the places of its glows. The glass of an
	 * add-on's lamp in the add-on's texture, in its colour at this place (the paper of a lamp blue by day, blue lit). */
	if (_night_windows_now && o != nullptr && !o->night_tri.empty()) {
		static std::vector<TurnedVertex> night;
		const TileIndex at = TileXY(std::clamp<int>(static_cast<int>(ox) / TILE_SIZE, 0, Map::MaxX()), std::clamp<int>(static_cast<int>(oy) / TILE_SIZE, 0, Map::MaxY()));
		const size_t corners = o->night_pos.size() / 3;
		night.resize(corners);
		for (size_t i = 0; i < corners; i++) {
			double sx, sy, xr, yr;
			lay.Project(ox + o->night_pos[i * 3], oy + o->night_pos[i * 3 + 1], oz + o->night_pos[i * 3 + 2], sx, sy);
			lay.Turn(ox + o->night_pos[i * 3], oy + o->night_pos[i * 3 + 1], xr, yr);
			night[i] = {sx, sy, o->night_uv[i * 2], o->night_uv[i * 2 + 1], 256.0, xr + yr + 2 * o->night_pos[i * 3 + 2]};
		}
		for (size_t t = 0; t + 2 < o->night_tri.size(); t += 3) {
			const uint8_t k = o->NightTexture(t);
			const ThreeDObjectTexture *nt = k == 0 ? tex : o->GetTextureOf(k, to_underlying(lay.zoom), o->AddonPaintAt(at, k));
			if (nt == nullptr) continue;
			TurnedVertex a = night[o->night_tri[t]], b = night[o->night_tri[t + 1]], c = night[o->night_tri[t + 2]];
			if (std::max({a.x, b.x, c.x}) < 0 || std::min({a.x, b.x, c.x}) >= fw || std::max({a.y, b.y, c.y}) < 0 || std::min({a.y, b.y, c.y}) >= fh) continue;
			for (TurnedVertex *v : {&a, &b, &c}) {
				v->u *= nt->w;
				v->v *= nt->h;
			}
			DrawTexturedTriangle(dst, pitch, fw, fh, nt->px.data(), nt->w, nt->h, a, b, c, false, zb.data(), &_night_lit);
		}
	}
	if (_night_windows_now && o != nullptr) {
		for (size_t i = 0; i + 2 < o->lights.size(); i += 3) _night_glows.push_back({ox + o->lights[i], oy + o->lights[i + 1], oz + o->lights[i + 2]});
	}
}

/** The pictures of the buildings drawn (GetTurnedObjectPicture()), by object, place and the view's turn and zoom. */
struct TurnedObjectKey {
	const ThreeDObject *obj;
	TileIndex origin;
	int z;
	uint32_t paint;
	double c, s, px, py;
	ZoomLevel zoom;
	bool cull;

	auto operator<=>(const TurnedObjectKey &) const = default;
};
static std::map<TurnedObjectKey, TurnedObjectPicture> _turned_object_pictures;
static size_t _turned_object_bytes = 0;
static uint64_t _turned_object_clock = 0;
/** The memory the pictures may take; the least lately drawn go first. */
static constexpr size_t TURNED_OBJECT_BUDGET = 256u << 20;
/** The largest picture kept, pixels; a larger one (a big house close) is drawn each time. */
static constexpr size_t TURNED_OBJECT_MOST_PIXELS = 4u << 20;
bool _turned_object_pictures_on = true; ///< The rig: 'test3d mezipamet 0' draws every building each time.

/**
 * Whether the turned view at a turn and zoom has stood a while: while it turns or zooms each frame is another view,
 * and a building drawn whole for a picture costs more than the part of it on the screen.
 */
static bool TurnedViewStands(const ObjectLay &lay)
{
	using Clock = std::chrono::steady_clock;
	static std::map<std::tuple<double, double, double, double, ZoomLevel>, std::pair<Clock::time_point, Clock::time_point>> views; // first and last seen
	const Clock::time_point now = Clock::now();
	auto [it, added] = views.try_emplace({lay.c, lay.s, lay.px, lay.py, lay.zoom}, now, now);
	it->second.second = now;
	if (added && views.size() > 16) {
		/* Those not seen for a while forgotten: those of turning. */
		for (auto v = views.begin(); v != views.end();) v = now - v->second.second > std::chrono::seconds(2) ? views.erase(v) : std::next(v);
	}
	return now - it->second.first >= std::chrono::milliseconds(200);
}

/**
 * The picture of a building's triangles as the turned view is now, drawn the first time it is wanted: the view
 * drawn again (another part of the screen, a moving vehicle, the next frame) only copies it.
 * @return nullptr when it is not kept (too large, or the view is turning)
 */
static const TurnedObjectPicture *GetTurnedObjectPicture(const ObjectLay &lay, const ThreeDObject *o, TileIndex origin, int z, uint32_t paint)
{
	const TurnedObjectKey key{o, origin, z, paint, lay.c, lay.s, lay.px, lay.py, lay.zoom, _turned_cull};
	auto it = _turned_object_pictures.find(key);
	if (it != _turned_object_pictures.end()) {
		it->second.used = ++_turned_object_clock;
		return &it->second;
	}
	if (!TurnedViewStands(lay)) return nullptr;
	/* Its extent: every corner of its shape, whole pixels of the view. */
	const ThreeDObjectShapeView shape = TurnedObjectShape(o, lay);
	if (shape.tri.empty()) return nullptr;
	const double ox = TileX(origin) * TILE_SIZE, oy = TileY(origin) * TILE_SIZE;
	double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
	for (size_t i = 0; i + 2 < shape.pos.size(); i += 3) {
		double sx, sy;
		lay.Project(ox + shape.pos[i], oy + shape.pos[i + 1], z + shape.pos[i + 2], sx, sy);
		x0 = std::min(x0, sx);
		x1 = std::max(x1, sx);
		y0 = std::min(y0, sy);
		y1 = std::max(y1, sy);
	}
	const int left = static_cast<int>(std::floor(x0)) - 2, top = static_cast<int>(std::floor(y0)) - 2;
	const int width = static_cast<int>(std::ceil(x1)) - left + 3, height = static_cast<int>(std::ceil(y1)) - top + 3;
	if (width <= 0 || height <= 0 || static_cast<size_t>(width) * height > TURNED_OBJECT_MOST_PIXELS) return nullptr;

	TurnedObjectPicture pic;
	pic.width = width;
	pic.height = height;
	pic.left = lay.left + left / lay.scale;
	pic.top = lay.top + top / lay.scale;
	pic.pixels.assign(static_cast<size_t>(width) * height, 0);
	ObjectLay own = lay;
	own.left = pic.left;
	own.top = pic.top;
	DrawObjectInto(pic.pixels.data(), width, width, height, own, o, ox, oy, z, paint, {}, nullptr, true, &pic.depth);

	const size_t bytes = static_cast<size_t>(width) * height * (sizeof(uint32_t) + sizeof(float));
	while (_turned_object_bytes + bytes > TURNED_OBJECT_BUDGET && !_turned_object_pictures.empty()) {
		auto oldest = std::ranges::min_element(_turned_object_pictures, {}, [](const auto &e) { return e.second.used; });
		_turned_object_bytes -= static_cast<size_t>(oldest->second.width) * oldest->second.height * (sizeof(uint32_t) + sizeof(float));
		_turned_object_pictures.erase(oldest);
	}
	_turned_object_bytes += bytes;
	pic.used = ++_turned_object_clock;
	return &_turned_object_pictures.emplace(key, std::move(pic)).first->second;
}

/** How much memory the pictures of the buildings take now (GetTurnedObjectPicture()), for the rig (test3d pamet). */
size_t GetTurnedObjectPictureBytes(size_t &count)
{
	count = _turned_object_pictures.size();
	return _turned_object_bytes;
}

/** Draw a part of a building drawn as a 3D object: from its picture when it is kept (GetTurnedObjectPicture()). */
static void DrawTurnedObjectPart(const DrawPixelInfo &frame, const ObjectLay &lay, const ThreeDObject *o, TileIndex origin, int z, uint32_t paint, std::span<const ThreeDObjectFigure> more)
{
	uint32_t *dst = static_cast<uint32_t *>(frame.dst_ptr);
	const int fw = UnScaleByZoom(frame.width, frame.zoom), fh = UnScaleByZoom(frame.height, frame.zoom);
	const double ox = TileX(origin) * TILE_SIZE, oy = TileY(origin) * TILE_SIZE;
	const TurnedObjectPicture *picture = o != nullptr && _turned_object_pictures_on ? GetTurnedObjectPicture(lay, o, origin, z, paint) : nullptr;
	DrawObjectInto(dst, frame.pitch, fw, fh, lay, o, ox, oy, z, paint, more, picture);
}

/**
 * Draw a building as a real 3D object (box_kind 7, DrawObjectInto()): once, whole; the sprites of its tiles are left
 * out (box_kind 4). A field's soil is drawn whole first, under its plants.
 */
static void DrawTurnedObject(const DrawPixelInfo &frame, const ParentSpriteToDraw &ps)
{
	if (ps.box_dx < 0 || static_cast<size_t>(ps.box_dx) >= _turned_objects.size()) return;
	const TurnedObject &to = _turned_objects[ps.box_dx];
	double t0 = TurnedNow();
	if (_north_house_objects_now) {
		/* Facing north, its box on the screen (with what overhangs its plot and the figures on its roof) made plain
		 * colours first (RealizeNorthRect()). */
		const int over = TILE_SIZE / 2, figures = static_cast<int>(6 * OBJECT_HEIGHT_PER_METRE);
		int l = INT32_MAX, r = INT32_MIN, t = INT32_MAX, b = INT32_MIN;
		for (int corner = 0; corner < 4; corner++) {
			for (int z : {ps.zmin, ps.zmax + figures}) {
				const Point p = RemapCoords((corner & 1) ? ps.xmax + over : ps.xmin - over, (corner & 2) ? ps.ymax + over : ps.ymin - over, z);
				l = std::min(l, p.x); r = std::max(r, p.x);
				t = std::min(t, p.y); b = std::max(b, p.y);
			}
		}
		RealizeNorthRect(frame, UnScaleByZoom(l - frame.left, frame.zoom) - 2, UnScaleByZoom(t - frame.top, frame.zoom) - 2, UnScaleByZoom(r - frame.left, frame.zoom) + 3, UnScaleByZoom(b - frame.top, frame.zoom) + 3);
	}
	const ObjectLay lay = MapLay(frame);
	if (to.ground != nullptr) DrawTurnedObjectPart(frame, lay, to.ground, to.origin, to.z, to.ground->PaintAt(to.origin), {});
	DrawTurnedObjectPart(frame, lay, to.obj, to.origin, to.z, to.obj != nullptr ? to.obj->PaintAt(to.origin) : 0, to.more);
	_turned_times.models += TurnedNow() - t0;
}

/**
 * A picture of 3D objects laid together (the preview beside an industry's window): at an angle of the view and a
 * zoom, its left and top about where the world origin of the placements is drawn, the first placement drawn first.
 * @return whether there is anything in it
 */
bool RenderThreeDObjectsPicture(std::span<const ThreeDObjectPlacement> parts, float yaw, ZoomLevel zoom, ThreeDModelPicture &out)
{
	ObjectLay lay;
	const double a = yaw * M_PI / 180.0;
	lay.c = std::cos(a);
	lay.s = std::sin(a);
	lay.scale = 1.0 / (1 << to_underlying(zoom));
	lay.yaw = yaw;
	lay.zoom = zoom;
	/* Its extent: every corner, and a tall figure's height over every placement. */
	double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
	auto grow = [&](double x, double y, double z) {
		double sx, sy;
		lay.Project(x, y, z, sx, sy);
		x0 = std::min(x0, sx); x1 = std::max(x1, sx);
		y0 = std::min(y0, sy); y1 = std::max(y1, sy);
	};
	for (const ThreeDObjectPlacement &p : parts) {
		for (const ThreeDObject *o : {p.ground, p.obj}) {
			if (o == nullptr) continue;
			o->Read();
			for (size_t i = 0; i + 2 < o->pos.size(); i += 3) grow(p.x + o->pos[i], p.y + o->pos[i + 1], p.z + o->pos[i + 2]);
			grow(p.x, p.y, p.z + 6 * OBJECT_HEIGHT_PER_METRE);
		}
		grow(p.x, p.y, p.z);
		grow(p.x + TILE_SIZE, p.y + TILE_SIZE, p.z + 6 * OBJECT_HEIGHT_PER_METRE);
	}
	if (x0 > x1) return false;
	const int left = static_cast<int>(std::floor(x0)) - 4, top = static_cast<int>(std::floor(y0)) - 4;
	const int width = static_cast<int>(std::ceil(x1)) - left + 8, height = static_cast<int>(std::ceil(y1)) - top + 8;
	if (width <= 0 || height <= 0 || static_cast<size_t>(width) * height > 16u * 1024 * 1024) return false;
	lay.left = left / lay.scale;
	lay.top = top / lay.scale;
	out.width = width;
	out.height = height;
	out.left = left;
	out.top = top;
	out.pixels.assign(static_cast<size_t>(width) * height, 0);
	for (const ThreeDObjectPlacement &p : parts) {
		if (p.ground != nullptr) DrawObjectInto(out.pixels.data(), width, width, height, lay, p.ground, p.x, p.y, p.z, p.paint, {});
		DrawObjectInto(out.pixels.data(), width, width, height, lay, p.obj, p.x, p.y, p.z, p.paint, p.more);
	}
	/* The pixels drawn are whole; the rest stays nothing. */
	for (uint32_t &px : out.pixels) {
		if (px != 0) px = Colour(Colour(px).r, Colour(px).g, Colour(px).b, 255).data;
	}
	return true;
}

/** Each building that is a 3D object is one thing to draw among the standing ones, by the box of its plot (box_kind 7). */
static void AddTurnedObjectsToDraw()
{
	/* Their textures made on every core first, before they are drawn. */
	std::vector<ThreeDTextureWanted> wanted;
	for (const TurnedObject &to : _turned_objects) {
		if (to.obj != nullptr) wanted.push_back({to.obj, to_underlying(_vd.dpi.zoom), to.obj->PaintAt(to.origin)});
		if (to.ground != nullptr) wanted.push_back({to.ground, to_underlying(_vd.dpi.zoom), to.ground->PaintAt(to.origin)});
	}
	PrepareThreeDObjectTextures(wanted);
	for (size_t m = 0; m < _turned_objects.size(); m++) {
		const TurnedObject &to = _turned_objects[m];
		ParentSpriteToDraw &ps = _vd.parent_sprites_to_draw.emplace_back();
		int ox = TileX(to.origin) * TILE_SIZE, oy = TileY(to.origin) * TILE_SIZE;
		/* Only figures (the girls by the field road): the box of the tile, as tall as a girl twice the size. */
		const int tiles_x = to.obj != nullptr ? to.obj->tiles_x : 1, tiles_y = to.obj != nullptr ? to.obj->tiles_y : 1;
		/* (Its height as read: read now if the textures made above did not read it, ThreeDObject::Read().) */
		if (to.obj != nullptr) to.obj->Read();
		const int top = to.obj != nullptr ? static_cast<int>(std::ceil(to.obj->top)) : 2 * TILE_HEIGHT;
		ps.xmin = ox;
		ps.xmax = ox + tiles_x * TILE_SIZE - 1;
		ps.ymin = oy;
		ps.ymax = oy + tiles_y * TILE_SIZE - 1;
		if (to.obj != nullptr && to.obj->street && to.obj->extent[1] >= to.obj->extent[0]) {
			/* A street lamp by the street, the cars on it: its own box, not its tile's. */
			ps.xmin = ox + static_cast<int>(std::floor(to.obj->extent[0]));
			ps.xmax = ox + static_cast<int>(std::ceil(to.obj->extent[1]));
			ps.ymin = oy + static_cast<int>(std::floor(to.obj->extent[2]));
			ps.ymax = oy + static_cast<int>(std::ceil(to.obj->extent[3]));
		}
		ps.zmin = to.z + to.sort_z;
		ps.zmax = to.z + to.sort_z + std::max(1, top);
		ps.x = ps.y = ps.left = ps.top = 0;
		ps.image = SPR_EMPTY_BOUNDING_BOX;
		ps.pal = PAL_NONE;
		ps.sub = nullptr;
		ps.first_child = -1;
		ps.order = 0;
		ps.ax = ox;
		ps.ay = oy;
		ps.az = to.z;
		ps.box = nullptr;
		ps.box_ox = ox;
		ps.box_oy = oy;
		ps.box_oz = to.z;
		ps.box_dx = static_cast<int16_t>(m);
		ps.box_dy = 0;
		ps.box_kind = 7;
	}
}

/**
 * Leave out the sprites a tile just added (box_kind 4): its building is drawn as a 3D object instead. Facing north
 * the walls of the foundation of a tile on a slope stay: without them there was nothing under the house -- black
 * holes in a screenshot, the player saw; on the screen what was drawn there before.
 * @param first_parent the first of the sprites the tile added
 * @param keep_foundation whether the foundation stays (facing north; turned, the ground is laid by itself)
 */
static void LeaveOutTileSprites(size_t first_parent, bool keep_foundation)
{
	for (size_t i = first_parent; i < _vd.parent_sprites_to_draw.size(); i++) {
		if (keep_foundation && (static_cast<int>(i) == _vd.foundation[FoundationPart::Normal] || static_cast<int>(i) == _vd.foundation[FoundationPart::Halftile])) {
			/* Its walls stay, and the ground laid on it (its children): the objects have no ground of their own
			 * under them -- without it the yard of a house on a slope by the water showed the water, elsewhere a
			 * black hole (the player, 16. 10.). */
			continue;
		}
		_vd.parent_sprites_to_draw[i].box_kind = 4;
	}
}

/**
 * One of the game's own industries as a real 3D object (the colleague's objekty/prumysl/): drawn once, whole, as a
 * house; a tile of a field (the marijuana plantation, the potato field) laid from the parts of the fields -- the
 * soil under the plants of the stage and the girl at work, and from the tile before the field road the road tile
 * behind it with the road (flat and first, under the cars), the shed and the haystack and the girls by its edges,
 * each its own thing. The sprites the tile just added are left out (box_kind 4). In the turned view, and facing
 * north as the houses (gui.three_d_north_houses): the potato field, the distillery and the pub have no pictures of
 * their own there.
 * @param first_parent the first of the sprites the tile added
 * @param north whether the view faces north (the foundations stay, LeaveOutTileSprites())
 */
static void AddIndustryObjects(size_t first_parent, bool north)
{
	if (_cur_ti.tile == INVALID_TILE || !IsTileType(_cur_ti.tile, TileType::Industry) || IsTransparencySet(TransparencyOption::Industries)) return;
	const bool built = IsIndustryCompleted(_cur_ti.tile);
	const Industry *ind = Industry::GetByTile(_cur_ti.tile);
	const ThreeDObject *obj = built ? GetThreeDIndustryObjectOf(ind) : nullptr;
	ThreeDPlantationTile pt;
	ThreeDFieldTile ft;
	if (const ThreeDIndustryTileParts *parts = obj == nullptr ? OriginalIndustryTileParts(_cur_ti.tile) : nullptr; parts != nullptr) {
		/* An industry of the original game, from the parts the industries share; being built, its ground only. */
		LeaveOutTileSprites(first_parent, north);
		AddOriginalIndustryTile(_cur_ti.tile, *parts, built);
		return;
	}
	if (!built) return;
	if (obj != nullptr) {
		LeaveOutTileSprites(first_parent, north);
		const TileIndex origin = ind->location.tile;
		bool known = false;
		for (const TurnedObject &to : _turned_objects) {
			if (to.origin == origin && to.obj == obj) known = true;
		}
		/* With the girls that came with the deliveries, when they are there. */
		if (!known) _turned_objects.push_back({obj, origin, GetTileMaxPixelZ(origin), nullptr, GetThreeDIndustryGirls(ind, obj)});
	} else if (GetThreeDPlantationTile(_cur_ti.tile, pt) && GetThreeDFieldTile(pt, ft)) {
		LeaveOutTileSprites(first_parent, north);
		if (ft.plants != nullptr) {
			_turned_objects.push_back({ft.plants, _cur_ti.tile, _cur_ti.z, ft.soil, ft.worker});
		} else {
			_turned_objects.push_back({ft.soil, _cur_ti.tile, _cur_ti.z, nullptr, ft.worker});
		}
		if (ft.road != nullptr) {
			const TileIndex road_tile = TileAddXY(_cur_ti.tile, 0, 1);
			const int road_z = GetTilePixelZ(road_tile);
			_turned_objects.push_back({ft.road, road_tile, road_z, nullptr, {}, -4 * static_cast<int>(TILE_HEIGHT)});
			if (ft.shed != nullptr) _turned_objects.push_back({ft.shed, road_tile, road_z});
			if (ft.hay != nullptr) _turned_objects.push_back({ft.hay, road_tile, road_z});
			for (int edge = 0; edge < 2; edge++) {
				if (!ft.girls[edge].empty()) _turned_objects.push_back({nullptr, road_tile, road_z, nullptr, ft.girls[edge]});
			}
		}
		if (_turned_debug_tile == _cur_ti.tile) IConsolePrint(CC_DEFAULT, "ladeni: field tile x {} stage {} worker {} road behind {}: plants {}, worker {}", pt.x, pt.stage, pt.worker, pt.road_behind, ft.plants != nullptr ? ft.plants->name : "none", ft.worker.size());
	}
}

/**
 * The parts of a tile of an industry of the original game drawn from the parts the industries share (the colleague's
 * hra/3d/PROSBA-PRUMYSL-VANILLA-3D.md, GetThreeDOriginalIndustryTile()); nullptr for a tile of a set (NewGRF) or one
 * the sets have none for (Toyland), drawn by its sprites as ever.
 */
static const ThreeDIndustryTileParts *OriginalIndustryTileParts(TileIndex tile)
{
	const IndustryGfx gfx = GetIndustryGfx(tile);
	if (gfx >= NEW_INDUSTRYTILEOFFSET || GetIndustryTileSpec(gfx)->grf_prop.HasGrfFile()) return nullptr;
	return GetThreeDOriginalIndustryTile(gfx);
}

/**
 * The anchor of the block a tile of an original industry is of (a factory of 2 x 2 tiles: its north tile draws it
 * whole) and its parts; the tile itself when it is no member of a block. INVALID_TILE when the anchor is not there.
 */
static TileIndex OriginalIndustryAnchor(TileIndex tile, const ThreeDIndustryTileParts *&parts)
{
	if (!parts->member) return tile;
	const TileIndex anchor = TileAddWrap(tile, -parts->anchor_dx, -parts->anchor_dy);
	if (anchor == INVALID_TILE || !IsTileType(anchor, TileType::Industry) || GetIndustryIndex(anchor) != GetIndustryIndex(tile)) return INVALID_TILE;
	parts = OriginalIndustryTileParts(anchor);
	return parts != nullptr && !parts->member ? anchor : INVALID_TILE;
}

/**
 * A tile of an original industry among the turned objects: its ground (flat and first, as the field road); when it is
 * built, the parts and plants of its building, from the anchor of its block (once, whichever of its tiles is drawn).
 */
static void AddOriginalIndustryTile(TileIndex tile, const ThreeDIndustryTileParts &parts, bool built)
{
	if (parts.ground != nullptr) _turned_objects.push_back({parts.ground, tile, GetTileMaxPixelZ(tile), nullptr, {}, -4 * static_cast<int>(TILE_HEIGHT)});
	if (!built) return;
	const ThreeDIndustryTileParts *of = &parts;
	const TileIndex anchor = OriginalIndustryAnchor(tile, of);
	if (anchor == INVALID_TILE) return;
	const int z = GetTileMaxPixelZ(anchor);
	auto add = [&](const ThreeDObject *obj) {
		for (const TurnedObject &to : _turned_objects) {
			if (to.origin == anchor && to.obj == obj) return;
		}
		_turned_objects.push_back({obj, anchor, z});
	};
	for (const ThreeDObject *p : of->parts) add(p);
	if (of->plants != nullptr) add(of->plants);
}

/**
 * Wheat on a field of a farm (the colleague's pv_pole_psenice_*, hra/3d/PROSBA-PRUMYSL-VANILLA-3D.md): none on the
 * ploughed field nor on the haystacks of the harvest, young while it grows (the field's type 1 to 3), ripe before the
 * harvest (4 to 7), each plant on the field's slope. No girls in the field, nothing is hoed there.
 */
static void AddFieldWheat()
{
	if (_cur_ti.tile == INVALID_TILE || !IsTileType(_cur_ti.tile, TileType::Clear) || !IsClearGround(_cur_ti.tile, ClearGround::Fields)) return;
	const uint type = GetFieldType(_cur_ti.tile);
	if (type == 0 || type >= 8) return;
	const ThreeDObject *wheat = GetThreeDFieldPart(type < 4 ? "psenice_mlada" : "psenice_zrala");
	if (wheat != nullptr) _turned_objects.push_back({GetThreeDSlopedPlants(wheat, _cur_ti.tileh), _cur_ti.tile, _cur_ti.z});
}

/** Whether a sprite is one of the game's own street lights (Roadside::StreetLights: the table _roadside_lamps of road_cmd.cpp). */
static bool IsStreetLightSprite(SpriteID image)
{
	const SpriteID s = image & SPRITE_MASK;
	return s == 0x57E || s == 0x57F;
}

/**
 * The game's own street lights a road tile just added (the paved streets of the towns, Roadside::StreetLights) as the
 * lamps of the objects (the colleague's set "lampy", GetThreeDStreetLamp()): each light's sprite left out (box_kind
 * 4), its lamp drawn among the standing things as a building is (box_kind 7) -- lit at night, its glow about it.
 * See-through houses leave the game's sprites as they are.
 * @param first_parent the first of the sprites the tile added
 */
static void AddStreetLamps(size_t first_parent)
{
	if (_cur_ti.tile == INVALID_TILE || IsTransparencySet(TransparencyOption::Houses)) return;
	for (size_t i = first_parent; i < _vd.parent_sprites_to_draw.size(); i++) {
		ParentSpriteToDraw &ps = _vd.parent_sprites_to_draw[i];
		if (!IsStreetLightSprite(ps.image)) continue;
		const ThreeDObject *lamp = GetThreeDStreetLamp(ps.xmin & 0xF, ps.ymin & 0xF);
		if (lamp == nullptr) continue;
		ps.box_kind = 4;
		_turned_objects.push_back({lamp, _cur_ti.tile, ps.zmin});
	}
}

/**
 * Facing north, a house that is a real 3D object drawn as in the turned view (the player, 15. 10.: "which houses show
 * at view 0, the original houses as today or the same as when we turn; by default the same as when we turn",
 * gui.three_d_north_houses): the sprites the house's tile just added are left out (box_kind 4) and its object is
 * drawn once, whole, among the standing things (box_kind 7, ViewportDrawParentSprites()).
 * @param first_parent the first of the sprites the tile added
 */
static void AddNorthHouseObject(size_t first_parent)
{
	if (_cur_ti.tile == INVALID_TILE || IsTransparencySet(TransparencyOption::Houses) || !IsHouseCompleted(_cur_ti.tile)) return;
	HouseID north = GetHouseType(_cur_ti.tile);
	TileIndex origin = _cur_ti.tile + GetHouseNorthPart(north);
	const ThreeDObject *obj = GetThreeDHouseObjectAt(north, origin);
	if (obj == nullptr) return;
	LeaveOutTileSprites(first_parent, true);
	for (const TurnedObject &to : _turned_objects) {
		if (to.origin == origin) return;
	}
	/* With the figures of its event when it is on (the new stadium's, GetThreeDHouseEvent()). */
	_turned_objects.push_back({obj, origin, GetTileMaxPixelZ(origin), nullptr, GetThreeDHouseEvent(obj, origin)});
}

/**
 * Lay the ground of a plain tile (no foundation) from the block's north-up
 * texture (ViewportDoDrawTurned()) over its four corners where the turned view
 * puts them: four triangles meeting in the middle, so slopes keep their folds.
 * @param frame the block of the screen being drawn
 * @param big the texture's DrawPixelInfo (virtual coordinates of the view facing north)
 * @param bw,bh the texture's size in pixels
 * @param x0,y0 the north corner of the tile (world units)
 * @param slope the slope of the tile
 * @param z the height of its north corner on flat ground
 */
static void DrawTurnedTileFromBlock(const DrawPixelInfo &frame, const DrawPixelInfo &big, int bw, int bh, int x0, int y0, Slope slope, int z)
{
	double t0 = TurnedNow();
	ZoomLevel zoom = frame.zoom;
	Slope s = RemoveHalftileSlope(slope);
	static const Corner corners[4] = {CORNER_N, CORNER_E, CORNER_S, CORNER_W};
	static const int cxs[4] = {0, 0, TILE_SIZE, TILE_SIZE};
	static const int cys[4] = {0, TILE_SIZE, TILE_SIZE, 0};
	double scale = 1.0 / (1 << to_underlying(zoom));
	TurnedVertex v[5];
	for (int i = 0; i < 5; i++) {
		double wx = (i < 4) ? x0 + cxs[i] : x0 + TILE_SIZE / 2.0;
		double wy = (i < 4) ? y0 + cys[i] : y0 + TILE_SIZE / 2.0;
		double wz = z + ((i < 4) ? GetSlopePixelZInCorner(s, corners[i]) : static_cast<int>(GetPartialPixelZ(TILE_SIZE / 2, TILE_SIZE / 2, s)));
		double fx, fy, cx, cy;
		ProjectTurnedF(wx, wy, wz, fx, fy);
		RemapCoordsF(wx, wy, wz, cx, cy);
		v[i].x = (fx - frame.left) * scale;
		v[i].y = (fy - frame.top) * scale;
		v[i].u = (cx - big.left) * scale;
		v[i].v = (cy - big.top) * scale;
	}
	uint32_t *dst = static_cast<uint32_t *>(frame.dst_ptr);
	int fw = UnScaleByZoom(frame.width, zoom), fh = UnScaleByZoom(frame.height, zoom);
	const uint32_t *tex = _turned_big.data();
	DrawTexturedTriangle(dst, frame.pitch, fw, fh, tex, bw, bh, v[0], v[1], v[4]);
	DrawTexturedTriangle(dst, frame.pitch, fw, fh, tex, bw, bh, v[1], v[2], v[4]);
	DrawTexturedTriangle(dst, frame.pitch, fw, fh, tex, bw, bh, v[2], v[3], v[4]);
	DrawTexturedTriangle(dst, frame.pitch, fw, fh, tex, bw, bh, v[3], v[0], v[4]);
	_turned_times.warp += TurnedNow() - t0;
}

/**
 * Draw a block of the main viewport with the view turned (three_d.h). Called
 * by ViewportDoDraw() with _vd.dpi set up for the block and _cur_dpi pointing
 * at it; takes care of everything up to the clearing of the lists.
 */
static void ViewportDoDrawTurned()
{
	const DrawPixelInfo frame = _vd.dpi;
	ZoomLevel zoom = frame.zoom;
	int mask = ScaleByZoom(-1, zoom);
	int fw = UnScaleByZoom(frame.width, zoom), fh = UnScaleByZoom(frame.height, zoom);
	if (fw <= 0 || fh <= 0) return;

	/* 1. The world box under the block, from the ground up to the tallest thing there can be. */
	int zmax = (_settings_game.construction.map_height_limit + _settings_game.construction.max_bridge_height) * TILE_HEIGHT + MAX_BUILDING_PIXELS;
	double minx = 1e18, maxx = -1e18, miny = 1e18, maxy = -1e18;
	for (int corner = 0; corner < 4; corner++) {
		for (int zz : {0, zmax}) {
			double vx = frame.left + ((corner & 1) ? frame.width : 0);
			double vy = frame.top + ((corner & 2) ? frame.height : 0);
			double wx, wy;
			InverseTurnedFlatF(vx, vy, zz, wx, wy);
			minx = std::min(minx, wx); maxx = std::max(maxx, wx);
			miny = std::min(miny, wy); maxy = std::max(maxy, wy);
		}
	}
	const int outside = 16; // tiles beyond the map edge still drawn (black)
	int tx0 = std::max(static_cast<int>(std::floor(minx / TILE_SIZE)) - 1, -outside);
	int tx1 = std::min(static_cast<int>(std::floor(maxx / TILE_SIZE)) + 1, static_cast<int>(Map::SizeX()) + outside);
	int ty0 = std::max(static_cast<int>(std::floor(miny / TILE_SIZE)) - 1, -outside);
	int ty1 = std::min(static_cast<int>(std::floor(maxy / TILE_SIZE)) + 1, static_cast<int>(Map::SizeY()) + outside);

	/* 2. The north-up box of that world box: the tiles and vehicles are collected against it, so nothing in the box is clipped away. */
	DrawPixelInfo cdpi = frame;
	{
		int cl = INT32_MAX, cr = INT32_MIN, ct = INT32_MAX, cb = INT32_MIN;
		for (int corner = 0; corner < 4; corner++) {
			for (int zz : {0, zmax}) {
				Point p = RemapCoords(((corner & 1) ? tx1 + 1 : tx0) * TILE_SIZE, ((corner & 2) ? ty1 + 1 : ty0) * TILE_SIZE, zz);
				cl = std::min(cl, p.x); cr = std::max(cr, p.x);
				ct = std::min(ct, p.y); cb = std::max(cb, p.y);
			}
		}
		cdpi.left = (cl - MAX_TILE_EXTENT_LEFT) & mask;
		cdpi.top = (ct - MAX_TILE_EXTENT_TOP) & mask;
		cdpi.width = (cr + MAX_TILE_EXTENT_RIGHT - cdpi.left + ~mask) & mask;
		cdpi.height = (cb + MAX_TILE_EXTENT_BOTTOM - cdpi.top + ~mask) & mask;
	}

	/* 3. Clear the block: black, and the animation buffer told these pixels do not animate. */
	BlitterFactory::GetCurrentBlitter()->DrawRect(frame.dst_ptr, fw, fh, PixelColour{0});

	/* 4. The tiles that can reach into the block, far first. */
	struct TurnedTile {
		int x, y;
		double near;
		TileType type;
		Slope raw_slope, top_slope;
		int raw_z, top_z;
		size_t first_sprite, end_sprite;
		int foundation[2];
		bool building;
	};
	std::vector<TurnedTile> tiles;
	for (int ty = ty0; ty <= ty1; ty++) {
		for (int tx = tx0; tx <= tx1; tx++) {
			_turned_times.tiles++;
			double vx, vy;
			ProjectTurnedF(tx * TILE_SIZE + TILE_SIZE / 2.0, ty * TILE_SIZE + TILE_SIZE / 2.0, 0, vx, vy);
			if (vx + 2 * TILE_PIXELS * ZOOM_BASE < frame.left || vx - 2 * TILE_PIXELS * ZOOM_BASE > frame.left + frame.width) continue;
			/* How high the tile reaches: its land, the tallest building, and a bridge over it if there is one. */
			int reach = MAX_BUILDING_PIXELS;
			bool inside = IsInsideBS(tx, 0, Map::SizeX()) && IsInsideBS(ty, 0, Map::SizeY());
			if (inside) {
				TileIndex tile = TileXY(tx, ty);
				reach += TilePixelHeight(tile) + 2 * TILE_HEIGHT;
				if (IsBridgeAbove(tile)) reach += GetBridgePixelHeight(GetNorthernBridgeEnd(tile)) - TilePixelHeight(tile);
			} else {
				reach += 2 * TILE_HEIGHT;
			}
			if (vy + MAX_TILE_EXTENT_BOTTOM + TILE_PIXELS * ZOOM_BASE < frame.top || vy - (reach + TILE_PIXELS) * ZOOM_BASE > frame.top + frame.height) continue;
			double xr, yr;
			TurnWorldPoint(tx * TILE_SIZE + TILE_SIZE / 2.0, ty * TILE_SIZE + TILE_SIZE / 2.0, xr, yr);
			tiles.push_back({tx, ty, xr + yr, TileType::Void, SLOPE_FLAT, SLOPE_FLAT, 0, 0, 0, 0, {-1, -1}, false});
		}
	}
	std::sort(tiles.begin(), tiles.end(), [](const TurnedTile &a, const TurnedTile &b) { return a.near < b.near; });

	/* The buildings that are 3D objects among them, each once, by its north tile, and the tiles of the fields from their parts. */
	_turned_objects.clear();
	_turned_vehicle_pictures.clear();
	_cab_bridge_sprites.clear();
	AutoRestoreBackup drawing_turned(_drawing_turned, true);

	/* Their draw procedures, in that order: what each says to draw is collected, and remembered per tile. The
	 * catenary and the fences along the rails stay out (the player: "turn them off in these 3D views"): they are
	 * lines of the north-up view, and turned they stand about the track as posts and ribbons. */
	TransparencyOptions no_catenary = _invisibility_opt;
	no_catenary.Set(TransparencyOption::Catenary);
	TransparencyOptions see_through = _transparency_opt;
	see_through.Set(TransparencyOption::Catenary);
	AutoRestoreBackup invisibility_backup(_invisibility_opt, no_catenary);
	AutoRestoreBackup transparency_backup(_transparency_opt, see_through);
	AutoRestoreBackup fences_backup(_settings_client.gui.show_rail_fences, false);
	AutoRestoreBackup field_fences_backup(_draw_fences, false);
	_vd.dpi = cdpi;
	double t_collect = 0;
	for (TurnedTile &t : tiles) {
		double tc0 = TurnedNow();
		TileType tile_type;
		_cur_ti.x = t.x * TILE_SIZE;
		_cur_ti.y = t.y * TILE_SIZE;
		if (IsInsideBS(t.x, 0, Map::SizeX()) && IsInsideBS(t.y, 0, Map::SizeY())) {
			_cur_ti.tile = TileXY(t.x, t.y);
			tile_type = GetTileType(_cur_ti.tile);
		} else {
			_cur_ti.tile = INVALID_TILE;
			tile_type = TileType::Void;
		}
		if (tile_type != TileType::Void) {
			std::tie(_cur_ti.tileh, _cur_ti.z) = GetTilePixelSlope(_cur_ti.tile);
		} else {
			std::tie(_cur_ti.tileh, _cur_ti.z) = GetTilePixelSlopeOutsideMap(t.x, t.y);
		}

		_vd.foundation_part = FoundationPart::None;
		_vd.foundation[FoundationPart::Normal] = -1;
		_vd.foundation[FoundationPart::Halftile] = -1;
		_vd.last_foundation_child[FoundationPart::Normal] = LAST_CHILD_NONE;
		_vd.last_foundation_child[FoundationPart::Halftile] = LAST_CHILD_NONE;

		size_t first = _vd.tile_sprites_to_draw.size();
		size_t first_parent = _vd.parent_sprites_to_draw.size();
		/* The land itself; the draw procedure levels the TileInfo when it draws a foundation. */
		t.type = tile_type;
		t.raw_slope = _cur_ti.tileh;
		t.raw_z = _cur_ti.z;
		_tile_type_procs[tile_type]->draw_tile_proc(&_cur_ti);
		if (_cur_ti.tile != INVALID_TILE) DrawTileSelection(&_cur_ti);
		/* The ground lies on top of the foundation, if the tile has one. */
		t.top_slope = t.raw_slope;
		t.top_z = t.raw_z;
		if (_cur_ti.tile != INVALID_TILE) std::tie(t.top_slope, t.top_z) = GetFoundationPixelSlope(_cur_ti.tile);
		t.first_sprite = first;
		t.end_sprite = _vd.tile_sprites_to_draw.size();
		t.foundation[0] = _vd.foundation[FoundationPart::Normal];
		t.foundation[1] = _vd.foundation[FoundationPart::Halftile];
		t.building = tile_type == TileType::House || tile_type == TileType::Industry;
		if (_turned_debug_tile != INVALID_TILE && _cur_ti.tile == _turned_debug_tile) {
			IConsolePrint(CC_DEFAULT, "ladeni: dlazdice {},{} typ {} svah {} z {} (po zakladech svah {} z {}) spritu dlazdice {} stojicich {} zaklady {}/{}", t.x, t.y, to_underlying(tile_type),
					to_underlying(t.raw_slope), t.raw_z, to_underlying(t.top_slope), t.top_z, t.end_sprite - first, _vd.parent_sprites_to_draw.size() - first_parent,
					t.foundation[0], t.foundation[1]);
			for (size_t i = first; i < t.end_sprite; i++) {
				const TileSpriteToDraw &ts = _vd.tile_sprites_to_draw[i];
				IConsolePrint(CC_DEFAULT, "ladeni:   sprite {} pal {} na {},{}", ts.image, ts.pal, ts.x, ts.y);
			}
			for (size_t i = first_parent; i < _vd.parent_sprites_to_draw.size(); i++) {
				const ParentSpriteToDraw &ps = _vd.parent_sprites_to_draw[i];
				IConsolePrint(CC_DEFAULT, "ladeni:   stojici {} pal {} na {},{} kotva {},{},{} deti {}", ps.image, ps.pal, ps.x, ps.y, ps.ax, ps.ay, ps.az, ps.first_child);
			}
		}
		t_collect += TurnedNow() - tc0;

		/* A bridge over the tile, and a bridge's end on it: built from their sprites (TurnedBridgeOverTile()). */
		if (_cur_ti.tile != INVALID_TILE) {
			for (const CabBridgeSprites &bs : _cab_bridge_sprites) {
				if (bs.tile == _cur_ti.tile && bs.to != SIZE_MAX) TurnedBridgeOverTile(_cur_ti.tile, bs.from, bs.to);
			}
			_cab_bridge_sprites.clear();
			if (tile_type == TileType::TunnelBridge && IsBridge(_cur_ti.tile)) TurnedBridgeHead(_cur_ti.tile, first_parent);
			/* A depot: one box over its tile (TurnedDepot()); see-through, its sprites as they are. */
			if ((tile_type == TileType::Railway || tile_type == TileType::Road || tile_type == TileType::Water) && !IsTransparencySet(TransparencyOption::Buildings)) {
				TurnedDepot(_cur_ti.tile, first_parent);
			}
		}

		/* A house made a real 3D object (three_d_objects.h) -- of the original game, or of a set the objects name, the
		 * Mars houses: drawn once, whole, by its triangles (DrawTurnedObject()), in place of the sprites of its tiles.
		 * One with eight renders keeps them. */
		if (tile_type == TileType::House && _cur_ti.tile != INVALID_TILE && !IsTransparencySet(TransparencyOption::Houses) && IsHouseCompleted(_cur_ti.tile)) {
			HouseID north = GetHouseType(_cur_ti.tile);
			{
				TileIndex origin = _cur_ti.tile + GetHouseNorthPart(north);
				const ThreeDObject *obj = GetThreeDHouseObjectAt(north, origin);
				if (obj != nullptr) {
					for (size_t i = first_parent; i < _vd.parent_sprites_to_draw.size(); i++) _vd.parent_sprites_to_draw[i].box_kind = 4;
					bool known = false;
					for (const TurnedObject &to : _turned_objects) {
						if (to.origin == origin) known = true;
					}
					/* With the figures of its event when it is on (the new stadium's, GetThreeDHouseEvent()). */
					if (!known) _turned_objects.push_back({obj, origin, GetTileMaxPixelZ(origin), nullptr, GetThreeDHouseEvent(obj, origin)});
				} else if (north >= NEW_HOUSE_OFFSET && _settings_client.gui.three_d_houses == 0) {
					/* A set's house without an object (ITL Houses): its boxes below, the set told of once. */
					NoteThreeDHouseBoxed(north);
				}
			}
		}
		/* One of the game's own industries as a real 3D object (AddIndustryObjects()). */
		if (tile_type == TileType::Industry) AddIndustryObjects(first_parent, false);
		/* The street lights of the towns' streets as lamps (AddStreetLamps()). */
		if (tile_type == TileType::Road) AddStreetLamps(first_parent);
		/* Wheat on the farm's fields (AddFieldWheat()). */
		if (tile_type == TileType::Clear) AddFieldWheat();
		/* A house of the original graphics with a hand table: its sprite is drawn as boxes, not as a billboard. */
		if (tile_type == TileType::House && _cur_ti.tile != INVALID_TILE && !IsTransparencySet(TransparencyOption::Houses)) {
			HouseID house = GetHouseType(_cur_ti.tile);
			if (house < NEW_HOUSE_OFFSET) {
				HouseID north = house;
				TileIndex north_tile = _cur_ti.tile + GetHouseNorthPart(north);
				const ThreeDHouse *model = GetThreeDHouse(north);
				if (model != nullptr) {
					for (size_t i = first_parent; i < _vd.parent_sprites_to_draw.size(); i++) {
						ParentSpriteToDraw &ps = _vd.parent_sprites_to_draw[i];
						if (ps.image == SPR_EMPTY_BOUNDING_BOX || ps.box_kind != 0) continue;
						ps.box_kind = 1;
						ps.box = model;
						ps.box_ox = TileX(north_tile) * TILE_SIZE;
						ps.box_oy = TileY(north_tile) * TILE_SIZE;
						ps.box_oz = _cur_ti.z;
						ps.box_dx = static_cast<int16_t>(static_cast<int>(TileX(_cur_ti.tile)) - static_cast<int>(TileX(north_tile)));
						ps.box_dy = static_cast<int16_t>(static_cast<int>(TileY(_cur_ti.tile)) - static_cast<int>(TileY(north_tile)));
					}
				}
			}
		}
		/* Anything else standing on a house, industry or object tile is one box, its bounding box: the box is
		 * narrowed to the sprite's width where the sprite is narrower than the footprint on the screen (a tower on
		 * a wide base); low things (fences, heaps) stay billboards. */
		bool boxed_type = (tile_type == TileType::House && !IsTransparencySet(TransparencyOption::Houses)) ||
				(tile_type == TileType::Industry && !IsTransparencySet(TransparencyOption::Industries)) ||
				(tile_type == TileType::Object && !IsTransparencySet(TransparencyOption::Structures));
		/* A station's platforms, buildings and roofs turn too, any set's: the layout gives every sprite its box.
		 * A low flat thing of a station (a platform, a bench) is a slab, its top from the sprite laid over the
		 * turned footprint, never snapping, so the platform's edge stays by the track; anything of a height is
		 * a box as on a house tile. */
		bool station_type = tile_type == TileType::Station && !IsTransparencySet(TransparencyOption::Buildings);
		/* A fence of a farm, an industry or a station -- a thin long low box -- is a line of the north-up view and
		 * stands about as a ribbon turned: not drawn (the player: "turn the fences off in these 3D views"). */
		for (size_t i = first_parent; i < _vd.parent_sprites_to_draw.size(); i++) {
			ParentSpriteToDraw &ps = _vd.parent_sprites_to_draw[i];
			if (ps.image == SPR_EMPTY_BOUNDING_BOX || ps.box_kind != 0) continue;
			int ex = ps.xmax - ps.xmin + 1, ey = ps.ymax - ps.ymin + 1, ez = ps.zmax - ps.zmin + 1;
			if (((ex >= 8 && ey <= 2) || (ey >= 8 && ex <= 2)) && ez < TILE_HEIGHT) ps.box_kind = 4;
		}
		if (boxed_type || station_type) {
			for (size_t i = first_parent; i < _vd.parent_sprites_to_draw.size(); i++) {
				ParentSpriteToDraw &ps = _vd.parent_sprites_to_draw[i];
				if (ps.image == SPR_EMPTY_BOUNDING_BOX || ps.box_kind != 0) continue;
				int ex = ps.xmax - ps.xmin + 1, ey = ps.ymax - ps.ymin + 1, ez = ps.zmax - ps.zmin + 1;
				if (ex < 2 || ey < 2) continue;
				if (ez < TILE_HEIGHT) {
					if (!station_type) continue;
					ps.box_kind = 6;
					ps.box = nullptr;
					ps.box_ox = ps.xmin;
					ps.box_oy = ps.ymin;
					ps.box_oz = ps.zmin;
					ps.box_dx = static_cast<int16_t>(ex);
					ps.box_dy = static_cast<int16_t>(ey);
					continue;
				}
				const Sprite *spr = GetSprite(ps.image & SPRITE_MASK, SpriteType::Normal);
				double footprint = 2.0 * ZOOM_BASE * (ex + ey);
				double f = std::min(1.0, spr->width / footprint);
				int nx = std::max(2, static_cast<int>(std::lround(ex * f))), ny = std::max(2, static_cast<int>(std::lround(ey * f)));
				ps.box_kind = 2;
				ps.box = nullptr;
				ps.box_ox = ps.xmin + (ex - nx) / 2;
				ps.box_oy = ps.ymin + (ey - ny) / 2;
				ps.box_oz = ps.zmin;
				ps.box_dx = static_cast<int16_t>(nx);
				ps.box_dy = static_cast<int16_t>(ny);
			}
		}
	}
	_turned_times.collect += t_collect;

	AddTurnedObjectsToDraw();

	/* The ground of the whole block, north-up, in one texture: the plain tiles' sprites drawn as the game draws
	 * them, so neighbouring tiles fit together and the edges of the warped tiles meet at any zoom. A tile with a
	 * foundation is drawn by itself (DrawTurnedTileGround()), since its raised ground and walls would lie over
	 * its neighbours here. */
	DrawPixelInfo bigdpi = frame;
	int bw = 0, bh = 0;
	{
		int cl = INT32_MAX, cr = INT32_MIN, ct = INT32_MAX, cb = INT32_MIN;
		for (const TurnedTile &t : tiles) {
			for (int corner = 0; corner < 4; corner++) {
				int x = t.x * TILE_SIZE + ((corner & 1) ? TILE_SIZE : 0), y = t.y * TILE_SIZE + ((corner & 2) ? TILE_SIZE : 0);
				Point lo = RemapCoords(x, y, t.raw_z), hi = RemapCoords(x, y, t.raw_z + 2 * TILE_HEIGHT);
				cl = std::min(cl, lo.x); cr = std::max(cr, lo.x);
				ct = std::min(ct, hi.y); cb = std::max(cb, lo.y);
			}
		}
		if (cl <= cr) {
			const int margin = TILE_HEIGHT * ZOOM_BASE;
			bigdpi.left = (cl - margin) & mask;
			bigdpi.top = (ct - margin) & mask;
			bigdpi.width = (cr + margin - bigdpi.left + ~mask) & mask;
			bigdpi.height = (cb + margin - bigdpi.top + ~mask) & mask;
			bw = UnScaleByZoom(bigdpi.width, zoom);
			bh = UnScaleByZoom(bigdpi.height, zoom);
		}
	}
	if (bw > 0 && bh > 0) {
		double t0 = TurnedNow();
		_turned_big.assign(static_cast<size_t>(bw) * bh, 0);
		bigdpi.dst_ptr = _turned_big.data();
		bigdpi.pitch = bw;
		{
			AutoRestoreBackup dpi_backup(_cur_dpi, &bigdpi);
			AutoRestoreBackup anim_backup(_screen_disable_anim, true);
			for (const TurnedTile &t : tiles) {
				if (t.foundation[0] != -1 || t.foundation[1] != -1) continue;
				for (size_t i = t.first_sprite; i < t.end_sprite; i++) {
					const TileSpriteToDraw &ts = _vd.tile_sprites_to_draw[i];
					DrawSpriteViewport(ts.image, ts.pal, ts.x, ts.y, ts.sub);
				}
			}
		}
		/* The round curves of rails along their arcs, into the ground's picture (round_track.h). */
		DrawRoundTrackStrips(bigdpi);
		double t1 = TurnedNow();
		_turned_times.texture += t1 - t0;
		/* The black under a building, grown over from the ground beside it: on the rows of that tile. */
		double scale = 1.0 / (1 << to_underlying(zoom));
		for (const TurnedTile &t : tiles) {
			if (!t.building || t.foundation[0] != -1 || t.foundation[1] != -1) continue;
			Point n = RemapCoords(t.x * TILE_SIZE, t.y * TILE_SIZE, t.top_z + GetSlopeMaxPixelZ(RemoveHalftileSlope(t.top_slope)));
			Point sth = RemapCoords(t.x * TILE_SIZE + TILE_SIZE, t.y * TILE_SIZE + TILE_SIZE, t.top_z);
			int x0 = Clamp(static_cast<int>((n.x - TILE_SIZE * 2 * ZOOM_BASE - bigdpi.left) * scale), 0, bw);
			int x1 = Clamp(static_cast<int>((n.x + TILE_SIZE * 2 * ZOOM_BASE - bigdpi.left) * scale) + 1, 0, bw);
			int y0 = Clamp(static_cast<int>((n.y - bigdpi.top) * scale), 0, bh);
			int y1 = Clamp(static_cast<int>((sth.y - bigdpi.top) * scale) + 1, 0, bh);
			FillTurnedFootprint(_turned_big.data(), bw, x0, y0, x1, y1);
		}
		_turned_times.widen += TurnedNow() - t1;
	}

	/* 5. The ground, far first: a plain tile laid from the block's texture, a tile with a foundation as a box from its own. */
	for (const TurnedTile &t : tiles) {
		if (t.foundation[0] != -1 || t.foundation[1] != -1) {
			DrawTurnedTileGround(frame, t.first_sprite, t.end_sprite, t.x * TILE_SIZE, t.y * TILE_SIZE, t.top_slope, t.top_z, t.raw_slope, t.raw_z, t.building, t.foundation);
		} else if (bw > 0 && t.first_sprite < t.end_sprite) {
			DrawTurnedTileFromBlock(frame, bigdpi, bw, bh, t.x * TILE_SIZE, t.y * TILE_SIZE, t.top_slope, t.top_z);
		}
	}
	_turned_times.drawn += static_cast<int>(tiles.size());

	ViewportAddVehicles(&cdpi);
	/* The people walking about the towns (three_d_walkers.h), the fireworks over the huts. */
	AddThreeDWalkersToViewport(zoom);
	AddThreeDDragonsToViewport(zoom);
	AddThreeDAnimalsToViewport(zoom);
	AddThreeDWitchesToViewport(zoom);
	AddThreeDDriversToViewport(zoom);
	AddThreeDEffectsToViewport(zoom);

	/* The signs and the floating texts: collected north-up, then each moved to where the ground under it is drawn turned. */
	size_t first_string = _vd.string_sprites_to_draw.size();
	ViewportAddKdtreeSigns(&cdpi);
	DrawTextEffects(&cdpi);
	for (size_t i = first_string; i < _vd.string_sprites_to_draw.size(); i++) {
		StringSpriteToDraw &ss = _vd.string_sprites_to_draw[i];
		int cx = ss.x + ScaleByZoom(ss.width / 2, zoom), cy = ss.y + 24 * ZOOM_BASE;
		Point ground = InverseRemapCoords2(cx, cy);
		int z = (ground.x >= 0 && ground.x < static_cast<int>(Map::SizeX() * TILE_SIZE) && ground.y >= 0 && ground.y < static_cast<int>(Map::SizeY() * TILE_SIZE)) ? GetSlopePixelZ(ground.x, ground.y) : 0;
		Point classic = RemapCoords(ground.x, ground.y, z);
		Point turned = ProjectTurned(ground.x, ground.y, z);
		ss.x += turned.x - classic.x;
		ss.y += turned.y - classic.y;
	}
	_vd.dpi = frame;

	/* 5. Everything standing: each sprite hung from its anchor turned, far first. */
	struct TurnedSprite {
		double near;
		ParentSpriteToDraw *ps;
	};
	std::vector<TurnedSprite> standing;
	standing.reserve(_vd.parent_sprites_to_draw.size());
	for (ParentSpriteToDraw &ps : _vd.parent_sprites_to_draw) {
		Point c = RemapCoords(ps.ax, ps.ay, ps.az);
		Point t = ProjectTurned(ps.ax, ps.ay, ps.az);
		int dx = t.x - c.x, dy = t.y - c.y;
		ps.x += dx;
		ps.y += dy;
		ps.left += dx;
		ps.top += dy;
		double xr, yr;
		TurnWorldPoint((ps.xmin + ps.xmax + 1) * 0.5, (ps.ymin + ps.ymax + 1) * 0.5, xr, yr);
		standing.push_back({xr + yr + 2.0 * ps.zmin, &ps});
	}
	std::sort(standing.begin(), standing.end(), [](const TurnedSprite &a, const TurnedSprite &b) { return a.near < b.near; });
	double tb0 = TurnedNow();
	for (const TurnedSprite &s : standing) {
		const ParentSpriteToDraw *ps = s.ps;
		if (ps->box_kind == 1) {
			DrawTurnedHouseBox(frame, *ps);
			continue;
		}
		if (ps->box_kind == 2) {
			DrawTurnedGenericBox(frame, *ps);
			continue;
		}
		if (ps->box_kind == 4) continue; // a tile of a building that is a 3D object: drawn whole (box_kind 7)
		if (ps->box_kind == 5) {
			DrawTurnedVehicle(frame, *ps, false);
			continue;
		}
		if (ps->box_kind == 6) {
			DrawTurnedSlab(frame, *ps);
			continue;
		}
		if (ps->box_kind == 7) {
			DrawTurnedObject(frame, *ps);
			continue;
		}
		if (ps->box_kind == 8) {
			DrawTurnedBridgeDeck(frame, *ps);
			continue;
		}
		if (ps->box_kind == 9) {
			DrawTurnedBridgeRailing(frame, *ps);
			continue;
		}
		if (ps->box_kind == 10) {
			DrawTurnedBridgeRamp(frame, *ps);
			continue;
		}
		if (ps->box_kind == 11) {
			DrawTurnedBridgePillar(frame, *ps);
			continue;
		}
		if (ps->box_kind == 12) {
			DrawTurnedDepot(frame, *ps);
			continue;
		}
		if (ps->image != SPR_EMPTY_BOUNDING_BOX) DrawSpriteViewport(ps->image, ps->pal, ps->x, ps->y, ps->sub);
		int child_idx = ps->first_child;
		while (child_idx >= 0) {
			const ChildScreenSpriteToDraw &cs = _vd.child_screen_sprites_to_draw[child_idx];
			child_idx = cs.next;
			if (cs.relative) {
				DrawSpriteViewport(cs.image, cs.pal, ps->left + cs.x, ps->top + cs.y, cs.sub);
			} else {
				DrawSpriteViewport(cs.image, cs.pal, ps->x + cs.x, ps->y + cs.y, cs.sub);
			}
		}
	}
	_turned_times.standing += TurnedNow() - tb0;
}

/**
 * The night's light (gui.night): how much of red, green and blue stays, 256ths, as light as the night is set
 * (gui.night_light; the player, 16. 10.: "not only a bright full moon but nearly the dawn too, a nice dark, but
 * sometimes too much"): a dark night, the moon, the full moon, a bright full moon, nearly the dawn (a little warmer).
 */
struct NightLight {
	uint r, g, b;
};
static NightLight GetNightLight()
{
	static constexpr NightLight LEVELS[] = {
		{70, 82, 128},   // a dark night
		{95, 108, 150},  // the moon
		{122, 134, 172}, // the full moon
		{152, 162, 194}, // a bright full moon
		{188, 180, 200}, // nearly the dawn
	};
	NightLight light = LEVELS[std::min<size_t>(_settings_client.gui.night_light, std::size(LEVELS) - 1)];
	if (_game_mode == GameMode::Menu && _title_night < 1) {
		/* The title screen's dusk and dawn: from the day's light to the night's. */
		auto towards = [](uint night) { return static_cast<uint>(std::lround(256 - (256 - static_cast<int>(night)) * _title_night)); };
		light = {towards(light.r), towards(light.g), towards(light.b)};
	}
	return light;
}

/**
 * How far the night of the title screen is come (SelectGameWindow, the player, 16. 10.: "a long day and then a
 * while of night, round and round"): 0 the day, 1 the night, between them the dusk and the dawn.
 */
float _title_night = 0;

/** Whether the night is drawn: the player's (gui.night) in a game, on the title screen its own day and night. */
/**
 * Whether a vehicle's lights are on: at night, unless the player switched them off from its cab; by day too when the
 * player switched them on (F, Vehicle::cab_lights).
 */
bool VehicleLightsOn(const Vehicle *v)
{
	const uint8_t asked = v->First()->cab_lights;
	return asked == Vehicle::CAB_LIGHTS_ON || (asked == Vehicle::CAB_LIGHTS_AUTO && IsNightNow());
}

bool IsNightNow()
{
	if (_game_mode == GameMode::Menu) return _title_night > 0;
	return _settings_client.gui.night;
}

/**
 * Add a light to the pixels of the block being drawn: a soft spot, round or flat as the ground shows a circle, its
 * colour times its strength falling off to its edge, added to what is there.
 * @param cx,cy its middle, pixels of the block
 * @param rx,ry its half width and height, pixels
 */
static void AddNightLight(uint32_t *dst, int pitch, int w, int h, double cx, double cy, double rx, double ry, Colour colour, double strength)
{
	if (rx < 0.5 || ry < 0.5) return;
	const int x0 = std::max(0, static_cast<int>(std::floor(cx - rx))), x1 = std::min(w - 1, static_cast<int>(std::ceil(cx + rx)));
	const int y0 = std::max(0, static_cast<int>(std::floor(cy - ry))), y1 = std::min(h - 1, static_cast<int>(std::ceil(cy + ry)));
	for (int y = y0; y <= y1; y++) {
		uint32_t *row = dst + static_cast<size_t>(y) * pitch;
		const double dy = (y + 0.5 - cy) / ry;
		for (int x = x0; x <= x1; x++) {
			const double dx = (x + 0.5 - cx) / rx;
			const double d2 = dx * dx + dy * dy;
			if (d2 >= 1.0) continue;
			const double f = strength * (1.0 - d2) * (1.0 - d2);
			Colour c(row[x]);
			row[x] = Colour(static_cast<uint8_t>(std::min(255.0, c.r + colour.r * f)), static_cast<uint8_t>(std::min(255.0, c.g + colour.g * f)), static_cast<uint8_t>(std::min(255.0, c.b + colour.b * f))).data;
		}
	}
}

/**
 * Add a beam of light to the pixels of the block being drawn (a witch's hand torch, three_d_witches.h): from a point
 * widening to another, brightest along its middle and at its start, gone at its end (where its pool is), its colour
 * times its strength added to what is there.
 * @param ax,ay where it starts, pixels of the block
 * @param bx,by where it ends
 * @param ra,rb its half width there, pixels
 */
static void AddNightBeam(uint32_t *dst, int pitch, int w, int h, double ax, double ay, double bx, double by, double ra, double rb, Colour colour, double strength)
{
	const double vx = bx - ax, vy = by - ay, vv = vx * vx + vy * vy;
	if (vv < 1.0) return;
	ra = std::max(ra, 0.75);
	rb = std::max(rb, ra);
	const int x0 = std::max(0, static_cast<int>(std::floor(std::min(ax - ra, bx - rb)))), x1 = std::min(w - 1, static_cast<int>(std::ceil(std::max(ax + ra, bx + rb))));
	const int y0 = std::max(0, static_cast<int>(std::floor(std::min(ay - ra, by - rb)))), y1 = std::min(h - 1, static_cast<int>(std::ceil(std::max(ay + ra, by + rb))));
	for (int y = y0; y <= y1; y++) {
		uint32_t *row = dst + static_cast<size_t>(y) * pitch;
		for (int x = x0; x <= x1; x++) {
			const double px = x + 0.5 - ax, py = y + 0.5 - ay;
			const double along = (px * vx + py * vy) / vv;
			if (along < 0.0 || along > 1.0) continue;
			const double r = ra + (rb - ra) * along;
			const double ox = px - vx * along, oy = py - vy * along, d2 = (ox * ox + oy * oy) / (r * r);
			if (d2 >= 1.0) continue;
			const double f = strength * (1.0 - d2) * (1.0 - along * along);
			Colour c(row[x]);
			row[x] = Colour(static_cast<uint8_t>(std::min(255.0, c.r + colour.r * f)), static_cast<uint8_t>(std::min(255.0, c.g + colour.g * f)), static_cast<uint8_t>(std::min(255.0, c.b + colour.b * f))).data;
		}
	}
}

/**
 * The night (gui.night, the player: "a dark mode, and it will make night, as if it were night, and the cars will
 * shine"): the block just drawn darkened as under the moon, and the lights of the vehicles in it -- two headlights at
 * the front of a road vehicle, a train or a ship, with the pool of their light on the way ahead; the red lights at
 * the back of its last part; an aircraft's landing light and its red beacon blinking. Only the picture, at 32bpp;
 * the vehicles' boxes have room for the lights (Vehicle::UpdateBoundingBoxCoordinates()).
 * @param turned whether the block was drawn turned (three_d.h)
 */
static void DrawNight(bool turned)
{
	if (BlitterFactory::GetCurrentBlitter()->GetScreenDepth() != 32) return;
	const DrawPixelInfo &dpi = _vd.dpi;
	const int w = UnScaleByZoom(dpi.width, dpi.zoom), h = UnScaleByZoom(dpi.height, dpi.zoom);
	if (w <= 0 || h <= 0) return;
	/* The lit windows that are still as they were drawn: nothing came over them since. */
	std::erase_if(_night_lit, [](const auto &p) { return *p.first != p.second; });
	const NightLight moon = GetNightLight();
	BlitterFactory::GetCurrentBlitter()->DarkenRect(dpi.dst_ptr, w, h, moon.r, moon.g, moon.b);
	for (const auto &[at, colour] : _night_lit) *at = colour;

	uint32_t *dst = static_cast<uint32_t *>(dpi.dst_ptr);
	const double scale = 1.0 / (1 << to_underlying(dpi.zoom));
	auto project = [&](double x, double y, double z, double &px, double &py) {
		double vx, vy;
		if (turned) {
			ProjectTurnedF(x, y, z, vx, vy);
		} else {
			RemapCoordsF(x, y, z, vx, vy);
		}
		px = (vx - dpi.left) * scale;
		py = (vy - dpi.top) * scale;
	};
	/* A unit of the world along the ground, in pixels of the block (as wide as the map draws it). */
	const double unit = 2 * ZOOM_BASE * scale;
	static const int8_t dir_dx[8] = {-1, -1, -1, 0, 1, 1, 1, 0};
	static const int8_t dir_dy[8] = {-1, 0, 1, 1, 1, 0, -1, -1};
	const Colour lamp(255, 236, 190), pool(255, 220, 150), tail(255, 30, 20), beacon(255, 40, 40);
	/* The pixels of the lit glass left lit (the windows of the houses, the lights of the vehicles' models): the glow of
	 * a light only where they are seen -- one on the far side of a house, or hidden behind another, has none left. */
	std::vector<uint8_t> seen;
	auto lit_near = [&](int cx, int cy) {
		if (seen.empty()) {
			seen.assign(static_cast<size_t>(w) * h, 0);
			for (const auto &[at, colour] : _night_lit) {
				const ptrdiff_t i = at - dst;
				if (i < 0) continue;
				const int x = static_cast<int>(i % dpi.pitch), y = static_cast<int>(i / dpi.pitch);
				if (x < w && y < h) seen[static_cast<size_t>(y) * w + x] = 1;
			}
		}
		for (int y = std::max(0, cy - 2); y <= std::min(h - 1, cy + 2); y++) {
			for (int x = std::max(0, cx - 2); x <= std::min(w - 1, cx + 2); x++) {
				if (seen[static_cast<size_t>(y) * w + x]) return true;
			}
		}
		return false;
	};
	/* The glows of the houses' lights (their "svetlo_NN"): small and warm. */
	const Colour window(255, 200, 110);
	for (const auto &g : _night_glows) {
		double px, py;
		project(g[0], g[1], g[2], px, py);
		if (px < 0 || py < 0 || px >= w || py >= h) continue;
		if (!lit_near(static_cast<int>(px), static_cast<int>(py))) continue;
		AddNightLight(dst, dpi.pitch, w, h, px, py, 1.4 * unit, 1.4 * unit, window, 0.35);
	}
	/* A light of a vehicle's model (ThreeDPictureLight), its picture hung from the point hx, hy of the block. */
	auto model_light = [&](double hx, double hy, const ThreeDPictureLight &l) {
		const double px = hx + l.x, py = hy + l.y;
		switch (l.kind) {
			case ThreeDPictureLight::Kind::Front:
				if (lit_near(static_cast<int>(px), static_cast<int>(py))) AddNightLight(dst, dpi.pitch, w, h, px, py, 1.2 * unit, 1.2 * unit, lamp, 0.55);
				break;
			case ThreeDPictureLight::Kind::Back:
				if (lit_near(static_cast<int>(px), static_cast<int>(py))) AddNightLight(dst, dpi.pitch, w, h, px, py, 0.7 * unit, 0.7 * unit, tail, 0.6);
				break;
			case ThreeDPictureLight::Kind::PoolNear:
				AddNightLight(dst, dpi.pitch, w, h, px, py, 4 * unit, 2 * unit, pool, 0.4);
				break;
			case ThreeDPictureLight::Kind::Pool:
				AddNightLight(dst, dpi.pitch, w, h, px, py, 8 * unit, 4 * unit, pool, 0.4);
				break;
		}
	};
	/* The cars of the game's own driving about (three_d_walkers.h), only on the turned view. */
	if (turned) {
		ForEachThreeDDriverLight(dpi.zoom, [&](int x, int y, int z, const ThreeDPictureLight &l) {
			double hx, hy;
			project(x, y, z, hx, hy);
			model_light(hx, hy, l);
		});
	}
	std::vector<ThreeDPictureLight> model_lights;
	for (const Vehicle *v : Vehicle::Iterate()) {
		if (v->vehstatus.Test(VehState::Hidden)) continue;
		/* Switched off from its cab (F, VehicleLightsOn()). */
		if (!VehicleLightsOn(v)) continue;
		const bool front = v->Previous() == nullptr;
		const bool last = v->Next() == nullptr;
		if (v->type != VehicleType::Road && v->type != VehicleType::Train && v->type != VehicleType::Ship && v->type != VehicleType::Aircraft) continue;
		if (v->type == VehicleType::Aircraft && !Aircraft::From(v)->IsNormalAircraft()) continue;
		double cx, cy;
		project(v->x_pos, v->y_pos, v->z_pos, cx, cy);
		const double margin = 60 * unit;
		if (cx < -margin || cy < -margin || cx > w + margin || cy > h + margin) continue;
		/* A vehicle drawn from its object whose model has its lights (the colleague's
		 * hra/3d/PROSBA-SVETLA-VOZIDEL-UZLY.md, the player: "the cars must have their lights marked as the houses their
		 * windows; about the car there are lights a metre away"): they shine from where they are in the model, its
		 * picture as drawn, the glow of a lamp where its glass is seen. */
		if (ThreeDEnabled() && !v->vehstatus.Test(VehState::Crashed) && v->direction != Direction::Invalid) {
			RoundTrackPose pose;
			const bool round = turned && GetRoundTrackPose(v, pose);
			int ax, ay, az;
			if (GetThreeDVehicleObjectLights(v, dpi.zoom, round ? &pose : nullptr, model_lights, ax, ay, az)) {
				double hx, hy;
				project(ax, ay, az, hx, hy);
				for (const ThreeDPictureLight &l : model_lights) model_light(hx, hy, l);
				continue;
			}
		}
		if (!front && !last) continue;
		const int d = to_underlying(v->direction) & 7;
		double fx = dir_dx[d], fy = dir_dy[d];
		const double len = std::sqrt(fx * fx + fy * fy);
		fx /= len;
		fy /= len;
		const double sx = -fy, sy = fx; // to its side
		const double z = v->z_pos;
		if (v->type == VehicleType::Aircraft) {
			/* Its landing light ahead, and the red beacon on its back, blinking. */
			double px, py;
			project(v->x_pos + fx * 6, v->y_pos + fy * 6, z + 2, px, py);
			AddNightLight(dst, dpi.pitch, w, h, px, py, 3 * unit, 3 * unit, lamp, 0.9);
			if ((TimerGameTick::counter + v->index.base() * 7) % 32 < 6) {
				project(v->x_pos, v->y_pos, z + 6, px, py);
				AddNightLight(dst, dpi.pitch, w, h, px, py, 2 * unit, 2 * unit, beacon, 1.0);
			}
			continue;
		}
		const double half = v->type == VehicleType::Ship ? 6 : 3.5; // half its length, units
		if (front && (v->type != VehicleType::Train || Train::From(v)->IsEngine() || Train::From(v)->IsFrontEngine())) {
			/* The pool of the light on the way ahead, then the two lamps. */
			double px, py;
			project(v->x_pos + fx * (half + 9), v->y_pos + fy * (half + 9), z, px, py);
			AddNightLight(dst, dpi.pitch, w, h, px, py, 9 * unit, 4.5 * unit, pool, 0.45);
			project(v->x_pos + fx * (half + 4), v->y_pos + fy * (half + 4), z, px, py);
			AddNightLight(dst, dpi.pitch, w, h, px, py, 5 * unit, 2.5 * unit, pool, 0.5);
			for (int side = -1; side <= 1; side += 2) {
				project(v->x_pos + fx * half + sx * side * 1.5, v->y_pos + fy * half + sy * side * 1.5, z + 2, px, py);
				AddNightLight(dst, dpi.pitch, w, h, px, py, 1.6 * unit, 1.6 * unit, lamp, 1.2);
			}
		}
		if (last && v->type != VehicleType::Ship) {
			for (int side = -1; side <= 1; side += 2) {
				double px, py;
				project(v->x_pos - fx * half + sx * side * 1.5, v->y_pos - fy * half + sy * side * 1.5, z + 2, px, py);
				AddNightLight(dst, dpi.pitch, w, h, px, py, 1.0 * unit, 1.0 * unit, tail, 0.9);
			}
		}
	}
	/* The witches flying (three_d_witches.h; the player: "light all round the lantern, and from the hand torch a
	 * beam"), where they are drawn: the flame and the light all round it -- over her and what is near --, the beam of
	 * the torch and its pool where it falls on the ground. */
	if (turned || _north_house_objects) {
		for (const ThreeDWitchLight &l : GetThreeDWitchLights(false)) {
			double px, py;
			project(l.x, l.y, l.z, px, py);
			const double r = l.reach * unit;
			if (!l.torch) {
				AddNightLight(dst, dpi.pitch, w, h, px, py, r, r, l.colour, 0.45);
				AddNightLight(dst, dpi.pitch, w, h, px, py, std::max(1.5, r * 0.12), std::max(1.5, r * 0.12), l.colour, 1.0);
				continue;
			}
			const double len = l.ground > 0 ? l.ground : l.reach;
			const double up = OBJECT_HEIGHT_PER_METRE / OBJECT_UNITS_PER_METRE;
			double qx, qy;
			project(l.x + l.dx * len, l.y + l.dy * len, l.z + l.dz * len * up, qx, qy);
			const double spread = len * std::tan(l.half_angle) * unit;
			AddNightBeam(dst, dpi.pitch, w, h, px, py, qx, qy, 0.1 * unit, spread, l.colour, 0.35);
			AddNightLight(dst, dpi.pitch, w, h, px, py, std::max(1.2, 0.6 * unit), std::max(1.2, 0.6 * unit), l.colour, 1.0);
			if (l.ground > 0) AddNightLight(dst, dpi.pitch, w, h, qx, qy, spread * 1.2, spread * 0.6, l.colour, 0.55);
		}
	}
}

void ViewportDoDraw(const Viewport &vp, int left, int top, int right, int bottom)
{
	/* The round curves of rails (round_track.h), only in the turned view: facing north the player builds, the curves are as always. */
	RoundTrackPass round_track_pass(IsViewportTurned(vp));
	_vd.dpi.zoom = vp.zoom;
	int mask = ScaleByZoom(-1, vp.zoom);

	_vd.combine_sprites = SpriteCombineMode::None;

	_vd.dpi.width = (right - left) & mask;
	_vd.dpi.height = (bottom - top) & mask;
	_vd.dpi.left = left & mask;
	_vd.dpi.top = top & mask;
	/* The lit windows of the houses, when the night is drawn on 32 bpp (DrawNight()). */
	_night_lit.clear();
	_night_glows.clear();
	_night_windows_now = IsNightNow() && BlitterFactory::GetCurrentBlitter()->GetScreenDepth() == 32;
	_vd.dpi.pitch = _cur_dpi->pitch;
	_vd.last_child = LAST_CHILD_NONE;

	int x = UnScaleByZoom(_vd.dpi.left - (vp.virtual_left & mask), vp.zoom) + vp.left;
	int y = UnScaleByZoom(_vd.dpi.top - (vp.virtual_top & mask), vp.zoom) + vp.top;

	_vd.dpi.dst_ptr = BlitterFactory::GetCurrentBlitter()->MoveTo(_cur_dpi->dst_ptr, x - _cur_dpi->left, y - _cur_dpi->top);
	AutoRestoreBackup dpi_backup(_cur_dpi, &_vd.dpi);

	if (IsViewportTurned(vp)) {
		/* The second renderer (three_d.h): the same lists, drawn turned; the signs it collected are drawn below as always. */
		ViewportDoDrawTurned();
	} else {
		_north_house_objects = ThreeDEnabled() && _settings_client.gui.three_d_north_houses == 0 && !IsViewTurned() && BlitterFactory::GetCurrentBlitter()->GetScreenDepth() == 32;
		_turned_objects.clear();
		_turned_vehicle_pictures.clear();
		AutoRestoreBackup north_objects_backup(_north_house_objects_now, _north_house_objects);
		ViewportAddLandscape();
		if (_north_house_objects) AddTurnedObjectsToDraw();
		if (_north_house_objects) AddThreeDDragonsToViewport(_vd.dpi.zoom);
		if (_north_house_objects) AddThreeDAnimalsToViewport(_vd.dpi.zoom);
		if (_north_house_objects) AddThreeDWitchesToViewport(_vd.dpi.zoom);
		ViewportAddVehicles(&_vd.dpi);

		ViewportAddKdtreeSigns(&_vd.dpi);

		DrawTextEffects(&_vd.dpi);

		if (!_vd.tile_sprites_to_draw.empty()) ViewportDrawTileSprites(&_vd.tile_sprites_to_draw);

		for (auto &psd : _vd.parent_sprites_to_draw) {
			_vd.parent_sprites_to_sort.push_back(&psd);
		}

		_vp_sprite_sorter(&_vd.parent_sprites_to_sort);
		ViewportDrawParentSprites(&_vd.parent_sprites_to_sort, &_vd.child_screen_sprites_to_draw);

		if (_draw_bounding_boxes) ViewportDrawBoundingBoxes(&_vd.parent_sprites_to_sort);
		if (_draw_dirty_blocks) ViewportDrawDirtyBlocks();
	}

	/* The night over it all but the signs (gui.night). */
	if (IsNightNow()) DrawNight(IsViewportTurned(vp));
	_night_lit.clear();
	_night_glows.clear();
	_night_windows_now = false;

	DrawPixelInfo dp = _vd.dpi;
	ZoomLevel zoom = _vd.dpi.zoom;
	dp.zoom = ZoomLevel::Min;
	dp.width = UnScaleByZoom(dp.width, zoom);
	dp.height = UnScaleByZoom(dp.height, zoom);
	AutoRestoreBackup cur_dpi(_cur_dpi, &dp);

	if (vp.overlay != nullptr && vp.overlay->GetCargoMask().Any() && vp.overlay->GetCompanyMask().Any()) {
		/* translate to window coordinates */
		dp.left = x;
		dp.top = y;
		vp.overlay->Draw(&dp);
	}

	if (!_vd.string_sprites_to_draw.empty()) {
		/* translate to world coordinates */
		dp.left = UnScaleByZoom(_vd.dpi.left, zoom);
		dp.top = UnScaleByZoom(_vd.dpi.top, zoom);
		ViewportDrawStrings(zoom, &_vd.string_sprites_to_draw);
	}

	_vd.string_sprites_to_draw.clear();
	_vd.tile_sprites_to_draw.clear();
	_vd.parent_sprites_to_draw.clear();
	_vd.parent_sprites_to_sort.clear();
	_vd.child_screen_sprites_to_draw.clear();
}

static inline void ViewportDraw(const Viewport &vp, int left, int top, int right, int bottom)
{
	if (right <= vp.left || bottom <= vp.top) return;

	if (left >= vp.left + vp.width) return;

	if (left < vp.left) left = vp.left;
	if (right > vp.left + vp.width) right = vp.left + vp.width;

	if (top >= vp.top + vp.height) return;

	if (top < vp.top) top = vp.top;
	if (bottom > vp.top + vp.height) bottom = vp.top + vp.height;

	ViewportDoDraw(vp,
		ScaleByZoom(left - vp.left, vp.zoom) + vp.virtual_left,
		ScaleByZoom(top - vp.top, vp.zoom) + vp.virtual_top,
		ScaleByZoom(right - vp.left, vp.zoom) + vp.virtual_left,
		ScaleByZoom(bottom - vp.top, vp.zoom) + vp.virtual_top
	);
}

/**
 * Draw the viewport of this window.
 */
void Window::DrawViewport() const
{
	PerformanceAccumulator framerate(PerformanceElement::ViewportDrawing);

	DrawPixelInfo *dpi = _cur_dpi;

	dpi->left += this->left;
	dpi->top += this->top;

	ViewportDraw(*this->viewport, dpi->left, dpi->top, dpi->left + dpi->width, dpi->top + dpi->height);

	dpi->left -= this->left;
	dpi->top -= this->top;
}

/**
 * Ensure that a given viewport has a valid scroll position.
 *
 * There must be a visible piece of the map in the center of the viewport.
 * If there isn't, the viewport will be scrolled to nearest such location.
 *
 * @param vp The viewport.
 * @param[in,out] scroll_x Viewport X scroll.
 * @param[in,out] scroll_y Viewport Y scroll.
 */
static inline void ClampViewportToMap(const Viewport &vp, int *scroll_x, int *scroll_y)
{
	/* Centre of the viewport is hot spot. */
	Point pt = {
		*scroll_x + vp.virtual_width / 2,
		*scroll_y + vp.virtual_height / 2
	};

	/* Turned, the view may go on past the edge of the map into the void around it, THREE_D_VOID_TILES tiles
	 * beyond (the player: the black nothing must be bigger, for the camera to move there); the ground point
	 * under the middle of the screen is what is kept within that. Facing north the game clamps as it always did. */
	if (IsViewportTurned(vp)) {
		double wx, wy;
		InverseTurnedFlatF(pt.x, pt.y, 0, wx, wy);
		const double margin = THREE_D_VOID_TILES * TILE_SIZE;
		double cx = Clamp(wx, -margin, Map::SizeX() * TILE_SIZE + margin);
		double cy = Clamp(wy, -margin, Map::SizeY() * TILE_SIZE + margin);
		if (cx != wx || cy != wy) {
			double vx, vy;
			ProjectTurnedF(cx, cy, 0, vx, vy);
			*scroll_x = static_cast<int>(std::lround(vx)) - vp.virtual_width / 2;
			*scroll_y = static_cast<int>(std::lround(vy)) - vp.virtual_height / 2;
		}
		return;
	}

	/* Find nearest tile that is within borders of the map. */
	bool clamped;
	pt = InverseRemapCoords2(pt.x, pt.y, true, &clamped);

	if (clamped) {
		/* Convert back to viewport coordinates and remove centering. */
		pt = RemapCoords2(pt.x, pt.y);
		*scroll_x = pt.x - vp.virtual_width / 2;
		*scroll_y = pt.y - vp.virtual_height / 2;
	}
}

/**
 * Clamp the smooth scroll to a maximum speed and distance based on time elapsed.
 *
 * Every 30ms, we move 1/4th of the distance, to give a smooth movement experience.
 * But we never go over the max_scroll speed.
 *
 * @param delta_ms Time elapsed since last update.
 * @param delta_hi The distance to move in highest dimension (can't be zero).
 * @param delta_lo The distance to move in lowest dimension.
 * @param[out] delta_hi_clamped The clamped distance to move in highest dimension.
 * @param[out] delta_lo_clamped The clamped distance to move in lowest dimension.
 */
static void ClampSmoothScroll(uint32_t delta_ms, int64_t delta_hi, int64_t delta_lo, int &delta_hi_clamped, int &delta_lo_clamped)
{
	/** A tile is 64 pixels in width at 1x zoom; viewport coordinates are in 4x zoom. */
	constexpr int PIXELS_PER_TILE = TILE_PIXELS * 2 * ZOOM_BASE;

	assert(delta_hi != 0);

	/* Move at most 75% of the distance every 30ms, for a smooth experience */
	int64_t delta_left = delta_hi * std::pow(0.75, delta_ms / 30.0);
	/* Move never more than 16 tiles per 30ms. */
	int max_scroll = Map::ScaleBySize1D(16 * PIXELS_PER_TILE * delta_ms / 30);

	/* We never go over the max_scroll speed. */
	delta_hi_clamped = Clamp(delta_hi - delta_left, -max_scroll, max_scroll);
	/* The lower delta is in ratio of the higher delta, so we keep going straight at the destination. */
	delta_lo_clamped = delta_lo * delta_hi_clamped / delta_hi;

	/* Ensure we always move (delta_hi can't be zero). */
	if (delta_hi_clamped == 0) {
		delta_hi_clamped = delta_hi > 0 ? 1 : -1;
	}
}

/**
 * Update the viewport position being displayed.
 * @param w %Window owning the viewport.
 * @param delta_ms Milliseconds since the last update.
 */
void UpdateViewportPosition(Window *w, uint32_t delta_ms)
{
	ViewportData &vp = *w->viewport;

	if (vp.follow_vehicle != VehicleID::Invalid()) {
		const Vehicle *veh = Vehicle::Get(vp.follow_vehicle)->GetMovingFront();
		Point pt = MapXYZToViewport(vp, veh->x_pos, veh->y_pos, veh->z_pos);

		vp.scrollpos_x = pt.x;
		vp.scrollpos_y = pt.y;
		SetViewportPosition(w, pt.x, pt.y);
	} else {
		/* Ensure the destination location is within the map */
		ClampViewportToMap(vp, &vp.dest_scrollpos_x, &vp.dest_scrollpos_y);

		int delta_x = vp.dest_scrollpos_x - vp.scrollpos_x;
		int delta_y = vp.dest_scrollpos_y - vp.scrollpos_y;

		int current_x = vp.scrollpos_x;
		int current_y = vp.scrollpos_y;

		bool update_overlay = false;
		if (delta_x != 0 || delta_y != 0) {
			if (_settings_client.gui.smooth_scroll) {
				int delta_x_clamped;
				int delta_y_clamped;

				if (abs(delta_x) > abs(delta_y)) {
					ClampSmoothScroll(delta_ms, delta_x, delta_y, delta_x_clamped, delta_y_clamped);
				} else {
					ClampSmoothScroll(delta_ms, delta_y, delta_x, delta_y_clamped, delta_x_clamped);
				}

				vp.scrollpos_x += delta_x_clamped;
				vp.scrollpos_y += delta_y_clamped;
			} else {
				vp.scrollpos_x = vp.dest_scrollpos_x;
				vp.scrollpos_y = vp.dest_scrollpos_y;
			}
			update_overlay = (vp.scrollpos_x == vp.dest_scrollpos_x &&
								vp.scrollpos_y == vp.dest_scrollpos_y);
		}

		ClampViewportToMap(vp, &vp.scrollpos_x, &vp.scrollpos_y);

		/* When moving small amounts around the border we can get stuck, and
		 * not actually move. In those cases, teleport to the destination. */
		if ((delta_x != 0 || delta_y != 0) && current_x == vp.scrollpos_x && current_y == vp.scrollpos_y) {
			vp.scrollpos_x = vp.dest_scrollpos_x;
			vp.scrollpos_y = vp.dest_scrollpos_y;
		}

		SetViewportPosition(w, vp.scrollpos_x, vp.scrollpos_y);
		if (update_overlay) RebuildViewportOverlay(w);
	}
}

/**
 * Marks a viewport as dirty for repaint if it displays (a part of) the area the needs to be repainted.
 * @param vp     The viewport to mark as dirty
 * @param left   Left edge of area to repaint
 * @param top    Top edge of area to repaint
 * @param right  Right edge of area to repaint
 * @param bottom Bottom edge of area to repaint
 * @return true if the viewport contains a dirty block
 * @ingroup dirty
 */
static bool MarkViewportDirty(const Viewport &vp, int left, int top, int right, int bottom)
{
	/* Turned (three_d.h), the place is drawn elsewhere on the screen. */
	if (IsViewportTurned(vp)) TurnDirtyRect(left, top, right, bottom);

	/* Rounding wrt. zoom-out level */
	right += (1 << to_underlying(vp.zoom)) - 1;
	bottom += (1 << to_underlying(vp.zoom)) - 1;

	right -= vp.virtual_left;
	if (right <= 0) return false;

	bottom -= vp.virtual_top;
	if (bottom <= 0) return false;

	left = std::max(0, left - vp.virtual_left);

	if (left >= vp.virtual_width) return false;

	top = std::max(0, top - vp.virtual_top);

	if (top >= vp.virtual_height) return false;

	AddDirtyBlock(
		UnScaleByZoomLower(left, vp.zoom) + vp.left,
		UnScaleByZoomLower(top, vp.zoom) + vp.top,
		UnScaleByZoom(right, vp.zoom) + vp.left + 1,
		UnScaleByZoom(bottom, vp.zoom) + vp.top + 1
	);

	return true;
}

/**
 * Mark all viewports that display an area as dirty (in need of repaint).
 * @param left   Left   edge of area to repaint. (viewport coordinates, that is wrt. #ZoomLevel::Min)
 * @param top    Top    edge of area to repaint. (viewport coordinates, that is wrt. #ZoomLevel::Min)
 * @param right  Right  edge of area to repaint. (viewport coordinates, that is wrt. #ZoomLevel::Min)
 * @param bottom Bottom edge of area to repaint. (viewport coordinates, that is wrt. #ZoomLevel::Min)
 * @return true if at least one viewport has a dirty block
 * @ingroup dirty
 */
bool MarkAllViewportsDirty(int left, int top, int right, int bottom)
{
	bool dirty = false;

	for (const Window *w : Window::Iterate()) {
		if (w->viewport != nullptr) {
			assert(w->viewport->width != 0);
			if (MarkViewportDirty(*w->viewport, left, top, right, bottom)) dirty = true;
		}
	}

	return dirty;
}

void ConstrainAllViewportsZoom()
{
	for (Window *w : Window::Iterate()) {
		if (w->viewport == nullptr) continue;

		ZoomLevel zoom = Clamp(w->viewport->zoom, _settings_client.gui.zoom_min, _settings_client.gui.zoom_max);
		if (zoom != w->viewport->zoom) {
			while (w->viewport->zoom < zoom) DoZoomInOutWindow(ZOOM_OUT, w);
			while (w->viewport->zoom > zoom) DoZoomInOutWindow(ZOOM_IN, w);
		}
	}
}

/**
 * Mark a tile given by its index dirty for repaint.
 * @param tile The tile to mark dirty.
 * @param bridge_level_offset Height of bridge on tile to also mark dirty. (Height level relative to north corner.)
 * @param tile_height_override Height of the tile (#TileHeight).
 * @ingroup dirty
 */
void MarkTileDirtyByTile(TileIndex tile, int bridge_level_offset, int tile_height_override)
{
	/* Turned (three_d.h): the tile and what stands on it, up to the highest building and its bridge, and what
	 * reaches a little over the next tiles. */
	DirtyPlace place;
	place.kind = DirtyPlace::Kind::Box;
	place.x0 = static_cast<double>(TileX(tile) * TILE_SIZE) - TILE_SIZE / 2;
	place.x1 = static_cast<double>((TileX(tile) + 1) * TILE_SIZE) + TILE_SIZE / 2;
	place.y0 = static_cast<double>(TileY(tile) * TILE_SIZE) - TILE_SIZE / 2;
	place.y1 = static_cast<double>((TileY(tile) + 1) * TILE_SIZE) + TILE_SIZE / 2;
	place.z0 = static_cast<double>(tile_height_override * TILE_HEIGHT) - 2 * TILE_HEIGHT;
	place.z1 = static_cast<double>(tile_height_override * TILE_HEIGHT) + MAX_BUILDING_PIXELS + (bridge_level_offset + 2) * TILE_HEIGHT;
	DirtyPlaceScope scope(place);
	Point pt = RemapCoords(TileX(tile) * TILE_SIZE, TileY(tile) * TILE_SIZE, tile_height_override * TILE_HEIGHT);
	MarkAllViewportsDirty(
			pt.x - MAX_TILE_EXTENT_LEFT,
			pt.y - MAX_TILE_EXTENT_TOP - ZOOM_BASE * TILE_HEIGHT * bridge_level_offset,
			pt.x + MAX_TILE_EXTENT_RIGHT,
			pt.y + MAX_TILE_EXTENT_BOTTOM);
}

/**
 * Something drawn on the turned map only as high as it is (a figure, a car of the game's own) drawn again: the
 * tiles about it, between two heights -- not up to the highest building as a tile that changed
 * (MarkTileDirtyByTile()), which in the turned view is a tall stripe of the screen drawn again.
 * @param tile the tile it is on
 * @param z0,z1 the heights it is between, units
 * @param reach how many tiles about it too, 0 the tile alone
 * @ingroup dirty
 */
void MarkTileDirtyBetween(TileIndex tile, int z0, int z1, int reach)
{
	MarkWorldBoxDirty(static_cast<double>((static_cast<int>(TileX(tile)) - reach) * static_cast<int>(TILE_SIZE)) - TILE_SIZE / 2,
			static_cast<double>((static_cast<int>(TileY(tile)) - reach) * static_cast<int>(TILE_SIZE)) - TILE_SIZE / 2,
			static_cast<double>((static_cast<int>(TileX(tile)) + 1 + reach) * static_cast<int>(TILE_SIZE)) + TILE_SIZE / 2,
			static_cast<double>((static_cast<int>(TileY(tile)) + 1 + reach) * static_cast<int>(TILE_SIZE)) + TILE_SIZE / 2, z0, z1);
}

/**
 * A box of the world drawn again: what stands in it (a figure that moves) and nothing else.
 * @param x0,y0,x1,y1 the ground it is over, world units
 * @param z0,z1 the heights it is between, units
 * @ingroup dirty
 */
void MarkWorldBoxDirty(double x0, double y0, double x1, double y1, int z0, int z1)
{
	DirtyPlace place;
	place.kind = DirtyPlace::Kind::Box;
	place.x0 = x0;
	place.x1 = x1;
	place.y0 = y0;
	place.y1 = y1;
	place.z0 = z0;
	place.z1 = z1;
	DirtyPlaceScope scope(place);
	/* The rectangle of the box in the view facing north, for a view not turned. */
	int left = INT_MAX, top = INT_MAX, right = INT_MIN, bottom = INT_MIN;
	for (double x : {place.x0, place.x1}) {
		for (double y : {place.y0, place.y1}) {
			for (int z : {z0, z1}) {
				Point pt = RemapCoords(static_cast<int>(x), static_cast<int>(y), z);
				left = std::min(left, pt.x);
				right = std::max(right, pt.x);
				top = std::min(top, pt.y);
				bottom = std::max(bottom, pt.y);
			}
		}
	}
	MarkAllViewportsDirty(left, top, right, bottom);
}

/**
 * Marks the selected tiles as dirty.
 *
 * This function marks the selected tiles as dirty for repaint
 *
 * @ingroup dirty
 */
static void SetSelectionTilesDirty()
{
	int x_size = _thd.size.x;
	int y_size = _thd.size.y;

	if (!_thd.diagonal) { // Selecting in a straight rectangle (or a single square)
		int x_start = _thd.pos.x;
		int y_start = _thd.pos.y;

		if (_thd.outersize.x != 0) {
			x_size  += _thd.outersize.x;
			x_start += _thd.offs.x;
			y_size  += _thd.outersize.y;
			y_start += _thd.offs.y;
		}

		x_size -= TILE_SIZE;
		y_size -= TILE_SIZE;

		assert(x_size >= 0);
		assert(y_size >= 0);

		int x_end = Clamp(x_start + x_size, 0, Map::SizeX() * TILE_SIZE - TILE_SIZE);
		int y_end = Clamp(y_start + y_size, 0, Map::SizeY() * TILE_SIZE - TILE_SIZE);

		x_start = Clamp(x_start, 0, Map::SizeX() * TILE_SIZE - TILE_SIZE);
		y_start = Clamp(y_start, 0, Map::SizeY() * TILE_SIZE - TILE_SIZE);

		/* make sure everything is multiple of TILE_SIZE */
		assert((x_end | y_end | x_start | y_start) % TILE_SIZE == 0);

		/* How it works:
		 * Suppose we have to mark dirty rectangle of 3x4 tiles:
		 *   x
		 *  xxx
		 * xxxxx
		 *  xxxxx
		 *   xxx
		 *    x
		 * This algorithm marks dirty columns of tiles, so it is done in 3+4-1 steps:
		 * 1)  x     2)  x
		 *    xxx       Oxx
		 *   Oxxxx     xOxxx
		 *    xxxxx     Oxxxx
		 *     xxx       xxx
		 *      x         x
		 * And so forth...
		 */

		int top_x = x_end; // coordinates of top dirty tile
		int top_y = y_start;
		int bot_x = top_x; // coordinates of bottom dirty tile
		int bot_y = top_y;

		do {
			/* topmost dirty point */
			TileIndex top_tile = TileVirtXY(top_x, top_y);
			Point top = RemapCoords(top_x, top_y, GetTileMaxPixelZ(top_tile));

			/* bottommost point */
			TileIndex bottom_tile = TileVirtXY(bot_x, bot_y);
			Point bot = RemapCoords(bot_x + TILE_SIZE, bot_y + TILE_SIZE, GetTilePixelZ(bottom_tile)); // bottommost point

			/* the 'x' coordinate of 'top' and 'bot' is the same (and always in the same distance from tile middle),
			 * tile height/slope affects only the 'y' on-screen coordinate! */

			int l = top.x - TILE_PIXELS * ZOOM_BASE; // 'x' coordinate of left   side of the dirty rectangle
			int t = top.y;                               // 'y' coordinate of top    side of the dirty rectangle
			int r = top.x + TILE_PIXELS * ZOOM_BASE; // 'x' coordinate of right  side of the dirty rectangle
			int b = bot.y;                               // 'y' coordinate of bottom side of the dirty rectangle

			static const int OVERLAY_WIDTH = 4 * ZOOM_BASE; // part of selection sprites is drawn outside the selected area (in particular: terraforming)

			/* For halftile foundations on SLOPE_STEEP_S the sprite extents some more towards the top */
			MarkAllViewportsDirty(l - OVERLAY_WIDTH, t - OVERLAY_WIDTH - TILE_HEIGHT * ZOOM_BASE, r + OVERLAY_WIDTH, b + OVERLAY_WIDTH);

			/* haven't we reached the topmost tile yet? */
			if (top_x != x_start) {
				top_x -= TILE_SIZE;
			} else {
				top_y += TILE_SIZE;
			}

			/* the way the bottom tile changes is different when we reach the bottommost tile */
			if (bot_y != y_end) {
				bot_y += TILE_SIZE;
			} else {
				bot_x -= TILE_SIZE;
			}
		} while (bot_x >= top_x);
	} else { // Selecting in a 45 degrees rotated (diagonal) rectangle.
		/* a_size, b_size describe a rectangle with rotated coordinates */
		int a_size = x_size + y_size, b_size = x_size - y_size;

		int interval_a = a_size < 0 ? -(int)TILE_SIZE : (int)TILE_SIZE;
		int interval_b = b_size < 0 ? -(int)TILE_SIZE : (int)TILE_SIZE;

		for (int a = -interval_a; a != a_size + interval_a; a += interval_a) {
			for (int b = -interval_b; b != b_size + interval_b; b += interval_b) {
				uint x = (_thd.pos.x + (a + b) / 2) / TILE_SIZE;
				uint y = (_thd.pos.y + (a - b) / 2) / TILE_SIZE;

				if (x < Map::MaxX() && y < Map::MaxY()) {
					MarkTileDirtyByTile(TileXY(x, y));
				}
			}
		}
	}
}


void SetSelectionRed(bool b)
{
	_thd.make_square_red = b;
	SetSelectionTilesDirty();
}

/**
 * Test whether a sign is below the mouse
 * @param vp the clicked viewport
 * @param x X position of click
 * @param y Y position of click
 * @param sign the sign to check
 * @return true if the sign was hit
 */
static bool CheckClickOnViewportSign(const Viewport &vp, int x, int y, const ViewportSign *sign)
{
	bool small = (vp.zoom >= ZoomLevel::Out4x);
	int sign_half_width = ScaleByZoom((small ? sign->width_small : sign->width_normal) / 2, vp.zoom);
	int sign_height = ScaleByZoom(WidgetDimensions::scaled.fullbevel.top + GetCharacterHeight(small ? FontSize::Small : FontSize::Normal) + WidgetDimensions::scaled.fullbevel.bottom, vp.zoom);

	return y >= sign->top && y < sign->top + sign_height &&
			x >= sign->center - sign_half_width && x < sign->center + sign_half_width;
}


/**
 * Check whether any viewport sign was clicked, and dispatch the click.
 * @param vp the clicked viewport
 * @param x X position of click
 * @param y Y position of click
 * @return true if the sign was hit
 */
static bool CheckClickOnViewportSign(const Viewport &vp, int x, int y)
{
	if (_game_mode == GameMode::Menu) return false;

	x = ScaleByZoom(x - vp.left, vp.zoom) + vp.virtual_left;
	y = ScaleByZoom(y - vp.top, vp.zoom) + vp.virtual_top;

	Rect search_rect{ x - 1, y - 1, x + 1, y + 1 };
	search_rect = ExpandRectWithViewportSignMargins(search_rect, vp.zoom);

	bool show_stations = _display_opt.Test(DisplayOption::ShowStationNames) && !IsInvisibilitySet(TransparencyOption::Signs);
	bool show_waypoints = _display_opt.Test(DisplayOption::ShowWaypointNames) && !IsInvisibilitySet(TransparencyOption::Signs);
	bool show_towns = _display_opt.Test(DisplayOption::ShowTownNames);
	bool show_signs = _display_opt.Test(DisplayOption::ShowSigns) && !IsInvisibilitySet(TransparencyOption::Signs);
	bool show_competitors = _display_opt.Test(DisplayOption::ShowCompetitorSigns);

	/* Topmost of each type that was hit */
	BaseStation *st = nullptr, *last_st = nullptr;
	Town *t = nullptr, *last_t = nullptr;
	Sign *si = nullptr, *last_si = nullptr;

	/* See ViewportAddKdtreeSigns() for details on the search logic */
	_viewport_sign_kdtree.FindContained(search_rect.left, search_rect.top, search_rect.right, search_rect.bottom, [&](const ViewportSignKdtreeItem & item) {
		switch (item.type) {
			case ViewportSignKdtreeItem::VKI_STATION: {
				if (!show_stations) break;
				st = BaseStation::Get(std::get<StationID>(item.id));
				if (!show_competitors && _local_company != st->owner && st->owner != OWNER_NONE) break;

				StationFacilities facilities = st->facilities;
				if (facilities.None()) facilities = STATION_FACILITY_GHOST;
				if (!facilities.Any(_facility_display_opt)) break;

				if (CheckClickOnViewportSign(vp, x, y, &st->sign)) last_st = st;
				break;
			}

			case ViewportSignKdtreeItem::VKI_WAYPOINT:
				if (!show_waypoints) break;
				st = BaseStation::Get(std::get<StationID>(item.id));
				if (!show_competitors && _local_company != st->owner && st->owner != OWNER_NONE) break;
				if (CheckClickOnViewportSign(vp, x, y, &st->sign)) last_st = st;
				break;

			case ViewportSignKdtreeItem::VKI_TOWN:
				if (!show_towns) break;
				t = Town::Get(std::get<TownID>(item.id));
				if (CheckClickOnViewportSign(vp, x, y, &t->cache.sign)) last_t = t;
				break;

			case ViewportSignKdtreeItem::VKI_SIGN:
				if (!show_signs) break;
				si = Sign::Get(std::get<SignID>(item.id));
				if (!show_competitors && _local_company != si->owner && si->owner != OWNER_DEITY) break;
				if (CheckClickOnViewportSign(vp, x, y, &si->sign)) last_si = si;
				break;

			default:
				NOT_REACHED();
		}
	});

	/* Select which hit to handle based on priority */
	if (last_st != nullptr) {
		if (Station::IsExpected(last_st)) {
			ShowStationViewWindow(last_st->index);
		} else {
			ShowWaypointWindow(Waypoint::From(last_st));
		}
		return true;
	} else if (last_t != nullptr) {
		ShowTownViewWindow(last_t->index);
		return true;
	} else if (last_si != nullptr) {
		HandleClickOnSign(last_si);
		return true;
	} else {
		return false;
	}
}


ViewportSignKdtreeItem ViewportSignKdtreeItem::MakeStation(StationID id)
{
	ViewportSignKdtreeItem item;
	item.type = VKI_STATION;
	item.id = id;

	const Station *st = Station::Get(id);
	assert(st->sign.kdtree_valid);
	item.center = st->sign.center;
	item.top = st->sign.top;

	/* Assume the sign can be a candidate for drawing, so measure its width */
	_viewport_sign_maxwidth = std::max<int>({_viewport_sign_maxwidth, st->sign.width_normal, st->sign.width_small});

	return item;
}

ViewportSignKdtreeItem ViewportSignKdtreeItem::MakeWaypoint(StationID id)
{
	ViewportSignKdtreeItem item;
	item.type = VKI_WAYPOINT;
	item.id = id;

	const Waypoint *st = Waypoint::Get(id);
	assert(st->sign.kdtree_valid);
	item.center = st->sign.center;
	item.top = st->sign.top;

	/* Assume the sign can be a candidate for drawing, so measure its width */
	_viewport_sign_maxwidth = std::max<int>({_viewport_sign_maxwidth, st->sign.width_normal, st->sign.width_small});

	return item;
}

ViewportSignKdtreeItem ViewportSignKdtreeItem::MakeTown(TownID id)
{
	ViewportSignKdtreeItem item;
	item.type = VKI_TOWN;
	item.id = id;

	const Town *town = Town::Get(id);
	assert(town->cache.sign.kdtree_valid);
	item.center = town->cache.sign.center;
	item.top = town->cache.sign.top;

	/* Assume the sign can be a candidate for drawing, so measure its width */
	_viewport_sign_maxwidth = std::max<int>({_viewport_sign_maxwidth, town->cache.sign.width_normal, town->cache.sign.width_small});

	return item;
}

ViewportSignKdtreeItem ViewportSignKdtreeItem::MakeSign(SignID id)
{
	ViewportSignKdtreeItem item;
	item.type = VKI_SIGN;
	item.id = id;

	const Sign *sign = Sign::Get(id);
	assert(sign->sign.kdtree_valid);
	item.center = sign->sign.center;
	item.top = sign->sign.top;

	/* Assume the sign can be a candidate for drawing, so measure its width */
	_viewport_sign_maxwidth = std::max<int>({_viewport_sign_maxwidth, sign->sign.width_normal, sign->sign.width_small});

	return item;
}

void RebuildViewportKdtree()
{
	/* Reset biggest size sign seen */
	_viewport_sign_maxwidth = 0;

	std::vector<ViewportSignKdtreeItem> items;
	items.reserve(BaseStation::GetNumItems() + Town::GetNumItems() + Sign::GetNumItems());

	for (const Station *st : Station::Iterate()) {
		if (st->sign.kdtree_valid) items.push_back(ViewportSignKdtreeItem::MakeStation(st->index));
	}

	for (const Waypoint *wp : Waypoint::Iterate()) {
		if (wp->sign.kdtree_valid) items.push_back(ViewportSignKdtreeItem::MakeWaypoint(wp->index));
	}

	for (const Town *town : Town::Iterate()) {
		if (town->cache.sign.kdtree_valid) items.push_back(ViewportSignKdtreeItem::MakeTown(town->index));
	}

	for (const Sign *sign : Sign::Iterate()) {
		if (sign->sign.kdtree_valid) items.push_back(ViewportSignKdtreeItem::MakeSign(sign->index));
	}

	_viewport_sign_kdtree.Build(items.begin(), items.end());
}


static bool CheckClickOnLandscape(const Viewport &vp, int x, int y)
{
	Point pt = TranslateXYToTileCoord(vp, x, y);

	if (pt.x != -1) return ClickTile(TileVirtXY(pt.x, pt.y));
	return true;
}

static void PlaceObject()
{
	Point pt;
	Window *w;

	pt = GetTileBelowCursor();
	if (pt.x == -1) return;

	if ((_thd.place_mode & HT_DRAG_MASK) == HT_POINT) {
		pt.x += TILE_SIZE / 2;
		pt.y += TILE_SIZE / 2;
	}

	/* Build on the tile the white marker was drawn on, not on whatever tile
	 * the cursor happens to be over at this instant.
	 *
	 * The two are worked out the same way from the same cursor position, so
	 * they agree -- as long as the cursor has not moved. It moves constantly:
	 * the marker is drawn once a frame, and by the time the press that follows
	 * that frame is dealt with, the pointer has been read again. Pressing is
	 * itself what moves it on a touch screen, where the contact patch shifts
	 * as the finger comes down and lifts again. Near the middle of a tile that
	 * is invisible; near an edge it lands a tile away, and the player sees the
	 * game build somewhere other than where it said it would.
	 *
	 * So the answer does not come from asking where the pointer is now. It
	 * comes from the marker the player was actually looking at when they
	 * pressed, which is the one from before this frame moved anything. The
	 * sub-tile position is kept, since tools that care which half of a tile
	 * was clicked still need it. */
	if ((_thd_drawn.drawstyle & HT_DRAG_MASK) != HT_NONE) {
		pt.x = _thd_drawn.pos.x + (pt.x & TILE_UNIT_MASK);
		pt.y = _thd_drawn.pos.y + (pt.y & TILE_UNIT_MASK);
	}

	_tile_fract_coords.x = pt.x & TILE_UNIT_MASK;
	_tile_fract_coords.y = pt.y & TILE_UNIT_MASK;

	w = _thd.GetCallbackWnd();
	if (w != nullptr) w->OnPlaceObject(pt, TileVirtXY(pt.x, pt.y));
}


bool HandleViewportClicked(const Viewport &vp, int x, int y)
{
	const Vehicle *v = CheckClickOnVehicle(vp, x, y);

	if (_thd.place_mode & HT_VEHICLE) {
		if (v != nullptr && VehicleClicked(v)) return true;
	}

	/* Vehicle placement mode already handled above. */
	if ((_thd.place_mode & HT_DRAG_MASK) != HT_NONE) {
		PlaceObject();
		return true;
	}

	if (CheckClickOnViewportSign(vp, x, y)) return true;
	bool result = CheckClickOnLandscape(vp, x, y);

	if (v != nullptr) {
		Debug(misc, 2, "Vehicle {} (index {}) at {}", v->unitnumber, v->index, fmt::ptr(v));
		if (IsCompanyBuildableVehicleType(v)) {
			v = v->First();
			if (_ctrl_pressed && v->owner == _local_company) {
				StartStopVehicle(v, true);
			} else {
				ShowVehicleViewWindow(v);
			}
		}
		return true;
	}
	return result;
}

void RebuildViewportOverlay(Window *w)
{
	if (w->viewport->overlay != nullptr &&
			w->viewport->overlay->GetCompanyMask().Any() &&
			w->viewport->overlay->GetCargoMask().Any()) {
		w->viewport->overlay->SetDirty();
		w->SetDirty();
	}
}

/**
 * Scrolls the viewport in a window to a given location.
 * @param x       Desired x location of the map to scroll to (world coordinate).
 * @param y       Desired y location of the map to scroll to (world coordinate).
 * @param z       Desired z location of the map to scroll to (world coordinate). Use \c -1 to scroll to the height of the map at the \a x, \a y location.
 * @param w       %Window containing the viewport.
 * @param instant Jump to the location instead of slowly moving to it.
 * @return Destination of the viewport was changed (to activate other actions when the viewport is already at the desired position).
 */
bool ScrollWindowTo(int x, int y, int z, Window *w, bool instant)
{
	/* The slope cannot be acquired outside of the map, so make sure we are always within the map. */
	if (z == -1) {
		if ( x >= 0 && x <= (int)Map::SizeX() * (int)TILE_SIZE - 1
				&& y >= 0 && y <= (int)Map::SizeY() * (int)TILE_SIZE - 1) {
			z = GetSlopePixelZ(x, y);
		} else {
			z = TileHeightOutsideMap(x / (int)TILE_SIZE, y / (int)TILE_SIZE);
		}
	}

	Point pt = MapXYZToViewport(*w->viewport, x, y, z);
	w->viewport->CancelFollow(*w);

	if (w->viewport->dest_scrollpos_x == pt.x && w->viewport->dest_scrollpos_y == pt.y) return false;

	if (instant) {
		w->viewport->scrollpos_x = pt.x;
		w->viewport->scrollpos_y = pt.y;
		RebuildViewportOverlay(w);
	}

	w->viewport->dest_scrollpos_x = pt.x;
	w->viewport->dest_scrollpos_y = pt.y;
	return true;
}

/**
 * Scrolls the viewport in a window to a given location.
 * @param tile    Desired tile to center on.
 * @param w       %Window containing the viewport.
 * @param instant Jump to the location instead of slowly moving to it.
 * @return Destination of the viewport was changed (to activate other actions when the viewport is already at the desired position).
 */
bool ScrollWindowToTile(TileIndex tile, Window *w, bool instant)
{
	return ScrollWindowTo(TileX(tile) * TILE_SIZE, TileY(tile) * TILE_SIZE, -1, w, instant);
}

/**
 * Scrolls the viewport of the main window to a given location.
 * @param tile    Desired tile to center on.
 * @param instant Jump to the location instead of slowly moving to it.
 * @return Destination of the viewport was changed (to activate other actions when the viewport is already at the desired position).
 */
bool ScrollMainWindowToTile(TileIndex tile, bool instant)
{
	return ScrollMainWindowTo(TileX(tile) * TILE_SIZE + TILE_SIZE / 2, TileY(tile) * TILE_SIZE + TILE_SIZE / 2, -1, instant);
}

/**
 * Set a tile to display a red error square.
 * @param tile Tile that should show the red error square.
 */
void SetRedErrorSquare(TileIndex tile)
{
	TileIndex old;

	old = _thd.redsq;
	_thd.redsq = tile;

	if (tile != old) {
		if (tile != INVALID_TILE) MarkTileDirtyByTile(tile);
		if (old  != INVALID_TILE) MarkTileDirtyByTile(old);
	}
}

/**
 * Highlight \a w by \a h tiles at the cursor.
 * @param w Width of the highlighted tiles rectangle.
 * @param h Height of the highlighted tiles rectangle.
 */
void SetTileSelectSize(int w, int h)
{
	_thd.new_size.x = w * TILE_SIZE;
	_thd.new_size.y = h * TILE_SIZE;
	_thd.new_outersize.x = 0;
	_thd.new_outersize.y = 0;
}

void SetTileSelectBigSize(int ox, int oy, int sx, int sy)
{
	_thd.offs.x = ox * TILE_SIZE;
	_thd.offs.y = oy * TILE_SIZE;
	_thd.new_outersize.x = sx * TILE_SIZE;
	_thd.new_outersize.y = sy * TILE_SIZE;
}

/**
 * Determine the best autorail highlight type from map coordinates.
 * @param x The map X-coordinate.
 * @param y The map Y-coordinate.
 * @return The best highlight style.
 */
static HighLightStyle GetAutorailHT(int x, int y)
{
	return HT_RAIL | _autorail_piece[x & TILE_UNIT_MASK][y & TILE_UNIT_MASK];
}

/**
 * Reset tile highlighting.
 */
void TileHighlightData::Reset()
{
	this->pos.x = 0;
	this->pos.y = 0;
	this->new_pos.x = 0;
	this->new_pos.y = 0;
	this->press_drawstyle = HT_NONE;
}

/**
 * Is the user dragging a 'diagonal rectangle'?
 * @return User is dragging a rotated rectangle.
 */
bool TileHighlightData::IsDraggingDiagonal()
{
	return (this->place_mode & HT_DIAGONAL) != 0 && _ctrl_pressed && _left_button_down;
}

/**
 * Get the window that started the current highlighting.
 * @return The window that requested the current tile highlighting, or \c nullptr if not available.
 */
Window *TileHighlightData::GetCallbackWnd()
{
	return FindWindowById(this->window_class, this->window_number);
}



/**
 * Updates tile highlighting for all cases.
 * Uses _thd.selstart and _thd.selend and _thd.place_mode (set elsewhere) to determine _thd.pos and _thd.size
 * Also drawstyle is determined. Uses _thd.new.* as a buffer and calls SetSelectionTilesDirty() twice,
 * Once for the old and once for the new selection.
 * _thd is TileHighlightData, found in viewport.h
 */
void UpdateTileSelection()
{
	int x1;
	int y1;

	/* Keep what the marker looked like before this frame moves it. That is the
	 * marker the player is looking at when they press, because the frame they
	 * are looking at was drawn from it -- and this runs before a press is
	 * dealt with, so from here on #_thd describes a marker nobody has seen
	 * yet. Anything a click builds is built from this copy. */
	_thd_drawn = _thd;

	if (_thd.freeze) return;

	HighLightStyle new_drawstyle = HT_NONE;
	bool new_diagonal = false;

	if ((_thd.place_mode & HT_DRAG_MASK) == HT_SPECIAL) {
		x1 = _thd.selend.x;
		y1 = _thd.selend.y;
		if (x1 != -1) {
			int x2 = _thd.selstart.x & ~TILE_UNIT_MASK;
			int y2 = _thd.selstart.y & ~TILE_UNIT_MASK;
			x1 &= ~TILE_UNIT_MASK;
			y1 &= ~TILE_UNIT_MASK;

			if (_thd.IsDraggingDiagonal()) {
				new_diagonal = true;
			} else {
				if (x1 >= x2) std::swap(x1, x2);
				if (y1 >= y2) std::swap(y1, y2);
			}
			_thd.new_pos.x = x1;
			_thd.new_pos.y = y1;
			_thd.new_size.x = x2 - x1;
			_thd.new_size.y = y2 - y1;
			if (!new_diagonal) {
				_thd.new_size.x += TILE_SIZE;
				_thd.new_size.y += TILE_SIZE;
			}
			new_drawstyle = _thd.next_drawstyle;
		}
	} else if ((_thd.place_mode & HT_DRAG_MASK) != HT_NONE) {
		Point pt = GetTileBelowCursor();
		x1 = pt.x;
		y1 = pt.y;
		if (x1 != -1) {
			switch (_thd.place_mode & HT_DRAG_MASK) {
				case HT_RECT:
					new_drawstyle = HT_RECT;
					break;
				case HT_POINT:
					new_drawstyle = HT_POINT;
					x1 += TILE_SIZE / 2;
					y1 += TILE_SIZE / 2;
					break;
				case HT_RAIL:
					/* Draw one highlighted tile in any direction */
					new_drawstyle = GetAutorailHT(pt.x, pt.y);
					break;
				case HT_LINE:
					switch (_thd.place_mode & HT_DIR_MASK) {
						case HT_DIR_X: new_drawstyle = HT_LINE | HT_DIR_X; break;
						case HT_DIR_Y: new_drawstyle = HT_LINE | HT_DIR_Y; break;

						case HT_DIR_HU:
						case HT_DIR_HL:
							new_drawstyle = (pt.x & TILE_UNIT_MASK) + (pt.y & TILE_UNIT_MASK) <= TILE_SIZE ? HT_LINE | HT_DIR_HU : HT_LINE | HT_DIR_HL;
							break;

						case HT_DIR_VL:
						case HT_DIR_VR:
							new_drawstyle = (pt.x & TILE_UNIT_MASK) > (pt.y & TILE_UNIT_MASK) ? HT_LINE | HT_DIR_VL : HT_LINE | HT_DIR_VR;
							break;

						default: NOT_REACHED();
					}
					_thd.selstart.x = x1 & ~TILE_UNIT_MASK;
					_thd.selstart.y = y1 & ~TILE_UNIT_MASK;
					break;
				default:
					NOT_REACHED();
			}
			_thd.new_pos.x = x1 & ~TILE_UNIT_MASK;
			_thd.new_pos.y = y1 & ~TILE_UNIT_MASK;
		}
	}

	/* redraw selection */
	if (_thd.drawstyle != new_drawstyle ||
			_thd.pos.x != _thd.new_pos.x || _thd.pos.y != _thd.new_pos.y ||
			_thd.size.x != _thd.new_size.x || _thd.size.y != _thd.new_size.y ||
			_thd.outersize.x != _thd.new_outersize.x ||
			_thd.outersize.y != _thd.new_outersize.y ||
			_thd.diagonal    != new_diagonal) {
		/* Clear the old tile selection? */
		if ((_thd.drawstyle & HT_DRAG_MASK) != HT_NONE) SetSelectionTilesDirty();

		_thd.drawstyle = new_drawstyle;
		_thd.pos = _thd.new_pos;
		_thd.size = _thd.new_size;
		_thd.outersize = _thd.new_outersize;
		_thd.diagonal = new_diagonal;
		_thd.dirty = 0xff;

		/* Draw the new tile selection? */
		if ((new_drawstyle & HT_DRAG_MASK) != HT_NONE) SetSelectionTilesDirty();
	}
}

/**
 * Displays the measurement tooltips when selecting multiple tiles
 * @param text String to be displayed
 */
static inline void ShowMeasurementTooltips(EncodedString &&text)
{
	if (!_settings_client.gui.measure_tooltip) return;
	GuiShowTooltips(_thd.GetCallbackWnd(), std::move(text), TooltipCloseCondition::ExitViewport);
}

static void HideMeasurementTooltips()
{
	CloseWindowById(WindowClass::ToolTips, 0);
}

/**
 * Prepare state for highlighting tiles while dragging with the mouse.
 * @param tile The start tile.
 * @param method The method/limits for what area can be selected.
 * @param process Unique identifier of the thing to do after the dragging is done.
 */
void VpStartPlaceSizing(TileIndex tile, ViewportPlaceMethod method, ViewportDragDropSelectionProcess process)
{
	_thd.select_method = method;
	_thd.select_proc   = process;
	_thd.selend.x = TileX(tile) * TILE_SIZE;
	_thd.selstart.x = TileX(tile) * TILE_SIZE;
	_thd.selend.y = TileY(tile) * TILE_SIZE;
	_thd.selstart.y = TileY(tile) * TILE_SIZE;

	/* Needed so several things (road, autoroad, bridges, ...) are placed correctly.
	 * In effect, placement starts from the centre of a tile
	 */
	if (method == VPM_X_OR_Y || method == VPM_FIX_X || method == VPM_FIX_Y) {
		_thd.selend.x += TILE_SIZE / 2;
		_thd.selend.y += TILE_SIZE / 2;
		_thd.selstart.x += TILE_SIZE / 2;
		_thd.selstart.y += TILE_SIZE / 2;
	}

	HighLightStyle others = _thd.place_mode & ~(HT_DRAG_MASK | HT_DIR_MASK);
	if ((_thd.place_mode & HT_DRAG_MASK) == HT_RECT) {
		_thd.place_mode = HT_SPECIAL | others;
		_thd.next_drawstyle = HT_RECT | others;
	} else if (_thd.place_mode & (HT_RAIL | HT_LINE)) {
		_thd.place_mode = HT_SPECIAL | others;
		/* Which piece of track this is going to be was decided when the marker
		 * was drawn, so take it from the marker that was drawn rather than
		 * working it out again from a pointer that has moved since. The one
		 * the player pressed on is the one they were looking at. Falling back
		 * to the current marker covers the first frame of a newly picked tool,
		 * when there is not yet a drawn one to take it from. */
		HighLightStyle drawn = (_thd_drawn.drawstyle & HT_DRAG_MASK) != HT_NONE ? _thd_drawn.drawstyle : _thd.drawstyle;
		_thd.next_drawstyle = drawn | others;
		/* And hold on to it, so a drag that never leaves this tile cannot
		 * decide otherwise later. See TileHighlightData::press_drawstyle. */
		_thd.press_drawstyle = _thd.next_drawstyle;
	} else {
		_thd.place_mode = HT_SPECIAL | others;
		_thd.next_drawstyle = HT_POINT | others;
	}
	_special_mouse_mode = SpecialMouseMode::Sizing;
}

/**
 * Drag over the map while holding the left mouse down.
 * @param process The chosen selection process.
 */
void VpStartDragging(ViewportDragDropSelectionProcess process)
{
	_thd.select_method = VPM_X_AND_Y;
	_thd.select_proc = process;
	_thd.selstart.x = 0;
	_thd.selstart.y = 0;
	_thd.next_drawstyle = HT_RECT;

	_special_mouse_mode = SpecialMouseMode::Dragging;
}

void VpSetPlaceSizingLimit(int limit)
{
	_thd.sizelimit = limit;
}

/**
 * Highlights all tiles between a set of two tiles. Used in dock and tunnel placement
 * @param from TileIndex of the first tile to highlight
 * @param to TileIndex of the last tile to highlight
 */
void VpSetPresizeRange(TileIndex from, TileIndex to)
{
	uint64_t distance = DistanceManhattan(from, to) + 1;

	_thd.selend.x = TileX(to) * TILE_SIZE;
	_thd.selend.y = TileY(to) * TILE_SIZE;
	_thd.selstart.x = TileX(from) * TILE_SIZE;
	_thd.selstart.y = TileY(from) * TILE_SIZE;
	_thd.next_drawstyle = HT_RECT;

	/* show measurement only if there is any length to speak of */
	if (distance > 1) {
		ShowMeasurementTooltips(GetEncodedString(STR_MEASURE_LENGTH, distance));
	} else {
		HideMeasurementTooltips();
	}
}

static void VpStartPreSizing()
{
	_thd.selend.x = -1;
	_special_mouse_mode = SpecialMouseMode::Presize;
}

/**
 * What would be the highlight style when trying to build a 2 tile long piece of rail.
 * @param direction The rough direction the drag has been made in.
 * @return The highlight style of the first tile.
 * @note Depending on where on the start tile the click was, and some hysterasis, the
 *       direction for dragging to the east could be either DiagDirection::NE or DiagDirection::SE.
 */
static HighLightStyle Check2x1AutoRail(DiagDirection direction)
{
	int fxpy = _tile_fract_coords.x + _tile_fract_coords.y;
	int sxpy = (_thd.selend.x & TILE_UNIT_MASK) + (_thd.selend.y & TILE_UNIT_MASK);
	int fxmy = _tile_fract_coords.x - _tile_fract_coords.y;
	int sxmy = (_thd.selend.x & TILE_UNIT_MASK) - (_thd.selend.y & TILE_UNIT_MASK);

	switch (direction) {
		default: NOT_REACHED();
		case DiagDirection::SE: // end piece is lower right
			if (fxpy >= 20 && sxpy <= 12) return HT_DIR_HL;
			if (fxmy < -3 && sxmy > 3) return HT_DIR_VR;
			return HT_DIR_Y;

		case DiagDirection::NW:
			if (fxmy > 3 && sxmy < -3) return HT_DIR_VL;
			if (fxpy <= 12 && sxpy >= 20) return HT_DIR_HU;
			return HT_DIR_Y;

		case DiagDirection::SW:
			if (fxmy > 3 && sxmy < -3) return HT_DIR_VL;
			if (fxpy >= 20 && sxpy <= 12) return HT_DIR_HL;
			return HT_DIR_X;

		case DiagDirection::NE:
			if (fxmy < -3 && sxmy > 3) return HT_DIR_VR;
			if (fxpy <= 12 && sxpy >= 20) return HT_DIR_HU;
			return HT_DIR_X;
	}
}

/**
 * Check if the direction of start and end tile should be swapped based on
 * the dragging-style. Default directions are:
 * in the case of a line (HT_RAIL, HT_LINE):  Direction::NE, Direction::NW, Direction::N, Direction::E
 * in the case of a rect (HT_RECT, HT_POINT): Direction::S, Direction::E
 * For example dragging a rectangle area from south to north should be swapped to
 * north-south (Direction::S) to obtain the same results with less code. This is what
 * the return value signifies.
 * @param style HighLightStyle dragging style
 * @param start_tile start tile of drag
 * @param end_tile end tile of drag
 * @return boolean value which when true means start/end should be swapped
 */
static bool SwapDirection(HighLightStyle style, TileIndex start_tile, TileIndex end_tile)
{
	uint start_x = TileX(start_tile);
	uint start_y = TileY(start_tile);
	uint end_x = TileX(end_tile);
	uint end_y = TileY(end_tile);

	switch (style & HT_DRAG_MASK) {
		case HT_RAIL:
		case HT_LINE: return (end_x > start_x || (end_x == start_x && end_y > start_y));

		case HT_RECT:
		case HT_POINT: return (end_x != start_x && end_y < start_y);
		default: NOT_REACHED();
	}

	return false;
}

/**
 * Calculates height difference between one tile and another.
 * Multiplies the result to suit the standard given by #TILE_HEIGHT_STEP.
 *
 * To correctly get the height difference we need the direction we are dragging
 * in, as well as with what kind of tool we are dragging. For example a horizontal
 * autorail tool that starts in bottom and ends at the top of a tile will need the
 * maximum of SW, S and SE, N corners respectively. This is handled by the lookup table below
 * See #_tileoffs_by_dir in map.cpp for the direction enums if you can't figure out the values yourself.
 * @param style      Highlighting style of the drag. This includes direction and style (autorail, rect, etc.)
 * @param distance   Number of tiles dragged, important for horizontal/vertical drags, ignored for others.
 * @param start_tile Start tile of the drag operation.
 * @param end_tile   End tile of the drag operation.
 * @return Height difference between two tiles. The tile measurement tool utilizes this value in its tooltip.
 */
static int CalcHeightdiff(HighLightStyle style, uint distance, TileIndex start_tile, TileIndex end_tile)
{
	bool swap = SwapDirection(style, start_tile, end_tile);
	uint h0, h1; // Start height and end height.

	if (start_tile == end_tile) return 0;
	if (swap) std::swap(start_tile, end_tile);

	switch (style & HT_DRAG_MASK) {
		case HT_RECT:
			/* In the case of an area we can determine whether we were dragging south or
			 * east by checking the X-coordinates of the tiles */
			if (TileX(end_tile) > TileX(start_tile)) {
				/* Dragging south does not need to change the start tile. */
				end_tile = TileAddByDir(end_tile, Direction::S);
			} else {
				/* Dragging east. */
				start_tile = TileAddByDir(start_tile, Direction::SW);
				end_tile = TileAddByDir(end_tile, Direction::SE);
			}
			[[fallthrough]];

		case HT_POINT:
			h0 = TileHeight(start_tile);
			h1 = TileHeight(end_tile);
			break;
		default: { // All other types, this is mostly only line/autorail
			static const HighLightStyle flip_style_direction[] = {
				HT_DIR_X, HT_DIR_Y, HT_DIR_HL, HT_DIR_HU, HT_DIR_VR, HT_DIR_VL
			};
			static const std::pair<TileIndexDiffC, TileIndexDiffC> start_heightdiff_line_by_dir[] = {
				{ {1, 0}, {1, 1} }, // HT_DIR_X
				{ {0, 1}, {1, 1} }, // HT_DIR_Y
				{ {1, 0}, {0, 0} }, // HT_DIR_HU
				{ {1, 0}, {1, 1} }, // HT_DIR_HL
				{ {1, 0}, {1, 1} }, // HT_DIR_VL
				{ {0, 1}, {1, 1} }, // HT_DIR_VR
			};
			static const std::pair<TileIndexDiffC, TileIndexDiffC> end_heightdiff_line_by_dir[] = {
				{ {0, 1}, {0, 0} }, // HT_DIR_X
				{ {1, 0}, {0, 0} }, // HT_DIR_Y
				{ {0, 1}, {0, 0} }, // HT_DIR_HU
				{ {1, 1}, {0, 1} }, // HT_DIR_HL
				{ {1, 0}, {0, 0} }, // HT_DIR_VL
				{ {0, 0}, {0, 1} }, // HT_DIR_VR
			};
			static_assert(std::size(start_heightdiff_line_by_dir) == HT_DIR_END);
			static_assert(std::size(end_heightdiff_line_by_dir) == HT_DIR_END);

			distance %= 2; // we're only interested if the distance is even or uneven
			style &= HT_DIR_MASK;
			assert(style < HT_DIR_END);

			/* To handle autorail, we do some magic to be able to use a lookup table.
			 * Firstly if we drag the other way around, we switch start&end, and if needed
			 * also flip the drag-position. Eg if it was on the left, and the distance is even
			 * that means the end, which is now the start is on the right */
			if (swap && distance == 0) style = flip_style_direction[style];

			/* Lambda to help calculating the height at one side of the line. */
			auto get_height = [](auto &tile, auto &heightdiffs) {
				return std::max(
					TileHeight(TileAdd(tile, ToTileIndexDiff(heightdiffs.first))),
					TileHeight(TileAdd(tile, ToTileIndexDiff(heightdiffs.second))));
			};

			/* Use lookup table for start-tile based on HighLightStyle direction */
			h0 = get_height(start_tile, start_heightdiff_line_by_dir[style]);

			/* Use lookup table for end-tile based on HighLightStyle direction
			 * flip around side (lower/upper, left/right) based on distance */
			if (distance == 0) style = flip_style_direction[style];
			h1 = get_height(end_tile, end_heightdiff_line_by_dir[style]);
			break;
		}
	}

	if (swap) std::swap(h0, h1);
	return (int)(h1 - h0) * TILE_HEIGHT_STEP;
}

/**
 * Check for underflowing the map.
 * @param test  the variable to test for underflowing
 * @param other the other variable to update to keep the line
 * @param mult  the constant to multiply the difference by for \c other
 */
static void CheckUnderflow(int &test, int &other, int mult)
{
	if (test >= 0) return;

	other += mult * test;
	test = 0;
}

/**
 * Check for overflowing the map.
 * @param test  the variable to test for overflowing
 * @param other the other variable to update to keep the line
 * @param max   the maximum value for the \c test variable
 * @param mult  the constant to multiply the difference by for \c other
 */
static void CheckOverflow(int &test, int &other, int max, int mult)
{
	if (test <= max) return;

	other += mult * (test - max);
	test = max;
}

/**
 * Determine and set the draw style, as well as the end point of the drag.
 * @param x The current X-coordinate of the mouse in map coordinates.
 * @param y The current Y-coordinate of the mouse in map coordinates.
 * @param method The user chosen method for dragging rails (specific orientation, autorail).
 */
static void CalcRaildirsDrawstyle(int x, int y, int method)
{
	HighLightStyle b;

	int dx = _thd.selstart.x - (_thd.selend.x & ~TILE_UNIT_MASK);
	int dy = _thd.selstart.y - (_thd.selend.y & ~TILE_UNIT_MASK);
	uint w = abs(dx) + TILE_SIZE;
	uint h = abs(dy) + TILE_SIZE;

	if (method & ~(VPM_RAILDIRS | VPM_SIGNALDIRS)) {
		/* We 'force' a selection direction; first four rail buttons. */
		method &= ~(VPM_RAILDIRS | VPM_SIGNALDIRS);
		int raw_dx = _thd.selstart.x - _thd.selend.x;
		int raw_dy = _thd.selstart.y - _thd.selend.y;
		switch (method) {
			case VPM_FIX_X:
				b = HT_LINE | HT_DIR_Y;
				x = _thd.selstart.x;
				break;

			case VPM_FIX_Y:
				b = HT_LINE | HT_DIR_X;
				y = _thd.selstart.y;
				break;

			case VPM_FIX_HORIZONTAL:
				if (dx == -dy) {
					/* We are on a straight horizontal line. Determine the 'rail'
					 * to build based the sub tile location. */
					b = (x & TILE_UNIT_MASK) + (y & TILE_UNIT_MASK) >= TILE_SIZE ? HT_LINE | HT_DIR_HL : HT_LINE | HT_DIR_HU;
				} else {
					/* We are not on a straight line. Determine the rail to build
					 * based on whether we are above or below it. */
					b = dx + dy >= (int)TILE_SIZE ? HT_LINE | HT_DIR_HU : HT_LINE | HT_DIR_HL;

					/* Calculate where a horizontal line through the start point and
					 * a vertical line from the selected end point intersect and
					 * use that point as the end point. */
					int offset = (raw_dx - raw_dy) / 2;
					x = _thd.selstart.x - (offset & ~TILE_UNIT_MASK);
					y = _thd.selstart.y + (offset & ~TILE_UNIT_MASK);

					/* 'Build' the last half rail tile if needed */
					if ((offset & TILE_UNIT_MASK) > (TILE_SIZE / 2)) {
						if (dx + dy >= (int)TILE_SIZE) {
							x -= (int)TILE_SIZE;
						} else {
							y += (int)TILE_SIZE;
						}
					}

					/* Make sure we do not overflow the map! */
					CheckUnderflow(x, y, 1);
					CheckUnderflow(y, x, 1);
					CheckOverflow(x, y, (Map::MaxX() - 1) * TILE_SIZE, 1);
					CheckOverflow(y, x, (Map::MaxY() - 1) * TILE_SIZE, 1);
					assert(x >= 0 && y >= 0 && x <= (int)(Map::MaxX() * TILE_SIZE) && y <= (int)(Map::MaxY() * TILE_SIZE));
				}
				break;

			case VPM_FIX_VERTICAL:
				if (dx == dy) {
					/* We are on a straight vertical line. Determine the 'rail'
					 * to build based the sub tile location. */
					b = (x & TILE_UNIT_MASK) > (y & TILE_UNIT_MASK) ? HT_LINE | HT_DIR_VL : HT_LINE | HT_DIR_VR;
				} else {
					/* We are not on a straight line. Determine the rail to build
					 * based on whether we are left or right from it. */
					b = dx < dy ? HT_LINE | HT_DIR_VL : HT_LINE | HT_DIR_VR;

					/* Calculate where a vertical line through the start point and
					 * a horizontal line from the selected end point intersect and
					 * use that point as the end point. */
					int offset = (raw_dx + raw_dy + (int)TILE_SIZE) / 2;
					x = _thd.selstart.x - (offset & ~TILE_UNIT_MASK);
					y = _thd.selstart.y - (offset & ~TILE_UNIT_MASK);

					/* 'Build' the last half rail tile if needed */
					if ((offset & TILE_UNIT_MASK) > (TILE_SIZE / 2)) {
						if (dx < dy) {
							y -= (int)TILE_SIZE;
						} else {
							x -= (int)TILE_SIZE;
						}
					}

					/* Make sure we do not overflow the map! */
					CheckUnderflow(x, y, -1);
					CheckUnderflow(y, x, -1);
					CheckOverflow(x, y, (Map::MaxX() - 1) * TILE_SIZE, -1);
					CheckOverflow(y, x, (Map::MaxY() - 1) * TILE_SIZE, -1);
					assert(x >= 0 && y >= 0 && x <= (int)(Map::MaxX() * TILE_SIZE) && y <= (int)(Map::MaxY() * TILE_SIZE));
				}
				break;

			default:
				NOT_REACHED();
		}
	} else if (TileVirtXY(_thd.selstart.x, _thd.selstart.y) == TileVirtXY(x, y)) { // check if we're only within one tile
		if (method & VPM_RAILDIRS) {
			/* The drag has not left the tile it started on, so it is not a drag
			 * at all yet -- it is the press, still being held. The piece of
			 * track was settled then, on what the player was looking at, and
			 * working it out again from where the pointer is taken to be now
			 * can only overrule it. That is what made a held button show one
			 * piece and lay another: the marker jumped as the button went down,
			 * stayed jumped for as long as it was held, and returned when it
			 * was let go, all without the pointer moving. Whatever is being
			 * held is what was drawn. */
			b = _thd.press_drawstyle;
			if ((b & HT_DRAG_MASK) == HT_NONE) b = GetAutorailHT(x, y);
		} else { // rect for autosignals on one tile
			b = HT_RECT;
		}
	} else if (h == TILE_SIZE) { // Is this in X direction?
		if (dx == (int)TILE_SIZE) { // 2x1 special handling
			b = Check2x1AutoRail(DiagDirection::NE) | HT_LINE;
		} else if (dx == -(int)TILE_SIZE) {
			b = Check2x1AutoRail(DiagDirection::SW) | HT_LINE;
		} else {
			b = HT_LINE | HT_DIR_X;
		}
		y = _thd.selstart.y;
	} else if (w == TILE_SIZE) { // Or Y direction?
		if (dy == (int)TILE_SIZE) { // 2x1 special handling
			b = Check2x1AutoRail(DiagDirection::NW) | HT_LINE;
		} else if (dy == -(int)TILE_SIZE) { // 2x1 other direction
			b = Check2x1AutoRail(DiagDirection::SE) | HT_LINE;
		} else {
			b = HT_LINE | HT_DIR_Y;
		}
		x = _thd.selstart.x;
	} else if (w > h * 2) { // still count as x dir?
		b = HT_LINE | HT_DIR_X;
		y = _thd.selstart.y;
	} else if (h > w * 2) { // still count as y dir?
		b = HT_LINE | HT_DIR_Y;
		x = _thd.selstart.x;
	} else { // complicated direction
		int d = w - h;
		_thd.selend.x = _thd.selend.x & ~TILE_UNIT_MASK;
		_thd.selend.y = _thd.selend.y & ~TILE_UNIT_MASK;

		/* four cases. */
		if (x > _thd.selstart.x) {
			if (y > _thd.selstart.y) {
				/* south */
				if (d == 0) {
					b = (x & TILE_UNIT_MASK) > (y & TILE_UNIT_MASK) ? HT_LINE | HT_DIR_VL : HT_LINE | HT_DIR_VR;
				} else if (d >= 0) {
					x = _thd.selstart.x + h;
					b = HT_LINE | HT_DIR_VL;
				} else {
					y = _thd.selstart.y + w;
					b = HT_LINE | HT_DIR_VR;
				}
			} else {
				/* west */
				if (d == 0) {
					b = (x & TILE_UNIT_MASK) + (y & TILE_UNIT_MASK) >= TILE_SIZE ? HT_LINE | HT_DIR_HL : HT_LINE | HT_DIR_HU;
				} else if (d >= 0) {
					x = _thd.selstart.x + h;
					b = HT_LINE | HT_DIR_HL;
				} else {
					y = _thd.selstart.y - w;
					b = HT_LINE | HT_DIR_HU;
				}
			}
		} else {
			if (y > _thd.selstart.y) {
				/* east */
				if (d == 0) {
					b = (x & TILE_UNIT_MASK) + (y & TILE_UNIT_MASK) >= TILE_SIZE ? HT_LINE | HT_DIR_HL : HT_LINE | HT_DIR_HU;
				} else if (d >= 0) {
					x = _thd.selstart.x - h;
					b = HT_LINE | HT_DIR_HU;
				} else {
					y = _thd.selstart.y + w;
					b = HT_LINE | HT_DIR_HL;
				}
			} else {
				/* north */
				if (d == 0) {
					b = (x & TILE_UNIT_MASK) > (y & TILE_UNIT_MASK) ? HT_LINE | HT_DIR_VL : HT_LINE | HT_DIR_VR;
				} else if (d >= 0) {
					x = _thd.selstart.x - h;
					b = HT_LINE | HT_DIR_VR;
				} else {
					y = _thd.selstart.y - w;
					b = HT_LINE | HT_DIR_VL;
				}
			}
		}
	}

	if (_settings_client.gui.measure_tooltip) {
		TileIndex t0 = TileVirtXY(_thd.selstart.x, _thd.selstart.y);
		TileIndex t1 = TileVirtXY(x, y);
		uint distance = DistanceManhattan(t0, t1) + 1;

		if (distance == 1) {
			HideMeasurementTooltips();
		} else {
			int heightdiff = CalcHeightdiff(b, distance, t0, t1);
			/* If we are showing a tooltip for horizontal or vertical drags,
			 * 2 tiles have a length of 1. To bias towards the ceiling we add
			 * one before division. It feels more natural to count 3 lengths as 2 */
			if ((b & HT_DIR_MASK) != HT_DIR_X && (b & HT_DIR_MASK) != HT_DIR_Y) {
				distance = CeilDiv(distance, 2);
			}

			if (heightdiff == 0) {
				ShowMeasurementTooltips(GetEncodedString(STR_MEASURE_LENGTH, distance));
			} else {
				ShowMeasurementTooltips(GetEncodedString(STR_MEASURE_LENGTH_HEIGHTDIFF, distance, heightdiff));
			}
		}
	}

	_thd.selend.x = x;
	_thd.selend.y = y;
	_thd.next_drawstyle = b;
}

/**
 * Selects tiles while dragging
 * @param x X coordinate of end of selection
 * @param y Y coordinate of end of selection
 * @param method modifies the way tiles are selected. Possible
 * methods are VPM_* in viewport.h
 */
void VpSelectTilesWithMethod(int x, int y, ViewportPlaceMethod method)
{
	int sx, sy;
	HighLightStyle style;

	if (x == -1) {
		_thd.selend.x = -1;
		return;
	}

	/* Special handling of drag in any (8-way) direction */
	if (method & (VPM_RAILDIRS | VPM_SIGNALDIRS)) {
		_thd.selend.x = x;
		_thd.selend.y = y;
		CalcRaildirsDrawstyle(x, y, method);
		return;
	}

	/* Needed so level-land is placed correctly */
	if ((_thd.next_drawstyle & HT_DRAG_MASK) == HT_POINT) {
		x += TILE_SIZE / 2;
		y += TILE_SIZE / 2;
	}

	sx = _thd.selstart.x;
	sy = _thd.selstart.y;

	int limit = 0;

	switch (method) {
		case VPM_X_OR_Y: // drag in X or Y direction
			if (abs(sy - y) < abs(sx - x)) {
				y = sy;
				style = HT_DIR_X;
			} else {
				x = sx;
				style = HT_DIR_Y;
			}
			goto calc_heightdiff_single_direction;

		case VPM_X_LIMITED: // Drag in X direction (limited size).
			limit = (_thd.sizelimit - 1) * TILE_SIZE;
			[[fallthrough]];

		case VPM_FIX_X: // drag in Y direction
			x = sx;
			style = HT_DIR_Y;
			goto calc_heightdiff_single_direction;

		case VPM_Y_LIMITED: // Drag in Y direction (limited size).
			limit = (_thd.sizelimit - 1) * TILE_SIZE;
			[[fallthrough]];

		case VPM_FIX_Y: // drag in X direction
			y = sy;
			style = HT_DIR_X;

calc_heightdiff_single_direction:;
			if (limit > 0) {
				x = sx + Clamp(x - sx, -limit, limit);
				y = sy + Clamp(y - sy, -limit, limit);
			}
			if (_settings_client.gui.measure_tooltip) {
				TileIndex t0 = TileVirtXY(sx, sy);
				TileIndex t1 = TileVirtXY(x, y);
				uint distance = DistanceManhattan(t0, t1) + 1;

				if (distance == 1) {
					HideMeasurementTooltips();
				} else {
					/* With current code passing a HT_LINE style to calculate the height
					 * difference is enough. However if/when a point-tool is created
					 * with this method, function should be called with new_style (below)
					 * instead of HT_LINE | style case HT_POINT is handled specially
					 * new_style := (_thd.next_drawstyle & HT_RECT) ? HT_LINE | style : _thd.next_drawstyle; */
					int heightdiff = CalcHeightdiff(HT_LINE | style, 0, t0, t1);

					if (heightdiff == 0) {
						ShowMeasurementTooltips(GetEncodedString(STR_MEASURE_LENGTH, distance));
					} else {
						ShowMeasurementTooltips(GetEncodedString(STR_MEASURE_LENGTH_HEIGHTDIFF, distance, heightdiff));
					}
				}
			}
			break;

		case VPM_X_AND_Y_LIMITED: // Drag an X by Y constrained rect area.
			limit = (_thd.sizelimit - 1) * TILE_SIZE;
			x = sx + Clamp(x - sx, -limit, limit);
			y = sy + Clamp(y - sy, -limit, limit);
			[[fallthrough]];

		case VPM_X_AND_Y: // drag an X by Y area
			if (_settings_client.gui.measure_tooltip) {
				TileIndex t0 = TileVirtXY(sx, sy);
				TileIndex t1 = TileVirtXY(x, y);
				uint dx = Delta(TileX(t0), TileX(t1)) + 1;
				uint dy = Delta(TileY(t0), TileY(t1)) + 1;

				/* If dragging an area (eg dynamite tool) and it is actually a single
				 * row/column, change the type to 'line' to get proper calculation for height */
				style = (HighLightStyle)_thd.next_drawstyle;
				if (_thd.IsDraggingDiagonal()) {
					/* Determine the "area" of the diagonal dragged selection.
					 * We assume the area is the number of tiles along the X
					 * edge and the number of tiles along the Y edge. However,
					 * multiplying these two numbers does not give the exact
					 * number of tiles; basically we are counting the black
					 * squares on a chess board and ignore the white ones to
					 * make the tile counts at the edges match up. There is no
					 * other way to make a proper count though.
					 *
					 * First convert to the rotated coordinate system. */
					int dist_x = TileX(t0) - TileX(t1);
					int dist_y = TileY(t0) - TileY(t1);
					int a_max = dist_x + dist_y;
					int b_max = dist_y - dist_x;

					/* Now determine the size along the edge, but due to the
					 * chess board principle this counts double. */
					a_max = abs(a_max + (a_max > 0 ? 2 : -2)) / 2;
					b_max = abs(b_max + (b_max > 0 ? 2 : -2)) / 2;

					/* We get a 1x1 on normal 2x1 rectangles, due to it being
					 * a seen as two sides. As the result for actual building
					 * will be the same as non-diagonal dragging revert to that
					 * behaviour to give it a more normally looking size. */
					if (a_max != 1 || b_max != 1) {
						dx = a_max;
						dy = b_max;
					}
				} else if (style & HT_RECT) {
					if (dx == 1) {
						style = HT_LINE | HT_DIR_Y;
					} else if (dy == 1) {
						style = HT_LINE | HT_DIR_X;
					}
				}

				if (dx != 1 || dy != 1) {
					int heightdiff = CalcHeightdiff(style, 0, t0, t1);

					dx -= (style & HT_POINT ? 1 : 0);
					dy -= (style & HT_POINT ? 1 : 0);

					if (heightdiff == 0) {
						ShowMeasurementTooltips(GetEncodedString(STR_MEASURE_AREA, dx, dy));
					} else {
						ShowMeasurementTooltips(GetEncodedString(STR_MEASURE_AREA_HEIGHTDIFF, dx, dy, heightdiff));
					}
				}
			}
			break;

		default: NOT_REACHED();
	}

	_thd.selend.x = x;
	_thd.selend.y = y;
}

/**
 * Handle the mouse while dragging for placement/resizing.
 * @return State of handling the event.
 */
EventState VpHandlePlaceSizingDrag()
{
	if (_special_mouse_mode != SpecialMouseMode::Sizing && _special_mouse_mode != SpecialMouseMode::Dragging) return EventState::NotHandled;

	/* stop drag mode if the window has been closed */
	Window *w = _thd.GetCallbackWnd();
	if (w == nullptr) {
		ResetObjectToPlace();
		return EventState::Handled;
	}

	/* while dragging execute the drag procedure of the corresponding window (mostly VpSelectTilesWithMethod() ) */
	if (_left_button_down) {
		if (_special_mouse_mode == SpecialMouseMode::Dragging) {
			/* Only register a drag event when the mouse moved. */
			if (_thd.new_pos.x == _thd.selstart.x && _thd.new_pos.y == _thd.selstart.y) return EventState::Handled;
			_thd.selstart.x = _thd.new_pos.x;
			_thd.selstart.y = _thd.new_pos.y;
		}

		w->OnPlaceDrag(_thd.select_method, _thd.select_proc, GetTileBelowCursor());
		return EventState::Handled;
	}

	/* Mouse button released. */
	_special_mouse_mode = SpecialMouseMode::None;
	/* Whatever was being held is no longer being held; the next press settles
	 * its own piece of track. See TileHighlightData::press_drawstyle. */
	_thd.press_drawstyle = HT_NONE;
	if (_special_mouse_mode == SpecialMouseMode::Dragging) return EventState::Handled;

	/* Keep the selected tool, but reset it to the original mode. */
	HighLightStyle others = _thd.place_mode & ~(HT_DRAG_MASK | HT_DIR_MASK);
	if ((_thd.next_drawstyle & HT_DRAG_MASK) == HT_RECT) {
		_thd.place_mode = HT_RECT | others;
	} else if (_thd.select_method & VPM_SIGNALDIRS) {
		_thd.place_mode = HT_RECT | others;
	} else if (_thd.select_method & VPM_RAILDIRS) {
		_thd.place_mode = (_thd.select_method & ~VPM_RAILDIRS) ? _thd.next_drawstyle : (HT_RAIL | others);
	} else {
		_thd.place_mode = HT_POINT | others;
	}
	SetTileSelectSize(1, 1);

	HideMeasurementTooltips();
	w->OnPlaceMouseUp(_thd.select_method, _thd.select_proc, _thd.selend, TileVirtXY(_thd.selstart.x, _thd.selstart.y), TileVirtXY(_thd.selend.x, _thd.selend.y));

	return EventState::Handled;
}

/**
 * Change the cursor and mouse click/drag handling to a mode for performing special operations like tile area selection, object placement, etc.
 * @param icon New shape of the mouse cursor.
 * @param pal Palette to use.
 * @param mode Mode to perform.
 * @param w %Window requesting the mode change.
 */
void SetObjectToPlaceWnd(CursorID icon, PaletteID pal, HighLightStyle mode, Window *w)
{
	SetObjectToPlace(icon, pal, mode, w->window_class, w->window_number);
}

#include "table/animcursors.h"

/**
 * Change the cursor and mouse click/drag handling to a mode for performing special operations like tile area selection, object placement, etc.
 * @param icon New shape of the mouse cursor.
 * @param pal Palette to use.
 * @param mode Mode to perform.
 * @param window_class %Window class of the window requesting the mode change.
 * @param window_num Number of the window in its class requesting the mode change.
 */
void SetObjectToPlace(CursorID icon, PaletteID pal, HighLightStyle mode, WindowClass window_class, WindowNumber window_num)
{
	if (_thd.window_class != WindowClass::Invalid) {
		/* Undo clicking on button and drag & drop */
		Window *w = _thd.GetCallbackWnd();
		/* Call the abort function, but set the window class to something
		 * that will never be used to avoid infinite loops. Setting it to
		 * the 'next' window class must not be done because recursion into
		 * this function might in some cases reset the newly set object to
		 * place or not properly reset the original selection. */
		_thd.window_class = WindowClass::Invalid;
		if (w != nullptr) {
			w->OnPlaceObjectAbort();
			HideMeasurementTooltips();
		}
	}

	/* Mark the old selection dirty, in case the selection shape or colour changes */
	if ((_thd.drawstyle & HT_DRAG_MASK) != HT_NONE) SetSelectionTilesDirty();

	SetTileSelectSize(1, 1);

	_thd.make_square_red = false;

	if (mode == HT_DRAG) { // HT_DRAG is for dragdropping trains in the depot window
		mode = HT_NONE;
		_special_mouse_mode = SpecialMouseMode::DragDrop;
	} else {
		_special_mouse_mode = SpecialMouseMode::None;
	}

	_thd.place_mode = mode;
	_thd.window_class = window_class;
	_thd.window_number = window_num;

	if ((mode & HT_DRAG_MASK) == HT_SPECIAL) { // special tools, like tunnels or docks start with presizing mode
		VpStartPreSizing();
	}

	SetCursor(icon, pal);
}

/** Reset the cursor and mouse mode handling back to default (normal cursor, only clicking in windows). */
void ResetObjectToPlace()
{
	SetObjectToPlace(SPR_CURSOR_MOUSE, PAL_NONE, HT_NONE, WindowClass::MainWindow, 0);
}

Point GetViewportStationMiddle(const Viewport &vp, const Station *st)
{
	int x = TileX(st->xy) * TILE_SIZE;
	int y = TileY(st->xy) * TILE_SIZE;
	int z = GetSlopePixelZ(Clamp(x, 0, Map::SizeX() * TILE_SIZE - 1), Clamp(y, 0, Map::SizeY() * TILE_SIZE - 1));

	Point p = RemapCoords(x, y, z);
	p.x = UnScaleByZoom(p.x - vp.virtual_left, vp.zoom) + vp.left;
	p.y = UnScaleByZoom(p.y - vp.virtual_top, vp.zoom) + vp.top;
	return p;
}

/** Helper class for getting the best sprite sorter. */
struct ViewportSSCSS {
	VpSorterChecker fct_checker; ///< The check function.
	VpSpriteSorter fct_sorter;   ///< The sorting function.
};

/** List of sorters ordered from best to worst. */
static const ViewportSSCSS _vp_sprite_sorters[] = {
#ifdef WITH_SSE
	{ &ViewportSortParentSpritesSSE41Checker, &ViewportSortParentSpritesSSE41 },
#endif
	{ []() { return true; /* Always available */ }, &ViewportSortParentSprites }
};

/** Choose the "best" sprite sorter and set _vp_sprite_sorter. */
void InitializeSpriteSorter()
{
	for (const auto &sprite_sorter : _vp_sprite_sorters) {
		if (sprite_sorter.fct_checker()) {
			_vp_sprite_sorter = sprite_sorter.fct_sorter;
			break;
		}
	}
	assert(_vp_sprite_sorter != nullptr);
}

/**
 * Scroll players main viewport.
 * @param flags type of operation
 * @param tile tile to center viewport on
 * @param target ViewportScrollTarget of scroll target
 * @param ref company or client id depending on the target
 * @return the cost of this operation or an error
 */
CommandCost CmdScrollViewport(DoCommandFlags flags, TileIndex tile, ViewportScrollTarget target, uint32_t ref)
{
	if (_current_company != OWNER_DEITY) return CMD_ERROR;
	switch (target) {
		case ViewportScrollTarget::Everyone:
			break;
		case ViewportScrollTarget::Company:
			if (_local_company != (CompanyID)ref) return CommandCost();
			break;
		case ViewportScrollTarget::Client:
			if (_network_own_client_id != (ClientID)ref) return CommandCost();
			break;
		default:
			return CMD_ERROR;
	}

	if (flags.Test(DoCommandFlag::Execute)) {
		ResetObjectToPlace();
		ScrollMainWindowToTile(tile);
	}
	return CommandCost();
}

void MarkCatchmentTilesDirty()
{
	if (_viewport_highlight_town != nullptr) {
		MarkWholeScreenDirty();
		return;
	}

	if (_viewport_highlight_station != nullptr) {
		if (_viewport_highlight_station->catchment_tiles.tile == INVALID_TILE) {
			MarkWholeScreenDirty();
			_viewport_highlight_station = nullptr;
		} else {
			BitmapTileIterator it(_viewport_highlight_station->catchment_tiles);
			for (TileIndex tile = it; tile != INVALID_TILE; tile = ++it) {
				MarkTileDirtyByTile(tile);
			}
		}
	}

	if (_viewport_highlight_station_rect != nullptr) {
		if (!_viewport_highlight_station_rect->IsInUse()) {
			_viewport_highlight_station_rect = nullptr;
		}
		MarkWholeScreenDirty();
	}

	if (_viewport_highlight_waypoint != nullptr) {
		if (!_viewport_highlight_waypoint->IsInUse()) {
			_viewport_highlight_waypoint = nullptr;
		}
		MarkWholeScreenDirty();
	}

	if (_viewport_highlight_waypoint_rect != nullptr) {
		if (!_viewport_highlight_waypoint_rect->IsInUse()) {
			_viewport_highlight_waypoint_rect = nullptr;
		}
		MarkWholeScreenDirty();
	}
}

static void SetWindowDirtyForViewportCatchment()
{
	if (_viewport_highlight_station != nullptr) SetWindowDirty(WindowClass::StationView, _viewport_highlight_station->index);
	if (_viewport_highlight_station_rect != nullptr) SetWindowDirty(WindowClass::StationView, _viewport_highlight_station_rect->index);
	if (_viewport_highlight_waypoint != nullptr) SetWindowDirty(WindowClass::WaypointView, _viewport_highlight_waypoint->index);
	if (_viewport_highlight_waypoint_rect != nullptr) SetWindowDirty(WindowClass::WaypointView, _viewport_highlight_waypoint_rect->index);
	if (_viewport_highlight_town != nullptr) SetWindowDirty(WindowClass::TownView, _viewport_highlight_town->index);
}

static void ClearViewportCatchment()
{
	MarkCatchmentTilesDirty();
	_viewport_highlight_station = nullptr;
	_viewport_highlight_station_rect = nullptr;
	_viewport_highlight_waypoint = nullptr;
	_viewport_highlight_waypoint_rect = nullptr;
	_viewport_highlight_town = nullptr;
}

/**
 * Select or deselect station for coverage area highlight.
 * Selecting a station will deselect a town.
 * @param *st Station in question
 * @param sel Select or deselect given station
 */
void SetViewportCatchmentStation(const Station *st, bool sel)
{
	SetWindowDirtyForViewportCatchment();
	/* Mark tiles dirty for redrawing and update selected station if a different station is already highlighted. */
	if (sel && _viewport_highlight_station != st) {
		ClearViewportCatchment();
		_viewport_highlight_station = st;
		MarkCatchmentTilesDirty();
	/* Mark tiles dirty for redrawing and clear station selection if deselecting highlight. */
	} else if (!sel && _viewport_highlight_station == st) {
		MarkCatchmentTilesDirty();
		_viewport_highlight_station = nullptr;
	}
	/* Redraw the currently selected station window */
	if (_viewport_highlight_station != nullptr) SetWindowDirty(WindowClass::StationView, _viewport_highlight_station->index);
}

/**
 * Select or deselect station for rectangle area highlight.
 * Selecting a station will deselect a town.
 * @param *st Station in question
 * @param sel Select or deselect given station
 */
void SetViewportStationRect(const Station *st, bool sel)
{
	SetWindowDirtyForViewportCatchment();
	/* Mark tiles dirty for redrawing and update selected station if a different station is already highlighted. */
	if (sel && _viewport_highlight_station_rect != st) {
		ClearViewportCatchment();
		_viewport_highlight_station_rect = st;
		MarkCatchmentTilesDirty();
	/* Mark tiles dirty for redrawing and clear station selection if deselecting highlight. */
	} else if (!sel && _viewport_highlight_station_rect == st) {
		MarkCatchmentTilesDirty();
		_viewport_highlight_station_rect = nullptr;
	}
	/* Redraw the currently selected station window */
	if (_viewport_highlight_station_rect != nullptr) SetWindowDirty(WindowClass::StationView, _viewport_highlight_station_rect->index);
}

/**
 * Select or deselect waypoint for coverage area highlight.
 * Selecting a waypoint will deselect a town.
 * @param *wp Waypoint in question
 * @param sel Select or deselect given waypoint
 */
void SetViewportCatchmentWaypoint(const Waypoint *wp, bool sel)
{
	SetWindowDirtyForViewportCatchment();
	/* Mark tiles dirty for redrawing and update selected waypoint if a different waypoint is already highlighted. */
	if (sel && _viewport_highlight_waypoint != wp) {
		ClearViewportCatchment();
		_viewport_highlight_waypoint = wp;
		MarkCatchmentTilesDirty();
	/* Mark tiles dirty for redrawing and clear waypoint selection if deselecting highlight. */
	} else if (!sel && _viewport_highlight_waypoint == wp) {
		MarkCatchmentTilesDirty();
		_viewport_highlight_waypoint = nullptr;
	}
	/* Redraw the currently selected waypoint window */
	if (_viewport_highlight_waypoint != nullptr) SetWindowDirty(WindowClass::WaypointView, _viewport_highlight_waypoint->index);
}

/**
 * Select or deselect waypoint for rectangle area highlight.
 * Selecting a waypoint will deselect a town.
 * @param *wp Waypoint in question
 * @param sel Select or deselect given waypoint
 */
void SetViewportWaypointRect(const Waypoint *wp, bool sel)
{
	SetWindowDirtyForViewportCatchment();
	/* Mark tiles dirty for redrawing and update selected waypoint if a different waypoint is already highlighted. */
	if (sel && _viewport_highlight_waypoint_rect != wp) {
		ClearViewportCatchment();
		_viewport_highlight_waypoint_rect = wp;
		MarkCatchmentTilesDirty();
	/* Mark tiles dirty for redrawing and clear waypoint selection if deselecting highlight. */
	} else if (!sel && _viewport_highlight_waypoint_rect == wp) {
		MarkCatchmentTilesDirty();
		_viewport_highlight_waypoint_rect = nullptr;
	}
	/* Redraw the currently selected waypoint window */
	if (_viewport_highlight_waypoint_rect != nullptr) SetWindowDirty(WindowClass::WaypointView, _viewport_highlight_waypoint_rect->index);
}

/**
 * Select or deselect town for coverage area highlight.
 * Selecting a town will deselect a station.
 * @param *t Town in question
 * @param sel Select or deselect given town
 */
void SetViewportCatchmentTown(const Town *t, bool sel)
{
	SetWindowDirtyForViewportCatchment();
	/* Mark tiles dirty for redrawing and update selected town if a different town is already highlighted. */
	if (sel && _viewport_highlight_town != t) {
		ClearViewportCatchment();
		_viewport_highlight_town = t;
		MarkWholeScreenDirty();
	/* Mark tiles dirty for redrawing and clear town selection if deselecting highlight. */
	} else if (!sel && _viewport_highlight_town == t) {
		_viewport_highlight_town = nullptr;
		MarkWholeScreenDirty();
	}
	/* Redraw the currently selected town window */
	if (_viewport_highlight_town != nullptr) SetWindowDirty(WindowClass::TownView, _viewport_highlight_town->index);
}

/**
 * Cancel viewport vehicle following, and raise follow location widget if needed.
 * @param viewport_window Window of this viewport.
 */
void ViewportData::CancelFollow(const Window &viewport_window)
{
	if (this->follow_vehicle == VehicleID::Invalid()) return;

	if (viewport_window.window_class == WindowClass::MainWindow) {
		/* We're cancelling follow in the main viewport, so we need to check for a vehicle view window
		 * to raise the location follow widget. */
		Window *vehicle_window = FindWindowById(WindowClass::VehicleView, this->follow_vehicle);
		if (vehicle_window != nullptr) vehicle_window->RaiseWidgetWhenLowered(WID_VV_LOCATION);
	}

	this->dest_scrollpos_x = this->scrollpos_x;
	this->dest_scrollpos_y = this->scrollpos_y;
	this->follow_vehicle = VehicleID::Invalid();
}

/*
 * ==========================================================================================
 * The cab view (cab_view.h): the world ahead of a vehicle in perspective, from the eye at its
 * front. The same lists the turned view collects, drawn by a perspective rasterizer with a
 * depth buffer: the ground from the block's north-up texture, everything standing as a
 * billboard of its sprite. Docs: docs/decouple_three_d.md.
 * ==========================================================================================
 */

/** True units of the cab view to a unit of height (z): a height level, 8 units of z, stands 3.27 units of x and y tall in the game's projection (30 degrees above the horizon). */
static constexpr double CAB_Z_SCALE = 0.408;
/** A pixel of a sprite at the normal zoom in world units: across (its width) and up (its height), from the game's projection. */
static constexpr double CAB_PX_ACROSS = 0.354;
static constexpr double CAB_PX_UP = 0.408;
static constexpr double CAB_NEAR = 2.0;        ///< world units ahead of the eye: nearer is cut off
static constexpr int CAB_RANGE_TILES = 20;     ///< how far the view reaches, tiles
static constexpr double CAB_FOV_TAN = 1.0;     ///< the tangent of half the horizontal field of view (90 degrees)
/** The eye this many units of height above the vehicle's position, by the vehicle's type; it bows lower under a low bridge (CabEyeHeight()). */
static constexpr int CAB_EYE_Z_TRAIN = 9;
static constexpr int CAB_EYE_Z_ROAD = 7;
static constexpr int CAB_EYE_Z_SHIP = 9;
static constexpr int CAB_EYE_Z_AIRCRAFT = 14;
static constexpr double CAB_EYE_Z_LOWEST = 3.0; ///< the eye bows no lower than this
static constexpr double CAB_EYE_AHEAD = 5.0;   ///< the eye this far ahead of the vehicle's position, world units
static constexpr double CAB_TURN_RATE = 0.22;  ///< how much of the way to the vehicle's direction the eye turns a frame
static constexpr double CAB_HAZE_FROM = 6.0 * TILE_SIZE; ///< the haze begins this far away
static constexpr size_t CAB_TEXTURES_KEPT = 4096; ///< sprites kept as textures for the billboards

/** A point in the eye's frame: forward, right and up from the eye (world units, up already scaled), with its texture pixel. */
struct CabPoint {
	double f, r, u, tu, tv;
};

/** A point on the picture with what the perspective needs: 1/depth and the texture pixel over depth. */
struct CabVertex {
	double sx, sy, iw, uw, vw;
};

/** The eye. */
struct CabEye {
	double x, y, z;  ///< world position (z in the game's units)
	double fx, fy;   ///< the direction looked along
	double rx, ry;   ///< the direction to the right
	double focal;    ///< pixels: a unit across at a unit of depth
	double cx, cy;   ///< the middle of the picture: the horizon is at cy
};

/** A sprite drawn into a texture for a billboard or a box. */
struct CabTexture {
	int w = 0, h = 0;
	int ox = 0, oy = 0; ///< where the texture's top left is in the north-up virtual coordinates the sprite was drawn at (RemapCoords())
	std::vector<uint32_t> px;
	uint64_t used = 0;
};
static std::map<uint64_t, CabTexture> _cab_textures;
static uint64_t _cab_texture_clock = 0;

static inline CabPoint CabToEye(const CabEye &eye, double wx, double wy, double wz, double tu, double tv)
{
	double dx = wx - eye.x, dy = wy - eye.y;
	return {dx * eye.fx + dy * eye.fy, dx * eye.rx + dy * eye.ry, (wz - eye.z) * CAB_Z_SCALE, tu, tv};
}

/**
 * The eye's view on the graphics card (ThreeDGpuEyePicture): a point of the world, from an origin, to the card's clip
 * space as CabToEye() and CabProject() place it on the picture -- x, y the picture's pixels (y down), w how far ahead,
 * z within the clip space from the near plane on.
 * @param ox,oy,oz the origin, world units (z the game's units of height)
 * @param near the near plane, world units ahead
 */
static std::array<double, 16> EyeClip(const CabEye &eye, int width, int height, double ox, double oy, double oz, double near)
{
	const double dx = ox - eye.x, dy = oy - eye.y;
	const double f0 = dx * eye.fx + dy * eye.fy, r0 = dx * eye.rx + dy * eye.ry, u0 = (oz - eye.z) * CAB_Z_SCALE;
	const double ax = 2 * eye.cx / width - 1, bx = 2 * eye.focal / width;
	const double ay = 2 * eye.cy / height - 1, by = 2 * eye.focal / height;
	return {
		ax * eye.fx + bx * eye.rx, ax * eye.fy + bx * eye.ry, 0.0, ax * f0 + bx * r0,
		ay * eye.fx, ay * eye.fy, -by * CAB_Z_SCALE, ay * f0 - by * u0,
		-eye.fx, -eye.fy, 0.0, 2 * near - f0,
		eye.fx, eye.fy, 0.0, f0,
	};
}

/** EyeClip() for the card, and after another matrix (a figure's corners to the world from its origin): rows first. */
static ThreeDGpuMatrix EyeClipMatrix(const std::array<double, 16> &clip, const std::array<double, 16> *then = nullptr)
{
	ThreeDGpuMatrix m{};
	for (int r = 0; r < 4; r++) {
		for (int c = 0; c < 4; c++) {
			double v = 0;
			if (then == nullptr) {
				v = clip[r * 4 + c];
			} else {
				for (int k = 0; k < 4; k++) v += clip[r * 4 + k] * (*then)[k * 4 + c];
			}
			m[r * 4 + c] = static_cast<float>(v);
		}
	}
	return m;
}

/**
 * Lay what the card drew of the view from the eyes (ThreeDGpuRenderEye()) into the eye's picture where it is nearer:
 * whole, or see-through (the glass over nothing of the card's) over what is drawn there.
 */
static void LayEyeCardPicture(const ThreeDGpuEyePicture &pic, uint32_t *dst, float *zb)
{
	const size_t n = static_cast<size_t>(pic.width) * pic.height;
	for (size_t i = 0; i < n; i++) {
		if (pic.depth[i] <= zb[i]) continue;
		const Colour c(pic.pixels[i]);
		if (c.a == 0) continue;
		if (c.a == 255) {
			dst[i] = pic.pixels[i];
			zb[i] = pic.depth[i];
			continue;
		}
		const Colour d(dst[i]);
		const uint na = 255 - c.a;
		dst[i] = Colour(static_cast<uint8_t>(std::min<uint>(255, c.r + (d.r * na + 127) / 255)), static_cast<uint8_t>(std::min<uint>(255, c.g + (d.g * na + 127) / 255)), static_cast<uint8_t>(std::min<uint>(255, c.b + (d.b * na + 127) / 255))).data;
		/* Over nothing (the sky): its depth, so the sky drawn after the dark (DrawEyeSky()) leaves it (as CabRasterRows()). */
		if (zb[i] == 0.0f) zb[i] = pic.depth[i];
	}
}

static inline CabVertex CabProject(const CabEye &eye, const CabPoint &p)
{
	double iw = 1.0 / p.f;
	return {eye.cx + p.r * eye.focal * iw, eye.cy - p.u * eye.focal * iw, iw, p.tu * iw, p.tv * iw};
}

/** The near plane of the picture being drawn: CAB_NEAR, but a few centimetres from a cab's eye inside its model (the cab is drawn round it). */
static double _cab_near = CAB_NEAR;

/** Cut a polygon to what lies ahead of a near plane. */
static void CabClipNear(std::vector<CabPoint> &poly, double near)
{
	std::vector<CabPoint> out;
	size_t n = poly.size();
	for (size_t i = 0; i < n; i++) {
		const CabPoint &p = poly[i], &q = poly[(i + 1) % n];
		bool pin = p.f >= near, qin = q.f >= near;
		if (pin) out.push_back(p);
		if (pin != qin) {
			double t = (near - p.f) / (q.f - p.f);
			out.push_back({near, p.r + (q.r - p.r) * t, p.u + (q.u - p.u) * t, p.tu + (q.tu - p.tu) * t, p.tv + (q.tv - p.tv) * t});
		}
	}
	poly.swap(out);
}

/** Cut a polygon to what lies ahead of the near plane of the picture being collected. */
static void CabClipNear(std::vector<CabPoint> &poly)
{
	CabClipNear(poly, _cab_near);
}

/** How a triangle of the view from an eye takes its texels (CabRasterRows()'s fill_holes): bits. */
static constexpr uint8_t CAB_FILL_HOLES = 1; ///< the nearest drawn texel where the texture has none (the diamonds of the ground)
static constexpr uint8_t CAB_REPEAT = 2;     ///< the texture repeated beyond its edges, as glTF's samplers do (a model's texture moved and stretched by "KHR_texture_transform": the ghost's sixteen times)

/**
 * Fill a triangle of the picture with a texture in perspective, its rows from row_lo up to row_hi only (a stripe of
 * the picture, CabFlushBatch()): 1/depth and the texture pixel over depth change linearly across the picture, so
 * each pixel's texture pixel is their quotient. A pixel is drawn where the triangle is nearer than what is there.
 * @param fill_holes CAB_FILL_HOLES, CAB_REPEAT (true: CAB_FILL_HOLES); beyond the texture's edges nothing without CAB_REPEAT
 * @param alpha below 256 see-through (a vehicle's glass): laid over what is there by so many 256ths, its depth not kept
 *              where something is behind it
 */
static void CabRasterRows(uint32_t *dst, float *zb, int w, int h, const uint32_t *tex, int tw, int th, const CabVertex &a, const CabVertex &b, const CabVertex &c, uint8_t fill_holes, uint light, int row_lo, int row_hi, uint alpha = 256)
{
	const bool repeat = (fill_holes & CAB_REPEAT) != 0;
	const bool fill = (fill_holes & CAB_FILL_HOLES) != 0;
	/* Its rows first: of a stripe of the picture (ParallelRows()) most triangles have none. */
	int y0 = std::max(row_lo, static_cast<int>(std::ceil(std::min({a.sy, b.sy, c.sy}) - 0.5)));
	int y1 = std::min(std::min(h, row_hi) - 1, static_cast<int>(std::floor(std::max({a.sy, b.sy, c.sy}) - 0.5)));
	if (y0 > y1) return;
	double area = (b.sx - a.sx) * (c.sy - a.sy) - (b.sy - a.sy) * (c.sx - a.sx);
	if (std::abs(area) < 1e-9) return;
	auto grad = [&](double va, double vb, double vc, double &ddx, double &ddy) {
		ddx = ((vb - va) * (c.sy - a.sy) - (vc - va) * (b.sy - a.sy)) / area;
		ddy = ((vc - va) * (b.sx - a.sx) - (vb - va) * (c.sx - a.sx)) / area;
	};
	double diw_dx, diw_dy, duw_dx, duw_dy, dvw_dx, dvw_dy;
	grad(a.iw, b.iw, c.iw, diw_dx, diw_dy);
	grad(a.uw, b.uw, c.uw, duw_dx, duw_dy);
	grad(a.vw, b.vw, c.vw, dvw_dx, dvw_dy);
	double sign = area > 0 ? 1.0 : -1.0;
	for (int y = y0; y <= y1; y++) {
		double yc = y + 0.5;
		double xlo = -1e18, xhi = 1e18;
		auto edge = [&](const CabVertex &p, const CabVertex &q) {
			double k = -(q.sy - p.sy) * sign;
			double e0 = ((q.sx - p.sx) * (yc - p.sy) + (q.sy - p.sy) * p.sx) * sign;
			if (k > 1e-12) {
				xlo = std::max(xlo, -e0 / k);
			} else if (k < -1e-12) {
				xhi = std::min(xhi, -e0 / k);
			} else if (e0 < 0) {
				xlo = 1e18;
			}
		};
		edge(a, b);
		edge(b, c);
		edge(c, a);
		int xs = std::max(0, static_cast<int>(std::ceil(xlo - 0.5)));
		int xe = std::min(w - 1, static_cast<int>(std::floor(xhi - 0.5)));
		if (xs > xe) continue;
		double ox = xs + 0.5 - a.sx, oy = yc - a.sy;
		double iw = a.iw + diw_dx * ox + diw_dy * oy;
		double uw = a.uw + duw_dx * ox + duw_dy * oy;
		double vw = a.vw + dvw_dx * ox + dvw_dy * oy;
		uint32_t *row = dst + static_cast<size_t>(y) * w;
		float *zrow = zb + static_cast<size_t>(y) * w;
		for (int x = xs; x <= xe; x++, iw += diw_dx, uw += duw_dx, vw += dvw_dx) {
			if (iw <= zrow[x]) continue;
			double inv = 1.0 / iw;
			int iu = static_cast<int>(std::floor(uw * inv)), iv = static_cast<int>(std::floor(vw * inv));
			if (repeat) {
				iu %= tw;
				iv %= th;
				if (iu < 0) iu += tw;
				if (iv < 0) iv += th;
			} else if (iu < 0 || iv < 0 || iu >= tw || iv >= th) {
				continue;
			}
			uint32_t p = tex[static_cast<size_t>(iv) * tw + iu];
			if (p == 0 && fill) {
				static const int near[24][2] = {
					{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {-1, 1}, {1, -1}, {-1, -1},
					{2, 0}, {-2, 0}, {0, 2}, {0, -2}, {2, 1}, {-2, 1}, {2, -1}, {-2, -1},
					{3, 0}, {-3, 0}, {0, 3}, {0, -3}, {3, 1}, {-3, 1}, {3, -1}, {-3, -1},
				};
				for (const auto &[dx, dy] : near) {
					int nu = iu + dx, nv = iv + dy;
					if (nu < 0 || nv < 0 || nu >= tw || nv >= th) continue;
					p = tex[static_cast<size_t>(nv) * tw + nu];
					if (p != 0) break;
				}
			}
			if (p == 0) continue;
			const uint32_t lit = light == 256 ? p : ShadedPixel(p, light);
			if (alpha < 256) {
				const Colour over(lit), under(row[x]);
				row[x] = Colour(static_cast<uint8_t>((over.r * alpha + under.r * (256 - alpha)) >> 8), static_cast<uint8_t>((over.g * alpha + under.g * (256 - alpha)) >> 8), static_cast<uint8_t>((over.b * alpha + under.b * (256 - alpha)) >> 8)).data;
				/* Over nothing (the sky): its depth, so the sky drawn after the dark (DrawEyeSky()) leaves it. */
				if (zrow[x] == 0.0f) zrow[x] = static_cast<float>(iw);
				continue;
			}
			row[x] = lit;
			zrow[x] = static_cast<float>(iw);
		}
	}
}

/*
 * Drawing the view from an eye on every core of the processor (the player, 16. 10.: "the technologies of OpenMW",
 * which prepares its world in threads of their own): the triangles of a pass whose textures stay as they are until
 * it ends -- the ground, the houses, the figures near the eye -- are first collected, then drawn at once by a thread
 * for each stripe of the picture's rows, each thread all of them but only its rows. The depth decides what is seen,
 * so the picture is the same as drawn one after the other.
 */
struct CabBatchTriangle {
	uint32_t *dst;
	float *zb;
	int w, h;
	const uint32_t *tex;
	int tw, th;
	CabVertex a, b, c;
	uint8_t fill_holes; ///< CAB_FILL_HOLES, CAB_REPEAT
	uint light;
	uint alpha = 256; ///< see-through (CabRasterRows())
};
static std::vector<CabBatchTriangle> _cab_batch;
static bool _cab_batching = false;
/** The glass of the vehicles (see-through): collected apart while this is set, drawn after all else of their pass (CabFlushGlass()). */
static std::vector<CabBatchTriangle> _cab_glass_batch;
static uint _cab_glass_alpha = 256;
/** At night the triangles of a material with a light of its own drawn by CabDrawFigureModel(), put here to be drawn again
 * over the dark (DrawEyeNight()) a hair nearer: not darkened. nullptr: not kept. */
static std::vector<CabBatchTriangle> *_cab_glow_after_night = nullptr;

/*
 * The picture from an eye in two steps (the player, 17. 10.: "how will one core compute the game and another load
 * the cars when eight cores draw the first person view"; "that is why it bites: every core draws but none computes
 * the game that is to be drawn"; "do 1 2 3 4"). What is to be drawn is collected under the lock of the game's state
 * -- the tiles, their sprites, the vehicles, the figures, the textures of them all -- into a frame (EyeFrame): lists
 * of triangles with the textures they point to, and a few jobs over the whole picture (the shadows, the haze, the
 * night), in the order they are to be drawn. Then the lock is let go of, the game goes on, and a thread of its own
 * draws the frame on the cores left to the drawing (EyeFrameThread(), DrawingCores()), while the window shows the
 * picture drawn before: one frame behind, forty milliseconds. Nothing of the game is read while a frame is drawn,
 * and what its triangles point to stays as it is till it is taken: the pictures of the vehicles and the figures are
 * pinned (PinThreeDPictures()), the textures of the objects are neither dropped nor forgotten (EyeFrameDrawing()),
 * and a frame is collected only when the one before was taken, so the caches the collecting writes (the sprites as
 * textures, the painted textures of the vehicles) are not written while they are read.
 */
/** A pass of a frame: triangles drawn at once by bands of rows (CabRasterLists()), or a job over the whole picture. */
struct EyePass {
	std::vector<CabBatchTriangle> triangles;
	std::function<void(uint32_t *, float *)> work; ///< the job, given the picture and its depth; none for the triangles
};
/** A picture drawn into of its own (a tunnel seen through its portal, CabDrawTunnelSeenIn()). */
struct EyeCanvas {
	std::vector<uint32_t> px;
	std::vector<float> z;
};
/** A frame of the view from an eye: what is to be drawn, as collected, and the picture it is drawn into. */
struct EyeFrame {
	int width = 0, height = 0;
	std::vector<uint32_t> pixels;
	std::vector<float> depth;                 ///< 1/depth of every pixel: the bigger the nearer, 0 nothing
	std::vector<EyePass> passes;              ///< in the order they are drawn
	std::deque<std::vector<uint32_t>> colours; ///< textures of this frame alone the triangles point to (a colour, the far land's)
	std::deque<CabTexture> sprites;           ///< sprites painted for this frame alone (a bridge's, a foundation's)
	std::deque<EyeCanvas> canvases;           ///< pictures of its own drawn while collecting (a tunnel seen in)
	std::vector<uint32_t> ground;             ///< the north-up texture of the ground ahead
	const void *client = nullptr;             ///< whose picture it is: the window's camera
	bool depot = false;                       ///< nothing to draw: the picture was filled while collecting (hidden in a depot)
	bool pinned = false;                      ///< the pictures of the vehicles pinned for it (PinThreeDPictures())
	double collect_ms = 0;                    ///< how long collecting took, under the lock
	double draw_ms = 0;                       ///< and drawing, on the thread
	std::chrono::steady_clock::time_point drawn_at{}; ///< when it was drawn: one its window does not take goes after a while

	/** A triangle, into the pass being collected (a new one after a job). */
	void Add(const CabBatchTriangle &t)
	{
		if (this->passes.empty() || this->passes.back().work) this->passes.emplace_back();
		this->passes.back().triangles.push_back(t);
	}
	/** A job over the whole picture, after all collected so far. */
	void Job(std::function<void(uint32_t *, float *)> work)
	{
		this->passes.push_back({{}, std::move(work)});
	}
};
/** The frame being collected: what is drawn goes into it (CabRasterTriangle()); nullptr: drawn at once, as before. */
static EyeFrame *_eye_frame = nullptr;

/** A texture of one colour a triangle may point to: the frame's while one is collected, else the colour given, drawn at once. */
static const uint32_t *EyeColour(uint32_t &colour)
{
	if (_eye_frame == nullptr) return &colour;
	return &_eye_frame->colours.emplace_back(1, colour).front();
}

/** A sprite texture of this frame alone (not kept for the next, CabSpriteTexture()): the frame's while one is collected, else the scratch given. */
static CabTexture &EyeScratchTexture(CabTexture &scratch)
{
	if (_eye_frame == nullptr) return scratch;
	return _eye_frame->sprites.emplace_back();
}

int _cab_threads = 0; ///< The rig: how many threads to draw on (test3d chodec mereni), 0 every core.

/**
 * How many cores the drawing has: the processor's but two -- one computes the game, one loads (the pictures of the
 * vehicles, three_d_vehicle_object.cpp) --, at most 16 (the player, 17. 10.: "we all have machines with four cores
 * at least": two draw there, six on eight).
 */
static int DrawingCores()
{
	static const int cores = std::clamp<int>(static_cast<int>(std::thread::hardware_concurrency()) - 2, 1, 16);
	return cores;
}

/** How many threads ParallelRows() runs a job on. */
static int ParallelThreads(int rows, bool worth)
{
	return worth ? std::min(_cab_threads > 0 ? _cab_threads : DrawingCores(), std::max(1, rows / 16)) : 1;
}

/**
 * The threads of the drawing, a core each but the game's own, started the first time and waiting for work (as the
 * worker threads of OpenMW): a thread started for each house drawn cost more than it saved.
 */
class DrawingWorkers {
public:
	/** Run a job, the stripe i of n on the worker i - 1 (the caller runs stripe 0); back when all are done. */
	void Run(int n, int rows, const std::function<void(int, int)> &work)
	{
		std::unique_lock lock(this->mutex);
		while (static_cast<int>(this->threads.size()) < n - 1) {
			const int index = static_cast<int>(this->threads.size()) + 1;
			this->threads.emplace_back([this, index] { this->Serve(index); });
		}
		this->work = &work;
		this->rows = rows;
		this->n = n;
		this->remaining = n - 1;
		this->round++;
		lock.unlock();
		this->start.notify_all();
		work(0, rows / n);
		lock.lock();
		this->finished.wait(lock, [this] { return this->remaining == 0; });
		this->work = nullptr;
	}

	~DrawingWorkers()
	{
		{
			std::lock_guard lock(this->mutex);
			this->stop = true;
		}
		this->start.notify_all();
		for (std::thread &t : this->threads) t.join();
	}

private:
	void Serve(int index)
	{
		uint64_t seen = 0;
		std::unique_lock lock(this->mutex);
		for (;;) {
			this->start.wait(lock, [&] { return this->stop || this->round != seen; });
			if (this->stop) return;
			seen = this->round;
			if (index >= this->n) continue;
			const std::function<void(int, int)> &job = *this->work;
			const int lo = this->rows * index / this->n, hi = this->rows * (index + 1) / this->n;
			lock.unlock();
			job(lo, hi);
			lock.lock();
			if (--this->remaining == 0) this->finished.notify_one();
		}
	}

	std::mutex mutex;
	std::condition_variable start;
	std::condition_variable finished;
	std::vector<std::thread> threads;
	const std::function<void(int, int)> *work = nullptr;
	int rows = 0;
	int n = 0;
	int remaining = 0;
	uint64_t round = 0;
	bool stop = false;
};

/**
 * Run a job over the rows of a picture in stripes, a thread each, as many as the processor has cores (at most 16);
 * one after the other when there is little to do, or when a job of the threads runs it.
 * @param rows how many rows
 * @param work the job for the rows from its first up to its second argument
 * @param worth whether the job is worth the threads
 */
static void ParallelRows(int rows, const std::function<void(int, int)> &work, bool worth)
{
	static DrawingWorkers workers;
	static std::atomic<bool> busy = false;
	const int n = ParallelThreads(rows, worth);
	if (n <= 1 || busy.exchange(true)) {
		work(0, rows);
		return;
	}
	workers.Run(n, rows, work);
	busy = false;
}

/**
 * Run a job over the rows of a picture in bands of rows, a few a thread, handed out one by one: where the work is
 * about the horizon or in a house, the threads still share it.
 * @param rows how many rows
 * @param work the job for the rows from its first up to its second argument
 * @param worth whether the job is worth the threads
 */
static void ParallelBands(int rows, const std::function<void(int, int)> &work, bool worth)
{
	const int threads = ParallelThreads(rows, worth);
	if (threads <= 1) {
		work(0, rows);
		return;
	}
	const int band = std::max(8, (rows + threads * 4 - 1) / (threads * 4));
	const int bands = (rows + band - 1) / band;
	std::atomic<int> next_band = 0;
	ParallelRows(rows, [&](int, int) {
		for (int b; (b = next_band++) < bands;) work(b * band, std::min(rows, (b + 1) * band));
	}, true);
}

/**
 * Draw lists of triangles at once, one after the other as they are, on every core: the picture in bands of rows, a
 * few a thread, handed out one by one (the houses about the horizon are most of the work, the sky none); a band looks
 * through the rows of every triangle, kept small apart, and draws only its own.
 */
static void CabRasterLists(std::span<const std::vector<CabBatchTriangle> *const> lists)
{
	if (_eye_frame != nullptr) {
		/* Collected: drawn when the frame is (EyeFrameThread()). */
		for (const std::vector<CabBatchTriangle> *l : lists) {
			for (const CabBatchTriangle &t : *l) _eye_frame->Add(t);
		}
		return;
	}
	size_t total = 0;
	int h = 0;
	for (const std::vector<CabBatchTriangle> *l : lists) {
		if (!l->empty()) h = l->front().h;
		total += l->size();
	}
	if (total == 0) return;
	const bool worth = total >= 64;
	const int threads = ParallelThreads(h, worth);
	if (threads <= 1) {
		for (const std::vector<CabBatchTriangle> *l : lists) {
			for (const CabBatchTriangle &t : *l) CabRasterRows(t.dst, t.zb, t.w, t.h, t.tex, t.tw, t.th, t.a, t.b, t.c, t.fill_holes, t.light, 0, h, t.alpha);
		}
		return;
	}
	/* The rows of each triangle, pieces of the lists a thread. */
	static constexpr size_t PIECE = 4096;
	std::vector<std::vector<std::pair<int16_t, int16_t>>> rows(lists.size());
	std::vector<std::pair<size_t, size_t>> pieces;
	for (size_t l = 0; l < lists.size(); l++) {
		rows[l].resize(lists[l]->size());
		for (size_t from = 0; from < lists[l]->size(); from += PIECE) pieces.emplace_back(l, from);
	}
	std::atomic<size_t> next_piece = 0;
	ParallelRows(h, [&](int, int) {
		for (size_t k; (k = next_piece++) < pieces.size();) {
			const auto [l, from] = pieces[k];
			const std::vector<CabBatchTriangle> &list = *lists[l];
			for (size_t i = from; i < std::min(list.size(), from + PIECE); i++) {
				const CabBatchTriangle &t = list[i];
				const double lo = std::clamp(std::ceil(std::min({t.a.sy, t.b.sy, t.c.sy}) - 0.5), -1.0, static_cast<double>(h));
				const double hi = std::clamp(std::floor(std::max({t.a.sy, t.b.sy, t.c.sy}) - 0.5), -1.0, static_cast<double>(h));
				rows[l][i] = {static_cast<int16_t>(lo), static_cast<int16_t>(hi)};
			}
		}
	}, pieces.size() >= 2);
	const int band = std::max(8, (h + threads * 8 - 1) / (threads * 8));
	const int bands = (h + band - 1) / band;
	std::atomic<int> next_band = 0;
	ParallelRows(h, [&](int, int) {
		for (int b; (b = next_band++) < bands;) {
			const int lo = b * band, hi = std::min(h, lo + band);
			for (size_t l = 0; l < lists.size(); l++) {
				const std::vector<CabBatchTriangle> &list = *lists[l];
				const std::pair<int16_t, int16_t> *r = rows[l].data();
				for (size_t i = 0; i < list.size(); i++) {
					if (r[i].second < lo || r[i].first >= hi) continue;
					const CabBatchTriangle &t = list[i];
					CabRasterRows(t.dst, t.zb, t.w, t.h, t.tex, t.tw, t.th, t.a, t.b, t.c, t.fill_holes, t.light, lo, hi, t.alpha);
				}
			}
		}
	}, worth);
}

/** Draw the triangles collected (CabBatchScope) on every core (CabRasterLists()). */
static void CabFlushBatch()
{
	const std::vector<CabBatchTriangle> *batch = &_cab_batch;
	CabRasterLists({&batch, 1});
	_cab_batch.clear();
}

/** Draw the glass collected, over all else drawn (each layer laid over what is behind it). */
static void CabFlushGlass()
{
	if (_eye_frame != nullptr) {
		/* A pass of its own of the frame: over all collected before it. */
		if (!_cab_glass_batch.empty()) _eye_frame->passes.push_back({std::move(_cab_glass_batch), nullptr});
		_cab_glass_batch.clear();
		return;
	}
	const std::vector<CabBatchTriangle> *batch = &_cab_glass_batch;
	CabRasterLists({&batch, 1});
	_cab_glass_batch.clear();
}

/** While it lives, the triangles drawn are collected; at its end drawn on every core (CabFlushBatch()). */
struct CabBatchScope {
	CabBatchScope() { _cab_batching = true; }
	~CabBatchScope()
	{
		_cab_batching = false;
		CabFlushBatch();
	}
	CabBatchScope(const CabBatchScope &) = delete;
	CabBatchScope &operator=(const CabBatchScope &) = delete;
};

/** A triangle of the picture: drawn now, or collected to be drawn on every core (CabBatchScope). */
static void CabRasterTriangle(uint32_t *dst, float *zb, int w, int h, const uint32_t *tex, int tw, int th, const CabVertex &a, const CabVertex &b, const CabVertex &c, uint8_t fill_holes, uint light = 256)
{
	if (_eye_frame != nullptr) {
		/* Into the frame being collected (the glass apart, laid over at CabFlushGlass()). */
		if (_cab_glass_alpha < 256) {
			_cab_glass_batch.push_back({dst, zb, w, h, tex, tw, th, a, b, c, fill_holes, light, _cab_glass_alpha});
		} else {
			_eye_frame->Add({dst, zb, w, h, tex, tw, th, a, b, c, fill_holes, light, 256});
		}
		return;
	}
	if (_cab_batching) {
		(_cab_glass_alpha < 256 ? _cab_glass_batch : _cab_batch).push_back({dst, zb, w, h, tex, tw, th, a, b, c, fill_holes, light, _cab_glass_alpha});
		return;
	}
	CabRasterRows(dst, zb, w, h, tex, tw, th, a, b, c, fill_holes, light, 0, h, _cab_glass_alpha);
}

/** Draw a polygon of the eye's frame, cut to the near plane, as a fan of triangles. */
static void CabDrawPolygon(uint32_t *dst, float *zb, int w, int h, const uint32_t *tex, int tw, int th, std::vector<CabPoint> poly, const CabEye &eye, uint8_t fill_holes, uint light = 256)
{
	CabClipNear(poly);
	if (poly.size() < 3) return;
	CabVertex v0 = CabProject(eye, poly[0]);
	CabVertex prev = CabProject(eye, poly[1]);
	for (size_t i = 2; i < poly.size(); i++) {
		CabVertex cur = CabProject(eye, poly[i]);
		CabRasterTriangle(dst, zb, w, h, tex, tw, th, v0, prev, cur, fill_holes, light);
		prev = cur;
	}
}

/**
 * A sprite as a texture at the normal zoom, with its child sprites: the sprite's top left at 0,0 of the texture.
 * Kept for the next frames when it has no children and no sub-sprite (a tree, a house, a vehicle's picture).
 */
static const CabTexture *CabSpriteTexture(const ParentSpriteToDraw &ps)
{
	const Sprite *spr = GetSprite(ps.image & SPRITE_MASK, SpriteType::Normal);
	int w = UnScaleByZoom(spr->width, ZoomLevel::Normal), h = UnScaleByZoom(spr->height, ZoomLevel::Normal);
	if (w <= 0 || h <= 0) return nullptr;
	bool cacheable = ps.first_child < 0 && ps.sub == nullptr;
	uint64_t key = (static_cast<uint64_t>(ps.image) << 32) ^ static_cast<uint64_t>(ps.pal);
	if (cacheable) {
		auto it = _cab_textures.find(key);
		if (it != _cab_textures.end()) {
			it->second.used = ++_cab_texture_clock;
			return &it->second;
		}
		if (_cab_textures.size() >= CAB_TEXTURES_KEPT) {
			/* The oldest half goes. */
			std::vector<std::pair<uint64_t, uint64_t>> ages;
			for (const auto &e : _cab_textures) ages.emplace_back(e.second.used, e.first);
			std::sort(ages.begin(), ages.end());
			for (size_t i = 0; i < ages.size() / 2; i++) _cab_textures.erase(ages[i].second);
		}
	}
	static CabTexture scratch;
	CabTexture &t = cacheable ? _cab_textures[key] : EyeScratchTexture(scratch);
	t.w = w;
	t.h = h;
	t.used = ++_cab_texture_clock;
	t.px.assign(static_cast<size_t>(w) * h, 0);
	/* The texture's origin on the whole pixel at or before the sprite's top left, so the sprite lands at 0,0. */
	int mask = ScaleByZoom(-1, ZoomLevel::Normal);
	t.ox = ps.left & mask;
	t.oy = ps.top & mask;
	DrawPixelInfo tdpi{
		.dst_ptr = t.px.data(),
		.left = t.ox,
		.top = t.oy,
		.width = ScaleByZoom(w, ZoomLevel::Normal),
		.height = ScaleByZoom(h, ZoomLevel::Normal),
		.pitch = w,
		.zoom = ZoomLevel::Normal
	};
	AutoRestoreBackup dpi_backup(_cur_dpi, &tdpi);
	AutoRestoreBackup anim_backup(_screen_disable_anim, true);
	DrawSpriteViewport(ps.image, ps.pal, ps.x, ps.y, ps.sub);
	int child_idx = ps.first_child;
	while (child_idx >= 0) {
		const ChildScreenSpriteToDraw &cs = _vd.child_screen_sprites_to_draw[child_idx];
		child_idx = cs.next;
		if (cs.relative) {
			DrawSpriteViewport(cs.image, cs.pal, ps.left + cs.x, ps.top + cs.y, cs.sub);
		} else {
			DrawSpriteViewport(cs.image, cs.pal, ps.x + cs.x, ps.y + cs.y, cs.sub);
		}
	}
	if (_debug_misc_level >= 6 && cacheable) {
		/* The rig: the texture as a PPM in the working directory. */
		auto f = FileHandle::Open(fmt::format("cab_tex_{}.ppm", ps.image & SPRITE_MASK), "wb");
		if (f.has_value()) {
			std::string head = fmt::format("P6\n{} {}\n255\n", w, h);
			fwrite(head.data(), 1, head.size(), *f);
			for (uint32_t p : t.px) {
				Colour c(p);
				uint8_t rgb[3] = {c.r, c.g, c.b};
				fwrite(rgb, 1, 3, *f);
			}
		}
	}
	return &t;
}

/**
 * The rig: a sprite as it is drawn at the normal zoom, written as a PPM in the working directory (test3d sprite);
 * where it has nothing, magenta. Says the sprite's size and offsets in the game's virtual units.
 */
std::string WriteSpritePPM(SpriteID image, const std::string &name)
{
	if (BlitterFactory::GetCurrentBlitter()->GetScreenDepth() != 32) return "not a 32bpp blitter";
	const Sprite *spr = GetSprite(image & SPRITE_MASK, SpriteType::Normal);
	int w = UnScaleByZoom(spr->width, ZoomLevel::Normal), h = UnScaleByZoom(spr->height, ZoomLevel::Normal);
	if (w <= 0 || h <= 0) return "empty sprite";
	std::vector<uint32_t> px(static_cast<size_t>(w) * h, 0);
	int mask = ScaleByZoom(-1, ZoomLevel::Normal);
	DrawPixelInfo tdpi{
		.dst_ptr = px.data(),
		.left = spr->x_offs & mask,
		.top = spr->y_offs & mask,
		.width = ScaleByZoom(w, ZoomLevel::Normal),
		.height = ScaleByZoom(h, ZoomLevel::Normal),
		.pitch = w,
		.zoom = ZoomLevel::Normal
	};
	{
		AutoRestoreBackup dpi_backup(_cur_dpi, &tdpi);
		AutoRestoreBackup anim_backup(_screen_disable_anim, true);
		DrawSpriteViewport(image, PAL_NONE, 0, 0, nullptr);
	}
	auto f = FileHandle::Open(name, "wb");
	if (!f.has_value()) return "cannot write " + name;
	std::string head = fmt::format("P6\n{} {}\n255\n", w, h);
	fwrite(head.data(), 1, head.size(), *f);
	for (uint32_t p : px) {
		Colour c(p);
		uint8_t rgb[3] = {c.r, c.g, c.b};
		if (p == 0) rgb[0] = rgb[2] = 255, rgb[1] = 0;
		fwrite(rgb, 1, 3, *f);
	}
	return fmt::format("{}: {}x{} px, offsets {},{} (virtual), origin {},{}", name, w, h, spr->x_offs, spr->y_offs, tdpi.left, tdpi.top);
}

/**
 * A sprite as a box in perspective: its bounding box, the sprite's pixels laid over the top and the two walls the
 * sprite shows (facing south-west and south-east), and over the two it never shows mirrored from those -- as the
 * turned view does it (DrawTurnedBoxes()), but at any angle. Every face's corner takes the texture pixel of the
 * world point where the game draws it north-up (RemapCoords()).
 * @param ps the sprite with its box
 */
static void CabDrawBox(uint32_t *dst, float *zb, int w, int h, const CabTexture &tex, const CabEye &eye, const ParentSpriteToDraw &ps)
{
	const double x0 = ps.xmin, x1 = ps.xmax + 1, y0 = ps.ymin, y1 = ps.ymax + 1, z0 = ps.zmin, z1 = ps.zmax + 1;
	const double scale = 1.0 / ZOOM_BASE;
	auto at = [&](double wx, double wy, double wz, double tx, double ty, double tz) {
		double cx, cy;
		RemapCoordsF(tx, ty, tz, cx, cy);
		return CabToEye(eye, wx, wy, wz, (cx - tex.ox) * scale, (cy - tex.oy) * scale);
	};
	auto same = [&](double x, double y, double z) { return at(x, y, z, x, y, z); };
	auto face = [&](std::vector<CabPoint> poly) { CabDrawPolygon(dst, zb, w, h, tex.px.data(), tex.w, tex.h, std::move(poly), eye, true); };
	/* The top. */
	face({same(x0, y0, z1), same(x0, y1, z1), same(x1, y1, z1), same(x1, y0, z1)});
	/* The walls the sprite shows: facing +X (south-west) and +Y (south-east). */
	face({same(x1, y0, z0), same(x1, y1, z0), same(x1, y1, z1), same(x1, y0, z1)});
	face({same(x0, y1, z0), same(x1, y1, z0), same(x1, y1, z1), same(x0, y1, z1)});
	/* The two it never shows take theirs mirrored through the middle. */
	face({at(x0, y0, z0, x1, y0, z0), at(x0, y1, z0, x1, y1, z0), at(x0, y1, z1, x1, y1, z1), at(x0, y0, z1, x1, y0, z1)});
	face({at(x0, y0, z0, x0, y1, z0), at(x1, y0, z0, x1, y1, z0), at(x1, y0, z1, x1, y1, z1), at(x0, y0, z1, x0, y1, z1)});
}

/** A thing to draw standing: a sprite as a billboard facing the eye, a wall along a thin long box (a fence), or a box. */
struct CabBillboard {
	double f;          ///< its depth, for the order
	double bx, by, bz; ///< the world point its foot stands on (z in the game's units)
	int kind;          ///< 0 a billboard facing the eye; 1 a wall along x from w0 to w1; 2 a wall along y; 3 a box of its bounding box (a building, a platform)
	double w0, w1;     ///< the wall's ends along its axis
	ParentSpriteToDraw ps;
};

/** The picture being drawn: its pixels and their depths. */
struct CabCanvas {
	uint32_t *dst;
	float *zb;
	int w, h;
};

/** A polygon of world points (z in the game's units) in one colour. */
static void CabDrawSolid(const CabCanvas &cv, const CabEye &eye, std::initializer_list<std::array<double, 3>> pts, Colour colour)
{
	uint32_t px = colour.data;
	const uint32_t *tex = EyeColour(px);
	std::vector<CabPoint> poly;
	for (const auto &p : pts) poly.push_back(CabToEye(eye, p[0], p[1], p[2], 0.5, 0.5));
	CabDrawPolygon(cv.dst, cv.zb, cv.w, cv.h, tex, 1, 1, std::move(poly), eye, false);
}

/** A colour darker or lighter: each part times the factor. */
static Colour CabShade(Colour c, double f)
{
	auto part = [f](uint8_t v) { return static_cast<uint8_t>(std::clamp(v * f, 0.0, 255.0)); };
	return Colour(part(c.r), part(c.g), part(c.b));
}

/** Paint a sprite with its children into the current DrawPixelInfo, as the viewport draws it. */
static void CabPaintParent(const ParentSpriteToDraw &ps)
{
	DrawSpriteViewport(ps.image, ps.pal, ps.x, ps.y, ps.sub);
	int child_idx = ps.first_child;
	while (child_idx >= 0) {
		const ChildScreenSpriteToDraw &cs = _vd.child_screen_sprites_to_draw[child_idx];
		child_idx = cs.next;
		if (cs.relative) {
			DrawSpriteViewport(cs.image, cs.pal, ps.left + cs.x, ps.top + cs.y, cs.sub);
		} else {
			DrawSpriteViewport(cs.image, cs.pal, ps.x + cs.x, ps.y + cs.y, cs.sub);
		}
	}
}

/**
 * Sprites painted together into one texture, in the order given, each with its children: the pieces of a bridge seen
 * the way the north-up view shows them, to be laid over its deck and sides.
 * @param[out] t the texture; empty when nothing is painted
 */
static void CabCompositeTexture(CabTexture &t, const std::vector<const ParentSpriteToDraw *> &list)
{
	t.w = t.h = 0;
	t.px.clear();
	int l = INT32_MAX, tp = INT32_MAX, r = INT32_MIN, b = INT32_MIN;
	auto add = [&](SpriteID image, int x, int y) {
		const Sprite *spr = GetSprite(image & SPRITE_MASK, SpriteType::Normal);
		l = std::min(l, x + spr->x_offs);
		tp = std::min(tp, y + spr->y_offs);
		r = std::max(r, x + spr->x_offs + spr->width);
		b = std::max(b, y + spr->y_offs + spr->height);
	};
	for (const ParentSpriteToDraw *ps : list) {
		add(ps->image, ps->x, ps->y);
		for (int child_idx = ps->first_child; child_idx >= 0;) {
			const ChildScreenSpriteToDraw &cs = _vd.child_screen_sprites_to_draw[child_idx];
			child_idx = cs.next;
			if (cs.relative) {
				add(cs.image, ps->left + cs.x, ps->top + cs.y);
			} else {
				add(cs.image, ps->x + cs.x, ps->y + cs.y);
			}
		}
	}
	if (l >= r || tp >= b) return;
	const int mask = ScaleByZoom(-1, ZoomLevel::Normal);
	t.ox = l & mask;
	t.oy = tp & mask;
	t.w = UnScaleByZoom(r - t.ox, ZoomLevel::Normal) + 1;
	t.h = UnScaleByZoom(b - t.oy, ZoomLevel::Normal) + 1;
	if (static_cast<size_t>(t.w) * t.h > 4u * 1024 * 1024) {
		t.w = t.h = 0;
		return;
	}
	t.px.assign(static_cast<size_t>(t.w) * t.h, 0);
	DrawPixelInfo tdpi{
		.dst_ptr = t.px.data(),
		.left = t.ox,
		.top = t.oy,
		.width = ScaleByZoom(t.w, ZoomLevel::Normal),
		.height = ScaleByZoom(t.h, ZoomLevel::Normal),
		.pitch = t.w,
		.zoom = ZoomLevel::Normal
	};
	AutoRestoreBackup dpi_backup(_cur_dpi, &tdpi);
	AutoRestoreBackup anim_backup(_screen_disable_anim, true);
	for (const ParentSpriteToDraw *ps : list) CabPaintParent(*ps);
}

/** The average colour of what a texture has, or the fallback when it has nothing. */
static Colour CabAverage(const CabTexture &t, Colour fallback)
{
	uint64_t r = 0, g = 0, b = 0, n = 0;
	for (uint32_t p : t.px) {
		if (p == 0) continue;
		Colour c(p);
		r += c.r;
		g += c.g;
		b += c.b;
		n++;
	}
	if (n == 0) return fallback;
	return Colour(static_cast<uint8_t>(r / n), static_cast<uint8_t>(g / n), static_cast<uint8_t>(b / n));
}

/**
 * A quad of world points dressed in a texture of the north-up view: every corner takes the texture pixel where the
 * game draws the world point given with it (its own place, or a mirrored one for a face the view never shows).
 * @param pts the corners: where they are, and where their picture is
 * @param fill_holes take the nearest pixel where the texture has none (a surface), or leave a hole (a railing)
 */
static void CabDrawDressed(const CabCanvas &cv, const CabEye &eye, const CabTexture &tex, std::initializer_list<std::array<double, 6>> pts, bool fill_holes)
{
	if (tex.w <= 0 || tex.h <= 0) return;
	const double scale = 1.0 / ZOOM_BASE;
	std::vector<CabPoint> poly;
	for (const auto &p : pts) {
		double cx, cy;
		RemapCoordsF(p[3], p[4], p[5], cx, cy);
		poly.push_back(CabToEye(eye, p[0], p[1], p[2], (cx - tex.ox) * scale, (cy - tex.oy) * scale));
	}
	CabDrawPolygon(cv.dst, cv.zb, cv.w, cv.h, tex.px.data(), tex.w, tex.h, std::move(poly), eye, fill_holes);
}

/** How thick the deck of a bridge is, in units of height below the track: thin, as a vehicle passes a level under it. */
static constexpr int CAB_DECK = 2;

/**
 * Where along and across a bridge or tunnel a point of a tile is, in world units: along its axis and across it.
 * @param axis the bridge's or tunnel's axis
 */
static inline void CabAlongAcross(Axis axis, int x0, int y0, double a, double c, double &wx, double &wy)
{
	if (axis == Axis::X) {
		wx = x0 + a;
		wy = y0 + c;
	} else {
		wx = x0 + c;
		wy = y0 + a;
	}
}

/**
 * The part of a bridge over a tile, built from the sprites the game draws for it: the deck at the bridge's height
 * dressed in the floor sprite, the railings standing on its edges in the sprites of the far and the near side, the
 * deck's edges and its underside, and the pillars as columns where the game draws them.
 * @param tile the tile under the bridge
 * @param sprites the bridge's sprites on that tile (DrawBridgeMiddle())
 */
static void CabDrawBridgeMiddle(const CabCanvas &cv, const CabEye &eye, TileIndex tile, const std::vector<const ParentSpriteToDraw *> &sprites)
{
	const Axis axis = GetBridgeAxis(tile);
	const int bz = GetBridgePixelHeight(GetSouthernBridgeEnd(tile));
	const int x0 = TileX(tile) * TILE_SIZE, y0 = TileY(tile) * TILE_SIZE;
	std::vector<const ParentSpriteToDraw *> back, front, pillars;
	for (const ParentSpriteToDraw *ps : sprites) {
		if (ps->image == SPR_EMPTY_BOUNDING_BOX) continue;
		int c_min = axis == Axis::X ? ps->ymin - y0 : ps->xmin - x0;
		int c_ext = axis == Axis::X ? ps->ymax - ps->ymin + 1 : ps->xmax - ps->xmin + 1;
		if (ps->zmin < bz - CAB_DECK) {
			pillars.push_back(ps);
		} else if (c_ext <= 2 && c_min >= TILE_SIZE / 2) {
			front.push_back(ps);
		} else {
			back.push_back(ps);
		}
	}
	static CabTexture back_scratch, front_scratch;
	CabTexture &back_tex = EyeScratchTexture(back_scratch), &front_tex = EyeScratchTexture(front_scratch);
	CabCompositeTexture(back_tex, back);
	CabCompositeTexture(front_tex, front);

	/* Corners: along a, across c, height z; dressed in the pixel of (a, tc, tz). */
	auto pt = [&](double a, double c, double z, double tc, double tz) {
		std::array<double, 6> p;
		CabAlongAcross(axis, x0, y0, a, c, p[0], p[1]);
		p[2] = z;
		CabAlongAcross(axis, x0, y0, a, tc, p[3], p[4]);
		p[5] = tz;
		return p;
	};
	const double c_far = 1, c_near = TILE_SIZE - 1, top = bz + 40, under = bz - CAB_DECK;
	/* The near railing first: on its plane the deck's edge only fills what the railing leaves. */
	CabDrawDressed(cv, eye, front_tex, {pt(0, c_near, under, c_near, under), pt(TILE_SIZE, c_near, under, c_near, under), pt(TILE_SIZE, c_near, top, c_near, top), pt(0, c_near, top, c_near, top)}, false);
	CabDrawDressed(cv, eye, back_tex, {pt(0, c_near, under, c_near, under), pt(TILE_SIZE, c_near, under, c_near, under), pt(TILE_SIZE, c_near, bz, c_near, bz), pt(0, c_near, bz, c_near, bz)}, true);
	/* The deck. */
	CabDrawDressed(cv, eye, back_tex, {pt(0, c_far, bz, c_far, bz), pt(TILE_SIZE, c_far, bz, c_far, bz), pt(TILE_SIZE, c_near, bz, c_near, bz), pt(0, c_near, bz, c_near, bz)}, true);
	/* The far railing, and the deck's far edge in the near one's picture. */
	CabDrawDressed(cv, eye, back_tex, {pt(0, c_far, bz, c_far, bz), pt(TILE_SIZE, c_far, bz, c_far, bz), pt(TILE_SIZE, c_far, top, c_far, top), pt(0, c_far, top, c_far, top)}, false);
	CabDrawDressed(cv, eye, back_tex, {pt(0, c_far, under, c_near, under), pt(TILE_SIZE, c_far, under, c_near, under), pt(TILE_SIZE, c_far, bz, c_near, bz), pt(0, c_far, bz, c_near, bz)}, true);
	/* The underside, in the shadow. */
	Colour below = CabShade(CabAverage(back_tex, Colour(110, 100, 90)), 0.45);
	std::array<double, 2> u[4];
	CabAlongAcross(axis, x0, y0, 0, c_far, u[0][0], u[0][1]);
	CabAlongAcross(axis, x0, y0, TILE_SIZE, c_far, u[1][0], u[1][1]);
	CabAlongAcross(axis, x0, y0, TILE_SIZE, c_near, u[2][0], u[2][1]);
	CabAlongAcross(axis, x0, y0, 0, c_near, u[3][0], u[3][1]);
	CabDrawSolid(cv, eye, {{u[0][0], u[0][1], under - 0.01}, {u[1][0], u[1][1], under - 0.01}, {u[2][0], u[2][1], under - 0.01}, {u[3][0], u[3][1], under - 0.01}}, below);

	/* The pillars: a column where the sprite's pixels are, along the bridge, as thick as the pillar's box across it. */
	for (const ParentSpriteToDraw *ps : pillars) {
		const CabTexture *tex = CabSpriteTexture(*ps);
		if (tex == nullptr) continue;
		double sum = 0;
		int n = 0;
		for (int ty = 0; ty < tex->h; ty++) {
			for (int tx = 0; tx < tex->w; tx++) {
				if (tex->px[static_cast<size_t>(ty) * tex->w + tx] == 0) continue;
				sum += tex->ox + (tx + 0.5) * ZOOM_BASE;
				n++;
			}
		}
		if (n == 0) continue;
		double vx = sum / n;
		/* On the plane across the middle of the pillar's box: x - y = -vx / 8 there. */
		double c_mid = axis == Axis::X ? (ps->ymin + ps->ymax + 1) * 0.5 : (ps->xmin + ps->xmax + 1) * 0.5;
		double a_world = axis == Axis::X ? c_mid - vx / (2.0 * ZOOM_BASE) : c_mid + vx / (2.0 * ZOOM_BASE);
		double half = 1.25;
		double cx0 = c_mid - 1.25, cx1 = c_mid + 1.25;
		double z0 = ps->zmin, z1 = ps->zmax + 1;
		double bx0, by0, bx1, by1;
		if (axis == Axis::X) {
			bx0 = a_world - half; bx1 = a_world + half; by0 = cx0; by1 = cx1;
		} else {
			bx0 = cx0; bx1 = cx1; by0 = a_world - half; by1 = a_world + half;
		}
		auto same = [&](double x, double y, double z) { return std::array<double, 6>{x, y, z, x, y, z}; };
		auto at = [&](double x, double y, double z, double tx, double ty, double tz) { return std::array<double, 6>{x, y, z, tx, ty, tz}; };
		/* The two faces the sprite shows, the two others mirrored, as CabDrawBox() does. */
		CabDrawDressed(cv, eye, *tex, {same(bx1, by0, z0), same(bx1, by1, z0), same(bx1, by1, z1), same(bx1, by0, z1)}, true);
		CabDrawDressed(cv, eye, *tex, {same(bx0, by1, z0), same(bx1, by1, z0), same(bx1, by1, z1), same(bx0, by1, z1)}, true);
		CabDrawDressed(cv, eye, *tex, {at(bx0, by0, z0, bx1, by0, z0), at(bx0, by1, z0, bx1, by1, z0), at(bx0, by1, z1, bx1, by1, z1), at(bx0, by0, z1, bx1, by0, z1)}, true);
		CabDrawDressed(cv, eye, *tex, {at(bx0, by0, z0, bx0, by1, z0), at(bx1, by0, z0, bx1, by1, z0), at(bx1, by0, z1, bx1, by1, z1), at(bx0, by0, z1, bx0, by1, z1)}, true);
	}
}

/**
 * The ramp of a bridge: the deck rising from the ground to the bridge (or level with it on a foundation), dressed
 * in the ramp's sprite, with its sides down to the ground and its end under the bridge.
 * @param head the bridge's end
 * @param sprites what the game draws standing on it
 */
static void CabDrawBridgeRamp(const CabCanvas &cv, const CabEye &eye, TileIndex head, const std::vector<const ParentSpriteToDraw *> &sprites)
{
	if (sprites.empty()) return;
	const DiagDirection dir = GetTunnelBridgeDirection(head);
	const Axis axis = DiagDirToAxis(dir);
	const int bz = GetBridgePixelHeight(head);
	const int x0 = TileX(head) * TILE_SIZE, y0 = TileY(head) * TILE_SIZE;
	/* Along the axis: the end towards the bridge (inner) and the one towards the road or rail (outer). */
	const bool inner_low = dir == DiagDirection::NE || dir == DiagDirection::NW;
	const double a_in = inner_low ? 0 : TILE_SIZE, a_out = inner_low ? TILE_SIZE : 0;
	double sx, sy;
	CabAlongAcross(axis, x0, y0, inner_low ? TILE_SIZE - 1 : 0, TILE_SIZE / 2, sx, sy);
	int z_out = GetSlopePixelZ(static_cast<int>(sx), static_cast<int>(sy), true);
	const double deck_out = (bz - z_out >= TILE_HEIGHT / 2) ? bz - TILE_HEIGHT : bz;
	const double deck_in = bz;
	auto ground = [&](double a, double c) {
		double wx, wy;
		CabAlongAcross(axis, x0, y0, std::clamp(a, 0.0, TILE_SIZE - 1.0), std::clamp(c, 0.0, TILE_SIZE - 1.0), wx, wy);
		return static_cast<double>(GetSlopePixelZ(static_cast<int>(wx), static_cast<int>(wy), false));
	};
	static CabTexture tex_scratch;
	CabTexture &tex = EyeScratchTexture(tex_scratch);
	CabCompositeTexture(tex, sprites);
	auto pt = [&](double a, double c, double z, double tc, double tz) {
		std::array<double, 6> p;
		CabAlongAcross(axis, x0, y0, a, c, p[0], p[1]);
		p[2] = z;
		CabAlongAcross(axis, x0, y0, a, tc, p[3], p[4]);
		p[5] = tz;
		return p;
	};
	const double c_far = 1, c_near = TILE_SIZE - 1;
	/* The deck. */
	CabDrawDressed(cv, eye, tex, {pt(a_out, c_far, deck_out, c_far, deck_out), pt(a_in, c_far, deck_in, c_far, deck_in), pt(a_in, c_near, deck_in, c_near, deck_in), pt(a_out, c_near, deck_out, c_near, deck_out)}, true);
	/* Its sides down to the ground: the near one as the sprite shows it, the far one mirrored. */
	for (double c : {c_near, c_far}) {
		double g_out = std::min(ground(a_out, c), deck_out), g_in = std::min(ground(a_in, c), deck_in - CAB_DECK);
		CabDrawDressed(cv, eye, tex, {pt(a_out, c, g_out, c_near, g_out), pt(a_in, c, g_in, c_near, g_in), pt(a_in, c, deck_in, c_near, deck_in), pt(a_out, c, deck_out, c_near, deck_out)}, true);
	}
	/* The end under the bridge, in the shadow; and the outer end when the ramp stands on a foundation. */
	Colour wall = CabShade(CabAverage(tex, Colour(120, 110, 96)), 0.6);
	double w[4][2];
	CabAlongAcross(axis, x0, y0, a_in, c_far, w[0][0], w[0][1]);
	CabAlongAcross(axis, x0, y0, a_in, c_near, w[1][0], w[1][1]);
	CabAlongAcross(axis, x0, y0, a_out, c_far, w[2][0], w[2][1]);
	CabAlongAcross(axis, x0, y0, a_out, c_near, w[3][0], w[3][1]);
	double gi = std::min(ground(a_in, c_far), ground(a_in, c_near));
	CabDrawSolid(cv, eye, {{w[0][0], w[0][1], gi}, {w[1][0], w[1][1], gi}, {w[1][0], w[1][1], deck_in}, {w[0][0], w[0][1], deck_in}}, wall);
	double go = std::min(ground(a_out, c_far), ground(a_out, c_near));
	if (deck_out > go + 1) CabDrawSolid(cv, eye, {{w[2][0], w[2][1], go}, {w[3][0], w[3][1], go}, {w[3][0], w[3][1], deck_out}, {w[2][0], w[2][1], deck_out}}, wall);
}

/** A tunnel in the cab view: its two ends and where its portals stand. */
struct CabTunnel {
	TileIndex a, b;   ///< its two ends
	Axis axis;        ///< which way it runs
	int z;            ///< the height of its floor
	double s_lo, s_hi; ///< the planes of its portals along the axis, world units
	double c_mid;     ///< the middle across it, world units
	bool rail;        ///< a railway tunnel (else a road one)
};

/** Half the width of a tunnel's floor, world units. */
static constexpr double CAB_TUNNEL_HALF = 6.0;
/** The height of a tunnel's walls and of its vault, units of height above its floor. */
static constexpr double CAB_TUNNEL_WALL = 10.0;
static constexpr double CAB_TUNNEL_TOP = 20.5;
/** The height of a portal's face above the floor; and how far it reaches beside the opening, world units. */
static constexpr double CAB_PORTAL_TOP = 24.0;
static constexpr double CAB_PORTAL_HALF = 10.0;
/** The cross-section of a tunnel: across from its middle (world units), up from its floor (units of height). */
static const std::array<std::array<double, 2>, 9> _cab_tunnel_profile = {{
	{-CAB_TUNNEL_HALF, 0}, {-CAB_TUNNEL_HALF, CAB_TUNNEL_WALL}, {-5.2, 15.0}, {-3.0, 19.0}, {0.0, CAB_TUNNEL_TOP},
	{3.0, 19.0}, {5.2, 15.0}, {CAB_TUNNEL_HALF, CAB_TUNNEL_WALL}, {CAB_TUNNEL_HALF, 0},
}};

/** The tunnel with an end on the tile, if there is one. */
static std::optional<CabTunnel> CabTunnelAt(TileIndex tile)
{
	if (!IsValidTile(tile) || !IsTileType(tile, TileType::TunnelBridge) || !IsTunnel(tile)) return std::nullopt;
	CabTunnel t;
	t.a = tile;
	t.b = GetOtherTunnelEnd(tile);
	t.axis = DiagDirToAxis(GetTunnelBridgeDirection(tile));
	t.z = GetTilePixelZ(tile);
	t.rail = GetTunnelBridgeTransportType(tile) == TransportType::Rail;
	/* A portal stands on the edge of its end tile towards the other end, where the hill starts. */
	auto plane = [&](TileIndex end) {
		DiagDirection d = GetTunnelBridgeDirection(end);
		int base = (t.axis == Axis::X ? TileX(end) : TileY(end)) * TILE_SIZE;
		return (d == DiagDirection::NE || d == DiagDirection::NW) ? static_cast<double>(base) : static_cast<double>(base + TILE_SIZE);
	};
	double pa = plane(t.a), pb = plane(t.b);
	t.s_lo = std::min(pa, pb);
	t.s_hi = std::max(pa, pb);
	t.c_mid = (t.axis == Axis::X ? TileY(tile) : TileX(tile)) * TILE_SIZE + TILE_SIZE / 2.0;
	return t;
}

/** A world point of a tunnel: along its axis s, across c from its middle, dz above its floor. */
static inline std::array<double, 3> CabTunnelPoint(const CabTunnel &t, double s, double c, double dz)
{
	if (t.axis == Axis::X) return {s, t.c_mid + c, t.z + dz};
	return {t.c_mid + c, s, t.z + dz};
}

/** The stones of portals and of the walls beside the way into a tunnel, 4 texture pixels to a world unit. */
static const CabTexture &CabStoneTexture()
{
	static CabTexture stone;
	if (!stone.px.empty()) return stone;
	stone.w = 96;
	stone.h = 48;
	stone.px.resize(static_cast<size_t>(stone.w) * stone.h);
	uint32_t seed = 12345;
	for (int y = 0; y < stone.h; y++) {
		int course = y / 6;
		for (int x = 0; x < stone.w; x++) {
			seed = seed * 1103515245 + 12345;
			int noise = static_cast<int>((seed >> 16) % 21) - 10;
			bool joint = (y % 6) == 5 || ((x + (course & 1) * 6) % 12) == 11;
			int base = joint ? 82 : 128 + ((course * 7 + (x + (course & 1) * 6) / 12 * 13) % 17) - 8;
			stone.px[static_cast<size_t>(y) * stone.w + x] = Colour(static_cast<uint8_t>(std::clamp(base + noise + 6, 0, 255)), static_cast<uint8_t>(std::clamp(base + noise, 0, 255)), static_cast<uint8_t>(std::clamp(base + noise - 10, 0, 255))).data;
		}
	}
	return stone;
}

/** A quad of world points dressed in the stones: (x, y, z, tu, tv) at each corner, the texture's pixels. */
static void CabDrawStone(const CabCanvas &cv, const CabEye &eye, std::initializer_list<std::array<double, 5>> pts)
{
	const CabTexture &stone = CabStoneTexture();
	std::vector<CabPoint> poly;
	for (const auto &p : pts) poly.push_back(CabToEye(eye, p[0], p[1], p[2], std::clamp(p[3], 0.0, stone.w - 0.01), std::clamp(p[4], 0.0, stone.h - 0.01)));
	CabDrawPolygon(cv.dst, cv.zb, cv.w, cv.h, stone.px.data(), stone.w, stone.h, std::move(poly), eye, false);
}

/**
 * A portal of a tunnel: a face of stones on the edge of its end tile, around an opening as tall as the tunnel, and
 * walls of stones beside the way into it, up to the hill on either side.
 * @param end the tunnel's end the portal is on
 */
static void CabDrawPortal(const CabCanvas &cv, const CabEye &eye, const CabTunnel &t, TileIndex end)
{
	DiagDirection d = GetTunnelBridgeDirection(end);
	int base = (t.axis == Axis::X ? TileX(end) : TileY(end)) * TILE_SIZE;
	bool into_low = d == DiagDirection::NE || d == DiagDirection::NW;
	double s = into_low ? base : base + TILE_SIZE;
	double s_out = into_low ? base + TILE_SIZE : base;
	const double k = 4.0; // texture pixels to a world unit
	auto face = [&](double c, double dz) {
		std::array<double, 3> p = CabTunnelPoint(t, s, c, dz);
		return std::array<double, 5>{p[0], p[1], p[2], (c + CAB_PORTAL_HALF) * k, (CAB_PORTAL_TOP - dz) * CAB_Z_SCALE * k};
	};
	/* Beside the opening, and over it along the vault. */
	CabDrawStone(cv, eye, {face(-CAB_PORTAL_HALF, 0), face(-CAB_TUNNEL_HALF, 0), face(-CAB_TUNNEL_HALF, CAB_PORTAL_TOP), face(-CAB_PORTAL_HALF, CAB_PORTAL_TOP)});
	CabDrawStone(cv, eye, {face(CAB_TUNNEL_HALF, 0), face(CAB_PORTAL_HALF, 0), face(CAB_PORTAL_HALF, CAB_PORTAL_TOP), face(CAB_TUNNEL_HALF, CAB_PORTAL_TOP)});
	for (size_t i = 1; i + 2 < _cab_tunnel_profile.size(); i++) {
		const auto &p = _cab_tunnel_profile[i], &q = _cab_tunnel_profile[i + 1];
		CabDrawStone(cv, eye, {face(p[0], p[1]), face(q[0], q[1]), face(q[0], CAB_PORTAL_TOP), face(p[0], CAB_PORTAL_TOP)});
	}
	/* The walls beside the way in: from the floor up to where the slope of the end tile was, on both sides. */
	for (double c : {-(TILE_SIZE / 2.0), TILE_SIZE / 2.0}) {
		std::array<double, 3> p0 = CabTunnelPoint(t, s_out, c, 0), p1 = CabTunnelPoint(t, s, c, 0), p2 = CabTunnelPoint(t, s, c, TILE_HEIGHT);
		CabDrawStone(cv, eye, {{p0[0], p0[1], p0[2], 0, 10}, {p1[0], p1[1], p1[2], TILE_SIZE * k, 10}, {p2[0], p2[1], p2[2], TILE_SIZE * k, 10 - TILE_HEIGHT * CAB_Z_SCALE * k}});
	}
}

/**
 * The inside of a tunnel from one plane along it to another: walls and vault in pieces, lit from its ends and dark
 * inside with lamps along the walls, the track or the road on its floor.
 * @param s0,s1 where the piece begins and ends along the tunnel
 */
static void CabDrawTube(const CabCanvas &cv, const CabEye &eye, const CabTunnel &t, double s0, double s1)
{
	const double step = 4.0;
	const Colour vault(112, 104, 92), floor = t.rail ? Colour(92, 84, 74) : Colour(78, 78, 80), rail(176, 176, 182), lamp(255, 228, 150);
	int k = 0;
	for (double sa = s0; sa < s1 - 0.01; sa += step, k++) {
		double sb = std::min(s1, sa + step);
		double sm = (sa + sb) * 0.5;
		/* Daylight from the nearer portal fades into the dark. */
		double from_end = std::min(std::abs(sm - t.s_lo), std::abs(t.s_hi - sm));
		double light = 0.10 + 0.90 * std::exp(-from_end / 10.0);
		if ((static_cast<int>(std::floor(sm / step)) & 1) != 0) light *= 0.85;
		Colour wall = CabShade(vault, light);
		for (size_t i = 0; i + 1 < _cab_tunnel_profile.size(); i++) {
			const auto &p = _cab_tunnel_profile[i], &q = _cab_tunnel_profile[i + 1];
			auto a = CabTunnelPoint(t, sa, p[0], p[1]), b = CabTunnelPoint(t, sb, p[0], p[1]), c = CabTunnelPoint(t, sb, q[0], q[1]), d = CabTunnelPoint(t, sa, q[0], q[1]);
			CabDrawSolid(cv, eye, {a, b, c, d}, wall);
		}
		auto f0 = CabTunnelPoint(t, sa, -CAB_TUNNEL_HALF, 0), f1 = CabTunnelPoint(t, sb, -CAB_TUNNEL_HALF, 0), f2 = CabTunnelPoint(t, sb, CAB_TUNNEL_HALF, 0), f3 = CabTunnelPoint(t, sa, CAB_TUNNEL_HALF, 0);
		CabDrawSolid(cv, eye, {f0, f1, f2, f3}, CabShade(floor, light));
		if (t.rail) {
			/* The rails, catching the light longer than the stones. */
			for (double c : {-1.6, 1.6}) {
				auto r0 = CabTunnelPoint(t, sa, c - 0.2, 0.6), r1 = CabTunnelPoint(t, sb, c - 0.2, 0.6), r2 = CabTunnelPoint(t, sb, c + 0.2, 0.6), r3 = CabTunnelPoint(t, sa, c + 0.2, 0.6);
				CabDrawSolid(cv, eye, {r0, r1, r2, r3}, CabShade(rail, std::min(1.0, light * 1.8 + 0.15)));
			}
		} else if ((k & 1) == 0) {
			/* The line down the middle of the road. */
			auto l0 = CabTunnelPoint(t, sa, -0.15, 0.3), l1 = CabTunnelPoint(t, sa + step * 0.5, -0.15, 0.3), l2 = CabTunnelPoint(t, sa + step * 0.5, 0.15, 0.3), l3 = CabTunnelPoint(t, sa, 0.15, 0.3);
			CabDrawSolid(cv, eye, {l0, l1, l2, l3}, CabShade(Colour(220, 220, 200), std::min(1.0, light * 1.5 + 0.2)));
		}
		/* A lamp on each wall every few pieces. */
		if (static_cast<int>(std::floor(sm / step)) % 6 == 0) {
			for (double c : {-CAB_TUNNEL_HALF + 0.05, CAB_TUNNEL_HALF - 0.05}) {
				auto m0 = CabTunnelPoint(t, sm - 0.6, c, 8.0), m1 = CabTunnelPoint(t, sm + 0.6, c, 8.0), m2 = CabTunnelPoint(t, sm + 0.6, c, 9.5), m3 = CabTunnelPoint(t, sm - 0.6, c, 9.5);
				CabDrawSolid(cv, eye, {m0, m1, m2, m3}, lamp);
			}
		}
	}
}

/**
 * A tunnel seen from outside, through the opening of a portal: its tube as far as the view reaches and the daylight
 * at its other end, only where the opening is seen. Drawn into a picture of its own first and put where the opening
 * shows, as the opening's plane, so the hill over a tunnel never shows in it -- a hill a level high is lower than
 * the vault -- and the vault never shows out of the hill.
 * @param end the tunnel's end whose portal is seen
 */
static void CabDrawTunnelSeenIn(const CabCanvas &cv, const CabEye &eye, const CabTunnel &t, TileIndex end)
{
	DiagDirection d = GetTunnelBridgeDirection(end);
	int base = (t.axis == Axis::X ? TileX(end) : TileY(end)) * TILE_SIZE;
	bool into_low = d == DiagDirection::NE || d == DiagDirection::NW;
	double s = into_low ? base : base + TILE_SIZE;
	double s_eye = t.axis == Axis::X ? eye.x : eye.y;
	if (into_low ? s_eye <= s : s_eye >= s) return; // not in front of this portal
	const size_t n = static_cast<size_t>(cv.w) * cv.h;
	/* Its own canvases, drawn now, not into the frame (of the frame while one is collected, so that two portals seen
	 * do not share them); what is seen through the opening is laid over the picture at its place in the frame. */
	static EyeCanvas scratch_tube, scratch_open;
	EyeCanvas &tube_canvas = _eye_frame != nullptr ? _eye_frame->canvases.emplace_back() : scratch_tube;
	EyeCanvas &open_canvas = _eye_frame != nullptr ? _eye_frame->canvases.emplace_back() : scratch_open;
	EyeFrame *frame = _eye_frame;
	{
		AutoRestoreBackup drawn_now(_eye_frame, static_cast<EyeFrame *>(nullptr));
		tube_canvas.px.assign(n, 0);
		tube_canvas.z.assign(n, 0.0f);
		open_canvas.px.assign(n, 0);
		open_canvas.z.assign(n, 0.0f);
		/* The opening: where it is seen, and how near. */
		const CabCanvas open{open_canvas.px.data(), open_canvas.z.data(), cv.w, cv.h};
		std::vector<CabPoint> hole;
		for (const auto &p : _cab_tunnel_profile) {
			auto w = CabTunnelPoint(t, s, p[0], p[1]);
			hole.push_back(CabToEye(eye, w[0], w[1], w[2], 0.5, 0.5));
		}
		uint32_t one = 1;
		CabDrawPolygon(open.dst, open.zb, open.w, open.h, &one, 1, 1, hole, eye, false);
		/* The tube, as far as the view reaches, and the daylight in the far portal. */
		const CabCanvas tube{tube_canvas.px.data(), tube_canvas.z.data(), cv.w, cv.h};
		double reach = CAB_RANGE_TILES * TILE_SIZE;
		double far = into_low ? t.s_lo : t.s_hi;
		if (into_low) {
			CabDrawTube(tube, eye, t, std::max(t.s_lo, s - reach), s);
		} else {
			CabDrawTube(tube, eye, t, s, std::min(t.s_hi, s + reach));
		}
		if (std::abs(far - s) <= reach) {
			std::vector<CabPoint> light;
			for (const auto &p : _cab_tunnel_profile) {
				auto w = CabTunnelPoint(t, far, p[0], p[1]);
				light.push_back(CabToEye(eye, w[0], w[1], w[2], 0.5, 0.5));
			}
			uint32_t day = Colour(205, 225, 240).data;
			CabDrawPolygon(tube.dst, tube.zb, tube.w, tube.h, &day, 1, 1, light, eye, false);
		}
	}
	auto lay = [&tube_canvas, &open_canvas, n](uint32_t *dst, float *zb) {
		for (size_t i = 0; i < n; i++) {
			if (open_canvas.z[i] <= 0.0f || open_canvas.z[i] < zb[i] || tube_canvas.px[i] == 0) continue;
			dst[i] = tube_canvas.px[i];
			zb[i] = open_canvas.z[i];
		}
	};
	if (frame != nullptr) {
		frame->Job(lay);
	} else {
		lay(cv.dst, cv.zb);
	}
}

/** The cab view collects the sprites of a tile; DrawBridgeMiddle() says which of them are a bridge's. */
static bool _cab_collecting = false;

/** Whether the view from the eyes (a cab's, the walker's) is collecting what to draw. */
bool ViewportDrawingEye()
{
	return _cab_collecting;
}

/** Whether a 3D view is collecting what to draw: the turned view, facing north with the houses as objects, or the view from the eyes. */
bool ViewportDrawing3D()
{
	return _drawing_turned || _north_house_objects_now || _cab_collecting;
}

/** A figure standing in the view from the eyes (the clay town's mushrooms, DrawThemedTrees()): a billboard of its picture. */
struct CabFigure {
	std::string name; ///< the figure of the indices
	float size;       ///< its scale
	double x, y, z;   ///< where it stands, world units
	float facing;     ///< which way it faces, degrees as the figures count it
	int frame;        ///< its frame (a dragon's), 0 for one that does not move
	float stretch = 1.0f; ///< its height against its width (a plant's)
	bool plant = false;   ///< a plant of a field: a picture of a few angles and sizes shared by all of its kind (GetThreeDPlantPicture())
	const ThreeDVehicleObject *obj = nullptr; ///< a vehicle's object instead of a figure of the indices (AddCabVehicle())
	float along = 1.0f;   ///< a vehicle's length foreshortened as the game spaces its vehicles
	int state = 0;        ///< a vehicle's state (its paint)
	int lights = 0;       ///< a vehicle's lights on at night
	bool own_car = false; ///< a car of the game's own driving about (its lights are not a vehicle's of the game)
	bool solid = false;   ///< drawn by its triangles this picture (not as a picture of it)
	double rolled = 0;    ///< a vehicle's way gone, world units: its wheels turn by it
	uint64_t walk = 0;    ///< the frames of a vehicle's crew that walks
	double curvature = 0; ///< how much its way turns, radians a world unit: its front wheels steer by it
	bool own_cab = false; ///< the vehicle whose cab the eye is in: drawn round the eye, its driver not
	double pitch = 0;     ///< degrees its +x is raised over the level (an arrow flying, AddCabMissile())
	bool missile = false; ///< an arrow or a bolt: always by its triangles, however small
};
static std::vector<CabFigure> _cab_figures;
/** A model drawn by its triangles is as detailed as this many pixels between its corners (ThreeDVehicleShape::Simpler()). The rig: 'test3d detail <px>', 0 the whole shape. */
double _cab_detail_pixels = 2.0;
/** The rig: 'test3d detail <px> textura': its textures at their full size, whatever its distance. */
bool _cab_full_textures = false;
/** The size of the figures standing on the buildings and fields in the view from the eyes: true, as they are (twice it outside). */
static constexpr float CAB_FIGURE_SCALE = 1.0f;

/** A figure the tiles being collected for the view from the eyes have standing on them (DrawThemedTrees()). */
void AddCabFigure(std::string_view name, float size, int x, int y, int z, float facing, int frame)
{
	_cab_figures.push_back({std::string(name), size, static_cast<double>(x), static_cast<double>(y), static_cast<double>(z), facing, frame});
}

/**
 * An arrow or a bolt of the walker in the view from the eyes (first_person_gui.cpp), flying or stuck where it hit:
 * by its triangles, along its way.
 * @param name the thing of the indices ("sip", "sipka"), its +x its tip
 * @param x,y,z its origin (the nock), world units and units of height
 * @param facing degrees in the world its tip points to, 0 along +x, 90 along +y
 * @param pitch degrees its tip is raised over the level
 */
void AddCabMissile(std::string_view name, double x, double y, double z, float facing, float pitch)
{
	CabFigure f{std::string(name), 1.0f, x, y, z, facing, 0};
	f.pitch = pitch;
	f.missile = true;
	_cab_figures.push_back(std::move(f));
}

/**
 * A vehicle drawn from its object in the view from the eyes (GetThreeDEyeVehicle()), or a car of the game's own driving
 * about: among the figures, by its triangles near and as a picture at the angle the eye sees it farther.
 */
static void AddCabVehicle(const ThreeDEyeVehicle &ev, bool own_car)
{
	CabFigure f{std::string(), ev.scale, ev.x, ev.y, ev.z, ev.facing, 0};
	f.obj = ev.obj;
	f.along = ev.along;
	f.pitch = ev.pitch;
	f.state = ev.state;
	f.lights = ev.lights;
	f.own_car = own_car;
	f.rolled = ev.rolled;
	f.walk = ev.walk;
	f.curvature = ev.curvature;
	_cab_figures.push_back(std::move(f));
}

/** A car of the game's own driving about (three_d_walkers.h) in the view from the eyes. */
void AddCabCar(const ThreeDVehicleObject *obj, float scale, double x, double y, double z, float facing, bool lit)
{
	/* Its shape if it is read; else read in the background, the car there the next frame. */
	const ThreeDVehicleShape *shape = obj != nullptr ? obj->TryGetShape() : nullptr;
	if (shape == nullptr) return;
	ThreeDEyeVehicle ev;
	ev.obj = obj;
	ev.scale = scale;
	ev.x = x;
	ev.y = y;
	ev.z = z;
	ev.facing = facing;
	ev.lights = lit && shape->HasLights() ? 3 : 0;
	AddCabVehicle(ev, true);
}

/** The vehicle a figure of the view from the eyes is (AddCabVehicle()). */
static ThreeDEyeVehicle CabFigureVehicle(const CabFigure &fg)
{
	ThreeDEyeVehicle ev;
	ev.obj = fg.obj;
	ev.scale = fg.size;
	ev.along = fg.along;
	ev.x = fg.x;
	ev.y = fg.y;
	ev.z = fg.z;
	ev.facing = fg.facing;
	ev.pitch = static_cast<float>(fg.pitch);
	ev.state = fg.state;
	ev.lights = fg.lights;
	ev.walk = fg.walk;
	return ev;
}

/**
 * A plant of a field in the view from the eyes (the technologies of OpenMW, the player, 16. 10.: thousands of the
 * same plant cheaply): one model and a few pictures of it for all of them, each drawn where it stands -- the
 * pictures at a few angles and sizes (GetThreeDPlantPicture()), the model only for those at the walker's feet.
 */
static void AddCabPlant(std::string_view name, float scale, float stretch, double x, double y, double z, float facing)
{
	_cab_figures.push_back({std::string(name), scale, x, y, z, facing, 0, stretch, true});
}

/** What a figure drawn by its triangles needs, resolved while collecting (CabFigureReady(), the Try... getters): drawn as a job of the frame (CabDrawFigureModel()). */
struct CabFigureModel {
	const ThreeDVehicleObject *fig = nullptr;
	const ThreeDVehicleShape *whole = nullptr; ///< its shape
	const ThreeDVehicleShape *shape = nullptr; ///< as simple as its distance allows
	std::vector<const ThreeDObjectTexture *> texs; ///< its textures at that detail, painted as its state has them
	ThreeDParts parts = 1; ///< the parts its state shows
	double turn = 0;      ///< how far its state turns the whole model, degrees
	/** Its crew that walks, each at its frame. */
	struct Crew {
		const ThreeDWalkingCrew *w;
		const ThreeDVehicleShape *shape; ///< as simple as its distance allows
		std::vector<const ThreeDObjectTexture *> texs;
	};
	std::vector<Crew> crew;
};

/**
 * A figure near the eye by its triangles (a dragon before the walker, a mushroom by the way): in the world where it
 * stands, turned to where it faces, its frame, its textures, lit from its normals -- a billboard of its picture would
 * be coarse this near, and seen from above as the map sees it. What it needs is looked for here, under the lock, and
 * only if it is there already (the player, 17. 10., 4: "load in the background"): a shape or a texture not read yet is
 * read by the thread in the background, and the figure is drawn as its picture, or not at all, this frame.
 * @param[out] model what to draw it from
 * @return whether it can be drawn so this frame
 */
static bool CabFigureReady(const CabEye &eye, const CabFigure &fg, CabFigureModel &model)
{
	const ThreeDVehicleObject *fig = fg.obj != nullptr ? fg.obj : GetThreeDFigure(fg.name);
	if (fig == nullptr) return false;
	const ThreeDVehicleShape *whole = fig->TryGetShape(fg.frame, fig->PartsOf(fg.state));
	if (whole == nullptr || whole->tri.empty()) return false;
	const double across = fg.size * OBJECT_UNITS_PER_METRE;
	/* As detailed as it is big on the picture: its corners joined about two pixels apart (Simpler()); the cab the eye
	 * is in whole, and an arrow (its shaft thinner than that would leave nothing of it). */
	const CabPoint mid = CabToEye(eye, fg.x, fg.y, fg.z, 0, 0);
	const double pixels_per_metre = eye.focal * across / std::max(mid.f, CAB_NEAR);
	model.fig = fig;
	model.whole = whole;
	model.shape = fg.own_cab || fg.missile ? whole : whole->TrySimpler(_cab_detail_pixels / pixels_per_metre);
	/* The textures at the detail of its distance, a texel to a pixel (the player, 16. 10.: "save when they are far");
	 * a vehicle's body painted as its state has it, its add-ons coloured. */
	model.texs.assign(std::max<size_t>(1, whole->pngs.size()), nullptr);
	for (size_t i = 0; i < model.texs.size(); i++) {
		model.texs[i] = TryGetThreeDEyeVehicleTexture(fig, fg.state, _cab_full_textures ? 1e9 : pixels_per_metre, static_cast<int>(i));
		if (model.texs[i] == nullptr) return false;
	}
	/* The parts its state shows (an add-on's node, a group of the crew; the colleague's hra/3d/PROSBA-V3S-3D.md), and
	 * how far it turns the whole model about its origin (the colleague's hra/3d/PROSBA-SPECIAL-3D.md: a cart pushed). */
	model.parts = fig->PartsOf(fg.state);
	const ThreeDVehicleState *st = fig->State(fg.state);
	model.turn = st != nullptr ? st->turn : 0.0;
	/* Its crew that walks (the colleague's hra/3d/PROSBA-SPECIAL-3D.md): each figure the state shows at its frame of the
	 * way gone; one not read yet is left out this frame. */
	model.crew.clear();
	const std::vector<ThreeDWalkingCrew> &walking = fig->WalkingCrew();
	for (size_t k = 0; k < walking.size(); k++) {
		const ThreeDWalkingCrew &w = walking[k];
		if (!ThreeDVehicleShape::Shown(w.part, model.parts)) continue;
		const ThreeDVehicleShape *frame_shape = w.figure->TryGetShape(k < 8 ? static_cast<int>((fg.walk >> (8 * k)) & 0xFF) : 0);
		if (frame_shape == nullptr || frame_shape->tri.empty()) continue;
		const double crew_ppm = pixels_per_metre * w.scale;
		CabFigureModel::Crew crew{&w, fg.own_cab ? frame_shape : frame_shape->TrySimpler(_cab_detail_pixels / crew_ppm), {}};
		crew.texs.assign(std::max<size_t>(1, frame_shape->pngs.size()), nullptr);
		bool ready = true;
		for (size_t i = 0; i < crew.texs.size() && ready; i++) {
			crew.texs[i] = TryGetThreeDEyeVehicleTexture(w.figure, 0, _cab_full_textures ? 1e9 : crew_ppm, static_cast<int>(i));
			ready = crew.texs[i] != nullptr;
		}
		if (ready) model.crew.push_back(std::move(crew));
	}
	return true;
}

/**
 * Draw a figure by its triangles from what was resolved for it (CabFigureReady()): a job of the frame, off the lock.
 * @param wheels_turn whether the wheels of the models turn (gui.three_d_wheels)
 */
static void CabDrawFigureModel(uint32_t *dst, float *zb, int width, int height, const CabEye &eye, const CabFigure &fg, const CabFigureModel &model, bool wheels_turn)
{
	const ThreeDVehicleObject *fig = model.fig;
	const ThreeDVehicleShape *shape = model.shape;
	const std::vector<const ThreeDObjectTexture *> &texs = model.texs;
	const double a = fg.facing * M_PI / 180.0, ca = std::cos(a), sa = std::sin(a);
	const double pa = fg.pitch * M_PI / 180.0, pc = std::cos(pa), ps = std::sin(pa);
	const double across = fg.size * OBJECT_UNITS_PER_METRE, up = fg.size * fg.stretch * OBJECT_HEIGHT_PER_METRE;
	/* A vehicle foreshortened along itself as the game spaces its vehicles. */
	const double length = across * fg.along;
	const bool lit = shape->nrm.size() == shape->pos.size();
	/* The light from above, a little from the south-east of the map, as the houses have it. */
	static constexpr double L[3] = {-0.45, 0.35, 0.82};
	const ThreeDParts parts = model.parts;
	const double ta = model.turn * M_PI / 180.0, tc = std::cos(ta), ts = std::sin(ta);
	auto turned = [&](double &x, double &y) {
		const double tx = x * tc - y * ts, ty = x * ts + y * tc;
		x = tx;
		y = ty;
	};
	/* Its wheels that turn as it goes, the front ones steering as its way turns (Ackermann's: the angle whose tangent
	 * is the wheelbase times the curvature), each about its middle. */
	struct WheelTurn {
		double c[3];
		double sc, ss, tc, ts; // its turn about its axle (y) and its steer about z
	};
	std::vector<WheelTurn> wheel_turn;
	/* Only as the player has them turning (gui.three_d_wheels, off by default): else they stand in the model. */
	if (wheels_turn && !shape->corner_wheel.empty() && !fig->model_wheels.empty()) {
		double front = -1e9, back_sum = 0;
		int backs = 0;
		for (const ThreeDModelWheel &w : fig->model_wheels) {
			if (w.steers) front = std::max(front, static_cast<double>(w.centre[0]));
			else { back_sum += w.centre[0]; backs++; }
		}
		const double wheelbase_m = backs > 0 && front > -1e8 ? front - back_sum / backs : 0;
		for (const ThreeDModelWheel &w : fig->model_wheels) {
			/* Turned round by its state (a cart pushed), it goes back in its own frame: its wheels the other way. */
			const double turn = fg.rolled * tc / (w.radius * across);
			double steer = 0;
			if (w.steers && wheelbase_m > 0) {
				steer = std::clamp(std::atan(fg.curvature * wheelbase_m * across), -w.max_angle * M_PI / 180.0, w.max_angle * M_PI / 180.0);
			}
			wheel_turn.push_back({{w.centre[0], w.centre[1], w.centre[2]}, std::cos(turn), std::sin(turn), std::cos(steer), std::sin(steer)});
		}
	}
	/* A corner of the model where it is drawn: its wheel turned. */
	auto place = [&](size_t i, double &px, double &py, double &pz, double &nx, double &ny, double &nz) {
		px = shape->pos[i * 3]; py = shape->pos[i * 3 + 1]; pz = shape->pos[i * 3 + 2];
		if (lit) { nx = shape->nrm[i * 3]; ny = shape->nrm[i * 3 + 1]; nz = shape->nrm[i * 3 + 2]; }
		const uint8_t wk = i < shape->corner_wheel.size() ? shape->corner_wheel[i] : 0;
		if (wk != 0 && wk <= wheel_turn.size()) {
			const WheelTurn &w = wheel_turn[wk - 1];
			/* Rolling forward turns the wheel's top ahead: about +y, x to -z. */
			double dx = px - w.c[0], dz = pz - w.c[2];
			double rx = dx * w.sc + dz * w.ss, rz = -dx * w.ss + dz * w.sc;
			double ry = py - w.c[1];
			px = w.c[0] + rx * w.tc - ry * w.ts;
			py = w.c[1] + rx * w.ts + ry * w.tc;
			pz = w.c[2] + rz;
			if (lit) {
				double mx = nx * w.sc + nz * w.ss, mz = -nx * w.ss + nz * w.sc;
				nx = mx * w.tc - ny * w.ts;
				ny = mx * w.ts + ny * w.tc;
				nz = mz;
			}
		}
	};
	/* A shape's corners to the eye as the triangles drawn ask for them, each once, with its light (an add-on not
	 * shown, a group of the crew not riding: never): the model's, then those of its crew that walks; each corner in the
	 * model's frame (to_model), turned as the state turns the model. */
	static std::vector<CabPoint> pts;
	static std::vector<CabVertex> scr;
	static std::vector<double> lights;
	static std::vector<uint8_t> ready;
	auto draw = [&](const ThreeDVehicleShape &s, auto &&to_model, const std::vector<const ThreeDObjectTexture *> &tx) {
		const size_t corners = s.pos.size() / 3;
		const bool shape_lit = s.nrm.size() == s.pos.size();
		pts.resize(corners);
		scr.resize(corners);
		lights.resize(corners);
		ready.assign(corners, 0);
		auto corner = [&](uint32_t i) {
			if (ready[i]) return;
			ready[i] = 1;
			double px, py, pz, nx = 0, ny = 0, nz = 1;
			to_model(i, px, py, pz, nx, ny, nz);
			turned(px, py);
			turned(nx, ny);
			/* In metres as it stands, foreshortened, and then raised about its y, its front (+x) up: as steep as the
			 * ground under it, not steeper for its length shortened (#287). */
			double mx = px * length / OBJECT_UNITS_PER_METRE, mz = pz * up / OBJECT_HEIGHT_PER_METRE;
			if (fg.pitch != 0) {
				const double qx = mx * pc - mz * ps, qz = mx * ps + mz * pc;
				mx = qx;
				mz = qz;
				const double rx = nx * pc - nz * ps, rz = nx * ps + nz * pc;
				nx = rx;
				nz = rz;
			}
			const double fx = mx * OBJECT_UNITS_PER_METRE, fy = py * across;
			pts[i] = CabToEye(eye, fg.x + fx * ca - fy * sa, fg.y + fx * sa + fy * ca, fg.z + mz * OBJECT_HEIGHT_PER_METRE, s.uv[i * 2], s.uv[i * 2 + 1]);
			if (pts[i].f >= _cab_near) scr[i] = CabProject(eye, pts[i]);
			if (shape_lit) {
				const double wx = nx * ca - ny * sa, wy = nx * sa + ny * ca;
				lights[i] = 0.55 + 0.45 * std::max(0.0, wx * L[0] + wy * L[1] + nz * L[2]);
			} else {
				lights[i] = 1.0;
			}
		};
		for (size_t t = 0; t + 2 < s.tri.size(); t += 3) {
			const uint8_t part = t / 3 < s.tri_part.size() ? s.tri_part[t / 3] : 0;
			if (fg.own_cab && ThreeDVehicleShape::AtDriverSeat(part)) continue;
			if (part != ThreeDVehicleShape::PART_GLASS && !ThreeDVehicleShape::Shown(part, parts)) continue;
			const int ti = t / 3 < s.tri_tex.size() ? s.tri_tex[t / 3] : 0;
			const ThreeDObjectTexture *tex = tx[static_cast<size_t>(ti) < tx.size() ? ti : 0];
			const uint32_t ia = s.tri[t], ib = s.tri[t + 1], ic = s.tri[t + 2];
			corner(ia);
			corner(ib);
			corner(ic);
			/* A material's own light (tex_glow) over it, unshaded: brighter by its colour's strongest. */
			const uint32_t glow = s.Glow(static_cast<size_t>(ti));
			uint light = static_cast<uint>((lights[ia] + lights[ib] + lights[ic]) / 3 * 256);
			if (glow != 0) {
				const Colour e(glow);
				light += std::max({e.r, e.g, e.b});
			}
			/* The glass and a material "BLEND": see-through, laid over the rest after it (CabFlushGlass()). */
			_cab_glass_alpha = s.IsSeeThrough(t / 3) ? std::max(1u, static_cast<uint>(std::clamp(s.SeeThroughAlpha(t / 3), 0.0f, 1.0f) * 256)) : 256;
			/* The texture's place in pixels of the texture of the triangle (its tu, tv as the unit square). */
			auto in_tex = [&](CabPoint p) {
				p.tu *= tex->w;
				p.tv *= tex->h;
				return p;
			};
			if (pts[ia].f >= _cab_near && pts[ib].f >= _cab_near && pts[ic].f >= _cab_near) {
				auto v = [&](uint32_t i) {
					CabVertex c = scr[i];
					c.uw *= tex->w;
					c.vw *= tex->h;
					return c;
				};
				const CabVertex va = v(ia), vb = v(ib), vc = v(ic);
				if (std::max({va.sx, vb.sx, vc.sx}) < 0 || std::min({va.sx, vb.sx, vc.sx}) >= width || std::max({va.sy, vb.sy, vc.sy}) < 0 || std::min({va.sy, vb.sy, vc.sy}) >= height) continue;
				CabRasterTriangle(dst, zb, width, height, tex->px.data(), tex->w, tex->h, va, vb, vc, CAB_REPEAT, light);
				if (glow != 0 && _cab_glow_after_night != nullptr) {
					/* Again after the dark, a hair nearer (its place in the texture the same), by its own light alone: the
					 * colour the material shines in, not that and the day's light over it (the clay town's giant
					 * mushrooms glowing under their caps were white, the colleague's colours lost). */
					auto nearer = [](CabVertex q) {
						q.iw *= 1.0005;
						q.uw *= 1.0005;
						q.vw *= 1.0005;
						return q;
					};
					const Colour e(glow);
					const uint own = std::max<uint>({e.r, e.g, e.b, 1});
					_cab_glow_after_night->push_back({dst, zb, width, height, tex->px.data(), tex->w, tex->h, nearer(va), nearer(vb), nearer(vc), CAB_REPEAT, own, _cab_glass_alpha});
				}
				continue;
			}
			CabDrawPolygon(dst, zb, width, height, tex->px.data(), tex->w, tex->h, {in_tex(pts[ia]), in_tex(pts[ib]), in_tex(pts[ic])}, eye, CAB_REPEAT, light);
		}
	};
	draw(*shape, place, texs);
	/* Its crew that walks: each figure at its frame of the way gone, turned, sized and stood at its place in the
	 * model's frame, as detailed as it is big on the picture. */
	for (const CabFigureModel::Crew &crew : model.crew) {
		const ThreeDWalkingCrew &w = *crew.w;
		const ThreeDVehicleShape &cs = *crew.shape;
		const double wa = w.turn * M_PI / 180.0, wc = std::cos(wa), ws = std::sin(wa);
		const bool crew_lit = cs.nrm.size() == cs.pos.size();
		auto crew_place = [&](size_t i, double &px, double &py, double &pz, double &nx, double &ny, double &nz) {
			const double fx0 = cs.pos[i * 3], fy0 = cs.pos[i * 3 + 1];
			px = w.at[0] + (fx0 * wc - fy0 * ws) * w.scale;
			py = w.at[1] + (fx0 * ws + fy0 * wc) * w.scale;
			pz = w.at[2] + cs.pos[i * 3 + 2] * w.scale;
			if (crew_lit) {
				const double n0 = cs.nrm[i * 3], n1 = cs.nrm[i * 3 + 1];
				nx = n0 * wc - n1 * ws;
				ny = n0 * ws + n1 * wc;
				nz = cs.nrm[i * 3 + 2];
			}
		};
		draw(cs, crew_place, crew.texs);
	}
	_cab_glass_alpha = 256;
}

/**
 * A figure near the eye by its triangles on the graphics card (the player, 20. 10.: "all the objects by the GPU"): its
 * shape, parts and textures as CabDrawFigureModel() draws them, its corners placed by a matrix -- turned as its state
 * has it, sized and foreshortened, raised up a hill, turned to where it faces --, its crew that walks so too. What the
 * card does not do yet the processor draws as before: the cab the eye is in, the wheels turning, and at night a
 * material with a light of its own (drawn again over the dark, glow_after_night).
 * @param night whether it is dark
 * @param[out] out its meshes
 * @return whether it goes to the card
 */
static bool EyeFigureOnCard(const CabEye &eye, int width, int height, double near, const CabFigure &fg, const CabFigureModel &model, bool night, bool wheels_turn, std::vector<ThreeDGpuMesh> &out)
{
	const ThreeDVehicleShape *shape = model.shape;
	if (fg.own_cab || shape == nullptr || shape->tri.empty() || model.whole == nullptr) return false;
	if (wheels_turn && !shape->corner_wheel.empty() && !model.fig->model_wheels.empty()) return false;
	if (night) {
		for (size_t i = 0; i < model.texs.size(); i++) {
			if (shape->Glow(i) != 0) return false;
		}
		for (const CabFigureModel::Crew &crew : model.crew) {
			for (size_t i = 0; i < crew.texs.size(); i++) {
				if (crew.shape->Glow(i) != 0) return false;
			}
		}
	}
	using M3 = std::array<double, 9>;
	auto mul = [](const M3 &a, const M3 &b) {
		M3 m{};
		for (int r = 0; r < 3; r++) {
			for (int c = 0; c < 3; c++) m[r * 3 + c] = a[r * 3] * b[c] + a[r * 3 + 1] * b[3 + c] + a[r * 3 + 2] * b[6 + c];
		}
		return m;
	};
	auto about_z = [](double degrees) {
		const double a = degrees * M_PI / 180.0, c = std::cos(a), s = std::sin(a);
		return M3{c, -s, 0, s, c, 0, 0, 0, 1};
	};
	const double pa = fg.pitch * M_PI / 180.0, pc = std::cos(pa), ps = std::sin(pa);
	/* As CabDrawFigureModel() places a corner: turned by its state, in metres sized (along it foreshortened, up stretched),
	 * raised about its y, turned to where it faces, in world units; its normals turned alike. */
	const M3 turn = about_z(model.turn), pitch{pc, 0, -ps, 0, 1, 0, ps, 0, pc}, facing = about_z(fg.facing);
	const M3 size{fg.size * fg.along, 0, 0, 0, fg.size, 0, 0, 0, fg.size * fg.stretch};
	const M3 units{OBJECT_UNITS_PER_METRE, 0, 0, 0, OBJECT_UNITS_PER_METRE, 0, 0, 0, OBJECT_HEIGHT_PER_METRE};
	const M3 rotate = mul(facing, mul(pitch, turn));
	const M3 place = mul(units, mul(facing, mul(pitch, mul(size, turn))));
	const std::array<double, 16> clip = EyeClip(eye, width, height, fg.x, fg.y, fg.z, near);
	static constexpr double L[3] = {-0.45, 0.35, 0.82};
	/* A shape whose corners are first placed in the model's frame by local and at (a figure of its crew). */
	auto mesh = [&](const ThreeDVehicleShape &s, const std::vector<const ThreeDObjectTexture *> &texs, const M3 &local, const std::array<double, 3> &at, const M3 &local_turn) {
		const M3 m3 = mul(place, local);
		std::array<double, 16> m4{};
		for (int r = 0; r < 3; r++) {
			for (int c = 0; c < 3; c++) m4[r * 4 + c] = m3[r * 3 + c];
			m4[r * 4 + 3] = place[r * 3] * at[0] + place[r * 3 + 1] * at[1] + place[r * 3 + 2] * at[2];
		}
		m4[15] = 1;
		const M3 n = mul(rotate, local_turn);
		ThreeDGpuMesh &g = out.emplace_back();
		g.shape = &s;
		g.triangles = ThreeDGpuTrianglesOf(s, model.parts, true, texs.size());
		g.matrix = EyeClipMatrix(clip, &m4);
		/* The light in the shape's own frame: n . light there is the world's normal . L. */
		for (int k = 0; k < 3; k++) g.light[k] = static_cast<float>(n[k] * L[0] + n[3 + k] * L[1] + n[6 + k] * L[2]);
		g.lit = s.nrm.size() == s.pos.size();
		g.textures = texs;
	};
	const M3 same{1, 0, 0, 0, 1, 0, 0, 0, 1};
	mesh(*shape, model.texs, same, {0, 0, 0}, same);
	for (const CabFigureModel::Crew &crew : model.crew) {
		const ThreeDWalkingCrew &w = *crew.w;
		const M3 wt = about_z(w.turn);
		const M3 local = mul(wt, M3{w.scale, 0, 0, 0, w.scale, 0, 0, 0, w.scale});
		mesh(*crew.shape, crew.texs, local, {w.at[0], w.at[1], w.at[2]}, wt);
	}
	return true;
}

/**
 * DrawBridgeMiddle() tells where the sprites of a bridge over a tile begin and end, for the cab view and the turned
 * view to build the bridge from them. Nothing else when neither is collecting.
 */
void MarkCabBridgeSprites(TileIndex tile, bool begin)
{
	if (!_cab_collecting && !_drawing_turned) return;
	if (begin) {
		_cab_bridge_sprites.push_back({tile, _vd.parent_sprites_to_draw.size(), SIZE_MAX});
	} else if (!_cab_bridge_sprites.empty() && _cab_bridge_sprites.back().to == SIZE_MAX) {
		_cab_bridge_sprites.back().to = _vd.parent_sprites_to_draw.size();
	}
}

/**
 * The ground of a tunnel's end as it is in the tunnel: level, with the track or the road straight on it, instead
 * of the slope with the portal painted on it.
 */
static void CabTunnelGround(TileIndex tile, int z)
{
	const Axis axis = DiagDirToAxis(GetTunnelBridgeDirection(tile));
	Point pt = RemapCoords(TileX(tile) * TILE_SIZE, TileY(tile) * TILE_SIZE, z);
	auto add = [&](SpriteID image) { _vd.tile_sprites_to_draw.push_back({image, PAL_NONE, nullptr, pt.x, pt.y}); };
	if (GetTunnelBridgeTransportType(tile) == TransportType::Rail) {
		const RailTypeInfo *rti = GetRailTypeInfo(GetRailType(tile));
		if (rti->UsesOverlay()) {
			add(SPR_FLAT_GRASS_TILE);
			SpriteID ground = GetCustomRailSprite(rti, tile, RailSpriteType::Ground);
			if (ground != 0) add(ground + (axis == Axis::X ? RTO_X : RTO_Y));
		} else {
			add(rti->base_sprites.track_y + (axis == Axis::X ? 1 : 0));
		}
	} else {
		add(axis == Axis::X ? SPR_ROAD_X : SPR_ROAD_Y);
	}
}

/**
 * Draw the view from the cab of a vehicle.
 * @param v the vehicle (its first piece)
 * @param width,height the picture
 * @param camera the eye between frames: it turns towards the vehicle's direction over a few frames
 * @param[out] out the picture
 * @return whether there is a picture (a 32bpp blitter)
 */
/** The height of the ground under a world point, units of height (the walker of the view from the first person stands on it). */
double FirstPersonGroundZ(double x, double y)
{
	const int ix = std::clamp(static_cast<int>(x), 0, static_cast<int>(Map::SizeX() * TILE_SIZE) - 1);
	const int iy = std::clamp(static_cast<int>(y), 0, static_cast<int>(Map::SizeY() * TILE_SIZE) - 1);
	return GetSlopePixelZOutsideMap(ix, iy);
}

/**
 * Everything standing in the view from the eyes from a model -- a girl, Mat, a dragon, a vehicle, a car of the game's
 * own -- by one rule (the player, 16. 10.: "the dragon too from near, and save when they are far"; "it is the same
 * principle"): as big on the picture as this many pixels (its longest side) or bigger, the model itself by its
 * triangles, at the angle it really stands at; smaller, a picture of it at about that angle. The rig: 'test3d plnost <px>'.
 */
double _cab_solid_pixels = 28.0;
/** Farther than this a picture of a model is made at half the detail. */
static constexpr double CAB_COARSE = 14.0 * TILE_SIZE;
/** And a plant of a field (AddCabPlant()): only those at the walker's feet, the field has hundreds. */
static constexpr double CAB_PLANT_SOLID = 0.6 * TILE_SIZE;
/** The wheat of the farm's fields is drawn this many tiles about the eye; farther, the field's ground. */
static constexpr double CAB_WHEAT_TILES = 10.0;

/**
 * The night sky in the view from an eye (the player, 15. 10.: "only the night sky", as OpenMW has it): the stars, fixed
 * in the sky so that they go round as the eye turns, a little twinkling, and the moon with its glow -- made by the
 * game, nothing to license. Only where the sky is, nothing drawn there.
 */
static void DrawEyeSky(const CabEye &eye, int width, int height, uint32_t *dst, const float *zb, double t, double seen)
{
	struct Star {
		float az, el;   ///< its direction: round from the world's x, up from the horizon, radians
		uint8_t bright; ///< how bright it is
		uint8_t warm;   ///< how yellow (or, below 128, how blue) it is
		float phase;    ///< its twinkling
	};
	static std::vector<Star> stars;
	if (stars.empty()) {
		uint32_t seed = 0x5EED5u;
		auto next = [&seed]() {
			seed = seed * 1664525u + 1013904223u;
			return (seed >> 8) / static_cast<float>(1 << 24);
		};
		for (int i = 0; i < 1400; i++) {
			Star st;
			st.az = next() * 2.0f * static_cast<float>(M_PI);
			st.el = std::asin(0.03f + 0.97f * next()); // evenly over the dome
			const float b = next();
			st.bright = static_cast<uint8_t>(b < 0.85f ? 70 + b * 110 : 170 + (b - 0.85f) * 560);
			st.warm = static_cast<uint8_t>(next() * 255);
			st.phase = next() * 6.28f;
			stars.push_back(st);
		}
	}
	auto sky = [&](int x, int y) {
		return x >= 0 && y >= 0 && x < width && y < height && y < eye.cy && zb[static_cast<size_t>(y) * width + x] <= 0.0f;
	};
	auto put = [&](int x, int y, Colour c, uint a) {
		if (!sky(x, y)) return;
		uint32_t &d = dst[static_cast<size_t>(y) * width + x];
		Colour o(d);
		d = Colour(static_cast<uint8_t>(std::min(255u, o.r + c.r * a / 255)), static_cast<uint8_t>(std::min(255u, o.g + c.g * a / 255)), static_cast<uint8_t>(std::min(255u, o.b + c.b * a / 255))).data;
	};
	/* Where a direction of the sky is on the picture: ahead of the eye only. */
	auto place = [&](double az, double el, double &sx, double &sy) {
		const double h = std::cos(el), dx = std::cos(az) * h, dy = std::sin(az) * h;
		const double f = dx * eye.fx + dy * eye.fy, r = dx * eye.rx + dy * eye.ry, u = std::sin(el);
		if (f <= 0.05) return false;
		sx = eye.cx + r * eye.focal / f;
		sy = eye.cy - u * eye.focal / f;
		return true;
	};
	for (const Star &st : stars) {
		double sx, sy;
		if (!place(st.az, st.el, sx, sy)) continue;
		const int x = static_cast<int>(sx), y = static_cast<int>(sy);
		const uint a = static_cast<uint>(st.bright * (0.8 + 0.2 * std::sin(t + st.phase)) * seen);
		const Colour c = st.warm > 200 ? Colour(255, 235, 200) : (st.warm < 60 ? Colour(200, 215, 255) : Colour(240, 240, 255));
		put(x, y, c, a);
		if (st.bright > 200) {
			/* A bright one: a little cross. */
			put(x - 1, y, c, a / 3);
			put(x + 1, y, c, a / 3);
			put(x, y - 1, c, a / 3);
			put(x, y + 1, c, a / 3);
		}
	}
	/* The moon, low in the south-east of the map, its glow round it and a few darker seas on it. */
	double mx, my;
	if (place(0.6, 0.42, mx, my)) {
		const double rad = std::max(3.0, 0.035 * eye.focal);
		const int x0 = static_cast<int>(mx - rad * 4), x1 = static_cast<int>(mx + rad * 4);
		const int y0 = static_cast<int>(my - rad * 4), y1 = static_cast<int>(my + rad * 4);
		static const double seas[][3] = {{-0.30, -0.20, 0.32}, {0.25, -0.35, 0.22}, {0.10, 0.30, 0.28}, {-0.35, 0.35, 0.15}};
		for (int y = std::max(0, y0); y <= std::min(height - 1, y1); y++) {
			for (int x = std::max(0, x0); x <= std::min(width - 1, x1); x++) {
				if (!sky(x, y)) continue;
				const double dx = (x + 0.5 - mx) / rad, dy = (y + 0.5 - my) / rad, d2 = dx * dx + dy * dy;
				if (d2 <= 1.0) {
					double k = 0.75 + 0.25 * std::sqrt(1.0 - d2); // darker towards its edge
					for (const auto &sea : seas) {
						const double ex = dx - sea[0], ey = dy - sea[1];
						if (ex * ex + ey * ey < sea[2] * sea[2]) k *= 0.82;
					}
					dst[static_cast<size_t>(y) * width + x] = Colour(static_cast<uint8_t>(240 * k), static_cast<uint8_t>(236 * k), static_cast<uint8_t>(218 * k)).data;
				} else {
					/* The glow, fading away from it. */
					const double g = std::max(0.0, 1.0 - (std::sqrt(d2) - 1.0) / 3.0);
					if (g > 0) put(x, y, Colour(150, 165, 200), static_cast<uint>(g * g * 90));
				}
			}
		}
	}
}

/**
 * The night in the view from an eye (gui.night): the picture darkened as the map is (DrawNight()), and the lights of
 * the vehicles about -- headlights, red lights at the back, an aircraft's beacon -- as glows the size their distance
 * makes them, hidden where something nearer stands in front of them (the depth of the picture). From the cab, the
 * pool of the own headlights on the way ahead.
 */
/** A light glowing in the night from an eye (DrawEyeNight()): where in the world, how big, which colour. */
struct EyeGlow {
	double x, y, z, radius;
	Colour colour;
	double strength, squash;
};

/**
 * The lights that glow in the night seen from an eye, collected under the lock of the game (DrawEyeNight() draws
 * them): the vehicles' about -- headlights, red lights at the back, an aircraft's beacon --, the own cars', and from
 * the cab the pool of the own headlights on the way ahead.
 */
static std::vector<EyeGlow> CollectEyeNightGlows(const CabEye &eye, const Vehicle *own)
{
	std::vector<EyeGlow> glows;
	auto glow = [&](double wx, double wy, double wz, double radius, Colour colour, double strength, double squash) {
		glows.push_back({wx, wy, wz, radius, colour, strength, squash});
	};
	static const int8_t dir_dx[8] = {-1, -1, -1, 0, 1, 1, 1, 0};
	static const int8_t dir_dy[8] = {-1, 0, 1, 1, 1, 0, -1, -1};
	const Colour lamp(255, 236, 190), pool(255, 220, 150), tail(255, 30, 20), beacon(255, 40, 40);
	const double reach = CAB_RANGE_TILES * TILE_SIZE;
	std::vector<ThreeDWorldLight> model_lights;
	auto shine = [&](const std::vector<ThreeDWorldLight> &lights) {
		for (const ThreeDWorldLight &l : lights) {
			switch (l.kind) {
				case ThreeDPictureLight::Kind::Front: glow(l.x, l.y, l.z, 0.35, lamp, 0.9, 1.0); break;
				case ThreeDPictureLight::Kind::Back: glow(l.x, l.y, l.z, 0.25, tail, 0.8, 1.0); break;
				case ThreeDPictureLight::Kind::PoolNear: glow(l.x, l.y, l.z, 3, pool, 0.3, 0.4); break;
				case ThreeDPictureLight::Kind::Pool: glow(l.x, l.y, l.z, 6, pool, 0.3, 0.4); break;
			}
		}
	};
	/* The cars of the game's own driving about (three_d_walkers.h): their lights as the vehicles' of their models. */
	for (const CabFigure &fg : _cab_figures) {
		if (!fg.own_car || fg.lights == 0) continue;
		GetThreeDEyeObjectLights(CabFigureVehicle(fg), model_lights);
		shine(model_lights);
	}
	for (const Vehicle *u : Vehicle::Iterate()) {
		if (u->vehstatus.Test(VehState::Hidden)) continue;
		if (u->type != VehicleType::Road && u->type != VehicleType::Train && u->type != VehicleType::Ship && u->type != VehicleType::Aircraft) continue;
		if (u->type == VehicleType::Aircraft && !Aircraft::From(u)->IsNormalAircraft()) continue;
		/* Switched off from its cab (F, VehicleLightsOn()). */
		if (!VehicleLightsOn(u)) continue;
		const bool front = u->Previous() == nullptr, last = u->Next() == nullptr;
		const double dx = u->x_pos - eye.x, dy = u->y_pos - eye.y;
		if (dx * dx + dy * dy > reach * reach) continue;
		/* A vehicle drawn from its object whose model has its lights: they shine from where they are in it (the
		 * colleague's hra/3d/PROSBA-SVETLA-VOZIDEL-UZLY.md). Not the own one's, seen from inside. */
		const bool own_one = own != nullptr && u->First() == own;
		if (!own_one && ThreeDEnabled() && !u->vehstatus.Test(VehState::Crashed) && u->direction != Direction::Invalid && GetThreeDVehicleObjectWorldLights(u, model_lights)) {
			shine(model_lights);
			continue;
		}
		if (!front && !last) continue;
		const int d = to_underlying(u->direction) & 7;
		double fx = dir_dx[d], fy = dir_dy[d];
		const double len = std::sqrt(fx * fx + fy * fy);
		fx /= len;
		fy /= len;
		const double sx = -fy, sy = fx;
		const double z = u->z_pos;
		const bool mine = own != nullptr && u->First() == own;
		if (u->type == VehicleType::Aircraft) {
			glow(u->x_pos + fx * 6, u->y_pos + fy * 6, z + 2, 1.5, lamp, 0.9, 1.0);
			if ((TimerGameTick::counter + u->index.base() * 7) % 32 < 6) glow(u->x_pos, u->y_pos, z + 6, 1.0, beacon, 1.0, 1.0);
			continue;
		}
		const double half = u->type == VehicleType::Ship ? 6 : 3.5;
		if (front && (u->type != VehicleType::Train || Train::From(u)->IsEngine() || Train::From(u)->IsFrontEngine())) {
			if (mine) {
				/* From the own cab: the pool of the headlights on the way ahead. */
				for (int k = 1; k <= 4; k++) glow(u->x_pos + fx * (half + 6 * k), u->y_pos + fy * (half + 6 * k), z, 3.0 + k, pool, 0.35, 0.35);
			} else {
				glow(u->x_pos + fx * (half + 8), u->y_pos + fy * (half + 8), z, 6, pool, 0.3, 0.4);
				for (int side = -1; side <= 1; side += 2) glow(u->x_pos + fx * half + sx * side * 1.5, u->y_pos + fy * half + sy * side * 1.5, z + 2, 0.8, lamp, 1.2, 1.0);
			}
		}
		if (last && !mine && u->type != VehicleType::Ship) {
			for (int side = -1; side <= 1; side += 2) glow(u->x_pos - fx * half + sx * side * 1.5, u->y_pos - fy * half + sy * side * 1.5, z + 2, 0.5, tail, 0.9, 1.0);
		}
	}
	/* The witches' lanterns and torches (three_d_witches.h): the flame and its light all round, the torch's glass, its
	 * beam as a row of glows widening along it, and its pool where it falls on the ground. */
	const double up = OBJECT_HEIGHT_PER_METRE / OBJECT_UNITS_PER_METRE;
	for (const ThreeDWitchLight &l : GetThreeDWitchLights(true)) {
		if (!l.torch) {
			glow(l.x, l.y, l.z, l.reach, l.colour, 0.35, 1.0);
			glow(l.x, l.y, l.z, std::max(0.15, l.reach * 0.1), l.colour, 1.0, 1.0);
			continue;
		}
		glow(l.x, l.y, l.z, 0.25, l.colour, 1.0, 1.0);
		const double len = l.ground > 0 ? l.ground : l.reach, spread = std::tan(l.half_angle);
		for (int k = 1; k <= 16; k++) {
			const double s = len * k / 16;
			glow(l.x + l.dx * s, l.y + l.dy * s, l.z + l.dz * s * up, std::max(0.2, s * spread), l.colour, 0.1 * (1.0 - k / 18.0), 1.0);
		}
		if (l.ground > 0) glow(l.x + l.dx * len, l.y + l.dy * len, l.z + l.dz * len * up, len * spread * 1.2, l.colour, 0.45, 0.4);
	}
	return glows;
}

/**
 * The night in the view from an eye (gui.night): the picture darkened as the map is (DrawNight()), and the lights
 * collected (CollectEyeNightGlows()) as glows the size their distance makes them, hidden where something nearer
 * stands in front of them (the depth of the picture).
 */
static void DrawEyeNight(const CabEye &eye, int width, int height, uint32_t *dst, const float *zb, NightLight moon, const std::vector<EyeGlow> &glows)
{
	ParallelRows(height, [&](int lo, int hi) {
		for (size_t i = static_cast<size_t>(lo) * width; i < static_cast<size_t>(hi) * width; i++) {
			Colour c(dst[i]);
			dst[i] = Colour(static_cast<uint8_t>(c.r * moon.r >> 8), static_cast<uint8_t>(c.g * moon.g >> 8), static_cast<uint8_t>(c.b * moon.b >> 8)).data;
		}
	});
	for (const EyeGlow &g : glows) {
		CabPoint p = CabToEye(eye, g.x, g.y, g.z, 0, 0);
		if (p.f < CAB_NEAR) continue;
		CabVertex v = CabProject(eye, p);
		const int px = static_cast<int>(v.sx), py = static_cast<int>(v.sy);
		if (px >= 0 && py >= 0 && px < width && py < height) {
			/* Behind something nearer: not seen. */
			if (zb[static_cast<size_t>(py) * width + px] > v.iw * 1.05f) continue;
		}
		const double r = std::max(1.0, g.radius * eye.focal / p.f);
		AddNightLight(dst, width, width, height, v.sx, v.sy, r, r * g.squash, g.colour, g.strength);
	}
}

/**
 * At night the glass of the lights that are on of the vehicles drawn by their triangles (CabDrawFigure(); the
 * colleague's hra/3d/PROSBA-SVETLA-VOZIDEL-UZLY.md): over the dark at full brightness, in its material's colour, a
 * few centimetres towards the eye (it lies on the lamp), hidden behind what is nearer.
 */
static void DrawEyeVehicleGlass(const CabEye &eye, uint32_t *dst, float *zb, int width, int height)
{
	for (const CabFigure &fg : _cab_figures) {
		if (fg.obj == nullptr || fg.lights == 0 || !fg.solid) continue;
		const ThreeDVehicleShape *shape = fg.obj->GetShape();
		if (shape == nullptr || shape->night_tri.empty()) continue;
		const double a = fg.facing * M_PI / 180.0, ca = std::cos(a), sa = std::sin(a);
		const double across = fg.size * OBJECT_UNITS_PER_METRE, length = across * fg.along, up = fg.size * OBJECT_HEIGHT_PER_METRE;
		const size_t corners = shape->night_pos.size() / 3;
		/* The lamps of the parts its state shows only (the colleague's lorries: the cab's on the cab shown). */
		const ThreeDParts parts = fg.obj->PartsOf(fg.state);
		for (size_t t = 0; t + 2 < shape->night_tri.size(); t += 3) {
			const size_t tn = t / 3;
			if ((fg.lights & (tn < shape->night_back.size() && shape->night_back[tn] ? 2 : 1)) == 0) continue;
			if (!ThreeDVehicleShape::Shown(tn < shape->night_part.size() ? shape->night_part[tn] : 0, parts)) continue;
			uint32_t glass = tn < shape->night_colour.size() ? shape->night_colour[tn] : Colour(255, 244, 216).data;
			const uint32_t *glass_tex = EyeColour(glass);
			std::vector<CabPoint> poly;
			for (int k = 0; k < 3; k++) {
				const uint32_t i = shape->night_tri[t + k];
				if (i >= corners) break;
				const double px = shape->night_pos[i * 3], py = shape->night_pos[i * 3 + 1], pz = shape->night_pos[i * 3 + 2];
				double wx = fg.x + px * length * ca - py * across * sa, wy = fg.y + px * length * sa + py * across * ca;
				const double wz = fg.z + pz * up;
				/* A few centimetres towards the eye. */
				const double dx = eye.x - wx, dy = eye.y - wy, d = std::max(std::hypot(dx, dy), 1e-6), lift = 0.05 * OBJECT_UNITS_PER_METRE;
				wx += dx / d * lift;
				wy += dy / d * lift;
				poly.push_back(CabToEye(eye, wx, wy, wz, 0, 0));
			}
			if (poly.size() == 3) CabDrawPolygon(dst, zb, width, height, glass_tex, 1, 1, std::move(poly), eye, false);
		}
	}
}

/** Where a pixel of the view from the eyes is in the world: x, y, and its height as x and y (CAB_Z_SCALE). */
static inline void CabPixelInWorld(const CabEye &eye, double sx, double sy, float iw, double &x, double &y, double &zu)
{
	const double f = 1.0 / iw;
	const double r = (sx - eye.cx) * f / eye.focal, u = (eye.cy - sy) * f / eye.focal;
	x = eye.x + f * eye.fx + r * eye.rx;
	y = eye.y + f * eye.fy + r * eye.ry;
	zu = eye.z * CAB_Z_SCALE + u;
}

/** How far from the eye the buildings cast their shadows, tiles. */
static constexpr int CAB_SHADOW_TILES = 24;
bool _cab_shadows = true; ///< The rig: 'test3d stiny 0' draws the view from the eyes without the sun's shadows.
bool _cab_lamp_light = true; ///< The rig: 'test3d lampy 0' without the light of the houses' lamps on what is about them.

/** A texel of the map of the shadows (DrawEyeShadows()), world units: the casters' shapes as simple as it allows. */
static constexpr double CAB_SHADOW_TEXEL = 0.35 * OBJECT_UNITS_PER_METRE;

/** A building casting its shadow (DrawEyeShadows()): what, where its north corner is, and its shape as simple as a texel of the map allows (made while collecting). */
struct CabShadowCaster {
	const ThreeDObject *obj;
	double ox, oy, oz;
	ThreeDObjectShapeView shape;
};

/**
 * The map of the sun's shadows (DrawEyeShadows()): the buildings seen from the sun, on a plane across its light,
 * each texel how near to the sun the nearest of them is there. Made again only when the buildings about the eye
 * change.
 */
struct CabShadowMap {
	uint64_t key = 0;
	int nu = 0, nv = 0;
	double u0 = 0, v0 = 0, texel = 1;
	std::vector<float> nearest;
};
static CabShadowMap _cab_shadow_map;

/** The sun: whence the houses are lit (their normals, three_d_objects.cpp), x and y and z as x and y; a unit long. */
static const std::array<double, 3> &CabSun()
{
	static const std::array<double, 3> sun = [] {
		const double x = -0.55, y = 0.35, z = 0.75, l = std::sqrt(x * x + y * y + z * z);
		return std::array<double, 3>{x / l, y / l, z / l};
	}();
	return sun;
}

/**
 * The sun's shadows of the buildings (the technologies of OpenMW, the player, 16. 10.): a pixel the sun does not
 * reach for a building between it and the sun is darker -- the ground, the walls of the next house. Each pixel's
 * place in the world from its depth, seen from the sun on the map of the shadows (CabShadowMap); a surface turned
 * away from the sun is dark of its own light already, and is left as it is.
 */
static void DrawEyeShadows(const CabEye &eye, uint32_t *dst, const float *zb, int width, int height, std::span<const CabShadowCaster> casters)
{
	if (casters.empty()) return;
	const std::array<double, 3> &s = CabSun();
	const double al = std::hypot(s[0], s[1]);
	const std::array<double, 3> a = {-s[1] / al, s[0] / al, 0.0};
	const std::array<double, 3> b = {s[1] * a[2] - s[2] * a[1], s[2] * a[0] - s[0] * a[2], s[0] * a[1] - s[1] * a[0]};
	auto dot = [](const std::array<double, 3> &v, double x, double y, double z) { return v[0] * x + v[1] * y + v[2] * z; };

	/* The map again when the buildings are others. */
	uint64_t key = 1469598103934665603ULL;
	for (const CabShadowCaster &c : casters) {
		for (uint64_t v : {reinterpret_cast<uintptr_t>(c.obj), static_cast<uint64_t>(c.ox), static_cast<uint64_t>(c.oy), static_cast<uint64_t>(c.oz)}) key = (key ^ v) * 1099511628211ULL;
	}
	CabShadowMap &map = _cab_shadow_map;
	if (map.key != key) {
		map.key = key;
		/* Every triangle as the sun sees it. */
		struct SunTri {
			float u[3], v[3], d[3];
		};
		std::vector<SunTri> tris;
		double u0 = 1e18, v0 = 1e18, u1 = -1e18, v1 = -1e18;
		for (const CabShadowCaster &c : casters) {
			const ThreeDObjectShapeView &shape = c.shape;
			for (size_t t = 0; t + 2 < shape.tri.size(); t += 3) {
				SunTri st;
				for (int k = 0; k < 3; k++) {
					const uint32_t i = shape.tri[t + k];
					const double x = c.ox + shape.pos[i * 3], y = c.oy + shape.pos[i * 3 + 1], z = (c.oz + shape.pos[i * 3 + 2]) * CAB_Z_SCALE;
					st.u[k] = static_cast<float>(dot(a, x, y, z));
					st.v[k] = static_cast<float>(dot(b, x, y, z));
					st.d[k] = static_cast<float>(dot(s, x, y, z));
					u0 = std::min<double>(u0, st.u[k]);
					u1 = std::max<double>(u1, st.u[k]);
					v0 = std::min<double>(v0, st.v[k]);
					v1 = std::max<double>(v1, st.v[k]);
				}
				tris.push_back(st);
			}
		}
		map.texel = std::max(CAB_SHADOW_TEXEL, std::max(u1 - u0, v1 - v0) / 2048);
		map.u0 = u0;
		map.v0 = v0;
		map.nu = static_cast<int>((u1 - u0) / map.texel) + 2;
		map.nv = static_cast<int>((v1 - v0) / map.texel) + 2;
		map.nearest.assign(static_cast<size_t>(map.nu) * map.nv, -std::numeric_limits<float>::infinity());
		const float iu = static_cast<float>(1.0 / map.texel);
		const float fu0 = static_cast<float>(u0), fv0 = static_cast<float>(v0);
		/* Filled by bands of its rows, a core each. */
		ParallelBands(map.nv, [&](int lo, int hi) {
			for (const SunTri &st : tris) {
				float tu[3], tv[3];
				for (int k = 0; k < 3; k++) {
					tu[k] = (st.u[k] - fu0) * iu;
					tv[k] = (st.v[k] - fv0) * iu;
				}
				const int y0 = std::max(lo, static_cast<int>(std::ceil(std::min({tv[0], tv[1], tv[2]}) - 0.5f)));
				const int y1 = std::min(hi - 1, static_cast<int>(std::floor(std::max({tv[0], tv[1], tv[2]}) - 0.5f)));
				if (y0 > y1) continue;
				const float area = (tu[1] - tu[0]) * (tv[2] - tv[0]) - (tv[1] - tv[0]) * (tu[2] - tu[0]);
				if (std::abs(area) < 1e-6f) continue;
				const int x0 = std::max(0, static_cast<int>(std::ceil(std::min({tu[0], tu[1], tu[2]}) - 0.5f)));
				const int x1 = std::min(map.nu - 1, static_cast<int>(std::floor(std::max({tu[0], tu[1], tu[2]}) - 0.5f)));
				for (int y = y0; y <= y1; y++) {
					const float py = y + 0.5f;
					float *row = &map.nearest[static_cast<size_t>(y) * map.nu];
					for (int x = x0; x <= x1; x++) {
						const float px = x + 0.5f;
						const float w0 = ((tu[1] - px) * (tv[2] - py) - (tv[1] - py) * (tu[2] - px)) / area;
						const float w1 = ((tu[2] - px) * (tv[0] - py) - (tv[2] - py) * (tu[0] - px)) / area;
						const float w2 = 1 - w0 - w1;
						if (w0 < 0 || w1 < 0 || w2 < 0) continue;
						row[x] = std::max(row[x], w0 * st.d[0] + w1 * st.d[1] + w2 * st.d[2]);
					}
				}
			}
		}, tris.size() >= 1024);
	}

	/* Each pixel: in the shadow when something is nearer to the sun there, by more than a texel or two. */
	const float bias = static_cast<float>(2.0 * map.texel);
	ParallelRows(height, [&](int lo, int hi) {
		for (int py = lo; py < std::min(hi, height - 1); py++) {
			for (int px = 0; px < width - 1; px++) {
				const size_t i = static_cast<size_t>(py) * width + px;
				const float iw = zb[i];
				if (iw <= 0.0f) continue;
				double x, y, zu;
				CabPixelInWorld(eye, px + 0.5, py + 0.5, iw, x, y, zu);
				const double u = (dot(a, x, y, zu) - map.u0) / map.texel, v = (dot(b, x, y, zu) - map.v0) / map.texel;
				if (u < 0 || v < 0 || u >= map.nu || v >= map.nv) continue;
				const float nearest = map.nearest[static_cast<size_t>(v) * map.nu + static_cast<size_t>(u)];
				if (nearest <= static_cast<float>(dot(s, x, y, zu)) + bias) continue;
				/* Its surface from its neighbours: turned away from the sun, it is dark of its own light. */
				const float iw_r = zb[i + 1], iw_d = zb[i + width];
				if (iw_r > 0.0f && iw_d > 0.0f && std::abs(iw_r - iw) < iw * 0.05f && std::abs(iw_d - iw) < iw * 0.05f) {
					double xr, yr, zr, xd, yd, zd;
					CabPixelInWorld(eye, px + 1.5, py + 0.5, iw_r, xr, yr, zr);
					CabPixelInWorld(eye, px + 0.5, py + 1.5, iw_d, xd, yd, zd);
					const double ex = xr - x, ey = yr - y, ez = zr - zu, gx = xd - x, gy = yd - y, gz = zd - zu;
					double nx = ey * gz - ez * gy, ny = ez * gx - ex * gz, nz = ex * gy - ey * gx;
					/* Towards the eye. */
					if (nx * (eye.x - x) + ny * (eye.y - y) + nz * (eye.z * CAB_Z_SCALE - zu) < 0) {
						nx = -nx;
						ny = -ny;
						nz = -nz;
					}
					const double nl = std::sqrt(nx * nx + ny * ny + nz * nz);
					if (nl > 0 && (nx * s[0] + ny * s[1] + nz * s[2]) / nl < 0.05) continue;
				}
				Colour c(dst[i]);
				dst[i] = Colour(static_cast<uint8_t>(c.r * 160 >> 8), static_cast<uint8_t>(c.g * 165 >> 8), static_cast<uint8_t>(c.b * 180 >> 8)).data;
			}
		}
	});
}

/**
 * The lights of the houses at night lighting what is about them (the technologies of OpenMW, the player, 16. 10.:
 * light by the pixel): each pixel within a few metres below a lamp or a lit window -- the pavement, the wall under
 * it -- takes its warm light, the more the nearer, its own colour coming back out of the dark. The nearest lights
 * only (CAB_LAMPS_LIT).
 * @param lights the places of the lights, world units (z in units of height)
 */
static void DrawEyeLampLight(const CabEye &eye, uint32_t *dst, const float *zb, int width, int height, std::vector<std::array<double, 3>> lights)
{
	static constexpr double REACH = 6.0 * OBJECT_UNITS_PER_METRE;
	static constexpr size_t CAB_LAMPS_LIT = 48;
	const Colour warm(255, 214, 150);
	const NightLight moon = GetNightLight();
	auto far = [&](const std::array<double, 3> &l) { return (l[0] - eye.x) * (l[0] - eye.x) + (l[1] - eye.y) * (l[1] - eye.y); };
	/* Those whose light may be seen: ahead, within the view. */
	std::erase_if(lights, [&](const std::array<double, 3> &l) {
		const CabPoint c = CabToEye(eye, l[0], l[1], l[2], 0, 0);
		return c.f < -REACH || c.f > CAB_RANGE_TILES * TILE_SIZE || std::abs(c.r) > std::max(c.f, 0.0) * CAB_FOV_TAN + REACH;
	});
	if (lights.size() > CAB_LAMPS_LIT) {
		std::ranges::nth_element(lights, lights.begin() + CAB_LAMPS_LIT, {}, far);
		lights.resize(CAB_LAMPS_LIT);
	}
	for (const auto &l : lights) {
		const CabPoint c = CabToEye(eye, l[0], l[1], l[2], 0, 0);
		/* The pixels it may reach: its sphere on the picture (all of it when the eye is in the sphere or the light behind). */
		int x0 = 0, x1 = width - 1, y0 = 0, y1 = height - 1;
		if (c.f > REACH + CAB_NEAR) {
			const CabVertex v = CabProject(eye, c);
			const double r = REACH * eye.focal / (c.f - REACH);
			x0 = std::max(0, static_cast<int>(v.sx - r));
			x1 = std::min(width - 1, static_cast<int>(v.sx + r));
			y0 = std::max(0, static_cast<int>(v.sy - r));
			y1 = std::min(height - 1, static_cast<int>(v.sy + r));
		}
		if (x0 > x1 || y0 > y1) continue;
		const double lzu = l[2] * CAB_Z_SCALE;
		ParallelRows(y1 - y0 + 1, [&](int lo, int hi) {
			for (int py = y0 + lo; py < y0 + hi; py++) {
				for (int px = x0; px <= x1; px++) {
					const size_t i = static_cast<size_t>(py) * width + px;
					if (zb[i] <= 0.0f) continue;
					double x, y, zu;
					CabPixelInWorld(eye, px + 0.5, py + 0.5, zb[i], x, y, zu);
					/* Down from it: what is above it it does not light (a lamp shines down, a window out and down). */
					if (zu > lzu + 0.5) continue;
					const double d = std::sqrt((x - l[0]) * (x - l[0]) + (y - l[1]) * (y - l[1]) + (zu - lzu) * (zu - lzu));
					if (d >= REACH) continue;
					const double k = (1.0 - d / REACH) * (1.0 - d / REACH) * 0.7;
					/* Its own colour out of the dark (DrawEyeNight()), in the warm light. */
					const Colour o(dst[i]);
					auto add = [&](uint8_t now, uint night, uint8_t light) {
						const double own = std::min(255.0, now * 256.0 / night);
						return static_cast<uint8_t>(std::min(255.0, now + own * light / 255.0 * k));
					};
					dst[i] = Colour(add(o.r, moon.r, warm.r), add(o.g, moon.g, warm.g), add(o.b, moon.b, warm.b)).data;
				}
			}
		}, (y1 - y0) * (x1 - x0) > 20000);
	}
}

/**
 * The witches' lights lighting what is about them (three_d_witches.h; the player: "light all round the lantern, and
 * from the hand torch a beam"): each pixel within a lantern's reach takes its light all round, the more the nearer;
 * each within a torch's cone and reach its light, the more the nearer to the torch and to the middle of the beam --
 * its own colour coming back out of the dark, as under the houses' lamps (DrawEyeLampLight()).
 */
/**
 * The smoke of the walker's rockets, or their blasts, seen from the eyes (ThreeDBlast): each a soft ball of its colour
 * over what is behind it, thinner to its edge, hidden behind what is nearer than its middle.
 * @param fire the blasts (else the smoke)
 */
static void DrawEyeBlasts(const CabEye &eye, uint32_t *dst, const float *zb, int width, int height, const std::vector<ThreeDBlast> &blasts, bool fire)
{
	for (const ThreeDBlast &b : blasts) {
		if (b.light != fire || b.alpha <= 0) continue;
		const CabPoint c = CabToEye(eye, b.x, b.y, b.z, 0, 0);
		if (c.f < CAB_NEAR) continue;
		const CabVertex v = CabProject(eye, c);
		const double r = b.radius_m * OBJECT_UNITS_PER_METRE * eye.focal / c.f;
		if (r < 0.5) continue;
		const int x0 = std::max(0, static_cast<int>(v.sx - r)), x1 = std::min(width - 1, static_cast<int>(v.sx + r));
		const int y0 = std::max(0, static_cast<int>(v.sy - r)), y1 = std::min(height - 1, static_cast<int>(v.sy + r));
		const Colour col(static_cast<uint8_t>(b.colour >> 16), static_cast<uint8_t>(b.colour >> 8), static_cast<uint8_t>(b.colour));
		for (int py = y0; py <= y1; py++) {
			for (int px = x0; px <= x1; px++) {
				const double dx = (px + 0.5 - v.sx) / r, dy = (py + 0.5 - v.sy) / r, d2 = dx * dx + dy * dy;
				if (d2 >= 1.0) continue;
				const size_t i = static_cast<size_t>(py) * width + px;
				if (zb[i] > v.iw * 1.02f) continue; // something nearer
				const double a = b.alpha * (1.0 - d2);
				const Colour o(dst[i]);
				dst[i] = Colour(static_cast<uint8_t>(o.r + (col.r - o.r) * a), static_cast<uint8_t>(o.g + (col.g - o.g) * a), static_cast<uint8_t>(o.b + (col.b - o.b) * a)).data;
			}
		}
	}
}

static void DrawEyeWitchLight(const CabEye &eye, uint32_t *dst, const float *zb, int width, int height, const std::vector<ThreeDWitchLight> &lights)
{
	const NightLight moon = GetNightLight();
	for (const ThreeDWitchLight &l : lights) {
		const double reach = l.reach * (l.torch ? 1.3 : 1.0);
		if (reach <= 0) continue;
		/* The sphere it may reach: about the lantern, about the middle of the beam. */
		const double half = l.torch ? reach / 2 : 0;
		const double cx = l.x + l.dx * half, cy = l.y + l.dy * half, cz = l.z + l.dz * half * OBJECT_HEIGHT_PER_METRE / OBJECT_UNITS_PER_METRE;
		const double radius = l.torch ? half + reach * std::tan(l.half_angle) : reach;
		const CabPoint c = CabToEye(eye, cx, cy, cz, 0, 0);
		if (c.f < -radius || c.f > CAB_RANGE_TILES * TILE_SIZE || std::abs(c.r) > std::max(c.f, 0.0) * CAB_FOV_TAN + radius) continue;
		int x0 = 0, x1 = width - 1, y0 = 0, y1 = height - 1;
		if (c.f > radius + CAB_NEAR) {
			const CabVertex v = CabProject(eye, c);
			const double r = radius * eye.focal / (c.f - radius);
			x0 = std::max(0, static_cast<int>(v.sx - r));
			x1 = std::min(width - 1, static_cast<int>(v.sx + r));
			y0 = std::max(0, static_cast<int>(v.sy - r));
			y1 = std::min(height - 1, static_cast<int>(v.sy + r));
		}
		if (x0 > x1 || y0 > y1) continue;
		const double lzu = l.z * CAB_Z_SCALE, cos_edge = std::cos(l.half_angle);
		/* Metres up as the eye's units count them (CAB_Z_SCALE), against the units along the ground. */
		const double zu_per_unit = CAB_Z_SCALE * OBJECT_HEIGHT_PER_METRE / OBJECT_UNITS_PER_METRE;
		ParallelRows(y1 - y0 + 1, [&](int lo, int hi) {
			for (int py = y0 + lo; py < y0 + hi; py++) {
				for (int px = x0; px <= x1; px++) {
					const size_t i = static_cast<size_t>(py) * width + px;
					if (zb[i] <= 0.0f) continue;
					double x, y, zu;
					CabPixelInWorld(eye, px + 0.5, py + 0.5, zb[i], x, y, zu);
					const double ox = x - l.x, oy = y - l.y, oz = (zu - lzu) / zu_per_unit;
					const double d = std::sqrt(ox * ox + oy * oy + oz * oz);
					double k;
					if (!l.torch) {
						if (d >= reach) continue;
						k = (1.0 - d / reach) * (1.0 - d / reach) * 0.8;
					} else {
						const double along = ox * l.dx + oy * l.dy + oz * l.dz;
						if (along <= 0 || along >= reach || d < 1e-6) continue;
						const double cosa = along / d;
						if (cosa <= cos_edge) continue;
						const double middle = (cosa - cos_edge) / (1.0 - cos_edge);
						k = (1.0 - along / reach) * std::min(1.0, middle * 2.5) * 0.9;
					}
					const Colour o(dst[i]);
					auto add = [&](uint8_t now, uint night, uint8_t light) {
						const double own = std::min(255.0, now * 256.0 / night);
						return static_cast<uint8_t>(std::min(255.0, now + own * light / 255.0 * k));
					};
					dst[i] = Colour(add(o.r, moon.r, l.colour.r), add(o.g, moon.g, l.colour.g), add(o.b, moon.b, l.colour.b)).data;
				}
			}
		}, (y1 - y0) * (x1 - x0) > 20000);
	}
}

/** How far the far land reaches (DrawEyeFarLand()), tiles. */
static constexpr int CAB_FAR_TILES = 96;
/** How far the houses and industries that are 3D objects are drawn on it, tiles. */
static constexpr int CAB_FAR_OBJECT_TILES = 64;

/** The average colour of a sprite at the normal zoom, made the first time (the far land's colours). */
static Colour CabSpriteColour(SpriteID image, PaletteID pal, Colour fallback)
{
	static std::map<std::pair<SpriteID, PaletteID>, Colour> kept;
	auto it = kept.find({image, pal});
	if (it != kept.end()) return it->second;
	const Sprite *spr = GetSprite(image & SPRITE_MASK, SpriteType::Normal);
	const int mask = ScaleByZoom(-1, ZoomLevel::Normal);
	const int left = spr->x_offs & mask, top = spr->y_offs & mask;
	const int w = UnScaleByZoom(spr->x_offs + spr->width - left, ZoomLevel::Normal) + 1, h = UnScaleByZoom(spr->y_offs + spr->height - top, ZoomLevel::Normal) + 1;
	CabTexture t;
	t.w = w;
	t.h = h;
	t.px.assign(static_cast<size_t>(w) * h, 0);
	DrawPixelInfo tdpi{
		.dst_ptr = t.px.data(),
		.left = left,
		.top = top,
		.width = ScaleByZoom(w, ZoomLevel::Normal),
		.height = ScaleByZoom(h, ZoomLevel::Normal),
		.pitch = w,
		.zoom = ZoomLevel::Normal
	};
	{
		AutoRestoreBackup dpi_backup(_cur_dpi, &tdpi);
		AutoRestoreBackup anim_backup(_screen_disable_anim, true);
		DrawSpriteViewport(image, pal, 0, 0, nullptr);
	}
	const Colour c = CabAverage(t, fallback);
	kept.emplace(std::make_pair(image, pal), c);
	return c;
}

/** The colour of a tile from afar (the far land): the sprite of its ground, averaged; its trees, roads and houses as their colours. */
static Colour CabFarColour(TileIndex tile)
{
	auto mix = [](Colour a, Colour b, int b_part) {
		auto m = [&](uint8_t x, uint8_t y) { return static_cast<uint8_t>((x * (256 - b_part) + y * b_part) >> 8); };
		return Colour(m(a.r, b.r), m(a.g, b.g), m(a.b, b.b));
	};
	static const SpriteID grass[4] = {SPR_FLAT_BARE_LAND, SPR_FLAT_1_THIRD_GRASS_TILE, SPR_FLAT_2_THIRD_GRASS_TILE, SPR_FLAT_GRASS_TILE};
	static const SpriteID snow_desert[4] = {SPR_FLAT_1_QUART_SNOW_DESERT_TILE, SPR_FLAT_2_QUART_SNOW_DESERT_TILE, SPR_FLAT_3_QUART_SNOW_DESERT_TILE, SPR_FLAT_SNOW_DESERT_TILE};
	const Colour green = CabSpriteColour(SPR_FLAT_GRASS_TILE, PAL_NONE, Colour(90, 130, 50));
	/* The land of a themed town (GetThemedLand()): its ash or its sand. */
	auto themed = [&](Colour own) {
		switch (GetThemedLand(tile)) {
			case ThemedLand::Ash: return CabSpriteColour(SPR_FLAT_GRASS_TILE, PALETTE_CRASH, Colour(90, 90, 90));
			case ThemedLand::Desert: return CabSpriteColour(SPR_DESERT_GROUND_BASE + 3 * 19, PAL_NONE, Colour(200, 180, 120));
			default: return own;
		}
	};
	switch (GetTileType(tile)) {
		case TileType::Clear: {
			if (IsSnowTile(tile)) return CabSpriteColour(snow_desert[GetClearDensity(tile) & 3], PAL_NONE, Colour(230, 230, 240));
			switch (GetClearGround(tile)) {
				case ClearGround::Grass: return themed(CabSpriteColour(grass[GetClearDensity(tile) & 3], PAL_NONE, green));
				case ClearGround::Rough: return themed(CabSpriteColour(SPR_FLAT_ROUGH_LAND, PAL_NONE, green));
				case ClearGround::Rocks: return CabSpriteColour(SPR_FLAT_ROCKY_LAND_1, PAL_NONE, Colour(130, 125, 115));
				case ClearGround::Fields: return CabSpriteColour(SPR_FARMLAND_STATE_3, PAL_NONE, Colour(150, 130, 70));
				case ClearGround::Desert: return CabSpriteColour(snow_desert[GetClearDensity(tile) & 3], PAL_NONE, Colour(210, 190, 130));
				default: return green;
			}
		}
		case TileType::Trees: {
			Colour ground = green;
			switch (GetTreeGround(tile)) {
				case TreeGround::SnowOrDesert: case TreeGround::RoughSnow: ground = CabSpriteColour(SPR_FLAT_SNOW_DESERT_TILE, PAL_NONE, Colour(230, 230, 240)); break;
				case TreeGround::Shore: ground = CabSpriteColour(SPR_FLAT_WATER_TILE, PAL_NONE, Colour(60, 90, 160)); break;
				default: break;
			}
			ground = themed(ground);
			Colour crown(52, 84, 38);
			if (GetThemedLand(tile) == ThemedLand::Ash) crown = Colour(120, 86, 96); // the mushrooms
			else if (GetTreeType(tile) == TREE_CACTUS || GetThemedLand(tile) == ThemedLand::Desert) crown = Colour(110, 140, 70);
			else if (GetTreeType(tile) >= TREE_SUB_ARCTIC && GetTreeType(tile) < TREE_RAINFOREST) crown = Colour(40, 70, 50);
			return mix(ground, crown, 96 + 32 * static_cast<int>(GetTreeCount(tile)));
		}
		case TileType::Water: return CabSpriteColour(SPR_FLAT_WATER_TILE, PAL_NONE, Colour(60, 90, 160));
		case TileType::Road: return Colour(112, 112, 108);
		case TileType::Railway: return Colour(122, 108, 92);
		case TileType::House: return Colour(150, 132, 118);
		case TileType::Station: return Colour(128, 124, 118);
		case TileType::Industry: return Colour(128, 112, 96);
		case TileType::TunnelBridge: return Colour(120, 112, 100);
		default: return themed(green);
	}
}

/**
 * The far land (as OpenMW's distant land): beyond the tiles drawn, the land to the horizon in blocks of tiles, the
 * larger the farther -- a tile up to 26 tiles away, 2 to 40, 4 to 80, then 8 -- each with the heights of its
 * corners and the colour of its tiles from afar, lit by its slope; the hills on the horizon stand against the sky.
 * Each block hangs a skirt from its edges: where a larger block meets smaller ones the edges differ, and the skirt
 * fills the slit. The tiles drawn (near) are left to them. The houses and industries that are 3D objects within
 * CAB_FAR_OBJECT_TILES are given to far_object, drawn as the near ones, simplified (ThreeDShapeLevel()).
 * @param near whether a tile is drawn as the tiles are
 * @param far_object a 3D object standing on the far land: the object and its north tile
 */
static void DrawEyeFarLand(const CabEye &eye, uint32_t *dst, float *zb, int width, int height, const std::function<bool(int, int)> &near, const std::function<void(const ThreeDObject *, TileIndex)> &far_object)
{
	struct Block {
		int x, y, size;
	};
	std::vector<Block> blocks;
	const double ex = eye.x / TILE_SIZE, ey = eye.y / TILE_SIZE;
	auto visible = [&](int x, int y, int size) {
		const double half = size * TILE_SIZE / 2.0;
		CabPoint c = CabToEye(eye, x * TILE_SIZE + half, y * TILE_SIZE + half, eye.z, 0, 0);
		const double radius = half * std::numbers::sqrt2;
		return c.f > -radius && std::abs(c.r) <= std::max(c.f, 0.0) * CAB_FOV_TAN + radius + TILE_SIZE;
	};
	std::function<void(int, int, int)> split = [&](int x, int y, int size) {
		if (x >= static_cast<int>(Map::SizeX()) || y >= static_cast<int>(Map::SizeY()) || x + size <= 0 || y + size <= 0) return;
		if (!visible(x, y, size)) return;
		const double d = std::hypot(x + size / 2.0 - ex, y + size / 2.0 - ey);
		if (d - size * std::numbers::sqrt2 / 2 > CAB_FAR_TILES) return;
		const double split_within = size == 8 ? 80 : size == 4 ? 40 : 26;
		if (size > 1 && d < split_within) {
			const int half = size / 2;
			for (int k = 0; k < 4; k++) split(x + (k & 1) * half, y + (k >> 1) * half, half);
			return;
		}
		if (size == 1 && near(x, y)) return;
		blocks.push_back({x, y, size});
	};
	const int x0 = (static_cast<int>(ex) - CAB_FAR_TILES) & ~7, y0 = (static_cast<int>(ey) - CAB_FAR_TILES) & ~7;
	for (int y = y0; y <= static_cast<int>(ey) + CAB_FAR_TILES; y += 8) {
		for (int x = x0; x <= static_cast<int>(ex) + CAB_FAR_TILES; x += 8) split(x, y, 8);
	}
	if (blocks.empty()) return;

	/* The colours, a pixel each of a texture a row high (the frame's while one is collected: its triangles point to them). */
	std::vector<uint32_t> own_colours;
	std::vector<uint32_t> &colours = _eye_frame != nullptr ? _eye_frame->colours.emplace_back() : own_colours;
	colours.assign(blocks.size(), 0);
	const uint max_x = Map::MaxX(), max_y = Map::MaxY();
	auto height_at = [&](int x, int y) {
		return static_cast<double>(TileHeight(TileXY(std::clamp<int>(x, 0, max_x), std::clamp<int>(y, 0, max_y))) * TILE_HEIGHT);
	};
	/* The light from the north-west, high, as the game's slopes are lit. */
	const double lx = 0.0, ly = -0.55, lz = 0.835;
	CabBatchScope batch;
	for (size_t k = 0; k < blocks.size(); k++) {
		const Block &b = blocks[k];
		/* The houses and industries on it that are 3D objects: the towns on the horizon. */
		if (std::hypot(b.x + b.size / 2.0 - ex, b.y + b.size / 2.0 - ey) < CAB_FAR_OBJECT_TILES) {
			for (int ty = std::max(0, b.y); ty < std::min<int>(b.y + b.size, max_y + 1); ty++) {
				for (int tx = std::max(0, b.x); tx < std::min<int>(b.x + b.size, max_x + 1); tx++) {
					const TileIndex tile = TileXY(tx, ty);
					if (IsTileType(tile, TileType::House) && IsHouseCompleted(tile)) {
						HouseID north = GetHouseType(tile);
						if (GetHouseNorthPart(north) != 0) continue;
						const ThreeDObject *obj = GetThreeDHouseObjectAt(north, tile);
						if (obj != nullptr) far_object(obj, tile);
					} else if (IsTileType(tile, TileType::Industry) && IsIndustryCompleted(tile)) {
						const Industry *ind = Industry::GetByTile(tile);
						const ThreeDObject *obj = GetThreeDIndustryObjectOf(ind);
						if (obj != nullptr) {
							if (ind->location.tile == tile) far_object(obj, tile);
						} else if (const ThreeDIndustryTileParts *parts = OriginalIndustryTileParts(tile); parts != nullptr && !parts->member) {
							/* An industry of the original game from the shared parts: its buildings from their anchors. */
							for (const ThreeDObject *p : parts->parts) far_object(p, tile);
						}
					}
				}
			}
		}
		uint r = 0, g = 0, bl = 0, n = 0;
		const int step = std::max(1, b.size / 4);
		for (int ty = b.y; ty < b.y + b.size; ty += step) {
			for (int tx = b.x; tx < b.x + b.size; tx += step) {
				if (tx < 0 || ty < 0 || static_cast<uint>(tx) > max_x || static_cast<uint>(ty) > max_y) continue;
				const Colour c = CabFarColour(TileXY(tx, ty));
				r += c.r;
				g += c.g;
				bl += c.b;
				n++;
			}
		}
		if (n == 0) continue;
		colours[k] = Colour(static_cast<uint8_t>(r / n), static_cast<uint8_t>(g / n), static_cast<uint8_t>(bl / n)).data;
		const double h00 = height_at(b.x, b.y), h10 = height_at(b.x + b.size, b.y), h11 = height_at(b.x + b.size, b.y + b.size), h01 = height_at(b.x, b.y + b.size);
		/* Its slope's light: the normal against the light, flat as it is. */
		const double span = b.size * TILE_SIZE;
		const double dzdx = ((h10 + h11) - (h00 + h01)) / 2 / span * CAB_Z_SCALE, dzdy = ((h01 + h11) - (h00 + h10)) / 2 / span * CAB_Z_SCALE;
		const double len = std::sqrt(dzdx * dzdx + dzdy * dzdy + 1);
		const double lit = (-dzdx * lx - dzdy * ly + lz) / len / lz;
		const uint light = static_cast<uint>(std::clamp(256.0 * (0.45 + 0.55 * lit), 110.0, 300.0));
		const double tu = k + 0.5, tv = 0.5;
		const double wx0 = b.x * TILE_SIZE, wy0 = b.y * TILE_SIZE, wx1 = wx0 + span, wy1 = wy0 + span;
		auto at = [&](double x, double y, double z) { return CabToEye(eye, x, y, z, tu, tv); };
		CabDrawPolygon(dst, zb, width, height, colours.data(), static_cast<int>(colours.size()), 1, {at(wx0, wy0, h00), at(wx1, wy0, h10), at(wx1, wy1, h11), at(wx0, wy1, h01)}, eye, false, light);
		if (b.size == 1) continue;
		/* The skirt: down from each edge. */
		const double drop = b.size * TILE_HEIGHT;
		const uint dark = light * 3 / 4;
		CabDrawPolygon(dst, zb, width, height, colours.data(), static_cast<int>(colours.size()), 1, {at(wx0, wy0, h00), at(wx1, wy0, h10), at(wx1, wy0, h10 - drop), at(wx0, wy0, h00 - drop)}, eye, false, dark);
		CabDrawPolygon(dst, zb, width, height, colours.data(), static_cast<int>(colours.size()), 1, {at(wx1, wy0, h10), at(wx1, wy1, h11), at(wx1, wy1, h11 - drop), at(wx1, wy0, h10 - drop)}, eye, false, dark);
		CabDrawPolygon(dst, zb, width, height, colours.data(), static_cast<int>(colours.size()), 1, {at(wx1, wy1, h11), at(wx0, wy1, h01), at(wx0, wy1, h01 - drop), at(wx1, wy1, h11 - drop)}, eye, false, dark);
		CabDrawPolygon(dst, zb, width, height, colours.data(), static_cast<int>(colours.size()), 1, {at(wx0, wy1, h01), at(wx0, wy0, h00), at(wx0, wy0, h00 - drop), at(wx0, wy1, h01 - drop)}, eye, false, dark);
	}
}

/**
 * The view from an eye in perspective: from the cab of a vehicle (cab_view.h), or from the eyes of the walker of the
 * view from the first person (first_person_gui.cpp, the player: "let's make Morrowind, first person view").
 * @param v the vehicle, nullptr for the walker
 * @param walker the walker, when there is no vehicle
 */
static bool CollectEyeFrame(const Vehicle *v, const FirstPersonEye *walker, int width, int height, CabViewCamera &camera, EyeFrame &out)
{
	if ((v == nullptr && walker == nullptr) || width <= 0 || height <= 0) return false;
	if (BlitterFactory::GetCurrentBlitter()->GetScreenDepth() != 32) return false;
	double t0 = TurnedNow();
	/* What is drawn goes into the frame (CabRasterTriangle()), drawn when the game's state is let go of; the pictures
	 * of the vehicles and the figures asked for stay till the frame is taken (PinThreeDPictures()). */
	AutoRestoreBackup frame_backup(_eye_frame, &out);
	PinThreeDPictures(true);
	out.pinned = true;
	/* The rig: how long each stage took (CabViewCamera::stage). */
	double t_last = t0;
	auto eye_stage = [&](int i) {
		const double now = TurnedNow();
		camera.stage[i] = now - t_last;
		t_last = now;
	};
	out.width = width;
	out.height = height;
	out.pixels.assign(static_cast<size_t>(width) * height, 0);
	out.depth.assign(static_cast<size_t>(width) * height, 0.0f);
	uint32_t *dst = out.pixels.data();
	float *zb = out.depth.data();

	/* The eye: at the front of the vehicle, looking where it goes; it turns towards a new direction over a few frames. */
	static const int8_t dir_dx[8] = {-1, -1, -1, 0, 1, 1, 1, 0};
	static const int8_t dir_dy[8] = {-1, 0, 1, 1, 1, 0, -1, -1};
	double target = camera.valid ? camera.heading : std::atan2(-1.0, -1.0);
	/* A vehicle drawn from its object with the eye of its cab ("kabina", the colleague's hra/3d/PROSBA-V3S-3D.md): the
	 * eye in the driver's head, the cab drawn round it (the driver not), facing as the object does. */
	ThreeDEyeVehicle own_model;
	bool in_model_cab = false;
	if (walker == nullptr && ThreeDEnabled()) {
		for (const Vehicle *u = v->First(); u != nullptr && !in_model_cab; u = u->Next()) {
			ThreeDEyeVehicle ev;
			if (GetThreeDEyeVehicle(u, ev) && ev.obj != nullptr && ev.obj->has_cab) {
				own_model = ev;
				in_model_cab = true;
			}
		}
	}
	if (walker != nullptr) {
		/* The walker looks where the player turns it, at once. */
		target = walker->heading;
		camera.valid = false;
	} else if (in_model_cab && !v->cab_free) {
		/* The cab of a model: as the model faces, which turns smoothly already. */
		target = own_model.facing * M_PI / 180.0;
		camera.valid = false;
	} else if (v->cab_free) {
		/* Driven freely (cab_drive.h): its own heading, finer than the eight directions. */
		target = v->cab_yaw * (2 * M_PI / CAB_YAW_TURN);
	} else if (v->direction != Direction::Invalid) {
		target = std::atan2(static_cast<double>(dir_dy[to_underlying(v->direction)]), static_cast<double>(dir_dx[to_underlying(v->direction)]));
	}
	if (!camera.valid) {
		camera.heading = target;
		camera.valid = true;
	} else {
		double d = std::remainder(target - camera.heading, 2 * M_PI);
		camera.heading = std::remainder(camera.heading + d * CAB_TURN_RATE, 2 * M_PI);
	}
	CabEye eye;
	eye.fx = std::cos(camera.heading);
	eye.fy = std::sin(camera.heading);
	eye.rx = eye.fy;
	eye.ry = -eye.fx;
	if (walker != nullptr) {
		/* The walker's eyes over the ground where it stands. */
		eye.x = walker->x;
		eye.y = walker->y;
		eye.z = FirstPersonGroundZ(walker->x, walker->y) + walker->eye_z;
	} else if (in_model_cab) {
		/* In the driver's head of the model, as big as the model is drawn, turned as its state turns it. */
		const double across = own_model.scale * OBJECT_UNITS_PER_METRE, ahead = across * own_model.along;
		const ThreeDVehicleObject &m = *own_model.obj;
		const ThreeDVehicleState *st = m.State(own_model.state);
		const double ta = (st != nullptr ? st->turn : 0.0) * M_PI / 180.0;
		const double cx = m.cab[0] * std::cos(ta) - m.cab[1] * std::sin(ta), cy = m.cab[0] * std::sin(ta) + m.cab[1] * std::cos(ta);
		eye.x = own_model.x + cx * ahead * eye.fx - cy * across * eye.fy;
		eye.y = own_model.y + cx * ahead * eye.fy + cy * across * eye.fx;
		eye.z = own_model.z + m.cab[2] * own_model.scale * OBJECT_HEIGHT_PER_METRE;
		own_model.facing = static_cast<float>(camera.heading * 180.0 / M_PI);
	} else {
		eye.x = v->x_pos + eye.fx * CAB_EYE_AHEAD;
		eye.y = v->y_pos + eye.fy * CAB_EYE_AHEAD;
		{
			/* The eye's height over the vehicle: as the type has it, bowed under a low bridge just ahead, so it passes
			 * under the deck, which is only a level over the road or the rails. */
			double want = v->type == VehicleType::Train ? CAB_EYE_Z_TRAIN : v->type == VehicleType::Road ? CAB_EYE_Z_ROAD : v->type == VehicleType::Ship ? CAB_EYE_Z_SHIP : CAB_EYE_Z_AIRCRAFT;
			bool close = false;
			for (int k = -1; k <= 3; k++) {
				double px = v->x_pos + eye.fx * (CAB_EYE_AHEAD + k * TILE_SIZE / 2.0), py = v->y_pos + eye.fy * (CAB_EYE_AHEAD + k * TILE_SIZE / 2.0);
				if (px < 0 || py < 0 || px >= Map::SizeX() * TILE_SIZE || py >= Map::SizeY() * TILE_SIZE) continue;
				TileIndex tile = TileVirtXY(static_cast<int>(px), static_cast<int>(py));
				if (!IsBridgeAbove(tile)) continue;
				int bz = GetBridgePixelHeight(GetSouthernBridgeEnd(tile));
				if (bz <= v->z_pos + 4) continue; // on the bridge, not under it
				want = std::min(want, std::max(CAB_EYE_Z_LOWEST, bz - CAB_DECK - 1.5 - v->z_pos));
				if (k <= 1) close = true;
			}
			if (camera.eye_dz < 0) {
				camera.eye_dz = want;
			} else if (want < camera.eye_dz) {
				camera.eye_dz = close ? want : camera.eye_dz + (want - camera.eye_dz) * 0.5;
			} else {
				camera.eye_dz += (want - camera.eye_dz) * 0.2;
			}
		}
		eye.z = v->z_pos + camera.eye_dz;
	}
	eye.focal = (width / 2.0) / CAB_FOV_TAN;
	eye.cx = width / 2.0;
	eye.cy = height * 0.5;
	/* Inside a model's cab its windscreen is a step from the eye: the near plane a few centimetres off. The walker
	 * looking down sees the ground at its feet, nearer than the cab's near plane: a ground cut off there showed the
	 * sky under it at the bottom of the picture. */
	_cab_near = in_model_cab ? 0.15 : (walker != nullptr ? 0.25 : CAB_NEAR);
	/* The walker looking up or down: the horizon moves the other way (the picture is sheared, not tilted: upright
	 * lines stay upright, as the players of the old shooters knew it). */
	if (walker != nullptr) eye.cy += std::tan(walker->pitch) * eye.focal;

	/* The sky down to the horizon, the far ground under it. */
	const Colour haze(165, 200, 240);
	for (int y = 0; y < height; y++) {
		Colour c;
		if (y < eye.cy) {
			double t = y / eye.cy;
			c = Colour(static_cast<uint8_t>(60 + (haze.r - 60) * t), static_cast<uint8_t>(110 + (haze.g - 110) * t), static_cast<uint8_t>(210 + (haze.b - 210) * t));
		} else {
			c = Colour(84, 120, 64);
		}
		std::fill_n(dst + static_cast<size_t>(y) * width, width, c.data);
	}

	/* A tunnel the eye is in: its tube is drawn round the eye, and the hill over it is left out. */
	std::optional<CabTunnel> in_tunnel;
	{
		std::vector<TileIndex> near;
		if (v != nullptr) near.push_back(v->tile);
		if (eye.x >= 0 && eye.y >= 0 && eye.x < Map::SizeX() * TILE_SIZE && eye.y < Map::SizeY() * TILE_SIZE) near.push_back(TileVirtXY(static_cast<int>(eye.x), static_cast<int>(eye.y)));
		for (TileIndex tile : near) {
			std::optional<CabTunnel> t = CabTunnelAt(tile);
			if (!t.has_value()) continue;
			double s_e = t->axis == Axis::X ? eye.x : eye.y, c_e = (t->axis == Axis::X ? eye.y : eye.x) - t->c_mid;
			if (s_e > t->s_lo && s_e < t->s_hi && std::abs(c_e) < TILE_SIZE / 2.0 && eye.z < t->z + CAB_PORTAL_TOP) {
				in_tunnel = t;
				break;
			}
		}
	}

	/* Hidden elsewhere than in a tunnel (a depot): the dark, with the light of the door. */
	if (v != nullptr && v->vehstatus.Test(VehState::Hidden) && !in_tunnel.has_value() && !CabTunnelAt(v->tile).has_value()) {
		std::fill(out.pixels.begin(), out.pixels.end(), Colour(8, 8, 10).data);
		int lw = std::max(4, width / 14), lh = std::max(3, height / 14);
		int lx = width / 2 - lw / 2, ly = static_cast<int>(eye.cy) - lh / 2;
		for (int y = std::max(0, ly); y < std::min(height, ly + lh); y++) std::fill_n(dst + static_cast<size_t>(y) * width + std::max(0, lx), std::min(lw, width - lx), haze.data);
		camera.ms = TurnedNow() - t0;
		camera.tiles = 0;
		camera.billboards = 0;
		out.depot = true;
		out.collect_ms = camera.ms;
		return true;
	}

	/* 1. The tiles ahead: within reach and the field of view, with a margin. */
	struct CabTile {
		int x, y;
		double f;
		Slope slope;
		int z;
		size_t first, end;
		bool building;
		bool foundation; ///< its ground lies on a foundation, as the foundation's children
	};
	std::vector<CabTile> tiles;
	int tcx = static_cast<int>(v != nullptr ? v->x_pos : eye.x) / static_cast<int>(TILE_SIZE), tcy = static_cast<int>(v != nullptr ? v->y_pos : eye.y) / static_cast<int>(TILE_SIZE);
	for (int ty = tcy - CAB_RANGE_TILES; ty <= tcy + CAB_RANGE_TILES; ty++) {
		for (int tx = tcx - CAB_RANGE_TILES; tx <= tcx + CAB_RANGE_TILES; tx++) {
			if (!IsInsideBS(tx, 0, Map::SizeX()) || !IsInsideBS(ty, 0, Map::SizeY())) continue;
			CabPoint c = CabToEye(eye, tx * TILE_SIZE + TILE_SIZE / 2.0, ty * TILE_SIZE + TILE_SIZE / 2.0, eye.z, 0, 0);
			if (c.f < -2.0 * TILE_SIZE || c.f > CAB_RANGE_TILES * TILE_SIZE) continue;
			if (std::abs(c.r) > c.f * CAB_FOV_TAN + 3.0 * TILE_SIZE) continue;
			if (in_tunnel.has_value()) {
				/* Inside a tunnel the hill over it would stick into the vault. */
				const CabTunnel &t = *in_tunnel;
				int along = t.axis == Axis::X ? tx : ty, across = t.axis == Axis::X ? ty : tx;
				if (across * static_cast<int>(TILE_SIZE) + static_cast<int>(TILE_SIZE) / 2 == static_cast<int>(t.c_mid) && along * TILE_SIZE >= t.s_lo && (along + 1) * TILE_SIZE <= t.s_hi) continue;
			}
			tiles.push_back({tx, ty, c.f, SLOPE_FLAT, 0, 0, 0, false, false});
		}
	}
	camera.tiles = static_cast<int>(tiles.size());
	camera.billboards = 0;
	if (tiles.empty()) {
		camera.ms = TurnedNow() - t0;
		out.depot = true;
		out.collect_ms = camera.ms;
		return true;
	}

	eye_stage(0);
	/* 2. Their draw procedures, collected against a north-up box that holds all of them, so nothing is clipped. */
	int tx0 = INT32_MAX, tx1 = INT32_MIN, ty0 = INT32_MAX, ty1 = INT32_MIN;
	for (const CabTile &t : tiles) {
		tx0 = std::min(tx0, t.x); tx1 = std::max(tx1, t.x);
		ty0 = std::min(ty0, t.y); ty1 = std::max(ty1, t.y);
	}
	const ZoomLevel zoom = ZoomLevel::Normal;
	const int mask = ScaleByZoom(-1, zoom);
	const int zmax = (_settings_game.construction.map_height_limit + _settings_game.construction.max_bridge_height) * TILE_HEIGHT + MAX_BUILDING_PIXELS;
	DrawPixelInfo cdpi{};
	cdpi.zoom = zoom;
	{
		int cl = INT32_MAX, cr = INT32_MIN, ct = INT32_MAX, cb = INT32_MIN;
		for (int corner = 0; corner < 4; corner++) {
			for (int zz : {0, zmax}) {
				Point p = RemapCoords(((corner & 1) ? tx1 + 1 : tx0) * TILE_SIZE, ((corner & 2) ? ty1 + 1 : ty0) * TILE_SIZE, zz);
				cl = std::min(cl, p.x); cr = std::max(cr, p.x);
				ct = std::min(ct, p.y); cb = std::max(cb, p.y);
			}
		}
		cdpi.left = (cl - MAX_TILE_EXTENT_LEFT) & mask;
		cdpi.top = (ct - MAX_TILE_EXTENT_TOP) & mask;
		cdpi.width = (cr + MAX_TILE_EXTENT_RIGHT - cdpi.left + ~mask) & mask;
		cdpi.height = (cb + MAX_TILE_EXTENT_BOTTOM - cdpi.top + ~mask) & mask;
	}
	_vd.tile_sprites_to_draw.clear();
	_vd.parent_sprites_to_draw.clear();
	_vd.child_screen_sprites_to_draw.clear();
	_vd.last_child = LAST_CHILD_NONE;
	_vd.dpi = cdpi;
	AutoRestoreBackup cur_dpi_backup(_cur_dpi, &_vd.dpi);
	std::vector<char> is_foundation;
	std::vector<uint8_t> ps_kind;
	/* The catenary stays out: its wires are lines of the north-up view and make no sense from here (to come as wires of their own). */
	TransparencyOptions no_catenary = _invisibility_opt;
	no_catenary.Set(TransparencyOption::Catenary);
	TransparencyOptions see_through = _transparency_opt;
	see_through.Set(TransparencyOption::Catenary); // invisibility counts only with the transparency set too (IsInvisibilitySet())
	AutoRestoreBackup invisibility_backup(_invisibility_opt, no_catenary);
	AutoRestoreBackup transparency_backup(_transparency_opt, see_through);
	/* The fences along the rails too: lines of the north-up view, posts or ribbons from here. */
	AutoRestoreBackup fences_backup(_settings_client.gui.show_rail_fences, false);
	AutoRestoreBackup field_fences_backup(_draw_fences, false);
	/* What stands on a tile: 0 a billboard, 1 a building's box, 2 a station's box, 3 a bridge over the tile, 4 a
	 * bridge's ramp, 5 nothing (a tunnel's portal, built anew). */
	_cab_figures.clear();
	_cab_bridge_sprites.clear();
	std::vector<TileIndex> ramps, tunnel_ends;
	/* The houses and the game's own industries that are real 3D objects (three_d_objects.h): drawn by their triangles,
	 * their sprites left out. */
	struct CabObject {
		const ThreeDObject *obj;
		TileIndex origin;
		int z;
		std::span<const ThreeDObjectFigure> more = {}; ///< an industry's girls that are there now (GetThreeDIndustryGirls())
	};
	std::vector<CabObject> objects;
	AutoRestoreBackup collecting_backup(_cab_collecting, true);
	for (CabTile &t : tiles) {
		_cur_ti.x = t.x * TILE_SIZE;
		_cur_ti.y = t.y * TILE_SIZE;
		_cur_ti.tile = TileXY(t.x, t.y);
		TileType tile_type = GetTileType(_cur_ti.tile);
		std::tie(_cur_ti.tileh, _cur_ti.z) = GetTilePixelSlope(_cur_ti.tile);
		_vd.foundation_part = FoundationPart::None;
		_vd.foundation[FoundationPart::Normal] = -1;
		_vd.foundation[FoundationPart::Halftile] = -1;
		_vd.last_foundation_child[FoundationPart::Normal] = LAST_CHILD_NONE;
		_vd.last_foundation_child[FoundationPart::Halftile] = LAST_CHILD_NONE;
		t.first = _vd.tile_sprites_to_draw.size();
		size_t first_parent = _vd.parent_sprites_to_draw.size();
		_tile_type_procs[tile_type]->draw_tile_proc(&_cur_ti);
		t.end = _vd.tile_sprites_to_draw.size();
		t.slope = _cur_ti.tileh;
		t.z = _cur_ti.z;
		t.building = tile_type == TileType::House || tile_type == TileType::Industry;
		t.foundation = _vd.foundation[FoundationPart::Normal] != -1 || _vd.foundation[FoundationPart::Halftile] != -1;
		bool tunnel_end = tile_type == TileType::TunnelBridge && IsTunnel(_cur_ti.tile);
		bool ramp = tile_type == TileType::TunnelBridge && !tunnel_end;
		if (tunnel_end) {
			/* Level, with the track or road into the portal, which is built anew. */
			_vd.tile_sprites_to_draw.resize(t.first);
			CabTunnelGround(_cur_ti.tile, _cur_ti.z);
			t.end = _vd.tile_sprites_to_draw.size();
			t.slope = SLOPE_FLAT;
			tunnel_ends.push_back(_cur_ti.tile);
		}
		if (ramp) ramps.push_back(_cur_ti.tile);
		is_foundation.resize(_vd.parent_sprites_to_draw.size(), 0);
		for (int part = 0; part < 2; part++) {
			int idx = _vd.foundation[static_cast<FoundationPart>(part)];
			if (idx >= static_cast<int>(first_parent) && idx < static_cast<int>(_vd.parent_sprites_to_draw.size())) is_foundation[idx] = 1;
		}
		/* What stands on the tile is a box of its sprite on a house, industry, object or station tile (a tree is a billboard). */
		uint8_t kind = (tile_type == TileType::House || tile_type == TileType::Industry || tile_type == TileType::Object) ? 1 : (tile_type == TileType::Station ? 2 : 0);
		if (ramp) kind = 4;
		if (tunnel_end) kind = 5;
		if (tile_type == TileType::House && IsHouseCompleted(_cur_ti.tile)) {
			HouseID north = GetHouseType(_cur_ti.tile);
			{
				/* An original house or a set's the objects name (the Mars houses). */
				TileIndex origin = _cur_ti.tile + GetHouseNorthPart(north);
				const ThreeDObject *obj = GetThreeDHouseObjectAt(north, origin);
				if (obj != nullptr) {
					kind = 5;
					bool known = false;
					for (const CabObject &co : objects) {
						if (co.origin == origin) known = true;
					}
					if (!known) objects.push_back({obj, origin, GetTileMaxPixelZ(origin), GetThreeDHouseEvent(obj, origin)});
				}
			}
		}
		if (const ThreeDIndustryTileParts *parts = tile_type == TileType::Industry && GetThreeDIndustryObjectOf(Industry::GetByTile(_cur_ti.tile)) == nullptr ? OriginalIndustryTileParts(_cur_ti.tile) : nullptr; parts != nullptr) {
			/* An industry of the original game from the parts the industries share (AddOriginalIndustryTile()): its
			 * ground, and when it is built its building from the anchor of its block, the trees of the forest. */
			kind = 5;
			auto add = [&](const ThreeDObject *obj, TileIndex origin) {
				for (const CabObject &co : objects) {
					if (co.origin == origin && co.obj == obj) return;
				}
				objects.push_back({obj, origin, GetTileMaxPixelZ(origin)});
			};
			if (parts->ground != nullptr) add(parts->ground, _cur_ti.tile);
			const ThreeDIndustryTileParts *of = parts;
			const TileIndex anchor = IsIndustryCompleted(_cur_ti.tile) ? OriginalIndustryAnchor(_cur_ti.tile, of) : INVALID_TILE;
			if (anchor != INVALID_TILE) {
				for (const ThreeDObject *p : of->parts) add(p, anchor);
				if (of->plants != nullptr && anchor == _cur_ti.tile) {
					const double oz = GetTileMaxPixelZ(anchor);
					for (const ThreeDObjectFigure &f : of->plants->plants) {
						AddCabPlant(f.figure, f.scale, f.stretch, _cur_ti.x + f.at[0] * OBJECT_UNITS_PER_METRE, _cur_ti.y + f.at[1] * OBJECT_UNITS_PER_METRE, oz + f.at[2] * OBJECT_HEIGHT_PER_METRE, f.facing);
					}
				}
			}
		}
		if (tile_type == TileType::Clear && IsClearGround(_cur_ti.tile, ClearGround::Fields) && std::hypot(_cur_ti.x + TILE_SIZE / 2.0 - eye.x, _cur_ti.y + TILE_SIZE / 2.0 - eye.y) < CAB_WHEAT_TILES * TILE_SIZE) {
			/* Wheat on the farm's fields (AddFieldWheat()), near the eye: farther the field's ground says enough. */
			const uint type = GetFieldType(_cur_ti.tile);
			const ThreeDObject *wheat = type == 0 || type >= 8 ? nullptr : GetThreeDSlopedPlants(GetThreeDFieldPart(type < 4 ? "psenice_mlada" : "psenice_zrala"), _cur_ti.tileh);
			if (wheat != nullptr) {
				for (const ThreeDObjectFigure &f : wheat->plants) {
					AddCabPlant(f.figure, f.scale, f.stretch, _cur_ti.x + f.at[0] * OBJECT_UNITS_PER_METRE, _cur_ti.y + f.at[1] * OBJECT_UNITS_PER_METRE, _cur_ti.z + f.at[2] * OBJECT_HEIGHT_PER_METRE, f.facing);
				}
			}
		}
		if (tile_type == TileType::Industry && IsIndustryCompleted(_cur_ti.tile)) {
			/* One of the game's own industries that is a real 3D object (the pub, the distillery, the gymnasium...): as a house. */
			const Industry *ind = Industry::GetByTile(_cur_ti.tile);
			const ThreeDObject *obj = GetThreeDIndustryObjectOf(ind);
			ThreeDPlantationTile pt;
			ThreeDFieldTile ft;
			if (obj != nullptr) {
				kind = 5;
				const TileIndex origin = ind->location.tile;
				bool known = false;
				for (const CabObject &co : objects) {
					if (co.origin == origin) known = true;
				}
				if (!known) objects.push_back({obj, origin, GetTileMaxPixelZ(origin), GetThreeDIndustryGirls(ind, obj)});
			} else if (GetThreeDPlantationTile(_cur_ti.tile, pt) && GetThreeDFieldTile(pt, ft)) {
				/* A tile of a field as in the turned view (AddIndustryObjects()): on the game's ground its plants -- a
				 * model of potatoes, or the marijuana plant by plant (AddCabPlant()) -- and the girl at work; the shed,
				 * the haystack and the girls on the road tile behind. */
				kind = 5;
				const double ox = _cur_ti.x, oy = _cur_ti.y, oz = _cur_ti.z;
				if (ft.plants != nullptr) {
					objects.push_back({ft.plants, _cur_ti.tile, _cur_ti.z});
					for (const ThreeDObjectFigure &f : ft.plants->plants) {
						AddCabPlant(f.figure, f.scale, f.stretch, ox + f.at[0] * OBJECT_UNITS_PER_METRE, oy + f.at[1] * OBJECT_UNITS_PER_METRE, oz + f.at[2] * OBJECT_HEIGHT_PER_METRE, f.facing);
					}
				}
				for (const ThreeDObjectFigure &f : ft.worker) AddCabFigure(f.figure, CAB_FIGURE_SCALE, static_cast<int>(ox + f.at[0] * OBJECT_UNITS_PER_METRE), static_cast<int>(oy + f.at[1] * OBJECT_UNITS_PER_METRE), static_cast<int>(oz + f.at[2] * OBJECT_HEIGHT_PER_METRE), f.facing, -1);
				if (ft.road != nullptr) {
					const TileIndex road_tile = TileAddXY(_cur_ti.tile, 0, 1);
					const int road_z = GetTilePixelZ(road_tile);
					if (ft.shed != nullptr) objects.push_back({ft.shed, road_tile, road_z});
					if (ft.hay != nullptr) objects.push_back({ft.hay, road_tile, road_z});
					for (int edge = 0; edge < 2; edge++) {
						for (const ThreeDObjectFigure &f : ft.girls[edge]) AddCabFigure(f.figure, CAB_FIGURE_SCALE, static_cast<int>(TileX(road_tile) * TILE_SIZE + f.at[0] * OBJECT_UNITS_PER_METRE), static_cast<int>(TileY(road_tile) * TILE_SIZE + f.at[1] * OBJECT_UNITS_PER_METRE), static_cast<int>(road_z + f.at[2] * OBJECT_HEIGHT_PER_METRE), f.facing, -1);
					}
				}
			}
		}
		ps_kind.resize(_vd.parent_sprites_to_draw.size(), kind);
		/* The street lights of the towns' streets as lamps, as the buildings (AddStreetLamps()). */
		if (tile_type == TileType::Road && !IsTransparencySet(TransparencyOption::Houses)) {
			for (size_t i = first_parent; i < _vd.parent_sprites_to_draw.size(); i++) {
				const ParentSpriteToDraw &ps = _vd.parent_sprites_to_draw[i];
				if (!IsStreetLightSprite(ps.image)) continue;
				const ThreeDObject *lamp = GetThreeDStreetLamp(ps.xmin & 0xF, ps.ymin & 0xF);
				if (lamp == nullptr) continue;
				ps_kind[i] = 5;
				objects.push_back({lamp, _cur_ti.tile, ps.zmin});
			}
		}
		/* A bridge over the tile is built from its own sprites. */
		for (auto it = _cab_bridge_sprites.rbegin(); it != _cab_bridge_sprites.rend() && it->from >= first_parent; ++it) {
			if (it->to == SIZE_MAX) it->to = _vd.parent_sprites_to_draw.size();
			for (size_t i = it->from; i < it->to && i < ps_kind.size(); i++) ps_kind[i] = 3;
		}
	}
	is_foundation.resize(_vd.parent_sprites_to_draw.size(), 0);
	ps_kind.resize(_vd.parent_sprites_to_draw.size(), 0);
	/* The figures standing on the buildings -- the girls at the doors and on the balconies, at the pub's tables, at the
	 * school after the studentky came: at their true size, as the eyes see them (the colleague's rule 12: twice it
	 * outside a cab, as they are in it; the player: "only the first person view normal"). A thing among them (the
	 * pub's spit over the grill) stays as big as the building it is on. */
	for (const CabObject &co : objects) {
		const double ox = TileX(co.origin) * TILE_SIZE, oy = TileY(co.origin) * TILE_SIZE, oz = co.z;
		auto add = [&](const ThreeDObjectFigure &f) {
			/* A vehicle standing among them (the fire Tatra on the pitch, GetThreeDStandingVehicle()): as the vehicles. */
			if (ThreeDEyeVehicle ev; GetThreeDStandingVehicle(f.figure, ev)) {
				ev.scale = f.scale;
				ev.x = ox + f.at[0] * OBJECT_UNITS_PER_METRE;
				ev.y = oy + f.at[1] * OBJECT_UNITS_PER_METRE;
				ev.z = oz + f.at[2] * OBJECT_HEIGHT_PER_METRE;
				ev.facing = f.facing;
				AddCabVehicle(ev, false);
				return;
			}
			const ThreeDVehicleObject *fig = GetThreeDFigure(f.figure);
			AddCabFigure(f.figure, fig != nullptr && fig->thing ? f.scale : CAB_FIGURE_SCALE, static_cast<int>(std::lround(ox + f.at[0] * OBJECT_UNITS_PER_METRE)), static_cast<int>(std::lround(oy + f.at[1] * OBJECT_UNITS_PER_METRE)),
					static_cast<int>(std::lround(oz + f.at[2] * OBJECT_HEIGHT_PER_METRE)), f.facing, -1);
		};
		for (const ThreeDObjectFigure &f : co.obj->figures) add(f);
		for (const ThreeDObjectFigure &f : co.more) add(f);
	}
	/* The dragons about (three_d_dragons.h), among the figures; Mat walking in the town and the cars of the game's own
	 * driving about (three_d_walkers.h; the player, 16. 10.: "Mat was not seen at all, I came out beside him"). */
	AddThreeDDragonsToEye();
	AddThreeDAnimalsToEye();
	AddThreeDMushroomsToEye();
	AddThreeDWitchesToEye();
	AddFirstPersonMissilesToEye();
	AddThreeDWalkersToEye(CAB_FIGURE_SCALE);
	AddThreeDDriversToEye();

	eye_stage(1);
	/* 3. The ground of all of them, north-up, in one texture at the normal zoom. */
	DrawPixelInfo bigdpi{};
	bigdpi.zoom = zoom;
	int bw = 0, bh = 0;
	{
		int cl = INT32_MAX, cr = INT32_MIN, ct = INT32_MAX, cb = INT32_MIN;
		for (const CabTile &t : tiles) {
			for (int corner = 0; corner < 4; corner++) {
				int x = t.x * TILE_SIZE + ((corner & 1) ? TILE_SIZE : 0), y = t.y * TILE_SIZE + ((corner & 2) ? TILE_SIZE : 0);
				Point lo = RemapCoords(x, y, t.z), hi = RemapCoords(x, y, t.z + 3 * TILE_HEIGHT);
				cl = std::min(cl, lo.x); cr = std::max(cr, lo.x);
				ct = std::min(ct, hi.y); cb = std::max(cb, lo.y);
			}
		}
		const int margin = TILE_HEIGHT * ZOOM_BASE;
		bigdpi.left = (cl - margin) & mask;
		bigdpi.top = (ct - margin) & mask;
		bigdpi.width = (cr + margin - bigdpi.left + ~mask) & mask;
		bigdpi.height = (cb + margin - bigdpi.top + ~mask) & mask;
		bw = UnScaleByZoom(bigdpi.width, zoom);
		bh = UnScaleByZoom(bigdpi.height, zoom);
	}
	const double scale = 1.0 / (1 << to_underlying(zoom));
	if (_debug_misc_level >= 6) {
		int mnx = INT32_MAX, mxx = INT32_MIN, mny = INT32_MAX, mxy = INT32_MIN;
		for (const CabTile &t : tiles) {
			mnx = std::min(mnx, t.x); mxx = std::max(mxx, t.x); mny = std::min(mny, t.y); mxy = std::max(mxy, t.y);
		}
		Debug(misc, 6, "cab ground texture {}x{} from {},{} tiles {} x {}..{} y {}..{} eye {:.0f},{:.0f} heading {:.2f}", bw, bh, bigdpi.left, bigdpi.top, tiles.size(), mnx, mxx, mny, mxy, eye.x, eye.y, camera.heading);
	}
	if (bw > 0 && bh > 0 && static_cast<size_t>(bw) * bh <= 24u * 1024 * 1024) {
		out.ground.assign(static_cast<size_t>(bw) * bh, 0);
		bigdpi.dst_ptr = out.ground.data();
		bigdpi.pitch = bw;
		{
			AutoRestoreBackup dpi_backup(_cur_dpi, &bigdpi);
			AutoRestoreBackup anim_backup(_screen_disable_anim, true);
			for (const CabTile &t : tiles) {
				for (size_t i = t.first; i < t.end; i++) {
					const TileSpriteToDraw &ts = _vd.tile_sprites_to_draw[i];
					DrawSpriteViewport(ts.image, ts.pal, ts.x, ts.y, ts.sub);
				}
			}
			/* On a foundation (a road or rail levelled on a slope) the ground is drawn as children of the foundation's
			 * sprite, on its top: the ground of the tile, which lies there (the tile's slope and height are the top's). */
			for (size_t i = 0; i < _vd.parent_sprites_to_draw.size(); i++) {
				if (!is_foundation[i]) continue;
				const ParentSpriteToDraw &ps = _vd.parent_sprites_to_draw[i];
				for (int child_idx = ps.first_child; child_idx >= 0;) {
					const ChildScreenSpriteToDraw &cs = _vd.child_screen_sprites_to_draw[child_idx];
					child_idx = cs.next;
					if (cs.relative) {
						DrawSpriteViewport(cs.image, cs.pal, ps.left + cs.x, ps.top + cs.y, cs.sub);
					} else {
						DrawSpriteViewport(cs.image, cs.pal, ps.x + cs.x, ps.y + cs.y, cs.sub);
					}
				}
			}
		}
		/* The black under a building, grown over from the ground beside it. */
		for (const CabTile &t : tiles) {
			if (!t.building) continue;
			Point n = RemapCoords(t.x * TILE_SIZE, t.y * TILE_SIZE, t.z + GetSlopeMaxPixelZ(RemoveHalftileSlope(t.slope)));
			Point sth = RemapCoords(t.x * TILE_SIZE + TILE_SIZE, t.y * TILE_SIZE + TILE_SIZE, t.z);
			int x0 = Clamp(static_cast<int>((n.x - TILE_SIZE * 2 * ZOOM_BASE - bigdpi.left) * scale), 0, bw);
			int x1 = Clamp(static_cast<int>((n.x + TILE_SIZE * 2 * ZOOM_BASE - bigdpi.left) * scale) + 1, 0, bw);
			int y0 = Clamp(static_cast<int>((n.y - bigdpi.top) * scale), 0, bh);
			int y1 = Clamp(static_cast<int>((sth.y - bigdpi.top) * scale) + 1, 0, bh);
			FillTurnedFootprint(out.ground.data(), bw, x0, y0, x1, y1);
		}

		if (_debug_misc_level >= 7) {
			/* The rig: the ground texture as a PPM in the working directory. */
			auto f = FileHandle::Open(std::string("cab_ground.ppm"), "wb");
			if (f.has_value()) {
				std::string head = fmt::format("P6\n{} {}\n255\n", bw, bh);
				fwrite(head.data(), 1, head.size(), *f);
				for (uint32_t p : out.ground) {
					Colour c(p);
					uint8_t rgb[3] = {c.r, c.g, c.b};
					fwrite(rgb, 1, 3, *f);
				}
			}
		}
		eye_stage(2);
		/* 4. The ground in perspective: each tile four triangles meeting in its middle, so slopes keep their folds. */
		static const Corner corners[4] = {CORNER_N, CORNER_E, CORNER_S, CORNER_W};
		static const int cxs[4] = {0, 0, TILE_SIZE, TILE_SIZE};
		static const int cys[4] = {0, TILE_SIZE, TILE_SIZE, 0};
		CabBatchScope ground_batch; // the ground texture stays as it is: on every core
		for (const CabTile &t : tiles) {
			if (t.first >= t.end && !t.foundation) continue;
			Slope s = RemoveHalftileSlope(t.slope);
			CabPoint p[5];
			for (int i = 0; i < 5; i++) {
				double wx = (i < 4) ? t.x * TILE_SIZE + cxs[i] : t.x * TILE_SIZE + TILE_SIZE / 2.0;
				double wy = (i < 4) ? t.y * TILE_SIZE + cys[i] : t.y * TILE_SIZE + TILE_SIZE / 2.0;
				double wz = t.z + ((i < 4) ? GetSlopePixelZInCorner(s, corners[i]) : static_cast<int>(GetPartialPixelZ(TILE_SIZE / 2, TILE_SIZE / 2, s)));
				double cx, cy;
				RemapCoordsF(wx, wy, wz, cx, cy);
				p[i] = CabToEye(eye, wx, wy, wz, (cx - bigdpi.left) * scale, (cy - bigdpi.top) * scale);
			}
			for (int i = 0; i < 4; i++) {
				CabDrawPolygon(dst, zb, width, height, out.ground.data(), bw, bh, {p[i], p[(i + 1) % 4], p[4]}, eye, true);
			}
		}
	}
	/* 4a. The far land beyond them, to the horizon (DrawEyeFarLand()); not in a tunnel. */
	if (!in_tunnel.has_value()) {
		DrawEyeFarLand(eye, dst, zb, width, height, [&](int tx, int ty) {
			/* As the tiles ahead are chosen (1. above). */
			if (std::abs(tx - tcx) > CAB_RANGE_TILES || std::abs(ty - tcy) > CAB_RANGE_TILES) return false;
			CabPoint c = CabToEye(eye, tx * TILE_SIZE + TILE_SIZE / 2.0, ty * TILE_SIZE + TILE_SIZE / 2.0, eye.z, 0, 0);
			return c.f >= -2.0 * TILE_SIZE && c.f <= CAB_RANGE_TILES * TILE_SIZE && std::abs(c.r) <= c.f * CAB_FOV_TAN + 3.0 * TILE_SIZE;
		}, [&](const ThreeDObject *obj, TileIndex origin) {
			for (const CabObject &co : objects) {
				if (co.origin == origin) return;
			}
			objects.push_back({obj, origin, GetTileMaxPixelZ(origin), GetThreeDHouseEvent(obj, origin)});
		});
	}

	eye_stage(3);
	/* 4b. Bridges and tunnels as things of their own: decks, ramps and pillars from the bridges' sprites; portals of
	 * stones, and the tube of the tunnel the eye is in. */
	const CabCanvas cv{dst, zb, width, height};
	/* The sides of foundations: walls round the box of the foundation's sprite, dressed in it where it shows them
	 * (the two it never shows mirrored), open where the sprite has nothing -- where the slope stands higher. */
	for (size_t i = 0; i < _vd.parent_sprites_to_draw.size(); i++) {
		if (!is_foundation[i]) continue;
		const ParentSpriteToDraw &ps = _vd.parent_sprites_to_draw[i];
		CabPoint c = CabToEye(eye, (ps.xmin + ps.xmax + 1) * 0.5, (ps.ymin + ps.ymax + 1) * 0.5, ps.zmin, 0, 0);
		if (c.f < -static_cast<double>(TILE_SIZE)) continue;
		static CabTexture found_scratch;
		CabTexture &found = EyeScratchTexture(found_scratch);
		CabCompositeTexture(found, {&ps});
		const double x0 = ps.xmin, x1 = ps.xmax + 1, y0 = ps.ymin, y1 = ps.ymax + 1, z0 = ps.zmin, z1 = ps.zmax + 1;
		auto same = [&](double x, double y, double z) { return std::array<double, 6>{x, y, z, x, y, z}; };
		auto at = [&](double x, double y, double z, double tx, double ty, double tz) { return std::array<double, 6>{x, y, z, tx, ty, tz}; };
		CabDrawDressed(cv, eye, found, {same(x1, y0, z0), same(x1, y1, z0), same(x1, y1, z1), same(x1, y0, z1)}, false);
		CabDrawDressed(cv, eye, found, {same(x0, y1, z0), same(x1, y1, z0), same(x1, y1, z1), same(x0, y1, z1)}, false);
		CabDrawDressed(cv, eye, found, {at(x0, y0, z0, x1, y0, z0), at(x0, y1, z0, x1, y1, z0), at(x0, y1, z1, x1, y1, z1), at(x0, y0, z1, x1, y0, z1)}, false);
		CabDrawDressed(cv, eye, found, {at(x0, y0, z0, x0, y1, z0), at(x1, y0, z0, x1, y1, z0), at(x1, y0, z1, x1, y1, z1), at(x0, y0, z1, x0, y1, z1)}, false);
	}
	/* The houses that are real 3D objects: their triangles with their texture, at the level of detail of their
	 * distance -- about a texture pixel to a pixel of the picture. Their textures stay: on every core. */
	{
	/* Which of them, at which level of detail; their textures made on every core first. */
	struct ObjectJob {
		const ThreeDObject *o;
		TileIndex origin;
		double ox, oy, oz;
		int level;
		uint32_t paint;
		const ThreeDObjectTexture *tex;
		int shape_level;
		ThreeDObjectShapeView shape;
		std::vector<const ThreeDObjectTexture *> texs; ///< its textures by the triangles' (ThreeDObject::GetTextureOf()): [0] tex; nullptr not made yet
	};
	std::vector<ObjectJob> jobs;
	for (const CabObject &co : objects) {
		const ThreeDObject &o = *co.obj;
		const double ox = TileX(co.origin) * TILE_SIZE, oy = TileY(co.origin) * TILE_SIZE, oz = co.z;
		CabPoint c = CabToEye(eye, ox + o.tiles_x * TILE_SIZE / 2.0, oy + o.tiles_y * TILE_SIZE / 2.0, oz, 0, 0);
		if (c.f < -1.0 * TILE_SIZE * std::max(o.tiles_x, o.tiles_y)) continue;
		/* Its texture if it is read; else read in the background, the house there the next frame (the player, 17. 10., 4). */
		const ThreeDObjectTexture *full = o.TryGetTexture(0);
		if (full == nullptr) continue;
		double texels_per_unit = full->w / static_cast<double>(TILE_SIZE);
		double pixels_per_unit = eye.focal / std::max(c.f, CAB_NEAR * 4);
		int level = std::max(0, static_cast<int>(std::floor(std::log2(std::max(1.0, texels_per_unit / std::max(pixels_per_unit, 1e-6))))));
		/* Its shape as simple as its nearest part allows (ThreeDShapeLevel()). */
		const double reach = TILE_SIZE * std::hypot(o.tiles_x, o.tiles_y) / 2 + o.top * CAB_Z_SCALE;
		const double near_pixels_per_unit = eye.focal / std::max(c.f - reach, CAB_NEAR * 4);
		const int shape_level = ThreeDShapeLevel(near_pixels_per_unit * OBJECT_UNITS_PER_METRE, 1.5);
		jobs.push_back({&o, co.origin, ox, oy, oz, level, o.PaintAt(co.origin), nullptr, shape_level, {}, {}});
	}
	/* Its texture painted and its shape simplified if they are made; else unpainted and whole meanwhile, made in the background. */
	for (ObjectJob &j : jobs) {
		j.tex = j.o->TryGetTexture(j.level, j.paint);
		j.shape = j.o->TryGetShape(j.shape_level);
		/* Its add-ons' textures too; one not made yet, its triangles not this frame. */
		j.texs.assign(1, j.tex);
		for (uint8_t k = 1; k < j.o->Textures(); k++) j.texs.push_back(j.o->TryGetTextureOf(k, j.level, j.o->AddonPaintAt(j.origin, k)));
	}
	/* As a job of the frame, off the lock: each object on a core of its own: every corner to the eye once, the
	 * triangles turned away left out (as the engines do), those wholly before the eye straight to its list, the others
	 * cut at the near plane; then the lists drawn by bands of rows (CabRasterLists()). Their shapes and textures stay
	 * as they are till the frame is taken (EyeFrameDrawing()). */
	camera.billboards += static_cast<int>(jobs.size());
	if (!jobs.empty()) out.Job([jobs = std::move(jobs), eye, width, height, near = _cab_near](uint32_t *dst, float *zb) {
		/* On the graphics card all at once (the player, 20. 10.: "all the objects by the GPU"), laid in where nearer
		 * than what is drawn; what it cannot draw the processor draws as below. */
		if (ThreeDGpuAvailable()) {
			ThreeDGpuEyePicture pic;
			pic.width = width;
			pic.height = height;
			pic.near_plane = static_cast<float>(near);
			for (const ObjectJob &job : jobs) {
				if (job.shape.tri.empty() || job.shape.uid == 0) continue;
				ThreeDGpuEyeBuilding &b = pic.buildings.emplace_back();
				b.building.uid = job.shape.uid;
				b.building.pos = job.shape.pos;
				b.building.uv = job.shape.uv;
				b.building.light = job.shape.light;
				b.building.tri = job.shape.tri;
				b.building.facing = job.shape.facing;
				b.building.tri_tex = job.shape.tri_tex;
				b.building.textures = job.texs;
				b.matrix = EyeClipMatrix(EyeClip(eye, width, height, job.ox, job.oy, job.oz, near));
			}
			if (ThreeDGpuRenderEye(pic)) {
				LayEyeCardPicture(pic, dst, zb);
				return;
			}
		}
		std::vector<std::vector<CabBatchTriangle>> lists(jobs.size());
		std::atomic<size_t> next_job = 0;
		auto set_up = [&](int, int) {
			std::vector<CabPoint> pts;
			std::vector<CabVertex> scr;
			for (size_t j; (j = next_job++) < jobs.size();) {
				const ObjectJob &job = jobs[j];
				const ThreeDObjectShapeView &o = job.shape;
				const ThreeDObjectTexture *tex = job.tex;
				const bool lit = o.light.size() == o.pos.size() / 3;
				const size_t corners = o.pos.size() / 3;
				pts.resize(corners);
				scr.resize(corners);
				for (size_t i = 0; i < corners; i++) {
					/* Its place in its own texture: the building's or an add-on's. */
					const uint8_t k = o.CornerTexture(i);
					const ThreeDObjectTexture *ct = k < job.texs.size() && job.texs[k] != nullptr ? job.texs[k] : tex;
					pts[i] = CabToEye(eye, job.ox + o.pos[i * 3], job.oy + o.pos[i * 3 + 1], job.oz + o.pos[i * 3 + 2], o.uv[i * 2] * ct->w, o.uv[i * 2 + 1] * ct->h);
					if (pts[i].f >= CAB_NEAR) scr[i] = CabProject(eye, pts[i]);
				}
				std::vector<CabBatchTriangle> &out_list = lists[j];
				for (size_t t = 0; t + 2 < o.tri.size(); t += 3) {
					const uint8_t k = o.TriangleTexture(t);
					if (k >= job.texs.size() || job.texs[k] == nullptr) continue;
					const ThreeDObjectTexture *tt = job.texs[k];
					const uint32_t ia = o.tri[t], ib = o.tri[t + 1], ic = o.tri[t + 2];
					const uint light = lit ? (o.light[ia] + o.light[ib] + o.light[ic]) / 3 : 256;
					if (pts[ia].f >= CAB_NEAR && pts[ib].f >= CAB_NEAR && pts[ic].f >= CAB_NEAR) {
						const CabVertex &a = scr[ia], &b = scr[ib], &c = scr[ic];
						if (std::max({a.sx, b.sx, c.sx}) < 0 || std::min({a.sx, b.sx, c.sx}) >= width || std::max({a.sy, b.sy, c.sy}) < 0 || std::min({a.sy, b.sy, c.sy}) >= height) continue;
						/* Turned away: a back wall, hidden behind the front anyway (y down: seen from outside it goes round clockwise). */
						const int8_t facing = t / 3 < o.facing.size() ? o.facing[t / 3] : 0;
						if (facing != 0 && ((b.sx - a.sx) * (c.sy - a.sy) - (b.sy - a.sy) * (c.sx - a.sx)) * facing >= 0) continue;
						out_list.push_back({dst, zb, width, height, tt->px.data(), tt->w, tt->h, a, b, c, false, light});
						continue;
					}
					std::vector<CabPoint> poly = {pts[ia], pts[ib], pts[ic]};
					CabClipNear(poly, near);
					if (poly.size() < 3) continue;
					const CabVertex v0 = CabProject(eye, poly[0]);
					CabVertex prev = CabProject(eye, poly[1]);
					for (size_t k = 2; k < poly.size(); k++) {
						const CabVertex cur = CabProject(eye, poly[k]);
						out_list.push_back({dst, zb, width, height, tt->px.data(), tt->w, tt->h, v0, prev, cur, false, light});
						prev = cur;
					}
				}
			}
		};
		ParallelRows(static_cast<int>(jobs.size()) * 16, set_up, jobs.size() >= 2);
		std::vector<const std::vector<CabBatchTriangle> *> drawn;
		for (const std::vector<CabBatchTriangle> &l : lists) drawn.push_back(&l);
		CabRasterLists(drawn);
	});
	}
	for (const CabBridgeSprites &bs : _cab_bridge_sprites) {
		std::vector<const ParentSpriteToDraw *> list;
		for (size_t i = bs.from; i < bs.to && i < _vd.parent_sprites_to_draw.size(); i++) list.push_back(&_vd.parent_sprites_to_draw[i]);
		CabDrawBridgeMiddle(cv, eye, bs.tile, list);
	}
	for (TileIndex head : ramps) {
		/* The ramp's own sprites: those of its tile that are not of a bridge over it. */
		std::vector<const ParentSpriteToDraw *> list;
		int hx = TileX(head) * TILE_SIZE, hy = TileY(head) * TILE_SIZE;
		for (size_t i = 0; i < _vd.parent_sprites_to_draw.size(); i++) {
			const ParentSpriteToDraw &ps = _vd.parent_sprites_to_draw[i];
			if (ps_kind[i] != 4 || is_foundation[i] || ps.image == SPR_EMPTY_BOUNDING_BOX) continue;
			if (ps.xmin < hx || ps.xmin >= hx + static_cast<int>(TILE_SIZE) || ps.ymin < hy || ps.ymin >= hy + static_cast<int>(TILE_SIZE)) continue;
			list.push_back(&ps);
		}
		CabDrawBridgeRamp(cv, eye, head, list);
	}
	for (TileIndex end : tunnel_ends) {
		std::optional<CabTunnel> t = CabTunnelAt(end);
		if (!t.has_value()) continue;
		CabDrawPortal(cv, eye, *t, end);
		if (in_tunnel.has_value() && (in_tunnel->a == t->a || in_tunnel->a == t->b)) continue;
		/* Seen from outside: the tube through the opening only. */
		CabDrawTunnelSeenIn(cv, eye, *t, end);
	}
	if (in_tunnel.has_value()) {
		const CabTunnel &t = *in_tunnel;
		double s_e = t.axis == Axis::X ? eye.x : eye.y;
		double reach = CAB_RANGE_TILES * TILE_SIZE;
		CabDrawTube(cv, eye, t, std::max(t.s_lo, s_e - reach), std::min(t.s_hi, s_e + reach));
	}

	eye_stage(4);
	/* 5. Everything standing as a billboard facing the eye, far first: the tiles' sprites, then the other vehicles. */
	std::vector<CabBillboard> bills;
	for (size_t i = 0; i < _vd.parent_sprites_to_draw.size(); i++) {
		const ParentSpriteToDraw &ps = _vd.parent_sprites_to_draw[i];
		if (ps.image == SPR_EMPTY_BOUNDING_BOX || is_foundation[i] || ps_kind[i] >= 3) continue;
		double bx = (ps.xmin + ps.xmax + 1) * 0.5, by = (ps.ymin + ps.ymax + 1) * 0.5;
		CabPoint c = CabToEye(eye, bx, by, ps.zmin, 0, 0);
		if (c.f < CAB_NEAR * 0.5) continue;
		/* What stands on a house, industry, object or station tile with a footprint is a box of its sprite: a
		 * building or a roof of a height, a platform or a bench low and flat (a slab). A thin long box (a fence
		 * along a tile's edge) is a wall along its length, not a billboard: a billboard of it would stand as a
		 * tall post. The rest -- a tree, a signal, a lamp -- is a billboard. */
		int ex = ps.xmax - ps.xmin + 1, ey = ps.ymax - ps.ymin + 1, ez = ps.zmax - ps.zmin + 1;
		int kind = 0;
		double w0 = 0, w1 = 0;
		if (ps_kind[i] != 0 && ex >= 2 && ey >= 2 && (ez >= TILE_HEIGHT || ps_kind[i] == 2)) {
			kind = 3;
		} else if (ex >= 8 && ey <= 2) {
			kind = 1;
			w0 = ps.xmin;
			w1 = ps.xmax + 1;
		} else if (ey >= 8 && ex <= 2) {
			kind = 2;
			w0 = ps.ymin;
			w1 = ps.ymax + 1;
		}
		bills.push_back({c.f, bx, by, static_cast<double>(ps.zmin), kind, w0, w1, ps});
	}
	/* The other vehicles: their picture of the direction as seen from the eye, as the turned map shows them. */
	double yaw_deg = camera.heading * 180.0 / M_PI + 135.0;
	int step = static_cast<int>(std::lround(yaw_deg / 45.0)) & 7;
	const Vehicle *own = v != nullptr ? v->First() : nullptr;
	/* The cab the eye is in, round it. */
	if (in_model_cab) {
		AddCabVehicle(own_model, false);
		_cab_figures.back().own_cab = true;
		/* (The eye sits in it level: its cab not tipped round the eye.) */
		_cab_figures.back().pitch = 0;
	}
	for (const Vehicle *u : Vehicle::Iterate()) {
		if (u->First() == own) continue;
		if (u->vehstatus.Test(VehState::Hidden)) {
			/* Hidden in the tunnel the eye is in: seen there. */
			bool same_tunnel = in_tunnel.has_value() && (u->type == VehicleType::Train || u->type == VehicleType::Road) && (u->tile == in_tunnel->a || u->tile == in_tunnel->b);
			if (!same_tunnel) continue;
		}
		if (u->type == VehicleType::Effect && u->sprite_cache.sprite_seq.count == 0) continue;
		CabPoint c = CabToEye(eye, u->x_pos, u->y_pos, u->z_pos, 0, 0);
		if (c.f < -2.0 * TILE_SIZE || c.f > CAB_RANGE_TILES * TILE_SIZE || std::abs(c.r) > std::max(c.f, 0.0) * CAB_FOV_TAN + 2.0 * TILE_SIZE) continue;
		/* A vehicle drawn from its object: the object itself among the figures, at the angle it stands at -- the
		 * sprites of its eight directions are only for what is drawn from them (the player, 16. 10.). */
		ThreeDEyeVehicle ev;
		if (ThreeDEnabled() && u->type != VehicleType::Effect && u->direction != Direction::Invalid && !u->vehstatus.Test(VehState::Crashed) && GetThreeDEyeVehicle(u, ev)) {
			if (ev.obj != nullptr) AddCabVehicle(ev, false);
			continue;
		}
		if (c.f < CAB_NEAR * 0.5) continue;
		VehicleSpriteSeq seq;
		if (u->type == VehicleType::Effect || u->direction == Direction::Invalid) {
			seq = u->sprite_cache.sprite_seq;
		} else {
			u->GetImage(static_cast<Direction>((to_underlying(u->direction) - step) & 7), EngineImageType::OnMap, &seq);
			if (!seq.IsValid()) seq = u->sprite_cache.sprite_seq;
		}
		PaletteID pal = PAL_NONE;
		if (u->vehstatus.Test(VehState::DefaultPalette)) pal = (u->vehstatus.Test(VehState::Crashed) || u->IsWrecked()) ? PALETTE_CRASH : GetVehiclePalette(u);
		Point pt = RemapCoords(u->x_pos, u->y_pos, u->z_pos);
		for (uint k = 0; k < seq.count; k++) {
			const Sprite *spr = GetSprite(seq.seq[k].sprite & SPRITE_MASK, SpriteType::Normal);
			ParentSpriteToDraw ps{};
			ps.image = seq.seq[k].sprite;
			ps.pal = (seq.seq[k].pal != PAL_NONE && !u->vehstatus.Test(VehState::Crashed)) ? seq.seq[k].pal : pal;
			ps.sub = nullptr;
			ps.first_child = LAST_CHILD_NONE;
			ps.x = pt.x;
			ps.y = pt.y;
			ps.left = pt.x + spr->x_offs;
			ps.top = pt.y + spr->y_offs;
			/* Its box, the vehicle's: an empty one (all 0) read as a fence piece, a point under a tall sprite, and the
			 * vehicles around were not drawn at all (the player: "I don't see the cars around from the cab"). */
			ps.xmin = u->x_pos + u->bounds.origin.x;
			ps.ymin = u->y_pos + u->bounds.origin.y;
			ps.zmin = u->z_pos + u->bounds.origin.z;
			ps.xmax = ps.xmin + std::max<int>(u->bounds.extent.x, 2) - 1;
			ps.ymax = ps.ymin + std::max<int>(u->bounds.extent.y, 2) - 1;
			ps.zmax = ps.zmin + std::max<int>(u->bounds.extent.z, 5) - 1;
			bills.push_back({c.f, static_cast<double>(u->x_pos), static_cast<double>(u->y_pos), static_cast<double>(u->z_pos), 0, 0.0, 0.0, ps});
		}
	}
	/* The figures standing on the tiles (the clay town's mushrooms, the dragons): near the eye their triangles as they
	 * are, further billboards of their pictures as the turned map shows them, from the eye's side (the angle in steps,
	 * so that a few pictures serve every heading). */
	/* All of them drawn at once on every core with the frame: their pictures kept till it is taken (PinThreeDPictures()). */
	std::vector<std::pair<CabFigure, CabFigureModel>> solid_figures;
	for (CabFigure &fg : _cab_figures) {
		const CabPoint foot = CabToEye(eye, fg.x, fg.y, fg.z, 0, 0);
		if (foot.f > CAB_RANGE_TILES * TILE_SIZE || std::abs(foot.r) > std::max(foot.f, 0.0) * CAB_FOV_TAN + 2.0 * TILE_SIZE) continue;
		/* How big it is: its longest side, world units (a field's plant: hundreds of them, only at the walker's feet);
		 * a shape not read yet is read in the background, the figure a billboard or nothing this frame. */
		const ThreeDVehicleObject *model = fg.obj != nullptr ? fg.obj : (fg.plant ? nullptr : GetThreeDFigure(fg.name));
		const ThreeDVehicleShape *model_shape = model != nullptr ? model->TryGetShape(fg.frame) : nullptr;
		const double extent = model_shape == nullptr ? 0.0 : std::max({(model_shape->x1 - model_shape->x0) * fg.along, model_shape->y1 - model_shape->y0, model_shape->z1 - model_shape->z0}) * fg.size * OBJECT_UNITS_PER_METRE;
		if (foot.f < -extent) continue; // all of it behind the eye
		/* Near, a finer picture: about a pixel of it to a pixel of the view; a big one far, a coarser one. */
		const ZoomLevel fine = foot.f < 3.0 * TILE_SIZE ? ZoomLevel::In4x : (foot.f < 8.0 * TILE_SIZE ? ZoomLevel::In2x : (foot.f > CAB_COARSE ? ZoomLevel::Out2x : ZoomLevel::Normal));
		const double per = std::pow(2.0, to_underlying(fine) - to_underlying(ZoomLevel::Normal));
		/* A small thing (the player, #286: the mushrooms a hand high "are not there at all", nor the dogs): its picture
		 * would be far coarser than the place it takes on the screen, or but a few pixels, and it is lost -- drawn by
		 * its triangles instead, as simple as its size on the screen allows. */
		const double on_screen = extent * eye.focal / std::max(foot.f, CAB_NEAR);
		const double in_picture = extent / (CAB_PX_ACROSS * per);
		const bool small = !fg.plant && extent > 0 && on_screen >= 1.0 && (on_screen > 3.0 * in_picture || in_picture < 4.0);
		fg.solid = fg.own_cab || fg.missile || small || (fg.plant ? foot.f < CAB_PLANT_SOLID : on_screen >= _cab_solid_pixels);
		if (fg.solid) {
			/* What it needs, if it is there (CabFigureReady()): drawn by its triangles as a job of the frame, after the
			 * billboards, all of them at once on every core (the depth sorts them out). */
			CabFigureModel model;
			fg.solid = CabFigureReady(eye, fg, model);
			if (fg.solid) {
				solid_figures.emplace_back(fg, std::move(model));
				camera.billboards++;
				continue;
			}
		}
		if (foot.f < CAB_NEAR) continue;
		const float side = static_cast<float>(std::round(yaw_deg / 15.0) * 15.0);
		/* A vehicle at the angle the eye sees it at, in steps of 5 degrees (GetThreeDEyeVehiclePicture()). */
		const ThreeDModelPicture *pic = fg.obj != nullptr ? GetThreeDEyeVehiclePicture(CabFigureVehicle(fg), fine, static_cast<float>(yaw_deg)) :
				fg.plant ? GetThreeDPlantPicture(fg.name, fg.size, fg.stretch, fg.facing, fine, nullptr, side) : GetThreeDFigurePicture(fg.name, fg.size, fg.facing, fine, INVALID_TILE, 0, nullptr, fg.frame, side);
		if (pic == nullptr || pic->width <= 0 || pic->height <= 0) continue;
		const double left = foot.r + pic->left * CAB_PX_ACROSS * per, right = left + pic->width * CAB_PX_ACROSS * per;
		const double top = foot.u - pic->top * CAB_PX_UP * per, bottom = top - pic->height * CAB_PX_UP * per;
		const double tw = pic->width, th = pic->height;
		std::vector<CabPoint> quad = {
			{foot.f, left, bottom, 0.0, th},
			{foot.f, right, bottom, tw, th},
			{foot.f, right, top, tw, 0.0},
			{foot.f, left, top, 0.0, 0.0},
		};
		_cab_batching = true;
		CabDrawPolygon(dst, zb, width, height, pic->pixels.data(), pic->width, pic->height, std::move(quad), eye, false);
		_cab_batching = false;
		camera.billboards++;
	}
	CabFlushBatch(); // the figures' billboards, collected above
	CabFlushGlass();
	/* The figures by their triangles: a job of the frame, their glass over them after (CabFlushGlass()); inside a
	 * model's cab the near plane is the cab's. At night what has a light of its own is kept to be drawn again over
	 * the dark (glow_after_night). */
	auto glow_after_night = IsNightNow() ? std::make_shared<std::vector<CabBatchTriangle>>() : nullptr;
	if (!solid_figures.empty()) out.Job([eye, width, height, near = _cab_near, wheels_turn = _settings_client.gui.three_d_wheels, solid_figures = std::move(solid_figures), glow_after_night](uint32_t *dst, float *zb) {
		AutoRestoreBackup near_backup(_cab_near, near);
		AutoRestoreBackup glow_backup(_cab_glow_after_night, glow_after_night.get());
		/* Those the graphics card draws (EyeFigureOnCard()) laid in first, the rest by the processor. */
		std::vector<bool> on_card(solid_figures.size(), false);
		if (ThreeDGpuAvailable()) {
			ThreeDGpuEyePicture pic;
			pic.width = width;
			pic.height = height;
			pic.near_plane = static_cast<float>(near);
			for (size_t k = 0; k < solid_figures.size(); k++) {
				on_card[k] = EyeFigureOnCard(eye, width, height, near, solid_figures[k].first, solid_figures[k].second, glow_after_night != nullptr, wheels_turn, pic.meshes);
			}
			if (!pic.meshes.empty()) {
				if (ThreeDGpuRenderEye(pic)) {
					LayEyeCardPicture(pic, dst, zb);
				} else {
					on_card.assign(solid_figures.size(), false);
				}
			}
		}
		{
			CabBatchScope batch;
			for (size_t k = 0; k < solid_figures.size(); k++) {
				if (!on_card[k]) CabDrawFigureModel(dst, zb, width, height, eye, solid_figures[k].first, solid_figures[k].second, wheels_turn);
			}
		}
		CabFlushGlass();
	});
	std::sort(bills.begin(), bills.end(), [](const CabBillboard &a, const CabBillboard &b) { return a.f > b.f; });
	for (const CabBillboard &b : bills) {
		const CabTexture *tex = CabSpriteTexture(b.ps);
		if (tex == nullptr) continue;
		double bw_units = tex->w * CAB_PX_ACROSS, bh_units = tex->h * CAB_PX_UP;
		if (b.kind == 3) {
			if (_debug_misc_level >= 5) Debug(misc, 5, "cab box sprite {} box {}..{} {}..{} z {}..{} tex {}x{} at {},{} depth {:.0f}", b.ps.image & SPRITE_MASK, b.ps.xmin, b.ps.xmax, b.ps.ymin, b.ps.ymax, b.ps.zmin, b.ps.zmax, tex->w, tex->h, tex->ox, tex->oy, b.f);
			CabDrawBox(dst, zb, width, height, *tex, eye, b.ps);
			camera.billboards++;
			continue;
		}
		/* A fence of a farm, an industry or a station (a thin long box): a line of the north-up view, a ribbon from here; not drawn (the player). */
		if (b.kind == 1 || b.kind == 2) continue;
		if (b.kind == 0) {
			/* A line of a sprite -- a fence seen along its length, a wire -- would stand as a tall post; and a fence
			 * piece, a point of a box a few units high under a sprite many units tall, likewise. Neither is drawn. */
			int ex = b.ps.xmax - b.ps.xmin + 1, ey = b.ps.ymax - b.ps.ymin + 1, ez = b.ps.zmax - b.ps.zmin + 1;
			if (tex->w <= 2 && tex->h >= 8) continue;
			if (ex <= 1 && ey <= 1 && ez <= 4 && bh_units > 3.0 * ez) continue;
		}
		if (_debug_misc_level >= 5) {
			/* The rig: what the tall things on the picture are. */
			double sh = bh_units * eye.focal / std::max(b.f, 1.0), sw = bw_units * eye.focal / std::max(b.f, 1.0);
			if (sh > 80 && sh > 3 * sw) Debug(misc, 5, "cab tall sprite {} pal {:#x} box {}..{} {}..{} z {}..{} tex {}x{} wall {} depth {:.0f} on screen {:.0f}x{:.0f}", b.ps.image & SPRITE_MASK, b.ps.pal, b.ps.xmin, b.ps.xmax, b.ps.ymin, b.ps.ymax, b.ps.zmin, b.ps.zmax, tex->w, tex->h, b.kind, b.f, sw, sh);
		}
		const double tw = tex->w, th = tex->h;
		std::vector<CabPoint> quad;
		if (b.kind == 0) {
			CabPoint foot = CabToEye(eye, b.bx, b.by, b.bz, 0, 0);
			quad = {
				{foot.f, foot.r - bw_units / 2, foot.u, 0.0, th},
				{foot.f, foot.r + bw_units / 2, foot.u, tw, th},
				{foot.f, foot.r + bw_units / 2, foot.u + bh_units, tw, 0.0},
				{foot.f, foot.r - bw_units / 2, foot.u + bh_units, 0.0, 0.0},
			};
		} else {
			/* A wall from one end of the box to the other, the sprite's height tall; both faces are drawn. */
			if (_debug_misc_level >= 5) Debug(misc, 5, "cab wall sprite {} box {}..{} {}..{} z {}..{} tex {}x{} depth {:.0f}", b.ps.image & SPRITE_MASK, b.ps.xmin, b.ps.xmax, b.ps.ymin, b.ps.ymax, b.ps.zmin, b.ps.zmax, tex->w, tex->h, b.f);
			CabPoint a = b.kind == 1 ? CabToEye(eye, b.w0, b.by, b.bz, 0, 0) : CabToEye(eye, b.bx, b.w0, b.bz, 0, 0);
			CabPoint e = b.kind == 1 ? CabToEye(eye, b.w1, b.by, b.bz, 0, 0) : CabToEye(eye, b.bx, b.w1, b.bz, 0, 0);
			quad = {
				{a.f, a.r, a.u, 0.0, th},
				{e.f, e.r, e.u, tw, th},
				{e.f, e.r, e.u + bh_units, tw, 0.0},
				{a.f, a.r, a.u + bh_units, 0.0, 0.0},
			};
		}
		CabDrawPolygon(dst, zb, width, height, tex->px.data(), tex->w, tex->h, std::move(quad), eye, false);
		camera.billboards++;
	}

	/* 5b. The sun's shadows of the buildings about the eye, by day (DrawEyeShadows()): their shapes made now, as simple
	 * as a texel of the map of the shadows allows; the map and the shadows as a job of the frame. */
	const bool night = IsNightNow();
	if (!in_tunnel.has_value() && !night && _cab_shadows) {
		std::vector<CabShadowCaster> casters;
		for (const CabObject &co : objects) {
			const double ox = TileX(co.origin) * TILE_SIZE, oy = TileY(co.origin) * TILE_SIZE;
			if (std::hypot(ox - eye.x, oy - eye.y) > CAB_SHADOW_TILES * TILE_SIZE) continue;
			casters.push_back({co.obj, ox, oy, static_cast<double>(co.z), {}});
		}
		if (!casters.empty()) {
			const int level = ThreeDShapeLevel(OBJECT_UNITS_PER_METRE / CAB_SHADOW_TEXEL, 1.0);
			for (CabShadowCaster &c : casters) c.shape = c.obj->TryGetShape(level);
			out.Job([eye, width, height, casters = std::move(casters)](uint32_t *dst, float *zb) { DrawEyeShadows(eye, dst, zb, width, height, casters); });
		}
	}

	eye_stage(5);
	/* 6. The haze: the far fades into the sky at the horizon, the more the farther, to the far land's end (a stripe
	 * of rows a core). */
	const double haze_depth = CAB_FAR_TILES * TILE_SIZE * 0.4;
	if (!in_tunnel.has_value()) out.Job([width, height, haze, haze_depth](uint32_t *dst, float *zb) {
		ParallelRows(height, [&](int lo, int hi) {
			for (size_t i = static_cast<size_t>(lo) * width; i < static_cast<size_t>(hi) * width; i++) {
				float z = zb[i];
				if (z <= 0.0f) continue;
				double dist = 1.0 / z;
				if (dist <= CAB_HAZE_FROM) continue;
				double t = (1.0 - std::exp(-(dist - CAB_HAZE_FROM) / haze_depth)) * 0.8;
				Colour c(dst[i]);
				dst[i] = Colour(static_cast<uint8_t>(c.r + (haze.r - c.r) * t), static_cast<uint8_t>(c.g + (haze.g - c.g) * t), static_cast<uint8_t>(c.b + (haze.b - c.b) * t)).data;
			}
		});
	});
	_vd.tile_sprites_to_draw.clear();
	_vd.parent_sprites_to_draw.clear();
	_vd.child_screen_sprites_to_draw.clear();
	/* The haze far off (as OpenMW's fog): from 60 % of the far land's reach the picture fades into the colour of the
	 * horizon, and the ground beyond it (past the map's edge) is all haze, so the view ends softly. */
	{
		const double fog_near = CAB_FAR_TILES * TILE_SIZE * 0.6, fog_far = CAB_FAR_TILES * TILE_SIZE * 0.98;
		const Colour far_haze(150, 178, 205);
		out.Job([eye, width, height, fog_near, fog_far, far_haze](uint32_t *dst, float *zb) {
			ParallelRows(height, [&](int lo, int hi) {
			for (int y = lo; y < hi; y++) {
				for (int x = 0; x < width; x++) {
					const size_t i = static_cast<size_t>(y) * width + x;
					double f;
					if (zb[i] <= 0.0f) {
						if (y < eye.cy) continue; // the sky
						f = 1.0;
					} else {
						f = std::clamp((1.0 / zb[i] - fog_near) / (fog_far - fog_near), 0.0, 1.0);
						if (f <= 0.0) continue;
					}
					Colour c(dst[i]);
					dst[i] = Colour(static_cast<uint8_t>(c.r + (far_haze.r - c.r) * f), static_cast<uint8_t>(c.g + (far_haze.g - c.g) * f), static_cast<uint8_t>(c.b + (far_haze.b - c.b) * f)).data;
				}
			}
			});
		});
	}
	/* What the walker holds (the hammer), before the night: the dark is over it too. A job of the frame: it draws
	 * by its own means (DrawThreeDHeldThing()), from the thing's model, which stays. */
	/* The smoke of the walker's rockets, before the night: the dark is over it too (DrawEyeBlasts()). */
	if (walker != nullptr && !walker->blasts.empty()) out.Job([eye, width, height, blasts = walker->blasts](uint32_t *dst, float *zb) { DrawEyeBlasts(eye, dst, zb, width, height, blasts, false); });
	if (walker != nullptr && !walker->held.thing.empty()) out.Job([held = walker->held, width, height, focal = eye.focal](uint32_t *dst, float *zb) { DrawThreeDHeldThing(held, dst, zb, width, height, focal); });
	/* The light in its left hand (the torch, the lantern): by day with the weapon, at night after the dark (it lights
	 * itself, and about the walker, DrawEyeWitchLight()). */
	if (walker != nullptr && !walker->left.thing.empty() && !night) out.Job([left = walker->left, width, height, focal = eye.focal](uint32_t *dst, float *zb) { DrawThreeDHeldThing(left, dst, zb, width, height, focal); });
	/* The rockets' blasts, by day here, at night after the dark (they light themselves). */
	if (walker != nullptr && !walker->blasts.empty() && !night) out.Job([eye, width, height, blasts = walker->blasts](uint32_t *dst, float *zb) { DrawEyeBlasts(eye, dst, zb, width, height, blasts, true); });
	if (night) {
		/* The dark and the glows of the vehicles' lights: collected now, drawn with the frame. */
		const NightLight moon = GetNightLight();
		out.Job([eye, width, height, moon, glows = CollectEyeNightGlows(eye, own)](uint32_t *dst, float *zb) { DrawEyeNight(eye, width, height, dst, zb, moon, glows); });
		DrawEyeVehicleGlass(eye, dst, zb, width, height);
		/* What has a light of its own (the beacon of the fire Tatra, the ghost) again over the dark. */
		if (glow_after_night != nullptr) out.Job([glow_after_night](uint32_t *, float *) {
			const std::vector<CabBatchTriangle> *list = glow_after_night.get();
			CabRasterLists({&list, 1});
		});
		/* The lamps and lit windows of the houses lighting what is about them (DrawEyeLampLight()). */
		std::vector<std::array<double, 3>> lamps;
		for (const CabObject &co : objects) {
			const ThreeDObject &o = *co.obj;
			if (!o.IsRead()) continue; // read in the background, its lamps the next frames
			const double ox = TileX(co.origin) * TILE_SIZE, oy = TileY(co.origin) * TILE_SIZE;
			for (size_t i = 0; i + 2 < o.lights.size(); i += 3) lamps.push_back({ox + o.lights[i], oy + o.lights[i + 1], co.z + o.lights[i + 2]});
		}
		if (_cab_lamp_light) out.Job([eye, width, height, lamps = std::move(lamps)](uint32_t *dst, float *zb) { DrawEyeLampLight(eye, dst, zb, width, height, lamps); });
		/* The witches' lanterns and torches lighting what is about them and where the beam falls (DrawEyeWitchLight()),
		 * and the light in the walker's left hand about it, warm, from just before it at the height of the hand. */
		std::vector<ThreeDWitchLight> witch_lights = GetThreeDWitchLights(true);
		if (walker != nullptr && walker->light_colour != 0 && walker->light_reach_m > 0) {
			ThreeDWitchLight &l = witch_lights.emplace_back();
			l.x = walker->x + std::cos(walker->heading) * 0.4 * OBJECT_UNITS_PER_METRE;
			l.y = walker->y + std::sin(walker->heading) * 0.4 * OBJECT_UNITS_PER_METRE;
			l.z = FirstPersonGroundZ(walker->x, walker->y) + walker->eye_z - 0.5 * OBJECT_HEIGHT_PER_METRE;
			l.reach = walker->light_reach_m * OBJECT_UNITS_PER_METRE;
			l.colour = Colour(static_cast<uint8_t>(walker->light_colour >> 16), static_cast<uint8_t>(walker->light_colour >> 8), static_cast<uint8_t>(walker->light_colour));
		}
		/* A rocket's blast lighting what is about it, as it swells and fades. */
		if (walker != nullptr) {
			for (const ThreeDBlast &b : walker->blasts) {
				if (!b.light || b.alpha <= 0) continue;
				ThreeDWitchLight &l = witch_lights.emplace_back();
				l.x = b.x;
				l.y = b.y;
				l.z = b.z;
				l.reach = (3.0 + 2.0 * b.radius_m) * b.alpha * OBJECT_UNITS_PER_METRE; // a blast's some 10 m, a rocket's flame 3
				l.colour = Colour(255, 160, 70);
			}
		}
		if (!witch_lights.empty()) {
			out.Job([eye, width, height, witch_lights = std::move(witch_lights)](uint32_t *dst, float *zb) { DrawEyeWitchLight(eye, dst, zb, width, height, witch_lights); });
		}
		if (walker != nullptr && !walker->blasts.empty()) out.Job([eye, width, height, blasts = walker->blasts](uint32_t *dst, float *zb) { DrawEyeBlasts(eye, dst, zb, width, height, blasts, true); });
		if (walker != nullptr && !walker->left.thing.empty()) out.Job([left = walker->left, width, height, focal = eye.focal](uint32_t *dst, float *zb) { DrawThreeDHeldThing(left, dst, zb, width, height, focal); });
		{
			const double t = static_cast<double>(TimerGameTick::counter) * 0.15;
			/* The lighter the night (gui.night_light), the fewer stars are seen: towards the dawn only the brightest. */
			const double seen = 1.0 - 0.2 * std::min<int>(_settings_client.gui.night_light, 4);
			out.Job([eye, width, height, t, seen](uint32_t *dst, float *zb) { DrawEyeSky(eye, width, height, dst, zb, t, seen); });
		}
		/* The lit windows of the houses (their "noc") over the dark, as they were drawn, and the glows of their
		 * lights ("svetlo_NN"): seen where nothing nearer is in front of them -- told by the depth when the frame is drawn. */
		const Colour window(255, 200, 110);
		struct WindowGlow {
			double sx, sy, r;
			float iw;
		};
		std::vector<WindowGlow> window_glows;
		for (const CabObject &co : objects) {
			const ThreeDObject &o = *co.obj;
			if (!o.IsRead() || (o.night_tri.empty() && o.lights.empty())) continue;
			const double ox = TileX(co.origin) * TILE_SIZE, oy = TileY(co.origin) * TILE_SIZE, oz = co.z;
			const CabPoint c = CabToEye(eye, ox + o.tiles_x * TILE_SIZE / 2.0, oy + o.tiles_y * TILE_SIZE / 2.0, oz, 0, 0);
			/* Near and on the far land: the lights of the towns on the horizon. */
			if (c.f < -1.0 * TILE_SIZE * std::max(o.tiles_x, o.tiles_y) || c.f > CAB_FAR_OBJECT_TILES * TILE_SIZE) continue;
			/* The texture at the level of detail of its distance, as the house (4b. above); one not read yet, not this frame. */
			const ThreeDObjectTexture *full = o.TryGetTexture(0);
			if (full == nullptr) continue;
			const double texels_per_unit = full->w / static_cast<double>(TILE_SIZE), pixels_per_unit = eye.focal / std::max(c.f, CAB_NEAR * 4);
			const int level = std::max(0, static_cast<int>(std::floor(std::log2(std::max(1.0, texels_per_unit / std::max(pixels_per_unit, 1e-6))))));
			const ThreeDObjectTexture *tex = o.TryGetTexture(level, o.PaintAt(co.origin));
			for (size_t t = 0; t + 2 < o.night_tri.size(); t += 3) {
				/* An add-on's lamp in the add-on's texture, in its colour at this place. */
				const uint8_t n = o.NightTexture(t);
				const ThreeDObjectTexture *nt = n == 0 ? tex : o.TryGetTextureOf(n, level, o.AddonPaintAt(co.origin, n));
				if (nt == nullptr) continue;
				std::vector<CabPoint> poly;
				for (int k = 0; k < 3; k++) {
					const uint32_t i = o.night_tri[t + k];
					poly.push_back(CabToEye(eye, ox + o.night_pos[i * 3], oy + o.night_pos[i * 3 + 1], oz + o.night_pos[i * 3 + 2], o.night_uv[i * 2] * nt->w, o.night_uv[i * 2 + 1] * nt->h));
				}
				CabDrawPolygon(dst, zb, width, height, nt->px.data(), nt->w, nt->h, std::move(poly), eye, false);
			}
			for (size_t i = 0; i + 2 < o.lights.size(); i += 3) {
				const CabPoint p = CabToEye(eye, ox + o.lights[i], oy + o.lights[i + 1], oz + o.lights[i + 2], 0, 0);
				if (p.f < CAB_NEAR) continue;
				const CabVertex v = CabProject(eye, p);
				window_glows.push_back({v.sx, v.sy, std::max(1.0, 1.2 * eye.focal / p.f), static_cast<float>(v.iw)});
			}
		}
		if (!window_glows.empty()) out.Job([width, height, window, window_glows = std::move(window_glows)](uint32_t *dst, float *zb) {
			for (const WindowGlow &g : window_glows) {
				const int px = static_cast<int>(g.sx), py = static_cast<int>(g.sy);
				if (px >= 0 && py >= 0 && px < width && py < height && zb[static_cast<size_t>(py) * width + px] > g.iw * 1.05f) continue;
				AddNightLight(dst, width, width, height, g.sx, g.sy, g.r, g.r, window, 0.35);
			}
		});
	}
	eye_stage(6);
	out.collect_ms = TurnedNow() - t0;
	camera.ms = out.collect_ms;
	_cab_near = CAB_NEAR;
	return true;
}

/*
 * The frames drawn (EyeFrame): handed to the thread (EyeFrameThread()), drawn, taken by the window that asked for
 * them. One at a time: a frame is collected only when the one before was taken, so what the collecting writes is not
 * read meanwhile; a window whose turn it is not shows the picture it has.
 */
static std::mutex _eye_mutex;                    ///< of _eye_to_draw, _eye_drawn, _eye_drawing and _eye_stop
static std::condition_variable _eye_wake;
static std::unique_ptr<EyeFrame> _eye_to_draw;   ///< handed to the thread, not in its hands yet
static std::unique_ptr<EyeFrame> _eye_drawn;     ///< drawn, waiting for its window to take it
static bool _eye_drawing = false;                ///< the thread has one in hand
static bool _eye_stop = false;
static std::thread _eye_thread;
static bool _eye_thread_failed = false;          ///< it could not be started: the frames are drawn at once, as before
static std::vector<std::unique_ptr<EyeFrame>> _eye_free; ///< frames done with, their buffers for the next (the main thread's)
static const void *_eye_last_client = nullptr;  ///< whose frame was collected last: two windows take turns (EyeView())
static const void *_eye_asked_client = nullptr; ///< a window that asked while another's frame was on the way
bool _eye_synchronous = false;

/** Draw a frame: its passes in their order, the triangles of each at once on every core of the drawing (CabRasterLists()). */
static void DrawEyeFrame(EyeFrame &f)
{
	const double start = TurnedNow();
	for (EyePass &p : f.passes) {
		if (p.work) {
			p.work(f.pixels.data(), f.depth.data());
		} else {
			const std::vector<CabBatchTriangle> *list = &p.triangles;
			CabRasterLists({&list, 1});
		}
	}
	if (_debug_misc_level >= 6) {
		/* The rig: the picture as a PPM in the working directory, and its depth: black where nothing is drawn, the nearer the lighter. */
		auto fz = FileHandle::Open(std::string("cab_depth.pgm"), "wb");
		if (fz.has_value()) {
			std::string head = fmt::format("P5\n{} {}\n255\n", f.width, f.height);
			fwrite(head.data(), 1, head.size(), *fz);
			for (float z : f.depth) {
				uint8_t v = z <= 0.0f ? 0 : static_cast<uint8_t>(std::clamp(255.0 - 255.0 * std::log2(1.0 / z) / 9.0, 1.0, 255.0));
				fwrite(&v, 1, 1, *fz);
			}
		}
		auto fp = FileHandle::Open(std::string("cab_picture.ppm"), "wb");
		if (fp.has_value()) {
			std::string head = fmt::format("P6\n{} {}\n255\n", f.width, f.height);
			fwrite(head.data(), 1, head.size(), *fp);
			for (uint32_t p : f.pixels) {
				Colour c(p);
				uint8_t rgb[3] = {c.r, c.g, c.b};
				fwrite(rgb, 1, 3, *fp);
			}
		}
	}
	/* What the frame pointed to is let go of here: nothing of it is read again. */
	f.passes.clear();
	f.colours.clear();
	f.sprites.clear();
	f.canvases.clear();
	f.draw_ms = TurnedNow() - start;
}

/** The thread drawing the frames: each as it comes, into _eye_drawn. */
static void EyeFrameThread()
{
	std::unique_lock lock(_eye_mutex);
	for (;;) {
		_eye_wake.wait(lock, [] { return _eye_stop || _eye_to_draw != nullptr; });
		if (_eye_stop) return;
		std::unique_ptr<EyeFrame> f = std::move(_eye_to_draw);
		_eye_drawing = true;
		lock.unlock();
		DrawEyeFrame(*f);
		f->drawn_at = std::chrono::steady_clock::now();
		lock.lock();
		_eye_drawn = std::move(f);
		_eye_drawing = false;
		_eye_wake.notify_all();
	}
}

/** Whether a frame is being drawn (or is about to be): what its triangles point to stays as it is till then. */
bool EyeFrameDrawing()
{
	std::lock_guard lock(_eye_mutex);
	return _eye_to_draw != nullptr || _eye_drawing;
}

/** A frame done with: the pictures it pinned let go of, its buffers kept for the next. */
static void FreeEyeFrame(std::unique_ptr<EyeFrame> f)
{
	if (f->pinned) {
		PinThreeDPictures(false);
		f->pinned = false;
	}
	if (_eye_free.size() < 3) _eye_free.push_back(std::move(f));
}

/** A frame to collect into: one done with, or a new one. */
static std::unique_ptr<EyeFrame> NewEyeFrame(const void *client)
{
	std::unique_ptr<EyeFrame> f;
	if (!_eye_free.empty()) {
		f = std::move(_eye_free.back());
		_eye_free.pop_back();
	} else {
		f = std::make_unique<EyeFrame>();
	}
	f->passes.clear();
	f->colours.clear();
	f->sprites.clear();
	f->canvases.clear();
	f->client = client;
	f->depot = false;
	f->pinned = false;
	f->collect_ms = f->draw_ms = 0;
	return f;
}

/** The picture of a frame drawn becomes the window's, with how long it took. */
static void TakeEyeFrame(EyeFrame &f, CabViewCamera &camera, CabViewPicture &out)
{
	out.width = f.width;
	out.height = f.height;
	out.pixels.swap(f.pixels);
	camera.collect_ms = f.collect_ms;
	camera.draw_ms = f.draw_ms;
	camera.ms = f.collect_ms + f.draw_ms;
	camera.fresh = true;
}

/**
 * Wait till the thread has drawn what it has, and let the frame drawn go: the objects the frames draw from are read
 * anew (ResetThreeDVehicleObjectSprites()).
 */
void WaitEyeFrame()
{
	std::unique_ptr<EyeFrame> drawn;
	{
		std::unique_lock lock(_eye_mutex);
		_eye_wake.wait(lock, [] { return _eye_to_draw == nullptr && !_eye_drawing; });
		drawn = std::move(_eye_drawn);
	}
	if (drawn != nullptr) FreeEyeFrame(std::move(drawn));
}

/** Stop the thread drawing the frames (ShutdownGame()). */
void StopEyeFrameThread()
{
	if (_eye_thread.joinable()) {
		{
			std::lock_guard lock(_eye_mutex);
			_eye_stop = true;
		}
		_eye_wake.notify_all();
		_eye_thread.join();
	}
	_eye_stop = false;
	_eye_to_draw.reset();
	_eye_drawn.reset();
	_eye_drawing = false;
	_eye_free.clear();
	PinThreeDPictures(false);
}

/** A window's frame drawn or to be drawn goes with the window (its camera): the pictures it pinned are let go of. */
void DropEyeFrame(const void *client)
{
	std::unique_ptr<EyeFrame> drawn;
	{
		std::unique_lock lock(_eye_mutex);
		if (_eye_to_draw != nullptr && _eye_to_draw->client == client) {
			/* About to be drawn, or being drawn: a moment. */
			_eye_wake.wait(lock, [] { return _eye_to_draw == nullptr && !_eye_drawing; });
		}
		if (_eye_drawn != nullptr && _eye_drawn->client == client) drawn = std::move(_eye_drawn);
	}
	if (drawn != nullptr) FreeEyeFrame(std::move(drawn));
}

/**
 * The view from an eye for a window: the frame drawn for it taken into its picture, and a new one collected and
 * handed to the thread when none is on the way (the window shows what it has meanwhile). The rig (_eye_synchronous),
 * or without the thread: collected, drawn and taken at once, as before.
 * @param camera the window's eye between frames; whose frame a frame is
 * @param[out] out the window's picture: the last one drawn
 * @return whether out holds a picture
 */
static bool EyeView(const Vehicle *v, const FirstPersonEye *walker, int width, int height, CabViewCamera &camera, CabViewPicture &out)
{
	camera.fresh = false;
	if (_eye_synchronous || _eye_thread_failed) {
		/* What is not read yet is read now (the rig: a picture is of the moment). */
		AutoRestoreBackup at_once(_three_d_load_at_once, true);
		std::unique_ptr<EyeFrame> f = NewEyeFrame(&camera);
		if (!CollectEyeFrame(v, walker, width, height, camera, *f)) {
			FreeEyeFrame(std::move(f));
			return false;
		}
		if (!f->depot) DrawEyeFrame(*f);
		TakeEyeFrame(*f, camera, out);
		FreeEyeFrame(std::move(f));
		return true;
	}
	std::unique_ptr<EyeFrame> done, stale;
	bool busy;
	{
		std::lock_guard lock(_eye_mutex);
		if (_eye_drawn != nullptr) {
			if (_eye_drawn->client == &camera) {
				done = std::move(_eye_drawn);
			} else if (std::chrono::steady_clock::now() - _eye_drawn->drawn_at > std::chrono::milliseconds(500)) {
				stale = std::move(_eye_drawn); // another window's, not taken for long (hidden, gone): dropped
			}
		}
		/* Another window's frame drawn waits for it: nothing is collected over it. */
		busy = _eye_to_draw != nullptr || _eye_drawing || _eye_drawn != nullptr;
	}
	if (stale != nullptr) FreeEyeFrame(std::move(stale));
	if (done != nullptr) {
		TakeEyeFrame(*done, camera, out);
		FreeEyeFrame(std::move(done));
	}
	if (busy) {
		if (&camera != _eye_last_client) _eye_asked_client = &camera;
	} else if (_eye_asked_client != nullptr && _eye_asked_client != &camera && _eye_last_client == &camera) {
		/* Another window asked while this one's frame was drawn: its turn (the cab and the walker both open). */
		_eye_asked_client = nullptr;
	} else {
		_eye_asked_client = nullptr;
		_eye_last_client = &camera;
		std::unique_ptr<EyeFrame> f = NewEyeFrame(&camera);
		if (!CollectEyeFrame(v, walker, width, height, camera, *f)) {
			FreeEyeFrame(std::move(f));
		} else if (f->depot) {
			/* Nothing to draw: the picture is as filled. */
			TakeEyeFrame(*f, camera, out);
			FreeEyeFrame(std::move(f));
		} else if (!_eye_thread.joinable() && !StartNewThread(&_eye_thread, "ottd:3d-eye", &EyeFrameThread)) {
			_eye_thread_failed = true;
			Debug(misc, 0, "3D: the thread drawing the view from the eyes cannot be started; drawn at once");
			DrawEyeFrame(*f);
			TakeEyeFrame(*f, camera, out);
			FreeEyeFrame(std::move(f));
		} else {
			{
				std::lock_guard lock(_eye_mutex);
				_eye_to_draw = std::move(f);
			}
			_eye_wake.notify_all();
		}
	}
	return out.width > 0;
}

/** The view from the cab of a vehicle (cab_view.h). */
bool DrawCabView(const Vehicle *v, int width, int height, CabViewCamera &camera, CabViewPicture &out)
{
	return v != nullptr && EyeView(v, nullptr, width, height, camera, out);
}

/** The view from the eyes of the walker of the view from the first person (first_person_gui.cpp). */
bool DrawFirstPersonView(const FirstPersonEye &walker, int width, int height, CabViewCamera &camera, CabViewPicture &out)
{
	return EyeView(nullptr, &walker, width, height, camera, out);
}
