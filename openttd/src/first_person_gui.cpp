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
/** How long taking the hammer out or putting it away takes, and a swing of it, seconds. */
static constexpr double FIRST_PERSON_DRAW_S = 0.35;
static constexpr double FIRST_PERSON_SWING_S = 0.6;
/** When in the swing the hammer strikes, seconds, and how far it reaches, metres. */
static constexpr double FIRST_PERSON_BLOW_S = 0.27;
static constexpr double FIRST_PERSON_REACH_M = 2.5;
/** The thing of the indices the walker holds ("predmet", objekty/predmety/: the colleague's hammer). */
static constexpr std::string_view FIRST_PERSON_HAMMER = "kladivo";

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
		case TileType::House:
		case TileType::Object:
			return false;
		case TileType::Water:
			return IsCoast(tile);
		case TileType::Industry: {
			const IndustryGfx gfx = GetIndustryGfx(tile);
			return gfx == GFX_MARIJUANA_PLANTATION || gfx == GFX_POTATO_FIELD;
		}
		case TileType::Station:
			return HasStationRail(tile) || IsAnyRoadStop(tile);
		default:
			return true;
	}
}

void ShowWalkerInventoryWindow();
TileIndex FirstPersonShopNearby();

/** The window of the view from the first person; there is one, number 0. */
struct FirstPersonWindow : Window {
	FirstPersonEye walker{};             ///< where the walker is and looks
	mutable CabViewCamera camera{};      ///< the eye between frames
	mutable CabViewPicture picture{};    ///< the last picture
	mutable bool picture_ok = true;       ///< whether the last picture could be made (32 bpp)
	bool full = false;                    ///< over the whole screen, between the toolbar and the status bar
	Rect before_full{};                   ///< where it was before that
	bool running = false;                 ///< runs instead of walking (also while Shift is held)
	bool hammer_out = false;              ///< the hammer taken out (F), in the hand
	double swing_time = -1;               ///< seconds into a swing of the hammer, -1 when it is not swung
	int hurt_steps = 0;                   ///< steps the picture is still red of a dragon's blow

	/*
	 * Moves while the keys are held and makes the picture anew, twenty-five times a second -- only here: a paint of
	 * the window (the map under it marks the screen dirty as its vehicles move, in several pieces a frame) puts the
	 * picture made on the screen and nothing more.
	 */
	IntervalTimer<TimerWindow> step{std::chrono::milliseconds(FIRST_PERSON_TICK_MS), [this](auto) {
		this->Walk(FIRST_PERSON_TICK_MS / 1000.0);
		this->Hold(FIRST_PERSON_TICK_MS / 1000.0);
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
		if (this->hurt_steps > 0) this->hurt_steps--;
		this->SetWidgetDirty(WID_FP_CANVAS);
		this->SetWidgetDirty(WID_FP_PANEL);
	}};

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

	/** The picture from the walker's eyes, as big as the canvas is now. */
	void MakePicture() const
	{
		const NWidgetBase *canvas = this->GetWidget<NWidgetBase>(WID_FP_CANVAS);
		const int w = canvas->current_x, h = canvas->current_y;
		if (w <= 0 || h <= 0) return;
		this->picture_ok = DrawFirstPersonView(this->walker, w, h, this->camera, this->picture);
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

	/** A step of the walker: the keys held while its window has the focus. */
	void Walk(double dt)
	{
		if (_focused_window != this) return;
		const DirectionKeys arrows = _dirkeys, wasd = _walkkeys;
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
		/* Along a wall, sliding: each way on its own where both together would go into it. */
		auto free = [](double x, double y) {
			return FirstPersonWalkable(x + FIRST_PERSON_RADIUS, y) && FirstPersonWalkable(x - FIRST_PERSON_RADIUS, y) &&
					FirstPersonWalkable(x, y + FIRST_PERSON_RADIUS) && FirstPersonWalkable(x, y - FIRST_PERSON_RADIUS);
		};
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

	/**
	 * The hammer in the hand (the player, 15. 10.: "I went through the whole of Morrowind with a big hammer"): taken
	 * out and put away with F, swung with the left button or Space -- as long as the walker carries one.
	 */
	void Hold(double dt)
	{
		const Company *c = Company::GetIfValid(_local_company);
		if (c == nullptr || c->walker_items[to_underlying(WalkerItem::Hammer)] == 0) this->hammer_out = false;
		const float target = this->hammer_out ? 1.0f : 0.0f, step = static_cast<float>(dt / FIRST_PERSON_DRAW_S);
		float &raise = this->walker.raise;
		raise = target > raise ? std::min(target, raise + step) : std::max(target, raise - step);
		this->walker.held = raise > 0 ? std::string(FIRST_PERSON_HAMMER) : std::string();
		if (this->swing_time >= 0) {
			/* The blow lands as the hammer comes down before the eyes: on a dragon there, if one is. */
			const bool before = this->swing_time < FIRST_PERSON_BLOW_S;
			this->swing_time += dt;
			if (before && this->swing_time >= FIRST_PERSON_BLOW_S) HitThreeDDragon(this->walker.x, this->walker.y, this->walker.heading, FIRST_PERSON_REACH_M);
			this->walker.swing = SwingAngle(this->swing_time);
			if (this->swing_time >= FIRST_PERSON_SWING_S) {
				this->swing_time = -1;
				this->walker.swing = 0;
			}
		}
	}

	/** A swing of the hammer, when it is in the hand and not swung already. */
	void Swing()
	{
		if (!this->hammer_out || this->walker.raise < 1.0f || this->swing_time >= 0) return;
		this->swing_time = 0;
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
		/* I: the inventory (as in Morrowind). */
		if (key == 'i' || key == 'I') {
			ShowWalkerInventoryWindow();
			return EventState::Handled;
		}
		/* F: the hammer out or away (as the weapon in Morrowind); Space: a swing of it. */
		if (key == 'f' || key == 'F') {
			this->hammer_out = !this->hammer_out;
			return EventState::Handled;
		}
		if (key == ' ' || (keycode & ~WKC_SPECIAL_KEYS) == WKC_SPACE) {
			this->Swing();
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
				break;
			}
			case WID_FP_PANEL: {
				GfxFillRect(r, GetColourGradient(Colours::Grey, Shade::Darkest));
				const Rect ir = r.Shrink(WidgetDimensions::scaled.framerect);
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
				if (shop != INVALID_STRING_ID) {
					DrawString(ir.left, ir.right, ir.top + GetCharacterHeight(FontSize::Normal) + 2, GetString(STR_FIRST_PERSON_SHOP_HERE, shop), TextColour::Yellow);
				} else {
					DrawStringMultiLine(ir.left, ir.right, ir.top + GetCharacterHeight(FontSize::Normal) + 2, ir.bottom, STR_FIRST_PERSON_KEYS, TextColour::Silver);
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
				this->Swing();
				break;
			case WID_FP_HERE:
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
	w->hammer_out = out;
	w->Hold(0);
	w->walker.raise = w->hammer_out ? 1.0f : 0.0f;
	w->walker.held = w->hammer_out ? std::string(FIRST_PERSON_HAMMER) : std::string();
	w->walker.swing = swing;
	w->MakePicture();
	w->SetDirty();
	return w->hammer_out;
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
	Debug(misc, 0, "rig: first person picture checksum {:016x}", sum);
	return total / times;
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
