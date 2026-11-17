/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 * Changed for ottd Decouple by Karel Mácha: the changes Copyright (C) 2026 Karel Mácha.
 */

/** @file airport_gui.cpp The GUI for airports. */

#include "stdafx.h"
#include "economy_func.h"
#include "window_gui.h"
#include "station_gui.h"
#include "terraform_gui.h"
#include "sound_func.h"
#include "window_func.h"
#include "strings_func.h"
#include "viewport_func.h"
#include "company_func.h"
#include "tilehighlight_func.h"
#include "company_base.h"
#include "station_type.h"
#include "newgrf_airport.h"
#include "newgrf_badge_gui.h"
#include "newgrf_callbacks.h"
#include "dropdown_type.h"
#include "dropdown_func.h"
#include "core/geometry_func.hpp"
#include "hotkeys.h"
#include "vehicle_func.h"
#include "gui.h"
#include "command_func.h"
#include "airport_cmd.h"
#include "station_cmd.h"
#include "zoom_func.h"
#include "timer/timer.h"
#include "timer/timer_game_calendar.h"

#include "widgets/airport_widget.h"
#include "air_map.h"
#include "rail.h"

#include "table/strings.h"

#include "three_d.h"

#include "safeguards.h"


static AirportClassID _selected_airport_class; ///< the currently visible airport class
static int _selected_airport_index;            ///< the index of the selected airport in the current class or -1
static uint8_t _selected_airport_layout;          ///< selected airport layout number.

static void ShowBuildAirportPicker(Window *parent);

SpriteID GetCustomAirportSprite(const AirportSpec *as, uint8_t layout);

void CcBuildAirport(Commands, const CommandCost &result, TileIndex tile)
{
	if (result.Failed()) return;

	if (_settings_client.sound.confirm) SndPlayTileFx(SND_1F_CONSTRUCTION_OTHER, tile);
	if (!_settings_client.gui.persistent_buildingtools) ResetObjectToPlace();
}

/**
 * Place an airport.
 * @param tile Position to put the new airport.
 */
static void PlaceAirport(TileIndex tile)
{
	if (_selected_airport_index == -1) return;

	uint8_t airport_type = AirportClass::Get(_selected_airport_class)->GetSpec(_selected_airport_index)->GetIndex();
	uint8_t layout = _selected_airport_layout;
	bool adjacent = _ctrl_pressed;

	auto proc = [=](bool test, StationID to_join) -> bool {
		if (test) {
			return Command<Commands::BuildAirport>::Do(CommandFlagsToDCFlags(GetCommandFlags<Commands::BuildAirport>()), tile, airport_type, layout, StationID::Invalid(), adjacent).Succeeded();
		} else {
			return Command<Commands::BuildAirport>::Post(STR_ERROR_CAN_T_BUILD_AIRPORT_HERE, CcBuildAirport, tile, airport_type, layout, to_join, adjacent);
		}
	};

	ShowSelectStationIfNeeded(TileArea(tile, _thd.size.x / TILE_SIZE, _thd.size.y / TILE_SIZE), proc);
}

/** The look of the next building of a modular airport (air_map.h): each one built takes the next. */
static uint8_t _modular_airport_building = 0;

/**
 * Build a piece of a modular airport on a tile (air_map.h): a hangar faces the taxiway next to it, clicked again it
 * turns; a building takes the next look, clicked again it changes it.
 * @param tile the tile
 * @param kind what to build
 */
static void PlaceModularAirportPiece(TileIndex tile, ModularAirportKind kind)
{
	DiagDirection dir = DiagDirection::SE;
	uint8_t building = _modular_airport_building;
	const bool same = IsModularAirportTile(tile) && GetModularAirportKind(tile) == kind;
	if (same && kind == ModularAirportKind::Hangar) {
		dir = ChangeDiagDir(GetModularAirportDirection(tile), DiagDirDiff::Right90);
	} else if (same && kind == ModularAirportKind::Building) {
		dir = GetModularAirportDirection(tile);
		building = (GetModularAirportBuilding(tile) + 1) % MODULAR_AIRPORT_BUILDINGS;
	} else if (kind == ModularAirportKind::Hangar) {
		for (DiagDirection d : EnumRange(DiagDirection::End)) {
			TileIndex next = TileAddByDiagDir(tile, d);
			if (IsValidTile(next) && IsModularAirportTile(next) && GetModularAirportTracks(next).Any(TrackBits{DiagDirToDiagTrack(d)})) {
				dir = d;
				break;
			}
		}
	} else if (kind == ModularAirportKind::Building) {
		_modular_airport_building = (building + 1) % MODULAR_AIRPORT_BUILDINGS;
	}
	bool adjacent = _ctrl_pressed;
	auto proc = [=](bool test, StationID to_join) -> bool {
		if (test) {
			return Command<Commands::BuildAirportPiece>::Do(CommandFlagsToDCFlags(GetCommandFlags<Commands::BuildAirportPiece>()), tile, kind, dir, building, AirportSurface::Asphalt, StationID::Invalid(), adjacent).Succeeded();
		}
		return Command<Commands::BuildAirportPiece>::Post(STR_ERROR_CAN_T_BUILD_AIRPORT_HERE, CcBuildAirport, tile, kind, dir, building, AirportSurface::Asphalt, to_join, adjacent);
	};
	ShowSelectStationIfNeeded(TileArea(tile, 1, 1), proc);
}

/**
 * Lay a dragged taxiway of a modular airport (air_map.h), as a railway's track is dragged.
 * @param start the tile the drag started on
 * @param end the tile it ended on
 * @param track the piece of track it starts with
 */
static void PlaceModularAirportTaxiway(TileIndex start, TileIndex end, Track track)
{
	bool adjacent = _ctrl_pressed;
	auto proc = [=](bool test, StationID to_join) -> bool {
		if (test) {
			return Command<Commands::BuildAirportTaxiway>::Do(CommandFlagsToDCFlags(GetCommandFlags<Commands::BuildAirportTaxiway>()), start, end, track, AirportSurface::Asphalt, StationID::Invalid(), adjacent).Succeeded();
		}
		return Command<Commands::BuildAirportTaxiway>::Post(STR_ERROR_CAN_T_BUILD_AIRPORT_HERE, CcBuildAirport, start, end, track, AirportSurface::Asphalt, to_join, adjacent);
	};
	ShowSelectStationIfNeeded(TileArea(start, end), proc);
}

/**
 * Build a dragged runway of a modular airport (air_map.h).
 * @param start one end
 * @param end the other end
 */
static void PlaceModularAirportRunway(TileIndex start, TileIndex end)
{
	bool adjacent = _ctrl_pressed;
	auto proc = [=](bool test, StationID to_join) -> bool {
		if (test) {
			return Command<Commands::BuildAirportRunway>::Do(CommandFlagsToDCFlags(GetCommandFlags<Commands::BuildAirportRunway>()), start, end, AirportSurface::Asphalt, StationID::Invalid(), adjacent).Succeeded();
		}
		return Command<Commands::BuildAirportRunway>::Post(STR_ERROR_CAN_T_BUILD_AIRPORT_HERE, CcBuildAirport, start, end, AirportSurface::Asphalt, to_join, adjacent);
	};
	ShowSelectStationIfNeeded(TileArea(start, end), proc);
}

/** Airport build toolbar window handler. */
struct BuildAirToolbarWindow : Window {
	WidgetID last_user_action = INVALID_WIDGET; ///< Last started user action.

	BuildAirToolbarWindow(WindowDesc &desc, WindowNumber window_number) : Window(desc)
	{
		this->InitNested(window_number);
		this->OnInvalidateData();
		if (_settings_client.gui.link_terraform_toolbar) ShowTerraformToolbar(this);
	}

	void Close([[maybe_unused]] int data = 0) override
	{
		if (this->IsWidgetLowered(WID_AT_AIRPORT)) SetViewportCatchmentStation(nullptr, true);
		if (_settings_client.gui.link_terraform_toolbar) CloseWindowById(WindowClass::ScenarioGenerateLandscape, 0, false);
		this->Window::Close();
	}

	/**
	 * Some data on this window has become invalid.
	 * @param data Information about the changed data.
	 * @param gui_scope Whether the call is done from GUI scope. You may not do everything when not in GUI scope. See #InvalidateWindowData() for details.
	 */
	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		if (!gui_scope) return;

		bool can_build = CanBuildVehicleInfrastructure(VehicleType::Aircraft);
		this->SetWidgetDisabledState(WID_AT_AIRPORT, !can_build);
		if (!can_build) {
			CloseWindowById(WindowClass::BuildStation, TransportType::Air);

			/* Show in the tooltip why this button is disabled. */
			this->GetWidget<NWidgetCore>(WID_AT_AIRPORT)->SetToolTip(STR_TOOLBAR_DISABLED_NO_VEHICLE_AVAILABLE);
		} else {
			this->GetWidget<NWidgetCore>(WID_AT_AIRPORT)->SetToolTip(STR_TOOLBAR_AIRCRAFT_BUILD_AIRPORT_TOOLTIP);
		}
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_AT_AIRPORT:
				if (HandlePlacePushButton(this, WID_AT_AIRPORT, SPR_CURSOR_AIRPORT, HT_RECT)) {
					ShowBuildAirportPicker(this);
					this->last_user_action = widget;
				}
				break;

			case WID_AT_DEMOLISH:
				HandlePlacePushButton(this, WID_AT_DEMOLISH, ANIMCURSOR_DEMOLISH, HT_RECT | HT_DIAGONAL);
				this->last_user_action = widget;
				break;

			/* The modular airport (air_map.h). */
			case WID_AT_TAXIWAY:
				HandlePlacePushButton(this, widget, GetRailTypeInfo(RAILTYPE_RAIL)->cursor.autorail, HT_RAIL);
				this->last_user_action = widget;
				break;

			case WID_AT_RUNWAY:
			case WID_AT_STAND:
			case WID_AT_HELIPAD:
			case WID_AT_HELIPORT:
			case WID_AT_HANGAR:
			case WID_AT_BUILDING:
				HandlePlacePushButton(this, widget, SPR_CURSOR_AIRPORT, HT_RECT);
				this->last_user_action = widget;
				break;

			case WID_AT_REMOVE_PIECES:
				HandlePlacePushButton(this, widget, ANIMCURSOR_DEMOLISH, HT_RECT);
				this->last_user_action = widget;
				break;

			default: break;
		}
	}


	void OnPlaceObject([[maybe_unused]] Point pt, TileIndex tile) override
	{
		switch (this->last_user_action) {
			case WID_AT_AIRPORT:
				PlaceAirport(tile);
				break;

			case WID_AT_DEMOLISH:
				PlaceProc_DemolishArea(tile);
				break;

			case WID_AT_TAXIWAY: VpStartPlaceSizing(tile, VPM_RAILDIRS, DDSP_PLACE_RAIL); break;
			case WID_AT_RUNWAY: VpStartPlaceSizing(tile, VPM_X_OR_Y, DDSP_BUILD_STATION); break;
			case WID_AT_REMOVE_PIECES: VpStartPlaceSizing(tile, VPM_X_AND_Y, DDSP_REMOVE_STATION); break;
			case WID_AT_STAND: PlaceModularAirportPiece(tile, ModularAirportKind::Stand); break;
			case WID_AT_HELIPAD: PlaceModularAirportPiece(tile, ModularAirportKind::Helipad); break;
			case WID_AT_HELIPORT: PlaceModularAirportPiece(tile, ModularAirportKind::Heliport); break;
			case WID_AT_HANGAR: PlaceModularAirportPiece(tile, ModularAirportKind::Hangar); break;
			case WID_AT_BUILDING: PlaceModularAirportPiece(tile, ModularAirportKind::Building); break;

			default: NOT_REACHED();
		}
	}

	void OnPlaceDrag(ViewportPlaceMethod select_method, [[maybe_unused]] ViewportDragDropSelectionProcess select_proc, [[maybe_unused]] Point pt) override
	{
		VpSelectTilesWithMethod(pt.x, pt.y, select_method);
	}

	Point OnInitialPosition(int16_t sm_width, [[maybe_unused]] int16_t sm_height, [[maybe_unused]] int window_number) override
	{
		return AlignInitialConstructionToolbar(sm_width);
	}

	void OnPlaceMouseUp([[maybe_unused]] ViewportPlaceMethod select_method, ViewportDragDropSelectionProcess select_proc, [[maybe_unused]] Point pt, TileIndex start_tile, TileIndex end_tile) override
	{
		if (pt.x == -1) return;
		switch (select_proc) {
			case DDSP_DEMOLISH_AREA:
				GUIPlaceProcDragXY(select_proc, start_tile, end_tile);
				break;

			case DDSP_PLACE_RAIL: {
				/* A taxiway, as the railway's drag (rail_gui.cpp HandleAutodirPlacement()). */
				Track track = static_cast<Track>(_thd.drawstyle & HT_DIR_MASK);
				if (_thd.drawstyle & HT_RAIL) {
					PlaceModularAirportTaxiway(start_tile, start_tile, track);
				} else {
					PlaceModularAirportTaxiway(TileVirtXY(_thd.selstart.x, _thd.selstart.y), TileVirtXY(_thd.selend.x, _thd.selend.y), track);
				}
				break;
			}

			case DDSP_BUILD_STATION:
				PlaceModularAirportRunway(start_tile, end_tile);
				break;

			case DDSP_REMOVE_STATION:
				Command<Commands::RemoveAirportPiece>::Post(STR_ERROR_CAN_T_REMOVE_PART_OF_STATION, CcBuildAirport, start_tile, end_tile);
				break;

			default: break;
		}
	}

	void OnPlaceObjectAbort() override
	{
		if (this->IsWidgetLowered(WID_AT_AIRPORT)) SetViewportCatchmentStation(nullptr, true);

		this->RaiseButtons();

		CloseWindowById(WindowClass::BuildStation, TransportType::Air);
		CloseWindowById(WindowClass::JoinStation, 0);
	}

	/**
	 * Handler for global hotkeys of the BuildAirToolbarWindow.
	 * @param hotkey Hotkey
	 * @return EventState::Handled if hotkey was accepted.
	 */
	static EventState AirportToolbarGlobalHotkeys(int hotkey)
	{
		if (_game_mode != GameMode::Normal) return EventState::NotHandled;
		Window *w = ShowBuildAirToolbar();
		if (w == nullptr) return EventState::NotHandled;
		return w->OnHotkey(hotkey);
	}

	static inline HotkeyList hotkeys{"airtoolbar", {
		Hotkey('1', "airport", WID_AT_AIRPORT),
		Hotkey('2', "demolish", WID_AT_DEMOLISH),
	}, AirportToolbarGlobalHotkeys};
};

static constexpr std::initializer_list<NWidgetPart> _nested_air_toolbar_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::DarkGreen),
		NWidget(WWT_CAPTION, Colours::DarkGreen), SetStringTip(STR_TOOLBAR_AIRCRAFT_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_STICKYBOX, Colours::DarkGreen),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_AT_AIRPORT), SetFill(0, 1), SetToolbarMinimalSize(2), SetSpriteTip(SPR_IMG_AIRPORT, STR_TOOLBAR_AIRCRAFT_BUILD_AIRPORT_TOOLTIP),
		/* The modular airport (air_map.h): its pieces one by one. */
		NWidget(WWT_TEXTBTN, Colours::DarkGreen, WID_AT_TAXIWAY), SetFill(0, 1), SetStringTip(STR_TOOLBAR_AIRCRAFT_TAXIWAY, STR_TOOLBAR_AIRCRAFT_TAXIWAY_TOOLTIP),
		NWidget(WWT_TEXTBTN, Colours::DarkGreen, WID_AT_RUNWAY), SetFill(0, 1), SetStringTip(STR_TOOLBAR_AIRCRAFT_RUNWAY, STR_TOOLBAR_AIRCRAFT_RUNWAY_TOOLTIP),
		NWidget(WWT_TEXTBTN, Colours::DarkGreen, WID_AT_STAND), SetFill(0, 1), SetStringTip(STR_TOOLBAR_AIRCRAFT_STAND, STR_TOOLBAR_AIRCRAFT_STAND_TOOLTIP),
		NWidget(WWT_TEXTBTN, Colours::DarkGreen, WID_AT_HELIPAD), SetFill(0, 1), SetStringTip(STR_TOOLBAR_AIRCRAFT_HELIPAD, STR_TOOLBAR_AIRCRAFT_HELIPAD_TOOLTIP),
		NWidget(WWT_TEXTBTN, Colours::DarkGreen, WID_AT_HELIPORT), SetFill(0, 1), SetStringTip(STR_TOOLBAR_AIRCRAFT_HELIPORT, STR_TOOLBAR_AIRCRAFT_HELIPORT_TOOLTIP),
		NWidget(WWT_TEXTBTN, Colours::DarkGreen, WID_AT_HANGAR), SetFill(0, 1), SetStringTip(STR_TOOLBAR_AIRCRAFT_HANGAR, STR_TOOLBAR_AIRCRAFT_HANGAR_TOOLTIP),
		NWidget(WWT_TEXTBTN, Colours::DarkGreen, WID_AT_BUILDING), SetFill(0, 1), SetStringTip(STR_TOOLBAR_AIRCRAFT_BUILDING, STR_TOOLBAR_AIRCRAFT_BUILDING_TOOLTIP),
		NWidget(WWT_TEXTBTN, Colours::DarkGreen, WID_AT_REMOVE_PIECES), SetFill(0, 1), SetStringTip(STR_TOOLBAR_AIRCRAFT_REMOVE_PIECES, STR_TOOLBAR_AIRCRAFT_REMOVE_PIECES_TOOLTIP),
		NWidget(WWT_PANEL, Colours::DarkGreen), SetToolbarSpacerMinimalSize(), SetFill(1, 1), EndContainer(),
		NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_AT_DEMOLISH), SetFill(0, 1), SetToolbarMinimalSize(1), SetSpriteTip(SPR_IMG_DYNAMITE, STR_TOOLTIP_DEMOLISH_BUILDINGS_ETC),
	EndContainer(),
};

/** Window definition for the air toolbar. */
static WindowDesc _air_toolbar_desc(
	WindowPosition::Manual, "toolbar_air", 0, 0,
	WindowClass::BuildToolbar, WindowClass::None,
	WindowDefaultFlag::Construction,
	_nested_air_toolbar_widgets,
	&BuildAirToolbarWindow::hotkeys
);

/**
 * Open the build airport toolbar window
 *
 * If the terraform toolbar is linked to the toolbar, that window is also opened.
 *
 * @return newly opened airport toolbar, or nullptr if the toolbar could not be opened.
 */
Window *ShowBuildAirToolbar()
{
	if (!Company::IsValidID(_local_company)) return nullptr;
	ThreeDViewForBuilding(); // building is done facing north (three_d.h)

	CloseWindowByClass(WindowClass::BuildToolbar);
	return AllocateWindowDescFront<BuildAirToolbarWindow>(_air_toolbar_desc, TransportType::Air);
}

class BuildAirportWindow : public PickerWindowBase {
	SpriteID preview_sprite{}; ///< Cached airport preview sprite.
	int line_height = 0;
	Scrollbar *vscroll = nullptr;

	/**
	 * Build a dropdown list of available airport classes.
	 * @return The constructed dropdown list.
	 */
	static DropDownList BuildAirportClassDropDown()
	{
		DropDownList list;

		for (const auto &cls : AirportClass::Classes()) {
			list.push_back(MakeDropDownListStringItem(cls.name, cls.Index().base()));
		}

		return list;
	}

public:
	BuildAirportWindow(WindowDesc &desc, Window *parent) : PickerWindowBase(desc, parent)
	{
		this->CreateNestedTree();

		this->vscroll = this->GetScrollbar(WID_AP_SCROLLBAR);
		this->vscroll->SetCapacity(5);
		this->vscroll->SetPosition(0);

		this->FinishInitNested(TransportType::Air);

		this->SetWidgetLoweredState(WID_AP_BTN_DONTHILIGHT, !_settings_client.gui.station_show_coverage);
		this->SetWidgetLoweredState(WID_AP_BTN_DOHILIGHT, _settings_client.gui.station_show_coverage);
		this->OnInvalidateData();

		/* Ensure airport class is valid (changing NewGRFs). */
		_selected_airport_class = Clamp(_selected_airport_class, AirportClassID::Begin(), static_cast<AirportClassID>(AirportClass::GetClassCount() - 1));
		const AirportClass *ac = AirportClass::Get(_selected_airport_class);
		this->vscroll->SetCount(ac->GetSpecCount());

		/* Ensure the airport index is valid for this class (changing NewGRFs). */
		_selected_airport_index = Clamp(_selected_airport_index, -1, ac->GetSpecCount() - 1);

		/* Only when no valid airport was selected, we want to select the first airport. */
		bool select_first_airport = true;
		if (_selected_airport_index != -1) {
			const AirportSpec *as = ac->GetSpec(_selected_airport_index);
			if (as->IsAvailable()) {
				/* Ensure the airport layout is valid. */
				_selected_airport_layout = Clamp(_selected_airport_layout, 0, static_cast<uint8_t>(as->layouts.size() - 1));
				select_first_airport = false;
				this->UpdateSelectSize();
			}
		}

		if (select_first_airport) this->SelectFirstAvailableAirport(true);
	}

	void Close([[maybe_unused]] int data = 0) override
	{
		CloseWindowById(WindowClass::JoinStation, 0);
		this->PickerWindowBase::Close();
	}

	std::string GetWidgetString(WidgetID widget, StringID stringid) const override
	{
		switch (widget) {
			case WID_AP_CLASS_DROPDOWN:
				return GetString(AirportClass::Get(_selected_airport_class)->name);

			case WID_AP_LAYOUT_NUM:
				if (_selected_airport_index != -1) {
					const AirportSpec *as = AirportClass::Get(_selected_airport_class)->GetSpec(_selected_airport_index);
					StringID string = GetAirportTextCallback(as, _selected_airport_layout, CBID_AIRPORT_LAYOUT_NAME);
					if (string != STR_UNDEFINED) {
						return GetString(string);
					} else if (as->layouts.size() > 1) {
						return GetString(STR_STATION_BUILD_AIRPORT_LAYOUT_NAME, _selected_airport_layout + 1);
					}
				}
				return {};

			default:
				return this->Window::GetWidgetString(widget, stringid);
		}
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		switch (widget) {
			case WID_AP_CLASS_DROPDOWN: {
				Dimension d = {0, 0};
				for (const auto &cls : AirportClass::Classes()) {
					d = maxdim(d, GetStringBoundingBox(cls.name));
				}
				d.width += padding.width;
				d.height += padding.height;
				size = maxdim(size, d);
				break;
			}

			case WID_AP_AIRPORT_LIST: {
				for (int i = 0; i < NUM_AIRPORTS; i++) {
					const AirportSpec *as = AirportSpec::Get(i);
					if (!as->enabled) continue;

					size.width = std::max(size.width, GetStringBoundingBox(as->name).width + padding.width);
				}

				this->line_height = GetCharacterHeight(FontSize::Normal) + padding.height;
				size.height = 5 * this->line_height;
				break;
			}

			case WID_AP_AIRPORT_SPRITE:
				for (int i = 0; i < NUM_AIRPORTS; i++) {
					const AirportSpec *as = AirportSpec::Get(i);
					if (!as->enabled) continue;
					for (uint8_t layout = 0; layout < static_cast<uint8_t>(as->layouts.size()); layout++) {
						SpriteID sprite = GetCustomAirportSprite(as, layout);
						if (sprite != 0) {
							Dimension d = GetSpriteSize(sprite);
							d.width += WidgetDimensions::scaled.framerect.Horizontal();
							d.height += WidgetDimensions::scaled.framerect.Vertical();
							size = maxdim(d, size);
						}
					}
				}
				break;

			case WID_AP_EXTRA_TEXT:
				for (int i = NEW_AIRPORT_OFFSET; i < NUM_AIRPORTS; i++) {
					const AirportSpec *as = AirportSpec::Get(i);
					if (!as->enabled) continue;
					for (uint8_t layout = 0; layout < static_cast<uint8_t>(as->layouts.size()); layout++) {
						StringID string = GetAirportTextCallback(as, layout, CBID_AIRPORT_ADDITIONAL_TEXT);
						if (string == STR_UNDEFINED) continue;

						Dimension d = GetStringMultiLineBoundingBox(string, size);
						size = maxdim(d, size);
					}
				}
				break;

			default: break;
		}
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		switch (widget) {
			case WID_AP_AIRPORT_LIST: {
				Rect row = r.WithHeight(this->line_height).Shrink(WidgetDimensions::scaled.bevel);
				Rect text = r.WithHeight(this->line_height).Shrink(WidgetDimensions::scaled.matrix);
				const auto specs = AirportClass::Get(_selected_airport_class)->Specs();
				auto [first, last] = this->vscroll->GetVisibleRangeIterators(specs);
				for (auto it = first; it != last; ++it) {
					const AirportSpec *as = *it;
					if (!as->IsAvailable()) {
						GfxFillRect(row, PC_BLACK, FillRectMode::Checker);
					}
					DrawString(text, as->name, (static_cast<int>(as->index) == _selected_airport_index) ? TextColour::White : TextColour::Black);
					row = row.Translate(0, this->line_height);
					text = text.Translate(0, this->line_height);
				}
				break;
			}

			case WID_AP_AIRPORT_SPRITE:
				if (this->preview_sprite != 0) {
					Dimension d = GetSpriteSize(this->preview_sprite);
					DrawSprite(this->preview_sprite, GetCompanyPalette(_local_company), CentreBounds(r.left, r.right, d.width), CentreBounds(r.top, r.bottom, d.height));
				}
				break;

			case WID_AP_EXTRA_TEXT:
				if (_selected_airport_index != -1) {
					const AirportSpec *as = AirportClass::Get(_selected_airport_class)->GetSpec(_selected_airport_index);
					StringID string = GetAirportTextCallback(as, _selected_airport_layout, CBID_AIRPORT_ADDITIONAL_TEXT);
					if (string != STR_UNDEFINED) {
						DrawStringMultiLine(r, string, TextColour::Black);
					}
				}
				break;
		}
	}

	void OnPaint() override
	{
		this->DrawWidgets();

		Rect r = this->GetWidget<NWidgetBase>(WID_AP_ACCEPTANCE)->GetCurrentRect();
		const int bottom = r.bottom;
		r.bottom = INT_MAX; // Allow overflow as we want to know the required height.

		if (_selected_airport_index != -1) {
			const AirportSpec *as = AirportClass::Get(_selected_airport_class)->GetSpec(_selected_airport_index);
			int rad = _settings_game.station.modified_catchment ? as->catchment : (uint)CA_UNMODIFIED;

			/* only show the station (airport) noise, if the noise option is activated */
			if (_settings_game.economy.station_noise_level) {
				/* show the noise of the selected airport */
				DrawString(r, GetString(STR_STATION_BUILD_NOISE, as->noise_level));
				r.top += GetCharacterHeight(FontSize::Normal) + WidgetDimensions::scaled.vsep_normal;
			}

			if (_settings_game.economy.infrastructure_maintenance) {
				Money monthly = _price[Price::InfrastructureAirport] * as->maintenance_cost >> 3;
				DrawString(r, GetString(TimerGameEconomy::UsingWallclockUnits() ? STR_STATION_BUILD_INFRASTRUCTURE_COST_PERIOD : STR_STATION_BUILD_INFRASTRUCTURE_COST_YEAR, monthly * 12));
				r.top += GetCharacterHeight(FontSize::Normal) + WidgetDimensions::scaled.vsep_normal;
			}

			/* strings such as 'Size' and 'Coverage Area' */
			r.top = DrawBadgeNameList(r, as->badges, GrfSpecFeature::Airports) + WidgetDimensions::scaled.vsep_normal;
			r.top = DrawStationCoverageAreaText(r, SCT_ALL, rad, false) + WidgetDimensions::scaled.vsep_normal;
			r.top = DrawStationCoverageAreaText(r, SCT_ALL, rad, true);
		}

		/* Resize background if the window is too small.
		 * Never make the window smaller to avoid oscillating if the size change affects the acceptance.
		 * (This is the case, if making the window bigger moves the mouse into the window.) */
		if (r.top > bottom) {
			ResizeWindow(this, 0, r.top - bottom, false);
		}
	}

	void SelectOtherAirport(int airport_index)
	{
		_selected_airport_index = airport_index;
		_selected_airport_layout = 0;

		this->UpdateSelectSize();
		this->SetDirty();
	}

	void UpdateSelectSize()
	{
		if (_selected_airport_index == -1) {
			SetTileSelectSize(1, 1);
			this->DisableWidget(WID_AP_LAYOUT_DECREASE);
			this->DisableWidget(WID_AP_LAYOUT_INCREASE);
		} else {
			const AirportSpec *as = AirportClass::Get(_selected_airport_class)->GetSpec(_selected_airport_index);
			int w = as->size_x;
			int h = as->size_y;
			Direction rotation = as->layouts[_selected_airport_layout].rotation;
			if (rotation == Direction::E || rotation == Direction::W) std::swap(w, h);
			SetTileSelectSize(w, h);

			this->preview_sprite = GetCustomAirportSprite(as, _selected_airport_layout);

			this->SetWidgetDisabledState(WID_AP_LAYOUT_DECREASE, _selected_airport_layout == 0);
			this->SetWidgetDisabledState(WID_AP_LAYOUT_INCREASE, _selected_airport_layout + 1U >= as->layouts.size());

			int rad = _settings_game.station.modified_catchment ? as->catchment : (uint)CA_UNMODIFIED;
			if (_settings_client.gui.station_show_coverage) SetTileSelectBigSize(-rad, -rad, 2 * rad, 2 * rad);
		}
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_AP_CLASS_DROPDOWN:
				ShowDropDownList(this, BuildAirportClassDropDown(), _selected_airport_class.base(), WID_AP_CLASS_DROPDOWN);
				break;

			case WID_AP_AIRPORT_LIST: {
				int32_t num_clicked = this->vscroll->GetScrolledRowFromWidget(pt.y, this, widget, 0, this->line_height);
				if (num_clicked == INT32_MAX) break;
				const AirportSpec *as = AirportClass::Get(_selected_airport_class)->GetSpec(num_clicked);
				if (as->IsAvailable()) this->SelectOtherAirport(num_clicked);
				break;
			}

			case WID_AP_BTN_DONTHILIGHT: case WID_AP_BTN_DOHILIGHT:
				_settings_client.gui.station_show_coverage = (widget != WID_AP_BTN_DONTHILIGHT);
				this->SetWidgetLoweredState(WID_AP_BTN_DONTHILIGHT, !_settings_client.gui.station_show_coverage);
				this->SetWidgetLoweredState(WID_AP_BTN_DOHILIGHT, _settings_client.gui.station_show_coverage);
				this->SetDirty();
				SndClickBeep();
				this->UpdateSelectSize();
				SetViewportCatchmentStation(nullptr, true);
				break;

			case WID_AP_LAYOUT_DECREASE:
				_selected_airport_layout--;
				this->UpdateSelectSize();
				this->SetDirty();
				break;

			case WID_AP_LAYOUT_INCREASE:
				_selected_airport_layout++;
				this->UpdateSelectSize();
				this->SetDirty();
				break;
		}
	}

	/**
	 * Select the first available airport.
	 * @param change_class If true, change the class if no airport in the current
	 *   class is available.
	 */
	void SelectFirstAvailableAirport(bool change_class)
	{
		/* First try to select an airport in the selected class. */
		AirportClass *sel_apclass = AirportClass::Get(_selected_airport_class);
		for (const AirportSpec *as : sel_apclass->Specs()) {
			if (as->IsAvailable()) {
				this->SelectOtherAirport(as->index);
				return;
			}
		}
		if (change_class) {
			/* If that fails, select the first available airport
			 * from the first class where airports are available. */
			for (const auto &cls : AirportClass::Classes()) {
				for (const auto &as : cls.Specs()) {
					if (as->IsAvailable()) {
						_selected_airport_class = cls.Index();
						this->vscroll->SetCount(cls.GetSpecCount());
						this->SelectOtherAirport(as->index);
						return;
					}
				}
			}
		}
		/* If all airports are unavailable, select nothing. */
		this->SelectOtherAirport(-1);
	}

	void OnDropdownSelect(WidgetID widget, int index, int) override
	{
		if (widget == WID_AP_CLASS_DROPDOWN) {
			_selected_airport_class = (AirportClassID)index;
			this->vscroll->SetCount(AirportClass::Get(_selected_airport_class)->GetSpecCount());
			this->SelectFirstAvailableAirport(false);
		}
	}

	void OnRealtimeTick([[maybe_unused]] uint delta_ms) override
	{
		CheckRedrawStationCoverage(this);
	}

	const IntervalTimer<TimerGameCalendar> yearly_interval = {{TimerGameCalendar::Trigger::Year, TimerGameCalendar::Priority::None}, [this](auto) {
		this->InvalidateData();
	}};
};

static constexpr std::initializer_list<NWidgetPart> _nested_build_airport_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::DarkGreen),
		NWidget(WWT_CAPTION, Colours::DarkGreen), SetStringTip(STR_STATION_BUILD_AIRPORT_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
	EndContainer(),
	NWidget(WWT_PANEL, Colours::DarkGreen),
		NWidget(NWID_VERTICAL), SetPIP(0, WidgetDimensions::unscaled.vsep_normal, 0), SetPadding(WidgetDimensions::unscaled.picker),
			NWidget(NWID_VERTICAL), SetPIP(0, WidgetDimensions::unscaled.vsep_picker, 0),
				NWidget(WWT_LABEL, Colours::Invalid), SetStringTip(STR_STATION_BUILD_AIRPORT_CLASS_LABEL), SetFill(1, 0),
				NWidget(WWT_DROPDOWN, Colours::Grey, WID_AP_CLASS_DROPDOWN), SetFill(1, 0), SetToolTip(STR_STATION_BUILD_AIRPORT_TOOLTIP),
				NWidget(WWT_EMPTY, Colours::Invalid, WID_AP_AIRPORT_SPRITE), SetFill(1, 0),
				NWidget(NWID_HORIZONTAL),
					NWidget(WWT_MATRIX, Colours::Grey, WID_AP_AIRPORT_LIST), SetFill(1, 0), SetMatrixDataTip(1, 5, STR_STATION_BUILD_AIRPORT_TOOLTIP), SetScrollbar(WID_AP_SCROLLBAR),
					NWidget(NWID_VSCROLLBAR, Colours::Grey, WID_AP_SCROLLBAR),
				EndContainer(),
				NWidget(WWT_LABEL, Colours::Invalid), SetStringTip(STR_STATION_BUILD_ORIENTATION), SetFill(1, 0),
				NWidget(NWID_HORIZONTAL),
					NWidget(WWT_PUSHARROWBTN, Colours::Grey, WID_AP_LAYOUT_DECREASE), SetMinimalSize(12, 0), SetArrowWidgetTypeTip(ArrowWidgetType::Decrease),
					NWidget(WWT_LABEL, Colours::Invalid, WID_AP_LAYOUT_NUM), SetResize(1, 0), SetFill(1, 0),
					NWidget(WWT_PUSHARROWBTN, Colours::Grey, WID_AP_LAYOUT_INCREASE), SetMinimalSize(12, 0), SetArrowWidgetTypeTip(ArrowWidgetType::Increase),
				EndContainer(),
				NWidget(WWT_EMPTY, Colours::Invalid, WID_AP_EXTRA_TEXT), SetFill(1, 0), SetMinimalSize(150, 0),
				NWidget(WWT_LABEL, Colours::Invalid), SetStringTip(STR_STATION_BUILD_COVERAGE_AREA_TITLE), SetFill(1, 0),
				NWidget(NWID_HORIZONTAL), SetPIP(14, 0, 14), SetPIPRatio(1, 0, 1),
					NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
						NWidget(WWT_TEXTBTN, Colours::Grey, WID_AP_BTN_DONTHILIGHT), SetMinimalSize(60, 12), SetFill(1, 0),
													SetStringTip(STR_STATION_BUILD_COVERAGE_OFF, STR_STATION_BUILD_COVERAGE_AREA_OFF_TOOLTIP),
						NWidget(WWT_TEXTBTN, Colours::Grey, WID_AP_BTN_DOHILIGHT), SetMinimalSize(60, 12), SetFill(1, 0),
													SetStringTip(STR_STATION_BUILD_COVERAGE_ON, STR_STATION_BUILD_COVERAGE_AREA_ON_TOOLTIP),
					EndContainer(),
				EndContainer(),
			EndContainer(),
			NWidget(WWT_EMPTY, Colours::Invalid, WID_AP_ACCEPTANCE), SetResize(0, 1), SetFill(1, 0), SetMinimalTextLines(2, WidgetDimensions::unscaled.vsep_normal),
		EndContainer(),
	EndContainer(),
};

/** Window definition for the airport build window. */
static WindowDesc _build_airport_desc(
	WindowPosition::Automatic, {}, 0, 0,
	WindowClass::BuildStation, WindowClass::BuildToolbar,
	WindowDefaultFlag::Construction,
	_nested_build_airport_widgets
);

static void ShowBuildAirportPicker(Window *parent)
{
	new BuildAirportWindow(_build_airport_desc, parent);
}

void InitializeAirportGui()
{
	_selected_airport_class = AirportClassID::Begin();
	_selected_airport_index = -1;
}
