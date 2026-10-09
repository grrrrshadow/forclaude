/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file vehicle_config_gui.cpp The configurator: the window where the player chooses the details a vehicle's set offers. */

#include "stdafx.h"
#include "vehicle_config.h"
#include "vehicle_base.h"
#include "vehicle_cmd.h"
#include "command_func.h"
#include "window_gui.h"
#include "window_func.h"
#include "strings_func.h"
#include "dropdown_type.h"
#include "dropdown_func.h"
#include "cargotype.h"
#include "widgets/vehicle_widget.h"

#include "table/strings.h"

#include "safeguards.h"

/**
 * The configurator (vehicle_config.h): a row for each detail the vehicle's
 * set offers -- the detail's name and a dropdown of its options -- and under
 * them the cargo, which is the refit window's to change and is only said
 * here. No picture of the vehicle: the player wants none here. Opened from
 * the refit window and closed with it; like a refit it works on a vehicle
 * stopped in a depot, and every choice goes to the vehicle at once
 * (Commands::ConfigureVehicle).
 */
struct VehicleConfigWindow : Window {
	std::vector<VehicleConfigAspect> aspects; ///< The details the set offers, read from it.

	VehicleConfigWindow(WindowDesc &desc, WindowNumber window_number, Window *parent) : Window(desc)
	{
		/* Opened from the refit window it goes with it: the player, "let the
		 * configurator close with the refit window". */
		this->parent = parent;
		this->CreateNestedTree();
		this->ReadAspects(static_cast<VehicleID>(window_number));
		this->FinishInitNested(window_number);
		this->owner = Vehicle::Get(static_cast<VehicleID>(window_number))->owner;
	}

	/** Which details the set offers: a row for each, the other rows away. */
	void ReadAspects(VehicleID index)
	{
		this->aspects = GetVehicleConfigAspects(Vehicle::Get(index)->engine_type);
		for (uint i = 0; i < VEHICLE_CONFIG_MAX_ASPECTS; i++) {
			this->GetWidget<NWidgetStacked>(WID_VC_ROW + i)->SetDisplayedPlane(i < this->aspects.size() ? 0 : SZSP_NONE);
		}
		this->GetWidget<NWidgetStacked>(WID_VC_NONE_SEL)->SetDisplayedPlane(this->aspects.empty() ? 0 : SZSP_NONE);
	}

	/** The cargo the vehicle carries, from the first part that carries any. */
	std::string CargoName() const
	{
		for (const Vehicle *u = Vehicle::Get(static_cast<VehicleID>(this->window_number)); u != nullptr; u = u->Next()) {
			if (u->cargo_cap == 0 || !IsValidCargoType(u->cargo_type)) continue;
			return GetString(CargoSpec::Get(u->cargo_type)->name);
		}
		return GetString(STR_VEHICLE_CONFIG_NO_CARGO);
	}

	/**
	 * The name of an option, or a number where the set has no name for it --
	 * a vehicle made with a release of the set that offered more.
	 */
	std::string OptionName(uint aspect, uint option) const
	{
		if (aspect >= this->aspects.size()) return {};
		const std::vector<std::string> &options = this->aspects[aspect].options;
		if (option >= options.size()) return GetString(STR_VEHICLE_CONFIG_OPTION_UNKNOWN, option);
		return options[option];
	}

	std::string GetWidgetString(WidgetID widget, StringID stringid) const override
	{
		if (widget == WID_VC_CAPTION) return GetString(STR_VEHICLE_CONFIG_CAPTION, this->window_number);
		if (widget == WID_VC_CARGO) return GetString(STR_VEHICLE_CONFIG_CARGO, this->CargoName());
		if (IsInsideMM(widget, WID_VC_LABEL, WID_VC_LABEL_END)) {
			uint i = widget - WID_VC_LABEL;
			return i < this->aspects.size() ? this->aspects[i].name : std::string{};
		}
		if (IsInsideMM(widget, WID_VC_DROPDOWN, WID_VC_DROPDOWN_END)) {
			uint i = widget - WID_VC_DROPDOWN;
			return this->OptionName(i, Vehicle::Get(static_cast<VehicleID>(this->window_number))->config_options[i]);
		}
		return this->Window::GetWidgetString(widget, stringid);
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		/* A dropdown holds its widest option whatever it shows now, so that
		 * the window does not jump about as the player chooses. */
		if (IsInsideMM(widget, WID_VC_DROPDOWN, WID_VC_DROPDOWN_END)) {
			uint i = widget - WID_VC_DROPDOWN;
			if (i >= this->aspects.size()) return;
			for (const std::string &option : this->aspects[i].options) {
				size.width = std::max<uint>(size.width, GetStringBoundingBox(option).width + NWidgetLeaf::dropdown_dimension.width + WidgetDimensions::scaled.dropdowntext.Horizontal());
			}
		}
	}

	void OnPaint() override
	{
		/* Like a refit, a change is for a vehicle stopped in a depot. */
		bool in_depot = Vehicle::Get(static_cast<VehicleID>(this->window_number))->IsStoppedInDepot();
		for (uint i = 0; i < VEHICLE_CONFIG_MAX_ASPECTS; i++) this->SetWidgetDisabledState(WID_VC_DROPDOWN + i, !in_depot);
		this->DrawWidgets();
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		if (!IsInsideMM(widget, WID_VC_DROPDOWN, WID_VC_DROPDOWN_END)) return;
		uint i = widget - WID_VC_DROPDOWN;
		if (i >= this->aspects.size()) return;
		DropDownList list;
		for (uint o = 0; o < this->aspects[i].options.size(); o++) {
			list.push_back(MakeDropDownListStringItem(std::string{this->aspects[i].options[o]}, o));
		}
		ShowDropDownList(this, std::move(list), Vehicle::Get(static_cast<VehicleID>(this->window_number))->config_options[i], widget);
	}

	void OnDropdownSelect(WidgetID widget, int index, int) override
	{
		if (!IsInsideMM(widget, WID_VC_DROPDOWN, WID_VC_DROPDOWN_END)) return;
		const Vehicle *v = Vehicle::Get(static_cast<VehicleID>(this->window_number));
		Command<Commands::ConfigureVehicle>::Post(STR_ERROR_CAN_T_CONFIGURE_VEHICLE, v->tile, v->index, static_cast<uint8_t>(widget - WID_VC_DROPDOWN), static_cast<uint8_t>(index));
	}

	/** The vehicle changed (a choice went through, or it was sold): read it again. */
	void OnInvalidateData([[maybe_unused]] int data = 0, bool gui_scope = true) override
	{
		if (!gui_scope) return;
		const Vehicle *v = Vehicle::GetIfValid(static_cast<VehicleID>(this->window_number));
		if (v == nullptr || v != v->First()) {
			this->Close();
			return;
		}
		this->ReadAspects(v->index);
		this->ReInit();
	}
};

/**
 * The rows of the details, one for each the set may offer
 * (VEHICLE_CONFIG_MAX_ASPECTS): its name and the dropdown of its options,
 * the rows the set does not use away (VehicleConfigWindow::ReadAspects()).
 * @return the rows
 */
static std::unique_ptr<NWidgetBase> MakeVehicleConfigRows()
{
	static_assert(WID_VC_ROW_END - WID_VC_ROW == VEHICLE_CONFIG_MAX_ASPECTS);
	auto rows = std::make_unique<NWidgetVertical>();
	rows->SetPIP(0, WidgetDimensions::unscaled.vsep_normal, 0);
	for (uint i = 0; i < VEHICLE_CONFIG_MAX_ASPECTS; i++) {
		auto label = std::make_unique<NWidgetLeaf>(WWT_TEXT, Colours::Invalid, WID_VC_LABEL + i, WidgetData{}, STR_NULL);
		label->SetFill(1, 0);
		auto dropdown = std::make_unique<NWidgetLeaf>(WWT_DROPDOWN, Colours::Grey, WID_VC_DROPDOWN + i, WidgetData{}, STR_VEHICLE_CONFIG_OPTION_TOOLTIP);
		dropdown->SetMinimalSize(170, 12);
		auto line = std::make_unique<NWidgetHorizontal>();
		line->SetPIP(0, WidgetDimensions::unscaled.hsep_wide, 0);
		line->Add(std::move(label));
		line->Add(std::move(dropdown));
		auto row = std::make_unique<NWidgetStacked>(WID_VC_ROW + i);
		row->Add(std::move(line));
		rows->Add(std::move(row));
	}
	return rows;
}

static constexpr std::initializer_list<NWidgetPart> _nested_vehicle_config_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::Grey),
		NWidget(WWT_CAPTION, Colours::Grey, WID_VC_CAPTION),
		NWidget(WWT_SHADEBOX, Colours::Grey),
		NWidget(WWT_STICKYBOX, Colours::Grey),
	EndContainer(),
	NWidget(WWT_PANEL, Colours::Grey),
		NWidget(NWID_VERTICAL), SetPadding(WidgetDimensions::unscaled.framerect), SetPIP(0, WidgetDimensions::unscaled.vsep_normal, 0),
			NWidget(NWID_SELECTION, Colours::Invalid, WID_VC_NONE_SEL),
				NWidget(WWT_TEXT, Colours::Invalid, WID_VC_NONE), SetStringTip(STR_VEHICLE_CONFIG_NONE), SetFill(1, 0),
			EndContainer(),
			NWidgetFunction(MakeVehicleConfigRows),
			NWidget(WWT_TEXT, Colours::Invalid, WID_VC_CARGO), SetFill(1, 0),
		EndContainer(),
	EndContainer(),
};

static WindowDesc _vehicle_config_desc(
	WindowPosition::Automatic, "vehicle_config", 0, 0,
	WindowClass::VehicleConfig, WindowClass::VehicleView,
	{},
	_nested_vehicle_config_widgets
);

/**
 * Open the configurator of a vehicle, or bring it to the front.
 * @param v the vehicle, its front
 * @param parent the window it is opened from, which it closes with; the refit window
 */
void ShowVehicleConfigWindow(const Vehicle *v, Window *parent)
{
	if (v != v->First()) return;
	AllocateWindowDescFront<VehicleConfigWindow>(_vehicle_config_desc, v->index, parent);
}
