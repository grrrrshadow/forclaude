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
#include "three_d_gpu.h"
#include "three_d.h"
#include "engine_base.h"
#include "cargotype.h"
#include "vehicle_base.h"
#include "vehicle_func.h"
#include "train.h"
#include "spritecache.h"
#include "zoom_func.h"
#include "landscape.h"
#include "viewport_func.h"
#include "direction_func.h"
#include "round_track.h"
#include "climate_industries.h"
#include "debug.h"
#include "settings_type.h"
#include "newgrf/newgrf_internal.h"
#include "vehicle_config.h"
#include "true_colour.h"
#include "roadveh.h"
#include "timer/timer_game_calendar.h"
#include "timer/timer_game_tick.h"
#include "timer/timer.h"
#include "timer/timer_window.h"
#include "cab_view.h"
#include "thread.h"
#include "window_func.h"
#include "window_gui.h"

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

#include "fileio_func.h"

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
/** The pictures of a turn of a thing that turns (ThreeDVehicleObject::spin_ticks, the spit of the pub). */
static constexpr int SPIN_STEPS = 24;
/** The pictures of the turned view kept: the vehicles ask for the same few over and over. */
static constexpr size_t PICTURES_KEPT = 1024;
/**
 * And no more memory than this for them: a figure close up is a big picture, and one moving is a new picture each frame
 * and each angle -- a flying dragon (120 frames, three times its size on the map) zoomed 16 times in, some 10 MB a
 * picture, took a phone's memory in seconds (the player, 24. 10.: "I zoomed in on the dragon over the sea and it went
 * down", the music of another app and the browser killed with it).
 */
static constexpr size_t PICTURE_BYTES_KEPT = 128u * 1024 * 1024;

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
	int fade = 0;                             ///< its state (StateStep())
	uint64_t walk = 0;                        ///< the frames of its crew that walks (WalkCode())
	int bounds_of = 0;                        ///< what its bounds before it is made were worked out of (ThreeDVehicleObjectSpriteBounds()): 0 not yet, 1 a guess, 2 its object's extent
	uint32_t extent_version = 0;              ///< with 2, of which extent of the object (ThreeDVehicleObject::KnownExtent()): it grows as parts are added
	Rect bounds{};
	bool gui = false;                         ///< for the windows (the purchase list, the news): made at the zoom of the interface (ThreeDVehicleObjectSpriteStandIn())
	uint32_t where = 0;                       ///< the piece and the place along of its key in _object_sprites (ObjectSprite())
};
static std::vector<ObjectSpriteRequest> _object_sprite_requests;
/** The sprites made: by the object, the piece it is hung from (and whether for the windows), the direction, its scale (thousandths), its state (StateStep()) and the frames of its crew that walks. */
static std::map<std::tuple<const ThreeDVehicleObject *, int, int, int, int, uint64_t>, SpriteID> _object_sprites;
static SpriteID _object_empty_sprite = 0;

/** A picture of an object in the turned view, kept. */
struct ObjectPictureEntry {
	uint64_t used;
	ThreeDModelPicture picture;
};
/** What a picture is of (ObjectPicture()): the object, the zoom, the angles seen and stood at, the size, the state (and the lean), the frame (and the spin, tint, lights), the stretch, the foreshortening, the crew's frames. */
using ObjectPictureKey = std::tuple<const ThreeDVehicleObject *, int, int, int, int, int, int, int, int, uint64_t>;
static std::map<ObjectPictureKey, ObjectPictureEntry> _object_pictures;
static uint64_t _object_pictures_clock = 0;

static void ForgetEyePainted();
static void ForgetGuiSpritesAndPreviews();

/** Forget the sprites made: the sprite cache they were in is gone (GfxInitSpriteMem()). */
void ResetThreeDVehicleObjectSprites()
{
	/* The thread making the pictures first, and the frame of the view from the eyes being drawn: the objects they draw from go. */
	StopThreeDPictureThread();
	WaitEyeFrame();
	_object_sprite_requests.clear();
	_object_sprites.clear();
	_object_empty_sprite = 0;
	_object_pictures.clear();
	ForgetEyePainted();
	ForgetGuiSpritesAndPreviews();
	/* The GRFs are loaded anew: their packages read again the next time they are looked at, one put into the NewGRF
	 * folders meanwhile among them (three_d_objects.h). */
	ResetThreeDVehicleObjects();
}

/** Whether a vehicle of the type is foreshortened along itself on straight track, as the sets draw trains and road vehicles. */
static inline bool SqueezedType(VehicleType type)
{
	return type == VehicleType::Train || type == VehicleType::Road;
}

/**
 * The scale an object is drawn at: what its set's size parameter makes ("velikost": the player sets it, all the 3D
 * sets at once, at the bottom of the active list of the NewGRF window), else the size of the game's setting -- the
 * original game's, as the bigger sets draw (BRYLE), 1.2, or CZTR's, 1 (the player, 14. 10.: "the game can make the
 * object bigger, so there need not be two files"). The objects are in true metres.
 */
static float ObjectScale(const ThreeDVehicleObject *obj)
{
	if (obj != nullptr && obj->size.param >= 0) {
		const GRFFile *file = GetFileByGRFID(obj->grfid);
		if (file != nullptr) {
			auto scale = obj->size.ScaleOf(file->GetParam(obj->size.param));
			if (scale.has_value()) return *scale;
		}
	}
	return _settings_game.vehicle.object_original_size ? 1.2f : 1.0f;
}

/** The rig: 'test3d stav <n> [<n> ...]' draws every vehicle object with states in its state n, several the states of its layers made one (empty: as its GRF picks). */
std::vector<int> _three_d_rig_states;
uint32_t _three_d_rig_cargo = 0; ///< the rig's cargo ('test3d naklad'): its label the load of a drawing of no vehicle (the purchase list, 'kresli') is of, 0 the engine's own

/**
 * The state of a vehicle's object its GRF picks ("stavy", grrrrf hra/3d/ROZHRANI-GRF-STAVY.md): for each layer of its
 * pictures the state of that layer whose range holds the number in the GRF of the first sprite of the set the GRF
 * chooses for the layer, by its own rules; the states of the layers made one (ThreeDVehicleObject::ComposeState()).
 * @return its step (ThreeDVehicleObject::State()); 0 none: the object as it is, its paint new, no rust
 */
static const Vehicle *PieceAway(const Vehicle *v, int steps);

/**
 * What the layers of a vehicle's state carry of the shared loads (ThreeDVehicleState::load_piece, on the bed of its
 * body): how many pieces -- of those its bed takes, as many as it is loaded (the colleague: "half the load, half the
 * pallets"), at least one with any cargo; its bed full in the purchase list -- and of which cargo, made one with the
 * state (ThreeDVehicleObject::ComposeState()). The pieces a layer puts on their places ("misto") are there whatever is
 * loaded, only of its cargo's colour. Of pieces with seats (a bench of the girls) the count is of the figures on the
 * seats of all of them, the benches all there.
 * @param v the piece of the vehicle the cargo is in, nullptr in the purchase list
 * @return the count above the cargo's label, 0 none
 */
static uint64_t LoadCode(const ThreeDVehicleObject *obj, const std::vector<int> &layers, const Vehicle *v, EngineID engine)
{
	std::string_view name;
	ThreeDLoadBed bed;
	bool placed = false;
	for (int step : layers) {
		if (step <= 0 || static_cast<size_t>(step) > obj->states.size()) continue;
		const ThreeDVehicleState &s = obj->states[step - 1];
		if (!s.load_piece.empty()) name = s.load_piece;
		if (s.bed.Given()) bed = s.bed;
		if (!s.placed_loads.empty()) placed = true;
	}
	CargoType cargo = INVALID_CARGO;
	if (v != nullptr) {
		cargo = v->cargo_type;
	} else if (const Engine *e = Engine::GetIfValid(engine); e != nullptr) {
		cargo = e->GetDefaultCargoType();
	}
	const uint32_t label = v == nullptr && _three_d_rig_cargo != 0 ? _three_d_rig_cargo : IsValidCargoType(cargo) ? CargoSpec::Get(cargo)->label.base() : 0;
	const ThreeDLoadPiece *piece = !name.empty() && bed.Given() ? GetThreeDLoadPiece(name) : nullptr;
	/* No GRF names it (a flatbed of BeamNG): its cargo's (ThreeDVehicleObject::ComposeState() puts it so). */
	if (piece == nullptr && name.empty() && obj->loads_by_cargo && bed.Given()) piece = GetThreeDLoadPieceOfCargo(label, bed);
	const int capacity = piece != nullptr ? ThreeDLoadCapacity(*piece, bed) * std::max<int>(1, static_cast<int>(piece->seats.size())) : 0;
	/* A cargo of no piece under its cover (the tarp of a flatbed of BeamNG): one, whenever anything is loaded. */
	const bool covered = piece == nullptr && name.empty() && obj->cover_part > 0 && bed.Given() && label != 0;
	if (capacity <= 0 && !placed && !covered) return 0;
	int count = covered ? 1 : std::min(capacity, 255);
	if (v != nullptr) {
		const uint stored = v->cargo.StoredCount(), cap = v->cargo_cap;
		if (covered) {
			count = stored > 0 ? 1 : 0;
		} else {
			count = cap == 0 || capacity <= 0 ? 0 : std::clamp<int>(static_cast<int>((static_cast<uint64_t>(stored) * capacity + cap / 2) / cap), stored > 0 ? 1 : 0, std::min(capacity, 255));
		}
	}
	return static_cast<uint64_t>(count) << 32 | label;
}

static int StateStep(const ThreeDVehicleObject *obj, EngineID engine, const Vehicle *v, EngineImageType image_type)
{
	/* The vehicle's colours of its set for its masks of details (the colleague's hra/3d/PROSBA-NAKLADAKY-TATRA-3D.md,
	 * point 2): as its 2D pictures are painted (GetCustomEngineSprite()), the default ones in the purchase list. */
	const uint16_t colours = obj->HasDetails() ? (v != nullptr ? GetVehicleTrueColours(v) : GetEngineTrueColours(engine, {})) : 0;
	if (obj->states.empty() && colours == 0) return 0;
	if (!_three_d_rig_states.empty()) {
		std::vector<int> layers;
		for (int s : _three_d_rig_states) layers.push_back(s > 0 && static_cast<size_t>(s) <= obj->states.size() ? s : 0);
		return obj->ComposeState(layers, colours, LoadCode(obj, layers, nullptr, engine));
	}
	/* A car of BeamNG: one of the mod's paints each car, the first in the purchase list, faded by its whole years; a
	 * flatbed with the pieces of its cargo on its bed, as many as it is loaded. */
	if (obj->paint_random) {
		const size_t years = std::max<size_t>(obj->paint_years, 1), paints = std::max<size_t>(obj->states.size() / years, 1);
		int step = 1;
		if (v != nullptr) {
			const size_t age = std::min<size_t>(static_cast<size_t>(std::max(v->age.base(), 0)) / CalendarTime::DAYS_IN_YEAR, years - 1);
			step = 1 + static_cast<int>((v->random_bits % paints) * years + age);
		}
		if (!obj->loads_by_cargo) return step;
		return obj->ComposeState({step}, 0, LoadCode(obj, {step}, v, engine));
	}
	/* The sets of the layers of its pieces, each piece's after the one before: of every piece of it (the colleague's
	 * semi-trailer has its sets in its first piece and hangs from its middle one, the lesovoz has them in its second,
	 * PROSBA-NAKLADAKY-TATRA-3D.md point 3), or of its anchor alone when its states name their piece ("clanek": "the
	 * middle's sets decide", PROSBA-VAGONY-3D.md point 2). In the purchase list and the news the engine's own. */
	struct Source {
		EngineID engine;
		const Vehicle *v;
		int role; ///< the piece's "clanek" (ThreeDVehicleState::piece), -1 any
	};
	std::vector<Source> sources{{engine, v, -1}};
	if (v != nullptr && v->engine_type == engine && image_type != EngineImageType::Purchase && image_type != EngineImageType::Preview) {
		int piece = 0;
		if (ThreeDVehicleObjectOf(v, &piece) == obj) {
			const int last = static_cast<int>(obj->pieces.size()) - 1;
			auto role = [&](int k) { return k == obj->anchor ? 1 : k == 0 ? 0 : k == last ? 2 : -1; };
			sources.clear();
			for (int k = 0; k <= last; k++) {
				if (obj->states_of_anchor && k != piece) continue;
				const Vehicle *u = PieceAway(v, k - piece);
				if (u != nullptr) sources.push_back({u->engine_type, u, role(k)});
			}
		}
	}
	std::vector<int> layers;
	std::vector<uint32_t> ids;
	/* The vehicle's colours of its set (callback 1C1) are those of the piece whose sets it shows, as in 2D each piece
	 * paints its own picture: the semi-trailer of the Terrno hangs from a piece of its own cargo, the marijuana on its
	 * first one. */
	const Source *painted_by = nullptr;
	const Source *loaded_by = nullptr; ///< the piece whose set names the pieces of a load: its cargo is theirs
	for (const Source &src : sources) {
		ids.clear();
		GetVehicleSetSpriteLocalIDs(src.engine, src.v, image_type, ids);
		for (size_t l = 0; l < ids.size(); l++) {
			int found = 0;
			for (size_t i = 0; i < obj->states.size() && ids[l] != UINT32_MAX; i++) {
				const ThreeDVehicleState &s = obj->states[i];
				if (src.role >= 0 && s.piece >= 0 && s.piece != src.role) continue;
				if (static_cast<size_t>(s.layer) == l && ids[l] >= s.first && ids[l] <= s.last) {
					found = static_cast<int>(i) + 1;
					break;
				}
			}
			if (found != 0 && painted_by == nullptr) painted_by = &src;
			if (found != 0 && !obj->states[found - 1].load_piece.empty()) loaded_by = &src;
			/* (A state its pieces all pick, Sergej's paint, once.) */
			if (found != 0 && std::ranges::find(layers, found) != layers.end()) found = 0;
			layers.push_back(found);
		}
	}
	const uint16_t painted = painted_by != nullptr && painted_by->v != nullptr && obj->HasDetails() ? GetVehicleTrueColours(painted_by->v) : colours;
	return obj->ComposeState(layers, painted, LoadCode(obj, layers, loaded_by != nullptr ? loaded_by->v : v, engine));
}

/**
 * The pieces of the shared loads a state carries, a kind of piece in a colour each: those side by side on the bed, as
 * many as are loaded (ThreeDVehicleState::load_piece), and those on their own places (ThreeDVehicleState::placed_loads);
 * of their own colour or their cargo's, where each lies on the bed.
 * @return false none
 */
bool GetThreeDLoadPieces(const ThreeDVehicleState *st, std::vector<ThreeDLoadPieces> &out)
{
	out.clear();
	if (st == nullptr || !st->bed.Given()) return false;
	/* The kind of a figure in a frame and a state, those of one kind together. */
	auto kind = [&out](const ThreeDVehicleObject *fig, int frame, int state) -> ThreeDLoadPieces * {
		if (fig == nullptr) return nullptr;
		for (ThreeDLoadPieces &k : out) {
			if (k.fig == fig && k.frame == frame && k.state == state) return &k;
		}
		ThreeDLoadPieces &k = out.emplace_back();
		k.fig = fig;
		k.frame = frame;
		k.state = state;
		return &k;
	};
	/* Of a piece, in a colour. */
	auto kind_of = [&kind, st](const ThreeDLoadPiece &piece, uint32_t colour) -> ThreeDLoadPieces * {
		if (piece.figure.empty()) return nullptr;
		return kind(GetThreeDFigure(piece.figure), piece.frame, ThreeDLoadColourState(piece, st->load_label, colour));
	};
	const ThreeDLoadPiece *piece = st->load_piece.empty() ? nullptr : GetThreeDLoadPiece(st->load_piece);
	if (piece != nullptr && (st->load_count > 0 || !piece->seats.empty())) {
		/* Pieces with seats (the benches of the girls) all, as many as the bed takes, empty too when the state shows them;
		 * the others as many as are loaded. */
		std::vector<ThreeDLoadPlace> places;
		ThreeDLoadPlaces(*piece, st->bed, piece->seats.empty() ? st->load_count : ThreeDLoadCapacity(*piece, st->bed), places);
		if (ThreeDLoadPieces *k = kind_of(*piece, st->load_colour); k != nullptr) k->places.insert(k->places.end(), places.begin(), places.end());
		/* The figures on their seats, as many as are loaded, from the front bench on, each bench's seats in their order. */
		int figures = piece->seats.empty() ? 0 : st->load_count;
		for (const ThreeDLoadPlace &at : places) {
			const float a = at.turn * static_cast<float>(M_PI) / 180.0f, c = std::cos(a), s = std::sin(a);
			for (const ThreeDVehicleCrew &seat : piece->seats) {
				if (figures <= 0) break;
				figures--;
				ThreeDLoadPieces *k = kind(GetThreeDFigure(seat.figure, piece->package), 0, 0);
				if (k == nullptr) continue;
				k->places.push_back({{at.at[0] + seat.at[0] * c - seat.at[1] * s, at.at[1] + seat.at[0] * s + seat.at[1] * c, at.at[2] + seat.at[2]}, at.turn + seat.turn, seat.scale});
			}
		}
	}
	for (const ThreeDPlacedLoad &placed : st->placed_loads) {
		const ThreeDLoadPiece *piece = GetThreeDLoadPiece(placed.piece);
		if (piece == nullptr) continue;
		if (ThreeDLoadPieces *k = kind_of(*piece, placed.colour); k != nullptr) k->places.push_back({{placed.at[0], placed.at[1], st->bed.floor}, 0.0f});
	}
	std::erase_if(out, [](const ThreeDLoadPieces &k) { return k.places.empty(); });
	return !out.empty();
}

/** The state of a step (StateStep()), nullptr for none. */
static const ThreeDVehicleState *StateOf(const ThreeDVehicleObject *obj, int step)
{
	return obj->State(step);
}

/** How far a state turns the whole object about z at its origin ("otoceni"), degrees. */
static float StateTurn(const ThreeDVehicleObject *obj, int step)
{
	const ThreeDVehicleState *s = StateOf(obj, step);
	return s != nullptr ? s->turn : 0.0f;
}

/** The colour of the paint a state gives (StateStep()): 0 none, the texture's own. */
static uint32_t PaletteColour(const ThreeDVehicleObject *obj, int step)
{
	const ThreeDVehicleState *s = StateOf(obj, step);
	return s != nullptr ? s->colour : 0;
}

/** The fading of the paint in a state: 0 new, 1 the target's colour. */
static float FadeAmount(const ThreeDVehicleObject *obj, int step)
{
	const ThreeDVehicleState *s = StateOf(obj, step);
	return s != nullptr && obj->paint.fades ? s->fade : 0.0f;
}

/** How far a state has rusted: 0 new, 1 all of it. */
static float RustTime(const ThreeDVehicleObject *obj, int step)
{
	const ThreeDVehicleState *s = StateOf(obj, step);
	return s != nullptr && !obj->rust.mask_file.empty() ? s->rust : 0.0f;
}

/** The view from the eyes: how far back on its way a vehicle's heading is taken from (EyeFacing()), world units. */
static constexpr double EYE_TRAIL_UNITS = 3.0;
/** And how much of the way to that heading it turns a tick: smooth, not in the steps of its way's points. */
static constexpr double EYE_TURN_A_TICK = 0.5;

/**
 * Where a vehicle drawn from its object has been: for the way it faces in the view from the eyes (EyeFacing()), the
 * way its wheels turn there, and the way its crew walks wherever it is drawn (WalkCode()).
 */
struct EyeTrail {
	std::array<std::pair<int, int>, 8> at{}; ///< its last places, the newest last
	int count = 0;
	double target = 0;  ///< the heading its way gives, radians in the world, 0 along +x
	double facing = 0;  ///< the heading it is drawn at, turning towards the target
	uint64_t tick = 0;  ///< the game's tick of the last turn
	double rolled = 0;  ///< how far it has gone, world units, back counting less (its wheels turn by it)
	double walked = 0;  ///< and as it is drawn: each step over the foreshortening along it there (SqueezeAt()), back counting less (its crew walks by it)
	double curvature = 0; ///< how much its way turns, radians a world unit, to the left (its front wheels steer by it)
	double since_turn = 0; ///< world units gone since its target last changed
	uint64_t seen = 0;  ///< the game's tick it was last looked at
};
static std::map<VehicleID, EyeTrail> _eye_trails;

/** The angle of a direction in the world, radians, 0 along +x, pi / 2 along +y. */
static inline double DirectionAngle(int d)
{
	return std::atan2(static_cast<double>(DIR_DY[d]), static_cast<double>(DIR_DX[d]));
}

/** A step of the chain along a heading, world units: 1 along x or y, the square root of 2 on the diagonal (DIR_DX[]). */
static inline double ChainStep(double facing)
{
	return 1.0 / std::max(std::abs(std::cos(facing)), std::abs(std::sin(facing)));
}

/** The foreshortening along a vehicle facing a heading, as the game spaces its vehicles: SQUEEZE along x or y, none on the diagonal. */
static inline float SqueezeAt(double facing)
{
	return 1.0f - (1.0f - SQUEEZE) * static_cast<float>(std::abs(std::cos(2.0 * facing)));
}

/**
 * The trail of a vehicle drawn from its object, brought to where it is now: the way gone since it was last looked at,
 * forward along its heading or back.
 * @param by_direction its direction's angle, radians (turned round when its object faces backwards)
 * @param squeezed whether it is foreshortened along itself (SqueezedType()): its crew walks the way as drawn
 */
static EyeTrail &TrailOf(const Vehicle *v, double by_direction, bool squeezed)
{
	const uint64_t now = TimerGameTick::counter;
	EyeTrail &e = _eye_trails[v->index];
	const std::pair<int, int> here{v->x_pos, v->y_pos};
	if (e.count == 0 || std::hypot(here.first - e.at[e.count - 1].first, here.second - e.at[e.count - 1].second) > 2.0 * TILE_SIZE) {
		/* Seen first, or far from where it was (another vehicle of the number, one out of a depot). */
		e = {};
		e.at[0] = here;
		e.count = 1;
		e.target = e.facing = by_direction;
		e.tick = now;
	}
	if (e.at[e.count - 1] != here) {
		/* The way gone: forward along its heading, or back. */
		const double dx = here.first - e.at[e.count - 1].first, dy = here.second - e.at[e.count - 1].second;
		const double step = std::hypot(dx, dy);
		e.rolled += (dx * std::cos(e.facing) + dy * std::sin(e.facing)) >= 0 ? step : -step;
		const double drawn = squeezed ? step / SqueezeAt(std::atan2(dy, dx)) : step;
		e.walked += (dx * std::cos(by_direction) + dy * std::sin(by_direction)) >= 0 ? drawn : -drawn;
		e.since_turn += step;
		if (e.count == static_cast<int>(e.at.size())) {
			std::move(e.at.begin() + 1, e.at.end(), e.at.begin());
			e.count--;
		}
		e.at[e.count++] = here;
	}
	e.seen = now;
	/* The trails of vehicles gone, now and then (not this one: it was just seen). */
	if (now % 512 == 0 && _eye_trails.size() > 64) {
		std::erase_if(_eye_trails, [&](const auto &kv) { return now - kv.second.seen > 2048; });
	}
	return e;
}

/**
 * The frames of the crew that walks (ThreeDVehicleObject::WalkingCrew()) the state shows, the way gone so far (the
 * colleague's hra/3d/PROSBA-SPECIAL-3D.md): a round of a figure's frames each "krok_m" of its own as big as it is drawn,
 * forward as it faces; the frame of the k-th of those the state shows in the bits 8k to 8k + 7, of the first eight (of
 * all its crew that walks, the colleague's Special 3D v3 has seven girls in three groups each). Standing, a figure
 * stays in its step.
 * @param walked the way gone as drawn (EyeTrail::walked), world units
 * @param scale the object's metres to the game's
 */
static uint64_t WalkCode(const ThreeDVehicleObject *obj, int state, double walked, float scale)
{
	const std::vector<ThreeDWalkingCrew> &walking = obj->WalkingCrew();
	if (walking.empty() || walked == 0) return 0;
	const ThreeDParts parts = obj->PartsOf(state);
	const float turn = StateTurn(obj, state);
	uint64_t code = 0;
	size_t slot = 0;
	for (size_t k = 0; k < walking.size() && slot < 8; k++) {
		const ThreeDWalkingCrew &w = walking[k];
		if (!ThreeDVehicleShape::Shown(w.part, parts)) continue;
		const size_t shown = slot++;
		const ThreeDVehicleObject &f = *w.figure;
		const double step = f.walk_step * scale * w.scale * UNITS_PER_METRE;
		if (step <= 0 || f.frames < 2) continue;
		/* Facing back as it goes (turned round in the vehicle, or the vehicle turned round), it walks back. */
		const double rounds = (std::cos((turn + w.turn) * M_PI / 180.0) < 0 ? -walked : walked) / step;
		const int frame = std::clamp(static_cast<int>((rounds - std::floor(rounds)) * f.frames), 0, f.frames - 1);
		code |= static_cast<uint64_t>(std::min(frame, 255)) << (8 * shown);
	}
	return code;
}

/** How far a vehicle drawn from its object has gone as drawn (EyeTrail::walked), for its crew that walks; 0 none (the purchase list). */
static double WalkedOf(const Vehicle *v, const ThreeDVehicleObject *obj)
{
	if (v == nullptr || obj->WalkingCrew().empty()) return 0;
	bool reversed = false;
	if (v->type == VehicleType::Train) {
		const Train *t = Train::From(v);
		if (t->flags.Test(VehicleRailFlag::Flipped)) reversed = !reversed;
		if (t->spritenum == CUSTOM_VEHICLE_SPRITENUM_REVERSED) reversed = !reversed;
	}
	const int d = to_underlying(reversed ? ReverseDir(v->direction) : v->direction) & 7;
	return TrailOf(v, DirectionAngle(d), SqueezedType(obj->type)).walked;
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
/** A step of the chain to a metre of a vehicle's object as drawn: a metre along a vehicle on the map, foreshortened as the sets draw. */
static constexpr double CHAIN_STEPS_PER_METRE = UNITS_PER_METRE / 1.41421356237;

/**
 * The steps of the chain to a length of its set of an object whose index gives the lengths its object is for
 * ("delky_clanku_grf", the colleague's hra/3d/PROSBA-NAKLADAKY-TATRA-3D.md point 3: "the metres to a unit from the
 * object, the lengths of the pieces from the GRF"); 0 for one that does not.
 */
static double StepsPerSetLength(const ThreeDVehicleObject *obj)
{
	if (obj->grf_lengths.empty() || !SqueezedType(obj->type)) return 0;
	int total = 0;
	for (int l : obj->grf_lengths) total += l;
	const float length = obj->Length();
	if (total <= 0 || length < 0.1f) return 0;
	return length * ObjectScale(obj) * CHAIN_STEPS_PER_METRE / total;
}

/** A piece's length on the map as its set's length makes it (StepsPerSetLength()), a whole vehicle at most. */
static int PieceLengthOfSet(const ThreeDVehicleObject *obj, int set_length)
{
	return std::clamp(static_cast<int>(std::lround(set_length * StepsPerSetLength(obj))), 1, static_cast<int>(VEHICLE_LENGTH));
}

static std::vector<int> PieceLengths(const ThreeDVehicleObject *obj)
{
	std::vector<int> lengths = SetPieceLengths(obj);
	/* A lorry of the colleague's: each piece as long as its set says, at the object's metres to a length. */
	if (StepsPerSetLength(obj) > 0) {
		for (int &l : lengths) l = PieceLengthOfSet(obj, l);
		return lengths;
	}
	const float object_length = SqueezedType(obj->type) ? obj->Length() : 0.0f;
	if (lengths.empty() || object_length < 0.1f) return lengths;
	const int n = static_cast<int>(lengths.size()), most = static_cast<int>(VEHICLE_LENGTH);
	int total = static_cast<int>(std::lround(object_length * ObjectScale(obj) * UNITS_PER_METRE / std::sqrt(2.0)));
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
	if (StepsPerSetLength(obj) > 0) return PieceLengthOfSet(obj, set_length);
	std::vector<int> lengths = PieceLengths(obj);
	return piece >= 0 && piece < static_cast<int>(lengths.size()) ? lengths[piece] : set_length;
}

/**
 * The length of a piece of a vehicle on the map (Train::ConsistChanged(), GetRoadVehLength()), as
 * ThreeDVehicleObjectPieceLength() -- the object it is a piece of told by the pieces around it (a piece of several,
 * ThreeDVehicleObjectOf()), and its length its set's now, the refit's (callback 36: a lorry without a trailer has a
 * shorter snout and an empty trailer of length 1, the colleague's point 3).
 * @param v the piece, linked into its vehicle
 * @param set_length its length as its set says now
 */
int ThreeDVehicleObjectPieceLengthOf(const Vehicle *v, int set_length)
{
	int piece = 0;
	const ThreeDVehicleObject *obj = ThreeDVehicleObjectOf(v, &piece);
	if (obj == nullptr || !SqueezedType(obj->type)) return set_length;
	if (StepsPerSetLength(obj) > 0) return PieceLengthOfSet(obj, set_length);
	std::vector<int> lengths = PieceLengths(obj);
	return piece >= 0 && piece < static_cast<int>(lengths.size()) ? lengths[piece] : set_length;
}

/**
 * Where the object's origin is from a piece's position, steps forward along the vehicle: the middle of the chain.
 * An object whose index gives its lengths of its set (the colleague's lorries and wagons) the middle of what its
 * pieces cover, so that the objects of a vehicle's articles lie one after another as its pieces do ("the end of an
 * object exactly at the coupling"): a road vehicle's piece reaching a whole vehicle's half ahead of its position and
 * its length back (the next piece follows it at its length), a train's piece its half each way.
 */
static double MiddleFromPiece(const ThreeDVehicleObject *obj, const std::vector<int> &lengths, int piece)
{
	std::vector<double> pos;
	double middle = ThreeDChainPositions(obj->type, lengths, pos);
	if (piece < 0 || static_cast<size_t>(piece) >= pos.size()) return 0.0;
	if (!obj->grf_lengths.empty() && !lengths.empty()) {
		if (obj->type == VehicleType::Road) {
			middle += (static_cast<double>(VEHICLE_LENGTH) - lengths.back()) / 2.0;
		} else if (obj->type == VehicleType::Train) {
			middle += (lengths.front() - lengths.back()) / 4.0;
		}
	}
	return middle - pos[piece];
}

/** Whether a piece of a vehicle drawn from its object faces the other way: a train turned round, the rear head of a double-headed engine. */
static bool PieceReversed(const Vehicle *v)
{
	bool reversed = false;
	if (v->type == VehicleType::Train) {
		const Train *t = Train::From(v);
		if (t->flags.Test(VehicleRailFlag::Flipped)) reversed = !reversed;
		if (t->spritenum == CUSTOM_VEHICLE_SPRITENUM_REVERSED) reversed = !reversed;
	}
	return reversed;
}

/** The direction a piece is drawn facing (PieceReversed()). */
static int PieceDirection(const Vehicle *v)
{
	return to_underlying(PieceReversed(v) ? ReverseDir(v->direction) : v->direction) & 7;
}

/** A piece of a vehicle so many pieces before (negative) or after it, nullptr past the vehicle's ends. */
static const Vehicle *PieceAway(const Vehicle *v, int steps)
{
	for (; v != nullptr && steps < 0; steps++) v = v->Previous();
	for (; v != nullptr && steps > 0; steps--) v = v->Next();
	return v;
}

/** The length of a piece on the map now, eighths of a tile. */
static int CachedLength(const Vehicle *v)
{
	if (v->type == VehicleType::Train) return Train::From(v)->gcache.cached_veh_length;
	if (v->type == VehicleType::Road) return RoadVehicle::From(v)->gcache.cached_veh_length;
	return static_cast<int>(VEHICLE_LENGTH);
}

/**
 * The lengths of an object's pieces as they are on the map, eighths of a tile: a vehicle's pieces' own (the refit
 * makes them other than its set's in the purchase list, callback 36), of the object of its index's lengths; else as
 * the object makes them (PieceLengths()).
 * @param v a piece of the vehicle, the object's piece piece; nullptr for the purchase list
 */
static std::vector<int> ObjectLengths(const ThreeDVehicleObject *obj, const Vehicle *v, int piece)
{
	std::vector<int> lengths = PieceLengths(obj);
	if (v == nullptr || obj->grf_lengths.empty()) return lengths;
	for (size_t k = 0; k < lengths.size(); k++) {
		const Vehicle *u = PieceAway(v, static_cast<int>(k) - piece);
		if (u != nullptr) lengths[k] = CachedLength(u);
	}
	return lengths;
}

/**
 * The object a vehicle's article is coupled behind ("spojeni", the colleague's point 3): the one of the piece just
 * before its first, when it has a front coupling and that one a back coupling, both facing the same way.
 * @param anchor the object's anchor piece
 * @param[out] before that object, and its anchor piece
 */
static bool CoupledBehind(const ThreeDVehicleObject *obj, const Vehicle *anchor, const ThreeDVehicleObject *&before, const Vehicle *&before_anchor)
{
	if (!obj->has_couple_front) return false;
	const Vehicle *first = PieceAway(anchor, -obj->anchor);
	const Vehicle *prev = first != nullptr ? first->Previous() : nullptr;
	if (prev == nullptr) return false;
	int piece = 0;
	before = ThreeDVehicleObjectOf(prev, &piece);
	if (before == nullptr || before == obj || !before->has_couple_back) return false;
	before_anchor = PieceAway(prev, before->anchor - piece);
	return before_anchor != nullptr && PieceReversed(before_anchor) == PieceReversed(anchor);
}

/** How far ahead of its front coupling an article's shape may reach and still not be drawn over the one before (DrawnOverBefore()), metres. */
static constexpr float OVERHANG_DRAWN_OVER_M = 0.1f;

/**
 * Whether an article is drawn over the one it is coupled behind (CoupledBehind()): its shape reaches ahead of its front
 * coupling, over the other's back (a semi-trailer over its tractor's fifth wheel by 1.3 to 2 m; a trailer on its
 * drawbar does not). Drawn as the boxes of their pieces sort them, the tractor of a lorry going towards the viewer was
 * drawn last, over the front of its semi-trailer (the colleague's hra/3d/TAHAC-NAVES-PORADI-PRO-HRU.md: the player,
 * "the Tatras have the tractor drawn over the semi-trailer going south-east and south-west").
 * @param anchor the article's anchor piece
 * @param[out] before the article before it, and its anchor piece
 */
static bool DrawnOverBefore(const ThreeDVehicleObject *obj, const Vehicle *anchor, const ThreeDVehicleObject *&before, const Vehicle *&before_anchor)
{
	if (!CoupledBehind(obj, anchor, before, before_anchor)) return false;
	/* Its front as far as any of its states reaches (the semi-trailer's body is a part its state shows, its frame ends
	 * at the kingpin). */
	const ThreeDVehicleShape *shape = obj->TryGetShape();
	if (shape == nullptr) return false;
	const float front = shape->all[1] > shape->all[0] ? shape->all[1] : shape->x1;
	return front > obj->couple_front[0] + OVERHANG_DRAWN_OVER_M;
}

/** The anchor piece of the article a vehicle's piece is drawn over (DrawnOverBefore()) when it is its article's anchor piece; nullptr else. */
const Vehicle *ThreeDVehicleDrawnOver(const Vehicle *v)
{
	int piece = 0;
	const ThreeDVehicleObject *obj = ThreeDVehicleObjectOf(v, &piece);
	if (obj == nullptr || piece != obj->anchor) return nullptr;
	const ThreeDVehicleObject *before = nullptr;
	const Vehicle *before_anchor = nullptr;
	return DrawnOverBefore(obj, v, before, before_anchor) ? before_anchor : nullptr;
}

/**
 * Where on the map an object hung from its anchor piece has its origin, world units: from the anchor piece along the
 * way it faces as far as the middle of its pieces (MiddleFromPiece()) -- or, an article coupled behind another
 * (CoupledBehind()), with its front coupling on the back coupling of that one, which is placed so in its turn: the
 * train of lorries bends at its couplings as the pieces turn, each article facing as its own anchor piece.
 * @param d the direction the anchor piece is drawn facing (PieceDirection())
 * @param depth how many articles before it are placed so (a few at most)
 */
static void ObjectOriginOnMap(const ThreeDVehicleObject *obj, const Vehicle *anchor, int d, double &x, double &y, int depth = 0)
{
	const double along = MiddleFromPiece(obj, ObjectLengths(obj, anchor, obj->anchor), obj->anchor);
	x = anchor->x_pos + along * DIR_DX[d];
	y = anchor->y_pos + along * DIR_DY[d];
	const ThreeDVehicleObject *before = nullptr;
	const Vehicle *before_anchor = nullptr;
	if (depth >= 4 || !CoupledBehind(obj, anchor, before, before_anchor)) return;
	const int bd = PieceDirection(before_anchor);
	double bx, by;
	ObjectOriginOnMap(before, before_anchor, bd, bx, by, depth + 1);
	const double back = before->couple_back[0] * ObjectScale(before) * CHAIN_STEPS_PER_METRE;
	const double front = obj->couple_front[0] * ObjectScale(obj) * CHAIN_STEPS_PER_METRE;
	x = bx + back * DIR_DX[bd] - front * DIR_DX[d];
	y = by + back * DIR_DY[bd] - front * DIR_DY[d];
}

/**
 * How far along its direction from its anchor piece an object is drawn on the map (ObjectOriginOnMap()), steps of the
 * chain: what its sprite of the direction is made about.
 */
static double ObjectAlongOnMap(const ThreeDVehicleObject *obj, const Vehicle *anchor, int d)
{
	double x, y;
	ObjectOriginOnMap(obj, anchor, d, x, y);
	const double dx = DIR_DX[d], dy = DIR_DY[d];
	return ((x - anchor->x_pos) * dx + (y - anchor->y_pos) * dy) / (dx * dx + dy * dy);
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

/**
 * The sprite of an object facing a direction, hung from one of its pieces, its crew that walks at its frames: made when it is first drawn.
 * @param along how far along its direction from the piece its origin is, steps of the chain (MiddleFromPiece(), ObjectAlongOnMap())
 * @param length the piece's length on the map, eighths of a tile
 * @param gui for the windows (ObjectSpriteRequest::gui)
 */
static SpriteID ObjectSprite(const ThreeDVehicleObject *obj, int piece, int dir, int fade, uint64_t walk, double along, int length, bool gui)
{
	/* (The piece and the place along in eighths of a step: the article of a train of lorries is hung where its
	 * coupling puts it, and a refit changes its pieces' lengths.) */
	const uint32_t where = static_cast<uint32_t>(piece & 0xFF) | (static_cast<uint32_t>(std::lround(along * 8)) & 0xFFFF) << 8 | static_cast<uint32_t>(length & 0x7F) << 24 | (gui ? 1U << 31 : 0U);
	auto key = std::make_tuple(obj, static_cast<int>(where), dir, static_cast<int>(std::lround(ObjectScale(obj) * 1000)), fade, walk);
	auto it = _object_sprites.find(key);
	if (it != _object_sprites.end()) return it->second;

	ObjectSpriteRequest req;
	req.obj = obj;
	req.dir = dir;
	req.scale = ObjectScale(obj);
	req.fade = fade;
	req.walk = walk;
	req.gui = gui;
	req.where = where;
	Point anchor = ThreeDSpriteAnchor(obj->type, length, static_cast<Direction>(dir));
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
	const ThreeDVehicleObject *obj = (v != nullptr && v->engine_type == engine) ? ThreeDVehicleObjectOf(v, &piece) : GetThreeDVehicleObject(engine, &piece);
	if (obj == nullptr || direction == Direction::Invalid) return false;
	const bool alone = image_type == EngineImageType::Purchase || image_type == EngineImageType::Preview || v == nullptr || v->engine_type != engine;
	int from = alone ? 0 : obj->anchor;
	SpriteID sprite = EmptyObjectSprite();
	if (piece == from) {
		const int state = StateStep(obj, engine, v, image_type);
		/* Its crew that walks at its step of the way gone (on the map, as it moves). */
		const uint64_t walk = image_type == EngineImageType::OnMap ? WalkCode(obj, state, WalkedOf(v, obj), ObjectScale(obj)) : 0;
		const int d = to_underlying(direction) & 7;
		/* Hung from the middle of its pieces as they are, or where its coupling puts it (an article of a train of
		 * lorries on the map); alone from its first piece as its set's lengths make it. */
		const std::vector<int> lengths = ObjectLengths(obj, alone ? nullptr : v, from);
		const double along = !alone && image_type == EngineImageType::OnMap ? ObjectAlongOnMap(obj, v, d) : MiddleFromPiece(obj, lengths, from);
		/* The purchase list and the news: a small picture at the interface's zoom, made in the background (the player,
		 * 17. 10.: "the picture in the purchase menu is tiny, what are you photographing there"). */
		const bool gui = image_type == EngineImageType::Purchase || image_type == EngineImageType::Preview;
		sprite = ObjectSprite(obj, from, d, state, walk, along, lengths[std::clamp<int>(from, 0, static_cast<int>(lengths.size()) - 1)], gui);
	}
	if (sprite == 0) return false;
	result->Set(sprite);
	return true;
}

/**
 * An engine of a set drawn from the set's own pictures, as no 3D object was found for it: when those pictures are
 * empty -- a set whose vehicles are drawn from the objects of its package -- the package was not found, and nothing at
 * all would be seen. Said once a set in the log, at level 0, with where the game looked (the colleague's
 * hra/3d/PROSBA-TATRA-V6.md: "v5 was seen neither in the purchase list nor on the road"). Only by the pictures of the
 * purchase list and of the front of a vehicle: a part of an articulated vehicle no one is meant to see has empty ones.
 */
void NoteThreeDVehicleWithoutObject(EngineID engine, const Vehicle *v, EngineImageType image_type, const VehicleSpriteSeq &seq)
{
	if (image_type != EngineImageType::Purchase && (v == nullptr || !v->IsPrimaryVehicle())) return;
	const Engine *e = Engine::GetIfValid(engine);
	if (e == nullptr || e->grf_prop.grffile == nullptr) return;
	static std::set<uint32_t> told;
	static std::set<std::pair<uint32_t, uint16_t>> looked;
	const uint32_t grfid = e->grf_prop.grfid;
	if (told.contains(grfid) || !looked.insert({grfid, e->grf_prop.local_id}).second) return;
	for (uint i = 0; i < seq.count; i++) {
		/* A pixel at the normal zoom: ZOOM_BASE square as the cache keeps it, at the finest. */
		const Sprite *s = GetSprite(seq.seq[i].sprite, SpriteType::Normal);
		if (s != nullptr && (s->width > ZOOM_BASE || s->height > ZOOM_BASE)) return;
	}
	told.insert(grfid);
	Debug(misc, 0, "3D vehicles: the set {} ({:08X}) draws its engine {} with empty pictures, and no package of its 3D objects was found (looked in {}: <package>/objekty/<set>/objekty.json, a folder or a tar, with \"grf\": \"{:08X}\")",
			e->grf_prop.grffile->filename, std::byteswap(grfid), e->grf_prop.local_id, ThreeDPackagePlaces(), std::byteswap(grfid));
}

/** An object drawn into pixels about its origin. */
struct ObjectRaster {
	int left = 0, top = 0, width = 0, height = 0;
	std::vector<Colour> px;
	std::vector<uint8_t> lit;                ///< at night, whether a pixel is of the lit glass of its lights
	std::vector<ThreeDPictureLight> lights;  ///< at night, the lights that are on, pixels from its origin
};

/**
 * The lights of a vehicle object on at night (RasterObject()): its headlights, its red lights at the back; going back
 * first, the headlights on its back end and the red lights on its front end (ThreeDVehicleShape::front_back_lights).
 * The bit of the glass of each kind (ThreeDVehicleShape::night_kind) is 1 << kind.
 */
static constexpr int LIGHTS_FRONT = 1, LIGHTS_BACK = 2, LIGHTS_FRONT_AT_BACK = 4, LIGHTS_BACK_AT_FRONT = 8;

/** Whether the glass of a triangle of a shape's "noc" is lit with these lights on. */
static inline bool NightGlassLit(const ThreeDVehicleShape &shape, size_t triangle, int lights)
{
	return (lights & (1 << (triangle < shape.night_kind.size() ? shape.night_kind[triangle] & 3 : 0))) != 0;
}

/** The texels to a metre of a shape's texture at its full size: how fine a level of it a picture needs. */
static float TexelsPerMetre(const ThreeDVehicleObject &obj, const ThreeDVehicleShape &shape, int texture = 0)
{
	/* Asked on the thread making the pictures of the vehicles too (RasterObject()). */
	/* (By the shape and its triangles: a shape made again with more parts may be where one let go of was.) */
	static std::map<std::tuple<const ThreeDVehicleShape *, size_t, int>, float> known;
	static std::mutex known_mutex;
	const auto key = std::make_tuple(&shape, shape.tri.size(), texture);
	{
		std::lock_guard lock(known_mutex);
		auto it = known.find(key);
		if (it != known.end()) return it->second;
	}
	/* The texture's size at its finest, not the texture: its finest level is made only when drawn (the textures by need).
	 * Not known yet: as the most of them are, and not kept -- taken as 1 x 1, every picture of a figure, a few pixels
	 * big, asked for its textures at their full size (three of 1024 x 1024 for a girl at a stop). */
	int tw = 1, th = 1;
	const bool size_known = obj.TextureSize(texture, tw, th);
	if (!size_known) tw = th = 1024;
	double uv_area = 0, area = 0;
	for (size_t i = 0; i + 2 < shape.tri.size(); i += 3) {
		/* Another texture than the body's: by its own triangles (the saucer's beam, 4 x 128 texels down 12 m, was
		 * made as coarse as the body's 1024 x 1024 over the whole of it: its fading in a few steps). */
		if (texture != 0 && (i / 3 >= shape.tri_tex.size() || shape.tri_tex[i / 3] != texture)) continue;
		const uint32_t a = shape.tri[i], b = shape.tri[i + 1], c = shape.tri[i + 2];
		double ux = shape.uv[b * 2] - shape.uv[a * 2], uy = shape.uv[b * 2 + 1] - shape.uv[a * 2 + 1];
		double vx = shape.uv[c * 2] - shape.uv[a * 2], vy = shape.uv[c * 2 + 1] - shape.uv[a * 2 + 1];
		uv_area += std::abs(ux * vy - uy * vx) / 2 * tw * th;
		double px = shape.pos[b * 3] - shape.pos[a * 3], py = shape.pos[b * 3 + 1] - shape.pos[a * 3 + 1], pz = shape.pos[b * 3 + 2] - shape.pos[a * 3 + 2];
		double qx = shape.pos[c * 3] - shape.pos[a * 3], qy = shape.pos[c * 3 + 1] - shape.pos[a * 3 + 1], qz = shape.pos[c * 3 + 2] - shape.pos[a * 3 + 2];
		double cx = py * qz - pz * qy, cy = pz * qx - px * qz, cz = px * qy - py * qx;
		area += std::sqrt(cx * cx + cy * cy + cz * cz) / 2;
	}
	float density = area > 0 ? static_cast<float>(std::sqrt(uv_area / area)) : 1.0f;
	if (!size_known) return density;
	std::lock_guard lock(known_mutex);
	known.emplace(key, density);
	return density;
}

/**
 * The paint of a state over the body's texture (texture 0) of an object: its colour by number, faded with age, then
 * rusted (the colleague's points 6 and 7) -- each texel as RasterObject() draws it and as the view from the eyes
 * paints the texture (GetThreeDVehicleObjectStateTexture()).
 */
struct ObjectPaintOfState {
	const ThreeDVehicleObject *obj = nullptr;
	const ThreeDObjectTexture *mask = nullptr;      ///< the paint's mask, nullptr: none (or all of it, whole)
	const ThreeDObjectTexture *rust_mask = nullptr; ///< the rust's mask, nullptr: none
	bool coloured = false, whole = false;
	Colour colour, target, rust_colour;
	float start = 255.0f, fade = 0, rust_time = 0;
	int level = 0;                                  ///< the level of detail of the textures and masks

	ObjectPaintOfState(const ThreeDVehicleObject &obj, const ThreeDVehicleShape &shape, int level, float fade, float rust_time, uint32_t paint_colour) : obj(&obj), fade(fade), rust_time(rust_time), level(level)
	{
		const ThreeDVehiclePaint &paint = obj.paint;
		this->coloured = paint.coloured || paint_colour != 0;
		this->mask = (paint.fades || this->coloured) ? obj.GetPaintMask(level) : nullptr;
		this->whole = this->coloured && shape.paint_whole;
		this->target = Colour(paint.target);
		this->colour = Colour(paint_colour != 0 ? paint_colour : paint.colour);
		this->start = paint.start > 0 ? static_cast<float>(paint.start) : 255.0f;
		this->rust_mask = rust_time > 0 ? obj.GetRustMask(level) : nullptr;
		this->rust_colour = Colour(obj.rust.target);
	}

	/** The paint at u, v of the body's texture over its colour: coloured at the texture's lightness, faded. */
	inline void Paint(float u, float v, float &cr, float &cg, float &cb) const
	{
		if (this->mask == nullptr && !this->whole) return;
		float m = 1.0f;
		if (this->mask != nullptr) {
			int mx = std::min(this->mask->w - 1, static_cast<int>(u * this->mask->w)), my = std::min(this->mask->h - 1, static_cast<int>(v * this->mask->h));
			m = Colour(this->mask->px[static_cast<size_t>(my) * this->mask->w + mx]).r / 255.0f;
		}
		if (m <= 0) return;
		if (this->coloured) {
			/* The colour at the texture's lightness: its own where the texture is "zacatek" light. */
			float l = std::max({cr, cg, cb}) / this->start;
			cr += (std::min(255.0f, this->colour.r * l) - cr) * m;
			cg += (std::min(255.0f, this->colour.g * l) - cg) * m;
			cb += (std::min(255.0f, this->colour.b * l) - cb) * m;
		}
		if (this->fade > 0) {
			float f = this->fade * m;
			cr += (this->target.r - cr) * f;
			cg += (this->target.g - cg) * f;
			cb += (this->target.b - cb) * f;
		}
	}

	/** The rust at u, v of the body's texture over its colour. */
	inline void Rust(float u, float v, float &cr, float &cg, float &cb) const
	{
		if (this->rust_mask == nullptr) return;
		int rx = std::min(this->rust_mask->w - 1, static_cast<int>(u * this->rust_mask->w)), ry = std::min(this->rust_mask->h - 1, static_cast<int>(v * this->rust_mask->h));
		float m = Colour(this->rust_mask->px[static_cast<size_t>(ry) * this->rust_mask->w + rx]).r;
		if (m <= 0) return;
		/* Gradual: the value is the moment it starts, from nothing to all rust in "nabeh"; else how much. */
		float f = this->obj->rust.gradual ? std::clamp((this->rust_time - (m - 1) / 254.0f) / this->obj->rust.ramp, 0.0f, 1.0f) : m / 255.0f * this->rust_time;
		cr += (this->rust_colour.r - cr) * f;
		cg += (this->rust_colour.g - cg) * f;
		cb += (this->rust_colour.b - cb) * f;
	}
};

/**
 * The masks of details of an object's textures painted in the vehicle's colours of its set (the colleague's
 * hra/3d/PROSBA-NAKLADAKY-TATRA-3D.md, point 2): a texel whose mask index is of a detail the set colours by callback
 * 1C1 (the configurator, the colour by the load, by chance) in the colour of the detail at the texel's lightness, as a
 * 32bpp sprite's mask is painted (ShadeTrueColour()); a detail that is turned (a heap of marijuana to hay) its own
 * colours turned (ShiftTrueColourHue()). Faded with age as the set gives it (AgeTrueColour()). A texel of no such
 * detail stays as it is, for the colours of the states ("barvy_doplnku", "lak") -- what the colleague's index gives
 * "until then".
 */
struct ObjectDetailPaint {
	const TrueColourSet *colours = nullptr; ///< the vehicle's colours, nullptr none
	std::vector<const ThreeDDetailMask *> masks; ///< each texture's mask of details, nullptr none

	/** @param drawn the textures drawn (RasterObject()), nullptr those not: their masks are not read; empty all */
	ObjectDetailPaint(const ThreeDVehicleObject &obj, const ThreeDVehicleShape &shape, const ThreeDVehicleState *st, size_t textures, std::span<const ThreeDObjectTexture *const> drawn = {})
	{
		if (st == nullptr || st->true_colours == 0 || st->true_colours > GetTrueColourSetCount()) return;
		this->colours = &GetTrueColourSet(st->true_colours);
		this->masks.assign(textures, nullptr);
		bool any = false;
		for (size_t i = 0; i < textures; i++) {
			const int addon = i < shape.tex_addon.size() ? shape.tex_addon[i] : -1;
			if (!drawn.empty() && (i >= drawn.size() || drawn[i] == nullptr)) continue;
			if (i == 0 || addon >= 0) this->masks[i] = obj.GetDetailMask(i == 0 ? -1 : addon);
			any |= this->masks[i] != nullptr;
		}
		if (!any) this->colours = nullptr;
	}

	/** The mask of a texture, nullptr when nothing is painted by it. */
	const ThreeDDetailMask *MaskOf(size_t texture) const
	{
		return this->colours != nullptr && texture < this->masks.size() ? this->masks[texture] : nullptr;
	}

	/**
	 * Paint a texel at u, v of a texture with a mask.
	 * @return whether its detail is one the set colours (the colours of the states then leave it as it is)
	 */
	bool Paint(const ThreeDDetailMask *mask, float u, float v, float &cr, float &cg, float &cb) const
	{
		const uint8_t m = mask->At(u, v);
		if (m == 0) return false;
		for (const TrueColourRange &range : *this->colours) {
			if (m < range.first || m >= range.first + TRUE_COLOUR_RANGE_SIZE) continue;
			const Colour own(static_cast<uint8_t>(std::min(255.0f, cr)), static_cast<uint8_t>(std::min(255.0f, cg)), static_cast<uint8_t>(std::min(255.0f, cb)));
			const Colour c = range.hue ? ShiftTrueColourHue(range, own) : ShadeTrueColour(range, std::max({own.r, own.g, own.b}));
			cr = c.r;
			cg = c.g;
			cb = c.b;
			return true;
		}
		return false;
	}
};

/**
 * The camera of the pictures of an object (RasterObject()): a point of the object's frame on the picture -- turned as
 * its state has it, leant, foreshortened, seen --, a normal of it lit by the world's light, a thing that turns turned
 * about its axis. The pictures of the processor and of the graphics card (GpuRasterObject()) by the same.
 */
struct ObjectCamera {
	float scale, along, stretch, p;
	float tp, tc, ts;   ///< the state's turn about z ("otoceni")
	float ca, sa;       ///< the camera's angle
	float fx, fy, lx, ly; ///< the object's front and left in the world, for the light
	float pitch, pc, ps, roll, rc, rs;
	float spin, sc, sn;
	const float *k, *kc; ///< the axis a thing that turns turns about, and a point of it
	float ll;           ///< the length of L
	static constexpr float L[3] = {-0.55f / 1.0f, 0.35f, 0.75f};

	ObjectCamera(const ThreeDVehicleObject &obj, const ThreeDVehicleState *st, float scale, float cam_az, float world_az, float along, float p, float stretch, float spin, float pitch, float roll) :
			scale(scale), along(along), stretch(stretch), p(p), pitch(pitch), roll(roll), spin(spin), k(obj.spin_axis), kc(obj.spin_centre)
	{
		this->tp = (st != nullptr ? st->turn : 0.0f) * static_cast<float>(M_PI) / 180.0f;
		this->tc = std::cos(this->tp);
		this->ts = std::sin(this->tp);
		this->ca = std::cos(cam_az * static_cast<float>(M_PI) / 180.0f);
		this->sa = std::sin(cam_az * static_cast<float>(M_PI) / 180.0f);
		/* The light: the object's front and left in the world, the normals turned into it. */
		const float wa = (world_az + 135.0f) * static_cast<float>(M_PI) / 180.0f;
		this->fx = std::cos(wa);
		this->fy = std::sin(wa);
		this->lx = -this->fy;
		this->ly = this->fx;
		this->ll = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
		const float sp = spin * static_cast<float>(M_PI) / 180.0f;
		this->sc = std::cos(sp);
		this->sn = std::sin(sp);
		const float pp = pitch * static_cast<float>(M_PI) / 180.0f;
		this->pc = std::cos(pp);
		this->ps = std::sin(pp);
		const float rp = roll * static_cast<float>(M_PI) / 180.0f;
		this->rc = std::cos(rp);
		this->rs = std::sin(rp);
	}

	/** Turned about z at its origin as the state has it (a cart pushed). */
	inline void Turned(float &vx, float &vy) const
	{
		if (this->tp == 0.0f) return;
		const float x = vx * this->tc - vy * this->ts, y = vx * this->ts + vy * this->tc;
		vx = x;
		vy = y;
	}

	/** Turned about its axis (Rodrigues): v' = v cos + (k x v) sin + k (k . v)(1 - cos). */
	inline void Spin(float &vx, float &vy, float &vz) const
	{
		const float d = k[0] * vx + k[1] * vy + k[2] * vz;
		const float cx = k[1] * vz - k[2] * vy, cy = k[2] * vx - k[0] * vz, cz = k[0] * vy - k[1] * vx;
		vx = vx * sc + cx * sn + k[0] * d * (1 - sc);
		vy = vy * sc + cy * sn + k[1] * d * (1 - sc);
		vz = vz * sc + cz * sn + k[2] * d * (1 - sc);
	}

	/** Up a hill its front higher (pitch), in a curve leaning in (roll, its left side down): about its middle on the rail. */
	inline void Lean(float &vx, float &vy, float &vz) const
	{
		if (this->pitch != 0.0f) {
			const float x = vx * this->pc - vz * this->ps, z = vx * this->ps + vz * this->pc;
			vx = x;
			vz = z;
		}
		if (this->roll != 0.0f) {
			const float y = vy * this->rc + vz * this->rs, z = vz * this->rc - vy * this->rs;
			vy = y;
			vz = z;
		}
	}

	/** A point of the vehicle's frame on the picture: turned as the state has it, foreshortened, leant, seen. */
	inline void Place(float px, float py, float pz, float &osx, float &osy, float &od) const
	{
		this->Turned(px, py);
		float x = px * this->scale * this->along, y = py * this->scale, z = pz * this->scale * this->stretch;
		/* Leant as it stands foreshortened: as steep as the ground under it, not steeper for its length shortened (#287). */
		this->Lean(x, y, z);
		const float xr = x * this->ca - y * this->sa, yr = x * this->sa + y * this->ca;
		osx = xr * this->p;
		osy = (-yr * CAM_SIN - z * CAM_COS) * this->p;
		od = yr * CAM_COS - z * CAM_SIN;
	}

	/** A point of a thing that turns: turned about its axis's point, then placed. */
	inline void PlaceSpun(float px, float py, float pz, float &osx, float &osy, float &od) const
	{
		if (this->spin != 0.0f) {
			px -= kc[0]; py -= kc[1]; pz -= kc[2];
			this->Spin(px, py, pz);
			px += kc[0]; py += kc[1]; pz += kc[2];
		}
		this->Place(px, py, pz, osx, osy, od);
	}

	/** The light's angle at a normal of it: n . L, both in the world. */
	inline float LightAt(float nx, float ny, float nz) const
	{
		this->Turned(nx, ny);
		this->Lean(nx, ny, nz);
		const float wx = nx * this->fx + ny * this->lx, wy = nx * this->fy + ny * this->ly;
		return (wx * L[0] + wy * L[1] + nz * L[2]) / this->ll;
	}

	/** And how light it is there. */
	inline float Shade(float nx, float ny, float nz) const
	{
		return 0.55f + 0.45f * std::max(0.0f, this->LightAt(nx, ny, nz));
	}
};

/** The lights on at night of an object (RasterObject()): the places they shine from on the picture, pixels from its origin. */
static void ObjectLightsOn(const ThreeDVehicleShape &whole, const ThreeDParts &parts, int lights, const ObjectCamera &cam, std::vector<ThreeDPictureLight> &out)
{
	out.clear();
	if (lights == 0) return;
	auto add_light = [&](float px, float py, float pz, ThreeDPictureLight::Kind kind) {
		float lx, ly, ld;
		cam.Place(px, py, pz, lx, ly, ld);
		out.push_back({lx / SUPERSAMPLE, ly / SUPERSAMPLE, kind});
	};
	if ((lights & LIGHTS_FRONT) != 0 && !whole.front_lights.empty()) {
		float ahead = -std::numeric_limits<float>::infinity(), mid = 0;
		size_t n_front = 0;
		for (size_t i = 0; i < whole.front_lights.size() / 3; i++) n_front += ThreeDVehicleShape::Shown(i < whole.front_part.size() ? whole.front_part[i] : 0, parts) ? 1 : 0;
		for (size_t i = 0; i < whole.front_lights.size() / 3; i++) {
			if (!ThreeDVehicleShape::Shown(i < whole.front_part.size() ? whole.front_part[i] : 0, parts)) continue;
			add_light(whole.front_lights[i * 3], whole.front_lights[i * 3 + 1], whole.front_lights[i * 3 + 2], ThreeDPictureLight::Kind::Front);
			ahead = std::max(ahead, whole.front_lights[i * 3]);
			mid += whole.front_lights[i * 3 + 1] / n_front;
		}
		/* The pool on the way, on the ground before the lamps (not over the bonnet): near, and further on. */
		if (n_front > 0) {
			add_light(ahead + 5.0f, mid, 0.0f, ThreeDPictureLight::Kind::PoolNear);
			add_light(ahead + 11.0f, mid, 0.0f, ThreeDPictureLight::Kind::Pool);
		}
	}
	if ((lights & LIGHTS_BACK) != 0) {
		for (size_t i = 0; i * 3 + 2 < whole.back_lights.size(); i++) {
			if (!ThreeDVehicleShape::Shown(i < whole.back_part.size() ? whole.back_part[i] : 0, parts)) continue;
			add_light(whole.back_lights[i * 3], whole.back_lights[i * 3 + 1], whole.back_lights[i * 3 + 2], ThreeDPictureLight::Kind::Back);
		}
	}
	/* Going back first: the headlights on its back end with their pool behind it, the red lights on its front end. */
	if ((lights & LIGHTS_FRONT_AT_BACK) != 0 && !whole.front_back_lights.empty()) {
		float behind = std::numeric_limits<float>::infinity(), mid = 0;
		const size_t n = whole.front_back_lights.size() / 3;
		for (size_t i = 0; i < n; i++) {
			add_light(whole.front_back_lights[i * 3], whole.front_back_lights[i * 3 + 1], whole.front_back_lights[i * 3 + 2], ThreeDPictureLight::Kind::Front);
			behind = std::min(behind, whole.front_back_lights[i * 3]);
			mid += whole.front_back_lights[i * 3 + 1] / n;
		}
		add_light(behind - 5.0f, mid, 0.0f, ThreeDPictureLight::Kind::PoolNear);
		add_light(behind - 11.0f, mid, 0.0f, ThreeDPictureLight::Kind::Pool);
	}
	if ((lights & LIGHTS_BACK_AT_FRONT) != 0) {
		for (size_t i = 0; i * 3 + 2 < whole.back_front_lights.size(); i++) {
			add_light(whole.back_front_lights[i * 3], whole.back_front_lights[i * 3 + 1], whole.back_front_lights[i * 3 + 2], ThreeDPictureLight::Kind::Back);
		}
	}
}

struct GpuObjectPicture;
static bool PrepareGpuObject(const ThreeDVehicleObject &obj, const ThreeDVehicleShape &whole, float scale, float cam_az, float world_az, float along, float pxm, float fade, float rust_time, uint32_t tint, uint32_t paint_colour, float stretch, float spin, float pitch, float roll, int lights, int state, uint64_t walk, GpuObjectPicture &gp);
static bool FinishGpuObject(const GpuObjectPicture &gp, ObjectRaster &out);
static std::optional<bool> GpuRasterObject(const ThreeDVehicleObject &obj, const ThreeDVehicleShape &whole, float scale, float cam_az, float world_az, float along, float pxm, float fade, ObjectRaster &out, float rust_time, uint32_t tint, uint32_t paint_colour, float stretch, float spin, float pitch, float roll, int lights, int state, uint64_t walk);

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
 * @param rust_time how far into its rusting it is (RustTime())
 * @param tint the colour of the material an effect is coloured in ("barvy"), 0 none
 * @param paint_colour the vehicle's colour of its palette ("barvy", PaletteColour()), 0 the paint's own
 * @param stretch its height against its width (a plant of a list, "meritko" [x, y, z])
 * @param spin how far a thing that turns has turned about its axis, degrees (ThreeDVehicleObject::spin_axis)
 * @param lights at night the lights that are on (LIGHTS_FRONT, LIGHTS_BACK): their glass at full brightness over the
 *               body, its pixels marked lit, and the places they shine from (ThreeDVehicleShape::front_lights)
 * @param state the state its GRF picks (StateStep()): the parts it shows -- an add-on's node, a group of the crew --,
 *              the colours of the add-ons ("barvy_doplnku") and how far the whole object is turned ("otoceni")
 * @param walk the frames of its crew that walks (WalkCode())
 */
static bool RasterObject(const ThreeDVehicleObject &obj, float scale, float cam_az, float world_az, float along, float pxm, float fade, ObjectRaster &out, int frame = 0, float rust_time = 0, uint32_t tint = 0, uint32_t paint_colour = 0, float stretch = 1.0f, float spin = 0.0f, float pitch = 0.0f, float roll = 0.0f, int lights = 0, int state = 0, uint64_t walk = 0)
{
	const ThreeDVehicleShape *whole = obj.GetShape(frame, obj.PartsOf(state));
	if (whole == nullptr) return false;
	/* On the graphics card where there is one (three_d_gpu.h); what it cannot draw the processor draws. */
	if (ThreeDGpuAvailable()) {
		const std::optional<bool> drawn = GpuRasterObject(obj, *whole, scale, cam_az, world_az, along, pxm, fade, out, rust_time, tint, paint_colour, stretch, spin, pitch, roll, lights, state, walk);
		if (drawn.has_value()) return *drawn;
	}
	/* As detailed as the picture is fine (the colleague's lorries and wagons are in full detail, PROSBA point 12: "the
	 * game makes fewer triangles when it is far"): its corners joined about one and a half pixels apart (Simpler()).
	 * Its textures, lamps and glass stay the whole shape's. */
	const ThreeDVehicleShape *shape = whole->Simpler(1.5 / (pxm * SUPERSAMPLE * std::max(scale, 0.1f)));
	/* The parts the state shows; the whole object turned about z at its origin as the state has it (a cart pushed). */
	const ThreeDParts parts = obj.PartsOf(state);
	const ThreeDVehicleState *st = obj.State(state);
	const int ss = SUPERSAMPLE;
	const float p = pxm * ss;
	const ObjectCamera cam(obj, st, scale, cam_az, world_az, along, p, stretch, spin, pitch, roll);
	const bool lit = !shape->nrm.empty();

	const size_t n = shape->pos.size() / 3;
	std::vector<float> sx(n), sy(n), depth(n), light(n, 1.0f);
	float minx = std::numeric_limits<float>::infinity(), miny = minx, maxx = -minx, maxy = -minx;
	auto place = [&](float px, float py, float pz, float &osx, float &osy, float &od) { cam.Place(px, py, pz, osx, osy, od); };
	auto shade = [&](float nx, float ny, float nz) { return cam.Shade(nx, ny, nz); };
	/* The triangles drawn: of the parts the state shows (an object of seventy five loads draws one), the glass after
	 * the rest; only their corners are placed, and the picture is as big as they are. */
	std::vector<size_t> drawn_triangles, glass_triangles;
	std::vector<uint8_t> used(n, 0);
	for (size_t t = 0; t + 2 < shape->tri.size(); t += 3) {
		const uint8_t part = t / 3 < shape->tri_part.size() ? shape->tri_part[t / 3] : 0;
		const bool glass = shape->IsSeeThrough(t / 3);
		if (part != ThreeDVehicleShape::PART_GLASS && !ThreeDVehicleShape::Shown(part, parts)) continue;
		(glass ? glass_triangles : drawn_triangles).push_back(t);
		for (int c = 0; c < 3; c++) used[shape->tri[t + c]] = 1;
	}
	for (size_t i = 0; i < n; i++) {
		if (used[i] == 0) continue;
		cam.PlaceSpun(shape->pos[i * 3], shape->pos[i * 3 + 1], shape->pos[i * 3 + 2], sx[i], sy[i], depth[i]);
		minx = std::min(minx, sx[i]); maxx = std::max(maxx, sx[i]);
		miny = std::min(miny, sy[i]); maxy = std::max(maxy, sy[i]);
		if (lit) {
			float nx = shape->nrm[i * 3], ny = shape->nrm[i * 3 + 1], nz = shape->nrm[i * 3 + 2];
			if (spin != 0.0f) cam.Spin(nx, ny, nz);
			light[i] = shade(nx, ny, nz);
		}
	}
	/* The side of a triangle facing away left out where its shape knows its outer side (a car of BeamNG); placed with
	 * its length or height turned the other way, the picture is the model mirrored. */
	if (!shape->facing.empty()) {
		const int mirror = along * stretch < 0 ? -1 : 1;
		auto away = [&](size_t t) {
			const uint32_t a = shape->tri[t], b = shape->tri[t + 1], c = shape->tri[t + 2];
			const double cross = static_cast<double>(sx[b] - sx[a]) * (sy[c] - sy[a]) - static_cast<double>(sy[b] - sy[a]) * (sx[c] - sx[a]);
			return shape->FacesAway(t / 3, cross, mirror);
		};
		std::erase_if(drawn_triangles, away);
		std::erase_if(glass_triangles, away);
	}
	/* The crew that walks (the colleague's hra/3d/PROSBA-SPECIAL-3D.md): each figure the state shows at its frame of
	 * the way gone, turned, sized and stood at its place in the vehicle's frame, then as the vehicle. */
	struct CrewPiece {
		const ThreeDVehicleObject *fig;
		const ThreeDVehicleShape *shape;
		float scale;
		int state; ///< a piece of a load: its figure's state in the cargo's colour; 0 as it is
		std::vector<float> sx, sy, depth, light;
	};
	std::vector<CrewPiece> crew;
	auto add_piece = [&](const ThreeDVehicleObject *fig, const ThreeDVehicleShape *fs, const float at[3], float turn, float size, int piece_state) {
		CrewPiece &c = crew.emplace_back(CrewPiece{fig, fs, size, piece_state, {}, {}, {}, {}});
		const size_t m = fs->pos.size() / 3;
		const bool flit = lit && fs->nrm.size() == fs->pos.size();
		c.sx.resize(m);
		c.sy.resize(m);
		c.depth.resize(m);
		c.light.assign(m, 1.0f);
		const float wa = turn * static_cast<float>(M_PI) / 180.0f, wc = std::cos(wa), ws = std::sin(wa);
		for (size_t i = 0; i < m; i++) {
			const float fx0 = fs->pos[i * 3], fy0 = fs->pos[i * 3 + 1];
			place(at[0] + (fx0 * wc - fy0 * ws) * size, at[1] + (fx0 * ws + fy0 * wc) * size, at[2] + fs->pos[i * 3 + 2] * size, c.sx[i], c.sy[i], c.depth[i]);
			minx = std::min(minx, c.sx[i]); maxx = std::max(maxx, c.sx[i]);
			miny = std::min(miny, c.sy[i]); maxy = std::max(maxy, c.sy[i]);
			if (flit) {
				const float nx = fs->nrm[i * 3], ny = fs->nrm[i * 3 + 1];
				c.light[i] = shade(nx * wc - ny * ws, nx * ws + ny * wc, fs->nrm[i * 3 + 2]);
			}
		}
	};
	const std::vector<ThreeDWalkingCrew> &walking = obj.WalkingCrew();
	size_t slot = 0;
	for (size_t k = 0; k < walking.size(); k++) {
		const ThreeDWalkingCrew &w = walking[k];
		if (!ThreeDVehicleShape::Shown(w.part, parts)) continue;
		const size_t shown = slot++;
		const ThreeDVehicleShape *fs = w.figure->GetShape(shown < 8 ? static_cast<int>((walk >> (8 * shown)) & 0xFF) : 0);
		if (fs != nullptr) add_piece(w.figure, fs, w.at, w.turn, w.scale, 0);
	}
	/* The pieces of the shared loads on its bed (the colleague's hra/3d/TATRY-138-NAKLADY-PRO-HRU.md): each where it
	 * lies in the vehicle's frame, then as the vehicle; in the colour of the cargo. */
	if (std::vector<ThreeDLoadPieces> loads; GetThreeDLoadPieces(st, loads)) {
		for (const ThreeDLoadPieces &load : loads) {
			if (const ThreeDVehicleShape *ls = load.fig->GetShape(load.frame); ls != nullptr) {
				for (const ThreeDLoadPlace &at : load.places) add_piece(load.fig, ls, at.at, at.turn, at.scale, load.state);
			}
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

	/* The level of the textures: about a texel to a pixel, each its own. */
	int level = 0;
	for (float d = TexelsPerMetre(obj, *shape); d >= 2.0f * p && level < 12; d /= 2.0f) level++;
	auto level_of = [&](int k) {
		if (k == 0) return level;
		int l = 0;
		for (float d = TexelsPerMetre(obj, *shape, k); d >= 2.0f * p && l < 12; d /= 2.0f) l++;
		return l;
	};
	/* Only the textures of the triangles drawn (and the body's, for a triangle without one): those of the add-ons the
	 * state does not show are not made at all (an add-on's picture is of sixteen million pixels). */
	std::vector<const ThreeDObjectTexture *> texs(std::max<size_t>(1, whole->pngs.size()), nullptr);
	texs[0] = obj.GetTexture(level, 0);
	for (const std::vector<size_t> *list : {&drawn_triangles, &glass_triangles}) {
		for (size_t t : *list) {
			const size_t ti = t / 3 < shape->tri_tex.size() ? shape->tri_tex[t / 3] : 0;
			if (ti < texs.size() && texs[ti] == nullptr) texs[ti] = obj.GetTexture(level_of(static_cast<int>(ti)), static_cast<int>(ti));
		}
	}
	/* The paint of the body's texture: a colour by number, faded with age (the colleague's point 6); the rust after
	 * the paint has faded (the colleague: the paint fades, the rust grows over it). */
	const ObjectPaintOfState painted(obj, *whole, level, fade, rust_time, paint_colour);
	/* The masks of details in the vehicle's colours of its set (callback 1C1). */
	const ObjectDetailPaint details(obj, *whole, st, texs.size(), texs);
	/* The textures an effect is coloured in: those of its material ("barvy"), the texture times the colour. */
	const Colour tint_colour(tint);
	std::vector<bool> tinted(texs.size(), false);
	if (tint != 0) {
		for (size_t i = 0; i < texs.size() && i < whole->tex_names.size(); i++) tinted[i] = whole->tex_names[i] == obj.tint_material;
	}

	/* The add-ons' colours the state gives, each over its mask at the texture's lightness. */
	struct AddonPaint {
		const ThreeDObjectTexture *mask = nullptr;
		Colour colour;
		float start = 255.0f;
	};
	std::vector<AddonPaint> addon_paint(texs.size());
	for (size_t i = 0; i < texs.size() && i < whole->tex_addon.size(); i++) {
		const int a = whole->tex_addon[i];
		if (texs[i] == nullptr) continue; // not drawn
		if (a < 0 || st == nullptr || static_cast<size_t>(a) >= st->addon_colours.size() || st->addon_colours[a] == 0) continue;
		addon_paint[i].mask = obj.GetAddonMask(a, level);
		addon_paint[i].colour = Colour(st->addon_colours[a]);
		addon_paint[i].start = obj.addons[a].start > 0 ? static_cast<float>(obj.addons[a].start) : 255.0f;
	}

	std::vector<float> zb(static_cast<size_t>(width) * height, std::numeric_limits<float>::infinity());
	std::vector<Colour> buf(static_cast<size_t>(width) * height, Colour(0, 0, 0, 0));
	/* Each subpixel whether it is lit -- not darkened at night: of a material with a light of its own (tex_glow), or
	 * of the glass of a lamp on at night (below). */
	const bool glows = whole->Glows() || std::ranges::any_of(crew, [](const CrewPiece &c) { return c.shape->Glows(); });
	std::vector<uint8_t> litbuf(glows ? buf.size() : 0, 0);
	/* A texel lit by the light at it (k), and a material's own light over it, unshaded: the texel times its colour, or
	 * its colour alone. */
	auto shine = [](uint32_t glow, float cr, float cg, float cb, float k) {
		float r = cr * k, g = cg * k, b = cb * k;
		if (glow != 0) {
			const Colour e(glow);
			if (e.a == ThreeDVehicleShape::GLOW_TEXTURED) {
				r += e.r * cr / 255.0f;
				g += e.g * cg / 255.0f;
				b += e.b * cb / 255.0f;
			} else {
				r += e.r;
				g += e.g;
				b += e.b;
			}
		}
		return Colour(static_cast<uint8_t>(std::min(255.0f, r)), static_cast<uint8_t>(std::min(255.0f, g)), static_cast<uint8_t>(std::min(255.0f, b)));
	};
	/* A triangle of a shape through the depth: of the vehicle's (its body painted, its add-ons coloured, an effect
	 * tinted) or of its crew that walks (as it is). */
	auto fill = [&](const ThreeDVehicleShape &s, const std::vector<float> &sx, const std::vector<float> &sy, const std::vector<float> &depth, const std::vector<float> &light, const std::vector<const ThreeDObjectTexture *> &texs, size_t t, bool body, const ObjectPaintOfState *own = nullptr) {
		const uint32_t a = s.tri[t], b = s.tri[t + 1], c = s.tri[t + 2];
		const int ti = t / 3 < s.tri_tex.size() ? s.tri_tex[t / 3] : 0;
		const ThreeDObjectTexture *tex = texs[static_cast<size_t>(ti) < texs.size() ? ti : 0];
		if (tex == nullptr) return;
		const uint32_t glow = s.Glow(static_cast<size_t>(ti));
		const float ax = sx[a] - left, ay = sy[a] - top, bx = sx[b] - left, by = sy[b] - top, cx = sx[c] - left, cy = sy[c] - top;
		const float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
		if (std::abs(area) < 1e-6f) return;
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
				float u = w0 * s.uv[a * 2] + w1 * s.uv[b * 2] + w2 * s.uv[c * 2];
				float v = w0 * s.uv[a * 2 + 1] + w1 * s.uv[b * 2 + 1] + w2 * s.uv[c * 2 + 1];
				u -= std::floor(u);
				v -= std::floor(v);
				int tx = std::min(tex->w - 1, static_cast<int>(u * tex->w)), ty = std::min(tex->h - 1, static_cast<int>(v * tex->h));
				uint32_t texel = tex->px[static_cast<size_t>(ty) * tex->w + tx];
				if (texel == 0) continue; // a hole in the texture
				float k = w0 * light[a] + w1 * light[b] + w2 * light[c];
				Colour col(texel);
				float cr = col.r, cg = col.g, cb = col.b;
				if (!body) {
					if (own != nullptr && ti == 0) own->Paint(u, v, cr, cg, cb);
					zb[at] = dz;
					buf[at] = shine(glow, cr, cg, cb, k);
					if (!litbuf.empty()) litbuf[at] = glow != 0 ? 1 : 0;
					continue;
				}
				/* A detail the set colours in the vehicle's colours; else the colours of the state. */
				const ThreeDDetailMask *dm = details.MaskOf(ti);
				const bool by_detail = dm != nullptr && details.Paint(dm, u, v, cr, cg, cb);
				if (ti == 0 && !by_detail) painted.Paint(u, v, cr, cg, cb);
				if (!by_detail && static_cast<size_t>(ti) < addon_paint.size() && addon_paint[ti].mask != nullptr) {
					const AddonPaint &ap = addon_paint[ti];
					const int mx = std::min(ap.mask->w - 1, static_cast<int>(u * ap.mask->w)), my = std::min(ap.mask->h - 1, static_cast<int>(v * ap.mask->h));
					const float m = Colour(ap.mask->px[static_cast<size_t>(my) * ap.mask->w + mx]).r / 255.0f;
					if (m > 0) {
						const float l = std::max({cr, cg, cb}) / ap.start;
						cr += (std::min(255.0f, ap.colour.r * l) - cr) * m;
						cg += (std::min(255.0f, ap.colour.g * l) - cg) * m;
						cb += (std::min(255.0f, ap.colour.b * l) - cb) * m;
					}
				}
				if (tint != 0 && static_cast<size_t>(ti) < tinted.size() && tinted[ti]) {
					cr = cr * tint_colour.r / 255.0f;
					cg = cg * tint_colour.g / 255.0f;
					cb = cb * tint_colour.b / 255.0f;
				}
				if (ti == 0) painted.Rust(u, v, cr, cg, cb);
				zb[at] = dz;
				buf[at] = shine(glow, cr, cg, cb, k);
				if (!litbuf.empty()) litbuf[at] = glow != 0 ? 1 : 0;
			}
		}
	};
	for (size_t t : drawn_triangles) fill(*shape, sx, sy, depth, light, texs, t, true);
	for (const CrewPiece &c : crew) {
		/* Its textures at about a texel to a pixel, as big as it is drawn. */
		int crew_level = 0;
		for (float d = TexelsPerMetre(*c.fig, *c.shape); d >= 2.0f * p * c.scale && crew_level < 12; d /= 2.0f) crew_level++;
		std::vector<const ThreeDObjectTexture *> crew_texs(std::max<size_t>(1, c.shape->pngs.size()));
		for (size_t i = 0; i < crew_texs.size(); i++) crew_texs[i] = c.fig->GetTexture(crew_level, static_cast<int>(i));
		/* A piece of a load in its cargo's colour over its mask, as a vehicle's paint. */
		std::optional<ObjectPaintOfState> own;
		if (c.state != 0) own.emplace(*c.fig, *c.shape, crew_level, 0.0f, 0.0f, PaletteColour(c.fig, c.state));
		for (size_t t = 0; t + 2 < c.shape->tri.size(); t += 3) fill(*c.shape, c.sx, c.sy, c.depth, c.light, crew_texs, t, false, own.has_value() ? &*own : nullptr);
	}

	/* The glass of its windows ("sklo") and what is of a material "BLEND" (the ghost), after all else: see-through,
	 * each layer of it laying its colour over what is behind by its alpha, not hiding what is behind it (no depth
	 * written); over nothing, a pane of that alpha. */
	for (size_t t : glass_triangles) {
		const uint32_t a = shape->tri[t], b = shape->tri[t + 1], c = shape->tri[t + 2];
		const int ti = t / 3 < shape->tri_tex.size() ? shape->tri_tex[t / 3] : 0;
		const ThreeDObjectTexture *tex = texs[static_cast<size_t>(ti) < texs.size() ? ti : 0];
		const float material_alpha = shape->SeeThroughAlpha(t / 3);
		const bool by_texel = shape->BlendsByTexel(t / 3);
		const uint32_t glow = shape->Glow(static_cast<size_t>(ti));
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
				const float w0 = ((cx - bx) * (qy - by) - (cy - by) * (qx - bx)) * inv;
				const float w1 = ((ax - cx) * (qy - cy) - (ay - cy) * (qx - cx)) * inv;
				const float w2 = 1.0f - w0 - w1;
				if (w0 < 0 || w1 < 0 || w2 < 0) continue;
				const size_t at = static_cast<size_t>(py) * width + px;
				const float dz = w0 * depth[a] + w1 * depth[b] + w2 * depth[c];
				if (dz >= zb[at]) continue;
				float u = w0 * shape->uv[a * 2] + w1 * shape->uv[b * 2] + w2 * shape->uv[c * 2];
				float v = w0 * shape->uv[a * 2 + 1] + w1 * shape->uv[b * 2 + 1] + w2 * shape->uv[c * 2 + 1];
				u -= std::floor(u);
				v -= std::floor(v);
				const int tx = std::min(tex->w - 1, static_cast<int>(u * tex->w)), ty = std::min(tex->h - 1, static_cast<int>(v * tex->h));
				const Colour g(tex->px[static_cast<size_t>(ty) * tex->w + tx]);
				/* A material "BLEND": as see-through as its texel too (the saucer's beam fading to the ground). */
				const float glass_alpha = by_texel ? material_alpha * g.a / 255.0f : material_alpha;
				if (glass_alpha <= 0.0f) continue;
				const float k = w0 * light[a] + w1 * light[b] + w2 * light[c];
				const Colour shone = shine(glow, g.r, g.g, g.b, k);
				const float gr = shone.r, gg = shone.g, gb = shone.b;
				if (glow != 0 && !litbuf.empty()) litbuf[at] = 1;
				Colour &d = buf[at];
				if (d.a == 0) {
					d = Colour(static_cast<uint8_t>(gr), static_cast<uint8_t>(gg), static_cast<uint8_t>(gb), static_cast<uint8_t>(glass_alpha * 255.0f));
				} else {
					const float keep = 1.0f - glass_alpha;
					const float da = d.a / 255.0f, na = da + glass_alpha * (1.0f - da);
					d = Colour(static_cast<uint8_t>((d.r * da * keep + gr * glass_alpha) / na), static_cast<uint8_t>((d.g * da * keep + gg * glass_alpha) / na), static_cast<uint8_t>((d.b * da * keep + gb * glass_alpha) / na), static_cast<uint8_t>(std::min(255.0f, na * 255.0f)));
				}
			}
		}
	}

	/* At night the glass of its lights that are on (the colleague's hra/3d/PROSBA-SVETLA-VOZIDEL-UZLY.md): over the
	 * body as it is turned and leant, in its material's colour unshaded, through the depth (it lies a few millimetres
	 * before the body), each subpixel of it marked lit; and where its lights are, the pool of the headlights ahead. */
	if (lights != 0 && !whole->night_tri.empty()) {
		if (litbuf.empty()) litbuf.assign(static_cast<size_t>(width) * height, 0);
		const size_t corners = whole->night_pos.size() / 3;
		std::vector<float> nx(corners), ny(corners), nd(corners);
		for (size_t i = 0; i < corners; i++) place(whole->night_pos[i * 3], whole->night_pos[i * 3 + 1], whole->night_pos[i * 3 + 2], nx[i], ny[i], nd[i]);
		const float bias = 0.03f * scale; // a few centimetres: the glass is drawn over the lamp it lies on
		for (size_t t = 0; t + 2 < whole->night_tri.size(); t += 3) {
			const size_t tn = t / 3;
			if (!NightGlassLit(*whole, tn, lights)) continue;
			/* The lamps of a part only where the part is (the cab's headlights only on the cab shown). */
			if (!ThreeDVehicleShape::Shown(tn < whole->night_part.size() ? whole->night_part[tn] : 0, parts)) continue;
			const Colour glass(tn < whole->night_colour.size() ? whole->night_colour[tn] : Colour(255, 244, 216).data);
			const uint32_t a = whole->night_tri[t], b = whole->night_tri[t + 1], c = whole->night_tri[t + 2];
			if (a >= corners || b >= corners || c >= corners) continue;
			const float ax = nx[a] - left, ay = ny[a] - top, bx = nx[b] - left, by = ny[b] - top, cx = nx[c] - left, cy = ny[c] - top;
			const float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
			if (std::abs(area) < 1e-6f) continue;
			const float inv = 1.0f / area;
			int x0 = std::max(0, static_cast<int>(std::floor(std::min({ax, bx, cx})))), x1 = std::min(width - 1, static_cast<int>(std::ceil(std::max({ax, bx, cx}))));
			int y0 = std::max(0, static_cast<int>(std::floor(std::min({ay, by, cy})))), y1 = std::min(height - 1, static_cast<int>(std::ceil(std::max({ay, by, cy}))));
			for (int py = y0; py <= y1; py++) {
				const float qy = py + 0.5f;
				for (int px = x0; px <= x1; px++) {
					const float qx = px + 0.5f;
					const float w0 = ((cx - bx) * (qy - by) - (cy - by) * (qx - bx)) * inv;
					const float w1 = ((ax - cx) * (qy - cy) - (ay - cy) * (qx - cx)) * inv;
					const float w2 = 1.0f - w0 - w1;
					if (w0 < 0 || w1 < 0 || w2 < 0) continue;
					const size_t at = static_cast<size_t>(py) * width + px;
					const float dz = w0 * nd[a] + w1 * nd[b] + w2 * nd[c];
					if (dz >= zb[at] + bias) continue;
					zb[at] = std::min(zb[at], dz);
					buf[at] = Colour(glass.r, glass.g, glass.b);
					litbuf[at] = 1;
				}
			}
		}
	}
	ObjectLightsOn(*whole, parts, lights, cam, out.lights);

	/* Shrink: the mean of the covered subpixels by their alpha, the alpha from how much they cover (a pane of glass over
	 * nothing a part); lit where any of them is (a lamp far off smaller than a pixel still shines). */
	out.width = width / ss;
	out.height = height / ss;
	out.left = left / ss;
	out.top = top / ss;
	out.px.assign(static_cast<size_t>(out.width) * out.height, Colour(0, 0, 0, 0));
	out.lit.assign(litbuf.empty() ? 0 : out.px.size(), 0);
	bool any = false;
	for (int y = 0; y < out.height; y++) {
		for (int x = 0; x < out.width; x++) {
			uint r = 0, g = 0, bl = 0, cover = 0, glass = 0;
			for (int dy = 0; dy < ss; dy++) {
				for (int dx = 0; dx < ss; dx++) {
					const size_t at = static_cast<size_t>(y * ss + dy) * width + (x * ss + dx);
					Colour cc = buf[at];
					if (cc.a == 0) continue;
					r += cc.r * cc.a; g += cc.g * cc.a; bl += cc.b * cc.a; cover += cc.a;
					if (!litbuf.empty() && litbuf[at] != 0) glass++;
				}
			}
			if (cover == 0) continue;
			any = true;
			out.px[static_cast<size_t>(y) * out.width + x] = Colour(r / cover, g / cover, bl / cover, static_cast<uint8_t>(cover / (ss * ss)));
			if (glass > 0) out.lit[static_cast<size_t>(y) * out.width + x] = 1;
		}
	}
	/* Not a pixel of it seen (the colleague's lorry without its trailer: the trailer's main file is two transparent
	 * triangles of a millimetre, its state shows nothing): no picture, and no lights of it. */
	if (!any) return false;
	return true;
}

/*
 * The colours of the details of a vehicle's set as the graphics card paints them (ThreeDGpuPaint::detail_colours): by
 * the set's colours, a picture of each mask index (a row) at each lightness (a column) -- the colour ShadeTrueColour()
 * gives, or for a detail turned its turn, saturation and value --, as ObjectDetailPaint::Paint() picks the first detail
 * of the index. Kept by the colours themselves; let go of when the objects are read anew.
 */
static std::vector<std::pair<TrueColourSet, std::shared_ptr<const ThreeDObjectTexture>>> _gpu_detail_colours;
static std::mutex _gpu_detail_colours_mutex;

/** The picture of the colours of the details of a set (_gpu_detail_colours), made the first time. */
static std::shared_ptr<const ThreeDObjectTexture> GpuDetailColoursOf(const TrueColourSet &colours)
{
	std::lock_guard lock(_gpu_detail_colours_mutex);
	for (const auto &[known, made] : _gpu_detail_colours) {
		if (known == colours) return made;
	}
	auto t = std::make_shared<ThreeDObjectTexture>();
	t->w = 256;
	t->h = 256;
	t->px.assign(256 * 256, 0);
	std::vector<bool> taken(256, false);
	for (const TrueColourRange &range : colours) {
		for (uint m = range.first; m < range.first + TRUE_COLOUR_RANGE_SIZE && m < 256; m++) {
			if (m == 0 || taken[m]) continue; // the first detail of an index paints it; 0 none
			taken[m] = true;
			uint32_t *row = &t->px[static_cast<size_t>(m) * 256];
			if (range.hue) {
				row[0] = Colour(static_cast<uint8_t>(range.hue_turn + 128), range.hue_sat, range.hue_val, 128).data;
				for (int l = 1; l < 256; l++) row[l] = Colour(0, 0, 0, 128).data;
				continue;
			}
			for (int l = 0; l < 256; l++) {
				Colour c = ShadeTrueColour(range, static_cast<uint8_t>(l));
				c.a = 255;
				row[l] = c.data;
			}
		}
	}
	if (_gpu_detail_colours.size() > 64) _gpu_detail_colours.clear();
	_gpu_detail_colours.emplace_back(colours, t);
	return t;
}

/** The objects are read anew: the colours of their sets' details go with them. */
static void ForgetGpuDetailColours()
{
	std::lock_guard lock(_gpu_detail_colours_mutex);
	_gpu_detail_colours.clear();
}

/**
 * How the graphics card paints a texture of an object as it draws it (GpuRasterObject()): as RasterObject() paints a
 * pixel over it -- the details in the vehicle's colours of its set, the body's paint faded, the add-ons' colours, an
 * effect's tint, the rust --, each texel on the card as it is drawn. It was painted on the processor once for each
 * state, level and colour, a texture of a lorry tens of megabytes (the player, 18. 10.: "the paint on the card").
 * @param held keeps the colours of the details while the picture is drawn
 */
static ThreeDGpuPaint GpuPaintOf(const ThreeDVehicleObject &obj, const ThreeDVehicleShape &whole, const ThreeDVehicleState *st, int index, const ObjectPaintOfState &painted, const ObjectDetailPaint &details, uint32_t tint, std::vector<std::shared_ptr<const ThreeDObjectTexture>> &held)
{
	ThreeDGpuPaint g;
	auto rgb = [](Colour c) { return std::array<float, 3>{static_cast<float>(c.r), static_cast<float>(c.g), static_cast<float>(c.b)}; };
	if (const ThreeDDetailMask *dm = details.MaskOf(index); dm != nullptr && details.colours != nullptr) {
		std::shared_ptr<const ThreeDObjectTexture> lut = GpuDetailColoursOf(*details.colours);
		g.detail = dm;
		g.detail_colours = lut.get();
		held.push_back(std::move(lut));
	}
	const bool body = index == 0;
	if (body && (painted.mask != nullptr || painted.whole)) {
		g.body = true;
		g.paint_mask = painted.mask;
		g.coloured = painted.coloured;
		g.colour = rgb(painted.colour);
		g.target = rgb(painted.target);
		g.start = painted.start;
		g.fade = painted.fade;
	}
	const int a = static_cast<size_t>(index) < whole.tex_addon.size() ? whole.tex_addon[index] : -1;
	if (a >= 0 && st != nullptr && static_cast<size_t>(a) < st->addon_colours.size() && st->addon_colours[a] != 0) {
		g.addon_mask = obj.GetAddonMask(a, painted.level);
		g.addon_colour = rgb(Colour(st->addon_colours[a]));
		g.addon_start = obj.addons[a].start > 0 ? static_cast<float>(obj.addons[a].start) : 255.0f;
	}
	if (tint != 0 && static_cast<size_t>(index) < whole.tex_names.size() && whole.tex_names[index] == obj.tint_material) {
		g.tinted = true;
		g.tint = rgb(Colour(tint));
	}
	if (body && painted.rust_mask != nullptr) {
		g.rust_mask = painted.rust_mask;
		g.rust_colour = rgb(painted.rust_colour);
		g.rust_time = painted.rust_time;
		g.rust_ramp = obj.rust.ramp;
		g.rust_gradual = obj.rust.gradual;
	}
	return g;
}

/** The extent of all the corners of a shape: x0, x1, y0, y1, z0, z1 (ThreeDVehicleShape::all, else counted). */
static void ShapeExtent(const ThreeDVehicleShape &s, float e[6])
{
	if (s.all[1] > s.all[0] || s.all[3] > s.all[2] || s.all[5] > s.all[4]) {
		std::copy(std::begin(s.all), std::end(s.all), e);
		return;
	}
	for (int k = 0; k < 3; k++) {
		e[k * 2] = std::numeric_limits<float>::infinity();
		e[k * 2 + 1] = -std::numeric_limits<float>::infinity();
	}
	for (size_t i = 0; i + 2 < s.pos.size(); i += 3) {
		for (int k = 0; k < 3; k++) {
			e[k * 2] = std::min(e[k * 2], s.pos[i + k]);
			e[k * 2 + 1] = std::max(e[k * 2 + 1], s.pos[i + k]);
		}
	}
}

/**
 * A figure's picture for the map no more pixels across than this as it is drawn (SUPERSAMPLE times its own): one bigger
 * is made at a coarser zoom and its pixels doubled on the map (FigurePictureZoom()), as the map's sprites at 16x are
 * doubled from 8x. A dragon three times its size zoomed 16 times in was 5608 x 4188, some 90 MB read back from the
 * card at once and 47 MB kept, each frame of its flight a new one (the player, 24. 10.: the phone went down as he
 * zoomed in on the dragon over the sea).
 */
static constexpr float FIGURE_PICTURE_SIDE = 2048.0f;

/**
 * The zoom a figure's picture for the map is made at: the map's, or coarser while the picture would be more than
 * FIGURE_PICTURE_SIDE across.
 * @param[out] up how many times coarser: its pixels drawn doubled so many times (AddTurnedVehiclePicture())
 */
static ZoomLevel FigurePictureZoom(const ThreeDVehicleShape &shape, float scale, ZoomLevel zoom, int &up)
{
	float e[6];
	ShapeExtent(shape, e);
	/* Seen from above at the camera's angle: as wide as its diagonal, as tall as that foreshortened and its height. */
	const float across = std::hypot(e[1] - e[0], e[3] - e[2]), tall = std::max(0.0f, e[5] - e[4]);
	const float side_m = std::max(across, across * CAM_SIN + tall * CAM_COS) * scale * SUPERSAMPLE;
	up = 0;
	while (zoom < ZoomLevel::Max && side_m * PXM_16X / (1 << to_underlying(zoom)) > FIGURE_PICTURE_SIDE) {
		zoom = static_cast<ZoomLevel>(to_underlying(zoom) + 1);
		up++;
	}
	return zoom;
}

/** A picture of an object for the card (PrepareGpuObject()), drawn with others at once (ThreeDGpuRender()), shrunk to its pixels (FinishGpuObject()). */
struct GpuObjectPicture {
	ThreeDGpuPicture pic;
	std::vector<std::shared_ptr<const ThreeDObjectTexture>> held; ///< the colours of its details, kept till it is drawn
	int left = 0, top = 0;                  ///< where its subpixels begin
	std::vector<ThreeDPictureLight> lights; ///< its lights on at night (ObjectLightsOn())
};

/**
 * RasterObject() on the graphics card (three_d_gpu.h): the same picture -- the camera, the light, the paint, the
 * glass, the crew that walks, the lights at night, at SUPERSAMPLE times the size and shrunk the same way, of the shape
 * as simple as the picture is small -- its triangles drawn by the card instead of one by one here. The picture is as
 * big as the boxes of what is drawn are seen, then cut to its pixels (FinishGpuObject()). Made ready here, drawn by
 * the card with the others asked for at once (ThreeDGpuRender()).
 * @return whether there is a picture to draw
 */
static bool PrepareGpuObject(const ThreeDVehicleObject &obj, const ThreeDVehicleShape &whole, float scale, float cam_az, float world_az, float along, float pxm, float fade, float rust_time, uint32_t tint, uint32_t paint_colour, float stretch, float spin, float pitch, float roll, int lights, int state, uint64_t walk, GpuObjectPicture &gp)
{
	const ThreeDParts parts = obj.PartsOf(state);
	const ThreeDVehicleState *st = obj.State(state);
	const int ss = SUPERSAMPLE;
	const float p = pxm * ss;
	const ObjectCamera cam(obj, st, scale, cam_az, world_az, along, p, stretch, spin, pitch, roll);
	/* Its corners joined about one and a half pixels apart, as RasterObject() draws it (Simpler()). */
	const ThreeDVehicleShape &shape = *whole.Simpler(1.5 / (pxm * SUPERSAMPLE * std::max(scale, 0.1f)));
	const bool lit = !shape.nrm.empty();

	ThreeDGpuPicture &pic = gp.pic;
	pic.glass_alpha = whole.glass_alpha;
	std::vector<std::shared_ptr<const ThreeDObjectTexture>> &held = gp.held;
	float minx = std::numeric_limits<float>::infinity(), miny = minx, mind = minx, maxx = -minx, maxy = -minx, maxd = -minx;
	auto grow = [&](float x, float y, float d) {
		minx = std::min(minx, x); maxx = std::max(maxx, x);
		miny = std::min(miny, y); maxy = std::max(maxy, y);
		mind = std::min(mind, d); maxd = std::max(maxd, d);
	};
	/* A shape's frame on the picture is affine: its matrix from where the origin and the unit vectors are seen; what of
	 * the shape is seen lies within its box's corners seen. */
	auto matrix_of = [](auto &&to, ThreeDGpuMatrix &m) {
		float ox, oy, od;
		to(0.0f, 0.0f, 0.0f, ox, oy, od);
		m = {};
		for (int a = 0; a < 3; a++) {
			float x, y, d;
			to(a == 0 ? 1.0f : 0.0f, a == 1 ? 1.0f : 0.0f, a == 2 ? 1.0f : 0.0f, x, y, d);
			m[a] = x - ox;
			m[4 + a] = y - oy;
			m[8 + a] = d - od;
		}
		m[3] = ox;
		m[7] = oy;
		m[11] = od;
		m[15] = 1.0f;
	};
	auto box = [&](const ThreeDVehicleShape &s, auto &&to) {
		float e[6];
		ShapeExtent(s, e);
		if (!(e[1] >= e[0])) return;
		for (int i = 0; i < 8; i++) {
			float x, y, d;
			to(e[(i & 1) ? 1 : 0], e[(i & 2) ? 3 : 2], e[(i & 4) ? 5 : 4], x, y, d);
			grow(x, y, d);
		}
	};
	/* The light at a corner from its normal: n . light, the linear part of ObjectCamera::LightAt(). */
	auto light_of = [](auto &&at, std::array<float, 3> &l) {
		for (int a = 0; a < 3; a++) l[a] = at(a == 0 ? 1.0f : 0.0f, a == 1 ? 1.0f : 0.0f, a == 2 ? 1.0f : 0.0f);
	};

	/* The vehicle, turned about its axis if it turns; its textures at about a texel to a pixel, painted as its state has it. */
	auto vehicle_to = [&cam](float x, float y, float z, float &ox, float &oy, float &od) { cam.PlaceSpun(x, y, z, ox, oy, od); };
	{
		ThreeDGpuMesh &m = pic.meshes.emplace_back();
		m.shape = &shape;
		m.triangles = ThreeDGpuTrianglesOf(shape, parts, true, whole.pngs.size());
		matrix_of(vehicle_to, m.matrix);
		m.lit = lit;
		light_of([&cam, spin](float nx, float ny, float nz) {
			if (spin != 0.0f) cam.Spin(nx, ny, nz);
			return cam.LightAt(nx, ny, nz);
		}, m.light);
		box(whole, vehicle_to);
		int level = 0;
		for (float d = TexelsPerMetre(obj, shape); d >= 2.0f * p && level < 12; d /= 2.0f) level++;
		/* Its textures as they are, each at its own level, painted by the card as they are drawn (GpuPaintOf()). */
		m.textures.assign(m.triangles->used.size(), nullptr);
		for (size_t i = 0; i < m.textures.size(); i++) {
			if (!m.triangles->used[i]) continue;
			int l = level;
			if (i != 0) {
				l = 0;
				for (float d = TexelsPerMetre(obj, shape, static_cast<int>(i)); d >= 2.0f * p && l < 12; d /= 2.0f) l++;
			}
			m.textures[i] = obj.GetTexture(l, static_cast<int>(i));
		}
		const ObjectPaintOfState painted(obj, whole, level, fade, rust_time, paint_colour);
		const ObjectDetailPaint details(obj, whole, st, std::max<size_t>(1, whole.pngs.size()), m.textures);
		m.paints.resize(m.textures.size());
		for (size_t i = 0; i < m.textures.size(); i++) {
			if (m.textures[i] != nullptr) m.paints[i] = GpuPaintOf(obj, whole, st, static_cast<int>(i), painted, details, tint, held);
		}
	}
	/* The crew that walks: each figure the state shows at its frame of the way gone, turned, sized and stood at its
	 * place in the vehicle's frame, then as the vehicle; its textures as they are. */
	const std::vector<ThreeDWalkingCrew> &walking = obj.WalkingCrew();
	size_t slot = 0;
	for (size_t k = 0; k < walking.size(); k++) {
		const ThreeDWalkingCrew &w = walking[k];
		if (!ThreeDVehicleShape::Shown(w.part, parts)) continue;
		const size_t shown = slot++;
		const ThreeDVehicleShape *fs = w.figure->GetShape(shown < 8 ? static_cast<int>((walk >> (8 * shown)) & 0xFF) : 0);
		if (fs == nullptr) continue;
		const float wa = w.turn * static_cast<float>(M_PI) / 180.0f, wc = std::cos(wa), ws = std::sin(wa);
		auto crew_to = [&cam, &w, wc, ws](float fx0, float fy0, float fz0, float &ox, float &oy, float &od) {
			cam.Place(w.at[0] + (fx0 * wc - fy0 * ws) * w.scale, w.at[1] + (fx0 * ws + fy0 * wc) * w.scale, w.at[2] + fz0 * w.scale, ox, oy, od);
		};
		ThreeDGpuMesh &m = pic.meshes.emplace_back();
		m.shape = fs;
		m.triangles = ThreeDGpuTrianglesOf(*fs, 0, false, fs->pngs.size());
		matrix_of(crew_to, m.matrix);
		m.lit = lit && fs->nrm.size() == fs->pos.size();
		light_of([&cam, wc, ws](float nx, float ny, float nz) { return cam.LightAt(nx * wc - ny * ws, nx * ws + ny * wc, nz); }, m.light);
		box(*fs, crew_to);
		int crew_level = 0;
		for (float d = TexelsPerMetre(*w.figure, *fs); d >= 2.0f * p * w.scale && crew_level < 12; d /= 2.0f) crew_level++;
		m.textures.assign(m.triangles->used.size(), nullptr);
		for (size_t i = 0; i < m.textures.size(); i++) {
			if (m.triangles->used[i]) m.textures[i] = w.figure->GetTexture(crew_level, static_cast<int>(i));
		}
	}
	/* The pieces of the shared loads on its bed, each where it lies, as the vehicle; in the cargo's colour over the
	 * piece's mask, painted by the card as a vehicle's paint. */
	std::vector<ThreeDLoadPieces> loads;
	GetThreeDLoadPieces(st, loads);
	for (const ThreeDLoadPieces &load : loads) {
		if (const ThreeDVehicleShape *ls = load.fig->GetShape(load.frame); ls != nullptr) {
			/* Its textures as fine as its biggest one needs (a girl on a bench twice her size). */
			float biggest = 1.0f;
			for (const ThreeDLoadPlace &at : load.places) biggest = std::max(biggest, at.scale);
			int load_level = 0;
			for (float d = TexelsPerMetre(*load.fig, *ls); d >= 2.0f * p * biggest && load_level < 12; d /= 2.0f) load_level++;
			std::shared_ptr<const ThreeDGpuTriangles> triangles = ThreeDGpuTrianglesOf(*ls, 0, false, ls->pngs.size());
			std::vector<const ThreeDObjectTexture *> textures(triangles->used.size(), nullptr);
			for (size_t i = 0; i < textures.size(); i++) {
				if (triangles->used[i]) textures[i] = load.fig->GetTexture(load_level, static_cast<int>(i));
			}
			std::vector<ThreeDGpuPaint> paints;
			if (load.state != 0) {
				const ObjectPaintOfState painted(*load.fig, *ls, load_level, 0.0f, 0.0f, PaletteColour(load.fig, load.state));
				const ObjectDetailPaint details(*load.fig, *ls, nullptr, std::max<size_t>(1, ls->pngs.size()), textures);
				paints.resize(textures.size());
				for (size_t i = 0; i < textures.size(); i++) {
					if (textures[i] != nullptr) paints[i] = GpuPaintOf(*load.fig, *ls, load.fig->State(load.state), static_cast<int>(i), painted, details, 0, held);
				}
			}
			for (const ThreeDLoadPlace &at : load.places) {
				const float wa = at.turn * static_cast<float>(M_PI) / 180.0f, wc = std::cos(wa), ws = std::sin(wa);
				auto piece_to = [&cam, &at, wc, ws](float fx0, float fy0, float fz0, float &ox, float &oy, float &od) {
					cam.Place(at.at[0] + (fx0 * wc - fy0 * ws) * at.scale, at.at[1] + (fx0 * ws + fy0 * wc) * at.scale, at.at[2] + fz0 * at.scale, ox, oy, od);
				};
				ThreeDGpuMesh &m = pic.meshes.emplace_back();
				m.shape = ls;
				m.triangles = triangles;
				matrix_of(piece_to, m.matrix);
				m.lit = lit && ls->nrm.size() == ls->pos.size();
				light_of([&cam, wc, ws](float nx, float ny, float nz) { return cam.LightAt(nx * wc - ny * ws, nx * ws + ny * wc, nz); }, m.light);
				box(*ls, piece_to);
				m.textures = textures;
				m.paints = paints;
			}
		}
	}
	/* At night the glass of its lights that are on, over the body as it is turned and leant, a few centimetres before
	 * it (it lies on the lamp). */
	const bool night = lights != 0 && !whole.night_tri.empty();
	if (night) {
		const size_t corners = whole.night_pos.size() / 3;
		std::vector<float> nx(corners), ny(corners), nd(corners);
		for (size_t i = 0; i < corners; i++) cam.Place(whole.night_pos[i * 3], whole.night_pos[i * 3 + 1], whole.night_pos[i * 3 + 2], nx[i], ny[i], nd[i]);
		const float bias = 0.03f * scale;
		for (size_t t = 0; t + 2 < whole.night_tri.size(); t += 3) {
			const size_t tn = t / 3;
			if (!NightGlassLit(whole, tn, lights)) continue;
			if (!ThreeDVehicleShape::Shown(tn < whole.night_part.size() ? whole.night_part[tn] : 0, parts)) continue;
			const uint32_t a = whole.night_tri[t], b = whole.night_tri[t + 1], c = whole.night_tri[t + 2];
			if (a >= corners || b >= corners || c >= corners) continue;
			ThreeDGpuNightTriangle &g = pic.night.emplace_back();
			const uint32_t abc[3] = {a, b, c};
			for (int i = 0; i < 3; i++) {
				g.x[i] = nx[abc[i]];
				g.y[i] = ny[abc[i]];
				g.depth[i] = nd[abc[i]] - bias;
				grow(g.x[i], g.y[i], g.depth[i]);
			}
			g.colour = tn < whole.night_colour.size() ? whole.night_colour[tn] : Colour(255, 244, 216).data;
		}
	}
	if (minx > maxx) return false;
	/* As RasterObject() aligns its picture: its subpixels in whole pixels of the picture shrunk. */
	int left = static_cast<int>(std::floor(minx)), top = static_cast<int>(std::floor(miny));
	left -= ((left % ss) + ss) % ss;
	top -= ((top % ss) + ss) % ss;
	int width = static_cast<int>(std::ceil(maxx)) - left + 1, height = static_cast<int>(std::ceil(maxy)) - top + 1;
	width += (ss - width % ss) % ss;
	height += (ss - height % ss) % ss;
	if (width <= 0 || height <= 0 || static_cast<size_t>(width) * height > 64u * 1024 * 1024) return false;
	pic.width = width;
	pic.height = height;
	const float margin = 0.01f * (maxd - mind) + 0.01f;
	pic.depth_near = mind - margin;
	pic.depth_far = maxd + margin;
	for (ThreeDGpuMesh &m : pic.meshes) {
		m.matrix[3] -= left;
		m.matrix[7] -= top;
	}
	for (ThreeDGpuNightTriangle &g : pic.night) {
		for (int i = 0; i < 3; i++) {
			g.x[i] -= left;
			g.y[i] -= top;
		}
	}
	/* Lit where a material has a light of its own (tex_glow), day and night: the card marks those pixels too. */
	const bool glows = std::ranges::any_of(pic.meshes, [](const ThreeDGpuMesh &m) { return m.shape->Glows(); });
	pic.read_lit = night || glows;
	gp.left = left;
	gp.top = top;
	ObjectLightsOn(whole, parts, lights, cam, gp.lights);
	return true;
}

/** A picture of an object the card drew (PrepareGpuObject()) shrunk as RasterObject() does, cut to its pixels: false when not a pixel of it is seen. */
static bool FinishGpuObject(const GpuObjectPicture &gp, ObjectRaster &out)
{
	const ThreeDGpuPicture &pic = gp.pic;
	const int ss = SUPERSAMPLE;
	const int width = pic.width, height = pic.height, left = gp.left, top = gp.top;
	/* Shrink as RasterObject() does: the mean of the covered subpixels by their alpha (the card's colours have it
	 * multiplied in), the alpha from how much they cover; lit where any is. Then cut to the pixels drawn. */
	const int ow = width / ss, oh = height / ss;
	std::vector<Colour> px(static_cast<size_t>(ow) * oh, Colour(0, 0, 0, 0));
	std::vector<uint8_t> lit_px(pic.read_lit ? px.size() : 0, 0);
	int x0 = ow, y0 = oh, x1 = -1, y1 = -1;
	for (int y = 0; y < oh; y++) {
		for (int x = 0; x < ow; x++) {
			uint r = 0, g = 0, bl = 0, cover = 0, glass = 0;
			for (int dy = 0; dy < ss; dy++) {
				for (int dx = 0; dx < ss; dx++) {
					const size_t at = static_cast<size_t>(y * ss + dy) * width + (x * ss + dx);
					const Colour cc(pic.pixels[at]);
					if (cc.a == 0) continue;
					r += cc.r * 255u; g += cc.g * 255u; bl += cc.b * 255u; cover += cc.a;
					if (!pic.lit.empty() && pic.lit[at] != 0) glass++;
				}
			}
			if (cover == 0) continue;
			px[static_cast<size_t>(y) * ow + x] = Colour(std::min(255u, r / cover), std::min(255u, g / cover), std::min(255u, bl / cover), static_cast<uint8_t>(cover / (ss * ss)));
			if (glass > 0 && !lit_px.empty()) lit_px[static_cast<size_t>(y) * ow + x] = 1;
			x0 = std::min(x0, x); x1 = std::max(x1, x);
			y0 = std::min(y0, y); y1 = std::max(y1, y);
		}
	}
	/* Not a pixel of it seen: no picture, and no lights of it. */
	if (x1 < x0) return false;
	out.left = left / ss + x0;
	out.top = top / ss + y0;
	out.width = x1 - x0 + 1;
	out.height = y1 - y0 + 1;
	out.px.resize(static_cast<size_t>(out.width) * out.height);
	out.lit.assign(pic.read_lit ? out.px.size() : 0, 0);
	for (int y = 0; y < out.height; y++) {
		std::copy_n(px.begin() + static_cast<size_t>(y + y0) * ow + x0, out.width, out.px.begin() + static_cast<size_t>(y) * out.width);
		if (pic.read_lit) std::copy_n(lit_px.begin() + static_cast<size_t>(y + y0) * ow + x0, out.width, out.lit.begin() + static_cast<size_t>(y) * out.width);
	}
	out.lights = gp.lights;
	return true;
}

/**
 * RasterObject() on the graphics card, a picture alone (PrepareGpuObject()).
 * @return whether there is a picture; nullopt: the card could not draw it (a texture bigger than it holds), the
 *         processor draws it
 */
static std::optional<bool> GpuRasterObject(const ThreeDVehicleObject &obj, const ThreeDVehicleShape &whole, float scale, float cam_az, float world_az, float along, float pxm, float fade, ObjectRaster &out, float rust_time, uint32_t tint, uint32_t paint_colour, float stretch, float spin, float pitch, float roll, int lights, int state, uint64_t walk)
{
	GpuObjectPicture gp;
	if (!PrepareGpuObject(obj, whole, scale, cam_az, world_az, along, pxm, fade, rust_time, tint, paint_colour, stretch, spin, pitch, roll, lights, state, walk, gp)) return false;
	ThreeDGpuPicture *one = &gp.pic;
	if (!ThreeDGpuRender({&one, 1}) || !gp.pic.drawn) return std::nullopt;
	return FinishGpuObject(gp, out);
}

/**
 * The pixels of a sprite of an object (MakeThreeDVehicleObjectSprite()): at 16x for the map, the levels coarser made
 * from it as from a set's zin16; for the windows (ObjectSpriteRequest::gui) at the zoom of the interface, not at 16x
 * and shrunk. On either thread.
 */
static bool RasterObjectSprite(const ObjectSpriteRequest &req, ZoomLevel zoom, float fade, float rust_time, uint32_t paint_colour, ObjectRaster &r)
{
	const float along = (SqueezedType(req.obj->type) && (req.dir & 1)) ? SQUEEZE : 1.0f;
	const float az = ViewAzimuth(req.dir);
	return RasterObject(*req.obj, req.scale, az, az, along, PXM_16X / (1 << to_underlying(zoom)), fade, r, 0, rust_time, 0, paint_colour, 1.0f, 0.0f, 0.0f, 0.0f, 0, req.fade, req.walk) && r.width <= UINT16_MAX && r.height <= UINT16_MAX;
}

/** The zoom a sprite of an object is made at: 16x for the map, the interface's for the windows. */
static ZoomLevel ObjectSpriteZoom(const ObjectSpriteRequest &req)
{
	return req.gui ? _gui_zoom : ZoomLevel::Min;
}

/** A sprite of an object made on the thread (ThreeDVehicleObjectSpriteStandIn()), for MakeThreeDVehicleObjectSprite() to take. */
struct SpriteMadeInBackground {
	ObjectRaster raster;
	ZoomLevel zoom = ZoomLevel::Min; ///< the zoom it was made at (ObjectSpriteZoom())
	bool drawn = false;              ///< false: not a pixel of the object seen
};
/** The sprites made on the thread, by their request; those asked of it. Under _bg_sprites_mutex. */
static std::map<uint32_t, SpriteMadeInBackground> _bg_sprites_made;
static std::set<uint32_t> _bg_sprites_asked;
static std::mutex _bg_sprites_mutex;
/** Some came: the windows, and the vehicles of the map, that draw them are drawn again (_owed_pictures_timer). */
static std::atomic<bool> _gui_sprites_came = false;
static std::atomic<bool> _map_sprites_came = false;

/**
 * What stands in on the map for a sprite of an object not made yet: the vehicle's sprite of the nearest direction made
 * (a vehicle turning shows the way it faced a moment longer), else nothing (the first of its object).
 */
static SpriteID NearestObjectSpriteMade(const ObjectSpriteRequest &req)
{
	for (int off = 1; off <= 4; off++) {
		for (int side : {1, -1}) {
			const int dir = (req.dir + side * off) & 7;
			auto it = _object_sprites.find(std::make_tuple(req.obj, static_cast<int>(req.where), dir, static_cast<int>(std::lround(req.scale * 1000)), req.fade, req.walk));
			if (it != _object_sprites.end() && ThreeDObjectSpriteMade(it->second)) return it->second;
		}
	}
	return EmptyObjectSprite();
}

/**
 * What the sprite cache draws for a sprite of an object not made yet (the main thread, GetRawSprite()): it is made on
 * the thread in the background, and drawn again when it comes. For the windows (ObjectSpriteRequest::gui) the object
 * read is tenths of a second, the first of a set seconds; the purchase list stood still as it scrolled, for a picture
 * of a few pixels (the player, 17. 10.: "the picture in the purchase menu is tiny, what are you photographing there"):
 * nothing drawn till then. On the map, the first vehicle of an object stood the game two or three seconds, each
 * direction after it ten or twenty milliseconds, "the last place where 0 degrees may bite" (the player, 17. 10.): the
 * vehicle's nearest direction made stands in. The rig makes it now.
 * @return the sprite to draw instead; 0: make it now (it is made)
 */
SpriteID ThreeDVehicleObjectSpriteStandIn(uint32_t request)
{
	if (request < 1 || request > _object_sprite_requests.size()) return 0;
	const ObjectSpriteRequest &req = _object_sprite_requests[request - 1];
	if (req.obj == nullptr || _three_d_load_at_once) return 0;
	const ZoomLevel zoom = ObjectSpriteZoom(req);
	{
		std::lock_guard lock(_bg_sprites_mutex);
		auto it = _bg_sprites_made.find(request);
		if (it != _bg_sprites_made.end() && it->second.zoom == zoom) return 0;
		if (!_bg_sprites_asked.insert(request).second) return req.gui ? EmptyObjectSprite() : NearestObjectSpriteMade(req); // asked already
	}
	/* What the game's state gives it now (its paint faded by the date), for the thread. */
	const float fade = FadeAmount(req.obj, req.fade), rust_time = RustTime(req.obj, req.fade);
	const uint32_t paint_colour = PaletteColour(req.obj, req.fade);
	ThreeDLoadInBackground([req, request, zoom, fade, rust_time, paint_colour] {
		{
			std::lock_guard lock(_bg_sprites_mutex);
			if (!_bg_sprites_asked.contains(request)) return; // forgotten meanwhile (the objects read anew)
		}
		SpriteMadeInBackground made;
		made.zoom = zoom;
		made.drawn = RasterObjectSprite(req, zoom, fade, rust_time, paint_colour, made.raster);
		std::lock_guard lock(_bg_sprites_mutex);
		if (_bg_sprites_asked.erase(request) == 0) return;
		_bg_sprites_made[request] = std::move(made);
		(req.gui ? _gui_sprites_came : _map_sprites_came) = true;
	});
	/* Without the thread it was made now. */
	{
		std::lock_guard lock(_bg_sprites_mutex);
		if (_bg_sprites_made.contains(request)) return 0;
	}
	return req.gui ? EmptyObjectSprite() : NearestObjectSpriteMade(req);
}

/**
 * Make a sprite of an object (ReadSprite()): the object drawn with the camera of the sprites, facing the direction
 * asked, about the point the game draws the sprite about -- at 16x for the map, the coarser levels made from it as from
 * a set's zin16, at the zoom of the interface for the windows, the finer made from it (ResizeSprites()); from the
 * thread's pixels when it made them (ThreeDVehicleObjectSpriteStandIn()).
 * @param request the sprite's request, counted from 1
 * @return the level made
 */
ZoomLevels MakeThreeDVehicleObjectSprite(uint32_t request, SpriteLoader::SpriteCollection &sprite)
{
	const ObjectSpriteRequest *req = (request >= 1 && request <= _object_sprite_requests.size()) ? &_object_sprite_requests[request - 1] : nullptr;
	ZoomLevels avail;
	const ZoomLevel zoom = req != nullptr ? ObjectSpriteZoom(*req) : ZoomLevel::Min;
	auto start = std::chrono::steady_clock::now();
	SpriteMadeInBackground made;
	bool found = false;
	if (req != nullptr && req->obj != nullptr) {
		{
			std::lock_guard lock(_bg_sprites_mutex);
			auto it = _bg_sprites_made.find(request);
			if (it != _bg_sprites_made.end()) {
				if (it->second.zoom == zoom) {
					made = std::move(it->second);
					found = true;
				}
				_bg_sprites_made.erase(it);
			}
		}
		if (!found) {
			made.zoom = zoom;
			made.drawn = RasterObjectSprite(*req, zoom, FadeAmount(req->obj, req->fade), RustTime(req->obj, req->fade), PaletteColour(req->obj, req->fade), made.raster);
		}
	}
	const ObjectRaster &r = made.raster;
	if (!made.drawn) {
		/* The empty picture, or an object that cannot be drawn: a transparent pixel. */
		SpriteLoader::Sprite &s = sprite[ZoomLevel::Min];
		s.colours = SpriteComponents{SpriteComponent::RGB, SpriteComponent::Alpha};
		s.width = 1;
		s.height = 1;
		s.x_offs = 0;
		s.y_offs = 0;
		s.AllocateData(ZoomLevel::Min, 1);
		avail.Set(ZoomLevel::Min);
		return avail;
	}
	SpriteLoader::Sprite &s = sprite[zoom];
	s.colours = SpriteComponents{SpriteComponent::RGB, SpriteComponent::Alpha};
	/* The object's origin from the point the sprite is drawn about, in pixels of the zoom (RemapCoords()). */
	const double ox = (req->off_y - req->off_x) * 2.0 * ZOOM_BASE / (1 << to_underlying(zoom)), oy = (req->off_x + req->off_y) * static_cast<double>(ZOOM_BASE) / (1 << to_underlying(zoom));
	s.width = static_cast<uint16_t>(r.width);
	s.height = static_cast<uint16_t>(r.height);
	s.x_offs = static_cast<int16_t>(r.left + std::lround(ox));
	s.y_offs = static_cast<int16_t>(r.top + std::lround(oy));
	s.AllocateData(zoom, static_cast<size_t>(r.width) * r.height);
	for (size_t i = 0; i < r.px.size(); i++) {
		SpriteLoader::CommonPixel &d = s.data[i];
		d.r = r.px[i].r;
		d.g = r.px[i].g;
		d.b = r.px[i].b;
		d.a = r.px[i].a;
		d.m = 0;
	}
	avail.Set(zoom);
	if (_debug_sprite_level >= 3) {
		/* And where the game thought it would be before it was made (ThreeDVehicleObjectSpriteBounds()): around it. */
		Rect before{};
		ThreeDVehicleObjectSpriteBounds(request, before);
		const int sc = 1 << to_underlying(zoom);
		const bool around = req->gui || (before.left <= s.x_offs && before.top <= s.y_offs && before.right >= s.x_offs + s.width - 1 && before.bottom >= s.y_offs + s.height - 1);
		Debug(sprite, 3, "3D vehicle {}: sprite{} of direction {}, state {}, {}x{} at zoom {}, {}, {} ms; at {},{}..{},{}, thought {},{}..{},{}{}", req->obj->name, req->gui ? " for the windows" : "", req->dir, req->fade, r.width, r.height, to_underlying(zoom),
				found ? "made in the background" : "made now", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count(),
				s.x_offs * sc, s.y_offs * sc, (s.x_offs + s.width - 1) * sc, (s.y_offs + s.height - 1) * sc, before.left, before.top, before.right, before.bottom, around ? "" : " NOT AROUND IT");
	}
	return avail;
}

/**
 * Where a sprite of an object not made yet lies about the point the game draws it about (MakeThreeDVehicleObjectSprite()),
 * pixels of 16x: the box of the object's extent (and of its crew, which may walk beside it) seen with the camera of the
 * sprites, and a pixel of the normal zoom round it (the rounding of RasterObject()). The game asks this of every vehicle as it moves, in
 * its tick (VehicleSpriteSeq::GetBounds(): the box redrawn about it), and the sprite itself was made for it -- tens of
 * milliseconds, the first of an object seconds -- wherever the player looked from (the player, 17. 10.: "we make
 * pictures from the objects even when we look at them from other angles"). An object whose shape was never read
 * (nothing drew it yet) a box as long as it is said to be, or of the longest lorry; its shape is not read for this.
 * @return false: no such sprite
 */
bool ThreeDVehicleObjectSpriteBounds(uint32_t request, Rect &bounds)
{
	if (request < 1 || request > _object_sprite_requests.size()) return false;
	ObjectSpriteRequest &req = _object_sprite_requests[request - 1];
	if (req.obj == nullptr) {
		/* The empty picture: a transparent pixel. */
		bounds = {0, 0, 0, 0};
		return true;
	}
	const ThreeDVehicleObject &obj = *req.obj;
	float e[6];
	uint32_t version = 0;
	const bool known = obj.KnownExtent(e, &version);
	if (req.bounds_of == 2 && version == req.extent_version) {
		bounds = req.bounds;
		return true;
	}
	if (!known && req.bounds_of == 1) {
		bounds = req.bounds;
		return true;
	}
	if (!known) {
		/* Its origin is in its middle (ThreeDVehicleShape). */
		const float half = (obj.length_m > 0 ? obj.length_m : 20.0f) / 2 + 1.0f;
		const float guess[6] = {-half, half, -2.0f, 2.0f, -0.5f, 5.0f};
		std::copy(std::begin(guess), std::end(guess), e);
	}
	const ThreeDVehicleState *st = obj.State(req.fade);
	const float tp = (st != nullptr ? st->turn : 0.0f) * static_cast<float>(M_PI) / 180.0f, tc = std::cos(tp), ts = std::sin(tp);
	const float along = (SqueezedType(obj.type) && (req.dir & 1)) ? SQUEEZE : 1.0f;
	const float az = ViewAzimuth(req.dir) * static_cast<float>(M_PI) / 180.0f, ca = std::cos(az), sa = std::sin(az);
	float minx = std::numeric_limits<float>::infinity(), miny = minx, maxx = -minx, maxy = -minx;
	/* The corners of a box in the object's frame on the picture, as RasterObject() places them. */
	auto add_box = [&](const float b[6]) {
		for (int c = 0; c < 8; c++) {
			const float px = b[c & 1], py = b[2 + ((c >> 1) & 1)], pz = b[4 + ((c >> 2) & 1)];
			const float x = (px * tc - py * ts) * req.scale * along, y = (px * ts + py * tc) * req.scale, z = pz * req.scale;
			const float xr = x * ca - y * sa, yr = x * sa + y * ca;
			const float sx = xr * PXM_16X, sy = (-yr * CAM_SIN - z * CAM_COS) * PXM_16X;
			minx = std::min(minx, sx); maxx = std::max(maxx, sx);
			miny = std::min(miny, sy); maxy = std::max(maxy, sy);
		}
	};
	add_box(e);
	auto add_crew = [&](const ThreeDVehicleCrew &c) {
		const float r = 0.6f * c.scale, h = 2.2f * c.scale;
		const float b[6] = {c.at[0] - r, c.at[0] + r, c.at[1] - r, c.at[1] + r, c.at[2], c.at[2] + h};
		add_box(b);
	};
	for (const ThreeDVehicleCrew &c : obj.crew) add_crew(c);
	for (const auto &group : obj.crew_groups) {
		for (const ThreeDVehicleCrew &c : group.second) add_crew(c);
	}
	const double ox = (req.off_y - req.off_x) * 2.0 * ZOOM_BASE, oy = (req.off_x + req.off_y) * static_cast<double>(ZOOM_BASE);
	const int pad = ZOOM_BASE;
	req.bounds.left = static_cast<int>(std::floor(minx)) + static_cast<int>(std::lround(ox)) - pad;
	req.bounds.top = static_cast<int>(std::floor(miny)) + static_cast<int>(std::lround(oy)) - pad;
	req.bounds.right = static_cast<int>(std::ceil(maxx)) + static_cast<int>(std::lround(ox)) + pad;
	req.bounds.bottom = static_cast<int>(std::ceil(maxy)) + static_cast<int>(std::lround(oy)) + pad;
	req.bounds_of = known ? 2 : 1;
	req.extent_version = version;
	bounds = req.bounds;
	return true;
}

/** Time spent making pictures of objects for the turned view, and how many, for the rig (test3d casy): at once, and on the thread. */
double _object_picture_ms = 0, _object_picture_thread_ms = 0;
uint64_t _object_pictures_made = 0, _object_pictures_made_thread = 0;

/** The pictures asked for since this clock stay (PinThreeDPictures()). */
static uint64_t _pictures_pinned_from = UINT64_MAX;
/** And those asked for by the block of the map being drawn: it draws them after it asked for them (KeepThreeDPicturesOfBlock()). */
static uint64_t _pictures_block_from = UINT64_MAX;

/**
 * Keep the pictures of the 3D objects asked for from now on until unpinned: a view that collects its triangles and
 * draws them at once on every core (the view from the eyes) draws their pixels after it asked for more of them.
 */
void PinThreeDPictures(bool pin)
{
	_pictures_pinned_from = pin ? _object_pictures_clock + 1 : UINT64_MAX;
}

/**
 * A block of the map is drawn from now on (ViewportDoDraw()): the pictures it asks for are kept till it is drawn, as it
 * draws them after it asked for them all.
 */
void KeepThreeDPicturesOfBlock()
{
	_pictures_block_from = _object_pictures_clock + 1;
}

/** The memory of a picture kept: its pixels and its lit pixels. */
static size_t PictureBytes(const ThreeDModelPicture &p)
{
	return (p.pixels.capacity() + p.lit.capacity()) * sizeof(uint32_t);
}

/** Room for one more picture kept: at PICTURES_KEPT pictures, or PICTURE_BYTES_KEPT of them, the longest not asked for go till half is left. */
static void MakeRoomForPicture()
{
	size_t bytes = 0;
	for (const auto &e : _object_pictures) bytes += PictureBytes(e.second.picture);
	if (_object_pictures.size() < PICTURES_KEPT && bytes <= PICTURE_BYTES_KEPT) return;
	std::vector<std::pair<uint64_t, std::map<ObjectPictureKey, ObjectPictureEntry>::iterator>> ages;
	ages.reserve(_object_pictures.size());
	for (auto i = _object_pictures.begin(); i != _object_pictures.end(); ++i) ages.emplace_back(i->second.used, i);
	std::ranges::sort(ages, {}, &std::pair<uint64_t, std::map<ObjectPictureKey, ObjectPictureEntry>::iterator>::first);
	/* Never those of the picture being drawn (PinThreeDPictures()) or the block (KeepThreeDPicturesOfBlock()): their
	 * pixels are still to be drawn. */
	const uint64_t keep_from = std::min(_pictures_pinned_from, _pictures_block_from);
	size_t count = _object_pictures.size();
	for (const auto &[used, it] : ages) {
		if (used >= keep_from || (count <= PICTURES_KEPT / 2 && bytes <= PICTURE_BYTES_KEPT / 2)) break;
		bytes -= PictureBytes(it->second.picture);
		count--;
		_object_pictures.erase(it);
	}
}

/** A picture of the turned view from an object drawn into pixels (RasterObject()). */
static void PictureOfRaster(ThreeDModelPicture &picture, ObjectRaster &r)
{
	picture.width = r.width;
	picture.height = r.height;
	picture.left = r.left;
	picture.top = r.top;
	picture.pixels.resize(r.px.size());
	for (size_t i = 0; i < r.px.size(); i++) picture.pixels[i] = r.px[i].a == 0 ? 0 : r.px[i].data;
	picture.lit.clear();
	for (size_t i = 0; i < r.lit.size(); i++) {
		if (r.lit[i] != 0) picture.lit.push_back(static_cast<uint32_t>(i));
	}
	picture.lights = std::move(r.lights);
}

/* (PictureToMake, PictureMade below.) */
struct PictureToMake;
struct PictureMade;
static void MakePictures(std::span<const PictureToMake> ps, std::vector<PictureMade> &made);

/*
 * The pictures of the vehicles on the map are made on a thread of their own (the player, 17. 10.: "put a thread in
 * the background"): each is a few or tens of milliseconds, the first of an object half a second (its textures read),
 * and all were made the moment they were asked for, on the thread that draws -- the game stood still as a lorry came
 * out of its depot or bent its trailer in a curve (a dozen pictures at once). Now the main thread asks the thread for
 * a picture it has not got and draws the vehicle as the nearest one made of it (or not at all, the first time), the
 * thread makes it (RasterObject()), and the vehicle is drawn again the moment it is there (_owed_pictures_timer).
 * The pictures of all else -- the figures, the plants, the effects, the previews, the view from the eyes -- are
 * made as they were, at once. What the thread makes of the objects on the way (their shapes, their textures) is
 * made under the locks of three_d_objects.cpp (MakeOnce() there).
 */
/** A picture to make: what ObjectPicture() was asked for, as RasterObject() takes it. */
struct PictureToMake {
	ObjectPictureKey key;
	const ThreeDVehicleObject *obj = nullptr;
	float scale = 1, cam_az = 0, world_az = 0, along = 1, pxm = PXM_16X, fade = 0, rust_time = 0, stretch = 1, spin = 0, pitch = 0, roll = 0;
	uint32_t tint = 0, paint_colour = 0;
	int frame = 0, lights = 0, state = 0;
	uint64_t walk = 0;
};
/** A picture made, for the main thread to keep. */
struct PictureMade {
	ObjectPictureKey key;
	ThreeDModelPicture picture; ///< width 0: the object draws nothing so
	double ms = 0;              ///< how long it took
};
/** Only so many wait to be made: the oldest asked for go when more come (a vehicle long gone round its curve). */
static constexpr size_t PICTURES_WAITING = 128;
/** So many the thread hands the graphics card at once (ThreeDGpuRender()): drawn side by side, read back once. */
static constexpr size_t PICTURES_AT_ONCE = 16; ///< (not more: the main thread's drawing waits for a job in hand, QueueGpuJob())

/**
 * The pictures of objects drawn by the graphics card all at once (ThreeDGpuRender(); the player, 20. 10.: "fix the
 * card so it works"): one frame of the card read back once, not a picture at a time -- 5-19 ms each on the player's
 * phone, a turn of the map a thousand of them.
 * @param[out] result per picture: 1 drawn, 0 nothing of it seen, -1 not drawn by the card (the processor draws it)
 */
static void RasterObjectsOnCard(std::span<const PictureToMake> ps, std::span<ObjectRaster> out, std::span<int8_t> result)
{
	std::vector<GpuObjectPicture> gps(ps.size());
	std::vector<ThreeDGpuPicture *> pics;
	for (size_t i = 0; i < ps.size(); i++) {
		const PictureToMake &p = ps[i];
		result[i] = 0;
		const ThreeDVehicleShape *whole = p.obj->GetShape(p.frame, p.obj->PartsOf(p.state));
		if (whole == nullptr || !PrepareGpuObject(*p.obj, *whole, p.scale, p.cam_az, p.world_az, p.along, p.pxm, p.fade, p.rust_time, p.tint, p.paint_colour, p.stretch, p.spin, p.pitch, p.roll, p.lights, p.state, p.walk, gps[i])) continue;
		result[i] = -1;
		pics.push_back(&gps[i].pic);
	}
	if (pics.empty()) return;
	ThreeDGpuRender(pics);
	for (size_t i = 0; i < ps.size(); i++) {
		if (result[i] < 0 && gps[i].pic.drawn) result[i] = FinishGpuObject(gps[i], out[i]) ? 1 : 0;
	}
}

/** Pictures of objects made (RasterObject()): by the card all at once where it draws them, else one by one; how long each took, a share of all. */
static void MakePictures(std::span<const PictureToMake> ps, std::vector<PictureMade> &made)
{
	const auto start = std::chrono::steady_clock::now();
	std::vector<ObjectRaster> rasters(ps.size());
	std::vector<int8_t> result(ps.size(), -1);
	if (ps.size() > 1 && ThreeDGpuAvailable()) RasterObjectsOnCard(ps, rasters, result);
	made.resize(ps.size());
	for (size_t i = 0; i < ps.size(); i++) {
		const PictureToMake &p = ps[i];
		if (result[i] < 0) {
			result[i] = RasterObject(*p.obj, p.scale, p.cam_az, p.world_az, p.along, p.pxm, p.fade, rasters[i], p.frame, p.rust_time, p.tint, p.paint_colour, p.stretch, p.spin, p.pitch, p.roll, p.lights, p.state, p.walk) ? 1 : 0;
		}
		made[i].key = p.key;
		if (result[i] > 0) PictureOfRaster(made[i].picture, rasters[i]);
	}
	const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	for (PictureMade &m : made) m.ms = ms / made.size();
}
static std::mutex _pictures_mutex; ///< of _pictures_to_make, _loads_to_do, _pictures_done, _picture_in_hand and _pictures_stop
static std::condition_variable _pictures_wake;
static std::deque<PictureToMake> _pictures_to_make;
/** What the view from the eyes asked to be made in the background (ThreeDLoadInBackground()): before the pictures, which may need it too. */
static std::deque<std::function<void()>> _loads_to_do;
static std::vector<PictureMade> _pictures_done;
static bool _picture_in_hand = false; ///< the thread is making one now
static bool _pictures_stop = false;
static std::thread _pictures_thread;
static bool _pictures_thread_failed = false; ///< it could not be started: the pictures are made at once, as before
/** The pictures asked of the thread and not kept yet (the main thread's own). */
static std::set<ObjectPictureKey> _pictures_asked;
/** The vehicles drawn with a picture not their own: drawn again when pictures come. */
static std::set<VehicleID> _vehicles_owed_pictures;

/** The thread: the loads first, then the pictures, the newest asked for first (what is on the screen now), each into _pictures_done. */
static void MakePicturesInBackground()
{
	std::unique_lock lock(_pictures_mutex);
	for (;;) {
		_pictures_wake.wait(lock, [] { return _pictures_stop || !_loads_to_do.empty() || !_pictures_to_make.empty(); });
		if (_pictures_stop) return;
		/* A load and a picture in turn when both wait: a town coming into view asks for a hundred loads. */
		static bool load_turn = true;
		load_turn = !load_turn;
		if (!_loads_to_do.empty() && (load_turn || _pictures_to_make.empty())) {
			std::function<void()> load = std::move(_loads_to_do.front());
			_loads_to_do.pop_front();
			_picture_in_hand = true;
			lock.unlock();
			load();
			lock.lock();
			_picture_in_hand = false;
			continue;
		}
		/* With the card as many as it draws at once (RasterObjectsOnCard()), else one. */
		std::vector<PictureToMake> batch;
		const size_t at_once = ThreeDGpuReady() ? PICTURES_AT_ONCE : 1;
		while (!_pictures_to_make.empty() && batch.size() < at_once) {
			batch.push_back(std::move(_pictures_to_make.back()));
			_pictures_to_make.pop_back();
		}
		_picture_in_hand = true;
		lock.unlock();

		std::vector<PictureMade> made;
		MakePictures(batch, made);
		for (size_t i = 0; i < made.size(); i++) {
			Debug(sprite, 4, "3D vehicle {}: picture at {} degrees, zoom {}: {}x{}, {:.0f} ms, in the background", batch[i].obj->name, std::get<2>(made[i].key), std::get<1>(made[i].key), made[i].picture.width, made[i].picture.height, made[i].ms);
		}

		lock.lock();
		for (PictureMade &m : made) _pictures_done.push_back(std::move(m));
		_picture_in_hand = false;
	}
}

/** The thread, started the first time it is wanted; false when it cannot be (the pictures are made at once then, the loads here). */
static bool StartPictureThread()
{
	if (_pictures_thread_failed) return false;
	if (_pictures_thread.joinable()) return true;
	if (!StartNewThread(&_pictures_thread, "ottd:3d-pictures", &MakePicturesInBackground)) {
		_pictures_thread_failed = true;
		Debug(misc, 0, "3D vehicles: the thread making their pictures cannot be started; they are made as asked for");
		return false;
	}
	return true;
}

bool _three_d_load_at_once = false;
/** How many jobs were asked of the thread in the background, for the rig (test3d casy). */
uint64_t _three_d_loads_asked = 0;

/** A job for the thread in the background (three_d_objects.h): what the view from the eyes has not got yet is made; without the thread (or for the rig), here. */
void ThreeDLoadInBackground(std::function<void()> job)
{
	_three_d_loads_asked++;
	if (_three_d_load_at_once || !StartPictureThread()) {
		job();
		return;
	}
	{
		std::lock_guard lock(_pictures_mutex);
		_loads_to_do.push_back(std::move(job));
	}
	_pictures_wake.notify_one();
}

/** Pictures were asked of the thread since it was last woken for them (AskForPicture()). */
static std::atomic<bool> _pictures_asked_since = false;

/** Ask the thread for a picture (started the first time); false when there is none (it could not be started): made at once then. */
static bool AskForPicture(PictureToMake &&p)
{
	if (!StartPictureThread()) return false;
	if (!_pictures_asked.insert(p.key).second) return true; // asked already
	{
		std::lock_guard lock(_pictures_mutex);
		while (_pictures_to_make.size() >= PICTURES_WAITING) {
			_pictures_asked.erase(_pictures_to_make.front().key);
			_pictures_to_make.pop_front();
		}
		_pictures_to_make.push_back(std::move(p));
	}
	/* The thread woken when the frame is drawn (_owed_pictures_timer), so that it hands the card the frame's at once. */
	_pictures_asked_since = true;
	return true;
}

/** Drawing the map (ViewportDoDraw()): a picture not made yet has the nearest made stand in (ObjectPicture()). */
bool _three_d_pictures_stand_in = false;
/** The pictures given to stand in since the pictures last came, how many, and the places drawn with them, drawn again when they come. */
static std::set<const ThreeDModelPicture *> _stand_in_pictures;
static uint64_t _stand_ins_given = 0;
static std::vector<std::array<double, 6>> _boxes_owed_pictures;

/** How many pictures stood in so far: a building drawn with one has its box drawn again when the pictures come (OweThreeDPictureBox()). */
uint64_t ThreeDStandInsGiven()
{
	return _stand_ins_given;
}

/** Whether a picture given was one standing in for another not made yet (ObjectPicture()). */
bool ThreeDPictureStandsIn(const ThreeDModelPicture *pic)
{
	return _stand_in_pictures.contains(pic);
}

/** A box of the world drawn with a picture standing in: drawn again when the pictures come (TakePicturesMade()). */
void OweThreeDPictureBox(double x0, double y0, double x1, double y1, int z0, int z1)
{
	if (_boxes_owed_pictures.size() < 4096) _boxes_owed_pictures.push_back({x0, y0, x1, y1, static_cast<double>(z0), static_cast<double>(z1)});
}

/**
 * Keep the pictures the thread has made (the main thread).
 * @return whether there were any
 */
static bool TakePicturesMade()
{
	std::vector<PictureMade> done;
	{
		std::lock_guard lock(_pictures_mutex);
		done.swap(_pictures_done);
	}
	for (PictureMade &made : done) {
		_pictures_asked.erase(made.key);
		_object_picture_thread_ms += made.ms;
		_object_pictures_made_thread++;
		MakeRoomForPicture();
		ObjectPictureEntry &e = _object_pictures[made.key];
		e.used = ++_object_pictures_clock;
		e.picture = std::move(made.picture);
	}
	if (done.empty()) return false;
	/* The places drawn with pictures standing in drawn again: with these, or with the nearest again till theirs come. */
	std::vector<std::array<double, 6>> owed;
	owed.swap(_boxes_owed_pictures);
	_stand_in_pictures.clear();
	for (const auto &b : owed) MarkWorldBoxDirty(b[0], b[1], b[2], b[3], static_cast<int>(b[4]), static_cast<int>(b[5]));
	return true;
}

/** The rig ('test3d dokresli'): the pictures the thread made kept now, not at the next round of the window's timer. */
void RigTakeThreeDPicturesMade()
{
	TakePicturesMade();
}

/** Whether the thread is making a picture of a vehicle or has some to make: what the objects made is held then (ForgetUnusedThreeDObjects()). */
bool ThreeDPicturesBeingMade()
{
	std::lock_guard lock(_pictures_mutex);
	return _picture_in_hand || !_pictures_to_make.empty() || !_loads_to_do.empty();
}

/** How many pictures of vehicles and loads wait for the thread, the one in hand among them (the rig, test3d casy). */
size_t ThreeDPicturesWaiting()
{
	if (_pictures_asked_since.exchange(false)) _pictures_wake.notify_one();
	std::lock_guard lock(_pictures_mutex);
	return _pictures_to_make.size() + _loads_to_do.size() + (_picture_in_hand ? 1 : 0);
}

/**
 * Stop the thread making the pictures of the vehicles and forget what it was asked for: the objects are read anew
 * (ResetThreeDVehicleObjectSprites()), or the game ends (ShutdownGame()). Started again the next time one is asked for.
 */
void StopThreeDPictureThread()
{
	if (_pictures_thread.joinable()) {
		{
			std::lock_guard lock(_pictures_mutex);
			_pictures_stop = true;
		}
		_pictures_wake.notify_all();
		_pictures_thread.join();
	}
	_pictures_stop = false;
	_pictures_to_make.clear();
	_loads_to_do.clear();
	_pictures_done.clear();
	_picture_in_hand = false;
	_pictures_asked.clear();
	_vehicles_owed_pictures.clear();
}

/** Whether the picture asked for now may wait for the thread (a vehicle on the map), and whether it waited: the nearest made was given. */
static bool _object_picture_may_wait = false;
static bool _object_picture_waited = false;
/** The pictures not made yet asked for while collecting (MakeThreeDPicturesAtOnce()), and their keys; nullptr: not collecting. */
static std::vector<PictureToMake> *_pictures_collected = nullptr;
static std::set<ObjectPictureKey> _pictures_collected_keys;

/** The pictures made come in, and the vehicles that waited for them are drawn again. */
static const IntervalTimer<TimerWindow> _owed_pictures_timer(std::chrono::milliseconds(30), [](auto) {
	/* The pictures asked for while the frames were drawn: the thread makes them now, together. */
	if (_pictures_asked_since.exchange(false)) _pictures_wake.notify_one();
	/* The sprites for the windows made in the background (ThreeDVehicleObjectSpriteStandIn()): the windows that draw them. */
	if (_gui_sprites_came.exchange(false)) {
		for (WindowClass wc : {WindowClass::BuildVehicle, WindowClass::ReplaceVehicle, WindowClass::EnginePreview}) SetWindowClassesDirty(wc);
	}
	/* Those of the map: the vehicles drawn as objects, and the windows of the vehicles. */
	if (_map_sprites_came.exchange(false)) {
		for (const Vehicle *v : Vehicle::Iterate()) {
			if (ThreeDVehicleObjectOf(v) != nullptr) v->MarkAllViewportsDirty();
		}
		for (WindowClass wc : {WindowClass::VehicleDepot, WindowClass::VehicleView, WindowClass::VehicleDetails, WindowClass::TrainList, WindowClass::RoadVehicleList, WindowClass::ShipList, WindowClass::AircraftList}) SetWindowClassesDirty(wc);
	}
	if (!TakePicturesMade() || _vehicles_owed_pictures.empty()) return;
	std::set<VehicleID> owed;
	owed.swap(_vehicles_owed_pictures);
	for (VehicleID id : owed) {
		const Vehicle *v = Vehicle::GetIfValid(id);
		if (v != nullptr) v->MarkAllViewportsDirty();
	}
});

/**
 * A picture of an object in the turned view at an angle and zoom, kept (RasterObject()): made now, or, for a vehicle
 * of the map (_object_picture_may_wait), asked of the thread and the nearest made given meanwhile.
 */
static const ThreeDModelPicture *ObjectPicture(const ThreeDVehicleObject *obj, float scale, ZoomLevel zoom, float cam_az, float world_az, bool squeeze, int fade, int frame = 0, int tint_index = -1, float stretch = 1.0f, int spin_step = 0, int pitch = 0, int roll = 0, int lights = 0, float along_given = 0.0f, uint64_t walk = 0)
{
	int cam_step = static_cast<int>(std::lround(std::fmod(cam_az + 720.0f, 360.0f))) % 360;
	int world_step = static_cast<int>(std::lround(std::fmod(world_az + 720.0f, 360.0f))) % 360;
	pitch = std::clamp(pitch, -60, 60);
	roll = std::clamp(roll, -60, 60);
	const ObjectPictureKey key = std::make_tuple(obj, to_underlying(zoom), cam_step, world_step, static_cast<int>(std::lround(scale * 1000)), (fade & 0xFFFF) | (pitch + 64) << 16 | (roll + 64) << 24, frame | spin_step << 8 | (tint_index + 1) << 16 | lights << 28, static_cast<int>(std::lround(stretch * 1000)), static_cast<int>(std::lround(along_given * 1000)), walk);
	const uint32_t tint = tint_index >= 0 && static_cast<size_t>(tint_index) < obj->tint_palette.size() ? obj->tint_palette[tint_index] : 0;
	auto it = _object_pictures.find(key);
	if (it != _object_pictures.end()) {
		it->second.used = ++_object_pictures_clock;
		return it->second.picture.width > 0 ? &it->second.picture : nullptr;
	}
	/* On the track foreshortened along itself as the map does, smoothly between the directions (as the likeness, RenderThreeDVehicle()). */
	float f = cam_step * static_cast<float>(M_PI) / 180.0f;
	float along = along_given > 0 ? along_given : (squeeze ? 1.0f - (1.0f - SQUEEZE) * std::fabs(std::sin(2.0f * f)) : 1.0f);
	const float pxm = PXM_16X / (1 << to_underlying(zoom));
	/* Drawing the map (_three_d_pictures_stand_in): the drawing waits for no card -- the nearest made of the object stands
	 * in, the thread makes this one and its place is drawn again when it comes (OweThreeDPictureBox()). Only the first of
	 * an object at a zoom is made at once. */
	const bool stand_in = _three_d_pictures_stand_in && !_object_picture_may_wait && !_three_d_load_at_once;
	if (_pictures_collected != nullptr || _object_picture_may_wait || stand_in) {
		PictureToMake p;
		p.key = key;
		p.obj = obj;
		p.scale = scale;
		p.cam_az = static_cast<float>(cam_step);
		p.world_az = static_cast<float>(world_step);
		p.along = along;
		p.pxm = pxm;
		p.fade = FadeAmount(obj, fade);
		p.rust_time = RustTime(obj, fade);
		p.tint = tint;
		p.paint_colour = PaletteColour(obj, fade);
		p.stretch = stretch;
		p.spin = spin_step * 360.0f / SPIN_STEPS;
		p.pitch = static_cast<float>(pitch);
		p.roll = static_cast<float>(roll);
		p.frame = frame;
		p.lights = lights;
		p.state = fade;
		p.walk = walk;
		/* Till it comes: the nearest made of the object at the zoom, its size and its state, by the angle seen. */
		const ThreeDModelPicture *nearest = nullptr;
		int best = INT_MAX;
		for (auto n = _object_pictures.lower_bound(std::make_tuple(obj, to_underlying(zoom), INT_MIN, INT_MIN, INT_MIN, INT_MIN, INT_MIN, INT_MIN, INT_MIN, uint64_t{0}));
				n != _object_pictures.end() && std::get<0>(n->first) == obj && std::get<1>(n->first) == to_underlying(zoom); ++n) {
			if (std::get<4>(n->first) != std::get<4>(key) || (std::get<5>(n->first) & 0xFFFF) != (std::get<5>(key) & 0xFFFF) || n->second.picture.width <= 0) continue;
			auto apart = [](int a, int b) { const int d = std::abs(a - b) % 360; return std::min(d, 360 - d); };
			const int off = apart(std::get<2>(n->first), cam_step) * 4 + apart(std::get<3>(n->first), world_step);
			if (off < best) {
				best = off;
				nearest = &n->second.picture;
			}
		}
		if (stand_in && nearest != nullptr && AskForPicture(std::move(p))) {
			_stand_ins_given++;
			_stand_in_pictures.insert(nearest);
			return nearest;
		}
		if (_pictures_collected != nullptr) {
			/* Collected, made with the others after (MakeThreeDPicturesAtOnce()). */
			if (_pictures_collected_keys.insert(key).second) _pictures_collected->push_back(std::move(p));
			return nullptr;
		}
		if (_object_picture_may_wait && AskForPicture(std::move(p))) {
			_object_picture_waited = true;
			return nearest;
		}
	}
	MakeRoomForPicture();
	ObjectPictureEntry &e = _object_pictures[key];
	e.used = ++_object_pictures_clock;
	ObjectRaster r;
	auto start = std::chrono::steady_clock::now();
	_object_pictures_made++;
	if (RasterObject(*obj, scale, static_cast<float>(cam_step), static_cast<float>(world_step), along, pxm, FadeAmount(obj, fade), r, frame, RustTime(obj, fade), tint, PaletteColour(obj, fade), stretch, spin_step * 360.0f / SPIN_STEPS, static_cast<float>(pitch), static_cast<float>(roll), lights, fade, walk)) {
		Debug(sprite, 4, "3D vehicle {}: picture at {} degrees, zoom {}: {}x{}, {} ms", obj->name, cam_step, to_underlying(zoom), r.width, r.height,
				std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
		PictureOfRaster(e.picture, r);
	}
	_object_picture_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	return e.picture.width > 0 ? &e.picture : nullptr;
}

/**
 * The pictures of objects a drawing will ask for made before it, all at once (three_d_vehicle_object.h; the player,
 * 20. 10.: "fix the card so it works"): what ask asks for and is not made yet is collected, then made together -- by the
 * card in one frame read back once (RasterObjectsOnCard()), not a picture at a time as the drawing comes to each.
 * @param ask asks for the pictures as the drawing will (GetThreeDFigurePicture(), GetThreeDPlantPicture(), ...)
 */
void MakeThreeDPicturesAtOnce(const std::function<void()> &ask)
{
	std::vector<PictureToMake> wanted;
	_pictures_collected = &wanted;
	_pictures_collected_keys.clear();
	ask();
	_pictures_collected = nullptr;
	_pictures_collected_keys.clear();
	if (wanted.empty()) return;
	std::vector<PictureMade> made;
	MakePictures(wanted, made);
	for (PictureMade &m : made) {
		MakeRoomForPicture();
		ObjectPictureEntry &e = _object_pictures[m.key];
		e.used = ++_object_pictures_clock;
		e.picture = std::move(m.picture);
		_object_pictures_made++;
		_object_picture_ms += m.ms;
	}
}

/**
 * Which lights of a vehicle drawn as an object are on at night (the colleague's hra/3d/PROSBA-SVETLA-VOZIDEL-UZLY.md):
 * the headlights of its front, +x, when it leads, the red ones of its back when nothing follows it. One turned round
 * lights those its model has for going back first (VAGONY-3D-V5-PRO-HRU.md), else none.
 * @param front,last its first and its last piece
 */
static bool NothingSeenAfter(const Vehicle *last, ZoomLevel zoom);

static int VehicleObjectLightsOn(const ThreeDVehicleObject *obj, const Vehicle *front, const Vehicle *last, bool reversed, ZoomLevel zoom)
{
	/* At night, or as the player switched them from the cab (VehicleLightsOn()). */
	if (!VehicleLightsOn(front)) return 0;
	const ThreeDVehicleShape *shape = obj->GetShape();
	if (shape == nullptr || !shape->HasLights()) return 0;
	const bool leads = front->Previous() == nullptr;
	/* Its red lights when nothing follows it, or nothing seen: a lorry of the colleague's without its trailer has the
	 * trailer's pieces behind it, drawn as nothing. */
	const bool last_one = last->Next() == nullptr || NothingSeenAfter(last, zoom);
	/* Turned round, going back first (the colleague's hra/3d/VAGONY-3D-V5-PRO-HRU.md): the headlights on its back end
	 * when it leads, the red lights on its front end when nothing follows; without those, none. */
	if (reversed) return shape->HasLightsGoingBack() ? (leads ? LIGHTS_FRONT_AT_BACK : 0) | (last_one ? LIGHTS_BACK_AT_FRONT : 0) : 0;
	return (leads ? LIGHTS_FRONT : 0) | (last_one ? LIGHTS_BACK : 0);
}

/** The first and the last piece of a vehicle an object hung from its anchor piece covers (an article of a train of lorries its own). */
static void ObjectEnds(const ThreeDVehicleObject *obj, const Vehicle *anchor, const Vehicle *&front, const Vehicle *&last)
{
	front = PieceAway(anchor, -obj->anchor);
	if (front == nullptr) front = anchor;
	last = front;
	for (size_t k = 1; k < obj->pieces.size() && last->Next() != nullptr; k++) last = last->Next();
}

/** Where a vehicle drawn as an object is drawn on the map (PlaceVehicleObject()). */
struct VehicleObjectPlace {
	const ThreeDModelPicture *pic = nullptr; ///< its picture at the view's angle, nullptr when it has none
	int ax = 0, ay = 0, az = 0;              ///< the world point the picture hangs from
	int xmin = 0, ymin = 0, zmin = 0, xmax = 0, ymax = 0, zmax = 0; ///< the box of its pieces, for the sorting
};

/**
 * Where and as which picture a vehicle drawn as an object is drawn on the map: the object at the angle of the view,
 * hung from the middle of its chain, sorted by the box of its pieces; its other pieces draw nothing.
 * @param pose where and at what angle to draw it instead (a round curve, round_track.h), or nullptr
 * @param[out] place where, filled for the anchor piece only
 * @return the object, nullptr for a vehicle not drawn as one; for a piece drawn with its anchor piece, the object with no picture
 */
static void EyeOrigin(const ThreeDVehicleObject *obj, const Vehicle *anchor, double &x, double &y, double &facing, double &rolled, double &curvature, int depth = 0);

/**
 * How far a vehicle's front is raised over its back, degrees (the player, #287: "the cars do not lean up a hill"; the
 * tilting of OpenLoco, the player 16. 10.): the ground under its front end and under its back end, its length apart,
 * as its wheels stand on it -- so it tips as its front reaches a slope, not at once on the slope's tile. Level on a
 * bridge and in a tunnel, and a ship or an aircraft.
 * @param obj its object
 * @param v its anchor piece
 * @param x,y its middle, world units
 * @param facing the way its front faces, radians
 */
static float VehicleSlopePitch(const ThreeDVehicleObject *obj, const Vehicle *v, double x, double y, double facing)
{
	if (v->type == VehicleType::Train) {
		if (Train::From(v)->track == Track::Wormhole) return 0;
	} else if (v->type == VehicleType::Road) {
		if (RoadVehicle::From(v)->state == RVSB_WORMHOLE) return 0;
	} else {
		return 0;
	}
	float length_m = obj->length_m;
	if (length_m <= 0) {
		const ThreeDVehicleShape *shape = obj->TryGetShape();
		if (shape == nullptr) return 0;
		length_m = shape->x1 - shape->x0;
	}
	const double half = length_m * ObjectScale(obj) * UNITS_PER_METRE / 2;
	if (half <= 0) return 0;
	auto ground = [](double px, double py) {
		const int gx = std::clamp(static_cast<int>(px), 0, static_cast<int>(Map::MaxX() * TILE_SIZE) - 1);
		const int gy = std::clamp(static_cast<int>(py), 0, static_cast<int>(Map::MaxY() * TILE_SIZE) - 1);
		return GetSlopePixelZ(gx, gy, true);
	};
	const double ax = std::cos(facing) * half, ay = std::sin(facing) * half;
	const double rise_m = (ground(x + ax, y + ay) - ground(x - ax, y - ay)) / OBJECT_HEIGHT_PER_METRE;
	return static_cast<float>(std::atan2(rise_m, 2 * half / UNITS_PER_METRE) * 180.0 / M_PI);
}

static const ThreeDVehicleObject *PlaceVehicleObject(const Vehicle *v, ZoomLevel zoom, const RoundTrackPose *pose, VehicleObjectPlace &place)
{
	int piece = 0;
	const ThreeDVehicleObject *obj = ThreeDVehicleObjectOf(v, &piece);
	if (obj == nullptr || piece != obj->anchor) return obj;

	/* The way it faces: a train turned round, the rear head of a double-headed engine, the other way. */
	const bool reversed = PieceReversed(v);
	const int d = PieceDirection(v);

	/* The box of its own pieces for the sorting (an article of a train of lorries its own, not the whole train's). */
	const Vehicle *front = PieceAway(v, -piece);
	if (front == nullptr) front = v;
	const Vehicle *last = front;
	int xmin = INT32_MAX, ymin = INT32_MAX, zmin = INT32_MAX, xmax = INT32_MIN, ymax = INT32_MIN, zmax = INT32_MIN;
	const Vehicle *u = front;
	for (size_t k = 0; u != nullptr && k < obj->pieces.size(); k++, u = u->Next()) {
		last = u;
		xmin = std::min(xmin, u->x_pos + u->bounds.origin.x);
		ymin = std::min(ymin, u->y_pos + u->bounds.origin.y);
		zmin = std::min(zmin, u->z_pos + u->bounds.origin.z);
		xmax = std::max(xmax, u->x_pos + u->bounds.origin.x + u->bounds.extent.x - 1);
		ymax = std::max(ymax, u->y_pos + u->bounds.origin.y + u->bounds.extent.y - 1);
		zmax = std::max(zmax, u->z_pos + u->bounds.origin.z + u->bounds.extent.z - 1);
	}

	const int lights = VehicleObjectLightsOn(obj, front, last, reversed, zoom);

	float scale = ObjectScale(obj);
	const int state = StateStep(obj, v->engine_type, v, EngineImageType::OnMap);
	/* Hung from the middle of its pieces (or where its coupling puts it), facing the way it goes: on a round curve as
	 * the curve has it; else smoothly as its way turns, as the view from the eyes has it (EyeOrigin(); the player,
	 * 17. 10.: "why do we turn in 45 degrees as the sprites, they can turn smoothly"), in steps of 5 degrees, its trailer
	 * bending on its coupling. */
	double ox, oy;
	float world_az;
	float along = 0.0f;
	if (pose != nullptr) {
		world_az = pose->azimuth + (reversed ? 180.0f : 0.0f);
		ObjectOriginOnMap(obj, v, d, ox, oy);
	} else {
		double facing, rolled, curvature;
		EyeOrigin(obj, v, ox, oy, facing, rolled, curvature);
		world_az = std::round((static_cast<float>(facing * 180.0 / M_PI) - 135.0f) / 5.0f) * 5.0f;
		if (SqueezedType(obj->type)) along = std::round(SqueezeAt(facing) * 20.0f) / 20.0f;
	}
	/* Up or down a hill leaning so, in steps of 2 degrees: a few pictures for the slopes of the map. */
	const int pitch = static_cast<int>(std::lround(VehicleSlopePitch(obj, v, ox, oy, (world_az + 135.0f) * M_PI / 180.0) / 2.0f)) * 2;
	_object_picture_may_wait = true;
	_object_picture_waited = false;
	place.pic = ObjectPicture(obj, scale, zoom, world_az + GetViewYaw(), world_az, along == 0.0f && SqueezedType(obj->type), state, 0, -1, 1.0f, 0, pitch, 0, lights, along, WalkCode(obj, state, WalkedOf(v, obj), scale));
	_object_picture_may_wait = false;
	if (_object_picture_waited) _vehicles_owed_pictures.insert(v->index);
	place.ax = static_cast<int>(std::lround(ox));
	place.ay = static_cast<int>(std::lround(oy));
	place.az = v->z_pos;
	if (pose != nullptr) {
		int nx = static_cast<int>(std::lround(pose->x)), ny = static_cast<int>(std::lround(pose->y));
		xmin += nx - v->x_pos; xmax += nx - v->x_pos; ymin += ny - v->y_pos; ymax += ny - v->y_pos;
		place.ax = nx;
		place.ay = ny;
	}
	place.xmin = xmin; place.ymin = ymin; place.zmin = zmin;
	place.xmax = xmax; place.ymax = ymax; place.zmax = zmax;
	return obj;
}

/** Whether the object a piece of a vehicle is a piece of draws nothing now (not a pixel of its picture, RasterObject()). */
static bool ObjectDrawsNothing(const ThreeDVehicleObject *obj, const Vehicle *v, int piece, ZoomLevel zoom)
{
	const Vehicle *anchor = PieceAway(v, obj->anchor - piece);
	if (anchor == nullptr) return false;
	VehicleObjectPlace place;
	PlaceVehicleObject(anchor, zoom, nullptr, place);
	return place.pic == nullptr;
}

/** Whether all that follows a piece of a vehicle is drawn as objects that draw nothing now (ObjectDrawsNothing()). */
static bool NothingSeenAfter(const Vehicle *last, ZoomLevel zoom)
{
	for (const Vehicle *u = last->Next(); u != nullptr;) {
		int piece = 0;
		const ThreeDVehicleObject *obj = ThreeDVehicleObjectOf(u, &piece);
		if (obj == nullptr || !ObjectDrawsNothing(obj, u, piece, zoom)) return false;
		u = PieceAway(u, static_cast<int>(obj->pieces.size()) - piece);
	}
	return true;
}

/**
 * Put a vehicle drawn as an object on the turned map (AddThreeDVehicleToViewport()), or facing north at night when
 * its model has its lights (DoDrawVehicle()): the object at the angle of the view (PlaceVehicleObject()).
 * @param pose where and at what angle to draw it instead (a round curve, round_track.h), or nullptr
 * @return whether the vehicle is drawn as an object (a piece drawn with its anchor piece counts)
 */
bool AddThreeDVehicleObjectToViewport(const Vehicle *v, ZoomLevel zoom, const RoundTrackPose *pose)
{
	VehicleObjectPlace place;
	const ThreeDVehicleObject *obj = PlaceVehicleObject(v, zoom, pose, place);
	if (obj == nullptr) return false;
	/* Articles drawn over the ones they are coupled behind (DrawnOverBefore()): one entry with them, the first article's
	 * picture first, sorted by the box of all their pieces; once in the block, from whichever piece of them comes. */
	if (pose == nullptr) {
		int piece = 0;
		ThreeDVehicleObjectOf(v, &piece);
		/* Each article as its anchor piece takes it, the object it is drawn as (PlaceVehicleObject()). */
		const Vehicle *head = PieceAway(v, obj->anchor - piece);
		const ThreeDVehicleObject *head_obj = ThreeDVehicleObjectOf(head);
		for (int depth = 0; head_obj != nullptr && depth < 4; depth++) {
			const ThreeDVehicleObject *before = nullptr;
			const Vehicle *before_anchor = nullptr;
			if (!DrawnOverBefore(head_obj, head, before, before_anchor)) break;
			head = before_anchor;
			head_obj = ThreeDVehicleObjectOf(head);
		}
		/* The articles from it on, each drawn over the one before. */
		std::vector<std::pair<const ThreeDVehicleObject *, const Vehicle *>> group;
		if (head_obj != nullptr) group.emplace_back(head_obj, head);
		while (!group.empty() && group.size() < 5) {
			const auto [a_obj, a] = group.back();
			const Vehicle *last = PieceAway(a, static_cast<int>(a_obj->pieces.size()) - 1 - a_obj->anchor);
			const Vehicle *next_first = last != nullptr ? last->Next() : nullptr;
			if (next_first == nullptr) break;
			int next_piece = 0;
			const ThreeDVehicleObject *first_obj = ThreeDVehicleObjectOf(next_first, &next_piece);
			const Vehicle *next = first_obj != nullptr ? PieceAway(next_first, first_obj->anchor - next_piece) : nullptr;
			const ThreeDVehicleObject *next_obj = ThreeDVehicleObjectOf(next);
			const ThreeDVehicleObject *before = nullptr;
			const Vehicle *before_anchor = nullptr;
			if (next_obj == nullptr || !DrawnOverBefore(next_obj, next, before, before_anchor) || before_anchor != a) break;
			group.emplace_back(next_obj, next);
		}
		if (group.size() > 1) {
			extern bool VehicleDrawnFirstInBlock(VehicleID id);
			if (!VehicleDrawnFirstInBlock(head->index)) return true;
			std::vector<TurnedVehiclePart> parts;
			int xmin = INT32_MAX, ymin = INT32_MAX, zmin = INT32_MAX, xmax = INT32_MIN, ymax = INT32_MIN, zmax = INT32_MIN;
			for (const auto &[a_obj, a] : group) {
				VehicleObjectPlace at;
				PlaceVehicleObject(a, zoom, nullptr, at);
				if (at.pic == nullptr) continue;
				parts.push_back({at.pic, at.ax, at.ay, at.az});
				xmin = std::min(xmin, at.xmin); ymin = std::min(ymin, at.ymin); zmin = std::min(zmin, at.zmin);
				xmax = std::max(xmax, at.xmax); ymax = std::max(ymax, at.ymax); zmax = std::max(zmax, at.zmax);
			}
			if (!parts.empty()) AddTurnedVehiclePictures(parts, xmin, ymin, zmin, xmax, ymax, zmax);
			return true;
		}
	}
	if (place.pic != nullptr) AddTurnedVehiclePicture(place.pic, place.ax, place.ay, place.az, place.xmin, place.ymin, place.zmin, place.xmax, place.ymax, place.zmax);
	return true;
}

/**
 * Have what a vehicle just bought is drawn with on the map made in the background while it stands in its depot: the
 * shape of its object with the parts of its state on the map, its textures painted in its colour, its picture facing
 * out of the depot at the zoom of each view (the main one, its own window's) -- a sprite at 0 degrees, a picture
 * turned. It went out of its depot drawn as nothing a while, all of it made only as it was first drawn (the player,
 * 18. 10.: "load the cars after they are bought, not when they leave the depot"); what the preview of the purchase
 * window read of the object is kept and drawn from, not read again.
 * @param front the first piece of the vehicle
 */
void PrepareThreeDVehicleObjectOnMap(const Vehicle *front)
{
	if (!ThreeDEnabled() || front == nullptr) return;
	std::set<ZoomLevel> zooms;
	for (const Window *w : Window::Iterate()) {
		if (w->viewport != nullptr) zooms.insert(w->viewport->zoom);
	}
	for (const Vehicle *u = front; u != nullptr; u = u->Next()) {
		int piece = 0;
		const ThreeDVehicleObject *obj = ThreeDVehicleObjectOf(u, &piece);
		if (obj == nullptr || piece != obj->anchor) continue;
		obj->TryGetShape(0, obj->PartsOf(StateStep(obj, u->engine_type, u, EngineImageType::OnMap)));
		for (ZoomLevel zoom : zooms) {
			if (IsViewTurned()) {
				VehicleObjectPlace place;
				PlaceVehicleObject(u, zoom, nullptr, place);
				continue;
			}
			VehicleSpriteSeq seq;
			if (u->direction == Direction::Invalid || !GetThreeDVehicleObjectSprite(u->engine_type, u, u->direction, EngineImageType::OnMap, &seq)) continue;
			for (uint k = 0; k < seq.count; k++) PrepareThreeDObjectSprite(seq.seq[k].sprite);
		}
	}
}

/**
 * Whether a vehicle is drawn facing north from its object at night, not from its sprites: its model has its lights
 * (their glass lit, kept bright by the night, as only the pictures of the turned view keep them).
 */
bool ThreeDVehicleObjectLitAtNight(const Vehicle *v)
{
	if (!VehicleLightsOn(v)) return false;
	const ThreeDVehicleObject *obj = ThreeDVehicleObjectOf(v);
	if (obj == nullptr) return false;
	const ThreeDVehicleShape *shape = obj->GetShape();
	return shape != nullptr && shape->HasLights();
}

/**
 * The lights of a vehicle drawn as an object whose model has them, where its picture is drawn (the colleague's
 * hra/3d/PROSBA-SVETLA-VOZIDEL-UZLY.md: "the lights marked in the model as the windows of the houses").
 * @param pose its round curve, or nullptr
 * @param[out] lights the lights that are on, pixels of the zoom from the world point hung from
 * @param[out] ax,ay,az that point
 * @return whether its lights are its model's (none, for a piece drawn with its anchor piece); false: drawn about its box as always
 */
bool GetThreeDVehicleObjectLights(const Vehicle *v, ZoomLevel zoom, const RoundTrackPose *pose, std::vector<ThreeDPictureLight> &lights, int &ax, int &ay, int &az)
{
	lights.clear();
	int piece = 0;
	const ThreeDVehicleObject *obj = ThreeDVehicleObjectOf(v, &piece);
	if (obj == nullptr) return false;
	/* An object that draws nothing now has no lights either (not those about its box: a lorry's trailer it has not). */
	if (ObjectDrawsNothing(obj, v, piece, zoom)) return true;
	const ThreeDVehicleShape *shape = obj->GetShape();
	if (shape == nullptr || !shape->HasLights()) return false;
	VehicleObjectPlace place;
	PlaceVehicleObject(v, zoom, pose, place);
	if (place.pic == nullptr) return true;
	lights = place.pic->lights;
	ax = place.ax;
	ay = place.ay;
	az = place.az;
	return true;
}


/**
 * The way a vehicle drawn from its object faces in the view from the eyes, finer than its eight directions (the player,
 * 16. 10.: "the car has the 45 degrees of the sprites although it is an object, it drives oddly"): an object over
 * several pieces from its last piece to its first, as a body on its bogies; one of a single piece as its way goes,
 * from where it was a few units back, turning to it smoothly. Never more than 60 degrees off its direction (a turn
 * in a depot, a vehicle that jumped): then its direction.
 * @param by_direction its direction's angle, radians (turned round when its object faces backwards)
 * @param squeezed whether it is foreshortened along itself (SqueezedType())
 */
static double EyeFacing(const Vehicle *v, const Vehicle *front, const Vehicle *last, double by_direction, bool squeezed, double &rolled, double &curvature)
{
	auto off = [&](double a) { return std::abs(std::remainder(a - by_direction, 2 * M_PI)); };
	auto ahead = [&](double a) { return off(a) > M_PI / 2 ? a + M_PI : a; };
	const uint64_t now = TimerGameTick::counter;
	EyeTrail &e = TrailOf(v, by_direction, squeezed);
	const std::pair<int, int> here{v->x_pos, v->y_pos};
	rolled = e.rolled;
	if (front != last) {
		/* Over several pieces: from its last to its first, as a body on its bogies. */
		const double dx = front->x_pos - last->x_pos, dy = front->y_pos - last->y_pos;
		curvature = 0;
		if (dx * dx + dy * dy < 4.0) return by_direction;
		const double a = ahead(std::atan2(dy, dx));
		return off(a) <= M_PI / 3 ? a : by_direction;
	}
	/* The heading from the newest place far enough back; how much the way turns, from how far the heading moved. */
	bool found = false;
	for (int i = e.count - 2; i >= 0; i--) {
		const double dx = here.first - e.at[i].first, dy = here.second - e.at[i].second;
		if (dx * dx + dy * dy < EYE_TRAIL_UNITS * EYE_TRAIL_UNITS) continue;
		const double a = ahead(std::atan2(dy, dx));
		const double was = e.target;
		e.target = off(a) <= M_PI / 3 ? a : by_direction;
		if (e.since_turn > 0.5) {
			const double bend = std::remainder(e.target - was, 2 * M_PI) / e.since_turn;
			e.curvature += (std::clamp(bend, -0.3, 0.3) - e.curvature) * 0.5;
			e.since_turn = 0;
		}
		found = true;
		break;
	}
	if (!found) e.curvature *= 0.5;
	curvature = e.curvature;
	if (!found && off(e.target) > M_PI / 3) e.target = by_direction;
	if (off(e.facing) > M_PI / 3) {
		e.facing = e.target;
	} else if (now != e.tick) {
		const double k = std::min(1.0, (now - e.tick) * EYE_TURN_A_TICK);
		e.facing = std::remainder(e.facing + std::remainder(e.target - e.facing, 2 * M_PI) * k, 2 * M_PI);
	}
	e.tick = now;
	return e.facing;
}

/**
 * A vehicle drawn from its object in the view from the eyes: the object itself in its place, at its scale, facing
 * the way it goes (EyeFacing()), foreshortened along as the game spaces its vehicles (as the map draws it), its state
 * and its lights -- not its sprites of eight directions, which are only for what is drawn from them.
 * @param[out] out where and how; out.obj nullptr for a piece drawn with its anchor piece (nothing of its own)
 * @return whether the vehicle is drawn from its object
 */
/**
 * Where an object hung from its anchor piece is in the view from the eyes, and the way it faces (EyeFacing()): as on
 * the map (ObjectOriginOnMap()), its front coupling on the back one of the article before it, each at its own
 * heading, foreshortened along as it is drawn.
 * @param[out] x,y its origin, world units
 * @param[out] facing its heading, radians
 * @param depth how many articles before it are placed so (a few at most)
 */
static void EyeOrigin(const ThreeDVehicleObject *obj, const Vehicle *anchor, double &x, double &y, double &facing, double &rolled, double &curvature, int depth)
{
	const bool reversed = PieceReversed(anchor);
	const Vehicle *front, *last;
	ObjectEnds(obj, anchor, front, last);
	facing = EyeFacing(anchor, reversed ? last : front, reversed ? front : last, DirectionAngle(PieceDirection(anchor)), SqueezedType(obj->type), rolled, curvature);
	/* Hung from the middle of its pieces, as on the map: from the anchor piece that many steps along. */
	const double along = MiddleFromPiece(obj, ObjectLengths(obj, anchor, obj->anchor), obj->anchor) * ChainStep(facing);
	x = anchor->x_pos + along * std::cos(facing);
	y = anchor->y_pos + along * std::sin(facing);
	const ThreeDVehicleObject *before = nullptr;
	const Vehicle *before_anchor = nullptr;
	if (depth >= 4 || !CoupledBehind(obj, anchor, before, before_anchor)) return;
	double bx, by, bf, br, bc;
	EyeOrigin(before, before_anchor, bx, by, bf, br, bc, depth + 1);
	const bool squeezed = SqueezedType(obj->type);
	const double back = before->couple_back[0] * ObjectScale(before) * UNITS_PER_METRE * (squeezed ? SqueezeAt(bf) : 1.0f);
	const double ahead = obj->couple_front[0] * ObjectScale(obj) * UNITS_PER_METRE * (squeezed ? SqueezeAt(facing) : 1.0f);
	x = bx + back * std::cos(bf) - ahead * std::cos(facing);
	y = by + back * std::sin(bf) - ahead * std::sin(facing);
}

bool GetThreeDEyeVehicle(const Vehicle *v, ThreeDEyeVehicle &out)
{
	out = {};
	int piece = 0;
	const ThreeDVehicleObject *obj = ThreeDVehicleObjectOf(v, &piece);
	if (obj == nullptr || obj->GetShape() == nullptr) return false;
	if (piece != obj->anchor) return true;
	const bool reversed = PieceReversed(v);
	const Vehicle *front, *last;
	ObjectEnds(obj, v, front, last);
	double x, y, facing, rolled = 0, curvature = 0;
	EyeOrigin(obj, v, x, y, facing, rolled, curvature);
	out.rolled = reversed ? -rolled : rolled;
	out.curvature = curvature;

	out.obj = obj;
	out.scale = ObjectScale(obj);
	out.along = SqueezedType(obj->type) ? SqueezeAt(facing) : 1.0f;
	out.x = x;
	out.y = y;
	out.z = v->z_pos;
	out.facing = static_cast<float>(facing * 180.0 / M_PI);
	out.pitch = VehicleSlopePitch(obj, v, x, y, facing);
	out.state = StateStep(obj, v->engine_type, v, EngineImageType::OnMap);
	out.walk = WalkCode(obj, out.state, WalkedOf(v, obj), out.scale);
	out.lights = VehicleObjectLightsOn(obj, front, last, reversed, ZoomLevel::Normal);
	return true;
}

/**
 * The lights of an object drawn in the view from the eyes, in the world (a vehicle of the game, GetThreeDEyeVehicle(),
 * or a car of the game's own driving about): where its lamps are, its object turned as it faces and as big as it is
 * drawn; the pool of its headlights on the way ahead.
 */
void GetThreeDEyeObjectLights(const ThreeDEyeVehicle &ev, std::vector<ThreeDWorldLight> &lights)
{
	lights.clear();
	const ThreeDVehicleShape *shape = ev.obj != nullptr ? ev.obj->GetShape() : nullptr;
	if (shape == nullptr || ev.lights == 0) return;
	/* The object's front and left in the world, as RasterObject() lights it; turned as its state has it. */
	const double wa = ev.facing * M_PI / 180.0;
	const double fx = std::cos(wa), fy = std::sin(wa), lx = -fy, ly = fx;
	const double ta = StateTurn(ev.obj, ev.state) * M_PI / 180.0, tc = std::cos(ta), ts = std::sin(ta);
	/* Its front in the world, turned as its state has it: where the headlights shine, the red lights the other way. */
	const double ahead_x = tc * fx + ts * lx, ahead_y = tc * fy + ts * ly;
	auto add = [&](float ox, float oy, float pz, ThreeDPictureLight::Kind kind, bool turned_round = false) {
		const double px = ox * tc - oy * ts, py = ox * ts + oy * tc;
		const double x = px * ev.scale * ev.along * UNITS_PER_METRE, y = py * ev.scale * UNITS_PER_METRE;
		const double way = (kind == ThreeDPictureLight::Kind::Front ? 1.0 : kind == ThreeDPictureLight::Kind::Back ? -1.0 : 0.0) * (turned_round ? -1.0 : 1.0);
		lights.push_back({ev.x + x * fx + y * lx, ev.y + x * fy + y * ly, ev.z + pz * ev.scale * OBJECT_HEIGHT_PER_METRE, kind, ahead_x * way, ahead_y * way});
	};
	/* The lamps of the parts its state shows only (the cab's on the cab shown). */
	const ThreeDParts parts = ev.obj->PartsOf(ev.state);
	auto shown = [&](const std::vector<uint8_t> &of, size_t i) { return ThreeDVehicleShape::Shown(i < of.size() ? of[i] : 0, parts); };
	if ((ev.lights & LIGHTS_FRONT) != 0 && !shape->front_lights.empty()) {
		float ahead = -std::numeric_limits<float>::infinity(), mid = 0;
		size_t n = 0;
		for (size_t i = 0; i < shape->front_lights.size() / 3; i++) n += shown(shape->front_part, i) ? 1 : 0;
		for (size_t i = 0; i < shape->front_lights.size() / 3; i++) {
			if (!shown(shape->front_part, i)) continue;
			add(shape->front_lights[i * 3], shape->front_lights[i * 3 + 1], shape->front_lights[i * 3 + 2], ThreeDPictureLight::Kind::Front);
			ahead = std::max(ahead, shape->front_lights[i * 3]);
			mid += shape->front_lights[i * 3 + 1] / n;
		}
		if (n > 0) {
			add(ahead + 5.0f, mid, 0.0f, ThreeDPictureLight::Kind::PoolNear);
			add(ahead + 11.0f, mid, 0.0f, ThreeDPictureLight::Kind::Pool);
		}
	}
	if ((ev.lights & LIGHTS_BACK) != 0) {
		for (size_t i = 0; i * 3 + 2 < shape->back_lights.size(); i++) {
			if (shown(shape->back_part, i)) add(shape->back_lights[i * 3], shape->back_lights[i * 3 + 1], shape->back_lights[i * 3 + 2], ThreeDPictureLight::Kind::Back);
		}
	}
	/* Going back first: the headlights on its back end shine back, the red lights on its front end the other way. */
	if ((ev.lights & LIGHTS_FRONT_AT_BACK) != 0 && !shape->front_back_lights.empty()) {
		float behind = std::numeric_limits<float>::infinity(), mid = 0;
		const size_t n = shape->front_back_lights.size() / 3;
		for (size_t i = 0; i < n; i++) {
			add(shape->front_back_lights[i * 3], shape->front_back_lights[i * 3 + 1], shape->front_back_lights[i * 3 + 2], ThreeDPictureLight::Kind::Front, true);
			behind = std::min(behind, shape->front_back_lights[i * 3]);
			mid += shape->front_back_lights[i * 3 + 1] / n;
		}
		add(behind - 5.0f, mid, 0.0f, ThreeDPictureLight::Kind::PoolNear);
		add(behind - 11.0f, mid, 0.0f, ThreeDPictureLight::Kind::Pool);
	}
	if ((ev.lights & LIGHTS_BACK_AT_FRONT) != 0) {
		for (size_t i = 0; i * 3 + 2 < shape->back_front_lights.size(); i++) {
			add(shape->back_front_lights[i * 3], shape->back_front_lights[i * 3 + 1], shape->back_front_lights[i * 3 + 2], ThreeDPictureLight::Kind::Back, true);
		}
	}
}

/**
 * The lights of a vehicle drawn as an object whose model has them, in the world (the view from the eyes,
 * GetThreeDEyeObjectLights()).
 * @return whether its lights are its model's (none, for a piece drawn with its anchor piece); false: about its box as always
 */
bool GetThreeDVehicleObjectWorldLights(const Vehicle *v, std::vector<ThreeDWorldLight> &lights)
{
	lights.clear();
	ThreeDEyeVehicle ev;
	if (!GetThreeDEyeVehicle(v, ev)) return false;
	if (ev.obj == nullptr) return true;
	const ThreeDVehicleShape *shape = ev.obj->GetShape();
	if (shape == nullptr || !shape->HasLights()) return false;
	GetThreeDEyeObjectLights(ev, lights);
	return true;
}

/** The body's textures painted as their states have them, the add-ons' coloured, for the view from the eyes (GetThreeDEyeVehicleTexture()); made on either thread. */
static std::map<std::tuple<const ThreeDVehicleObject *, int, int, int>, ThreeDObjectTexture> _eye_painted;
static std::mutex _eye_painted_mutex;
/** Those asked to be made in the background and not made yet (TryGetThreeDEyeVehicleTexture()). */
static std::set<std::tuple<const ThreeDVehicleObject *, int, int, int>> _eye_painted_asked;
/** Those drawn since the last trimming (TrimThreeDEyePainted()). */
static std::set<std::tuple<const ThreeDVehicleObject *, int, int, int>> _eye_painted_seen;
/**
 * Those whose state paints nothing over them: the object's own texture is drawn. Without knowing it, the asking for
 * them went on for ever -- each frame another job for the thread in the background, the model never ready (its
 * billboard drawn, blocky and at the angle of another picture close by) and the pictures of the vehicles waiting
 * behind the jobs (the player, 17. 10.: "the cars are blocks now, strangely turned").
 */
static std::set<std::tuple<const ThreeDVehicleObject *, int, int, int>> _eye_painted_plain;

/** The objects are read anew: their painted textures go with them. */
static void ForgetEyePainted()
{
	std::lock_guard lock(_eye_painted_mutex);
	_eye_painted.clear();
	_eye_painted_asked.clear();
	_eye_painted_seen.clear();
	_eye_painted_plain.clear();
	_eye_trails.clear();
	ForgetGpuDetailColours();
}

/**
 * Between the frames (ForgetUnusedThreeDObjects()): the painted textures not drawn since the last time let go of -- a
 * texture of a lorry close is sixty four megabytes --, made again when drawn (the textures by need).
 */
void TrimThreeDEyePainted()
{
	std::lock_guard lock(_eye_painted_mutex);
	size_t freed = 0;
	for (auto it = _eye_painted.begin(); it != _eye_painted.end();) {
		if (_eye_painted_seen.contains(it->first)) {
			++it;
			continue;
		}
		freed += it->second.px.capacity() * sizeof(uint32_t);
		it = _eye_painted.erase(it);
	}
	_eye_painted_seen.clear();
	if (freed > 0) Debug(misc, 1, "3D textures: {:.1f} MB painted for the view from the eyes and not drawn let go of", freed / (1024.0 * 1024.0));
}

/** The level of detail of an object's textures for the view from the eyes: about a texel to a pixel. */
static int EyeTextureLevel(const ThreeDVehicleObject *obj, const ThreeDVehicleShape *shape, double pixels_per_metre)
{
	int level = 0;
	for (double d = TexelsPerMetre(*obj, *shape); d >= 2.0 * pixels_per_metre && level < 12; d /= 2.0) level++;
	return level;
}

/**
 * A texture of an object as the view from the eyes draws it by its triangles: the body's (0) painted as its state has
 * it -- its colour, faded, rusted, as RasterObject() paints it (ObjectPaintOfState) --, the others as they are; at the
 * level of detail of a texel to a pixel.
 * @param pixels_per_metre how big a metre of the object is on the picture
 */
const ThreeDObjectTexture *GetThreeDEyeVehicleTexture(const ThreeDVehicleObject *obj, int state, double pixels_per_metre, int index)
{
	const ThreeDVehicleShape *shape = obj->GetShape();
	if (shape == nullptr) return nullptr;
	const int level = EyeTextureLevel(obj, shape, pixels_per_metre);
	const ThreeDObjectTexture *raw = obj->GetTexture(level, index);
	if (state == 0 || raw == nullptr) return raw;
	const std::tuple<const ThreeDVehicleObject *, int, int, int> key(obj, state, level, index);
	{
		std::lock_guard lock(_eye_painted_mutex);
		auto it = _eye_painted.find(key);
		if (it != _eye_painted.end()) {
			_eye_painted_seen.insert(key);
			return &it->second;
		}
	}
	/* Made outside the lock (a texture of millions of texels), kept under it: whichever thread is first. */
	ThreeDObjectTexture t;
	auto keep = [&]() -> const ThreeDObjectTexture * {
		std::lock_guard lock(_eye_painted_mutex);
		_eye_painted_asked.erase(key);
		_eye_painted_seen.insert(key);
		return &_eye_painted.emplace(key, std::move(t)).first->second;
	};
	/* Nothing to paint: the object's own, and known so (TryGetThreeDEyeVehicleTexture()). */
	auto plain = [&]() {
		std::lock_guard lock(_eye_painted_mutex);
		_eye_painted_asked.erase(key);
		_eye_painted_plain.insert(key);
		return raw;
	};
	const ThreeDVehicleState *st = obj->State(state);
	/* The mask of details of the texture in the vehicle's colours of its set (callback 1C1), as RasterObject() paints it. */
	const ObjectDetailPaint details(*obj, *shape, st, std::max<size_t>(1, shape->pngs.size()));
	const ThreeDDetailMask *detail = details.MaskOf(static_cast<size_t>(index));
	if (index != 0) {
		/* An add-on's texture: the colour its state gives it over its mask, at the texture's lightness. */
		const int addon = static_cast<size_t>(index) < shape->tex_addon.size() ? shape->tex_addon[index] : -1;
		const bool coloured = addon >= 0 && st != nullptr && static_cast<size_t>(addon) < st->addon_colours.size() && st->addon_colours[addon] != 0;
		const ThreeDObjectTexture *mask = coloured ? obj->GetAddonMask(addon, level) : nullptr;
		if (mask == nullptr && detail == nullptr) return plain();
		const Colour colour(coloured ? st->addon_colours[addon] : 0);
		const float start = addon >= 0 && obj->addons[addon].start > 0 ? static_cast<float>(obj->addons[addon].start) : 255.0f;
		t.w = raw->w;
		t.h = raw->h;
		t.px = raw->px;
		for (int y = 0; y < raw->h; y++) {
			const float v = (y + 0.5f) / raw->h;
			const int my = mask != nullptr ? std::min(mask->h - 1, static_cast<int>(v * mask->h)) : 0;
			for (int x = 0; x < raw->w; x++) {
				uint32_t &texel = t.px[static_cast<size_t>(y) * raw->w + x];
				if (texel == 0) continue;
				const float u = (x + 0.5f) / raw->w;
				const Colour c(texel);
				float cr = c.r, cg = c.g, cb = c.b;
				if (detail != nullptr && details.Paint(detail, u, v, cr, cg, cb)) {
					texel = Colour(static_cast<uint8_t>(cr), static_cast<uint8_t>(cg), static_cast<uint8_t>(cb), c.a).data;
					continue;
				}
				if (mask == nullptr) continue;
				const int mx = std::min(mask->w - 1, static_cast<int>(u * mask->w));
				const float m = Colour(mask->px[static_cast<size_t>(my) * mask->w + mx]).r / 255.0f;
				if (m <= 0) continue;
				const float l = std::max({c.r, c.g, c.b}) / start;
				auto mix = [&](uint8_t from, uint8_t to) { return static_cast<uint8_t>(from + (std::min(255.0f, to * l) - from) * m); };
				texel = Colour(mix(c.r, colour.r), mix(c.g, colour.g), mix(c.b, colour.b), c.a).data;
			}
		}
		return keep();
	}
	const ObjectPaintOfState painted(*obj, *shape, level, FadeAmount(obj, state), RustTime(obj, state), PaletteColour(obj, state));
	if (painted.mask == nullptr && !painted.whole && painted.rust_mask == nullptr && detail == nullptr) return plain();
	t.w = raw->w;
	t.h = raw->h;
	t.px.resize(raw->px.size());
	for (int y = 0; y < raw->h; y++) {
		const float v = (y + 0.5f) / raw->h;
		for (int x = 0; x < raw->w; x++) {
			const size_t at = static_cast<size_t>(y) * raw->w + x;
			const uint32_t texel = raw->px[at];
			if (texel == 0) continue; // a hole in the texture
			const float u = (x + 0.5f) / raw->w;
			const Colour col(texel);
			float cr = col.r, cg = col.g, cb = col.b;
			if (detail == nullptr || !details.Paint(detail, u, v, cr, cg, cb)) painted.Paint(u, v, cr, cg, cb);
			painted.Rust(u, v, cr, cg, cb);
			t.px[at] = Colour(static_cast<uint8_t>(std::min(255.0f, cr)), static_cast<uint8_t>(std::min(255.0f, cg)), static_cast<uint8_t>(std::min(255.0f, cb)), col.a).data;
		}
	}
	return keep();
}

/**
 * A texture of an object for the view from the eyes (GetThreeDEyeVehicleTexture()) if it is made already; else
 * nullptr, and it is made in the background (ThreeDLoadInBackground()) -- its shape and its textures read, painted --
 * for the frames to come. The textures of a state that paints nothing are the object's own.
 */
const ThreeDObjectTexture *TryGetThreeDEyeVehicleTexture(const ThreeDVehicleObject *obj, int state, double pixels_per_metre, int index)
{
	const ThreeDVehicleShape *shape = obj->TryGetShape();
	if (shape == nullptr) return nullptr;
	const int level = EyeTextureLevel(obj, shape, pixels_per_metre);
	const ThreeDObjectTexture *raw = obj->TryGetTexture(level, index);
	if (raw == nullptr) return nullptr;
	if (state == 0) return raw;
	const std::tuple<const ThreeDVehicleObject *, int, int, int> key(obj, state, level, index);
	{
		std::lock_guard lock(_eye_painted_mutex);
		auto it = _eye_painted.find(key);
		if (it != _eye_painted.end()) {
			_eye_painted_seen.insert(key);
			return &it->second;
		}
		if (_eye_painted_plain.contains(key)) return raw;
		if (!_eye_painted_asked.insert(key).second) return nullptr; // asked already
	}
	ThreeDLoadInBackground([obj, state, pixels_per_metre, index, key] {
		/* The painting may find nothing to paint: then the texture is the object's own, and the asking is over. */
		if (GetThreeDEyeVehicleTexture(obj, state, pixels_per_metre, index) != nullptr) {
			std::lock_guard lock(_eye_painted_mutex);
			_eye_painted_asked.erase(key);
		}
	});
	if (!_three_d_load_at_once) return nullptr;
	/* The rig: made now. The texture of a state that paints nothing is the object's own. */
	std::lock_guard lock(_eye_painted_mutex);
	auto it = _eye_painted.find(key);
	return it != _eye_painted.end() ? &it->second : raw;
}

/**
 * The picture of an object far in the view from the eyes, standing there as a billboard: at the angle the eye sees it
 * at (both angles in steps of 5 degrees, so that a few pictures serve a vehicle as it goes), foreshortened as it is
 * drawn near, in its state, its lights on.
 * @param yaw the angle the eye looks at it from, as GetThreeDFigurePicture() counts it
 */
const ThreeDModelPicture *GetThreeDEyeVehiclePicture(const ThreeDEyeVehicle &ev, ZoomLevel zoom, float yaw)
{
	if (ev.obj == nullptr) return nullptr;
	const float world_az = std::round((ev.facing - 135.0f) / 5.0f) * 5.0f;
	const float cam = std::round(yaw / 5.0f) * 5.0f;
	const float along = std::round(ev.along * 20.0f) / 20.0f;
	/* Up or down a hill leaning as near, in steps of 2 degrees as on the map. */
	const int pitch = static_cast<int>(std::lround(ev.pitch / 2.0f)) * 2;
	/* On the thread too (as the vehicles of the map): the view draws again every frame as it goes, and the nearest made
	 * stands in meanwhile. */
	_object_picture_may_wait = true;
	const ThreeDModelPicture *pic = ObjectPicture(ev.obj, ev.scale, zoom, world_az + cam, world_az, false, ev.state, 0, -1, 1.0f, 0, pitch, 0, ev.lights, along, ev.walk);
	_object_picture_may_wait = false;
	return pic;
}

/**
 * The picture of an engine drawn from its 3D object at an angle, for the turning preview beside the purchase window:
 * the object itself, as the turned map draws it (not the likeness made from its sprites); nullptr for an engine
 * without an object.
 * @param azimuth the angle, as the likeness counts it (ViewAzimuth())
 */
const ThreeDModelPicture *GetThreeDVehicleObjectPreviewPicture(EngineID engine, ZoomLevel zoom, float azimuth)
{
	const ThreeDVehicleObject *obj = GetThreeDVehicleObject(engine);
	if (obj == nullptr) return nullptr;
	return ObjectPicture(obj, ObjectScale(obj), zoom, azimuth, azimuth, SqueezedType(obj->type), StateStep(obj, engine, nullptr, EngineImageType::Purchase));
}

/** A frame of the turning preview beside the purchase window (GetThreeDVehicleObjectPreviewFit()). */
struct PreviewFrame {
	ThreeDModelPicture picture; ///< width 0: nothing of it seen
	int cx = 0, cy = 0;         ///< where the middle of its plan, half its height up, is on the picture, pixels from its top left
};
/** What a frame of the preview is of: the object, its state, the side of the square, the angle in whole degrees. */
using PreviewFrameKey = std::tuple<const ThreeDVehicleObject *, int, int, int>;
/**
 * The frames of the preview of the engine chosen, made on the thread in the background a few ahead of the turn (the
 * player, 17. 10.: "what has the preview got in common with the rest, it is only to turn there, no wonder it bites"),
 * and those asked: of the one engine only, they go when another is chosen. The main thread draws from them without
 * the lock: a frame stays where it is till the main thread lets it go. Under _preview_mutex.
 */
static std::map<PreviewFrameKey, PreviewFrame> _preview_frames;
static std::set<PreviewFrameKey> _preview_frames_asked;
/** The extent of an object in a state (the parts it shows), for its middle and its size in the square. Under _preview_mutex. */
static std::map<std::pair<const ThreeDVehicleObject *, int>, std::array<float, 6>> _preview_extents;
static std::mutex _preview_mutex;

/** The sprites for the windows and the frames of the previews go with the objects (ResetThreeDVehicleObjectSprites(), the thread stopped). */
static void ForgetGuiSpritesAndPreviews()
{
	{
		std::lock_guard lock(_bg_sprites_mutex);
		_bg_sprites_made.clear();
		_bg_sprites_asked.clear();
	}
	ForgetThreeDVehicleObjectPreviews();
	std::lock_guard lock(_preview_mutex);
	_preview_extents.clear();
}

/** The frames of the previews made go: no preview of a vehicle is shown (three_d_preview_gui.cpp). */
void ForgetThreeDVehicleObjectPreviews()
{
	std::lock_guard lock(_preview_mutex);
	_preview_frames.clear();
	_preview_frames_asked.clear();
}

/**
 * Make a frame of the preview (the thread): the object as big as the square beside the purchase window (the player,
 * 17. 10.: "imagine the window is there, the model as big as from one edge of that window to the other"), turning about
 * the middle of its plan, not its origin (a semi-trailer's is its kingpin at the front: it turned about its front);
 * drawn as it is, not foreshortened as the map spaces its vehicles. Its extent is that of the parts its state shows
 * (the colleague's lorries: the main file only their length, the cab, the body and the trailer add-ons).
 */
static void MakePreviewFrame(const PreviewFrameKey &key, float fade, float rust_time, uint32_t paint_colour, PreviewFrame &f)
{
	const auto [obj, state, side, step] = key;
	const ThreeDVehicleShape *shape = obj->GetShape(0, obj->PartsOf(state));
	if (shape == nullptr) return;
	std::array<float, 6> e;
	bool known = false;
	{
		std::lock_guard lock(_preview_mutex);
		auto it = _preview_extents.find({obj, state});
		if (it != _preview_extents.end()) {
			e = it->second;
			known = true;
		}
	}
	if (!known) {
		const ThreeDParts parts = obj->PartsOf(state);
		e = {std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()};
		for (size_t t = 0; t + 2 < shape->tri.size(); t += 3) {
			const uint8_t part = t / 3 < shape->tri_part.size() ? shape->tri_part[t / 3] : 0;
			if (part != ThreeDVehicleShape::PART_GLASS && !ThreeDVehicleShape::Shown(part, parts)) continue;
			for (int k = 0; k < 3; k++) {
				const float *v = &shape->pos[static_cast<size_t>(shape->tri[t + k]) * 3];
				for (int a = 0; a < 3; a++) {
					e[a * 2] = std::min(e[a * 2], v[a]);
					e[a * 2 + 1] = std::max(e[a * 2 + 1], v[a]);
				}
			}
		}
		if (!(e[0] <= e[1])) e = {shape->x0, shape->x1, shape->y0, shape->y1, shape->z0, shape->z1};
		std::lock_guard lock(_preview_mutex);
		_preview_extents[{obj, state}] = e;
	}
	const float diagonal = std::hypot(e[1] - e[0], e[3] - e[2]);
	if (diagonal <= 0.0f) return;
	constexpr ZoomLevel zoom = ZoomLevel::In4x;
	const float pxm = PXM_16X / (1 << to_underlying(zoom));
	/* The plan's diagonal fills the square, so that it fits at every angle. */
	const float scale = std::max(0.001f, side * 0.94f / (diagonal * pxm));
	const float az = static_cast<float>(step);
	ObjectRaster r;
	if (!RasterObject(*obj, scale, az, az, 1.0f, pxm, fade, r, 0, rust_time, 0, paint_colour, 1.0f, 0.0f, 0.0f, 0.0f, 0, state, 0)) return;
	PictureOfRaster(f.picture, r);
	/* Its middle as RasterObject() places a point: turned by the angle, seen from above. */
	const float a = az * static_cast<float>(M_PI) / 180.0f, ca = std::cos(a), sa = std::sin(a);
	const float mx = (e[0] + e[1]) / 2 * scale, my = (e[2] + e[3]) / 2 * scale, mz = (e[4] + e[5]) / 2 * scale;
	const float xr = mx * ca - my * sa, yr = mx * sa + my * ca;
	f.cx = static_cast<int>(std::lround(xr * pxm)) - f.picture.left;
	f.cy = static_cast<int>(std::lround((-yr * CAM_SIN - mz * CAM_COS) * pxm)) - f.picture.top;
}

/** The frame of the preview of an engine at an angle (whole degrees): whether it is made; asked of the thread if not. */
static bool AskPreviewFrame(EngineID engine, int side, float azimuth, PreviewFrameKey &key)
{
	const ThreeDVehicleObject *obj = GetThreeDVehicleObject(engine);
	if (obj == nullptr || side <= 0) return false;
	const int state = StateStep(obj, engine, nullptr, EngineImageType::Purchase);
	key = PreviewFrameKey(obj, state, side, static_cast<int>(std::lround(std::fmod(azimuth + 720.0f, 360.0f))) % 360);
	{
		std::lock_guard lock(_preview_mutex);
		if (_preview_frames.contains(key)) return true;
		/* Another engine, or the square another size: the frames of the one before go. */
		auto other = [&key](const PreviewFrameKey &k) { return std::get<0>(k) != std::get<0>(key) || std::get<1>(k) != std::get<1>(key) || std::get<2>(k) != std::get<2>(key); };
		if ((!_preview_frames.empty() && other(_preview_frames.begin()->first)) || (!_preview_frames_asked.empty() && other(*_preview_frames_asked.begin()))) {
			_preview_frames.clear();
			_preview_frames_asked.clear();
		}
		if (!_preview_frames_asked.insert(key).second) return false; // asked already
	}
	/* What the game's state gives it now (its paint faded by the date), for the thread. */
	const float fade = FadeAmount(obj, state), rust_time = RustTime(obj, state);
	const uint32_t paint_colour = PaletteColour(obj, state);
	ThreeDLoadInBackground([key, fade, rust_time, paint_colour] {
		{
			std::lock_guard lock(_preview_mutex);
			if (!_preview_frames_asked.contains(key)) return; // another engine chosen meanwhile
		}
		PreviewFrame f;
		MakePreviewFrame(key, fade, rust_time, paint_colour, f);
		std::lock_guard lock(_preview_mutex);
		if (_preview_frames_asked.erase(key) == 0) return;
		_preview_frames.emplace(key, std::move(f));
	});
	/* Without the thread it was made now. */
	std::lock_guard lock(_preview_mutex);
	return _preview_frames.contains(key);
}

/**
 * Whether the frame of the turning preview of an engine's 3D object at an angle is made (GetThreeDVehicleObjectPreviewFit());
 * if not, it is asked of the thread in the background: the preview asks a few ahead of its turn and turns on when the
 * next is there.
 * @param side the square's side, pixels
 * @param azimuth the angle, as the likeness counts it (ViewAzimuth())
 */
bool ThreeDVehicleObjectPreviewFitMade(EngineID engine, int side, float azimuth)
{
	PreviewFrameKey key;
	return AskPreviewFrame(engine, side, azimuth, key);
}

/**
 * The picture of an engine's 3D object for the turning preview beside the purchase window, as big as a square beside
 * it, turning about the middle of its plan (MakePreviewFrame()); made on the thread in the background.
 * @param side the square's side, pixels: the plan's diagonal fills it, so that it fits at every angle
 * @param azimuth the angle, as the likeness counts it (ViewAzimuth())
 * @param[out] cx,cy where the middle of its plan, half its height up, is on the picture, pixels from its top left
 * @return the picture; nullptr for an engine without an object, or while it is being made
 */
const ThreeDModelPicture *GetThreeDVehicleObjectPreviewFit(EngineID engine, int side, float azimuth, int &cx, int &cy)
{
	PreviewFrameKey key;
	if (!AskPreviewFrame(engine, side, azimuth, key)) return nullptr;
	std::lock_guard lock(_preview_mutex);
	auto it = _preview_frames.find(key);
	if (it == _preview_frames.end() || it->second.picture.width <= 0) return nullptr;
	cx = it->second.cx;
	cy = it->second.cy;
	return &it->second.picture;
}

/** The huts whose fireworks this drawing of the turned view puts on (QueueThreeDFireworks()). */
static std::vector<TileIndex> _turned_fireworks;

/** A hut's fireworks to be drawn as the 3D object, after the tiles of the turned view (they would go with the hut's own sprites, left out where the hut is drawn from its renders). */
void QueueThreeDFireworks(TileIndex hut_front)
{
	if (std::ranges::find(_turned_fireworks, hut_front) == _turned_fireworks.end()) _turned_fireworks.push_back(hut_front);
}

/**
 * Put the fireworks over the huts of this drawing on the turned view, from the colleague's object (the player: "and
 * then fireworks 3D, use the changes of colour of one and the same object, let it be all colours"): the launch and the
 * three phases of the ball as its frames, the phase as the hut's sprites go (HutFireworkPhase()), each phase of each
 * firework its colour of the palette; over the middle of the hut's front tile, at true size, unlit (it glows).
 */
void AddThreeDEffectsToViewport(ZoomLevel zoom)
{
	const ThreeDVehicleObject *fx = _turned_fireworks.empty() ? nullptr : GetThreeDEffect("ohnostroj");
	for (TileIndex tile : _turned_fireworks) {
		if (fx == nullptr) break;
		uint part = 0;
		uint32_t hash = 0;
		if (!HutFireworkPhase(tile, TimerGameTick::counter, part, hash) || part >= 4) continue;
		int frame = fx->frames > 0 ? static_cast<int>(std::min<uint>(part, fx->frames - 1)) : 0;
		int tint = part == 0 || fx->tint_palette.empty() ? -1 : static_cast<int>(hash % fx->tint_palette.size());
		const ThreeDVehicleShape *shape = fx->GetShape(frame);
		if (shape == nullptr) continue;
		const ThreeDModelPicture *pic = ObjectPicture(fx, 1.0f, zoom, GetViewYaw(), 0.0f, false, 0, frame, tint);
		if (pic == nullptr) continue;
		int x = TileX(tile) * TILE_SIZE + TILE_SIZE / 2, y = TileY(tile) * TILE_SIZE + TILE_SIZE / 2, z = GetTileMaxPixelZ(tile);
		int height = std::max(1, static_cast<int>(std::lround(shape->z1 * 8.0 / 3.0)));
		AddTurnedVehiclePicture(pic, x, y, z, x - 1, y - 1, z, x + 1, y + 1, z + height);
	}
	_turned_fireworks.clear();
}

/** A figure that moves drawn on the map: its tile is drawn again when its frame changes (AddThreeDFigureToViewport()). */
struct MovingFigure {
	TileIndex tile;
	int ticks;     ///< the ticks a frame of it is held
	uint64_t step; ///< the frame's step it was drawn in: the game's tick counter / ticks
	int z0, z1;    ///< the heights it is between, units; z0 INT_MIN not known: its tile as high as a building may be
	double x0, y0, x1, y1; ///< the ground it is over, world units, when its heights are known
};
static std::vector<MovingFigure> _moving_figures;

/** Draw the tiles of the figures that move again when their frames change, every tick of the game (only the
 * screen, nothing of the game); a figure no longer drawn drops out. */
static const IntervalTimer<TimerGameTick> _moving_figures_timer({TimerGameTick::Priority::None, 1}, [](auto) {
	for (auto it = _moving_figures.begin(); it != _moving_figures.end();) {
		if (TimerGameTick::counter / it->ticks == it->step) {
			++it;
			continue;
		}
		if (it->z0 == INT_MIN) {
			MarkTileDirtyByTile(it->tile);
		} else {
			MarkWorldBoxDirty(it->x0, it->y0, it->x1, it->y1, it->z0, it->z1);
		}
		it = _moving_figures.erase(it);
	}
});

/**
 * A figure standing on the turned map (the girls at a bus stop): drawn from its object at the angle of the view,
 * lit as the vehicles, hung from where its feet stand, sorted by a small box about them. A figure that moves
 * ("animace") is drawn in its frame of the game's tick, and its tile is drawn again when the frame changes.
 * @param name the figure's name in the index ("druh": "postava")
 * @param scale its size against true metres (the girls of a stop are drawn twice theirs, as their sprites are)
 * @param x,y,z where its feet stand, world units
 * @param facing the way it faces, degrees in the world: 0 along +x, 90 along +y (the colleague's renders' angles)
 * @param zoom the zoom of the view
 * @param tile the tile it stands on, drawn again when it moves
 * @param phase how many frames on it is from the others (a hash of its tile: the girls of a map do not all move at once)
 * @return whether it was drawn so; else the caller draws its sprite
 */
/**
 * A vehicle object of no vehicle on the turned map (a car of the game's own driving about, three_d_walkers.h): at a
 * place, facing a way (degrees, 0 along +x), sorted by a box about it as long as it is.
 */
bool AddThreeDObjectToViewport(const ThreeDVehicleObject *obj, float scale, int x, int y, int z, float facing, ZoomLevel zoom, bool lit)
{
	const ThreeDVehicleShape *shape = obj != nullptr ? obj->GetShape() : nullptr;
	if (shape == nullptr) return false;
	float world_az = facing - 135.0f;
	/* At night (lit) with its lights on, front and back, when its model has them. */
	const int lights = lit && shape->HasLights() ? LIGHTS_FRONT | LIGHTS_BACK : 0;
	const ThreeDModelPicture *pic = ObjectPicture(obj, scale, zoom, world_az + GetViewYaw(), world_az, false, 0, 0, -1, 1.0f, 0, 0, 0, lights);
	if (pic == nullptr) return false;
	int half = std::max(1, static_cast<int>(std::lround(std::max(shape->x1 - shape->x0, shape->y1 - shape->y0) * scale * OBJECT_UNITS_PER_METRE / 2)));
	int height = std::max(1, static_cast<int>(std::lround(shape->z1 * scale * 8.0 / 3.0)));
	AddTurnedVehiclePicture(pic, x, y, z, x - half, y - half, z, x + half, y + half, z + height);
	return true;
}

/**
 * The lights of an object drawn by itself at night (AddThreeDObjectToViewport() lit, a car of the game's own), pixels of
 * the zoom from where it stands; nullptr when its model has none.
 */
const std::vector<ThreeDPictureLight> *GetThreeDObjectLights(const ThreeDVehicleObject *obj, float scale, float facing, ZoomLevel zoom)
{
	const ThreeDVehicleShape *shape = obj != nullptr ? obj->GetShape() : nullptr;
	if (shape == nullptr || !shape->HasLights()) return nullptr;
	float world_az = facing - 135.0f;
	const ThreeDModelPicture *pic = ObjectPicture(obj, scale, zoom, world_az + GetViewYaw(), world_az, false, 0, 0, -1, 1.0f, 0, 0, 0, LIGHTS_FRONT | LIGHTS_BACK);
	return pic != nullptr ? &pic->lights : nullptr;
}

/** The poses of a round of a creature's animation on the map (ThreeDMapPose()). */
static constexpr int MAP_POSES = 4;

/**
 * The frame of a creature's animation drawn on the map: one of MAP_POSES poses of its round, evenly through it (one that
 * does not go round: its first and its last among them). Each pose at each angle is a picture of its own; with its bones
 * a frame a tick -- the brown dragon's rest 156 of them, its flight 36 -- the map made a new picture every tick (the
 * player, 24. 10.: "why does it draw a thousand pictures? four pictures are enough for the flight"). The view from the
 * eyes draws them all.
 * @param figure the figure of the animation (GetThreeDFigure())
 * @param frame the frame it is at
 */
int ThreeDMapPose(std::string_view figure, int frame)
{
	const ThreeDVehicleObject *fig = GetThreeDFigure(figure);
	if (fig == nullptr || fig->frames <= MAP_POSES || frame <= 0) return frame;
	const int frames = fig->frames;
	frame = std::min(frame, frames - 1);
	if (!fig->loop) {
		const int step = (frame * (MAP_POSES - 1) + (frames - 1) / 2) / (frames - 1);
		return step * (frames - 1) / (MAP_POSES - 1);
	}
	return frame * MAP_POSES / frames * frames / MAP_POSES;
}

bool AddThreeDFigureToViewport(std::string_view name, float scale, int x, int y, int z, float facing, ZoomLevel zoom, TileIndex tile, uint32_t phase, int frame)
{
	int height = 0, up = 0;
	const ThreeDModelPicture *pic = GetThreeDFigurePicture(name, scale, facing, zoom, tile, phase, &height, frame, NAN, z, x, y, &up);
	if (pic == nullptr) return false;
	AddTurnedVehiclePicture(pic, x, y, z, x - 1, y - 1, z, x + 1, y + 1, z + height, up);
	return true;
}

/**
 * The picture of a figure of the turned map at the angle of the view (AddThreeDFigureToViewport(); a figure on a
 * building, drawn among its triangles): its frame of the game's tick when it moves, its tile then drawn again as the
 * frame changes.
 * @param[out] height how tall it is, units of height
 * @param frame the frame to draw (one walking: by the way walked, three_d_walkers.h); -1 by the game's tick
 * @param yaw the angle of the view it is seen from (the preview beside an industry's window), NaN the map's
 * @param z the height it stands at, units, INT_MIN not known: its tile drawn again as high as a building may be
 * @param x,y where it stands, world units, when z is known
 * @param[out] up nullptr: the picture at the zoom asked for; else made coarser when it would be too big
 *                (FigurePictureZoom()), how many times coarser -- for the map, which draws its pixels doubled
 * @return the picture, or nullptr when the figure is not there
 */
/**
 * A vehicle standing as a figure of a building ("vozidlo:<record>|<set>", the new stadium's event: the fire Tatra on
 * the pitch): the vehicle of its package by its record's name, in the state of the set so named, standing empty -- none
 * of its crew (the player, 18. 10.: "on the pitch stands an empty Tatra, without a driver"). "+<file>=<node>" its state
 * by its add-ons instead: the first of its states that shows that node of each add-on so named and nothing of the others
 * (the wheelbarrow of the Special by the cowshed: its frame, empty); "^<degrees>" its front raised so much, leaning.
 * @param[out] ev its object and state; its place, size and facing are the caller's
 * @return false: not such a name, or its package is not there
 */
bool GetThreeDStandingVehicle(std::string_view name, ThreeDEyeVehicle &ev)
{
	if (!name.starts_with("vozidlo:")) return false;
	name.remove_prefix(std::string_view("vozidlo:").size());
	size_t bar = name.find('|');
	const ThreeDVehicleObject *obj = GetThreeDVehicleObjectNamed(name.substr(0, bar));
	if (obj == nullptr) return false;
	/* Its sets, one of each layer it shows (the cab, the grille, the body), made one state as the GRF's layers are. */
	std::vector<int> steps;
	ThreeDParts wanted, addon_parts;
	bool by_addons = false;
	ev.pitch = 0;
	while (bar != std::string_view::npos) {
		const size_t next = name.find('|', bar + 1);
		const std::string_view token = name.substr(bar + 1, next == std::string_view::npos ? std::string_view::npos : next - bar - 1);
		if (token.starts_with('+')) {
			/* An add-on's node: its part, "<file>/<node>" (ThreeDVehicleObject::part_names). */
			const size_t eq = token.find('=');
			if (eq != std::string_view::npos) {
				const std::string part = fmt::format("{}/{}", token.substr(1, eq - 1), token.substr(eq + 1));
				auto it = std::ranges::find(obj->part_names, part);
				if (it != obj->part_names.end()) wanted.Set(static_cast<int>(it - obj->part_names.begin()));
				by_addons = true;
			}
		} else if (token.starts_with('^')) {
			ev.pitch = static_cast<float>(std::atof(std::string(token.substr(1)).c_str()));
		} else {
			const int step = obj->StateNamed(token);
			if (step > 0) steps.push_back(step);
		}
		bar = next;
	}
	int state = obj->ComposeState(steps);
	if (by_addons) {
		/* The add-ons' parts: all but the crew's. */
		for (size_t k = 0; k < obj->part_names.size(); k++) {
			if (!obj->part_names[k].empty() && !obj->part_names[k].starts_with("posadk")) addon_parts.Set(static_cast<int>(k));
		}
		for (size_t k = 0; k < obj->states.size(); k++) {
			const ThreeDParts &p = obj->states[k].parts;
			if ((p.bits[0] & addon_parts.bits[0]) == wanted.bits[0] && (p.bits[1] & addon_parts.bits[1]) == wanted.bits[1]) {
				state = static_cast<int>(k + 1);
				break;
			}
		}
	}
	ev.obj = obj;
	ev.state = obj->EmptyStateOf(state);
	if (_debug_misc_level >= 2) {
		const ThreeDParts parts = obj->PartsOf(ev.state);
		Debug(misc, 2, "3D standing vehicle {}: {} sets found, state {}, parts {:x}.{:x}", obj->name, steps.size(), ev.state, parts.bits[1], parts.bits[0]);
	}
	return true;
}

const ThreeDModelPicture *GetThreeDFigurePicture(std::string_view name, float scale, float facing, ZoomLevel zoom, TileIndex tile, uint32_t phase, int *height, int frame, float yaw, int z, int x, int y, int *up)
{
	if (up != nullptr) *up = 0;
	if (std::isnan(yaw)) yaw = GetViewYaw();
	if (name.starts_with("vozidlo:")) {
		/* A vehicle standing as a figure: at its true size against the building, empty, still. */
		ThreeDEyeVehicle ev;
		if (!GetThreeDStandingVehicle(name, ev)) return nullptr;
		const bool for_eye = ViewportDrawingEye();
		const ThreeDParts parts = ev.obj->PartsOf(ev.state);
		const ThreeDVehicleShape *shape = for_eye ? ev.obj->TryGetShape(0, parts) : ev.obj->GetShape(0, parts);
		if (shape == nullptr) return nullptr;
		if (up != nullptr) zoom = FigurePictureZoom(*shape, scale, zoom, *up);
		const float world_az = facing - 135.0f;
		_object_picture_may_wait = for_eye;
		const ThreeDModelPicture *pic = ObjectPicture(ev.obj, scale, zoom, world_az + yaw, world_az, false, ev.state, 0, -1, 1.0f, 0, static_cast<int>(std::lround(ev.pitch)));
		_object_picture_may_wait = false;
		if (pic != nullptr && height != nullptr) *height = std::max(1, static_cast<int>(std::lround(shape->z1 * scale * 8.0 / 3.0)));
		return pic;
	}
	const ThreeDVehicleObject *fig = GetThreeDFigure(name);
	if (fig == nullptr) return nullptr;
	const bool by_tick = frame < 0;
	if (by_tick) frame = fig->FrameAt(TimerGameTick::counter, phase);
	/* For the view from the eyes nothing is read now: its frame draws without the figure till it is read (TryGetShape()).
	 * Nor for a witch flying (three_d_witches.h): drawn again every tick, she comes as her frame is read and her picture
	 * made on the thread (the colleague's Marisa has 41 000 triangles a frame). */
	const bool for_eye = ViewportDrawingEye();
	const bool later = for_eye || fig->frame_lights;
	const ThreeDVehicleShape *shape = later ? fig->TryGetShape(frame) : fig->GetShape(frame);
	if (shape == nullptr) return nullptr;
	if (up != nullptr) zoom = FigurePictureZoom(*shape, scale, zoom, *up);
	/* A thing that turns: where in its turn it is, by the game's tick (its frames change after so many turns). */
	const int spin_step = fig->spin_ticks > 0 ? static_cast<int>((TimerGameTick::counter % fig->spin_ticks) * SPIN_STEPS / fig->spin_ticks) : 0;
	/* The angle as the vehicles count it: the front to the right of the picture at 0, along +x of the world at 135. */
	float world_az = facing - 135.0f;
	_object_picture_may_wait = later;
	const ThreeDModelPicture *pic = ObjectPicture(fig, scale, zoom, world_az + yaw, world_az, false, 0, frame, -1, 1.0f, spin_step);
	_object_picture_may_wait = false;
	if (pic == nullptr) return nullptr;
	const int tall = std::max(1, static_cast<int>(std::lround(shape->z1 * scale * 8.0 / 3.0)));
	if (height != nullptr) *height = tall;
	if (by_tick && (fig->frames > 1 || fig->spin_ticks > 0) && tile != INVALID_TILE) {
		/* Its tile is drawn again as the picture changes: a frame, or a step of a turn. */
		const int ticks = fig->spin_ticks > 0 ? std::max(1, fig->spin_ticks / SPIN_STEPS) : fig->frame_ticks;
		uint64_t step = TimerGameTick::counter / ticks;
		bool known = false;
		/* Where it stands, as far as it reaches about (drawn bigger on the map than it is, its scale), a little below
		 * its feet and above its head: where it reaches over as it moves. */
		const double rx = std::max(std::abs(shape->x0), std::abs(shape->x1)), ry = std::max(std::abs(shape->y0), std::abs(shape->y1));
		const double reach = std::hypot(rx, ry) * scale * OBJECT_UNITS_PER_METRE + 2;
		MovingFigure f{tile, ticks, step, INT_MIN, INT_MIN, x - reach, y - reach, x + reach, y + reach};
		if (z != INT_MIN) {
			f.z0 = z - TILE_HEIGHT;
			f.z1 = z + tall + TILE_HEIGHT;
		}
		for (MovingFigure &m : _moving_figures) {
			if (m.tile != tile) continue;
			if (m.ticks != ticks) continue;
			m.step = step;
			/* Two figures on one tile: the box of both, or the tile as high as a building when one is not known. */
			if (m.z0 == INT_MIN || f.z0 == INT_MIN) {
				m.z0 = INT_MIN;
			} else {
				m.z0 = std::min(m.z0, f.z0);
				m.z1 = std::max(m.z1, f.z1);
				m.x0 = std::min(m.x0, f.x0);
				m.y0 = std::min(m.y0, f.y0);
				m.x1 = std::max(m.x1, f.x1);
				m.y1 = std::max(m.y1, f.y1);
			}
			known = true;
		}
		if (!known) _moving_figures.push_back(f);
	}
	return pic;
}

/**
 * The picture of a plant standing on a building or a field ("rostliny", three_d_objects.h) at the angle of the view:
 * a figure that never moves, its angles to the view and to the light in steps of PLANT_STEP degrees, so that the
 * hundreds of plants of a field share a few pictures (a plant looks about the same from a few degrees aside).
 * @param stretch its height against its width
 * @param[out] height how tall it is, units of height
 * @param yaw the angle of the view it is seen from, NaN the map's
 */
const ThreeDModelPicture *GetThreeDPlantPicture(std::string_view name, float scale, float stretch, float facing, ZoomLevel zoom, int *height, float yaw)
{
	static constexpr float PLANT_STEP = 10.0f;
	const ThreeDVehicleObject *plant = GetThreeDFigure(name);
	if (plant == nullptr) return nullptr;
	const bool for_eye = ViewportDrawingEye();
	const ThreeDVehicleShape *shape = for_eye ? plant->TryGetShape(0) : plant->GetShape(0);
	if (shape == nullptr) return nullptr;
	if (std::isnan(yaw)) yaw = GetViewYaw();
	float world_az = std::round((facing - 135.0f) / PLANT_STEP) * PLANT_STEP;
	float cam_az = std::round((facing - 135.0f + yaw) / PLANT_STEP) * PLANT_STEP;
	/* Sizes in steps of a twentieth: the plants of a list are each a little other. */
	scale = std::max(0.05f, std::round(scale * 20.0f) / 20.0f);
	stretch = std::max(0.05f, std::round(stretch * 20.0f) / 20.0f);
	_object_picture_may_wait = for_eye;
	const ThreeDModelPicture *pic = ObjectPicture(plant, scale, zoom, cam_az, world_az, false, 0, 0, -1, stretch);
	_object_picture_may_wait = false;
	if (pic == nullptr) return nullptr;
	if (height != nullptr) *height = std::max(1, static_cast<int>(std::lround(shape->z1 * scale * stretch * 8.0 / 3.0)));
	return pic;
}

/**
 * A thing in the hands of the walker of the view from the first person ("predmet" of the indices, the colleague's
 * hammer, hra/3d/PROSBA-PREDMETY-KLADIVO-TRUHLA.md; his bow, crossbow and swords, hra/3d/LUK-SIP-PRO-HRU.md; the
 * player: "the game swings it, we need only the models"): drawn over the picture from its eyes, in perspective and
 * lit from its normals, as it is held (ThreeDHold):
 *  - swung, a hammer or a sword: at the bottom right as a right hand holds it by its grip ("uchyt"), the handle along
 *    its +z leaning forward, a striking face or an edge ahead; a swing turns it about the line through the hand
 *    across the view: back and up as it is raised, forward and down as it strikes;
 *  - a bow: upright in the left hand a little left of the middle, its string to the eye (-x), the arrow on the string
 *    flying along +x ahead, the top of it leaning a little to the right;
 *  - a crossbow: at the right shoulder below the eye, along the look, its bolt in the groove.
 * The arrow or the bolt on it (loaded) is drawn with it, its origin at the nock. Its triangles on the picture, to be
 * drawn (DrawThreeDHeldThing(), or by the graphics card) where the thing is nearer, its depth kept there so that the
 * night's lights behind it stay behind it -- 1 over the distance ahead in world units.
 * @param held what is held and how
 * @param focal pixels of the picture across, for a unit across at a unit ahead
 * @param[out] out its triangles, added
 */
void ThreeDHeldThingTriangles(const ThreeDHeldThing &held, int width, int height, double focal, std::vector<ThreeDHeldTriangle> &out)
{
	if (held.raise <= 0.0f || held.thing.empty() || width <= 0 || height <= 0) return;
	const ThreeDVehicleObject *obj = GetThreeDFigure(held.thing);
	if (obj == nullptr) return;
	const ThreeDVehicleShape *shape = obj->GetShape(held.frame);
	if (shape == nullptr || shape->tri.empty()) return;

	/* The eye: x to the right, y up, z ahead, metres. A point of the thing's frame goes to the eye along its axes,
	 * ax, ay and az, from where its grip is held, at the hand. */
	struct V { float x, y, z; };
	auto unit = [](V v) {
		const float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
		return V{v.x / l, v.y / l, v.z / l};
	};
	V ax{1, 0, 0}, ay{0, 1, 0}, az{0, 0, 1}, hand{0, 0, 0};
	const float lowered = 1.0f - held.raise; // lower while it is taken out or put away
	float swing_c = 1, swing_s = 0;          // a swing, about the hand's line across
	switch (held.hold) {
		case ThreeDHold::Swung: {
			/* At rest the handle leans forward and a little to the left, a striking face ahead; the hand at the
			 * bottom right. Striking, the arms go forward and up with it, so that the head comes down before the eyes. */
			az = unit({-0.12f, 0.62f, 0.75f});
			ax = V{0.1f, 0.0f, 1.0f};
			const float d = ax.x * az.x + ax.y * az.y + ax.z * az.z;
			ax = unit({ax.x - d * az.x, ax.y - d * az.y, ax.z - d * az.z});
			ay = V{az.y * ax.z - az.z * ax.y, az.z * ax.x - az.x * ax.z, az.x * ax.y - az.y * ax.x};
			const float reach = std::clamp(held.swing / 70.0f, 0.0f, 1.0f);
			hand = V{0.40f - 0.12f * reach, -0.50f - lowered * 0.7f + 0.22f * reach, 0.52f + 0.30f * reach};
			const float a = held.swing * static_cast<float>(M_PI) / 180.0f;
			swing_c = std::cos(a);
			swing_s = std::sin(a);
			break;
		}
		case ThreeDHold::Bow: {
			/* Its +x ahead, +z up leaning right by the cant, +y (the side the arrow passes the grip) to the left; held
			 * at the left, brought before the eye as it is drawn, so that the arrow points at the dot in the middle. */
			const float d = std::clamp(held.drawn, 0.0f, 1.0f);
			const float cant = (25.0f - 15.0f * d) * static_cast<float>(M_PI) / 180.0f;
			ax = V{0.0f, 0.0f, 1.0f};
			az = V{std::sin(cant), std::cos(cant), 0.0f};
			ay = V{-std::cos(cant), std::sin(cant), 0.0f};
			hand = V{-0.24f + 0.17f * d, -0.20f + 0.12f * d - lowered * 0.8f, 0.70f + 0.05f * d - 0.05f * held.kick};
			break;
		}
		case ThreeDHold::Crossbow: {
			/* Its +x ahead along the look, +z up, +y to the left; the right hand at its grip below and right of the eye. */
			ax = V{0.0f, 0.0f, 1.0f};
			az = V{0.0f, 1.0f, 0.0f};
			ay = V{-1.0f, 0.0f, 0.0f};
			hand = V{0.12f, -0.22f - lowered * 0.7f + 0.02f * held.kick, 0.25f - 0.06f * held.kick};
			break;
		}
		case ThreeDHold::Shoulder: {
			/* As the crossbow, only further down, right and ahead: the tube is wide and long, and at the shoulder it
			 * would fill the picture. */
			ax = V{0.0f, 0.0f, 1.0f};
			az = V{0.0f, 1.0f, 0.0f};
			ay = V{-1.0f, 0.0f, 0.0f};
			hand = V{0.24f, -0.36f - lowered * 0.7f + 0.03f * held.kick, 0.45f - 0.08f * held.kick};
			break;
		}
		case ThreeDHold::Left: {
			/* A light: upright, leaning a little in towards the middle, its flame (where it is held from, its "svetlo")
			 * before the eye's lower left. */
			az = unit({0.12f, 0.98f, 0.10f});
			ax = V{0.0f, 0.0f, 1.0f};
			const float d = ax.x * az.x + ax.y * az.y + ax.z * az.z;
			ax = unit({ax.x - d * az.x, ax.y - d * az.y, ax.z - d * az.z});
			ay = V{az.y * ax.z - az.z * ax.y, az.z * ax.x - az.x * ax.z, az.x * ax.y - az.y * ax.x};
			hand = V{-0.40f, -0.20f - lowered * 0.9f, 0.80f};
			break;
		}
	}
	/* A point of the thing (or a direction, without the grip and the hand), offset by where the loaded one sits,
	 * turned about its handle. */
	const float tw = held.hold == ThreeDHold::Swung ? held.twist * static_cast<float>(M_PI) / 180.0f : 0.0f, twc = std::cos(tw), tws = std::sin(tw);
	auto place = [&](float px, float py, float pz, bool point, const float *offset) {
		if (offset != nullptr) {
			px += offset[0];
			py += offset[1];
			pz += offset[2];
		}
		if (point) {
			px -= held.grip[0];
			py -= held.grip[1];
			pz -= held.grip[2];
		}
		const float qx = px * twc - py * tws, qy = px * tws + py * twc;
		px = qx;
		py = qy;
		V v{ax.x * px + ay.x * py + az.x * pz, ax.y * px + ay.y * py + az.y * pz, ax.z * px + ay.z * py + az.z * pz};
		v = V{v.x, v.y * swing_c - v.z * swing_s, v.y * swing_s + v.z * swing_c};
		if (point) v = V{v.x + hand.x, v.y + hand.y, v.z + hand.z};
		return v;
	};

	/* The light from above, a little left and behind the eye. */
	const V sun = unit({-0.45f, 0.75f, -0.5f});
	const float cx = width / 2.0f, cy = height / 2.0f, f = static_cast<float>(focal);
	auto add = [&](const ThreeDVehicleObject *o, const ThreeDVehicleShape *sh, const float *offset) {
		const size_t n = sh->pos.size() / 3;
		std::vector<float> sx(n), sy(n), iz(n), light(n, 1.0f);
		const bool lit = sh->nrm.size() == sh->pos.size();
		for (size_t i = 0; i < n; i++) {
			const V p = place(sh->pos[i * 3], sh->pos[i * 3 + 1], sh->pos[i * 3 + 2], true, offset);
			iz[i] = p.z > 0.02f ? 1.0f / p.z : -1.0f;
			sx[i] = cx + f * p.x * std::max(iz[i], 0.0f);
			sy[i] = cy - f * p.y * std::max(iz[i], 0.0f);
			if (lit) {
				const V q = place(sh->nrm[i * 3], sh->nrm[i * 3 + 1], sh->nrm[i * 3 + 2], false, nullptr);
				light[i] = 0.45f + 0.55f * std::max(0.0f, std::abs(q.x * sun.x + q.y * sun.y + q.z * sun.z));
			}
		}
		for (size_t t = 0; t + 2 < sh->tri.size(); t += 3) {
			const uint32_t c[3] = {sh->tri[t], sh->tri[t + 1], sh->tri[t + 2]};
			if (iz[c[0]] <= 0 || iz[c[1]] <= 0 || iz[c[2]] <= 0) continue; // behind the eye
			const int ti = t / 3 < sh->tri_tex.size() ? sh->tri_tex[t / 3] : 0;
			const ThreeDObjectTexture *tex = o->GetTexture(0, ti);
			if (tex == nullptr) continue;
			ThreeDHeldTriangle &h = out.emplace_back();
			h.tex = tex;
			/* A material's own light (a flame): unshaded, brighter by it. */
			const uint32_t glow = sh->Glow(static_cast<size_t>(ti));
			for (int k = 0; k < 3; k++) {
				h.sx[k] = sx[c[k]];
				h.sy[k] = sy[c[k]];
				h.iz[k] = iz[c[k]];
				h.u[k] = sh->uv[c[k] * 2];
				h.v[k] = sh->uv[c[k] * 2 + 1];
				h.light[k] = glow != 0 ? 1.0f + std::max({Colour(glow).r, Colour(glow).g, Colour(glow).b}) / 255.0f : light[c[k]];
			}
			h.glow = glow != 0;
		}
	};
	add(obj, shape, nullptr);
	/* The arrow on the string, the bolt in the groove. */
	if (!held.loaded.empty()) {
		const ThreeDVehicleObject *missile = GetThreeDFigure(held.loaded);
		const ThreeDVehicleShape *ms = missile != nullptr ? missile->GetShape(0) : nullptr;
		if (ms != nullptr && !ms->tri.empty()) add(missile, ms, held.nock);
	}
}

/**
 * Draw the thing in the walker's hand (ThreeDHeldThingTriangles()) into the view from the eyes, over what is farther:
 * each pixel its texel in perspective, lit by its corners' light straight across the picture.
 * @return whether anything of it was drawn
 */
bool DrawThreeDHeldThing(const ThreeDHeldThing &held, uint32_t *dst, float *zb, int width, int height, double focal)
{
	std::vector<ThreeDHeldTriangle> tris;
	ThreeDHeldThingTriangles(held, width, height, focal, tris);
	const float to_world = 1.0f / static_cast<float>(OBJECT_UNITS_PER_METRE);
	bool drawn = false;
	for (const ThreeDHeldTriangle &h : tris) {
		const ThreeDObjectTexture *tex = h.tex;
		const float ax0 = h.sx[0], ay0 = h.sy[0], bx = h.sx[1], by = h.sy[1], qx = h.sx[2], qy = h.sy[2];
		const float area = (bx - ax0) * (qy - ay0) - (by - ay0) * (qx - ax0);
		if (std::abs(area) < 1e-6f) continue;
		const int x0 = std::max(0, static_cast<int>(std::floor(std::min({ax0, bx, qx})))), x1 = std::min(width - 1, static_cast<int>(std::ceil(std::max({ax0, bx, qx}))));
		const int y0 = std::max(0, static_cast<int>(std::floor(std::min({ay0, by, qy})))), y1 = std::min(height - 1, static_cast<int>(std::ceil(std::max({ay0, by, qy}))));
		const float inv = 1.0f / area;
		for (int py = y0; py <= y1; py++) {
			const float ry = py + 0.5f;
			for (int px = x0; px <= x1; px++) {
				const float rx = px + 0.5f;
				const float w0 = ((qx - bx) * (ry - by) - (qy - by) * (rx - bx)) * inv;
				const float w1 = ((ax0 - qx) * (ry - qy) - (ay0 - qy) * (rx - qx)) * inv;
				const float w2 = 1.0f - w0 - w1;
				if (w0 < 0 || w1 < 0 || w2 < 0) continue;
				/* Perspective: 1/z goes evenly across the picture, the texture's place over z with it. */
				const float z_inv = w0 * h.iz[0] + w1 * h.iz[1] + w2 * h.iz[2];
				const float near = z_inv * to_world;
				const size_t at = static_cast<size_t>(py) * width + px;
				if (near <= zb[at]) continue;
				const float u = (w0 * h.u[0] * h.iz[0] + w1 * h.u[1] * h.iz[1] + w2 * h.u[2] * h.iz[2]) / z_inv;
				const float v = (w0 * h.v[0] * h.iz[0] + w1 * h.v[1] * h.iz[1] + w2 * h.v[2] * h.iz[2]) / z_inv;
				const float uu = u - std::floor(u), vv = v - std::floor(v);
				const int tx = std::min(tex->w - 1, static_cast<int>(uu * tex->w)), ty = std::min(tex->h - 1, static_cast<int>(vv * tex->h));
				const uint32_t texel = tex->px[static_cast<size_t>(ty) * tex->w + tx];
				if (texel == 0) continue; // a hole in the texture
				const float k = h.glow ? h.light[0] : w0 * h.light[0] + w1 * h.light[1] + w2 * h.light[2];
				const Colour c(texel);
				dst[at] = Colour(static_cast<uint8_t>(std::min(255.0f, c.r * k)), static_cast<uint8_t>(std::min(255.0f, c.g * k)), static_cast<uint8_t>(std::min(255.0f, c.b * k))).data;
				zb[at] = near;
				drawn = true;
			}
		}
	}
	return drawn;
}

/**
 * The rig ('test3d pamet'): how much memory the pictures made of the 3D objects take -- those of the turned view
 * (ObjectPicture()), the textures painted for the view from the eyes, and the sprites of the eight directions in the
 * sprite cache (the view facing north, the lists, the depots), of all the sprite cache.
 */
std::string DescribeThreeDPictureMemory()
{
	auto mb = [](size_t b) { return b / (1024.0 * 1024.0); };
	size_t turned = 0;
	for (const auto &[key, e] : _object_pictures) turned += e.picture.pixels.capacity() * sizeof(uint32_t) + e.picture.lit.capacity() * sizeof(uint32_t);
	size_t painted = 0;
	{
		std::lock_guard lock(_eye_painted_mutex);
		for (const auto &[key, t] : _eye_painted) painted += t.px.capacity() * sizeof(uint32_t);
	}
	extern size_t GetThreeDObjectSpriteBytes(size_t &count);
	extern size_t GetSpriteCacheBytesUsed();
	extern size_t GetTurnedObjectPictureBytes(size_t &count);
	size_t sprite_count = 0, house_count = 0;
	const size_t sprites = GetThreeDObjectSpriteBytes(sprite_count);
	const size_t houses = GetTurnedObjectPictureBytes(house_count);
	return fmt::format("obrazky: domy otoceneho pohledu {:.1f} MB ({}), otoceny pohled {:.1f} MB ({} obrazku), lakovane textury pohledu z oci {:.1f} MB ({}), sprity 8 smeru v cache spritu {:.1f} MB ({} nactenych z {} pozadanych), cache spritu celkem {:.1f} MB z {} MB",
			mb(houses), house_count, mb(turned), _object_pictures.size(), mb(painted), _eye_painted.size(), mb(sprites), sprite_count, _object_sprites.size(), mb(GetSpriteCacheBytesUsed()), _settings_client.gui.sprite_cache_size_mb);
}

/**
 * The rig ('test3d kresli'): how long drawing the object of the first vehicle drawn from one takes at each zoom of the
 * map (RasterObject() at 30 degrees, its shape and textures made by the first drawing, which is not counted).
 */
std::string RigTimeThreeDObjectRaster(int times)
{
	const Vehicle *found = nullptr;
	const ThreeDVehicleObject *obj = nullptr;
	for (const Vehicle *v : Vehicle::Iterate()) {
		int piece = 0;
		obj = v->IsPrimaryVehicle() ? ThreeDVehicleObjectOf(v, &piece) : nullptr;
		if (obj != nullptr) {
			found = v;
			break;
		}
	}
	if (found == nullptr) return "no vehicle drawn from a 3D object";
	const int state = StateStep(obj, found->engine_type, found, EngineImageType::OnMap);
	std::string out = obj->name;
	times = std::clamp(times, 1, 1000);
	for (ZoomLevel zoom : {ZoomLevel::In16x, ZoomLevel::In8x, ZoomLevel::In4x, ZoomLevel::In2x, ZoomLevel::Normal}) {
		const float pxm = PXM_16X / (1 << to_underlying(zoom));
		ObjectRaster r;
		RasterObject(*obj, ObjectScale(obj), 30.0f, 30.0f, 1.0f, pxm, FadeAmount(obj, state), r, 0, RustTime(obj, state), 0, PaletteColour(obj, state), 1.0f, 0.0f, 0.0f, 0.0f, 0, state);
		const auto start = std::chrono::steady_clock::now();
		for (int i = 0; i < times; i++) {
			RasterObject(*obj, ObjectScale(obj), 30.0f, 30.0f, 1.0f, pxm, FadeAmount(obj, state), r, 0, RustTime(obj, state), 0, PaletteColour(obj, state), 1.0f, 0.0f, 0.0f, 0.0f, 0, state);
		}
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / times;
		out += fmt::format(", zoom {} ({}x{} px) {:.2f} ms", to_underlying(zoom), r.width, r.height, ms);
		/* The picture too, over grey (kresli_<object>_<zoom>.ppm where the game runs): to look at what each zoom draws. */
		if (auto f = FileHandle::Open(fmt::format("kresli_{}_{}.ppm", obj->name, to_underlying(zoom)), "wb"); f.has_value() && r.width > 0) {
			const std::string head = fmt::format("P6\n{} {}\n255\n", r.width, r.height);
			fwrite(head.data(), 1, head.size(), *f);
			std::vector<uint8_t> rgb;
			rgb.reserve(r.px.size() * 3);
			for (const Colour &c : r.px) {
				for (uint8_t v : {c.r, c.g, c.b}) rgb.push_back(static_cast<uint8_t>((v * c.a + 128 * (255 - c.a)) / 255));
			}
			fwrite(rgb.data(), 1, rgb.size(), *f);
		}
	}
	return out;
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

/**
 * The rig: a vehicle's object as a picture of its own, big, written as a PPM over grey ('test3d kresli').
 * @param az the angle the camera sees it at (ViewAzimuth()), degrees
 * @param pxm pixels to a metre
 * @param state its state (StateStep()): 0 as it is; -1 the states of the layers the rig set ('test3d stav <n> <n> ...')
 * @param lights its lights on (LIGHTS_FRONT, LIGHTS_BACK)
 * @param walked_m the way gone, metres: its crew that walks at the frames of it (WalkCode())
 */
bool WriteThreeDVehicleObjectPPM(EngineID engine, float az, float pxm, int state, int lights, const std::string &path, double walked_m)
{
	int piece = 0;
	const ThreeDVehicleObject *obj = GetThreeDVehicleObject(engine, &piece);
	if (obj == nullptr) return false;
	if (state < 0) {
		state = StateStep(obj, engine, nullptr, EngineImageType::Purchase);
	} else if (const uint64_t load = LoadCode(obj, {state}, nullptr, engine); load != 0) {
		/* A state of a load piece with its bed full, as the purchase list has it. */
		state = obj->ComposeState({state}, 0, load);
	}
	const uint64_t walk = WalkCode(obj, state, walked_m * UNITS_PER_METRE, 1.0f);
	ObjectRaster r;
	if (!RasterObject(*obj, 1.0f, az, az, 1.0f, pxm, FadeAmount(obj, state), r, 0, RustTime(obj, state), 0, PaletteColour(obj, state), 1.0f, 0, 0, 0, lights, state, walk)) return false;
	auto f = FileHandle::Open(path, "wb");
	if (!f.has_value()) return false;
	std::string head = fmt::format("P6\n{} {}\n255\n", r.width, r.height);
	fwrite(head.data(), 1, head.size(), *f);
	std::vector<uint8_t> row;
	for (size_t i = 0; i < r.px.size(); i++) {
		const Colour c = r.px[i];
		const int a = c.a;
		row.push_back(static_cast<uint8_t>((c.r * a + 96 * (255 - a)) / 255));
		row.push_back(static_cast<uint8_t>((c.g * a + 96 * (255 - a)) / 255));
		row.push_back(static_cast<uint8_t>((c.b * a + 104 * (255 - a)) / 255));
	}
	fwrite(row.data(), 1, row.size(), *f);
	return true;
}
