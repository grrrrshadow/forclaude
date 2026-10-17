/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file first_person_gui.cpp The view from the first person: a walker on the map, seen from its eyes.
 *
 * The player (15. 10.), after looking at OpenMW: "let's make Morrowind, the inventory, the first person view", "and
 * don't forget anything, the lights". The world is drawn as from the cab of a vehicle (cab_view.h, DrawEyeView() in
 * viewport.cpp), the eye at the walker's eyes; the walker goes with W and S or the arrows up and down, steps aside
 * with A and D, turns with the arrows left and right, looks about with the right mouse button held (as the map is
 * dragged), runs with Shift. It walks the ground as it lies and does not go through houses, industries' buildings
 * or water. At night (gui.night) the picture is dark and the vehicles shine. Only the picture: nothing of the game
 * changes, and nothing of it is saved.
 */

#include "stdafx.h"
#include "cab_view.h"
#include "window_gui.h"
#include "window_func.h"
#include "gfx_func.h"
#include "strings_func.h"
#include "viewport_func.h"
#include "tile_map.h"
#include "map_func.h"
#include "town.h"
#include "house.h"
#include "industry.h"
#include "industry_map.h"
#include "station_map.h"
#include "water_map.h"
#include "three_d.h"
#include "three_d_objects.h"
#include "timer/timer.h"
#include "timer/timer_window.h"
#include "blitter/factory.hpp"
#include "widgets/first_person_widget.h"
#include "walker_inventory.h"
#include "company_base.h"
#include "company_func.h"
#include "command_func.h"
#include "walker_cmd.h"
#include "three_d_dragons.h"
#include "vehicle_base.h"
#include "cab_drive.h"
#include "sound_func.h"

#include "table/strings.h"
#include "table/sprites.h"

#include <cmath>

#include "safeguards.h"

/** The walker's eyes over the ground, metres (a girl of the indices is 1.62 m tall). */
static constexpr double FIRST_PERSON_EYES_M = 1.55;
/** How fast it walks and runs, metres a second. */
static constexpr double FIRST_PERSON_WALK_MS = 1.6;
static constexpr double FIRST_PERSON_RUN_MS = 6.5;
/** How fast it turns on the arrows, radians a second. */
static constexpr double FIRST_PERSON_TURN = 2.0;
/** How far it looks up or down at most, radians. */
static constexpr double FIRST_PERSON_PITCH_MAX = 1.0;
/** How much a pixel of the mouse turns it, radians. */
static constexpr double FIRST_PERSON_MOUSE = 0.005;
/** How far from a wall it stops, world units. */
static constexpr double FIRST_PERSON_RADIUS = 1.5;
/** How often it moves and the picture is made, milliseconds. */
static constexpr int FIRST_PERSON_TICK_MS = 40;
/** How long taking a weapon out or putting it away takes, and a swing of it, seconds. */
static constexpr double FIRST_PERSON_DRAW_S = 0.35;
static constexpr double FIRST_PERSON_SWING_S = 0.6;
/** When in the swing the weapon strikes, seconds. */
static constexpr double FIRST_PERSON_BLOW_S = 0.27;
/** How long the bow is drawn fully, how long a new arrow is set on its string, how long the crossbow is drawn again, seconds. */
static constexpr double FIRST_PERSON_BOW_DRAW_S = 0.8;
static constexpr double FIRST_PERSON_NOCK_S = 0.5;
static constexpr double FIRST_PERSON_RELOAD_S = 1.5;
/** How long a shot kicks the bow or the crossbow back, seconds. */
static constexpr double FIRST_PERSON_KICK_S = 0.15;
/** An arrow's fall, metres a second each second; how long one flies at most and stays stuck where it hit, seconds; how many there are at most. */
static constexpr double FIRST_PERSON_GRAVITY = 9.81;
static constexpr double FIRST_PERSON_FLY_S = 8.0;
static constexpr double FIRST_PERSON_STUCK_S = 20.0;
static constexpr size_t FIRST_PERSON_MISSILES = 40;
/** How near a vehicle the walker gets into it (E), metres from a piece of it. */
static constexpr double FIRST_PERSON_BOARD_M = 5.0;

/** How a weapon of the walker is used. */
enum class WeaponUse : uint8_t {
	Swing,    ///< swung: the hammer, the swords
	Bow,      ///< drawn while held, shot let go
	Crossbow, ///< shot, then drawn again
};

/**
 * A weapon of the walker, by its key 1 to 5 (the player, 15. 10.: the big hammer; 17. 10.: "the bow and the crossbow
 * are there", and the colleague's swords): the thing of the inventory, and the colleague's thing of the indices
 * (objekty/predmety/, hra/3d/LUK-SIP-PRO-HRU.md).
 */
struct FirstPersonWeapon {
	WalkerItem item;        ///< the thing of the inventory
	std::string_view thing; ///< its thing of the indices
	WeaponUse use;          ///< swung or shot
	double reach_m;         ///< swung: how far it reaches, metres
	int damage;             ///< the health a blow or a hit takes of a dragon
	WalkerItem ammo = WalkerItem::End; ///< shot: what it shoots ...
	std::string_view missile = {};     ///< ... and its thing of the indices
	double speed_ms = 0;    ///< how fast that flies, metres a second (the bow drawn fully)
	float twist = 0;        ///< swung: degrees it is turned about its handle in the hand (a sword's flat to the eye, not its edge)
};
static const FirstPersonWeapon _first_person_weapons[] = {
	{WalkerItem::Hammer, "kladivo", WeaponUse::Swing, 2.5, 1},
	{WalkerItem::Bow, "luk", WeaponUse::Bow, 0, 1, WalkerItem::Arrows, "sip", 55},
	{WalkerItem::Crossbow, "kuse", WeaponUse::Crossbow, 0, 2, WalkerItem::Bolts, "sipka", 70},
	{WalkerItem::Sword, "mec", WeaponUse::Swing, 2.2, 1, WalkerItem::End, {}, 0, 65},
	{WalkerItem::TwoHandedSword, "mec_obourucni", WeaponUse::Swing, 2.8, 2, WalkerItem::End, {}, 0, 65},
};

/** An arrow or a bolt the walker shot: flying, or stuck where it hit. Only the screen, never saved. */
struct FirstPersonMissile {
	std::string_view thing; ///< its thing of the indices, its +x its tip, its origin its nock
	int damage = 1;         ///< the health it takes of a dragon it hits
	double x = 0, y = 0, z = 0;    ///< its origin, world units and units of height
	double vx = 0, vy = 0, vz = 0; ///< how fast it goes, metres a second
	double length_m = 0.75; ///< from its nock to its tip
	double age = 0;         ///< seconds flying, or since it stuck
	bool stuck = false;     ///< stuck in the ground or a wall
	bool gone = false;      ///< in a dragon, in the water or off the map: gone
	float facing = 0;       ///< degrees in the world its tip points to
	float pitch = 0;        ///< degrees its tip is raised
};

/**
 * The angle of a swing of the hammer a time into it, degrees (DrawThreeDHeldThing()): raised back over the shoulder,
 * struck forward and down, and back to rest.
 */
static float SwingAngle(double t)
{
	if (t < 0.15) return static_cast<float>(-40.0 * t / 0.15);
	if (t < 0.27) return static_cast<float>(-40.0 + 110.0 * (t - 0.15) / 0.12);
	if (t < FIRST_PERSON_SWING_S) return static_cast<float>(70.0 * (1.0 - (t - 0.27) / (FIRST_PERSON_SWING_S - 0.27)));
	return 0.0f;
}

static constexpr NWidgetPart _nested_first_person_widgets[] = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::Grey),
		NWidget(WWT_CAPTION, Colours::Grey, WID_FP_CAPTION), SetStringTip(STR_FIRST_PERSON_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_SHADEBOX, Colours::Grey),
		NWidget(WWT_DEFSIZEBOX, Colours::Grey),
		NWidget(WWT_PUSHIMGBTN, Colours::Grey, WID_FP_FULL), SetSpriteTip(SPR_LARGE_SMALL_WINDOW, STR_FIRST_PERSON_FULL_TOOLTIP), SetAspect(1),
		NWidget(WWT_STICKYBOX, Colours::Grey),
	EndContainer(),
	NWidget(WWT_PANEL, Colours::Grey),
		NWidget(WWT_EMPTY, Colours::Invalid, WID_FP_CANVAS), SetMinimalSize(480, 300), SetResize(1, 1), SetFill(1, 1),
	EndContainer(),
	NWidget(WWT_PANEL, Colours::Grey, WID_FP_PANEL), SetMinimalSize(480, 52), SetResize(1, 0), SetFill(1, 0), EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_FP_HERE), SetStringTip(STR_FIRST_PERSON_HERE, STR_FIRST_PERSON_HERE_TOOLTIP), SetFill(1, 0), SetResize(1, 0),
		NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_FP_CENTRE), SetStringTip(STR_FIRST_PERSON_CENTRE, STR_FIRST_PERSON_CENTRE_TOOLTIP), SetFill(1, 0), SetResize(1, 0),
		NWidget(WWT_TEXTBTN, Colours::Grey, WID_FP_RUN), SetStringTip(STR_FIRST_PERSON_RUN, STR_FIRST_PERSON_RUN_TOOLTIP), SetFill(1, 0), SetResize(1, 0),
		NWidget(WWT_PUSHTXTBTN, Colours::Brown, WID_FP_INVENTORY), SetStringTip(STR_FIRST_PERSON_INVENTORY, STR_FIRST_PERSON_INVENTORY_TOOLTIP), SetFill(1, 0), SetResize(1, 0),
		NWidget(WWT_RESIZEBOX, Colours::Grey),
	EndContainer(),
};

/**
 * Whether the walker can stand on a world point: not in a house, an industry's building (a field is walked), a
 * station's building (a platform or a stop is walked), an object, the water or off the map.
 */
static bool FirstPersonWalkable(double x, double y)
{
	if (x < TILE_SIZE || y < TILE_SIZE || x >= (Map::SizeX() - 1) * TILE_SIZE || y >= (Map::SizeY() - 1) * TILE_SIZE) return false;
	TileIndex tile = TileVirtXY(static_cast<int>(x), static_cast<int>(y));
	switch (GetTileType(tile)) {
		case TileType::Void:
		case TileType::Object:
			return false;
		case TileType::House: {
			/* A house that is a 3D object: by its shape (ThreeDObject::Blocks()), into its park, under its roof. */
			if (!IsHouseCompleted(tile)) return false;
			HouseID north = GetHouseType(tile);
			const TileIndex origin = tile + GetHouseNorthPart(north);
			const ThreeDObject *obj = GetThreeDHouseObjectAt(north, origin);
			return obj != nullptr && !obj->Blocks(x - TileX(origin) * TILE_SIZE, y - TileY(origin) * TILE_SIZE);
		}
		case TileType::Water:
			return IsCoast(tile);
		case TileType::Industry: {
			const IndustryGfx gfx = GetIndustryGfx(tile);
			if (gfx == GFX_MARIJUANA_PLANTATION || gfx == GFX_POTATO_FIELD) return true;
			/* One of the game's own that is a 3D object (the pub, the distillery): by its shape, as a house. */
			if (!IsIndustryCompleted(tile)) return false;
			const Industry *ind = Industry::GetByTile(tile);
			const ThreeDObject *obj = GetThreeDIndustryObjectOf(ind);
			const TileIndex origin = ind->location.tile;
			return obj != nullptr && !obj->Blocks(x - TileX(origin) * TILE_SIZE, y - TileY(origin) * TILE_SIZE);
		}
		case TileType::Station:
			return HasStationRail(tile) || IsAnyRoadStop(tile);
		default:
			return true;
	}
}

/**
 * Whether the walker, a circle, can stand on a world point: its middle and eight points round it, so that the thin
 * walls of a house's shape (ThreeDObject::Blocks()) slip between none of them.
 */
static bool FirstPersonFree(double x, double y)
{
	if (!FirstPersonWalkable(x, y)) return false;
	for (int k = 0; k < 8; k++) {
		const double a = k * M_PI / 4;
		if (!FirstPersonWalkable(x + FIRST_PERSON_RADIUS * std::cos(a), y + FIRST_PERSON_RADIUS * std::sin(a))) return false;
	}
	return true;
}

void ShowWalkerInventoryWindow();
TileIndex FirstPersonShopNearby();
void RedrawFirstPersonPicture();

/** The window of the view from the first person; there is one, number 0. */
struct FirstPersonWindow : Window {
	FirstPersonEye walker{};             ///< where the walker is and looks
	mutable CabViewCamera camera{};      ///< the eye between frames
	mutable CabViewPicture picture{};    ///< the last picture
	mutable bool picture_ok = true;       ///< whether the last picture could be made (32 bpp)
	bool full = false;                    ///< over the whole screen, between the toolbar and the status bar
	Rect before_full{};                   ///< where it was before that
	bool running = false;                 ///< runs instead of walking (also while Shift is held)
	bool weapon_out = false;              ///< the weapon taken out (F, or its key), in the hand
	int weapon = 0;                       ///< the weapon in the hand or put away (_first_person_weapons)
	int wanted = 0;                       ///< the weapon asked for (its key): the one in the hand is put away first
	double swing_time = -1;               ///< seconds into a swing of the weapon, -1 when it is not swung
	double draw_time = -1;                ///< seconds the bow is drawn, -1 when it is not
	bool mouse_draw = false;              ///< the bow drawn with the left button held: let go, it shoots
	double reload_time = 0;               ///< seconds till the next arrow is on the string, the crossbow drawn again
	float kick = 0;                       ///< a shot's kick, 1 as it is shot to 0
	std::vector<FirstPersonMissile> missiles; ///< the arrows and the bolts shot
	int hurt_steps = 0;                   ///< steps the picture is still red of a dragon's blow
	/*
	 * In a vehicle (the player, 17. 10.: "in the first person game, E gets into the car, into the cab view; no other
	 * window of the cab view should open, it stays in the window of the first person -- a special window, the game
	 * under it is not drawn"): the picture is the view from its cab, the arrows drive it as from the cab view when it
	 * is the company's (else the walker rides along), E again gets out beside it.
	 */
	VehicleID riding = VehicleID::Invalid(); ///< the vehicle the walker is in, Invalid on foot
	CabArrows arrows{};                      ///< the arrows driving it

	/** The vehicle the walker is in, nullptr on foot. */
	const Vehicle *Riding() const
	{
		return this->riding != VehicleID::Invalid() ? Vehicle::GetIfValid(this->riding) : nullptr;
	}

	/**
	 * The vehicle near enough to get into: the nearest going about (a car, a train, a ship, not in its depot), a piece
	 * of it within reach and about as high as the ground under the walker (not on a bridge overhead).
	 */
	const Vehicle *VehicleToBoard() const
	{
		const Vehicle *best = nullptr;
		double best_d = FIRST_PERSON_BOARD_M * OBJECT_UNITS_PER_METRE;
		const double ground = FirstPersonGroundZ(this->walker.x, this->walker.y);
		for (const Vehicle *u : Vehicle::Iterate()) {
			if (u->type != VehicleType::Road && u->type != VehicleType::Train && u->type != VehicleType::Ship) continue;
			if (u->vehstatus.Any({VehState::Hidden, VehState::Crashed})) continue;
			if (std::abs(u->z_pos - ground) > 2 * TILE_HEIGHT) continue;
			const double d = std::hypot(u->x_pos - this->walker.x, u->y_pos - this->walker.y);
			if (d < best_d) {
				best_d = d;
				best = u->First();
			}
		}
		return best;
	}

	/** Into a vehicle: the picture from its cab from now on, the weapon away. */
	void GetIn(const Vehicle *v)
	{
		this->riding = v->index;
		this->weapon_out = false;
		this->swing_time = -1;
		this->draw_time = -1;
		this->arrows = {};
		this->camera = {};
		this->walker.x = v->x_pos;
		this->walker.y = v->y_pos;
		this->MakePicture();
		this->SetDirty();
	}

	/**
	 * Out of the vehicle: the cab lets go of it as when the cab view closes (CabLetGo()), and the walker stands beside
	 * its front, on its left (the driver's door), else on its right, else at the nearest place it can stand, facing
	 * the way the vehicle goes.
	 */
	void GetOut()
	{
		const Vehicle *v = this->Riding();
		this->riding = VehicleID::Invalid();
		this->arrows = {};
		this->camera = {};
		if (v != nullptr) {
			CabLetGo(v);
			const int yaw = v->cab_free ? v->cab_yaw : (v->direction != Direction::Invalid ? DirectionToCabYaw(v->direction) : 0);
			const double heading = yaw * 2 * M_PI / CAB_YAW_TURN, fx = std::cos(heading), fy = std::sin(heading);
			this->walker.heading = std::remainder(heading, 2 * M_PI);
			bool placed = false;
			for (double side : {1.0, -1.0}) {
				for (double metres : {2.5, 3.5, 5.0}) {
					const double d = metres * OBJECT_UNITS_PER_METRE;
					const double x = v->x_pos - fy * d * side, y = v->y_pos + fx * d * side;
					if (!placed && FirstPersonFree(x, y)) {
						this->walker.x = x;
						this->walker.y = y;
						placed = true;
					}
				}
			}
			if (!placed) this->PutNear(v->x_pos, v->y_pos);
		}
		this->MakePicture();
		this->SetDirty();
	}

	/** E: into the vehicle near, or out of the one the walker is in. */
	void BoardOrLeave()
	{
		if (this->Riding() != nullptr) {
			this->GetOut();
			return;
		}
		this->riding = VehicleID::Invalid();
		const Vehicle *v = this->VehicleToBoard();
		if (v != nullptr) this->GetIn(v);
	}

	/*
	 * Moves while the keys are held and makes the picture anew, twenty-five times a second -- only here: a paint of
	 * the window (the map under it marks the screen dirty as its vehicles move, in several pieces a frame) puts the
	 * picture made on the screen and nothing more.
	 */
	IntervalTimer<TimerWindow> step{std::chrono::milliseconds(FIRST_PERSON_TICK_MS), [this](uint count) { this->Step(count); }};

	/**
	 * The steps of the walker since the last picture, and the picture. A frame of the game longer than a step (the
	 * picture itself taking 30 to 200 ms, the map, the vehicles) is so many steps: each is taken, against the walls
	 * on its own, and one picture after them -- not one step a frame, which walked the walker (the player, 16. 10.:
	 * "walking is slow") as many times slower as a frame was longer than a step. At most a second of them at once (a
	 * save, the window dragged).
	 * @param count the steps since the last picture
	 */
	void Step(uint count)
	{
		const uint steps = std::min(count, 1000u / FIRST_PERSON_TICK_MS);
		/* In a vehicle: where it is (gone or crashed, the walker is out where it was); the arrows drive it. */
		const Vehicle *rv = this->Riding();
		if (rv != nullptr && rv->vehstatus.Test(VehState::Crashed)) {
			this->GetOut();
			rv = nullptr;
		}
		if (rv == nullptr) this->riding = VehicleID::Invalid();
		for (uint i = 0; i < steps; i++) {
			if (rv == nullptr) this->Walk(FIRST_PERSON_TICK_MS / 1000.0);
			this->Hold(FIRST_PERSON_TICK_MS / 1000.0);
			this->Fly(FIRST_PERSON_TICK_MS / 1000.0);
		}
		if (rv != nullptr) {
			this->walker.x = rv->x_pos;
			this->walker.y = rv->y_pos;
			this->arrows.Drive(rv, ArrowsWindow() == this ? _dirkeys : DirectionKeys{}, static_cast<int>(steps) * FIRST_PERSON_TICK_MS);
		}
		/* No health left: the walker is gone, back to the map (the player, 15. 10.). */
		const Company *c = Company::GetIfValid(_local_company);
		if (c != nullptr && c->walker_health == 0) {
			this->Close();
			return;
		}
		/* The dragons' blows since the last step (three_d_dragons.h): the health through the command, the picture red a moment. */
		const uint damage = TakeThreeDDragonDamage();
		if (damage > 0 && c != nullptr) {
			Command<Commands::HurtWalker>::Post(TileIndex{}, static_cast<uint8_t>(std::min(damage, 255u)));
			this->hurt_steps = 6;
		}
		this->MakePicture();
		this->hurt_steps = std::max(0, this->hurt_steps - static_cast<int>(steps));
		this->SetWidgetDirty(WID_FP_CANVAS);
		this->SetWidgetDirty(WID_FP_PANEL);
	}

	FirstPersonWindow(WindowDesc &desc, WindowNumber number) : Window(desc)
	{
		this->CreateNestedTree();
		this->FinishInitNested(number);
		this->walker.eye_z = FIRST_PERSON_EYES_M * OBJECT_HEIGHT_PER_METRE;
		this->walker.heading = std::atan2(1.0, 1.0); // looking south-east, down the screen of the map facing north
		this->PutWhereTheMainViewLooks();
		/* Opened over the whole screen (the player, 15. 10.); the button makes it a window again. */
		this->ToggleFull();
	}

	void Close(int data = 0) override
	{
		/* Out of the vehicle the walker was in: let go of it, as the cab view closing does. */
		CabLetGo(this->Riding());
		this->Window::Close(data);
	}

	/** The picture from the walker's eyes, as big as the canvas is now. */
	void MakePicture() const
	{
		const NWidgetBase *canvas = this->GetWidget<NWidgetBase>(WID_FP_CANVAS);
		const int w = canvas->current_x, h = canvas->current_y;
		if (w <= 0 || h <= 0) return;
		const Vehicle *rv = this->Riding();
		this->picture_ok = rv != nullptr ? DrawCabView(rv, w, h, this->camera, this->picture) : DrawFirstPersonView(this->walker, w, h, this->camera, this->picture);
		if (this->picture_ok && this->hurt_steps > 0) {
			/* Hurt: red over the picture, fading. */
			const uint a = 30 + this->hurt_steps * 18;
			for (uint32_t &p : this->picture.pixels) {
				Colour o(p);
				p = Colour(static_cast<uint8_t>((o.r * (255 - a) + 220 * a) / 255), static_cast<uint8_t>(o.g * (255 - a) / 255), static_cast<uint8_t>(o.b * (255 - a) / 255)).data;
			}
		}
	}

	/** Over the whole screen between the toolbar and the status bar, or back where the window was. */
	void ToggleFull()
	{
		if (!this->full) {
			this->before_full = {this->left, this->top, this->left + this->width, this->top + this->height};
			this->full = true;
			const int top = GetMainViewTop(), bottom = GetMainViewBottom();
			this->left = 0;
			this->top = top;
			ResizeWindow(this, _screen.width - this->width, bottom - top - this->height, false);
		} else {
			this->full = false;
			this->left = this->before_full.left;
			this->top = this->before_full.top;
			ResizeWindow(this, this->before_full.Width() - this->width, this->before_full.Height() - this->height, false);
		}
		this->MakePicture();
		MarkWholeScreenDirty();
	}

	/** The walker to where the middle of the main view is, on the ground; it steps aside to the nearest place it can stand. */
	void PutWhereTheMainViewLooks()
	{
		const Window *main = GetMainWindow();
		if (main == nullptr || main->viewport == nullptr) return;
		const Viewport &vp = *main->viewport;
		const double vx = vp.virtual_left + vp.virtual_width / 2.0, vy = vp.virtual_top + vp.virtual_height / 2.0;
		double x = 0, y = 0, z = 0;
		for (int k = 0; k < 3; k++) {
			InverseTurnedFlatF(vx, vy, z, x, y);
			z = FirstPersonGroundZ(x, y);
		}
		this->PutNear(x, y);
	}

	/** The walker to the nearest place it can stand from a world point. */
	void PutNear(double x, double y)
	{
		/* The nearest place to stand, in rings a quarter of a tile apart. */
		for (int ring = 0; ring < 24; ring++) {
			for (int k = 0; k < std::max(1, ring * 8); k++) {
				const double a = k * 2 * M_PI / std::max(1, ring * 8), r = ring * TILE_SIZE / 4.0;
				const double px = x + r * std::cos(a), py = y + r * std::sin(a);
				if (FirstPersonWalkable(px, py)) {
					this->walker.x = px;
					this->walker.y = py;
					return;
				}
			}
		}
		this->walker.x = x;
		this->walker.y = y;
	}

	/**
	 * A step of the walker: the arrows held while they are its window's (ArrowsWindow(): the player, 17. 10.: "the
	 * arrows always to that window, not to the map"), W, A, S and D while its window has the focus.
	 */
	void Walk(double dt)
	{
		const DirectionKeys arrows = ArrowsWindow() == this ? _dirkeys : DirectionKeys{};
		const DirectionKeys wasd = _focused_window == this && !EditBoxInGlobalFocus() ? _walkkeys : DirectionKeys{};
		if (arrows.None() && wasd.None()) return;
		if (arrows.Test(DirectionKey::Left)) this->walker.heading += FIRST_PERSON_TURN * dt;
		if (arrows.Test(DirectionKey::Right)) this->walker.heading -= FIRST_PERSON_TURN * dt;
		this->walker.heading = std::remainder(this->walker.heading, 2 * M_PI);
		double ahead = 0, aside = 0;
		if (arrows.Test(DirectionKey::Up) || wasd.Test(DirectionKey::Up)) ahead += 1;
		if (arrows.Test(DirectionKey::Down) || wasd.Test(DirectionKey::Down)) ahead -= 1;
		if (wasd.Test(DirectionKey::Right)) aside += 1;
		if (wasd.Test(DirectionKey::Left)) aside -= 1;
		if (ahead == 0 && aside == 0) return;
		const double norm = std::sqrt(ahead * ahead + aside * aside);
		const double speed = ((this->running || _shift_pressed) ? FIRST_PERSON_RUN_MS : FIRST_PERSON_WALK_MS) * OBJECT_UNITS_PER_METRE * dt / norm;
		const double fx = std::cos(this->walker.heading), fy = std::sin(this->walker.heading);
		const double rx = fy, ry = -fx;
		const double dx = (fx * ahead + rx * aside) * speed, dy = (fy * ahead + ry * aside) * speed;
		/* Along a wall, sliding: each way on its own where both together would go into it (FirstPersonFree()). */
		auto free = FirstPersonFree;
		const double x = this->walker.x, y = this->walker.y;
		if (free(x + dx, y + dy)) {
			this->walker.x = x + dx;
			this->walker.y = y + dy;
		} else if (free(x + dx, y)) {
			this->walker.x = x + dx;
		} else if (free(x, y + dy)) {
			this->walker.y = y + dy;
		}
	}

	/** How many of a thing the walker carries. */
	static uint Carried(WalkerItem item)
	{
		const Company *c = Company::GetIfValid(_local_company);
		return c != nullptr && item < WalkerItem::End ? c->walker_items[to_underlying(item)] : 0;
	}

	/**
	 * The weapon in the hand (the player, 15. 10.: "I went through the whole of Morrowind with a big hammer"; 17. 10.:
	 * the bow and the crossbow): taken out and put away with F or chosen with its key, another put away first; a
	 * hammer or a sword swung with the left button or Space, the bow drawn while the button is held and shot as it is
	 * let go, the crossbow shot and drawn again -- as long as the walker carries it.
	 */
	void Hold(double dt)
	{
		if (Carried(_first_person_weapons[this->wanted].item) == 0) this->wanted = this->weapon;
		if (Carried(_first_person_weapons[this->weapon].item) == 0 && this->wanted == this->weapon) this->weapon_out = false;
		const bool changing = this->wanted != this->weapon;
		ThreeDHeldThing &held = this->walker.held;
		const float target = this->weapon_out && !changing ? 1.0f : 0.0f, step = static_cast<float>(dt / FIRST_PERSON_DRAW_S);
		held.raise = target > held.raise ? std::min(target, held.raise + step) : std::max(target, held.raise - step);
		if (changing && held.raise <= 0) {
			this->weapon = this->wanted;
			this->swing_time = -1;
			this->draw_time = -1;
			held.swing = 0;
		}
		const FirstPersonWeapon &w = _first_person_weapons[this->weapon];
		if (this->swing_time >= 0) {
			/* The blow lands as the weapon comes down before the eyes: on a dragon there, if one is. */
			const bool before = this->swing_time < FIRST_PERSON_BLOW_S;
			this->swing_time += dt;
			if (before && this->swing_time >= FIRST_PERSON_BLOW_S) HitThreeDDragon(this->walker.x, this->walker.y, this->walker.heading, w.reach_m, w.damage);
			held.swing = SwingAngle(this->swing_time);
			if (this->swing_time >= FIRST_PERSON_SWING_S) {
				this->swing_time = -1;
				held.swing = 0;
			}
		}
		if (this->draw_time >= 0) {
			/* The bow drawn: put away or out of arrows, let down; drawn with the button, shot as it is let go. */
			if (held.raise < 1.0f || Carried(w.ammo) == 0) {
				this->draw_time = -1;
				this->mouse_draw = false;
			} else {
				this->draw_time += dt;
				if (this->mouse_draw && !_left_button_down) this->Release();
			}
		}
		this->reload_time = std::max(0.0, this->reload_time - dt);
		this->kick = std::max(0.0f, this->kick - static_cast<float>(dt / FIRST_PERSON_KICK_S));
		this->Dress();
	}

	/**
	 * What the walker holds as it is drawn (DrawThreeDHeldThing()): the weapon in its state -- the bow drawn or not,
	 * the crossbow drawn or shot -- held by its grip ("uchyt"), the arrow on the string or the bolt in the groove at
	 * its nock ("zarez_<stav>") while there is one to shoot.
	 */
	void Dress()
	{
		ThreeDHeldThing &held = this->walker.held;
		const FirstPersonWeapon &w = _first_person_weapons[this->weapon];
		if (held.raise <= 0) {
			held.thing.clear();
			held.loaded.clear();
			return;
		}
		std::string_view state;
		switch (w.use) {
			case WeaponUse::Swing: held.hold = ThreeDHold::Swung; break;
			case WeaponUse::Bow: held.hold = ThreeDHold::Bow; state = this->draw_time >= FIRST_PERSON_BOW_DRAW_S * 0.4 ? "napnuto" : "volno"; break;
			case WeaponUse::Crossbow: held.hold = ThreeDHold::Crossbow; state = this->reload_time > 0 ? "vystreleno" : "natazeno"; break;
		}
		held.thing = state.empty() ? std::string(w.thing) : fmt::format("{}/{}", w.thing, state);
		const auto grip = GetThreeDThingPlace(w.thing, "uchyt");
		const std::array<float, 3> at = grip.value_or(w.use == WeaponUse::Swing ? std::array<float, 3>{0, 0, 0.22f} : std::array<float, 3>{0, 0, 0});
		std::copy(at.begin(), at.end(), held.grip);
		held.loaded.clear();
		if (!w.missile.empty() && this->reload_time <= 0 && Carried(w.ammo) > 0) {
			auto nock = GetThreeDThingPlace(w.thing, fmt::format("zarez_{}", state));
			if (!nock.has_value()) nock = GetThreeDThingPlace(w.thing, "zarez");
			if (nock.has_value()) {
				held.loaded = std::string(w.missile);
				std::copy(nock->begin(), nock->end(), held.nock);
			}
		}
		held.kick = this->kick;
		held.drawn = this->draw_time >= 0 ? static_cast<float>(std::min(1.0, this->draw_time / FIRST_PERSON_BOW_DRAW_S)) : 0.0f;
		held.twist = w.twist;
	}

	/**
	 * The left button or Space: a swing of the hammer or the sword in the hand; the bow drawn (with the button: shot as
	 * it is let go; Space again shoots); the crossbow shot.
	 * @param mouse with the left button
	 */
	void Attack(bool mouse)
	{
		if (this->Riding() != nullptr || !this->weapon_out || this->wanted != this->weapon || this->walker.held.raise < 1.0f) return;
		const FirstPersonWeapon &w = _first_person_weapons[this->weapon];
		switch (w.use) {
			case WeaponUse::Swing:
				if (this->swing_time < 0) this->swing_time = 0;
				break;
			case WeaponUse::Bow:
				if (this->draw_time >= 0) {
					if (!mouse) this->Release();
					break;
				}
				if (this->reload_time > 0 || Carried(w.ammo) == 0) break;
				this->draw_time = 0;
				this->mouse_draw = mouse;
				if (auto sound = GetThreeDThingSound(w.thing, "napnuti"); sound.has_value()) SndPlayFx(*sound);
				break;
			case WeaponUse::Crossbow:
				if (this->reload_time > 0 || Carried(w.ammo) == 0) break;
				this->Shoot(w, 1.0);
				this->reload_time = FIRST_PERSON_RELOAD_S;
				break;
		}
		this->Dress();
	}

	/** The bow let go: the arrow flies as fast as the bow was drawn, the next one set on the string a moment after. */
	void Release()
	{
		const double drawn = std::clamp(this->draw_time / FIRST_PERSON_BOW_DRAW_S, 0.0, 1.0);
		this->draw_time = -1;
		this->mouse_draw = false;
		this->Shoot(_first_person_weapons[this->weapon], 0.25 + 0.75 * drawn);
		this->reload_time = FIRST_PERSON_NOCK_S;
	}

	/**
	 * An arrow or a bolt shot where the walker looks (the dot in the middle), from just below its eyes: one less of it
	 * in the inventory (the command), flying along its arc (Fly()).
	 * @param power how fast, of the weapon's speed
	 */
	void Shoot(const FirstPersonWeapon &w, double power)
	{
		if (Carried(w.ammo) == 0) return;
		Command<Commands::ShootWalkerItem>::Post(w.ammo);
		const double h = this->walker.heading, p = this->walker.pitch;
		const double fx = std::cos(h) * std::cos(p), fy = std::sin(h) * std::cos(p), fz = std::sin(p);
		const double speed = w.speed_ms * power;
		FirstPersonMissile m;
		m.thing = w.missile;
		m.damage = w.damage;
		const ThreeDVehicleObject *fig = GetThreeDFigure(w.missile);
		const ThreeDVehicleShape *shape = fig != nullptr ? fig->GetShape(0) : nullptr;
		if (shape != nullptr) m.length_m = shape->x1;
		m.x = this->walker.x + fx * 0.3 * OBJECT_UNITS_PER_METRE;
		m.y = this->walker.y + fy * 0.3 * OBJECT_UNITS_PER_METRE;
		m.z = FirstPersonGroundZ(this->walker.x, this->walker.y) + this->walker.eye_z + (fz * 0.3 - 0.12) * OBJECT_HEIGHT_PER_METRE;
		m.vx = fx * speed;
		m.vy = fy * speed;
		m.vz = fz * speed;
		m.facing = static_cast<float>(h * 180.0 / M_PI);
		m.pitch = static_cast<float>(p * 180.0 / M_PI);
		this->missiles.push_back(m);
		if (this->missiles.size() > FIRST_PERSON_MISSILES) this->missiles.erase(this->missiles.begin());
		this->kick = 1.0f;
		if (auto sound = GetThreeDThingSound(w.thing, "vystrel"); sound.has_value()) SndPlayFx(*sound);
	}

	/**
	 * The arrows and the bolts on: each along its arc, falling, turned along its way, in steps of half a metre so that
	 * none passes through a dragon; its tip in a dragon (the dragon takes it, HitThreeDDragonAt()), in the water or off
	 * the map, it is gone; in the ground or a wall, it stays stuck a while.
	 */
	void Fly(double dt)
	{
		for (FirstPersonMissile &m : this->missiles) {
			m.age += dt;
			if (m.stuck || m.gone) continue;
			const int n = std::max(1, static_cast<int>(std::ceil(std::sqrt(m.vx * m.vx + m.vy * m.vy + m.vz * m.vz) * dt / 0.5)));
			const double sub = dt / n;
			/* How high its tip was over the ground before the step (where it went in, when it goes in). */
			double above = -1;
			for (int k = 0; k < n && !m.stuck && !m.gone; k++) {
				const double px = m.x, py = m.y, pz = m.z;
				m.vz -= FIRST_PERSON_GRAVITY * sub;
				m.x += m.vx * sub * OBJECT_UNITS_PER_METRE;
				m.y += m.vy * sub * OBJECT_UNITS_PER_METRE;
				m.z += m.vz * sub * OBJECT_HEIGHT_PER_METRE;
				const double level = std::hypot(m.vx, m.vy), all = std::hypot(level, m.vz);
				const double ux = m.vx / all, uy = m.vy / all, uz = m.vz / all;
				m.facing = static_cast<float>(std::atan2(m.vy, m.vx) * 180.0 / M_PI);
				m.pitch = static_cast<float>(std::atan2(m.vz, level) * 180.0 / M_PI);
				const double tx = m.x + ux * m.length_m * OBJECT_UNITS_PER_METRE, ty = m.y + uy * m.length_m * OBJECT_UNITS_PER_METRE;
				const double tz = m.z + uz * m.length_m * OBJECT_HEIGHT_PER_METRE;
				if (tx < TILE_SIZE || ty < TILE_SIZE || tx >= (Map::SizeX() - 1) * TILE_SIZE || ty >= (Map::SizeY() - 1) * TILE_SIZE) {
					m.gone = true;
					continue;
				}
				if (HitThreeDDragonAt(tx, ty, tz, m.damage)) {
					m.gone = true;
					continue;
				}
				const double ground = FirstPersonGroundZ(tx, ty);
				const TileIndex tile = TileVirtXY(static_cast<int>(tx), static_cast<int>(ty));
				if (tz <= ground) {
					if (IsTileType(tile, TileType::Water) && !IsCoast(tile)) {
						m.gone = true;
						continue;
					}
					/* Into the ground where its tip met it, and a few centimetres in -- not as far as the step went. */
					const double t = above > 0 ? above / (above + (ground - tz)) : 0.0;
					m.x = px + (m.x - px) * t + ux * 0.06 * OBJECT_UNITS_PER_METRE;
					m.y = py + (m.y - py) * t + uy * 0.06 * OBJECT_UNITS_PER_METRE;
					m.z = pz + (m.z - pz) * t + uz * 0.06 * OBJECT_HEIGHT_PER_METRE;
					m.stuck = true;
				} else if (tz - ground < 12 * OBJECT_HEIGHT_PER_METRE && !FirstPersonWalkable(tx, ty) && !IsTileType(tile, TileType::Water)) {
					/* A house or a building as high as it is: stuck in its wall, where it was before the step. */
					m.x = px;
					m.y = py;
					m.z = pz;
					m.stuck = true;
				}
				above = tz - ground;
			}
			if (m.stuck) m.age = 0;
		}
		std::erase_if(this->missiles, [](const FirstPersonMissile &m) {
			return m.gone || m.age > (m.stuck ? FIRST_PERSON_STUCK_S : FIRST_PERSON_FLY_S);
		});
	}

	/** A weapon by its key, out in the hand (another put away first), when the walker carries it. */
	void Choose(int k)
	{
		if (k < 0 || k >= static_cast<int>(std::size(_first_person_weapons)) || Carried(_first_person_weapons[k].item) == 0) return;
		this->wanted = k;
		this->weapon_out = true;
	}

	EventState OnKeyPress(char32_t key, uint16_t keycode) override
	{
		/* The walking keys are the walker's, not the toolbar's (they are read as held, Walk()). */
		switch (keycode & ~WKC_SPECIAL_KEYS) {
			case 'W': case 'A': case 'S': case 'D':
			case WKC_UP: case WKC_DOWN: case WKC_LEFT: case WKC_RIGHT:
				return EventState::Handled;
			default:
				break;
		}
		if (key == 'w' || key == 'a' || key == 's' || key == 'd' || key == 'W' || key == 'A' || key == 'S' || key == 'D') return EventState::Handled;
		/* E: into the vehicle near, or out of it (as using a thing in Morrowind). */
		if (key == 'e' || key == 'E') {
			this->BoardOrLeave();
			return EventState::Handled;
		}
		/* In a vehicle, as in its cab: I the horn, F the lights, T its route or the wheel again. */
		if (const Vehicle *rv = this->Riding(); rv != nullptr) {
			if (key == 'i' || key == 'I') {
				CabHorn(rv);
				return EventState::Handled;
			}
			if (key == 'f' || key == 'F') {
				CabToggleLights(rv);
				return EventState::Handled;
			}
			if (key == 't' || key == 'T') {
				CabToggleRoute(rv, this->arrows);
				this->SetWidgetDirty(WID_FP_PANEL);
				return EventState::Handled;
			}
		}
		/* I: the inventory (as in Morrowind). */
		if (key == 'i' || key == 'I') {
			ShowWalkerInventoryWindow();
			return EventState::Handled;
		}
		/* 1 to 5: a weapon (the hammer, the bow, the crossbow, the sword, the two-handed sword); F: it out or away (as
		 * the weapon in Morrowind); Space: a swing of it, a shot. */
		if (key >= '1' && key <= '5') {
			this->Choose(static_cast<int>(key - '1'));
			return EventState::Handled;
		}
		if (key == 'f' || key == 'F') {
			this->weapon_out = !this->weapon_out;
			return EventState::Handled;
		}
		if (key == ' ' || (keycode & ~WKC_SPECIAL_KEYS) == WKC_SPACE) {
			this->Attack(false);
			return EventState::Handled;
		}
		return EventState::NotHandled;
	}

	bool OnRightClick([[maybe_unused]] Point pt, WidgetID widget) override
	{
		/* Looking about with the right mouse button held, as the map is dragged (OnScroll()). */
		if (widget != WID_FP_CANVAS || _scrolling_viewport) return false;
		_scrolling_viewport = true;
		return true;
	}

	void OnScroll(Point delta) override
	{
		this->walker.heading = std::remainder(this->walker.heading + delta.x * FIRST_PERSON_MOUSE, 2 * M_PI);
		this->walker.pitch = std::clamp(this->walker.pitch + delta.y * FIRST_PERSON_MOUSE, -FIRST_PERSON_PITCH_MAX, FIRST_PERSON_PITCH_MAX);
		this->SetWidgetDirty(WID_FP_CANVAS);
	}

	/** The walker's health changed (a dragon's blow, CmdHurtWalker()): with none left, back to the map at once. */
	void OnInvalidateData([[maybe_unused]] int data = 0, bool gui_scope = true) override
	{
		if (!gui_scope) return;
		const Company *c = Company::GetIfValid(_local_company);
		if (c != nullptr && c->walker_health == 0) this->Close();
	}

	void OnPaint() override
	{
		this->SetWidgetLoweredState(WID_FP_RUN, this->running);
		this->DrawWidgets();
	}

	/** The picture onto the screen, within the clipping of the moment. */
	static void BlitPicture(const CabViewPicture &pic, int px, int py)
	{
		const DrawPixelInfo *dpi = _cur_dpi;
		Blitter *blitter = BlitterFactory::GetCurrentBlitter();
		int x0 = std::max(px, dpi->left), x1 = std::min(px + pic.width, dpi->left + dpi->width);
		int y0 = std::max(py, dpi->top), y1 = std::min(py + pic.height, dpi->top + dpi->height);
		for (int y = y0; y < y1; y++) {
			const uint32_t *src = &pic.pixels[static_cast<size_t>(y - py) * pic.width];
			uint32_t *dst = static_cast<uint32_t *>(blitter->MoveTo(dpi->dst_ptr, x0 - dpi->left, y - dpi->top));
			for (int x = x0; x < x1; x++, dst++) {
				Colour sc(src[x - px]);
				*dst = Colour(sc.r, sc.g, sc.b).data;
			}
		}
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		switch (widget) {
			case WID_FP_CANVAS: {
				int w = r.Width(), h = r.Height();
				if (w <= 0 || h <= 0) return;
				/* Made by the step; anew here only when the window changed its size since. */
				if (this->picture.width != w || this->picture.height != h) this->MakePicture();
				if (!this->picture_ok) {
					GfxFillRect(r, GetColourGradient(Colours::Grey, Shade::Darkest));
					DrawString(r.left, r.right, r.top + h / 2, STR_FIRST_PERSON_NEEDS_32BPP, TextColour::White, AlignmentH::Centre);
					return;
				}
				BlitPicture(this->picture, r.left, r.top);
				/* A dot in the middle, where it looks. */
				const int cx = r.left + w / 2, cy = r.top + h / 2;
				GfxFillRect(cx - 1, cy - 1, cx + 1, cy + 1, GetColourGradient(Colours::White, Shade::Lightest));
				/* The weapon out, and how many arrows or bolts there are for it. */
				if (this->weapon_out && this->Riding() == nullptr) {
					const FirstPersonWeapon &wp = _first_person_weapons[this->weapon];
					const StringID name = GetWalkerItemSpec(wp.item).name;
					const std::string text = wp.ammo != WalkerItem::End ? GetString(STR_FIRST_PERSON_WEAPON_AMMO, name, GetWalkerItemSpec(wp.ammo).name, Carried(wp.ammo)) : GetString(name);
					DrawString(r.left, r.right - WidgetDimensions::scaled.framerect.right * 2, r.bottom - GetCharacterHeight(FontSize::Normal) - WidgetDimensions::scaled.framerect.bottom * 2, text, TextColour::White, AlignmentH::End);
				}
				break;
			}
			case WID_FP_PANEL: {
				GfxFillRect(r, GetColourGradient(Colours::Grey, Shade::Darkest));
				const Rect ir = r.Shrink(WidgetDimensions::scaled.framerect);
				const int second = ir.top + GetCharacterHeight(FontSize::Normal) + 2;
				/* In a vehicle: which, how fast, and the keys of its cab. */
				if (const Vehicle *rv = this->Riding(); rv != nullptr) {
					DrawString(ir.left, ir.right, ir.top, GetString(this->arrows.route || rv->owner != _local_company ? STR_FIRST_PERSON_RIDING_ROUTE : STR_FIRST_PERSON_RIDING, rv->index, PackVelocity(rv->GetDisplaySpeed(), rv->type)), TextColour::White);
					DrawStringMultiLine(ir.left, ir.right, second, ir.bottom, rv->owner == _local_company ? STR_FIRST_PERSON_RIDING_KEYS : STR_FIRST_PERSON_PASSENGER_KEYS, TextColour::Silver);
					break;
				}
				const TileIndex tile = TileVirtXY(static_cast<int>(this->walker.x), static_cast<int>(this->walker.y));
				const Town *town = ClosestTownFromTile(tile, UINT_MAX);
				const int deg = static_cast<int>(std::lround(std::fmod(this->walker.heading * 180.0 / M_PI + 360.0, 360.0)));
				const Company *c = Company::GetIfValid(_local_company);
				if (town != nullptr && c != nullptr) {
					DrawString(ir.left, ir.right, ir.top, GetString(STR_FIRST_PERSON_WHERE_HEALTH, c->walker_health, town->index, TileX(tile), TileY(tile), deg), TextColour::White);
				} else if (town != nullptr) {
					DrawString(ir.left, ir.right, ir.top, GetString(STR_FIRST_PERSON_WHERE, town->index, TileX(tile), TileY(tile), deg), TextColour::White);
				}
				/* Near a place that sells things: said instead of the keys. */
				const StringID shop = WalkerShopName(FirstPersonShopNearby());
				const Vehicle *near = this->VehicleToBoard();
				if (shop != INVALID_STRING_ID) {
					DrawString(ir.left, ir.right, second, GetString(STR_FIRST_PERSON_SHOP_HERE, shop), TextColour::Yellow);
				} else if (near != nullptr) {
					/* Beside a vehicle: E gets into it. */
					DrawString(ir.left, ir.right, second, GetString(STR_FIRST_PERSON_BOARD_HERE, near->index), TextColour::Yellow);
				} else {
					DrawStringMultiLine(ir.left, ir.right, second, ir.bottom, STR_FIRST_PERSON_KEYS, TextColour::Silver);
				}
				break;
			}
			default:
				break;
		}
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_FP_CANVAS:
				this->Attack(true);
				break;
			case WID_FP_HERE:
				/* Out of a vehicle first. */
				if (this->Riding() != nullptr) this->GetOut();
				this->riding = VehicleID::Invalid();
				this->PutWhereTheMainViewLooks();
				this->SetDirty();
				break;
			case WID_FP_CENTRE:
				/* The map shows again: the view a window. */
				if (this->full) this->ToggleFull();
				ScrollMainWindowTo(static_cast<int>(this->walker.x), static_cast<int>(this->walker.y), static_cast<int>(FirstPersonGroundZ(this->walker.x, this->walker.y)));
				break;
			case WID_FP_RUN:
				this->running = !this->running;
				this->SetDirty();
				break;
			case WID_FP_INVENTORY:
				ShowWalkerInventoryWindow();
				break;
			case WID_FP_FULL:
				this->ToggleFull();
				break;
			default:
				break;
		}
	}
};

static WindowDesc _first_person_desc(
	WindowPosition::Automatic, {}, 0, 0,
	WindowClass::FirstPerson, WindowClass::None,
	{},
	_nested_first_person_widgets
);

/** Open the view from the first person, or bring it to the front; the walker stands where the main view looks. */
void ShowFirstPersonWindow()
{
	/* After the walker had no health left, it comes back whole. */
	const Company *c = Company::GetIfValid(_local_company);
	if (c != nullptr && c->walker_health == 0) Command<Commands::ReviveWalker>::Post(TileIndex{}, WALKER_HEALTH_MAX);
	AllocateWindowDescFront<FirstPersonWindow>(_first_person_desc, 0);
}

/**
 * Where things are sold near the walker of the view from the first person (walker_inventory.h): the nearest tile
 * of a pub, a vending machine, a coffeeshop or a shop of a town, within a tile of where it stands.
 * @return the tile, or INVALID_TILE when there is no walker or nothing is sold near it
 */
TileIndex FirstPersonShopNearby()
{
	const FirstPersonWindow *w = static_cast<const FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	if (w == nullptr) return INVALID_TILE;
	TileIndex best = INVALID_TILE;
	double best_d = 1e18;
	const int tx = static_cast<int>(w->walker.x) / static_cast<int>(TILE_SIZE), ty = static_cast<int>(w->walker.y) / static_cast<int>(TILE_SIZE);
	for (int dy = -1; dy <= 1; dy++) {
		for (int dx = -1; dx <= 1; dx++) {
			const int x = tx + dx, y = ty + dy;
			if (x < 0 || y < 0 || x >= static_cast<int>(Map::SizeX()) || y >= static_cast<int>(Map::SizeY())) continue;
			const TileIndex tile = TileXY(x, y);
			if (WalkerShopName(tile) == INVALID_STRING_ID) continue;
			const double cx = (x + 0.5) * TILE_SIZE - w->walker.x, cy = (y + 0.5) * TILE_SIZE - w->walker.y;
			if (cx * cx + cy * cy < best_d) {
				best_d = cx * cx + cy * cy;
				best = tile;
			}
		}
	}
	return best;
}

/** Whether the view from the first person is open over the whole screen: the map under it is then not drawn (widget.cpp). */
bool FirstPersonCoversMap()
{
	const FirstPersonWindow *w = static_cast<const FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	return w != nullptr && w->full;
}

/** The walker of the view from the first person: where it stands, world units; false when the view is not open (three_d_dragons.h). */
bool FirstPersonWalkerAt(double &x, double &y)
{
	const FirstPersonWindow *w = static_cast<const FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	if (w == nullptr) return false;
	x = w->walker.x;
	y = w->walker.y;
	return true;
}

/** The rig: the direction the walker of the view from the first person faces, radians (0 without the window). */
double FirstPersonWalkerHeading()
{
	const FirstPersonWindow *w = static_cast<const FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	return w != nullptr ? w->walker.heading : 0.0;
}

/** The tile the walker of the view from the first person stands on, or INVALID_TILE without the window. */
TileIndex FirstPersonWalkerTile()
{
	const FirstPersonWindow *w = static_cast<const FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	if (w == nullptr) return INVALID_TILE;
	const int x = static_cast<int>(w->walker.x) / static_cast<int>(TILE_SIZE), y = static_cast<int>(w->walker.y) / static_cast<int>(TILE_SIZE);
	if (x < 0 || y < 0 || x >= static_cast<int>(Map::SizeX()) || y >= static_cast<int>(Map::SizeY())) return INVALID_TILE;
	return TileXY(x, y);
}

/** The rig: the walker of the view from the first person, where it is and how long its last picture took. */
std::string DescribeFirstPerson()
{
	const FirstPersonWindow *w = static_cast<const FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	if (w == nullptr) return "zadny chodec";
	return fmt::format("chodec na {:.1f},{:.1f} (dlazdice {},{}), smer {:.0f}, pohled {:.0f}, {}x{} px, {:.1f} ms, {} dlazdic, {} plakatu",
			w->walker.x, w->walker.y, static_cast<int>(w->walker.x) / TILE_SIZE, static_cast<int>(w->walker.y) / TILE_SIZE,
			w->walker.heading * 180.0 / M_PI, w->walker.pitch * 180.0 / M_PI, w->picture.width, w->picture.height, w->camera.ms, w->camera.tiles, w->camera.billboards);
}

/**
 * The rig: the hammer in the hand at once, swung so far (degrees), or put away.
 * @return whether the walker has it out
 */
bool RigFirstPersonHold(bool out, float swing)
{
	FirstPersonWindow *w = static_cast<FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	if (w == nullptr) return false;
	w->weapon = w->wanted = 0;
	w->weapon_out = out;
	w->Hold(0);
	w->walker.held.raise = w->weapon_out ? 1.0f : 0.0f;
	w->Dress();
	w->walker.held.swing = swing;
	w->MakePicture();
	w->SetDirty();
	return w->weapon_out;
}

/**
 * The rig: a weapon by its key (1 to 5) in the hand at once; then a shot -- the bow drawn so many milliseconds and let
 * go, the crossbow shot, a sword swung (0 ms: only in the hand) -- and the arrows let fly so many steps.
 * @return what is held and where the arrows are
 */
std::string RigFirstPersonWeapon(int key, int draw_ms, int steps)
{
	FirstPersonWindow *w = static_cast<FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	if (w == nullptr) return "zadny chodec";
	w->Choose(key - 1);
	w->weapon = w->wanted;
	w->walker.held.raise = w->weapon_out ? 1.0f : 0.0f;
	w->reload_time = 0;
	w->Dress();
	const double dt = FIRST_PERSON_TICK_MS / 1000.0;
	if (draw_ms > 0) {
		w->Attack(false);
		for (int t = 0; t < draw_ms; t += FIRST_PERSON_TICK_MS) w->Hold(dt);
		if (w->draw_time >= 0 && steps >= 0) w->Attack(false);
	}
	std::string held = fmt::format("drzi {} ({}) nabito {} zarez {:.3f},{:.3f},{:.3f} uchyt {:.3f},{:.3f},{:.3f}", w->walker.held.thing, w->weapon_out ? "venku" : "schovano",
			w->walker.held.loaded.empty() ? "-" : w->walker.held.loaded, w->walker.held.nock[0], w->walker.held.nock[1], w->walker.held.nock[2], w->walker.held.grip[0], w->walker.held.grip[1], w->walker.held.grip[2]);
	for (int i = 0; i < steps; i++) {
		w->Hold(dt);
		w->Fly(dt);
	}
	for (const FirstPersonMissile &m : w->missiles) {
		held += fmt::format("; {} na {:.1f},{:.1f} vyska {:.1f} m nad zemi, smer {:.0f} sklon {:.0f}{}", m.thing, m.x, m.y, (m.z - FirstPersonGroundZ(m.x, m.y)) / OBJECT_HEIGHT_PER_METRE, m.facing, m.pitch, m.stuck ? " zabodnuty" : "");
	}
	w->MakePicture();
	w->SetDirty();
	return held;
}

/** The arrows and the bolts of the walker in the view from the eyes (AddCabMissile()). */
void AddFirstPersonMissilesToEye()
{
	const FirstPersonWindow *w = static_cast<const FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	if (w == nullptr) return;
	for (const FirstPersonMissile &m : w->missiles) AddCabMissile(m.thing, m.x, m.y, m.z, m.facing, m.pitch);
}

/** The rig: make the picture so many times on so many threads (0 every core); the milliseconds a picture took. */
double RigFirstPersonMeasure(int times, int threads)
{
	extern int _cab_threads;
	FirstPersonWindow *w = static_cast<FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	if (w == nullptr || times <= 0) return 0;
	_cab_threads = threads;
	double total = 0;
	for (int i = 0; i < times; i++) {
		w->MakePicture();
		total += w->camera.ms;
	}
	_cab_threads = 0;
	uint64_t sum = 0;
	for (uint32_t p : w->picture.pixels) sum = sum * 1000003u + p;
	Debug(misc, 0, "rig: first person picture checksum {:016x}; stages ms: tiles {:.1f}, collect {:.1f}, ground texture {:.1f}, ground {:.1f}, bridges and objects {:.1f}, billboards and figures {:.1f}, haze and night {:.1f}", sum,
			w->camera.stage[0], w->camera.stage[1], w->camera.stage[2], w->camera.stage[3], w->camera.stage[4], w->camera.stage[5], w->camera.stage[6]);
	return total / times;
}

/** The picture from the eyes made again at once, as it is drawn only as the walker steps (a setting of the night changed). */
void RedrawFirstPersonPicture()
{
	FirstPersonWindow *w = static_cast<FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	if (w == nullptr) return;
	w->MakePicture();
	w->SetDirty();
}

/**
 * The rig: the walker beside a vehicle (on foot, a few metres off its left), or E pressed (into the vehicle near, out
 * of the one it is in).
 * @return what the walker is in now, or VehicleID::Invalid() on foot
 */
VehicleID RigFirstPersonBoard(const Vehicle *beside, bool press_e)
{
	FirstPersonWindow *w = static_cast<FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	if (w == nullptr) return VehicleID::Invalid();
	if (beside != nullptr) {
		const double heading = DirectionToCabYaw(beside->direction) * 2 * M_PI / CAB_YAW_TURN, d = 3.0 * OBJECT_UNITS_PER_METRE;
		w->walker.x = beside->x_pos - std::sin(heading) * d;
		w->walker.y = beside->y_pos + std::cos(heading) * d;
	}
	if (press_e) {
		SetFocusedWindow(w);
		w->OnKeyPress('e', 'E');
	}
	w->SetDirty();
	return w->riding;
}

/**
 * The rig: the walker so many metres from a vehicle, at an angle from its front (0 before it, 90 on its left, 180
 * behind it), looking at it.
 */
void RigFirstPersonLookAt(const Vehicle *v, double metres, double angle_deg)
{
	FirstPersonWindow *w = static_cast<FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	if (w == nullptr || v == nullptr) return;
	const int yaw = v->cab_free ? v->cab_yaw : DirectionToCabYaw(v->direction);
	const double a = yaw * 2 * M_PI / CAB_YAW_TURN + angle_deg * M_PI / 180.0, d = metres * OBJECT_UNITS_PER_METRE;
	w->walker.x = v->x_pos + std::cos(a) * d;
	w->walker.y = v->y_pos + std::sin(a) * d;
	w->walker.heading = std::atan2(v->y_pos - w->walker.y, v->x_pos - w->walker.x);
	w->walker.pitch = 0;
	w->MakePicture();
	w->SetDirty();
}

/** The rig: walk the walker as if the keys were held so many steps (ahead, aside, turn: -1, 0 or 1). */
void RigFirstPersonWalk(int steps, int ahead, int aside, int turn, double pitch)
{
	FirstPersonWindow *w = static_cast<FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	if (w == nullptr) return;
	w->walker.pitch = std::clamp(pitch, -FIRST_PERSON_PITCH_MAX, FIRST_PERSON_PITCH_MAX);
	for (int i = 0; i < steps; i++) {
		w->walker.heading = std::remainder(w->walker.heading + turn * FIRST_PERSON_TURN * FIRST_PERSON_TICK_MS / 1000.0, 2 * M_PI);
		if (ahead == 0 && aside == 0) continue;
		const double speed = FIRST_PERSON_WALK_MS * OBJECT_UNITS_PER_METRE * FIRST_PERSON_TICK_MS / 1000.0;
		const double fx = std::cos(w->walker.heading), fy = std::sin(w->walker.heading);
		const double nx = w->walker.x + (fx * ahead + fy * aside) * speed, ny = w->walker.y + (fy * ahead - fx * aside) * speed;
		if (FirstPersonWalkable(nx, ny)) {
			w->walker.x = nx;
			w->walker.y = ny;
		}
	}
	w->MakePicture();
	w->SetDirty();
}

/**
 * The rig: W held so many frames of the game, each so many milliseconds long, the steps counted as the window's timer
 * counts them (IntervalTimer<TimerWindow>::Elapsed()); the metres walked.
 */
double RigFirstPersonHoldW(int frame_ms, int frames)
{
	FirstPersonWindow *w = static_cast<FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	if (w == nullptr || frame_ms <= 0) return 0;
	SetFocusedWindow(w);
	const double x = w->walker.x, y = w->walker.y;
	_walkkeys.Set(DirectionKey::Up);
	int elapsed = 0;
	for (int i = 0; i < frames; i++) {
		elapsed += frame_ms;
		const uint count = elapsed / FIRST_PERSON_TICK_MS;
		elapsed %= FIRST_PERSON_TICK_MS;
		if (count > 0) w->Step(count);
	}
	_walkkeys.Reset(DirectionKey::Up);
	return std::hypot(w->walker.x - x, w->walker.y - y) / OBJECT_UNITS_PER_METRE;
}
