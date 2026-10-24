/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file train_driver_gui.cpp The driver window: one train's own "brake, fail
 * to brake and crash" -- how far he sees, through how many signals, how much
 * weaker the emergency brake is, how long he remembers a yellow -- and its
 * own braking table. Every setting can be left "as the game says", which is
 * what every train starts with; anything set here stands in for the game's
 * setting for this train alone. Opened by a click on the train's name in its
 * window (VehicleViewWindow::OnCaptionClick()).
 *
 * The window works on a draft: the dropdowns change the draft, "keep the
 * changes" writes it to the train (Commands::SetTrainDriver, one command a
 * changed setting), "everything as the game says" clears the draft. The
 * player's two buttons.
 */

#include "stdafx.h"
#include "command_func.h"
#include "querystring_gui.h"
#include "string_func.h"
#include "dropdown_func.h"
#include "dropdown_type.h"
#include "settings_type.h"
#include "strings_func.h"
#include "train.h"
#include "train_cmd.h"
#include "vehicle_gui.h"
#include "window_func.h"
#include "window_gui.h"
#include "zoom_func.h"

#include "widgets/vehicle_widget.h"

#include "table/strings.h"

#include "safeguards.h"

/** The speeds over the braking table's boxes, top band first; the last is the stand. */
static constexpr int DRIVER_BAND_TOP[12] = {300, 250, 200, 160, 130, 100, 80, 60, 40, 20, 10, 0};

/** A driver's settings as the window holds them: what a train has, or what the player has set and not yet kept. */
struct DriverDraft {
	uint8_t sight = 0;
	uint8_t signals = 0;
	uint8_t stop_brake = 0;
	uint8_t memory = 0;
	uint8_t drop[11] = {};
	std::string name;

	bool operator==(const DriverDraft &) const = default;

	void ReadFrom(const Train *t)
	{
		this->name = t->driver_name;
		this->sight = t->driver_sight;
		this->signals = t->driver_signals;
		this->stop_brake = t->driver_stop_brake;
		this->memory = t->driver_memory;
		std::copy(std::begin(t->driver_drop), std::end(t->driver_drop), std::begin(this->drop));
	}

	uint8_t Get(TrainDriverField field) const
	{
		switch (field) {
			case TDF_SIGHT: return this->sight;
			case TDF_SIGNALS: return this->signals;
			case TDF_STOP_BRAKE: return this->stop_brake;
			case TDF_MEMORY: return this->memory;
			default: return this->drop[field - TDF_BAND];
		}
	}

	void Set(TrainDriverField field, uint8_t value)
	{
		switch (field) {
			case TDF_SIGHT: this->sight = value; break;
			case TDF_SIGNALS: this->signals = value; break;
			case TDF_STOP_BRAKE: this->stop_brake = value; break;
			case TDF_MEMORY: this->memory = value; break;
			default: this->drop[field - TDF_BAND] = value; break;
		}
	}
};

struct TrainDriverWindow : Window {
	DriverDraft draft; ///< what the window shows
	DriverDraft kept; ///< what the train has
	QueryString name_editbox; ///< the driver's name, the first row

	TrainDriverWindow(WindowDesc &desc, WindowNumber window_number) : Window(desc),
			name_editbox(MAX_LENGTH_VEHICLE_NAME_CHARS * MAX_CHAR_LENGTH, MAX_LENGTH_VEHICLE_NAME_CHARS)
	{
		this->CreateNestedTree();
		this->FinishInitNested(window_number);
		this->querystrings[WID_DRV_NAME] = &this->name_editbox;
		this->owner = Vehicle::Get(window_number)->owner;
		this->kept.ReadFrom(Train::Get(window_number));
		this->draft = this->kept;
		this->name_editbox.text.Assign(this->draft.name);
	}

	/** The name as typed goes into the draft, and "keep the changes" lights up. */
	void OnEditboxChanged(WidgetID widget) override
	{
		if (widget != WID_DRV_NAME) return;
		this->draft.name = this->name_editbox.text.GetText();
		this->SetWidgetDirty(WID_DRV_APPLY);
	}

	/* The texts the four dropdowns and their lists share: the game's own
	 * value strings, and "as the game says" with the game's value in it. */

	std::string AsGame(std::string &&value) const
	{
		return GetString(STR_TRAIN_DRIVER_AS_GAME, std::move(value));
	}

	std::string SightText(uint8_t value) const
	{
		if (value == 0) return this->AsGame(GetString(STR_CONFIG_SETTING_TRAIN_BRAKING_ETCS + std::min<uint8_t>(_settings_game.vehicle.train_braking, 4)));
		return GetString(STR_CONFIG_SETTING_TRAIN_BRAKING_ETCS + value - 1);
	}

	std::string SignalsText(uint8_t value) const
	{
		if (value == 0) return this->AsGame(GetString(STR_CONFIG_SETTING_TRAIN_WARNING_SIGNALS_1 + Clamp<int>(_settings_game.vehicle.train_driver_signals, 1, 3) - 1));
		return GetString(STR_CONFIG_SETTING_TRAIN_WARNING_SIGNALS_1 + value - 1);
	}

	std::string StopBrakeText(uint8_t value) const
	{
		if (value == 0) return this->AsGame(GetString(STR_CONFIG_SETTING_TRAIN_STOP_BRAKE_WEAKER_30 + std::min<uint8_t>(_settings_game.vehicle.train_stop_brake_weaker, 1)));
		return GetString(STR_CONFIG_SETTING_TRAIN_STOP_BRAKE_WEAKER_30 + value - 1);
	}

	std::string MemoryText(uint8_t value) const
	{
		if (value == 0) return this->AsGame(GetString(STR_CONFIG_SETTING_TRAIN_WARNING_MEMORY_NEVER + std::min<uint8_t>(_settings_game.vehicle.train_warning_memory, 5)));
		return GetString(STR_CONFIG_SETTING_TRAIN_WARNING_MEMORY_NEVER + value - 1);
	}

	/** A box of the table: the train's own number in white, the game's in black. */
	std::string BandText(uint band) const
	{
		uint8_t own = this->draft.drop[band];
		if (own == 0) return GetString(STR_TRAIN_DRIVER_BAND_GAME, GameBrakeDropTable()[band]);
		return GetString(STR_TRAIN_DRIVER_BAND_OWN, own);
	}

	std::string GetWidgetString(WidgetID widget, StringID stringid) const override
	{
		switch (widget) {
			case WID_DRV_CAPTION: return GetString(STR_TRAIN_DRIVER_CAPTION, this->window_number);
			case WID_DRV_SIGHT: return this->SightText(this->draft.sight);
			case WID_DRV_SIGNALS: return this->SignalsText(this->draft.signals);
			case WID_DRV_STOP_BRAKE: return this->StopBrakeText(this->draft.stop_brake);
			case WID_DRV_MEMORY: return this->MemoryText(this->draft.memory);
			default:
				if (IsInsideMM(widget, WID_DRV_BAND, WID_DRV_BAND_END)) return this->BandText(widget - WID_DRV_BAND);
				return this->Window::GetWidgetString(widget, stringid);
		}
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		switch (widget) {
			case WID_DRV_TABLE_HEAD:
				size.height = GetCharacterHeight(FontSize::Normal);
				break;

			case WID_DRV_HELP:
				size.width = std::max<uint>(size.width, ScaleGUITrad(360));
				size.height = GetStringHeight(GetString(STR_TRAIN_DRIVER_HELP), size.width);
				break;

			default:
				/* A box of the table has to hold its widest number whatever it
				 * shows now, or the row would jump about as numbers change. */
				if (IsInsideMM(widget, WID_DRV_BAND, WID_DRV_BAND_END)) {
					size.width = std::max<uint>(size.width, GetStringBoundingBox(GetString(STR_TRAIN_DRIVER_BAND_OWN, 60)).width + NWidgetLeaf::dropdown_dimension.width + WidgetDimensions::scaled.dropdowntext.Horizontal());
				}
				break;
		}
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		switch (widget) {
			case WID_DRV_TABLE_HEAD: {
				/* A speed over each edge between two boxes, the first and last
				 * over the outer edges -- the same row the settings window
				 * draws (BrakeTableEntry). Read off the boxes below, so the two
				 * rows line up whatever the window's width. */
				for (uint k = 0; k <= 11; k++) {
					const NWidgetBase *box = this->GetWidget<NWidgetBase>(WID_DRV_BAND + std::min<uint>(k, 10));
					int x = k < 11 ? box->pos_x : box->pos_x + box->current_x;
					int half = box->current_x / 2;
					std::string label = k == 0 ? "300+" : fmt::format("{}", DRIVER_BAND_TOP[k]);
					if (k == 0) {
						DrawString(x, x + half, r.top, label, TextColour::Black, AlignmentH::ForceLeft);
					} else if (k == 11) {
						DrawString(x - half, x, r.top, label, TextColour::Black, AlignmentH::ForceRight);
					} else {
						DrawString(x - half, x + half, r.top, label, TextColour::Black, AlignmentH::Centre);
					}
				}
				break;
			}

			case WID_DRV_HELP:
				DrawStringMultiLine(r, STR_TRAIN_DRIVER_HELP);
				break;

			default:
				break;
		}
	}

	void OnPaint() override
	{
		this->SetWidgetDisabledState(WID_DRV_APPLY, this->draft == this->kept);
		this->DrawWidgets();
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_DRV_SIGHT: {
				DropDownList list;
				for (uint8_t i = 0; i <= 5; i++) list.push_back(MakeDropDownListStringItem(this->SightText(i), i));
				ShowDropDownList(this, std::move(list), this->draft.sight, widget);
				break;
			}

			case WID_DRV_SIGNALS: {
				DropDownList list;
				for (uint8_t i = 0; i <= 3; i++) list.push_back(MakeDropDownListStringItem(this->SignalsText(i), i));
				ShowDropDownList(this, std::move(list), this->draft.signals, widget);
				break;
			}

			case WID_DRV_STOP_BRAKE: {
				DropDownList list;
				for (uint8_t i = 0; i <= 2; i++) list.push_back(MakeDropDownListStringItem(this->StopBrakeText(i), i));
				ShowDropDownList(this, std::move(list), this->draft.stop_brake, widget);
				break;
			}

			case WID_DRV_MEMORY: {
				DropDownList list;
				for (uint8_t i = 0; i <= 6; i++) list.push_back(MakeDropDownListStringItem(this->MemoryText(i), i));
				ShowDropDownList(this, std::move(list), this->draft.memory, widget);
				break;
			}

			case WID_DRV_RESET: {
				/* Everything as the game says -- in the draft; keeping it is
				 * the other button's. The name is no game setting and stays. */
				std::string name = std::move(this->draft.name);
				this->draft = DriverDraft{};
				this->draft.name = std::move(name);
				this->SetDirty();
				break;
			}

			case WID_DRV_APPLY: {
				const Train *t = Train::Get(static_cast<VehicleID>(this->window_number));
				for (uint f = TDF_SIGHT; f < TDF_END; f++) {
					TrainDriverField field = static_cast<TrainDriverField>(f);
					if (this->draft.Get(field) == this->kept.Get(field)) continue;
					Command<Commands::SetTrainDriver>::Post(t->tile, t->index, field, this->draft.Get(field));
				}
				if (this->draft.name != this->kept.name) Command<Commands::SetTrainDriverName>::Post(t->index, this->draft.name);
				break;
			}

			default:
				if (IsInsideMM(widget, WID_DRV_BAND, WID_DRV_BAND_END)) {
					uint band = widget - WID_DRV_BAND;
					DropDownList list;
					list.push_back(MakeDropDownListStringItem(this->AsGame(GetString(STR_JUST_INT, GameBrakeDropTable()[band])), 0));
					for (int i = 1; i <= 60; i++) list.push_back(MakeDropDownListStringItem(GetString(STR_JUST_INT, i), i));
					ShowDropDownList(this, std::move(list), this->draft.drop[band], widget);
				}
				break;
		}
	}

	void OnDropdownSelect(WidgetID widget, int index, int) override
	{
		switch (widget) {
			case WID_DRV_SIGHT: this->draft.sight = static_cast<uint8_t>(index); break;
			case WID_DRV_SIGNALS: this->draft.signals = static_cast<uint8_t>(index); break;
			case WID_DRV_STOP_BRAKE: this->draft.stop_brake = static_cast<uint8_t>(index); break;
			case WID_DRV_MEMORY: this->draft.memory = static_cast<uint8_t>(index); break;
			default:
				if (IsInsideMM(widget, WID_DRV_BAND, WID_DRV_BAND_END)) this->draft.drop[widget - WID_DRV_BAND] = static_cast<uint8_t>(index);
				break;
		}
		this->SetDirty();
	}

	/**
	 * The train's driver changed (a command went through, ours or another
	 * player's): read him again. A draft the player is still working on is
	 * kept; one that matched the train follows it.
	 */
	void OnInvalidateData([[maybe_unused]] int data = 0, bool gui_scope = true) override
	{
		if (!gui_scope) return;
		const Train *t = Train::GetIfValid(static_cast<VehicleID>(this->window_number));
		if (t == nullptr || !t->IsFrontEngine()) {
			this->Close();
			return;
		}
		bool following = this->draft == this->kept;
		this->kept.ReadFrom(t);
		if (following) {
			this->draft = this->kept;
			this->name_editbox.text.Assign(this->draft.name);
		}
		this->SetDirty();
	}
};

static constexpr std::initializer_list<NWidgetPart> _nested_train_driver_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::Grey),
		NWidget(WWT_CAPTION, Colours::Grey, WID_DRV_CAPTION),
		NWidget(WWT_SHADEBOX, Colours::Grey),
		NWidget(WWT_STICKYBOX, Colours::Grey),
	EndContainer(),
	NWidget(WWT_PANEL, Colours::Grey),
		NWidget(NWID_VERTICAL), SetPadding(WidgetDimensions::unscaled.framerect), SetPIP(0, WidgetDimensions::unscaled.vsep_normal, 0),
			NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_wide, 0),
				NWidget(WWT_TEXT, Colours::Invalid), SetStringTip(STR_TRAIN_DRIVER_NAME), SetFill(1, 0),
				NWidget(WWT_EDITBOX, Colours::Grey, WID_DRV_NAME), SetMinimalSize(170, 12), SetStringTip(STR_TRAIN_DRIVER_NAME_OSKTITLE, STR_TRAIN_DRIVER_NAME_TOOLTIP),
			EndContainer(),
			NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_wide, 0),
				NWidget(WWT_TEXT, Colours::Invalid), SetStringTip(STR_TRAIN_DRIVER_SIGHT), SetFill(1, 0),
				NWidget(WWT_DROPDOWN, Colours::Grey, WID_DRV_SIGHT), SetMinimalSize(170, 12), SetToolTip(STR_TRAIN_DRIVER_SIGHT_TOOLTIP),
			EndContainer(),
			NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_wide, 0),
				NWidget(WWT_TEXT, Colours::Invalid), SetStringTip(STR_TRAIN_DRIVER_SIGNALS), SetFill(1, 0),
				NWidget(WWT_DROPDOWN, Colours::Grey, WID_DRV_SIGNALS), SetMinimalSize(170, 12), SetToolTip(STR_TRAIN_DRIVER_SIGNALS_TOOLTIP),
			EndContainer(),
			NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_wide, 0),
				NWidget(WWT_TEXT, Colours::Invalid), SetStringTip(STR_TRAIN_DRIVER_STOP_BRAKE), SetFill(1, 0),
				NWidget(WWT_DROPDOWN, Colours::Grey, WID_DRV_STOP_BRAKE), SetMinimalSize(170, 12), SetToolTip(STR_TRAIN_DRIVER_STOP_BRAKE_TOOLTIP),
			EndContainer(),
			NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_wide, 0),
				NWidget(WWT_TEXT, Colours::Invalid), SetStringTip(STR_TRAIN_DRIVER_MEMORY), SetFill(1, 0),
				NWidget(WWT_DROPDOWN, Colours::Grey, WID_DRV_MEMORY), SetMinimalSize(170, 12), SetToolTip(STR_TRAIN_DRIVER_MEMORY_TOOLTIP),
			EndContainer(),
			NWidget(WWT_EMPTY, Colours::Invalid, WID_DRV_TABLE_HEAD), SetFill(1, 0),
			NWidget(NWID_HORIZONTAL),
				NWidget(WWT_DROPDOWN, Colours::Grey, WID_DRV_BAND + 0), SetFill(1, 0), SetToolTip(STR_TRAIN_DRIVER_BAND_TOOLTIP),
				NWidget(WWT_DROPDOWN, Colours::Grey, WID_DRV_BAND + 1), SetFill(1, 0), SetToolTip(STR_TRAIN_DRIVER_BAND_TOOLTIP),
				NWidget(WWT_DROPDOWN, Colours::Grey, WID_DRV_BAND + 2), SetFill(1, 0), SetToolTip(STR_TRAIN_DRIVER_BAND_TOOLTIP),
				NWidget(WWT_DROPDOWN, Colours::Grey, WID_DRV_BAND + 3), SetFill(1, 0), SetToolTip(STR_TRAIN_DRIVER_BAND_TOOLTIP),
				NWidget(WWT_DROPDOWN, Colours::Grey, WID_DRV_BAND + 4), SetFill(1, 0), SetToolTip(STR_TRAIN_DRIVER_BAND_TOOLTIP),
				NWidget(WWT_DROPDOWN, Colours::Grey, WID_DRV_BAND + 5), SetFill(1, 0), SetToolTip(STR_TRAIN_DRIVER_BAND_TOOLTIP),
				NWidget(WWT_DROPDOWN, Colours::Grey, WID_DRV_BAND + 6), SetFill(1, 0), SetToolTip(STR_TRAIN_DRIVER_BAND_TOOLTIP),
				NWidget(WWT_DROPDOWN, Colours::Grey, WID_DRV_BAND + 7), SetFill(1, 0), SetToolTip(STR_TRAIN_DRIVER_BAND_TOOLTIP),
				NWidget(WWT_DROPDOWN, Colours::Grey, WID_DRV_BAND + 8), SetFill(1, 0), SetToolTip(STR_TRAIN_DRIVER_BAND_TOOLTIP),
				NWidget(WWT_DROPDOWN, Colours::Grey, WID_DRV_BAND + 9), SetFill(1, 0), SetToolTip(STR_TRAIN_DRIVER_BAND_TOOLTIP),
				NWidget(WWT_DROPDOWN, Colours::Grey, WID_DRV_BAND + 10), SetFill(1, 0), SetToolTip(STR_TRAIN_DRIVER_BAND_TOOLTIP),
			EndContainer(),
			NWidget(WWT_EMPTY, Colours::Invalid, WID_DRV_HELP), SetFill(1, 0),
		EndContainer(),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_DRV_RESET), SetFill(1, 0), SetStringTip(STR_TRAIN_DRIVER_RESET, STR_TRAIN_DRIVER_RESET_TOOLTIP),
		NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_DRV_APPLY), SetFill(1, 0), SetStringTip(STR_TRAIN_DRIVER_APPLY, STR_TRAIN_DRIVER_APPLY_TOOLTIP),
	EndContainer(),
};

static WindowDesc _train_driver_desc(
	WindowPosition::Automatic, "train_driver", 0, 0,
	WindowClass::TrainDriver, WindowClass::VehicleView,
	{},
	_nested_train_driver_widgets
);

/**
 * Open the driver window of a train, or bring it to the front.
 * @param v the train, the head of its consist
 */
void ShowTrainDriverWindow(const Vehicle *v)
{
	if (v->type != VehicleType::Train || !Train::From(v)->IsFrontEngine()) return;
	AllocateWindowDescFront<TrainDriverWindow>(_train_driver_desc, v->index);
}
