/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file copypaste_gui.cpp Copying a patch of railway and road and building it again elsewhere.
 *
 * A junction that took a quarter of an hour to lay out is a junction a player
 * will want again, and laying it a second time by hand is the same quarter of an
 * hour with nothing new in it. This marks out a patch of map, remembers what is
 * built on it and where, and builds the same thing again wherever it is put
 * down.
 *
 * What is remembered is a list of things to build, not a picture of the map.
 * Putting it down is therefore the ordinary build commands, one after another,
 * exactly as if the player had laid every piece by hand: it costs what it costs,
 * it obeys who owns what, it refuses what the ground will not take, and it works
 * the same in a game with other people in it. Nothing here writes on the map by
 * itself.
 *
 * Anything that is not track, road or a depot is passed over. A station belongs
 * to a station, a bridge and a tunnel have two ends and only one of them may be
 * inside the marked patch, and a half of either is not a thing that can be
 * built. Those are their own job.
 */

#include "stdafx.h"
#include "copypaste_func.h"
#include "command_func.h"
#include "error.h"
#include "company_func.h"
#include "rail_cmd.h"
#include "rail_map.h"
#include "road_cmd.h"
#include "road_map.h"
#include "sound_func.h"
#include "strings_func.h"
#include "tilehighlight_func.h"
#include "viewport_func.h"
#include "window_func.h"
#include "window_gui.h"

#include "widgets/copypaste_widget.h"

#include "table/strings.h"

#include "safeguards.h"

/** One thing found on one tile of the marked patch, and where it sat in it. */
struct CopiedTile {
	uint16_t dx = 0; ///< Tiles east of the patch's corner.
	uint16_t dy = 0; ///< Tiles south of the patch's corner.

	/* Plain track, and whatever signals stand on it. */
	RailType railtype = INVALID_RAILTYPE;
	TrackBits tracks{};
	uint8_t present_signals = 0; ///< As GetPresentSignals(), so both sides of both tracks.
	std::array<SignalType, to_underlying(Track::End)> signal_types{};
	std::array<SignalVariant, to_underlying(Track::End)> signal_variants{};

	/* Road and tram, which live on the same tile and are built separately. */
	RoadType road_type = INVALID_ROADTYPE;
	RoadBits road_bits{};
	RoadType tram_type = INVALID_ROADTYPE;
	RoadBits tram_bits{};

	/* A depot is one tile and one direction. */
	bool rail_depot = false;
	bool road_depot = false;
	DiagDirection depot_dir = DiagDirection::Begin;
};

/** What the player last marked out, kept until they mark out something else. */
struct CopiedArea {
	uint16_t w = 0; ///< Width of the patch in tiles.
	uint16_t h = 0; ///< Height of the patch in tiles.
	std::vector<CopiedTile> tiles{};

	bool IsEmpty() const { return this->tiles.empty(); }

	void Clear()
	{
		this->w = 0;
		this->h = 0;
		this->tiles.clear();
	}
};

static CopiedArea _copied_area;

/**
 * Read one tile, and say whether anything worth remembering was on it.
 *
 * @param tile the tile to read
 * @param[out] out what was found
 * @return whether anything was found
 */
static bool ReadTile(TileIndex tile, CopiedTile &out)
{
	bool found = false;

	if (IsPlainRailTile(tile)) {
		out.railtype = GetRailType(tile);
		out.tracks = GetTrackBits(tile);
		found = out.tracks.Any();

		if (GetRailTileType(tile) == RailTileType::Signals) {
			out.present_signals = static_cast<uint8_t>(GetPresentSignals(tile));
			for (Track track : out.tracks) {
				if (!HasSignalOnTrack(tile, track)) continue;
				out.signal_types[to_underlying(track)] = GetSignalType(tile, track);
				out.signal_variants[to_underlying(track)] = GetSignalVariant(tile, track);
			}
		}
	} else if (IsRailDepotTile(tile)) {
		out.railtype = GetRailType(tile);
		out.rail_depot = true;
		out.depot_dir = GetRailDepotDirection(tile);
		found = true;
	} else if (IsRoadDepotTile(tile)) {
		out.road_depot = true;
		out.depot_dir = GetRoadDepotDirection(tile);
		out.road_type = GetRoadTypeRoad(tile);
		out.tram_type = GetRoadTypeTram(tile);
		found = true;
	}

	/* Road and tram sit on their own tiles and on level crossings alike, so they
	 * are read whatever else the tile turned out to be. */
	if (IsNormalRoadTile(tile) || IsLevelCrossingTile(tile)) {
		RoadBits road = IsLevelCrossingTile(tile) ? GetCrossingRoadBits(tile) : GetRoadBits(tile, RoadTramType::Road);
		RoadBits tram = IsLevelCrossingTile(tile) ? GetCrossingRoadBits(tile) : GetRoadBits(tile, RoadTramType::Tram);

		if (GetRoadTypeRoad(tile) != INVALID_ROADTYPE && road.Any()) {
			out.road_type = GetRoadTypeRoad(tile);
			out.road_bits = road;
			found = true;
		}
		if (GetRoadTypeTram(tile) != INVALID_ROADTYPE && tram.Any()) {
			out.tram_type = GetRoadTypeTram(tile);
			out.tram_bits = tram;
			found = true;
		}
	}

	return found;
}

/**
 * Remember what is built on the marked patch.
 *
 * @param start one corner of the patch
 * @param end the other corner
 */
static void CopyArea(TileIndex start, TileIndex end)
{
	uint sx = std::min(TileX(start), TileX(end));
	uint sy = std::min(TileY(start), TileY(end));
	uint ex = std::max(TileX(start), TileX(end));
	uint ey = std::max(TileY(start), TileY(end));

	_copied_area.Clear();
	_copied_area.w = ex - sx + 1;
	_copied_area.h = ey - sy + 1;

	for (uint y = sy; y <= ey; y++) {
		for (uint x = sx; x <= ex; x++) {
			CopiedTile ct;
			ct.dx = x - sx;
			ct.dy = y - sy;
			if (ReadTile(TileXY(x, y), ct)) _copied_area.tiles.push_back(ct);
		}
	}

	if (_copied_area.IsEmpty()) _copied_area.Clear();
	InvalidateWindowClassesData(WindowClass::CopyPaste);
}

/**
 * Build what was remembered, with the patch's corner put on @p corner.
 *
 * Track goes down before signals, because a signal needs the track it stands on;
 * beyond that the order does not matter. Anything the ground refuses is simply
 * not built -- the rest still is, and the player is told by the usual error the
 * command itself raises.
 *
 * @param corner where the patch's corner is to go
 */
static void PasteArea(TileIndex corner)
{
	uint cx = TileX(corner);
	uint cy = TileY(corner);

	/* Off the edge of the map is not somewhere to put it. */
	if (cx + _copied_area.w > Map::SizeX() || cy + _copied_area.h > Map::SizeY()) {
		ShowErrorMessage(GetEncodedString(STR_ERROR_CAN_T_PASTE_AREA), GetEncodedString(STR_ERROR_SITE_UNSUITABLE), WarningLevel::Info);
		return;
	}

	for (const CopiedTile &ct : _copied_area.tiles) {
		TileIndex tile = TileXY(cx + ct.dx, cy + ct.dy);

		if (ct.rail_depot) {
			Command<Commands::BuildRailDepot>::Post(STR_ERROR_CAN_T_BUILD_TRAIN_DEPOT, tile, ct.railtype, ct.depot_dir);
			continue;
		}

		if (ct.road_depot) {
			RoadType rt = ct.road_type != INVALID_ROADTYPE ? ct.road_type : ct.tram_type;
			if (rt != INVALID_ROADTYPE) Command<Commands::BuildRoadDepot>::Post(STR_ERROR_CAN_T_BUILD_ROAD_DEPOT, tile, rt, ct.depot_dir);
			continue;
		}

		if (ct.railtype != INVALID_RAILTYPE) {
			for (Track track : ct.tracks) {
				Command<Commands::BuildRail>::Post(STR_ERROR_CAN_T_BUILD_RAILROAD_TRACK, tile, ct.railtype, track, false);
			}
		}

		if (ct.road_type != INVALID_ROADTYPE && ct.road_bits.Any()) {
			Command<Commands::BuildRoad>::Post(STR_ERROR_CAN_T_BUILD_ROAD_HERE, tile, ct.road_bits, ct.road_type, DisallowedRoadDirections{}, TownID::Invalid());
		}
		if (ct.tram_type != INVALID_ROADTYPE && ct.tram_bits.Any()) {
			Command<Commands::BuildRoad>::Post(STR_ERROR_CAN_T_BUILD_ROAD_HERE, tile, ct.tram_bits, ct.tram_type, DisallowedRoadDirections{}, TownID::Invalid());
		}
	}

	/* Signals afterwards, once every piece of track they stand on is down. */
	for (const CopiedTile &ct : _copied_area.tiles) {
		if (ct.present_signals == 0) continue;
		TileIndex tile = TileXY(cx + ct.dx, cy + ct.dy);

		for (Track track : ct.tracks) {
			uint8_t on_this_track = ct.present_signals & static_cast<uint8_t>(SignalOnTrack(track));
			if (on_this_track == 0) continue;

			SignalType type = ct.signal_types[to_underlying(track)];
			SignalVariant variant = ct.signal_variants[to_underlying(track)];
			/* The last parameter carries the exact pair of bits, so which way the
			 * signal faces comes over with it rather than being guessed at. */
			Command<Commands::BuildSignal>::Post(STR_ERROR_CAN_T_BUILD_SIGNALS_HERE, tile, track, type, variant, false, false, false, type, type, 0, on_this_track);
		}
	}
}

/** Window from which a patch of map is marked out and put down again. */
struct CopyPasteWindow : Window {
	WidgetID last_user_action = INVALID_WIDGET;

	CopyPasteWindow(WindowDesc &desc, WindowNumber window_number) : Window(desc)
	{
		this->InitNested(window_number);
	}

	void Close([[maybe_unused]] int data = 0) override
	{
		if (_thd.window_class == this->window_class) ResetObjectToPlace();
		this->Window::Close();
	}

	std::string GetWidgetString(WidgetID widget, StringID stringid) const override
	{
		if (widget != WID_CP_INFO) return this->Window::GetWidgetString(widget, stringid);

		if (_copied_area.IsEmpty()) return GetString(STR_COPY_PASTE_NOTHING_COPIED);
		return GetString(STR_COPY_PASTE_AREA_COPIED, _copied_area.w, _copied_area.h, static_cast<uint>(_copied_area.tiles.size()));
	}

	void OnPaint() override
	{
		this->SetWidgetDisabledState(WID_CP_PASTE, _copied_area.IsEmpty());
		this->SetWidgetDisabledState(WID_CP_CLEAR, _copied_area.IsEmpty());
		this->DrawWidgets();
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_CP_COPY:
				HandlePlacePushButton(this, WID_CP_COPY, SPR_CURSOR_QUERY, HT_RECT);
				this->last_user_action = widget;
				break;

			case WID_CP_PASTE:
				if (_copied_area.IsEmpty()) break;
				HandlePlacePushButton(this, WID_CP_PASTE, SPR_CURSOR_QUERY, HT_RECT);
				this->last_user_action = widget;
				/* Carry the shape of what is going to be built on the pointer, so
				 * it can be lined up before anything happens. */
				if (this->IsWidgetLowered(WID_CP_PASTE)) SetTileSelectSize(_copied_area.w, _copied_area.h);
				break;

			case WID_CP_CLEAR:
				_copied_area.Clear();
				this->SetDirty();
				break;

			default: break;
		}
	}

	void OnPlaceObject([[maybe_unused]] Point pt, TileIndex tile) override
	{
		switch (this->last_user_action) {
			case WID_CP_COPY:
				VpStartPlaceSizing(tile, VPM_X_AND_Y, DDSP_COPY_AREA);
				break;

			case WID_CP_PASTE:
				PasteArea(tile);
				break;

			default: break;
		}
	}

	void OnPlaceDrag(ViewportPlaceMethod select_method, [[maybe_unused]] ViewportDragDropSelectionProcess select_proc, [[maybe_unused]] Point pt) override
	{
		VpSelectTilesWithMethod(pt.x, pt.y, select_method);
	}

	void OnPlaceMouseUp([[maybe_unused]] ViewportPlaceMethod select_method, ViewportDragDropSelectionProcess select_proc, [[maybe_unused]] Point pt, TileIndex start_tile, TileIndex end_tile) override
	{
		if (select_proc != DDSP_COPY_AREA) return;
		if (start_tile == INVALID_TILE || end_tile == INVALID_TILE) return;

		CopyArea(start_tile, end_tile);

		/* Marking out is over the moment it is done -- the patch is remembered
		 * and the highlight has nothing left to say. */
		ResetObjectToPlace();
		this->SetDirty();
	}

	void OnPlaceObjectAbort() override
	{
		SetTileSelectSize(1, 1);
		this->RaiseButtons();
		this->last_user_action = INVALID_WIDGET;
	}

	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		if (!gui_scope) return;
		this->SetDirty();
	}
};

static constexpr std::initializer_list<NWidgetPart> _nested_copypaste_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::DarkGreen),
		NWidget(WWT_CAPTION, Colours::DarkGreen, WID_CP_CAPTION), SetStringTip(STR_COPY_PASTE_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_STICKYBOX, Colours::DarkGreen),
	EndContainer(),
	NWidget(WWT_PANEL, Colours::DarkGreen),
		NWidget(NWID_VERTICAL), SetPIP(0, WidgetDimensions::unscaled.vsep_normal, 0), SetPadding(WidgetDimensions::unscaled.framerect),
			NWidget(WWT_TEXT, Colours::DarkGreen, WID_CP_INFO), SetMinimalSize(160, 12), SetFill(1, 0),
			NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0),
				NWidget(WWT_TEXTBTN, Colours::DarkGreen, WID_CP_COPY), SetMinimalSize(60, 12), SetFill(1, 0),
						SetStringTip(STR_COPY_PASTE_COPY_BUTTON, STR_COPY_PASTE_COPY_TOOLTIP),
				NWidget(WWT_TEXTBTN, Colours::DarkGreen, WID_CP_PASTE), SetMinimalSize(60, 12), SetFill(1, 0),
						SetStringTip(STR_COPY_PASTE_PASTE_BUTTON, STR_COPY_PASTE_PASTE_TOOLTIP),
				NWidget(WWT_PUSHTXTBTN, Colours::DarkGreen, WID_CP_CLEAR), SetMinimalSize(60, 12), SetFill(1, 0),
						SetStringTip(STR_COPY_PASTE_CLEAR_BUTTON, STR_COPY_PASTE_CLEAR_TOOLTIP),
			EndContainer(),
		EndContainer(),
	EndContainer(),
};

static WindowDesc _copypaste_desc(
	WindowPosition::Manual, {}, 0, 0,
	WindowClass::CopyPaste, WindowClass::None,
	{},
	_nested_copypaste_widgets
);

/** Open the copy/paste window, over at the right-hand edge where it is out of the way. */
void ShowCopyPasteToolbar()
{
	Window *w = BringWindowToFrontById(WindowClass::CopyPaste, 0);
	if (w != nullptr) return;

	w = new CopyPasteWindow(_copypaste_desc, 0);
	/* The map is what is being worked on, so the window keeps to the edge of it
	 * rather than sitting in the middle of the thing the player is looking at. */
	w->left = std::max(0, _screen.width - w->width - 10);
	w->top = 40;
	w->SetDirty();
}
