/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file intro_gui.cpp The main menu GUI. */

#include "stdafx.h"
#include "error.h"
#include "gui.h"
#include "window_gui.h"
#include "window_func.h"
#include "textbuf_gui.h"
#include "help_gui.h"
#include "network/network.h"
#include "genworld.h"
#include "network/network_gui.h"
#include "network/network_content.h"
#include "network/network_survey.h"
#include "landscape_type.h"
#include "landscape.h"
#include "strings_func.h"
#include "fios.h"
#include "ai/ai_gui.hpp"
#include "game/game_gui.hpp"
#include "gfx_func.h"
#include "core/string_consumer.hpp"
#include "language.h"
#include "rev.h"
#include "highscore.h"
#include "signs_base.h"
#include "viewport_func.h"
#include "three_d.h"
#include "core/random_func.hpp"
#include "vehicle_base.h"
#include <cmath>
#include <regex>

#include "widgets/intro_widget.h"

#include "table/strings.h"
#include "table/sprites.h"

#include "safeguards.h"


/**
 * A viewport command for the main menu background (intro game).
 */
struct IntroGameViewportCommand {
	int command_index = 0;               ///< Sequence number of the command (order they are performed in).
	Point position{ 0, 0 };              ///< Calculated world coordinate to position viewport top-left at.
	VehicleID vehicle = VehicleID::Invalid(); ///< Vehicle to follow, or VehicleID::Invalid() if not following a vehicle.
	uint delay = 0;                      ///< Delay until next command.
	int zoom_adjust = 0;                 ///< Adjustment to zoom level from base zoom level.
	bool pan_to_next = false;            ///< If true, do a smooth pan from this position to the next.
	Alignment align = {AlignmentH::Centre, AlignmentV::Middle}; ///< Alignment.

	/**
	 * Calculate effective position.
	 * This will update the position field if a vehicle is followed.
	 * @param vp Viewport to calculate position for.
	 * @return Calculated position in the viewport.
	 */
	Point PositionForViewport(const Viewport &vp)
	{
		if (this->vehicle != VehicleID::Invalid()) {
			const Vehicle *v = Vehicle::Get(this->vehicle);
			this->position = RemapCoords(v->x_pos, v->y_pos, v->z_pos);
		}

		Point p;
		switch (this->align.ResolveRTL()) {
			case AlignmentH::ForceLeft: p.x = this->position.x; break;
			case AlignmentH::Centre: p.x = this->position.x - vp.virtual_width / 2; break;
			case AlignmentH::ForceRight: p.x = this->position.x - vp.virtual_width; break;
			default: NOT_REACHED();
		}
		switch (this->align.v) {
			case AlignmentV::Top: p.y = this->position.y; break;
			case AlignmentV::Middle: p.y = this->position.y - vp.virtual_height / 2; break;
			case AlignmentV::Bottom: p.y = this->position.y - vp.virtual_height; break;
		}
		return p;
	}
};


/** The title screen's day and night (the player, 16. 10.: "a long day and then a while of night, and round and round
 * between the day and the night"), milliseconds: the day, the dusk, the night and the dawn. */
static constexpr uint TITLE_DAY_MS = 60000, TITLE_DUSK_MS = 4000, TITLE_NIGHT_MS = 15000, TITLE_DAWN_MS = 4000;
/** The title screen's turns of the map (the player, 17. 10.: "the camera moves, but at right angles; better turn the
 * game's ground a little now and then, a while nothing and again"), milliseconds: still, and a turn. */
static constexpr uint TITLE_STILL_MS = 9000, TITLE_TURN_MS = 3500;

struct SelectGameWindow : public Window {
	/** Where in the title screen's day and night it is, milliseconds (TITLE_DAY_MS ...). */
	uint day_night_time = 0;
	/** The turn of the map: how long it has been still or turning, milliseconds, and the turn going on. */
	uint turn_time = 0;
	bool turning = false;
	float turn_from = 0;
	float turn_by = 0;
	/** Vector of viewport commands parsed. */
	std::vector<IntroGameViewportCommand> intro_viewport_commands{};

	/**
	 * Find and parse all viewport command signs.
	 * Fills the intro_viewport_commands vector and deletes parsed signs from the world.
	 */
	void ReadIntroGameViewportCommand()
	{
		intro_viewport_commands.clear();

		/* Regular expression matching the commands: T, spaces, integer, spaces, flags, spaces, integer */
		static const std::string sign_language = "^T\\s*([0-9]+)\\s*([-+A-Z0-9]+)\\s*([0-9]+)";
		std::regex re(sign_language, std::regex_constants::icase);

		/* List of signs successfully parsed to delete afterwards. */
		std::vector<SignID> signs_to_delete;

		for (const Sign *sign : Sign::Iterate()) {
			std::smatch match;
			if (!std::regex_search(sign->name, match, re)) continue;

			IntroGameViewportCommand vc;
			/* Sequence index from the first matching group. */
			if (auto value = ParseInteger<int>(match[1].str()); value.has_value()) {
				vc.command_index = *value;
			} else {
				continue;
			}
			/* Sign coordinates for positioning. */
			vc.position = RemapCoords(sign->x, sign->y, sign->z);
			/* Delay from the third matching group. */
			if (auto value = ParseInteger<uint>(match[3].str()); value.has_value()) {
				vc.delay = *value * 1000; // milliseconds
			} else {
				continue;
			}

			/* Parse flags from second matching group. */
			auto flags = match[2].str();
			StringConsumer consumer{flags};
			while (consumer.AnyBytesLeft()) {
				auto c = consumer.ReadUtf8();
				switch (toupper(c)) {
					case '-': vc.zoom_adjust = +1; break;
					case '+': vc.zoom_adjust = -1; break;
					case 'T': vc.align.v = AlignmentV::Top; break;
					case 'M': vc.align.v = AlignmentV::Middle; break;
					case 'B': vc.align.v = AlignmentV::Bottom; break;
					case 'L': vc.align.h = AlignmentH::ForceLeft; break;
					case 'C': vc.align.h = AlignmentH::Centre; break;
					case 'R': vc.align.h = AlignmentH::ForceRight; break;
					case 'P': vc.pan_to_next = true; break;
					case 'V': vc.vehicle = static_cast<VehicleID>(consumer.ReadIntegerBase<uint32_t>(10, VehicleID::Invalid().base())); break;
				}
			}

			/* Successfully parsed, store. */
			intro_viewport_commands.push_back(vc);
			signs_to_delete.push_back(sign->index);
		}

		/* Sort the commands by sequence index. */
		std::sort(intro_viewport_commands.begin(), intro_viewport_commands.end(), [](const IntroGameViewportCommand &a, const IntroGameViewportCommand &b) { return a.command_index < b.command_index; });

		/* Delete all the consumed signs, from last ID to first ID. */
		std::sort(signs_to_delete.begin(), signs_to_delete.end(), [](SignID a, SignID b) { return a > b; });
		for (SignID sign_id : signs_to_delete) {
			delete Sign::Get(sign_id);
		}
	}

	SelectGameWindow(WindowDesc &desc) : Window(desc)
	{
		this->CreateNestedTree();
		this->FinishInitNested(0);
		this->OnInvalidateData();

		this->ReadIntroGameViewportCommand();
		_title_night = 0;
	}

	/**
	 * The title screen's day and night on: the night drawn (IsNightNow()) as far as it is come, in steps, so the
	 * screen is drawn anew only in the dusk and the dawn; the vehicles' boxes made room for their lights.
	 */
	void AdvanceDayAndNight(uint delta_ms)
	{
		static constexpr uint CYCLE = TITLE_DAY_MS + TITLE_DUSK_MS + TITLE_NIGHT_MS + TITLE_DAWN_MS;
		this->day_night_time = (this->day_night_time + delta_ms) % CYCLE;
		const uint t = this->day_night_time;
		float night = 1;
		if (t < TITLE_DAY_MS) {
			night = 0;
		} else if (t < TITLE_DAY_MS + TITLE_DUSK_MS) {
			night = static_cast<float>(t - TITLE_DAY_MS) / TITLE_DUSK_MS;
		} else if (t >= TITLE_DAY_MS + TITLE_DUSK_MS + TITLE_NIGHT_MS) {
			night = 1 - static_cast<float>(t - TITLE_DAY_MS - TITLE_DUSK_MS - TITLE_NIGHT_MS) / TITLE_DAWN_MS;
		}
		night = std::round(night * 32) / 32;
		if (night == _title_night) return;
		const bool was = _title_night > 0;
		_title_night = night;
		if (was != (night > 0)) {
			for (const Vehicle *v : Vehicle::Iterate()) v->UpdateBoundingBoxCoordinates(true);
		}
		MarkWholeScreenDirty();
	}

	/**
	 * The title screen's turns of the map: still a while, then a slow turn of 30 to 60 degrees, easing in and out --
	 * mostly the same way round, now and then back, about the middle of the screen. Only with the 3D on (SetViewYaw()
	 * keeps it north else).
	 */
	void AdvanceTurn(uint delta_ms)
	{
		if (!ThreeDEnabled()) return;
		this->turn_time += delta_ms;
		if (!this->turning) {
			if (this->turn_time < TITLE_STILL_MS) return;
			this->turning = true;
			this->turn_time = 0;
			this->turn_from = GetViewYaw();
			this->turn_by = static_cast<float>(30 + InteractiveRandomRange(31)) * (InteractiveRandomRange(4) == 0 ? -1.0f : 1.0f);
		}
		const float t = std::min(1.0f, static_cast<float>(this->turn_time) / TITLE_TURN_MS);
		SetViewYaw(this->turn_from + this->turn_by * t * t * (3 - 2 * t));
		if (t >= 1.0f) {
			this->turning = false;
			this->turn_time = 0;
		}
	}

	void OnRealtimeTick(uint delta_ms) override
	{
		this->AdvanceDayAndNight(delta_ms);
		this->AdvanceTurn(delta_ms);

		/* The camera stays where the title game puts it first (the player, 17. 10.: "the title screen without the
		 * camera moving, only the map turned now and then"): the first of its places, once; the turns keep the
		 * middle of the screen in place (SetViewYaw()). */
		if (intro_viewport_commands.empty()) return;
		IntroGameViewportCommand &vc = intro_viewport_commands.front();
		Window *mw = GetMainWindow();
		FixTitleGameZoom(vc.zoom_adjust);
		Point pos = vc.PositionForViewport(*mw->viewport);
		mw->viewport->dest_scrollpos_x = mw->viewport->scrollpos_x = pos.x;
		mw->viewport->dest_scrollpos_y = mw->viewport->scrollpos_y = pos.y;
		UpdateViewportPosition(mw, delta_ms);
		mw->SetDirty();
		intro_viewport_commands.clear();
	}

	void OnInit() override
	{
		bool missing_sprites = _missing_extra_graphics > 0 && !IsReleasedVersion();
		this->GetWidget<NWidgetStacked>(WID_SGI_BASESET_SELECTION)->SetDisplayedPlane(missing_sprites ? 0 : SZSP_NONE);

		bool missing_lang = _current_language->missing >= _settings_client.gui.missing_strings_threshold && !IsReleasedVersion();
		this->GetWidget<NWidgetStacked>(WID_SGI_TRANSLATION_SELECTION)->SetDisplayedPlane(missing_lang ? 0 : SZSP_NONE);

		/* Coupling changes what a consist is and how one is taken apart, and
		 * none of that has been looked at for keeping two clients in step yet.
		 * Rather than let a game desync halfway through and take the players'
		 * work with it, close multiplayer off until it has been. */
		this->SetWidgetDisabledState(WID_SGI_PLAY_NETWORK, true);
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		switch (widget) {
			case WID_SGI_BASESET:
				DrawStringMultiLine(r, GetString(STR_INTRO_BASESET, _missing_extra_graphics), TextColour::FromString, {AlignmentH::Centre, AlignmentV::Middle});
				break;

			case WID_SGI_TRANSLATION:
				DrawStringMultiLine(r, GetString(STR_INTRO_TRANSLATION, _current_language->missing), TextColour::FromString, {AlignmentH::Centre, AlignmentV::Middle});
				break;
		}
	}

	void OnResize() override
	{
		bool changed = false;

		if (NWidgetResizeBase *wid = this->GetWidget<NWidgetResizeBase>(WID_SGI_BASESET); wid != nullptr && wid->current_x > 0) {
			changed |= wid->UpdateMultilineWidgetSize(GetString(STR_INTRO_BASESET, _missing_extra_graphics), 3);
		}

		if (NWidgetResizeBase *wid = this->GetWidget<NWidgetResizeBase>(WID_SGI_TRANSLATION); wid != nullptr && wid->current_x > 0) {
			changed |= wid->UpdateMultilineWidgetSize(GetString(STR_INTRO_TRANSLATION, _current_language->missing), 3);
		}

		if (changed) this->ReInit(0, 0, this->flags.Test(WindowFlag::Centred));
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_SGI_GENERATE_GAME:
				_is_network_server = false;
				if (_ctrl_pressed) {
					StartNewGameWithoutGUI(GENERATE_NEW_SEED);
				} else {
					ShowGenerateLandscape();
				}
				break;
			case WID_SGI_LOAD_GAME:
				_is_network_server = false;
				ShowSaveLoadDialog(AbstractFileType::Savegame, SaveLoadOperation::Load);
				break;
			case WID_SGI_PLAY_SCENARIO:
				_is_network_server = false;
				ShowSaveLoadDialog(AbstractFileType::Scenario, SaveLoadOperation::Load);
				break;
			case WID_SGI_PLAY_HEIGHTMAP:
				_is_network_server = false;
				ShowSaveLoadDialog(AbstractFileType::Heightmap,SaveLoadOperation::Load);
				break;
			case WID_SGI_EDIT_SCENARIO:
				_is_network_server = false;
				StartScenarioEditor();
				break;

			case WID_SGI_PLAY_NETWORK:
				if (!_network_available) {
					ShowErrorMessage(GetEncodedString(STR_NETWORK_ERROR_NOTAVAILABLE), {}, WarningLevel::Error);
				} else {
					ShowNetworkGameWindow();
				}
				break;

			case WID_SGI_OPTIONS:         ShowGameOptions(); break;
			case WID_SGI_HIGHSCORE:       ShowHighscoreTable(); break;
			case WID_SGI_HELP:            ShowHelpWindow(); break;
			case WID_SGI_CONTENT_DOWNLOAD:
				if (!_network_available) {
					ShowErrorMessage(GetEncodedString(STR_NETWORK_ERROR_NOTAVAILABLE), {}, WarningLevel::Error);
				} else {
					ShowNetworkContentListWindow();
				}
				break;
			case WID_SGI_EXIT:            HandleExitGameRequest(); break;
		}
	}
};

static constexpr std::initializer_list<NWidgetPart> _nested_select_game_widgets = {
	NWidget(WWT_CAPTION, Colours::Brown), SetStringTip(STR_INTRO_CAPTION),
	NWidget(WWT_PANEL, Colours::Brown),
		NWidget(NWID_VERTICAL), SetPIP(0, WidgetDimensions::unscaled.vsep_wide, 0), SetPadding(WidgetDimensions::unscaled.sparse),

			/* Single player */
			NWidget(NWID_VERTICAL), SetPIP(0, WidgetDimensions::unscaled.vsep_normal, 0),
				NWidget(WWT_PUSHIMGTEXTBTN, Colours::Orange, WID_SGI_GENERATE_GAME), SetToolbarMinimalSize(1), SetSpriteStringTip(SPR_IMG_LANDSCAPING, STR_INTRO_NEW_GAME, STR_INTRO_TOOLTIP_NEW_GAME), SetAlignment({AlignmentH::Start, AlignmentV::Middle}), SetFill(1, 0),
				NWidget(WWT_PUSHIMGTEXTBTN, Colours::Orange, WID_SGI_PLAY_HEIGHTMAP), SetToolbarMinimalSize(1), SetSpriteStringTip(SPR_IMG_SHOW_COUNTOURS, STR_INTRO_PLAY_HEIGHTMAP, STR_INTRO_TOOLTIP_PLAY_HEIGHTMAP), SetAlignment({AlignmentH::Start, AlignmentV::Middle}), SetFill(1, 0),
				NWidget(WWT_PUSHIMGTEXTBTN, Colours::Orange, WID_SGI_PLAY_SCENARIO), SetToolbarMinimalSize(1), SetSpriteStringTip(SPR_IMG_SUBSIDIES, STR_INTRO_PLAY_SCENARIO, STR_INTRO_TOOLTIP_PLAY_SCENARIO), SetAlignment({AlignmentH::Start, AlignmentV::Middle}), SetFill(1, 0),
				NWidget(WWT_PUSHIMGTEXTBTN, Colours::Orange, WID_SGI_LOAD_GAME), SetToolbarMinimalSize(1), SetSpriteStringTip(SPR_IMG_SAVE, STR_INTRO_LOAD_GAME, STR_INTRO_TOOLTIP_LOAD_GAME), SetAlignment({AlignmentH::Start, AlignmentV::Middle}), SetFill(1, 0),
				NWidget(WWT_PUSHIMGTEXTBTN, Colours::Orange, WID_SGI_HIGHSCORE), SetToolbarMinimalSize(1), SetSpriteStringTip(SPR_IMG_COMPANY_LEAGUE, STR_INTRO_HIGHSCORE, STR_INTRO_TOOLTIP_HIGHSCORE), SetAlignment({AlignmentH::Start, AlignmentV::Middle}), SetFill(1, 0),
			EndContainer(),

			/* Multi player */
			NWidget(NWID_VERTICAL), SetPIP(0, WidgetDimensions::unscaled.vsep_normal, 0),
				NWidget(WWT_PUSHIMGTEXTBTN, Colours::Orange, WID_SGI_PLAY_NETWORK), SetToolbarMinimalSize(1), SetSpriteStringTip(SPR_IMG_COMPANY_GENERAL, STR_INTRO_MULTIPLAYER, STR_INTRO_TOOLTIP_MULTIPLAYER), SetAlignment({AlignmentH::Start, AlignmentV::Middle}), SetFill(1, 0),
			EndContainer(),

			NWidget(NWID_SELECTION, Colours::Invalid, WID_SGI_BASESET_SELECTION),
				NWidget(NWID_VERTICAL),
					NWidget(WWT_EMPTY, Colours::Invalid, WID_SGI_BASESET), SetFill(1, 0),
				EndContainer(),
			EndContainer(),

			NWidget(NWID_SELECTION, Colours::Invalid, WID_SGI_TRANSLATION_SELECTION),
				NWidget(NWID_VERTICAL),
					NWidget(WWT_EMPTY, Colours::Invalid, WID_SGI_TRANSLATION), SetFill(1, 0),
				EndContainer(),
			EndContainer(),

			/* Other */
			NWidget(NWID_VERTICAL), SetPIP(0, WidgetDimensions::unscaled.vsep_normal, 0),
				NWidget(WWT_PUSHIMGTEXTBTN, Colours::Orange, WID_SGI_OPTIONS), SetToolbarMinimalSize(1), SetSpriteStringTip(SPR_IMG_SETTINGS, STR_INTRO_GAME_OPTIONS, STR_INTRO_TOOLTIP_GAME_OPTIONS), SetAlignment({AlignmentH::Start, AlignmentV::Middle}), SetFill(1, 0),
				NWidget(WWT_PUSHIMGTEXTBTN, Colours::Orange, WID_SGI_CONTENT_DOWNLOAD), SetToolbarMinimalSize(1), SetSpriteStringTip(SPR_IMG_SHOW_VEHICLES, STR_INTRO_ONLINE_CONTENT, STR_INTRO_TOOLTIP_ONLINE_CONTENT), SetAlignment({AlignmentH::Start, AlignmentV::Middle}), SetFill(1, 0),
				NWidget(WWT_PUSHIMGTEXTBTN, Colours::Orange, WID_SGI_EDIT_SCENARIO), SetToolbarMinimalSize(1), SetSpriteStringTip(SPR_IMG_SMALLMAP, STR_INTRO_SCENARIO_EDITOR, STR_INTRO_TOOLTIP_SCENARIO_EDITOR), SetAlignment({AlignmentH::Start, AlignmentV::Middle}), SetFill(1, 0),
				NWidget(WWT_PUSHIMGTEXTBTN, Colours::Orange, WID_SGI_HELP), SetToolbarMinimalSize(1), SetSpriteStringTip(SPR_IMG_QUERY, STR_INTRO_HELP, STR_INTRO_TOOLTIP_HELP), SetAlignment({AlignmentH::Start, AlignmentV::Middle}), SetFill(1, 0),
			EndContainer(),

			NWidget(NWID_VERTICAL),
				NWidget(WWT_PUSHTXTBTN, Colours::Orange, WID_SGI_EXIT), SetToolbarMinimalSize(1), SetStringTip(STR_INTRO_QUIT, STR_INTRO_TOOLTIP_QUIT),
			EndContainer(),
		EndContainer(),
	EndContainer(),
};

/** Window definition for the select game window. */
/** The title screen that much on, its camera and its day and night, as if that much time went by (the rig: 'test3d titulnoc'). */
void AdvanceTitleDayAndNight(uint delta_ms)
{
	SelectGameWindow *w = static_cast<SelectGameWindow *>(FindWindowByClass(WindowClass::SelectGame));
	if (w != nullptr) w->OnRealtimeTick(delta_ms);
}

static WindowDesc _select_game_desc(
	WindowPosition::Center, {}, 0, 0,
	WindowClass::SelectGame, WindowClass::None,
	WindowDefaultFlag::NoClose,
	_nested_select_game_widgets
);

void ShowSelectGameWindow()
{
	new SelectGameWindow(_select_game_desc);
}

static void AskExitGameCallback(Window *, bool confirmed)
{
	if (confirmed) {
		_survey.Transmit(NetworkSurveyHandler::Reason::Exit, true);
		_exit_game = true;
	}
}

void AskExitGame()
{
	ShowQuery(
		GetEncodedString(STR_QUIT_CAPTION),
		GetEncodedString(STR_QUIT_ARE_YOU_SURE_YOU_WANT_TO_EXIT_OPENTTD),
		nullptr,
		AskExitGameCallback,
		true
	);
}


static void AskExitToGameMenuCallback(Window *, bool confirmed)
{
	if (confirmed) {
		_switch_mode = SwitchMode::Menu;
		ClearErrorMessages();
	}
}

void AskExitToGameMenu()
{
	ShowQuery(
		GetEncodedString(STR_ABANDON_GAME_CAPTION),
		GetEncodedString((_game_mode != GameMode::Editor) ? STR_ABANDON_GAME_QUERY : STR_ABANDON_SCENARIO_QUERY),
		nullptr,
		AskExitToGameMenuCallback,
		true
	);
}
