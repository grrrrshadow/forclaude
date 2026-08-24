/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file train_cmd.cpp Handling of trains. */

#include "stdafx.h"
#include "error.h"
#include "articulated_vehicles.h"
#include "command_func.h"
#include "core/backup_type.hpp"
#include "economy_base.h"
#include "error_func.h"
#include "pathfinder/yapf/yapf.hpp"
#include "news_func.h"
#include "company_func.h"
#include "newgrf_sound.h"
#include "newgrf_text.h"
#include "strings_func.h"
#include "viewport_func.h"
#include "vehicle_func.h"
#include "sound_func.h"
#include "ai/ai.hpp"
#include "game/game.hpp"
#include "newgrf_station.h"
#include "effectvehicle_func.h"
#include "effectvehicle_base.h"
#include "network/network.h"
#include "core/random_func.hpp"
#include "company_base.h"
#include "newgrf.h"
#include "order_backup.h"
#include "zoom_func.h"
#include "newgrf_debug.h"
#include "framerate_type.h"
#include "train_cmd.h"
#include "vehicle_cmd.h"
#include "misc_cmd.h"
#include "timer/timer_game_calendar.h"
#include "timer/timer_game_economy.h"
#include "timetable.h"

#include "table/strings.h"
#include "table/train_sprites.h"

#include "safeguards.h"

static Track ChooseTrainTrack(Train *v, TileIndex tile, DiagDirection enterdir, TrackBits tracks, bool force_res, bool *got_reservation, bool mark_stuck);
static bool TrainCheckIfLineEnds(Train *v, bool reverse = true);
bool TrainController(Train *v, Vehicle *nomove, bool reverse = true); // Also used in vehicle_sl.cpp.
static TileIndex TrainApproachingCrossingTile(const Train *v);
static void CheckIfTrainNeedsService(Train *v);
static void CheckNextTrainTile(Train *v);

/** Initial x subtile coordinate of rail vehicles for each direction. */
static constexpr DiagDirectionIndexArray<uint8_t> _vehicle_initial_x_fract{10, 8, 4,  8};
/** Initial y subtile coordinate of rail vehicles for each direction. */
static constexpr DiagDirectionIndexArray<uint8_t> _vehicle_initial_y_fract{ 8, 4, 8, 10};

/** @copydoc IsValidImageIndex */
template <>
bool IsValidImageIndex<VehicleType::Train>(uint8_t image_index)
{
	return image_index < lengthof(_engine_sprite_base);
}


/**
 * Return the cargo weight multiplier to use for a rail vehicle
 * @param cargo Cargo type to get multiplier for
 * @return Cargo weight multiplier
 */
uint8_t FreightWagonMult(CargoType cargo)
{
	if (!CargoSpec::Get(cargo)->is_freight) return 1;
	return _settings_game.vehicle.freight_trains;
}

/** Checks if lengths of all rail vehicles are valid. If not, shows an error message. */
void CheckTrainsLengths()
{
	bool first = true;

	for (const Train *v : Train::Iterate()) {
		if (v->First() == v && !v->vehstatus.Test(VehState::Crashed)) {
			for (const Train *u = v->GetMovingFront(), *w = v->GetMovingNext(); w != nullptr; u = w, w = w->GetMovingNext()) {
				if (u->track != Track::Depot) {
					if ((w->track != Track::Depot &&
							std::max(abs(u->x_pos - w->x_pos), abs(u->y_pos - w->y_pos)) != u->CalcNextVehicleOffset()) ||
							(w->track == Track::Depot && TicksToLeaveDepot(u) <= 0)) {
						ShowErrorMessage(GetEncodedString(STR_BROKEN_VEHICLE_LENGTH, v->index, v->owner), {}, WarningLevel::Critical);

						if (!_networking && first) {
							first = false;
							Command<Commands::Pause>::Post(PauseMode::Error, true);
						}
						/* Break so we warn only once for each train. */
						break;
					}
				}
			}
		}
	}
}

/**
 * Recalculates the cached stuff of a train. Should be called each time a vehicle is added
 * to/removed from the chain, and when the game is loaded.
 * Note: this needs to be called too for 'wagon chains' (in the depot, without an engine)
 * @param allowed_changes Stuff that is allowed to change.
 */
void Train::ConsistChanged(ConsistChangeFlags allowed_changes)
{
	uint16_t max_speed = UINT16_MAX;

	assert(this->IsFrontEngine() || this->IsFreeWagon());

	const RailVehicleInfo *rvi_v = RailVehInfo(this->engine_type);
	EngineID first_engine = this->IsFrontEngine() ? this->engine_type : EngineID::Invalid();
	this->gcache.cached_total_length = 0;
	this->compatible_railtypes = {};

	bool train_can_tilt = true;
	int16_t min_curve_speed_mod = INT16_MAX;

	for (Train *u = this; u != nullptr; u = u->Next()) {
		const RailVehicleInfo *rvi_u = RailVehInfo(u->engine_type);

		/* Check the this->first cache. */
		assert(u->First() == this);

		/* update the 'first engine' */
		u->gcache.first_engine = this == u ? EngineID::Invalid() : first_engine;
		u->railtypes = rvi_u->railtypes;

		if (u->IsEngine()) first_engine = u->engine_type;

		/* Set user defined data to its default value */
		u->tcache.user_def_data = rvi_u->user_def_data;
		this->InvalidateNewGRFCache();
		u->InvalidateNewGRFCache();
	}

	for (Train *u = this; u != nullptr; u = u->Next()) {
		/* Update user defined data (must be done before other properties) */
		u->tcache.user_def_data = GetVehicleProperty(u, PROP_TRAIN_USER_DATA, u->tcache.user_def_data);
		this->InvalidateNewGRFCache();
		u->InvalidateNewGRFCache();
	}

	for (Train *u = this; u != nullptr; u = u->Next()) {
		const Engine *e_u = u->GetEngine();
		const RailVehicleInfo *rvi_u = &e_u->VehInfo<RailVehicleInfo>();

		if (!e_u->info.misc_flags.Test(EngineMiscFlag::RailTilts)) train_can_tilt = false;
		min_curve_speed_mod = std::min(min_curve_speed_mod, u->GetCurveSpeedModifier());

		/* Cache wagon override sprite group. nullptr is returned if there is none */
		u->tcache.cached_override = GetWagonOverrideSpriteSet(u->engine_type, u->cargo_type, u->gcache.first_engine);

		/* Reset colour map */
		u->colourmap = PAL_NONE;

		/* Update powered-wagon-status and visual effect */
		u->UpdateVisualEffect(true);

		if (rvi_v->pow_wag_power != 0 && rvi_u->railveh_type == RailVehicleType::Wagon &&
				UsesWagonOverride(u) && !HasBit(u->vcache.cached_vis_effect, VE_DISABLE_WAGON_POWER)) {
			/* wagon is powered */
			u->flags.Set(VehicleRailFlag::PoweredWagon); // cache 'powered' status
		} else {
			u->flags.Reset(VehicleRailFlag::PoweredWagon);
		}

		if (!u->IsArticulatedPart()) {
			/* Do not count powered wagons for the compatible railtypes, as wagons always
			   have railtype normal */
			if (rvi_u->power > 0) {
				this->compatible_railtypes.Set(GetAllPoweredRailTypes(u->railtypes));
			}

			/* Some electric engines can be allowed to run on normal rail. It happens to all
			 * existing electric engines when elrails are disabled and then re-enabled */
			if (u->flags.Test(VehicleRailFlag::AllowedOnNormalRail)) {
				u->railtypes.Set(RAILTYPE_RAIL);
				u->compatible_railtypes.Set(RAILTYPE_RAIL);
			}

			/* max speed is the minimum of the speed limits of all vehicles in the consist */
			if ((rvi_u->railveh_type != RailVehicleType::Wagon || _settings_game.vehicle.wagon_speed_limits) && !UsesWagonOverride(u)) {
				uint16_t speed = GetVehicleProperty(u, PROP_TRAIN_SPEED, rvi_u->max_speed);
				if (speed != 0) max_speed = std::min(speed, max_speed);
			}
		}

		uint16_t new_cap = e_u->DetermineCapacity(u);
		if (allowed_changes.Test(ConsistChangeFlag::Capacity)) {
			/* Update vehicle capacity. */
			if (u->cargo_cap > new_cap) u->cargo.Truncate(new_cap);
			u->refit_cap = std::min(new_cap, u->refit_cap);
			u->cargo_cap = new_cap;
		} else {
			/* Verify capacity hasn't changed. */
			if (new_cap != u->cargo_cap) ShowNewGrfVehicleError(u->engine_type, STR_NEWGRF_BROKEN, STR_NEWGRF_BROKEN_CAPACITY, GRFBug::VehCapacity, true);
		}
		u->vcache.cached_cargo_age_period = GetVehicleProperty(u, PROP_TRAIN_CARGO_AGE_PERIOD, e_u->info.cargo_age_period);

		/* check the vehicle length (callback) */
		uint16_t veh_len = CALLBACK_FAILED;
		if (e_u->GetGRF() != nullptr && e_u->GetGRF()->grf_version >= 8) {
			/* Use callback 36 */
			veh_len = GetVehicleProperty(u, PROP_TRAIN_SHORTEN_FACTOR, CALLBACK_FAILED);

			if (veh_len != CALLBACK_FAILED && veh_len >= VEHICLE_LENGTH) {
				ErrorUnknownCallbackResult(e_u->GetGRFID(), CBID_VEHICLE_LENGTH, veh_len);
			}
		} else if (e_u->info.callback_mask.Test(VehicleCallbackMask::Length)) {
			/* Use callback 11 */
			veh_len = GetVehicleCallback(CBID_VEHICLE_LENGTH, 0, 0, u->engine_type, u);
		}
		if (veh_len == CALLBACK_FAILED) veh_len = rvi_u->shorten_factor;
		veh_len = VEHICLE_LENGTH - Clamp(veh_len, 0, VEHICLE_LENGTH - 1);

		if (allowed_changes.Test(ConsistChangeFlag::Length)) {
			/* Update vehicle length. */
			u->gcache.cached_veh_length = veh_len;
		} else {
			/* Verify length hasn't changed. */
			if (veh_len != u->gcache.cached_veh_length) VehicleLengthChanged(u);
		}

		this->gcache.cached_total_length += u->gcache.cached_veh_length;
		this->InvalidateNewGRFCache();
		u->InvalidateNewGRFCache();
	}

	/* store consist weight/max speed in cache */
	this->vcache.cached_max_speed = max_speed;
	this->tcache.cached_tilt = train_can_tilt;
	this->tcache.cached_curve_speed_mod = min_curve_speed_mod;
	this->tcache.cached_max_curve_speed = this->GetCurveSpeedLimit();

	/* recalculate cached weights and power too (we do this *after* the rest, so it is known which wagons are powered and need extra weight added) */
	this->CargoChanged();

	if (this->IsFrontEngine()) {
		this->UpdateAcceleration();
		SetWindowDirty(WindowClass::VehicleDetails, this->index);
		InvalidateWindowData(WindowClass::VehicleRefit, this->index, VIWD_CONSIST_CHANGED);
		InvalidateWindowData(WindowClass::VehicleOrders, this->index, VIWD_CONSIST_CHANGED);
		InvalidateNewGRFInspectWindow(GrfSpecFeature::Trains, this->index);

		/* If the consist is changed while in a depot, the vehicle view window must be invalidated to update the availability of refitting. */
		InvalidateWindowData(WindowClass::VehicleView, this->index, VIWD_CONSIST_CHANGED);
	}
}

/**
 * Get the stop location of (the center) of the front vehicle of a train at
 * a platform of a station.
 * @param station_id     the ID of the station where we're stopping
 * @param tile           the tile where the vehicle currently is
 * @param moving_front the moving front of the train to get the stop location of
 * @param station_ahead  'return' the amount of 1/16th tiles in front of the train
 * @param station_length 'return' the station length in 1/16th tiles
 * @return the location, calculated from the begin of the station to stop at.
 */
int GetTrainStopLocation(StationID station_id, TileIndex tile, const Train *moving_front, int *station_ahead, int *station_length)
{
	const Train *consist = moving_front->First();
	const Station *st = Station::Get(station_id);
	*station_ahead  = st->GetPlatformLength(tile, DirToDiagDir(moving_front->GetMovingDirection())) * TILE_SIZE;
	*station_length = st->GetPlatformLength(tile) * TILE_SIZE;

	/* Default to the middle of the station for stations stops that are not in
	 * the order list like intermediate stations when non-stop is disabled */
	OrderStopLocation osl = OrderStopLocation::Middle;
	if (consist->gcache.cached_total_length >= *station_length) {
		/* The train is longer than the station, make it stop at the far end of the platform */
		osl = OrderStopLocation::FarEnd;
	} else if (consist->current_order.IsType(OT_GOTO_STATION) && consist->current_order.GetDestination() == station_id) {
		osl = consist->current_order.GetStopLocation();

		/* A train that came to couple is not stopping at a place on the
		 * platform, it is stopping against the consist it came for, and that
		 * is as far along as it can get. Stopping where an ordinary arrival
		 * would - in the middle by default - leaves it standing most of a
		 * platform short of its partner, too far away to couple to, so it
		 * finishes loading and leaves again having done nothing. The wagons
		 * themselves are what stops it before the far end. See
		 * FEATURE_DESIGN_COUPLING_TOW.md. */
		if (consist->current_order.ShouldGoToCouple()) osl = OrderStopLocation::FarEnd;
	}

	/* The stop location of the FRONT! of the train */
	int stop;
	switch (osl) {
		default: NOT_REACHED();

		case OrderStopLocation::NearEnd:
			stop = consist->gcache.cached_total_length;
			break;

		case OrderStopLocation::Middle:
			stop = *station_length - (*station_length - consist->gcache.cached_total_length) / 2;
			break;

		case OrderStopLocation::FarEnd:
			stop = *station_length;
			break;
	}

	/* Subtract half the front vehicle length of the train so we get the real
	 * stop location of the train. */
	uint8_t rounding = consist->IsDrivingBackwards() ? 2 : 1;
	return stop - (consist->gcache.cached_veh_length + rounding) / 2;
}


/**
 * Computes train speed limit caused by curves
 * @return imposed speed limit
 */
uint16_t Train::GetCurveSpeedLimit() const
{
	assert(this->First() == this);

	static const int absolute_max_speed = UINT16_MAX;
	int max_speed = absolute_max_speed;

	if (_settings_game.vehicle.train_acceleration_model == AccelerationModel::Original) return max_speed;

	int curvecount[2] = {0, 0};

	/* first find the curve speed limit */
	int numcurve = 0;
	int sum = 0;
	int pos = 0;
	int lastpos = -1;
	for (const Train *u = this; u->Next() != nullptr; u = u->Next(), pos += u->gcache.cached_veh_length) {
		Direction this_dir = u->direction;
		Direction next_dir = u->Next()->direction;

		DirDiff dirdiff = DirDifference(this_dir, next_dir);
		if (dirdiff == DirDiff::Same) continue;

		if (dirdiff == DirDiff::Left45) curvecount[0]++;
		if (dirdiff == DirDiff::Right45) curvecount[1]++;
		if (dirdiff == DirDiff::Left45 || dirdiff == DirDiff::Right45) {
			if (lastpos != -1) {
				numcurve++;
				sum += pos - lastpos;
				if (pos - lastpos <= static_cast<int>(VEHICLE_LENGTH) && max_speed > 88) {
					max_speed = 88;
				}
			}
			lastpos = pos;
		}

		/* if we have a 90 degree turn, fix the speed limit to 60 */
		if (dirdiff == DirDiff::Left90 || dirdiff == DirDiff::Right90) {
			max_speed = 61;
		}
	}

	if (numcurve > 0 && max_speed > 88) {
		if (curvecount[0] == 1 && curvecount[1] == 1) {
			max_speed = absolute_max_speed;
		} else {
			sum = CeilDiv(sum, VEHICLE_LENGTH);
			sum /= numcurve;
			max_speed = 232 - (13 - Clamp(sum, 1, 12)) * (13 - Clamp(sum, 1, 12));
		}
	}

	if (max_speed != absolute_max_speed) {
		/* Apply the current railtype's curve speed advantage */
		const RailTypeInfo *rti = GetRailTypeInfo(GetRailType(this->tile));
		max_speed += (max_speed / 2) * rti->curve_speed;

		if (this->tcache.cached_tilt) {
			/* Apply max_speed bonus of 20% for a tilting train */
			max_speed += max_speed / 5;
		}

		/* Apply max_speed modifier (cached value is fixed-point binary with 8 fractional bits)
		 * and clamp the result to an acceptable range. */
		max_speed += (max_speed * this->tcache.cached_curve_speed_mod) / 256;
		max_speed = Clamp(max_speed, 2, absolute_max_speed);
	}

	return static_cast<uint16_t>(max_speed);
}

/**
 * Calculates the maximum speed of the vehicle under its current conditions.
 * @return Maximum speed of the vehicle.
 */
int Train::GetCurrentMaxSpeed() const
{
	const Train *moving_front = this->GetMovingFront();
	int max_speed = _settings_game.vehicle.train_acceleration_model == AccelerationModel::Original ?
			this->gcache.cached_max_track_speed :
			this->tcache.cached_max_curve_speed;

	if (_settings_game.vehicle.train_acceleration_model == AccelerationModel::Realistic && IsRailStationTile(moving_front->tile)) {
		StationID sid = GetStationIndex(moving_front->tile);
		if (this->current_order.ShouldStopAtStation(this, sid)) {
			int station_ahead;
			int station_length;
			int stop_at = GetTrainStopLocation(sid, moving_front->tile, moving_front, &station_ahead, &station_length);

			/* The distance to go is whatever is still ahead of the train minus the
			 * distance from the train's stop location to the end of the platform */
			int distance_to_go = station_ahead / TILE_SIZE - (station_length - stop_at) / TILE_SIZE;

			if (distance_to_go > 0) {
				int st_max_speed = 120;

				int delta_v = this->cur_speed / (distance_to_go + 1);
				if (max_speed > (this->cur_speed - delta_v)) {
					st_max_speed = this->cur_speed - (delta_v / 10);
				}

				st_max_speed = std::max(st_max_speed, 25 * distance_to_go);
				max_speed = std::min(max_speed, st_max_speed);
			}
		}
	}

	for (const Train *u = this; u != nullptr; u = u->Next()) {
		if (_settings_game.vehicle.train_acceleration_model == AccelerationModel::Realistic && u->track == Track::Depot) {
			constexpr int DEPOT_SPEED_LIMIT = 61;
			max_speed = std::min(max_speed, DEPOT_SPEED_LIMIT);
			break;
		}

		/* Vehicle is on the middle part of a bridge. */
		if (u->track == Track::Wormhole && !u->vehstatus.Test(VehState::Hidden)) {
			max_speed = std::min<int>(max_speed, GetBridgeSpec(GetBridgeType(u->tile))->speed);
		}
	}

	max_speed = std::min<int>(max_speed, this->current_order.GetMaxSpeed());

	/* If the train is going backwards, without a leading cab, restrict its speed. */
	if (!moving_front->CanLeadTrain()) {
		constexpr int BACKWARDS_NO_CAB_SPEED_LIMIT = 32;
		max_speed = std::min<int>(max_speed, BACKWARDS_NO_CAB_SPEED_LIMIT);
	}

	return std::min<int>(max_speed, this->gcache.cached_max_track_speed);
}

/** Update acceleration of the train from the cached power and weight. */
void Train::UpdateAcceleration()
{
	assert(this->IsFrontEngine() || this->IsFreeWagon());

	uint power = this->gcache.cached_power;
	uint weight = this->gcache.cached_weight;
	assert(weight != 0);
	this->acceleration = Clamp(power / weight * 4, 1, 255);
}

/**
 * Get the offset for train image when it is used as cursor.
 * @return The offset in horizontal direction.
 */
int Train::GetCursorImageOffset() const
{
	if (this->gcache.cached_veh_length != 8 && this->flags.Test(VehicleRailFlag::Flipped) && !EngInfo(this->engine_type)->misc_flags.Test(EngineMiscFlag::RailFlips)) {
		int reference_width = TRAININFO_DEFAULT_VEHICLE_WIDTH;

		const Engine *e = this->GetEngine();
		if (e->GetGRF() != nullptr && IsCustomVehicleSpriteNum(e->VehInfo<RailVehicleInfo>().image_index)) {
			reference_width = e->GetGRF()->traininfo_vehicle_width;
		}

		return ScaleSpriteTrad((this->gcache.cached_veh_length - (int)VEHICLE_LENGTH) * reference_width / (int)VEHICLE_LENGTH);
	}
	return 0;
}

/**
 * Get the width of a train vehicle image in the GUI.
 * @param offset Additional offset for positioning the sprite; set to nullptr if not needed
 * @return Width in pixels
 */
int Train::GetDisplayImageWidth(Point *offset) const
{
	int reference_width = TRAININFO_DEFAULT_VEHICLE_WIDTH;
	int vehicle_pitch = 0;

	const Engine *e = this->GetEngine();
	if (e->GetGRF() != nullptr && IsCustomVehicleSpriteNum(e->VehInfo<RailVehicleInfo>().image_index)) {
		reference_width = e->GetGRF()->traininfo_vehicle_width;
		vehicle_pitch = e->GetGRF()->traininfo_vehicle_pitch;
	}

	if (offset != nullptr) {
		if (this->flags.Test(VehicleRailFlag::Flipped) && !EngInfo(this->engine_type)->misc_flags.Test(EngineMiscFlag::RailFlips)) {
			offset->x = ScaleSpriteTrad(((int)this->gcache.cached_veh_length - (int)VEHICLE_LENGTH / 2) * reference_width / (int)VEHICLE_LENGTH);
		} else {
			offset->x = ScaleSpriteTrad(reference_width) / 2;
		}
		offset->y = ScaleSpriteTrad(vehicle_pitch);
	}
	return ScaleSpriteTrad(this->gcache.cached_veh_length * reference_width / VEHICLE_LENGTH);
}

static SpriteID GetDefaultTrainSprite(uint8_t spritenum, Direction direction)
{
	assert(IsValidImageIndex<VehicleType::Train>(spritenum));
	return ((to_underlying(direction) + _engine_sprite_add[spritenum]) & _engine_sprite_and[spritenum]) + _engine_sprite_base[spritenum];
}

/**
 * Get the sprite to display the train.
 * @param direction Direction of view/travel.
 * @param image_type Visualisation context.
 * @param result Sprite sequence to add the to be drawn sprites to.
 */
void Train::GetImage(Direction direction, EngineImageType image_type, VehicleSpriteSeq *result) const
{
	uint8_t spritenum = this->spritenum;

	if (this->flags.Test(VehicleRailFlag::Flipped)) direction = ReverseDir(direction);

	if (IsCustomVehicleSpriteNum(spritenum)) {
		if (spritenum == CUSTOM_VEHICLE_SPRITENUM_REVERSED) direction = ReverseDir(direction);
		GetCustomVehicleSprite(this, direction, image_type, result);
		if (result->IsValid()) return;

		spritenum = this->GetEngine()->original_image_index;
	}

	assert(IsValidImageIndex<VehicleType::Train>(spritenum));
	SpriteID sprite = GetDefaultTrainSprite(spritenum, direction);

	if (this->cargo.StoredCount() >= this->cargo_cap / 2U) sprite += _wagon_full_adder[spritenum];

	result->Set(sprite);
}

static void GetRailIcon(EngineID engine, bool rear_head, int &y, EngineImageType image_type, VehicleSpriteSeq *result)
{
	const Engine *e = Engine::Get(engine);
	Direction dir = rear_head ? Direction::E : Direction::W;
	uint8_t spritenum = e->VehInfo<RailVehicleInfo>().image_index;

	if (IsCustomVehicleSpriteNum(spritenum)) {
		GetCustomVehicleIcon(engine, dir, image_type, result);
		if (result->IsValid()) {
			if (e->GetGRF() != nullptr) {
				y += ScaleSpriteTrad(e->GetGRF()->traininfo_vehicle_pitch);
			}
			return;
		}

		spritenum = Engine::Get(engine)->original_image_index;
	}

	if (rear_head) spritenum++;

	result->Set(GetDefaultTrainSprite(spritenum, Direction::W));
}

void DrawTrainEngine(int left, int right, int preferred_x, int y, EngineID engine, PaletteID pal, EngineImageType image_type)
{
	const GRFFile *grf = Engine::Get(engine)->GetGRF();
	int vehicle_width = ScaleSpriteTrad(grf == nullptr ? TRAININFO_DEFAULT_VEHICLE_WIDTH : grf->traininfo_vehicle_width);

	if (RailVehInfo(engine)->railveh_type == RailVehicleType::Multihead) {
		int yf = y;
		int yr = y;

		VehicleSpriteSeq seqf, seqr;
		GetRailIcon(engine, false, yf, image_type, &seqf);
		GetRailIcon(engine, true, yr, image_type, &seqr);

		Rect rectf, rectr;
		seqf.GetBounds(&rectf);
		seqr.GetBounds(&rectr);

		preferred_x = Clamp(preferred_x,
				left - UnScaleGUI(rectf.left) + vehicle_width / 2,
				right - UnScaleGUI(rectr.right) - (vehicle_width - vehicle_width / 2));

		seqf.Draw(preferred_x - vehicle_width / 2, yf, pal, pal == PALETTE_CRASH);
		seqr.Draw(preferred_x + (vehicle_width - vehicle_width / 2), yr, pal, pal == PALETTE_CRASH);
	} else {
		VehicleSpriteSeq seq;
		GetRailIcon(engine, false, y, image_type, &seq);

		Rect rect;
		seq.GetBounds(&rect);
		preferred_x = Clamp(preferred_x,
				left - UnScaleGUI(rect.left),
				right - UnScaleGUI(rect.right));

		seq.Draw(preferred_x, y, pal, pal == PALETTE_CRASH);
	}
}

/**
 * Get the size of the sprite of a train sprite heading west, or both heads (used for lists).
 * @param engine The engine to get the sprite from.
 * @param[out] width The width of the sprite.
 * @param[out] height The height of the sprite.
 * @param[out] xoffs Number of pixels to shift the sprite to the right.
 * @param[out] yoffs Number of pixels to shift the sprite downwards.
 * @param image_type Context the sprite is used in.
 */
void GetTrainSpriteSize(EngineID engine, uint &width, uint &height, int &xoffs, int &yoffs, EngineImageType image_type)
{
	int y = 0;

	VehicleSpriteSeq seq;
	GetRailIcon(engine, false, y, image_type, &seq);

	Rect rect;
	seq.GetBounds(&rect);

	width  = UnScaleGUI(rect.Width());
	height = UnScaleGUI(rect.Height());
	xoffs  = UnScaleGUI(rect.left);
	yoffs  = UnScaleGUI(rect.top);

	if (RailVehInfo(engine)->railveh_type == RailVehicleType::Multihead) {
		const GRFFile *grf = Engine::Get(engine)->GetGRF();
		int vehicle_width = ScaleSpriteTrad(grf == nullptr ? TRAININFO_DEFAULT_VEHICLE_WIDTH : grf->traininfo_vehicle_width);

		GetRailIcon(engine, true, y, image_type, &seq);
		seq.GetBounds(&rect);

		/* Calculate values relative to an imaginary center between the two sprites. */
		width = vehicle_width + UnScaleGUI(rect.right) - xoffs;
		height = std::max<uint>(height, UnScaleGUI(rect.Height()));
		xoffs  = xoffs - vehicle_width / 2;
		yoffs  = std::min(yoffs, UnScaleGUI(rect.top));
	}
}

/**
 * Get a list of free wagons in a depot.
 * @param tile Tile of depot.
 * @return List of free wagons, sorted by vehicle index.
 */
static std::vector<VehicleID> GetFreeWagonsInDepot(TileIndex tile)
{
	std::vector<VehicleID> free_wagons;

	for (Vehicle *v : VehiclesOnTile(tile)) {
		if (v->type != VehicleType::Train) continue;
		if (v->vehstatus.Test(VehState::Crashed)) continue;
		if (!Train::From(v)->IsFreeWagon()) continue;

		free_wagons.push_back(v->index);
	}

	/* Sort by vehicle index for consistency across clients. */
	std::ranges::sort(free_wagons);
	return free_wagons;
}

/**
 * Build a railroad wagon.
 * @param flags    type of operation.
 * @param tile     tile of the depot where rail-vehicle is built.
 * @param e        the engine to build.
 * @param[out] ret the vehicle that has been built.
 * @return the cost of this operation or an error.
 */
static CommandCost CmdBuildRailWagon(DoCommandFlags flags, TileIndex tile, const Engine *e, Vehicle **ret)
{
	const RailVehicleInfo *rvi = &e->VehInfo<RailVehicleInfo>();

	/* Check that the wagon can drive on the track in question */
	if (!IsCompatibleRail(rvi->railtypes, GetRailType(tile))) return CMD_ERROR;

	if (flags.Test(DoCommandFlag::Execute)) {
		Train *v = Train::Create();
		*ret = v;
		v->spritenum = rvi->image_index;

		v->engine_type = e->index;
		v->gcache.first_engine = EngineID::Invalid(); // needs to be set before first callback

		DiagDirection dir = GetRailDepotDirection(tile);

		v->direction = DiagDirToDir(dir);
		v->tile = tile;

		int x = TileX(tile) * TILE_SIZE | _vehicle_initial_x_fract[dir];
		int y = TileY(tile) * TILE_SIZE | _vehicle_initial_y_fract[dir];

		v->x_pos = x;
		v->y_pos = y;
		v->z_pos = GetSlopePixelZ(x, y, true);
		v->owner = _current_company;
		v->track = Track::Depot;
		v->vehstatus = {VehState::Hidden, VehState::DefaultPalette};

		v->SetWagon();

		v->SetFreeWagon();
		InvalidateWindowData(WindowClass::VehicleDepot, v->tile);

		v->cargo_type = e->GetDefaultCargoType();
		assert(IsValidCargoType(v->cargo_type));
		v->cargo_cap = rvi->capacity;
		v->refit_cap = 0;

		v->railtypes = rvi->railtypes;

		v->date_of_last_service = TimerGameEconomy::date;
		v->date_of_last_service_newgrf = TimerGameCalendar::date;
		v->build_year = TimerGameCalendar::year;
		v->sprite_cache.sprite_seq.Set(SPR_IMG_QUERY);
		v->random_bits = Random();

		v->group_id = DEFAULT_GROUP;

		auto prob = TestVehicleBuildProbability(v, BuildProbabilityType::Reversed);
		if (prob.has_value()) v->flags.Set(VehicleRailFlag::Flipped, prob.value());
		AddArticulatedParts(v);

		v->UpdatePosition();
		v->First()->ConsistChanged(CCF_ARRANGE);
		UpdateTrainGroupID(v->First());

		CheckConsistencyOfArticulatedVehicle(v);

		/* Try to connect the vehicle to one of free chains of wagons. */
		for (VehicleID vehicle : GetFreeWagonsInDepot(tile)) {
			if (vehicle == v->index) continue;

			const Train *w = Train::Get(vehicle);
			if (w->engine_type != v->engine_type) continue; ///< Must be same type
			if (w->First() == v) continue; ///< Don't connect to ourself

			if (Command<Commands::MoveRailVehicle>::Do(DoCommandFlag::Execute, v->index, w->Last()->index, true).Succeeded()) {
				break;
			}
		}
	}

	return CommandCost();
}

/**
 * Move all free vehicles in the depot to the train.
 * @param u The train to move the free vehicles to.
 */
void NormalizeTrainVehInDepot(const Train *u)
{
	assert(u->IsEngine());
	for (VehicleID vehicle : GetFreeWagonsInDepot(u->tile)) {
		if (Command<Commands::MoveRailVehicle>::Do(DoCommandFlag::Execute, vehicle, u->index, true).Failed()) {
			break;
		}
	}
}

static void AddRearEngineToMultiheadedTrain(Train *v)
{
	Train *u = Train::Create();
	v->value >>= 1;
	u->value = v->value;
	u->direction = v->direction;
	u->owner = v->owner;
	u->tile = v->tile;
	u->x_pos = v->x_pos;
	u->y_pos = v->y_pos;
	u->z_pos = v->z_pos;
	u->track = Track::Depot;
	u->vehstatus = v->vehstatus;
	u->vehstatus.Reset(VehState::Stopped);
	u->spritenum = v->spritenum + 1;
	u->cargo_type = v->cargo_type;
	u->cargo_subtype = v->cargo_subtype;
	u->cargo_cap = v->cargo_cap;
	u->refit_cap = v->refit_cap;
	u->railtypes = v->railtypes;
	u->engine_type = v->engine_type;
	u->date_of_last_service = v->date_of_last_service;
	u->date_of_last_service_newgrf = v->date_of_last_service_newgrf;
	u->build_year = v->build_year;
	u->sprite_cache.sprite_seq.Set(SPR_IMG_QUERY);
	u->random_bits = Random();
	v->SetMultiheaded();
	u->SetMultiheaded();
	v->SetNext(u);
	auto prob = TestVehicleBuildProbability(u, BuildProbabilityType::Reversed);
	if (prob.has_value()) u->flags.Set(VehicleRailFlag::Flipped, prob.value());
	u->UpdatePosition();

	/* Now we need to link the front and rear engines together */
	v->other_multiheaded_part = u;
	u->other_multiheaded_part = v;
}

/**
 * Build a railroad vehicle.
 * @param flags    type of operation.
 * @param tile     tile of the depot where rail-vehicle is built.
 * @param e        the engine to build.
 * @param[out] ret the vehicle that has been built.
 * @return the cost of this operation or an error.
 */
CommandCost CmdBuildRailVehicle(DoCommandFlags flags, TileIndex tile, const Engine *e, Vehicle **ret)
{
	const RailVehicleInfo *rvi = &e->VehInfo<RailVehicleInfo>();

	if (rvi->railveh_type == RailVehicleType::Wagon) return CmdBuildRailWagon(flags, tile, e, ret);

	/* Check if depot and new engine uses the same kind of tracks *
	 * We need to see if the engine got power on the tile to avoid electric engines in non-electric depots */
	if (!HasPowerOnRail(rvi->railtypes, GetRailType(tile))) return CMD_ERROR;

	if (flags.Test(DoCommandFlag::Execute)) {
		DiagDirection dir = GetRailDepotDirection(tile);
		int x = TileX(tile) * TILE_SIZE + _vehicle_initial_x_fract[dir];
		int y = TileY(tile) * TILE_SIZE + _vehicle_initial_y_fract[dir];

		Train *v = Train::Create();
		*ret = v;
		v->direction = DiagDirToDir(dir);
		v->tile = tile;
		v->owner = _current_company;
		v->x_pos = x;
		v->y_pos = y;
		v->z_pos = GetSlopePixelZ(x, y, true);
		v->track = Track::Depot;
		v->vehstatus = {VehState::Hidden, VehState::Stopped, VehState::DefaultPalette};
		v->spritenum = rvi->image_index;
		v->cargo_type = e->GetDefaultCargoType();
		assert(IsValidCargoType(v->cargo_type));
		v->cargo_cap = rvi->capacity;
		v->refit_cap = 0;
		v->last_station_visited = StationID::Invalid();
		v->last_loading_station = StationID::Invalid();

		v->engine_type = e->index;
		v->gcache.first_engine = EngineID::Invalid(); // needs to be set before first callback

		v->reliability = e->reliability;
		v->reliability_spd_dec = e->reliability_spd_dec;
		v->max_age = e->GetLifeLengthInDays();

		v->railtypes = rvi->railtypes;

		v->SetServiceInterval(Company::Get(_current_company)->settings.vehicle.servint_trains);
		v->date_of_last_service = TimerGameEconomy::date;
		v->date_of_last_service_newgrf = TimerGameCalendar::date;
		v->build_year = TimerGameCalendar::year;
		v->sprite_cache.sprite_seq.Set(SPR_IMG_QUERY);
		v->random_bits = Random();

		if (e->flags.Test(EngineFlag::ExclusivePreview)) v->vehicle_flags.Set(VehicleFlag::BuiltAsPrototype);
		v->SetServiceIntervalIsPercent(Company::Get(_current_company)->settings.vehicle.servint_ispercent);

		v->group_id = DEFAULT_GROUP;

		v->SetFrontEngine();
		v->SetEngine();

		auto prob = TestVehicleBuildProbability(v, BuildProbabilityType::Reversed);
		if (prob.has_value()) v->flags.Set(VehicleRailFlag::Flipped, prob.value());
		v->UpdatePosition();

		if (rvi->railveh_type == RailVehicleType::Multihead) {
			AddRearEngineToMultiheadedTrain(v);
		} else {
			AddArticulatedParts(v);
		}

		v->ConsistChanged(CCF_ARRANGE);
		UpdateTrainGroupID(v);

		CheckConsistencyOfArticulatedVehicle(v);
	}

	return CommandCost();
}

static Train *FindGoodVehiclePos(const Train *src)
{
	EngineID eng = src->engine_type;

	for (VehicleID vehicle : GetFreeWagonsInDepot(src->tile)) {
		Train *dst = Train::Get(vehicle);

		/* check so all vehicles in the line have the same engine. */
		Train *t = dst;
		while (t->engine_type == eng) {
			t = t->Next();
			if (t == nullptr) return dst;
		}
	}

	return nullptr;
}

/** Helper type for lists/vectors of trains */
typedef std::vector<Train *> TrainList;

/**
 * Make a backup of a train into a train list.
 * @param list to make the backup in
 * @param t    the train to make the backup of
 */
static void MakeTrainBackup(TrainList &list, Train *t)
{
	for (; t != nullptr; t = t->Next()) list.push_back(t);
}

/**
 * Restore the train from the backup list.
 * @param list the train to restore.
 */
static void RestoreTrainBackup(TrainList &list)
{
	/* No train, nothing to do. */
	if (list.empty()) return;

	Train *prev = nullptr;
	/* Iterate over the list and rebuild it. */
	for (Train *t : list) {
		if (prev != nullptr) {
			prev->SetNext(t);
		} else if (t->Previous() != nullptr) {
			/* Make sure the head of the train is always the first in the chain. */
			t->Previous()->SetNext(nullptr);
		}
		prev = t;
	}
}

/**
 * Remove the given wagon from its consist.
 * @param part the part of the train to remove.
 * @param chain whether to remove the whole chain.
 */
static void RemoveFromConsist(Train *part, bool chain = false)
{
	Train *tail;

	if (chain) {
		/* We're moving several vehicles, find the last one in the chain. */
		tail = part;
		while (tail->Next() != nullptr) tail = tail->Next();
	} else {
		/* We're just moving one vehicle, but make sure we get all the articulated parts. */
		tail = part->GetLastEnginePart();
	}

	/* Unlink at the front, but make it point to the next
	 * vehicle after the to be remove part. */
	if (part->Previous() != nullptr) part->Previous()->SetNext(tail->Next());

	/* Unlink at the back */
	tail->SetNext(nullptr);
}

/**
 * Inserts a chain into the train at dst.
 * @param dst   the place where to append after.
 * @param chain the chain to actually add.
 */
static void InsertInConsist(Train *dst, Train *chain)
{
	/* We do not want to add something in the middle of an articulated part. */
	assert(dst != nullptr && (dst->Next() == nullptr || !dst->Next()->IsArticulatedPart()));

	chain->Last()->SetNext(dst->Next());
	dst->SetNext(chain);
}

/**
 * Normalise the dual heads in the train, i.e. if one is
 * missing move that one to this train.
 * @param t the train to normalise.
 */
static void NormaliseDualHeads(Train *t)
{
	for (; t != nullptr; t = t->GetNextVehicle()) {
		if (!t->IsMultiheaded() || !t->IsEngine()) continue;

		/* Make sure that there are no free cars before next engine */
		Train *u;
		for (u = t; u->Next() != nullptr && !u->Next()->IsEngine(); u = u->Next()) {}

		if (u == t->other_multiheaded_part) continue;

		/* Remove the part from the 'wrong' train */
		RemoveFromConsist(t->other_multiheaded_part);
		/* And add it to the 'right' train */
		InsertInConsist(u, t->other_multiheaded_part);
	}
}

/**
 * Normalise the sub types of the parts in this chain.
 * @param chain the chain to normalise.
 */
static void NormaliseSubtypes(Train *chain)
{
	/* Nothing to do */
	if (chain == nullptr) return;

	/* We must be the first in the chain. */
	assert(chain->Previous() == nullptr);

	/* Set the appropriate bits for the first in the chain. */
	if (chain->IsWagon()) {
		chain->SetFreeWagon();
	} else {
		assert(chain->IsEngine());
		chain->SetFrontEngine();
	}

	/* Now clear the bits for the rest of the chain */
	for (Train *t = chain->Next(); t != nullptr; t = t->Next()) {
		t->ClearFreeWagon();
		t->ClearFrontEngine();
	}
}

/**
 * Check/validate whether we may actually build a new train.
 * @note All vehicles are/were 'heads' of their chains.
 * @param original_dst The original destination chain.
 * @param dst          The destination chain after constructing the train.
 * @param original_src The original source chain.
 * @param src          The source chain after constructing the train.
 * @return possible error of this command.
 */
static CommandCost CheckNewTrain(Train *original_dst, Train *dst, Train *original_src, Train *src)
{
	/* Just add 'new' engines and subtract the original ones.
	 * If that's less than or equal to 0 we can be sure we did
	 * not add any engines (read: trains) along the way. */
	if ((src          != nullptr && src->IsEngine()          ? 1 : 0) +
			(dst          != nullptr && dst->IsEngine()          ? 1 : 0) -
			(original_src != nullptr && original_src->IsEngine() ? 1 : 0) -
			(original_dst != nullptr && original_dst->IsEngine() ? 1 : 0) <= 0) {
		return CommandCost();
	}

	/* Get a free unit number and check whether it's within the bounds.
	 * There will always be a maximum of one new train. */
	if (GetFreeUnitNumber(VehicleType::Train) <= _settings_game.vehicle.max_trains) return CommandCost();

	return CommandCost(STR_ERROR_TOO_MANY_VEHICLES_IN_GAME);
}

/**
 * Check whether the train parts can be attached.
 * @param t the train to check
 * @return possible error of this command.
 */
static CommandCost CheckTrainAttachment(Train *t)
{
	/* No multi-part train, no need to check. */
	if (t == nullptr || t->Next() == nullptr) return CommandCost();

	/* The maximum length for a train. For each part we decrease this by one
	 * and if the result is negative the train is simply too long. */
	int allowed_len = _settings_game.vehicle.max_train_length * TILE_SIZE - t->gcache.cached_veh_length;

	/* For free-wagon chains, check if they are within the max_train_length limit. */
	if (!t->IsEngine()) {
		t = t->Next();
		while (t != nullptr) {
			allowed_len -= t->gcache.cached_veh_length;

			t = t->Next();
		}

		if (allowed_len < 0) return CommandCost(STR_ERROR_TRAIN_TOO_LONG);
		return CommandCost();
	}

	Train *head = t;
	Train *prev = t;

	/* Break the prev -> t link so it always holds within the loop. */
	t = t->Next();
	prev->SetNext(nullptr);

	/* Make sure the cache is cleared. */
	head->InvalidateNewGRFCache();

	while (t != nullptr) {
		allowed_len -= t->gcache.cached_veh_length;

		Train *next = t->Next();

		/* Unlink the to-be-added piece; it is already unlinked from the previous
		 * part due to the fact that the prev -> t link is broken. */
		t->SetNext(nullptr);

		/* Don't check callback for articulated or rear dual headed parts */
		if (!t->IsArticulatedPart() && !t->IsRearDualheaded()) {
			/* Back up and clear the first_engine data to avoid using wagon override group */
			EngineID first_engine = t->gcache.first_engine;
			t->gcache.first_engine = EngineID::Invalid();

			/* We don't want the cache to interfere. head's cache is cleared before
			 * the loop and after each callback does not need to be cleared here. */
			t->InvalidateNewGRFCache();

			std::array<int32_t, 1> regs100;
			uint16_t callback = GetVehicleCallbackParent(CBID_TRAIN_ALLOW_WAGON_ATTACH, 0, 0, head->engine_type, t, head, regs100);

			/* Restore original first_engine data */
			t->gcache.first_engine = first_engine;

			/* We do not want to remember any cached variables from the test run */
			t->InvalidateNewGRFCache();
			head->InvalidateNewGRFCache();

			if (callback != CALLBACK_FAILED) {
				/* A failing callback means everything is okay */
				StringID error = STR_NULL;

				if (head->GetGRF()->grf_version < 8) {
					if (callback == 0xFD) error = STR_ERROR_INCOMPATIBLE_RAIL_TYPES;
					if (callback  < 0xFD) error = GetGRFStringID(head->GetGRFID(), GRFSTR_MISC_GRF_TEXT + callback);
					if (callback >= 0x100) ErrorUnknownCallbackResult(head->GetGRFID(), CBID_TRAIN_ALLOW_WAGON_ATTACH, callback);
				} else {
					if (callback < 0x400) {
						error = GetGRFStringID(head->GetGRFID(), GRFSTR_MISC_GRF_TEXT + callback);
					} else {
						switch (callback) {
							case 0x400: // allow if railtypes match (always the case for OpenTTD)
							case 0x401: // allow
								break;

							case 0x40F:
								error = GetGRFStringID(head->GetGRFID(), static_cast<GRFStringID>(regs100[0]));
								break;

							default:    // unknown reason -> disallow
							case 0x402: // disallow attaching
								error = STR_ERROR_INCOMPATIBLE_RAIL_TYPES;
								break;
						}
					}
				}

				if (error != STR_NULL) return CommandCost(error);
			}
		}

		/* And link it to the new part. */
		prev->SetNext(t);
		prev = t;
		t = next;
	}

	if (allowed_len < 0) return CommandCost(STR_ERROR_TRAIN_TOO_LONG);
	return CommandCost();
}

/**
 * Validate whether we are going to create valid trains.
 * @note All vehicles are/were 'heads' of their chains.
 * @param original_dst The original destination chain.
 * @param dst          The destination chain after constructing the train.
 * @param original_src The original source chain.
 * @param src          The source chain after constructing the train.
 * @param check_limit  Whether to check the vehicle limit.
 * @return possible error of this command.
 */
static CommandCost ValidateTrains(Train *original_dst, Train *dst, Train *original_src, Train *src, bool check_limit)
{
	/* Check whether we may actually construct the trains. */
	CommandCost ret = CheckTrainAttachment(src);
	if (ret.Failed()) return ret;
	ret = CheckTrainAttachment(dst);
	if (ret.Failed()) return ret;

	/* Check whether we need to build a new train. */
	return check_limit ? CheckNewTrain(original_dst, dst, original_src, src) : CommandCost();
}

/**
 * Arrange the trains in the wanted way.
 * @param dst_head   The destination chain of the to be moved vehicle.
 * @param dst        The destination for the to be moved vehicle.
 * @param src_head   The source chain of the to be moved vehicle.
 * @param src        The to be moved vehicle.
 * @param move_chain Whether to move all vehicles after src or not.
 */
static void ArrangeTrains(Train **dst_head, Train *dst, Train **src_head, Train *src, bool move_chain)
{
	/* First determine the front of the two resulting trains */
	if (*src_head == *dst_head) {
		/* If we aren't moving part(s) to a new train, we are just moving the
		 * front back and there is not destination head. */
		*dst_head = nullptr;
	} else if (*dst_head == nullptr) {
		/* If we are moving to a new train the head of the move train would become
		 * the head of the new vehicle. */
		*dst_head = src;
	}

	if (src == *src_head) {
		/* If we are moving the front of a train then we are, in effect, creating
		 * a new head for the train. Point to that. Unless we are moving the whole
		 * train in which case there is not 'source' train anymore.
		 * In case we are a multiheaded part we want the complete thing to come
		 * with us, so src->GetNextUnit(), however... when we are e.g. a wagon
		 * that is followed by a rear multihead we do not want to include that. */
		*src_head = move_chain ? nullptr :
				(src->IsMultiheaded() ? src->GetNextUnit() : src->GetNextVehicle());
	}

	/* Now it's just simply removing the part that we are going to move from the
	 * source train and *if* the destination is a not a new train add the chain
	 * at the destination location. */
	RemoveFromConsist(src, move_chain);
	if (*dst_head != src) InsertInConsist(dst, src);

	/* Now normalise the dual heads, that is move the dual heads around in such
	 * a way that the head and rear of a dual head are in the same train */
	NormaliseDualHeads(*src_head);
	NormaliseDualHeads(*dst_head);
}

/**
 * Normalise the head of the train again, i.e. that is tell the world that
 * we have changed and update all kinds of variables.
 * @param head the train to update.
 */
static void NormaliseTrainHead(Train *head)
{
	/* Not much to do! */
	if (head == nullptr) return;

	/* Tell the 'world' the train changed. */
	head->ConsistChanged(CCF_ARRANGE);
	UpdateTrainGroupID(head);

	/* Not a front engine, i.e. a free wagon chain. No need to do more. */
	if (!head->IsFrontEngine()) return;

	/* Update the refit button and window */
	InvalidateWindowData(WindowClass::VehicleRefit, head->index, VIWD_CONSIST_CHANGED);
	SetWindowWidgetDirty(WindowClass::VehicleView, head->index, WID_VV_REFIT);

	/* If we don't have a unit number yet, set one. */
	if (head->unitnumber != 0) return;
	head->unitnumber = Company::Get(head->owner)->freeunits[head->type].UseID(GetFreeUnitNumber(VehicleType::Train));
}

static CommandCost TryConsistSplice(DoCommandFlags flags, Train *src, Train *dst, bool move_chain, bool keep_absorbed_identity = false);
static void TrainEnterStation(Train *consist, StationID station);
static void AdvanceWagonsBeforeSwap(Train *moving_front);
static void AdvanceWagonsAfterSwap(Train *moving_front);
void ReverseTrainSwapVehicles(Train *v);
static bool IsAnyPartInsideDepot(const Train *v);

/**
 * Move a rail vehicle around inside the depot.
 * @param flags type of operation
 *              Note: DoCommandFlag::AutoReplace is set when autoreplace tries to undo its modifications or moves vehicles to temporary locations inside the depot.
 * @param src_veh source vehicle index
 * @param dest_veh what wagon to put the source wagon AFTER, XXX - VehicleID::Invalid() to make a new line
 * @param move_chain move all vehicles following the source vehicle
 * @return the cost of this operation or an error
 */
CommandCost CmdMoveRailVehicle(DoCommandFlags flags, VehicleID src_veh, VehicleID dest_veh, bool move_chain)
{
	Train *src = Train::GetIfValid(src_veh);
	if (src == nullptr) return CMD_ERROR;

	CommandCost ret = CheckOwnership(src->owner);
	if (ret.Failed()) return ret;

	/* Do not allow moving crashed vehicles inside the depot, it is likely to cause asserts later */
	if (src->vehstatus.Test(VehState::Crashed)) return CMD_ERROR;

	/* if nothing is selected as destination, try and find a matching vehicle to drag to. */
	Train *dst;
	if (dest_veh == VehicleID::Invalid()) {
		dst = (src->IsEngine() || flags.Test(DoCommandFlag::AutoReplace)) ? nullptr : FindGoodVehiclePos(src);
	} else {
		dst = Train::GetIfValid(dest_veh);
		if (dst == nullptr) return CMD_ERROR;

		ret = CheckOwnership(dst->owner);
		if (ret.Failed()) return ret;

		/* Do not allow appending to crashed vehicles, too */
		if (dst->vehstatus.Test(VehState::Crashed)) return CMD_ERROR;
	}

	/* if an articulated part is being handled, deal with its parent vehicle */
	src = src->GetFirstEnginePart();
	if (dst != nullptr) {
		dst = dst->GetFirstEnginePart();
	}

	/* don't move the same vehicle.. */
	if (src == dst) return CommandCost();

	/* locate the head of the two chains, purely to run the depot-specific
	 * preconditions below; TryConsistSplice() will compute them again. */
	Train *src_head = src->First();
	Train *dst_head;
	if (dst != nullptr) {
		dst_head = dst->First();
		if (dst_head->tile != src_head->tile) return CMD_ERROR;
		/* Now deal with articulated part of destination wagon */
		dst = dst->GetLastEnginePart();
	} else {
		dst_head = nullptr;
	}

	if (src->IsRearDualheaded()) return CommandCost(STR_ERROR_REAR_ENGINE_FOLLOW_FRONT);

	/* When moving all wagons, we can't have the same src_head and dst_head */
	if (move_chain && src_head == dst_head) return CommandCost();

	/* When moving a multiheaded part to be place after itself, bail out. */
	if (!move_chain && dst != nullptr && dst->IsRearDualheaded() && src == dst->other_multiheaded_part) return CommandCost();

	/* Check if all vehicles in the source train are stopped inside a depot. */
	if (!src_head->IsStoppedInDepot()) return CommandCost(STR_ERROR_TRAINS_CAN_ONLY_BE_ALTERED_INSIDE_A_DEPOT);

	/* Check if all vehicles in the destination train are stopped inside a depot. */
	if (dst_head != nullptr && !dst_head->IsStoppedInDepot()) return CommandCost(STR_ERROR_TRAINS_CAN_ONLY_BE_ALTERED_INSIDE_A_DEPOT);

	return TryConsistSplice(flags, src, dst, move_chain);
}

/**
 * Split/splice two train consists into a new arrangement: move @p src
 * (and, if @p move_chain, everything following it) to right after
 * @p dst. Shared primitive behind every operation that mutates train
 * consists -- inside a depot (#CmdMoveRailVehicle) as well as coupling/
 * decoupling on the open track (#CmdCoupleTrains, decouple-on-departure
 * orders). Always backs up both sides, always validates before
 * committing, always restores both sides symmetrically on failure --
 * see FEATURE_DESIGN_COUPLING_TOW.md ("Bug B" in the reference patch
 * this design avoids: an asymmetric backup/rollback that only restored
 * one side of a failed split).
 *
 * @param flags      type of operation
 * @param src        the vehicle (and possibly its trailing chain) to move
 * @param dst        the vehicle after which @p src should be inserted,
 *                   or nullptr to make @p src (and chain) a new standalone train
 * @param move_chain if true, move @p src and everything following it;
 *                   otherwise just @p src (and any of its articulated parts)
 * @param keep_absorbed_identity if true, an engine that stops being the head of
 *                   its own train keeps its orders, its number and its name
 *                   instead of having them thrown away -- for a coupling that
 *                   is meant to come apart again, where the engine is still the
 *                   train it was and is only travelling as part of another one.
 *                   Rearranging a train in a depot passes false: there the
 *                   engine really has stopped being a train of its own.
 * @return the cost of this operation or an error
 */
static CommandCost TryConsistSplice(DoCommandFlags flags, Train *src, Train *dst, bool move_chain, bool keep_absorbed_identity)
{
	/* locate the head of the two chains */
	Train *src_head = src->First();
	Train *dst_head = (dst != nullptr) ? dst->First() : nullptr;

	/* First make a backup of the order of the trains. That way we can do
	 * whatever we want with the order and later on easily revert. */
	TrainList original_src;
	TrainList original_dst;

	MakeTrainBackup(original_src, src_head);
	MakeTrainBackup(original_dst, dst_head);

	/* Also make backup of the original heads as ArrangeTrains can change them.
	 * For the destination head we do not care if it is the same as the source
	 * head because in that case it's just a copy. */
	Train *original_src_head = src_head;
	Train *original_dst_head = (dst_head == src_head ? nullptr : dst_head);

	/* We want this information from before the rearrangement, but execute this after the validation.
	 * original_src_head can't be nullptr; src is by definition != nullptr, so src_head can't be nullptr as
	 * src->GetFirst() always yields non-nullptr, so eventually original_src_head != nullptr as well. */
	bool original_src_head_front_engine = original_src_head->IsFrontEngine();
	bool original_dst_head_front_engine = original_dst_head != nullptr && original_dst_head->IsFrontEngine();

	/* (Re)arrange the trains in the wanted arrangement. */
	ArrangeTrains(&dst_head, dst, &src_head, src, move_chain);

	if (!flags.Test(DoCommandFlag::AutoReplace)) {
		/* If the autoreplace flag is set we do not need to test for the validity
		 * because we are going to revert the train to its original state. As we
		 * assume the original state was correct autoreplace can skip this. */
		CommandCost ret = ValidateTrains(original_dst_head, dst_head, original_src_head, src_head, true);
		if (ret.Failed()) {
			/* Restore the train we had. */
			RestoreTrainBackup(original_src);
			RestoreTrainBackup(original_dst);
			return ret;
		}
	}

	/* do it? */
	if (flags.Test(DoCommandFlag::Execute)) {
		/* Remove old heads from the statistics */
		if (original_src_head_front_engine) GroupStatistics::CountVehicle(original_src_head, -1);
		if (original_dst_head_front_engine) GroupStatistics::CountVehicle(original_dst_head, -1);

		/* First normalise the sub types of the chains. */
		NormaliseSubtypes(src_head);
		NormaliseSubtypes(dst_head);

		/* There are 14 different cases:
		 *  1) front engine gets moved to a new train, it stays a front engine.
		 *     a) the 'next' part is a wagon that becomes a free wagon chain.
		 *     b) the 'next' part is an engine that becomes a front engine.
		 *     c) there is no 'next' part, nothing else happens
		 *  2) front engine gets moved to another train, it is not a front engine anymore
		 *     a) the 'next' part is a wagon that becomes a free wagon chain.
		 *     b) the 'next' part is an engine that becomes a front engine.
		 *     c) there is no 'next' part, nothing else happens
		 *  3) front engine gets moved to later in the current train, it is not a front engine anymore.
		 *     a) the 'next' part is a wagon that becomes a free wagon chain.
		 *     b) the 'next' part is an engine that becomes a front engine.
		 *  4) free wagon gets moved
		 *     a) the 'next' part is a wagon that becomes a free wagon chain.
		 *     b) the 'next' part is an engine that becomes a front engine.
		 *     c) there is no 'next' part, nothing else happens
		 *  5) non front engine gets moved and becomes a new train, nothing else happens
		 *  6) non front engine gets moved within a train / to another train, nothing happens
		 *  7) wagon gets moved, nothing happens
		 */
		if (src == original_src_head && src->IsEngine() && !src->IsFrontEngine()) {
			/* Cases #2 and #3: the front engine gets trashed. */
			CloseWindowById(WindowClass::VehicleView, src->index);
			CloseWindowById(WindowClass::VehicleOrders, src->index);
			CloseWindowById(WindowClass::VehicleRefit, src->index);
			CloseWindowById(WindowClass::VehicleDetails, src->index);
			CloseWindowById(WindowClass::VehicleTimetable, src->index);
			DeleteNewGRFInspectWindow(GrfSpecFeature::Trains, src->index);
			SetWindowDirty(WindowClass::Company, _current_company);

			if (src_head != nullptr && src_head->IsFrontEngine()) {
				/* Cases #?b: Transfer order, unit number and other stuff
				 * to the new front engine. */
				src_head->orders = src->orders;
				if (src_head->orders != nullptr) src_head->AddToShared(src);
				src_head->CopyVehicleConfigAndStatistics(src);
			} else if (keep_absorbed_identity) {
				/* Two trains joining to run as one for a while, and this is the
				 * one that is going to travel as somebody else's wagons. It is
				 * still the train it was: it keeps its orders, its number and
				 * its name, and simply does nothing about any of them until it
				 * is put down again. Nothing reads them in the meantime -- only
				 * the head of a train is asked what it is supposed to be doing.
				 *
				 * Throwing them away is what made this impossible before. A
				 * number thrown away is a number handed out again to something
				 * else, so the train could not even come back as itself. See
				 * FEATURE_DESIGN_COUPLING_TOW.md. */
			} else {
				/* Remove stuff not valid anymore for non-front engines. */
				DeleteVehicleOrders(src);
				src->ReleaseUnitNumber();
				src->name.clear();
			}
		}

		/* We weren't a front engine but are becoming one. So
		 * we should be put in the default group. */
		if (original_src_head != src && dst_head == src) {
			SetTrainGroupID(src, DEFAULT_GROUP);
			SetWindowDirty(WindowClass::Company, _current_company);
		}

		/* Handle 'new engine' part of cases #1b, #2b, #3b, #4b and #5 in NormaliseTrainHead. */
		NormaliseTrainHead(src_head);
		NormaliseTrainHead(dst_head);

		/* Add new heads to statistics.
		 * This should be done after NormaliseTrainHead due to engine total limit checks in GetFreeUnitNumber. */
		if (src_head != nullptr && src_head->IsFrontEngine()) GroupStatistics::CountVehicle(src_head, 1);
		if (dst_head != nullptr && dst_head->IsFrontEngine()) GroupStatistics::CountVehicle(dst_head, 1);

		if (!flags.Test(DoCommandFlag::NoCargoCapacityCheck)) {
			CheckCargoCapacity(src_head);
			CheckCargoCapacity(dst_head);
		}

		if (src_head != nullptr) src_head->First()->MarkDirty();
		if (dst_head != nullptr) dst_head->First()->MarkDirty();

		/* Splitting a consist can leave a headless "free wagon" chain
		 * standing on ordinary reservable track (e.g. a station platform)
		 * instead of a depot -- something that never happens in vanilla,
		 * where free wagons only ever exist inside a depot (where track
		 * reservation is irrelevant, see the TRACK_BIT_DEPOT case in
		 * Train::ReserveTrackUnderConsist()). Nothing else in the engine
		 * actively maintains reservation under a stationary, engineless
		 * consist -- the only other caller of ReserveTrackUnderConsist()
		 * is crash handling, re-asserting it for exactly the same reason
		 * ("Crash() clears the reservation!"). Re-assert it here for both
		 * halves of the split so a PBS signal elsewhere can never read a
		 * decoupled train's tiles as free and route another train through
		 * it. See FEATURE_DESIGN_COUPLING_TOW.md. */
		if (src_head != nullptr) src_head->ReserveTrackUnderConsist();
		if (dst_head != nullptr) dst_head->ReserveTrackUnderConsist();

		/* Inside a depot a train has no extent -- every vehicle sits hidden on
		 * the one tile -- so which way its vehicles face is not a fact about
		 * where they are but a convention, and it is that convention, together
		 * with which end leads, that says which way the train will drive out.
		 * A vehicle put into a depot is faced the way the depot faces, which is
		 * the convention for a train that has not been turned round. Add one to
		 * a train that has, and the train ends up with vehicles facing both
		 * ways: the end that leads reads its own facing, gets the wrong answer,
		 * and drives at the back wall. Face every vehicle the way its head
		 * faces. */
		for (Train *head : {src_head, dst_head}) {
			if (head == nullptr || head->track != Track::Depot) continue;
			for (Train *u = head->Next(); u != nullptr; u = u->Next()) u->direction = head->direction;
		}

		/* A loading indicator belongs to the head of a consist and is taken
		 * down when that consist leaves the station. Splicing can retire a head
		 * without it ever leaving: coupling turns the other train's head into
		 * an ordinary vehicle in the middle of a load. Its indicator would then
		 * hang over the platform for the rest of the game, reading 0% at a
		 * train that has long since gone, with nothing left to update or remove
		 * it.
		 *
		 * Which vehicle is carrying that indicator cannot be worked out from
		 * the heads the splice was given. Joining two consists that lie
		 * opposite ways round relinks one of them back to front first, and that
		 * moves its head to the other end of the list -- so on a platform
		 * approached from one side the retired head is one of the heads named
		 * here, and on a platform approached from the other side it is a
		 * vehicle somewhere in the middle. Mirror-image platforms, and the
		 * indicator was only ever cleaned up on one of them. So walk the
		 * finished consists end to end and take down whatever is found;
		 * LoadUnloadVehicle() puts one back on the next tick for whichever
		 * consist is still loading.
		 *
		 * The same applies to the station's list of loading vehicles, which a
		 * consist comes off when it leaves. A retired head never leaves, and an
		 * ordinary vehicle left on that list is never given a new loading
		 * countdown, so the station trips over it on its next load cycle --
		 * assert(v->load_unload_ticks != 0) in LoadUnloadStation(), which is
		 * what crashed the game on coupling to a train that was still loading.
		 * Take it off the same way leaving does, cargo payment and reservation
		 * included. And a rake waiting to be collected has a window of its own
		 * (see IsWaitingWagonChain) which now has nothing left to show. */
		for (Train *head : {src_head, dst_head}) {
			if (head == nullptr) continue;
			for (Train *u = head->First(); u != nullptr; u = u->Next()) {
				HideFillingPercent(&u->fill_percent_te_id);

				/* A rake that has been collected is not waiting for anybody any
				 * more, so the engine written on it has to come off with
				 * everything else. Left there it is a claim on a vehicle that is
				 * no longer a rake at all -- harmless while it stays part of this
				 * train, but it comes back to life the day this vehicle is put
				 * down somewhere as a rake again, already spoken for by an engine
				 * that has long since finished with it. */
				u->couple_claim = VehicleID::Invalid();

				/* The one order a rake carried so the player could read and
				 * change it goes when the rake does. An engine keeps its own
				 * (see the note in TryConsistSplice); a rake has nothing to come
				 * back to. */
				if (u->IsFreeWagon() && u->orders != nullptr) DeleteVehicleOrders(u);

				if (u->IsFrontEngine() || u->IsFreeWagon()) continue;
				/* An engine travelling as somebody else's wagons keeps its
				 * orders, so it also keeps a "waiting to be collected" order
				 * that is no longer true. It has been collected. */
				u->current_order.SetWaitForCouple(false);

				if (Station::IsValidID(u->last_station_visited)) {
					Station *st = Station::Get(u->last_station_visited);
					st->loading_vehicles.remove(u);
					u->CancelReservation(StationID::Invalid(), st);
					delete u->cargo_payment;
					u->last_station_visited = StationID::Invalid();
				}
				CloseWindowById(WindowClass::VehicleView, u->index);
			}
		}

		/* We are undoubtedly changing something in the depot and train list. */
		InvalidateWindowData(WindowClass::VehicleDepot, src->tile);
		InvalidateWindowClassesData(WindowClass::TrainList, 0);
	} else {
		/* We don't want to execute what we're just tried. */
		RestoreTrainBackup(original_src);
		RestoreTrainBackup(original_dst);
	}

	return CommandCost();
}

/**
 * Is this train sitting somewhere on the line waiting for a rescue engine?
 *
 * Broken down or wrecked, still within the time it is prepared to wait, and
 * out on the network rather than tucked away in a depot where nothing needs
 * fetching. See TrainAwaitsRescue() for the waiting itself.
 *
 * @param v the train, front of its consist
 * @return whether it is a casualty an engine could be sent to
 */
bool IsWaitingToBeRescued(const Train *v)
{
	if (!v->IsFrontEngine()) return false;
	if (v->vehicle_flags.Test(VehicleFlag::RescueEngine)) return false;
	if (v->IsInDepot()) return false;
	if (v->breakdown_ctr != 1 && !v->vehstatus.Test(VehState::Crashed)) return false;
	if (v->rescue_deadline == TimerGameEconomy::Date{}) return false;
	return TimerGameEconomy::date < v->rescue_deadline;
}

/**
 * Is this rescue engine out on a call, either on its way to a casualty or
 * bringing one in?
 *
 * @param v the train, front of its consist
 * @return whether it is in the middle of a rescue
 */
bool IsOnRescueRun(const Train *v)
{
	return v->IsFrontEngine() && v->vehicle_flags.Test(VehicleFlag::RescueEngine) &&
			v->rescue_target != VehicleID::Invalid();
}

/**
 * Move a rake of wagons on to one of the orders it is carrying.
 *
 * A rake left at a platform carries two: the job the engine left it doing
 * here, and waiting to be collected. Nothing works through that list by itself
 * -- only the head of a train is asked what to do next, and a rake has no
 * engine at its head -- so both ways of moving between them, the loading
 * finishing and the player pressing Skip, come through here.
 *
 * What actually changes is the live order the rake is working on, since that is
 * what everything else reads. The list index is kept in step so the window
 * shows the right line.
 *
 * @param rake  the rake, its head
 * @param index which of its orders to take up
 */
void AdoptWagonRakeOrder(Train *rake, VehicleOrderID index)
{
	const Order *order = rake->GetOrder(index);
	if (order == nullptr) return;

	rake->cur_real_order_index = rake->cur_implicit_order_index = index;
	rake->current_order.SetWaitForCouple(order->ShouldWaitForCouple());
	rake->current_order.SetLoadType(order->GetLoadType());
	rake->current_order.SetUnloadType(order->GetUnloadType());
	if (order->ShouldWaitForCouple()) rake->vehicle_flags.Set(VehicleFlag::LoadingFinished);

	InvalidateVehicleOrder(rake, VIWD_MODIFY_ORDERS);
	InvalidateWindowData(WindowClass::VehicleView, rake->index);
}

/**
 * Is this consist standing where it is, waiting for somebody to come and
 * collect it?
 *
 * The flag on its own is not enough. It is set on an order well before the
 * train gets there, so a train still on its way to the station -- or sitting
 * in a depot about to set off -- would read as waiting and be coupled to on
 * the spot. That is how engines leaving a depot together ended up glued into
 * one train before any of them had gone anywhere.
 *
 * What makes it true is the flag plus being unable to go anywhere: either
 * standing in the station it was sent to, working through the order there, or
 * stranded -- broken down or wrecked. A stranded train is waiting to be
 * collected in the plainest sense there is, which is why a rescue engine needs
 * no machinery of its own to recognise one. See FEATURE_DESIGN_COUPLING_TOW.md.
 *
 * @param v any part of the consist
 * @return whether it is standing there to be collected
 */
bool IsWaitingToBeCoupled(const Train *v)
{
	const Train *head = v->First();
	if (!head->current_order.ShouldWaitForCouple()) return false;
	if (head->current_order.IsType(OT_LOADING)) return true;
	return head->breakdown_ctr == 1 || head->vehstatus.Test(VehState::Crashed);
}

/**
 * Is this train party to a coupling at all -- either the one that came to
 * collect, or the one standing there to be collected?
 *
 * Two entirely different pieces of code have to agree about this and they were
 * each written to their own list of conditions. One is the exception that says
 * a train pulling up against its partner is not a collision; the other is the
 * moment the coupling is actually performed. When the first says yes and the
 * second says no, the train neither crashes nor couples -- it simply carries on
 * through what it came for, which is what a rescue engine was seen to do to the
 * train it had been sent to fetch. They ask this now, so they cannot differ.
 *
 * @param v any part of the consist
 * @return whether a coupling is this train's business
 */
static bool IsPartyToACoupling(const Train *v)
{
	const Train *head = v->First();
	return head->current_order.ShouldGoToCouple() || head->couple_target != VehicleID::Invalid() ||
			IsOnRescueRun(head) || IsWaitingToBeCoupled(head);
}

/**
 * Has the engine that spoke for this rake stopped coming for it?
 *
 * A claim is only worth as much as the engine behind it. One that has been
 * sold, crashed, or given something else to do is not coming, and the rake it
 * spoke for would otherwise stand there claimed by a ghost for the rest of the
 * game with no engine able to touch it.
 *
 * @param rake the front of a headless rake
 * @return whether its claim should be let go
 */
static bool IsCoupleClaimStale(const Train *rake)
{
	if (rake->couple_claim == VehicleID::Invalid()) return false;

	const Train *claimer = Train::GetIfValid(rake->couple_claim);
	if (claimer == nullptr) return true;
	if (!claimer->IsFrontEngine()) return true;
	if (claimer->vehstatus.Test(VehState::Crashed)) return true;
	/* The two halves of the claim have to agree. If the engine no longer says
	 * it is coming for this rake -- it was given other orders, or it has
	 * already collected something -- then it is not coming. */
	return claimer->couple_target != rake->index;
}

/**
 * Does the rake @p rake answer the description the order @p order gives of
 * what it is going to collect?
 *
 * Three filters: how full the wagons are, what they carry, and how many of
 * them there are. Each narrows the choice further and each is free to be set
 * or left alone; every combination is allowed. Left alone, nothing is asked
 * and the first rake waiting will do.
 *
 * They pick which rake to fetch. They never pick part of one -- a rake either
 * answers and is collected whole, or it does not and is left where it stands.
 * Taking a train apart is what the decoupling order is for.
 *
 * @param order the order doing the collecting
 * @param rake  the front of a headless rake waiting to be collected
 * @return whether the rake is what the order asked for
 */
static bool MatchesCoupleFilter(const Order &order, const Train *rake)
{
	switch (order.GetCoupleLoad()) {
		case OrderCoupleLoad::Any:
			break;

		case OrderCoupleLoad::Empty:
			for (const Train *u = rake; u != nullptr; u = u->Next()) {
				if (u->cargo.StoredCount() != 0) return false;
			}
			break;

		case OrderCoupleLoad::Full:
			/* A rake the player has called done counts as full whatever is in
			 * it: it has finished what it was told to do here and nothing more
			 * is going into it. See CmdFinishWagonLoading(). */
			if (rake->current_order.GetLoadType() == OrderLoadType::NoLoad) break;
			/* Otherwise, room left anywhere means it is not full. A vehicle that
			 * carries nothing at all -- a brake van, say -- has no room either,
			 * so it neither makes a rake full nor stops it being full. */
			for (const Train *u = rake; u != nullptr; u = u->Next()) {
				if (u->cargo.StoredCount() < u->cargo_cap) return false;
			}
			break;

		default: NOT_REACHED();
	}

	if (IsValidCargoType(order.GetCoupleCargo())) {
		bool carries_it = false;
		for (const Train *u = rake; u != nullptr; u = u->Next()) {
			if (u->cargo_type == order.GetCoupleCargo() && u->cargo_cap != 0) {
				carries_it = true;
				break;
			}
		}
		if (!carries_it) return false;
	}

	if (order.GetCoupleCount() != 0) {
		uint count = 0;
		for (const Train *u = rake; u != nullptr; u = u->GetNextUnit()) count++;
		if (count != order.GetCoupleCount()) return false;
	}

	return true;
}

/**
 * Could @p v couple to @p partner, ignoring where either of them is?
 *
 * This is the single place that decides what counts as a valid coupling
 * partner; everything else that asks the question (whether a partner is
 * adjacent, whether one is somewhere on a platform, whether one can be
 * coupled to right now) goes through here, so the pathfinder can never
 * route to a spot where the coupling would then be refused, and the
 * coupling can never refuse a spot the pathfinder deliberately aimed for.
 * See FEATURE_DESIGN_COUPLING_TOW.md.
 *
 * @param v       the train looking to couple
 * @param partner candidate partner, front of its own consist (may be nullptr)
 * @return true if the two could be coupled
 */
static bool IsValidCouplePartner(const Train *v, const Train *partner)
{
	if (partner == nullptr) return false;
	if (partner == v->First()) return false; // that's us
	if (partner->owner != v->owner) return false;

	/* And this train has to be here for a coupling in the first place. Without
	 * that the question is only ever asked of trains that are, so nothing was
	 * asking it; now that the collision exception asks as well, it is asked of
	 * every train that comes near another one. An engine simply passing a rake
	 * of wagons is not collecting them. */
	if (!IsPartyToACoupling(v)) return false;

	/* A rescue engine sent to fetch a particular train couples to that train
	 * and to nothing else. What it is going to fetch is a casualty by
	 * definition -- broken down or wrecked, an engine at its head, none of
	 * which describes wagons waiting to be collected -- so it is answered here
	 * on its own terms and none of the ordinary conditions apply to it. */
	if (v->First()->rescue_target == partner->index) return IsOnRescueRun(v->First());

	if (partner->vehstatus.Test(VehState::Crashed)) return false;
	if (partner->cur_speed != 0) return false;
	/* A train that has anything to do with a depot is not standing anywhere to
	 * be coupled to. One on its way out is halfway between two worlds, and one
	 * inside has no position at all -- grabbing either by the tail drags it
	 * back into the depot, brake and all, which is what was seen to happen to a
	 * train reversing in while engines came past under a coupling order. */
	if (IsAnyPartInsideDepot(partner)) return false;

	/* And it has to be something that is actually waiting to be collected: a
	 * rake of wagons with no engine, left at a platform under a "wait to be
	 * coupled" order.
	 *
	 * Without this, any train of ours standing still anywhere counted as a
	 * partner -- an engine that has just finished collecting its wagons, or one
	 * sitting broken down on the line. An engine on its way to couple then
	 * drove at it and the two collided, because the exception that says "this
	 * is not a crash, this is what I came for" only covers a headless rake.
	 * Worse, the collecting engine kept seeing a partner in wagons that had
	 * already been collected by somebody else, so it never gave up on them. */
	if (!IsWaitingToBeCoupled(partner)) return false;
	/* Either a rake of wagons or a whole train of its own that is waiting to be
	 * picked up and carried along as part of a bigger one. Two little trains
	 * joining to run as one is the same arrangement as an engine collecting
	 * wagons, seen from the other end. */
	if (!partner->IsFreeWagon() && !partner->IsFrontEngine()) return false;

	/* Once this train has chosen what it is going for, nothing else will do.
	 * Without that, the route search would happily settle on some other rake
	 * that also passes the filter, and the train would end up driving to
	 * whatever it had reserved instead of to what it had picked. */
	const Train *head = v->First();
	if (head->couple_target != VehicleID::Invalid()) return partner->index == head->couple_target;

	/* And it must not already be somebody else's errand. */
	if (partner->couple_claim != VehicleID::Invalid() && partner->couple_claim != head->index &&
			!IsCoupleClaimStale(partner)) {
		return false;
	}

	return MatchesCoupleFilter(head->current_order, partner);
}

/**
 * Find the rake a train under a "go to couple" order is going to fetch,
 * speaking for it if nobody has yet.
 *
 * An engine sent to collect wagons has nowhere to stop once it has set off --
 * reaching them is the whole route, and the rules that let it pull up against
 * them are the rules that let it past the signals guarding the platform. It
 * has nowhere else to go either, since the order names wagons and not a place
 * to be. So two engines sent to the same rake means one of them arriving to
 * find nothing, with no way to stop and nothing to do instead. The first to
 * want a rake speaks for it, and it is offered to nobody else until that
 * engine has it or has given up.
 *
 * @param v the train, front of its consist, on a "go to couple" order
 * @return the rake it is to fetch, or nullptr if there is nothing for it
 */
static Train *FindOrClaimCoupleTarget(Train *v)
{
	StationID dest = v->current_order.GetDestination().ToStationID();
	Train *unclaimed = nullptr;

	for (Train *rake : Train::Iterate()) {
		/* Wagons waiting to be collected, or a whole little train waiting to be
		 * carried along as part of a bigger one. */
		if (!rake->IsFreeWagon() && !rake->IsFrontEngine()) continue;
		if (rake == v) continue;
		if (rake->owner != v->owner) continue;
		if (!IsWaitingToBeCoupled(rake)) continue;
		if (rake->last_station_visited != dest) continue;

		if (IsCoupleClaimStale(rake)) rake->couple_claim = VehicleID::Invalid();

		if (rake->couple_claim == v->index) {
			v->couple_target = rake->index; // already ours
			return rake;
		}
		if (rake->couple_claim != VehicleID::Invalid()) continue; // somebody else's
		if (!MatchesCoupleFilter(v->current_order, rake)) continue;

		if (unclaimed == nullptr) unclaimed = rake;
	}

	if (unclaimed != nullptr) {
		unclaimed->couple_claim = v->index;
		v->couple_target = unclaimed->index;
	} else {
		v->couple_target = VehicleID::Invalid();
	}
	return unclaimed;
}

/**
 * Is there a rake waiting for this train to come and fetch it?
 *
 * Asks the question without answering it in the affirmative for anything that
 * is not already this train's, so it can be asked from anywhere -- the vehicle
 * window, for one -- without quietly speaking for a rake as a side effect.
 *
 * @param v the train, front of its consist
 * @return whether it has something to go and collect
 */
bool HasCoupleTarget(const Train *v)
{
	if (!v->current_order.ShouldGoToCouple()) return false;

	StationID dest = v->current_order.GetDestination().ToStationID();
	for (const Train *rake : Train::Iterate()) {
		if (rake->couple_claim != v->index) continue;
		if (!rake->IsFreeWagon() && !rake->IsFrontEngine()) continue;
		if (!IsWaitingToBeCoupled(rake)) continue;
		if (rake->last_station_visited != dest) continue;
		return true;
	}
	return false;
}

/**
 * Find the train, if any, standing on @p tile that @p v could couple to.
 *
 * @param v    the train looking to couple
 * @param tile the tile to inspect
 * @return the front of the partner train on that tile, or nullptr
 */
static Train *FindCouplePartnerOnTile(const Train *v, TileIndex tile)
{
	if (tile == INVALID_TILE) return nullptr;

	for (Vehicle *u : VehiclesOnTile(tile)) {
		if (u->type != VehicleType::Train) continue;
		Train *partner = Train::From(u)->First();
		if (IsValidCouplePartner(v, partner)) return partner;
	}
	return nullptr;
}

/**
 * Find the train, if any, sitting on the tile immediately beyond @p tile in
 * the direction @p td exits towards, that @p v could couple to.
 *
 * A tile occupied by another train can never itself be reserved, so a
 * reservation walk that stops at @p tile (the last tile it could reserve)
 * always stops one tile short of a partner standing on open track just
 * beyond it -- this checks that next tile explicitly. See
 * #FindCouplePartnerOnTile and FEATURE_DESIGN_COUPLING_TOW.md.
 *
 * @param v    the train looking to couple
 * @param tile tile the search has reached
 * @param td   the trackdir the search is facing on that tile
 * @return the front of the partner train on the adjacent tile, or nullptr
 */
static Train *FindCouplePartnerOnAdjacentTile(const Train *v, TileIndex tile, Trackdir td)
{
	if (tile == INVALID_TILE) return nullptr;

	return FindCouplePartnerOnTile(v, TileAddByDiagDir(tile, TrackdirToExitdir(td)));
}

/**
 * Is a train @p v could couple to standing anywhere on the station platform
 * that @p tile is part of?
 *
 * A "go to couple" order can only name a station, but a station usually has
 * several platforms and only one of them has the partner on it. The
 * pathfinder's destination test (#CYapfDestinationTileOrStationRailT in
 * yapf_destrail.hpp) uses this to accept only that one platform, instead of
 * the first reservable platform of the station it happens to reach -- which
 * is what made a go-to-couple train drive to an empty platform and then sit
 * there with no partner ever becoming adjacent.
 *
 * The whole platform is checked rather than just the single tile the search
 * stopped on, because the track follower skips across a station platform in
 * one step: the tile the search reports is the far end of the platform, not
 * the tile next to the wagons standing part-way along it. See
 * FEATURE_DESIGN_COUPLING_TOW.md.
 *
 * @param v    the train looking to couple
 * @param tile any tile of the platform to inspect
 * @return true if a valid partner stands somewhere on that platform
 */
/**
 * Is the casualty this rescue engine was called out to standing on this tile?
 *
 * A casualty is wherever it broke down, which is out on the open line and not
 * at a station, so the platform rule that lets a collecting engine pull up
 * against wagons has nothing to say about it. The same reasoning does: the
 * thing in the way is the thing that was sent for, so stopping short of it is
 * the end of the journey and not a place to be turned away from.
 *
 * @param v    the train asking, any part of it
 * @param tile the tile being considered as somewhere to stop
 * @return whether that tile is where the casualty is
 */
bool IsRescueTargetOnTile(const Train *v, TileIndex tile)
{
	const Train *tow = v->First();
	if (!IsOnRescueRun(tow)) return false;

	const Train *casualty = Train::GetIfValid(tow->rescue_target);
	if (casualty == nullptr || casualty->First() == tow) return false;

	for (const Train *u = casualty->First(); u != nullptr; u = u->Next()) {
		if (u->tile == tile) return true;
	}
	return false;
}

bool IsCouplePartnerOnPlatform(const Train *v, TileIndex tile)
{
	if (!IsRailStationTile(tile)) return false;

	TileIndexDiff delta = TileOffsByAxis(GetRailStationAxis(tile));

	if (FindCouplePartnerOnTile(v, tile) != nullptr) return true;
	for (TileIndex t = tile + delta; IsRailStationTile(t) && IsCompatibleTrainStationTile(t, tile); t += delta) {
		if (FindCouplePartnerOnTile(v, t) != nullptr) return true;
	}
	for (TileIndex t = tile - delta; IsRailStationTile(t) && IsCompatibleTrainStationTile(t, tile); t -= delta) {
		if (FindCouplePartnerOnTile(v, t) != nullptr) return true;
	}
	return false;
}

/**
 * Look for a coupling partner off one end of @p v: first along whatever
 * reservation leads away from that end, then on the tile immediately beyond
 * where that reservation stops.
 *
 * The second step is not a fallback for exotic cases, it is the normal one.
 * A reservation walk that finds no other train very often just arrives back
 * at @p v itself, because a train parked against a partner holds no
 * reservation past the tile it is standing on -- the next tile is occupied,
 * and an occupied tile cannot be reserved. "The walk came back to me" must
 * therefore mean "keep looking", not "there is nobody there"; treating it as
 * an answer is what stopped a train that had driven right up to its wagons
 * from ever coupling to them. See FEATURE_DESIGN_COUPLING_TOW.md.
 *
 * @param v         the train looking to couple
 * @param from_rear search off the rear of the consist instead of the front
 * @return the front of the partner train, or nullptr
 */
static Train *FindCouplePartnerAlongReservation(const Train *v, bool from_rear)
{
	Vehicle *train_on_res = nullptr;
	PBSTileInfo res = FollowTrainReservation(v, &train_on_res, from_rear);

	if (train_on_res != nullptr && train_on_res->type == VehicleType::Train) {
		Train *partner = Train::From(train_on_res)->First();
		if (IsValidCouplePartner(v, partner)) return partner;
	}

	return FindCouplePartnerOnAdjacentTile(v, res.tile, res.trackdir);
}

/**
 * Find the train, if any, that a stopped train @p v could couple to:
 * another stopped train immediately adjacent to it on the track, either
 * ahead of or behind @p v.
 *
 * This deliberately does not compare x_pos/y_pos pixel distances (see
 * FEATURE_DESIGN_COUPLING_TOW.md, "Bug C" in the reference patch that
 * inspired this feature) — it works purely in tiles and trackdirs, which is
 * exact and independent of rounding. See #FindCouplePartnerAlongReservation
 * for how each end is searched.
 *
 * Both directions are checked (front first, then rear) because which way
 * either train happens to be facing shouldn't matter for coupling -- real
 * locomotives are frequently bidirectional, and requiring the player to
 * manually turn a train around to match a specific facing just to trigger
 * a couple is exactly the kind of friction this feature exists to remove.
 * Meeting nose-to-nose from opposite directions (i.e. neither train's
 * front or rear is adjacent to the other, they're pointed at each other
 * mid-track) is intentionally still out of scope; see the design doc for
 * the follow-up plan.
 *
 * @param v the train looking to couple; must be the front of its own consist
 * @param partner_is_behind if non-null, set to true if the returned partner
 *        was found behind @p v (partner's front adjacent to v's rear)
 *        rather than ahead of it (the more common case)
 * @return the front of the adjacent train that @p v can couple to, or
 *         nullptr if there is none
 */
Train *GetTrainCouplePartner(const Train *v, bool *partner_is_behind)
{
	if (!v->IsFrontEngine()) return nullptr;
	if (v->vehstatus.Test(VehState::Crashed)) return nullptr;
	if (v->cur_speed != 0) return nullptr;

	bool behind = false;
	Train *partner = FindCouplePartnerAlongReservation(v, false);
	if (partner == nullptr) {
		partner = FindCouplePartnerAlongReservation(v, true);
		behind = true;
	}
	if (partner == nullptr) return nullptr;

	if (partner_is_behind != nullptr) *partner_is_behind = behind;
	return partner;
}

/**
 * Is @p b close enough to @p a that coupling them should pull the two together,
 * rather than drag vehicles across the map?
 *
 * A train stops short of its partner by roughly a tile, because it cannot
 * reserve the tile the partner stands on, so some gap is normal and expected.
 * This only rejects the absurd, and is deliberately generous.
 *
 * @param a One consist.
 * @param b The other consist.
 * @return true if the two are near enough to be closed up.
 */
static bool AreConsistsCloseEnoughToCouple(const Train *a, const Train *b)
{
	for (const Train *u = a; u != nullptr; u = u->Next()) {
		for (const Train *w = b; w != nullptr; w = w->Next()) {
			if (abs(u->z_pos - w->z_pos) > 8) continue;

			int x_diff = u->x_pos - w->x_pos;
			int y_diff = u->y_pos - w->y_pos;
			int reach = 2 * TILE_SIZE;
			if (x_diff * x_diff + y_diff * y_diff <= reach * reach) return true;
		}
	}
	return false;
}

/**
 * Pull a freshly coupled part up against the rest of its train, closing the gap
 * the coupling was made across.
 *
 * Splicing two consists together moves nothing, so the join is only sound if
 * they were already touching -- and they never are. A train cannot reserve the
 * tile its partner occupies, so it stops about a tile short, and joining there
 * produces a train with a hole in it. A wagon behind that hole then has to work
 * out which track connects it to the vehicle ahead, finds none, and the game
 * asserts a few tiles later. So the hole has to be walked shut.
 *
 * This mirrors #AdvanceWagonsAfterSwap, which closes the same kind of spacing
 * gap after a consist is rearranged, but is kept as its own copy rather than
 * bent to fit: that one computes an exact differential from vehicle lengths for
 * a train whose parts are already snug, which is not the situation here, and
 * TrainController() is used by the whole game and is no place for our special
 * case. See FEATURE_DESIGN_COUPLING_TOW.md.
 *
 * @param consist The merged train.
 */
static void CloseUpCoupledConsist(Train *consist)
{
	/* Bounded well above the tile-and-a-bit any real gap can be, purely so a
	 * consist that somehow refuses to close cannot spin here forever. */
	for (uint step = 0; step < 4 * TILE_SIZE; step++) {
		/* Find the first place the train is further apart than it should be.
		 * Walking in movement order rather than chain order matters: the two
		 * run opposite ways round for a train that is driving backwards, and
		 * TrainController() moves a vehicle and everything behind it in
		 * movement order -- hand it the wrong end and it drags the wrong half
		 * of the train. Most couplings happen to a reversing train, so that is
		 * the common case, not the exception. */
		Train *behind_gap = nullptr;
		for (Train *u = consist->GetMovingFront(); u != nullptr; u = u->GetMovingNext()) {
			Train *next = u->GetMovingNext();
			if (next == nullptr) break;

			int x_diff = u->x_pos - next->x_pos;
			int y_diff = u->y_pos - next->y_pos;
			int want = u->CalcNextVehicleOffset();
			if (x_diff * x_diff + y_diff * y_diff > want * want) {
				behind_gap = next;
				break;
			}
		}
		if (behind_gap == nullptr) break;

		if (!TrainController(behind_gap, nullptr)) break;
	}

	consist->ConsistChanged(CCF_TRACK);
	consist->UpdateViewport(true, true);
}

/**
 * Squared pixel distance between two vehicles, used only to compare which of
 * two ends of a consist is the nearer one.
 */
static int64_t DistanceSquaredBetweenVehicles(const Train *a, const Train *b)
{
	int64_t x_diff = a->x_pos - b->x_pos;
	int64_t y_diff = a->y_pos - b->y_pos;
	return x_diff * x_diff + y_diff * y_diff;
}

/**
 * Reverse the order of a consist's vehicle list, leaving every vehicle exactly
 * where it stands.
 *
 * A train's vehicle list has to run the same way along the track as its
 * vehicles physically lie, so joining two consists that lie the opposite way
 * round to each other needs one of them turned round. There are two ways to do
 * that, and only one of them is right here. Vanilla's #ReverseTrainSwapVehicles
 * keeps the list order and swaps where the vehicles sit -- first with last,
 * second with second-last -- which turns the train round on the ground. Doing
 * that to a rake of wagons waiting at a platform is absurd: the wagons visibly
 * swap places with each other while standing still, and an engine that decides
 * anything by it is letting the wagons dictate to it.
 *
 * So this does the mirror image. Nothing moves; the list is relinked back to
 * front, so it runs the other way while every vehicle stays put. Articulated
 * parts and the two halves of a dual-headed engine keep their own order inside
 * the unit they belong to, which is what GetNextVehicle() and
 * GetLastEnginePart() are for.
 *
 * @param head Head of the consist to relink.
 * @return The new head, which is what used to be the last unit.
 */
static Train *ReverseConsistOrder(Train *head)
{
	std::vector<Train *> units;
	for (Train *u = head; u != nullptr; u = u->GetNextVehicle()) units.push_back(u);
	if (units.size() < 2) return head;

	for (Train *u : units) RemoveFromConsist(u);

	Train *new_head = units.back();
	for (size_t i = units.size() - 1; i > 0; i--) {
		InsertInConsist(units[i]->GetLastEnginePart(), units[i - 1]);
	}
	return new_head;
}

/**
 * Bring every vehicle's recorded facing into line with the chain it now sits
 * in, after two consists have been joined.
 *
 * Within one train each vehicle's direction points from the back of the train
 * towards its front, i.e. from a vehicle towards the one before it in the
 * list. Two consists that met never agreed on that: an engine that drove up to
 * a rake nose first is facing the exact opposite way to the wagons it has just
 * been joined to. Left alone that is a train whose halves disagree about which
 * way is forwards, and the movement code has no way to make sense of it.
 *
 * Reversing such a vehicle's direction is only a change of bookkeeping -- it
 * has not turned round, it is standing still -- so its Flipped flag is toggled
 * to match. GetImage() reverses the direction again for a flipped vehicle, so
 * the picture on screen does not change at all; what changes is that the train
 * now agrees with itself. And that flag is exactly the truth of the matter: an
 * engine coupled to its wagons the other way round is an engine running
 * flipped, which is also what makes it visible as such in the depot list.
 *
 * @param consist Head of the freshly joined consist.
 */
static void NormaliseCoupledConsistFacing(Train *consist)
{
	for (Train *u = consist; u != nullptr; u = u->Next()) {
		/* Line each vehicle up against the one ahead of it in the list. The
		 * head has none, so it uses the one behind it and the opposite sense:
		 * its facing points away from the body of the train. */
		const Train *ahead = u->Previous();
		const Train *reference = (ahead != nullptr) ? ahead : u->Next();
		if (reference == nullptr) break; // A single vehicle has nothing to disagree with.

		int towards_x = reference->x_pos - u->x_pos;
		int towards_y = reference->y_pos - u->y_pos;
		if (ahead == nullptr) {
			towards_x = -towards_x;
			towards_y = -towards_y;
		}

		/* Only a facing that points the opposite way is wrong. A vehicle on a
		 * curve is up to 45 degrees off the line to its neighbour and is
		 * perfectly correct, so compare the two as directions and not for
		 * equality. */
		TileIndexDiffC facing = TileIndexDiffCByDir(u->direction);
		if (facing.x * towards_x + facing.y * towards_y >= 0) continue;

		u->direction = ReverseDir(u->direction);
		u->flags.Flip(VehicleRailFlag::Flipped);
	}
}

/** How long a casualty waits to be fetched before sorting itself out the vanilla way. */
static constexpr int RESCUE_DEADLINE_DAYS = EconomyTime::DAYS_IN_ECONOMY_YEAR / 2;

/**
 * Should this broken-down train stay broken down and wait to be fetched?
 *
 * A vanilla breakdown fixes itself after a short delay, which leaves nothing
 * for a rescue engine to be sent to. A train that is waiting for one therefore
 * stays broken until it is fetched -- but not for ever: if no rescue engine
 * can reach it, or the player has none, the deadline runs out and the
 * breakdown clears itself as it always did, so a line can never be blocked
 * permanently by a feature the player did not set up. The deadline is measured
 * from the breakdown rather than from a rescue engine setting off, so a
 * casualty nobody is coming for is not stuck waiting for a call-out that will
 * never happen. See FEATURE_DESIGN_COUPLING_TOW.md.
 *
 * @param v The broken-down train (its head).
 * @return true if the breakdown should be kept.
 */
bool TrainAwaitsRescue(Train *v)
{
	v = v->First();

	/* A rescue engine that breaks down on its way to a casualty is not a
	 * casualty waiting for itself. It fixes itself the vanilla way. */
	if (v->vehicle_flags.Test(VehicleFlag::RescueEngine)) return false;

	if (v->rescue_deadline == TimerGameEconomy::Date{}) {
		v->rescue_deadline = TimerGameEconomy::date + RESCUE_DEADLINE_DAYS;
		SetWindowDirty(WindowClass::VehicleView, v->index);
	}

	/* A train stranded on the line is waiting to be collected, in the plainest
	 * sense of the words, so it says so on its order and needs nothing else. An
	 * engine coming to fetch it then treats it exactly as it treats wagons
	 * waiting at a platform -- same partner test, same approach, same coupling
	 * -- and rescue stops being a case of its own.
	 *
	 * Nothing about this shows in the window: what a vehicle is reported as
	 * doing puts a breakdown and a wreck ahead of any order, so it goes on
	 * saying broken down or crashed, which is what the player needs to see. */
	if (!v->current_order.ShouldWaitForCouple()) {
		v->current_order.SetWaitForCouple(true);
		v->current_order.SetGoToCouple(false);
	}

	if (TimerGameEconomy::date < v->rescue_deadline) {
		/* Vanilla puffs smoke once, for as long as the breakdown was going to
		 * last. A breakdown that now lasts until someone comes would go quiet
		 * long before that, and a silent stationary train in the middle of a
		 * line tells the player nothing about why it is there. Keep it
		 * smoking. */
		if (!v->vehstatus.Test(VehState::Hidden) && (v->tick_counter & 0xFF) == 0 &&
				!EngInfo(v->engine_type)->misc_flags.Test(EngineMiscFlag::NoBreakdownSmoke)) {
			/* Each puff counts its own animation_state down and deletes
			 * itself when it reaches zero. Leaving it at zero does not mean
			 * "no time", it means the very first decrement goes below zero and
			 * wraps, so the puff never expires -- which is why the smoke
			 * outlived the breakdown and hung over the track for good. Give it
			 * a life a little longer than the gap between puffs, so the trail
			 * is continuous while the wait lasts and gone shortly after. */
			EffectVehicle *smoke = CreateEffectVehicleRel(v, 4, 4, 5, EV_BREAKDOWN_SMOKE);
			if (smoke != nullptr) smoke->animation_state = 0x140;
		}
		return true;
	}

	/* Waited long enough. Give up on being fetched and let vanilla run its
	 * course, which is what clears the line.
	 *
	 * The deadline deliberately stays where it is rather than being cleared.
	 * Clearing it would read as "nothing is wrong here" to the next call a
	 * moment later, which would set a fresh deadline and start the wait over:
	 * a crashed train would lose one wagon every half year instead of one
	 * every few ticks, which is indistinguishable from never. It is cleared
	 * where the trouble actually ends -- the breakdown lifting, or the depot
	 * putting the train right. */
	return false;
}

/**
 * Let go of a call-out, from either end.
 *
 * An errand is written down twice -- the engine says what it is going for and
 * the casualty says who is coming -- so ending it means rubbing out both.
 * Leaving half of it behind leaves an engine that is no longer coming for
 * anything still recognised as coming for it: it would go on being excused from
 * crashing into that train, and would still couple to it if it happened to
 * touch it. And the casualty would go on being spoken for by an engine that has
 * been given something else to do, so nobody else would be sent.
 *
 * One place does it, because there is more than one way to end an errand: the
 * player stands the engine down, or writes it orders, and both mean the same
 * thing here.
 *
 * @param tow the rescue engine, front of its consist
 */
void EndRescueErrand(Train *tow)
{
	Train *casualty = Train::GetIfValid(tow->rescue_target);
	if (casualty != nullptr && casualty->couple_claim == tow->index) casualty->couple_claim = VehicleID::Invalid();

	tow->rescue_target = VehicleID::Invalid();
	tow->couple_target = VehicleID::Invalid();
}

/**
 * Station a train in the depot it is standing in as a rescue engine, or stand
 * it back down again.
 *
 * A rescue engine is not doing a job, it is on call: it waits in its depot
 * until something breaks down or crashes, goes and fetches it, and comes back
 * here. That is a standing arrangement rather than an order, which is why it
 * is set once from the vehicle window and stays set, and why a train that has
 * orders of its own cannot be one -- it would have two contradictory ideas of
 * where it ought to be. See FEATURE_DESIGN_COUPLING_TOW.md.
 *
 * @param flags type of operation
 * @param veh_id the train to station (or stand down)
 * @param rescue whether it is to be a rescue engine
 * @return the cost of this operation or an error
 */
CommandCost CmdSetRescueEngine(DoCommandFlags flags, VehicleID veh_id, bool rescue)
{
	Train *v = Train::GetIfValid(veh_id);
	if (v == nullptr || !v->IsFrontEngine()) return CMD_ERROR;

	CommandCost ret = CheckOwnership(v->owner);
	if (ret.Failed()) return ret;

	if (!rescue) {
		/* One that has a casualty coupled on behind it has to see the job
		 * through. Letting go of it here would leave the casualty merged into
		 * this train with nothing left that knows to put it down again -- and
		 * the train it used to be would never come back. It can be stood down
		 * on the way out, before it has picked anything up, and it can be stood
		 * down once it has handed its load over. */
		const Train *in_tow = Train::GetIfValid(v->rescue_target);
		if (in_tow != nullptr && in_tow != v && in_tow->First() == v) {
			return CommandCost(STR_ERROR_RESCUE_ENGINE_IS_TOWING);
		}
	}

	if (rescue) {
		/* Standing one down is otherwise always allowed, whatever state it is
		 * in -- otherwise a train could get stuck being a rescue engine. Only
		 * taking the job on has conditions. */
		if (!v->IsInDepot() || !v->vehstatus.Test(VehState::Stopped)) return CommandCost(STR_ERROR_RESCUE_ENGINE_NOT_IN_DEPOT);
		if (v->GetNumOrders() != 0) return CommandCost(STR_ERROR_RESCUE_ENGINE_HAS_ORDERS);
		/* It has to leave at a moment's notice and bring something back, so it
		 * cannot already be pulling something: wagons rule it out exactly as
		 * orders do. */
		if (v->GetNextUnit() != nullptr) return CommandCost(STR_ERROR_RESCUE_ENGINE_HAS_WAGONS);
	}

	if (flags.Test(DoCommandFlag::Execute)) {
		v->vehicle_flags.Set(VehicleFlag::RescueEngine, rescue);
		/* Where it lives, and so where it comes back to. Taking it from
		 * where the train is standing rather than from an order means a
		 * player moves a rescue engine simply by driving it to another
		 * depot and stationing it there. */
		v->rescue_home_depot = rescue ? v->tile : INVALID_TILE;
		/* Standing one down in the middle of a call-out lets go of the errand.
		 * One that is already towing cannot get here at all -- see above. */
		EndRescueErrand(v);

		/* The vehicle window works out which of its buttons are lowered in
		 * UpdateButtons(), which only runs on an invalidate. Merely marking
		 * the window dirty repaints it exactly as it was, so the button would
		 * spring back up and pressing it would look like it did nothing. */
		InvalidateWindowData(WindowClass::VehicleView, v->index);
		SetWindowDirty(WindowClass::VehicleDepot, v->tile);
		SetWindowClassesDirty(WindowClass::TrainList);
	}

	return CommandCost();
}

/**
 * Is @p tile a rail station tile belonging to station @p station?
 */
static bool IsRailStationTileOfStation(TileIndex tile, StationID station)
{
	return IsRailStationTile(tile) && GetStationIndex(tile) == station;
}

/**
 * Is any part of @p consist standing on a platform of station @p station?
 * Being merely near it does not count; the vehicle has to be on the station's
 * own tiles.
 */
static bool IsConsistStandingAtStation(const Train *consist, StationID station)
{
	for (const Train *u = consist; u != nullptr; u = u->Next()) {
		if (IsRailStationTileOfStation(u->tile, station)) return true;
	}
	return false;
}

/**
 * Send an idle rescue engine out to the nearest train that is waiting to be
 * fetched.
 *
 * Called from the engine's own tick while it stands on call in its depot, so
 * the work of looking scales with the number of rescue engines a player has
 * set up -- which is a handful -- rather than with the number of things that
 * could go wrong.
 *
 * It is given no orders. A casualty is a place on the map and not a station,
 * and an order names a station; what it is given instead is the tile, and
 * being on a call is what keeps that tile from being wiped by the order code
 * (see ProcessOrders). Two engines are never sent to the same casualty.
 *
 * @param tow the rescue engine, front of its consist
 */
static void TryDispatchRescueEngine(Train *tow)
{
	if (tow->rescue_target != VehicleID::Invalid()) return;
	if (!tow->IsInDepot()) return;
	/* Looking means reading every train in the game twice over, and a train
	 * that has just broken down is in no hurry. Once every few seconds is
	 * often enough, and the counter is the train's own, so every client works
	 * it out on the same tick. */
	if ((tow->tick_counter & 0x3F) != 0) return;
	/* Brake off is what puts one on call; a rescue engine standing with its
	 * brake on is parked, not waiting. */
	if (tow->vehstatus.Test(VehState::Stopped)) return;
	if (tow->GetNumOrders() != 0) return;

	Train *nearest = nullptr;
	uint nearest_distance = UINT_MAX;
	for (Train *casualty : Train::Iterate()) {
		if (casualty->owner != tow->owner) continue;
		if (!IsWaitingToBeRescued(casualty)) continue;

		/* Somebody else's call-out. */
		bool taken = false;
		for (const Train *other : Train::Iterate()) {
			if (other->rescue_target == casualty->index) {
				taken = true;
				break;
			}
		}
		if (taken) continue;

		uint distance = DistanceManhattan(tow->tile, casualty->tile);
		if (distance < nearest_distance) {
			nearest = casualty;
			nearest_distance = distance;
		}
	}

	if (nearest == nullptr) return;

	tow->rescue_target = nearest->index;
	/* Spoken for, in the same words every other coupling uses: the casualty
	 * says which engine is coming and the engine says what it is going for. */
	tow->couple_target = nearest->index;
	nearest->couple_claim = tow->index;
	tow->SetDestTile(nearest->tile);
	tow->current_order.MakeDummy();
	InvalidateWindowData(WindowClass::VehicleView, tow->index);
	SetWindowDirty(WindowClass::VehicleDepot, tow->tile);
}

/**
 * Deal with a rescue engine that has just come to a stand in a depot.
 *
 * Two things bring one here. It has towed a casualty in, in which case the
 * casualty is put down: repaired and sent on its way again where it left off,
 * or, if it was a wreck, scrapped -- a depot is where a wreck stops being
 * something the line has to work around. Or it has simply got itself home
 * afterwards, in which case it goes back on call.
 *
 * @param tow the train that has entered the depot, front of its consist
 */
void HandleRescueEngineInDepot(Train *tow)
{
	if (!tow->IsFrontEngine()) return;
	if (!tow->vehicle_flags.Test(VehicleFlag::RescueEngine)) return;

	if (tow->rescue_target == VehicleID::Invalid()) {
		/* Home again with nothing in tow: back on call. Entering a depot always
		 * stops a vehicle, and a stopped rescue engine reads as parked rather
		 * than waiting, so let the brake off again. */
		if (tow->tile == tow->rescue_home_depot) {
			tow->current_order.MakeDummy();
			tow->vehstatus.Reset(VehState::Stopped);
			InvalidateWindowData(WindowClass::VehicleView, tow->index);
		}
		return;
	}

	Train *casualty = Train::GetIfValid(tow->rescue_target);
	EndRescueErrand(tow);

	/* Nothing was ever picked up -- the casualty was sold, or sorted itself out
	 * before this engine got there. Nothing to put down. */
	if (casualty != nullptr && casualty->First() == tow && casualty != tow) {
		bool wrecked = casualty->vehstatus.Test(VehState::Crashed);

		/* Split it back off. It is standing in a depot, which is where taking
		 * trains apart is an ordinary thing to do. */
		TryConsistSplice(DoCommandFlag::Execute, casualty, nullptr, true);

		/* If for any reason it would not come apart, leave it exactly where it
		 * is rather than acting on a train that is still half of another one.
		 * Scrapping from the middle of a consist, or handing orders to a
		 * vehicle that is not the head of anything, does lasting damage. */
		if (casualty->First() != casualty) return;

		if (wrecked) {
			/* A wreck brought into a depot is scrapped there. */
			delete casualty;
			casualty = nullptr;
		} else {
			/* Whatever was wrong with it is put right, as a depot puts anything
			 * right, and it carries on from the order it had got to. It has had
			 * its own orders the whole way -- being towed does not take a train's
			 * orders off it, see TryConsistSplice. */
			casualty->breakdown_ctr = 0;
			casualty->breakdown_delay = 0;
			casualty->rescue_deadline = TimerGameEconomy::Date{};
			/* Collected and put right, so it is no longer waiting for anybody. */
			casualty->current_order.SetWaitForCouple(false);
			casualty->vehstatus.Reset(VehState::Stopped);
			casualty->ConsistChanged(CCF_ARRANGE);
			InvalidateWindowData(WindowClass::VehicleView, casualty->index);
		}
	}

	/* Home if this is not home, otherwise straight back on call. Named
	 * explicitly rather than asking for the nearest depot: it lives in one
	 * particular depot, that is where the player put it, and the nearest one is
	 * the one it is standing in. */
	if (tow->tile != tow->rescue_home_depot && IsRailDepotTile(tow->rescue_home_depot)) {
		tow->current_order.MakeGoToDepot(GetDepotIndex(tow->rescue_home_depot), OrderDepotTypeFlags{},
				OrderNonStopFlags{}, OrderDepotActionFlags{OrderDepotActionFlag::Halt});
		tow->SetDestTile(tow->rescue_home_depot);
		tow->vehstatus.Reset(VehState::Stopped);
	} else {
		tow->current_order.MakeDummy();
		tow->vehstatus.Reset(VehState::Stopped);
	}

	InvalidateWindowData(WindowClass::VehicleView, tow->index);
	SetWindowDirty(WindowClass::VehicleDepot, tow->tile);
	SetWindowClassesDirty(WindowClass::TrainList);
}

/**
 * Couple a stopped train to another stopped train immediately adjacent to
 * it on the open track (as opposed to #CmdMoveRailVehicle, which rearranges
 * consists inside a depot). See #GetTrainCouplePartner for exactly which
 * geometry is currently recognised, and FEATURE_DESIGN_COUPLING_TOW.md for
 * the full design this is one piece of.
 *
 * @param flags  type of operation
 * @param veh_id the train initiating the coupling
 * @return the cost of this operation or an error
 */
CommandCost CmdCoupleTrains(DoCommandFlags flags, VehicleID veh_id)
{
	Train *v = Train::GetIfValid(veh_id);
	if (v == nullptr) return CMD_ERROR;

	CommandCost ret = CheckOwnership(v->owner);
	if (ret.Failed()) return ret;

	bool partner_is_behind = false;
	Train *partner = GetTrainCouplePartner(v, &partner_is_behind);
	if (partner == nullptr) return CommandCost(STR_ERROR_CAN_T_COUPLE_TRAIN_NO_PARTNER);

	/* Re-check ownership of the partner explicitly: GetTrainCouplePartner()
	 * already requires matching owners, but CheckOwnership() also handles
	 * the "spectator"/deity edge cases that a plain == comparison would not. */
	ret = CheckOwnership(partner->owner);
	if (ret.Failed()) return ret;

	/* The head of the merged chain has to be an engine. A chain heading with a
	 * wagon is not a train at all but a "free wagon" set (NormaliseSubtypes
	 * decides exactly that way): it stops being a primary vehicle, so it loses
	 * its orders, refuses new ones, and the vehicle window trips
	 * assert(v->IsPrimaryVehicle()) the moment it is touched. So the engine
	 * takes the head, whichever of the two trains it happens to be and
	 * whichever end it arrived from -- the geometry is made to fit around that
	 * below, rather than the other way round. */
	Train *leading = partner_is_behind ? v : partner;
	Train *trailing = partner_is_behind ? partner : v;
	if (!leading->IsFrontEngine()) std::swap(leading, trailing);
	if (!leading->IsFrontEngine()) return CommandCost(STR_ERROR_CAN_T_COUPLE_TRAIN_WRONG_END);

	/* On a rescue the two ends are both engines, so the rule above settles
	 * nothing: geometry would hand the head to whichever happens to be in
	 * front, and that could be a train that has broken down or been wrecked and
	 * is in no state to drive anything anywhere. The engine that came to fetch
	 * takes the head. */
	Train *tow = nullptr;
	if (IsOnRescueRun(v->First()) && v->First()->rescue_target == partner->index) tow = v->First();
	if (IsOnRescueRun(partner) && partner->rescue_target == v->First()->index) tow = partner;
	if (tow != nullptr && leading != tow) std::swap(leading, trailing);

	/* Same question when two ordinary trains join: which of them drives the
	 * result. Geometry would hand it to whichever happens to be in front, and
	 * that is not a decision -- it is an accident of which way they were
	 * pointing. The one that came to collect leads, and the one that was
	 * waiting to be collected travels as its wagons until it is put down again.
	 * See FEATURE_DESIGN_COUPLING_TOW.md. */
	Train *collector = nullptr;
	if (v->First()->current_order.ShouldGoToCouple()) collector = v->First();
	else if (partner->current_order.ShouldGoToCouple()) collector = partner;
	if (collector != nullptr && collector->IsFrontEngine() && leading != collector) std::swap(leading, trailing);

	/* How the collecting train came in. Read now, before any relinking below
	 * moves a head about, because the way out of the platform is measured
	 * against it -- see the note where the joined train's leading end is
	 * settled. */
	Direction arrived_heading = (collector != nullptr ? collector : leading)->GetMovingDirection();

	/* Some gap is normal -- a train cannot reserve the tile its partner stands
	 * on, so it stops about a tile short and the two are closed up after the
	 * splice by CloseUpCoupledConsist(). Only refuse a distance that no amount
	 * of closing up should be asked to cover. */
	if (!AreConsistsCloseEnoughToCouple(v, partner)) return CommandCost(STR_ERROR_CAN_T_COUPLE_TRAIN_GAP);

	if (flags.Test(DoCommandFlag::Execute)) {
		/* Let go of the track each of them was holding, and do it now, before
		 * anything about either train changes.
		 *
		 * Track is released by walking forward from the end that leads, along
		 * the reservation, until it runs out. That only reaches the path a train
		 * is about to drive over. A moment from now this train leads with its
		 * other end -- it has to leave the way it came in -- so the way it came
		 * in stops being in front of it and becomes something behind it that
		 * nothing will ever walk along again. It stayed reserved for the rest of
		 * the game: the joined train could not leave over it, and no other train
		 * could ever use it either.
		 *
		 * Done here, while both trains still face the way they arrived, so what
		 * is released is exactly what each of them holds. See
		 * FEATURE_DESIGN_COUPLING_TOW.md. */
		FreeTrainTrackReservation(v->First());
		FreeTrainTrackReservation(partner);

		/* Splicing appends the trailing list to the end of the leading one, so
		 * the two ends that meet in the middle have to be the two ends that
		 * meet on the rails: the leading train's last vehicle and the trailing
		 * train's first. Either train may be lying the other way round -- an
		 * engine that comes back to a rake from the end it did not leave by is
		 * the whole point of this -- and the one that is has its list relinked
		 * back to front, which moves nothing. Measuring which ends face each
		 * other rather than deriving it from which way anything faces keeps
		 * this right for a train that reversed on its way here, which is the
		 * usual case. */
		if (DistanceSquaredBetweenVehicles(leading->First(), trailing) < DistanceSquaredBetweenVehicles(leading->Last(), trailing)) {
			leading = ReverseConsistOrder(leading);
		}
		if (DistanceSquaredBetweenVehicles(trailing->Last(), leading) < DistanceSquaredBetweenVehicles(trailing->First(), leading)) {
			trailing = ReverseConsistOrder(trailing);
		}
	}

	Train *src = trailing->GetFirstEnginePart();
	Train *dst = leading->Last()->GetLastEnginePart();

	if (src->IsRearDualheaded()) return CommandCost(STR_ERROR_REAR_ENGINE_FOLLOW_FRONT);

	/* Every coupling done here is meant to come apart again, so whichever train
	 * ends up travelling as the other one's wagons stays the train it was: it
	 * keeps its orders, its number and its name and simply does nothing about
	 * them until it is put down. That is what makes two little trains able to
	 * run as one and then go their separate ways again, and it is what lets a
	 * fetched casualty carry its own orders rather than having them handed
	 * about. See FEATURE_DESIGN_COUPLING_TOW.md. */
	Train *new_head = leading;
	ret = TryConsistSplice(flags, src, dst, true, true);
	if (ret.Failed() || !flags.Test(DoCommandFlag::Execute)) return ret;

	/* The vehicles of the two halves point every which way relative to each
	 * other; line them up along the list so nothing reads a nose that
	 * contradicts its neighbours. */
	NormaliseCoupledConsistFacing(new_head);

	/* Which end of the joined train leads is a separate question, and it is not
	 * one to answer from the order of the list.
	 *
	 * A train is described by two things that have nothing to do with each
	 * other: the order of its vehicles, and which end goes first when it moves.
	 * Coupling does not change the first and reversing does not change the
	 * second. On a platform reached by backing in, those two point at opposite
	 * ends of the train; on one reached nose first, at the same end. So any
	 * rule that names an end -- "the head leads" -- is right on one and
	 * backwards on the other, which is why fixing one kind of platform kept
	 * breaking the other.
	 *
	 * So measure it. The joined train has to leave the platform by the way the
	 * collecting train came in, because that is the way it got in past
	 * everything else; therefore it leads with the end that was trailing on the
	 * way in. Whether that end happens to be the head of the list is not asked
	 * and does not matter.
	 *
	 * Compared as a direction rather than for equality: a vehicle on a curve is
	 * up to 45 degrees off and is perfectly correct. See
	 * FEATURE_DESIGN_COUPLING_TOW.md. */
	TileIndexDiffC nose = TileIndexDiffCByDir(new_head->direction);
	TileIndexDiffC leaving = TileIndexDiffCByDir(ReverseDir(arrived_heading));
	new_head->vehicle_flags.Set(VehicleFlag::DrivingBackwards, nose.x * leaving.x + nose.y * leaving.y < 0);

	/* Unless only one end of the joined train has a driving cab, in which case
	 * that end leads and there is nothing to measure. A train nobody can see out
	 * of runs at walking pace, and which way round a train runs has to be
	 * something a player can say in advance rather than something that falls out
	 * of where the wagons happened to be standing.
	 *
	 * When both ends have one -- which is what collecting an engine from the far
	 * end of a train makes it, a push-pull set -- neither is worse than the
	 * other, and the way it came in is the way that is known to be passable. */
	bool head_leads = new_head->CanLeadTrain();
	bool tail_leads = new_head->Last()->CanLeadTrain();
	if (head_leads != tail_leads) {
		new_head->vehicle_flags.Set(VehicleFlag::DrivingBackwards, tail_leads);
	}

	new_head->flags.Reset(VehicleRailFlag::Reversing);
	new_head->ConsistChanged(CCF_TRACK);

	/* The two halves were joined across a gap; walk it shut before anything
	 * tries to drive this train. */
	CloseUpCoupledConsist(new_head);

	/* It has what it came for, so it is not coming for anything any more. The
	 * matching half written on the collected train is rubbed out by the splice.
	 */
	new_head->couple_target = VehicleID::Invalid();

	/* The coupling this order asked for has happened, so the order is done.
	 * Left set, the merged train goes on reading itself as still waiting for a
	 * partner and simply stands there for good -- it has no reason left to
	 * move and never advances to whatever the engine was supposed to do next.
	 * Clearing it on the current order only, not in the order list, so a train
	 * looping back round to this order couples again as intended. */
	bool was_go_to_couple = new_head->current_order.ShouldGoToCouple();
	new_head->current_order.SetGoToCouple(false);
	new_head->current_order.SetWaitForCouple(false);

	/* A train that came here under a "go to couple" order has now done what
	 * that order asked, so the order is finished and the next one is due.
	 *
	 * Nothing else will conclude that for it. A train only moves on from a
	 * station order by arriving: the platform's own tile handler notices the
	 * front reach the stop location, the train loads, and departing advances
	 * the order. But a train sent to collect wagons stops when it touches
	 * them, which is short of the stop location while they are standing in
	 * the rest of the platform, so that handler never fires. The order stays
	 * current, the train still reads it as "get to this station", and off it
	 * goes -- round the loop and back to the same platform, whose only
	 * purpose is to finally trigger the arrival that lets the order advance.
	 * That is the pointless lap round the station.
	 *
	 * Whether it happens depends only on how far into the platform the wagons
	 * are: a train that happened to stop exactly on the stop location does
	 * arrive, loads, and couples from the loading code instead, which departs
	 * normally and advances the order by itself. Same order, same wagons,
	 * different platform -- which is why some platforms looked fine.
	 *
	 * So do here what arriving would have done. If the train is standing in
	 * the station it was sent to, let it enter properly, cargo handling and
	 * all; that is the same route the lucky platforms take. If it is short of
	 * the platform, there is nothing to load at, so just mark the station
	 * reached and let the next order be picked up. */
	if (was_go_to_couple && new_head->current_order.IsType(OT_GOTO_STATION)) {
		StationID dest = new_head->current_order.GetDestination().ToStationID();
		if (IsConsistStandingAtStation(new_head, dest)) {
			if (IsRailStationTileOfStation(new_head->GetMovingFront()->tile, dest)) {
				TrainEnterStation(new_head, dest);
			} else {
				new_head->DeleteUnreachedImplicitOrders();
				new_head->last_station_visited = dest;
				UpdateVehicleTimetable(new_head, true);
				new_head->IncrementImplicitOrderIndex();
			}
		}
	}

	/* Both paths were thrown away before the splice, so the route this train was
	 * following is gone and will be planned again from here. That is wanted for
	 * its own sake as well: the old route was worked out while the wagons now
	 * being carried were a separate headless consist standing in the way --
	 * impassable, so anything leading past them was ruled out and the search
	 * settled on a way round. That obstacle has just become part of this train,
	 * so the route it forced is a route around nothing. All that is left to do
	 * is hold the ground the joined train is standing on. */
	new_head->ReserveTrackUnderConsist();

	/* A rescue engine has what it came for. Where it takes it is the nearest
	 * depot -- a casualty is fetched to get it off the line, and the nearest
	 * way off the line is the best one. What happens to it there is
	 * HandleRescueEngineInDepot()'s business. */
	if (tow != nullptr) {
		AutoRestoreBackup cur_company(_current_company, new_head->owner);
		Command<Commands::SendVehicleToDepot>::Do(DoCommandFlag::Execute, new_head->index,
				DepotCommandFlags{DepotCommandFlag::DontCancel}, VehicleListIdentifier{});
	}

	return ret;
}

/**
 * Decouple a stopped train down to its front @p keep_count "real"
 * (non-articulated-part) vehicles, splitting the rest off into a new
 * standalone train left behind. Called from #Vehicle::LeaveStation when
 * the order just finished has Order::ShouldDecoupleOnDeparture() set; see
 * FEATURE_DESIGN_COUPLING_TOW.md.
 *
 * If @p keep_count is 0 or is not achievable with the train's current
 * length (e.g. the consist has since been shortened, or it's a
 * multiheaded engine that can't be split at that point), this silently
 * does nothing rather than failing loudly - the order's decouple count is
 * a per-order setting that can outlive changes to the consist it was set
 * on, similar to how e.g. full-load orders don't error on a consist that
 * can never fill up.
 *
 * Right after the split, the left-behind remainder is given a synthetic
 * "wait to couple" order at the station it's still standing on, so it
 * immediately reads as "Waiting for couple" (rather than "No orders") and
 * is ready to accept a partner the moment one becomes adjacent -- matching
 * the mental model that a decoupled remainder is always left in a state
 * ready to be picked up again. See FEATURE_DESIGN_COUPLING_TOW.md.
 *
 * @param v          front of the consist that just finished loading
 * @param keep_count number of vehicles to keep at the front
 */
void TryDecoupleAtStation(Train *v, uint8_t keep_count, OrderLoadType load_type, OrderUnloadType unload_type)
{
	if (keep_count == 0) return;
	if (v->vehstatus.Test(VehState::Crashed)) return;

	Train *split_point = v;
	for (uint8_t i = 0; i < keep_count; i++) {
		split_point = split_point->GetNextVehicle();
		if (split_point == nullptr) return; // consist has fewer than keep_count vehicles
	}

	if (split_point->IsRearDualheaded()) return; // can't split a multiheaded engine in half

	/* Which end leads belongs to the whole train and has to be handed on to the
	 * part being put down before the front engine's own copy is reset below. */
	bool was_driving_backwards = v->vehicle_flags.Test(VehicleFlag::DrivingBackwards);

	/* Release the whole train's current reservation before splitting,
	 * not just what's about to become each half's own footprint. A
	 * train stopped at a station commonly holds a PBS reservation
	 * extending some distance beyond its own physical body (the normal
	 * "safe waiting position" lookahead) -- TryConsistSplice() below
	 * only ever re-asserts reservation for each new consist's own
	 * tiles (via ReserveTrackUnderConsist()), so any such excess was
	 * never released by anything and just sat there, phantom, forever
	 * (visible in-game as a reserved-looking track segment past the
	 * decoupled wagons with nothing on it) -- and then confused later
	 * pathfinding attempts that ran into it. v is still one whole train
	 * with a normal front engine here, so this safely clears
	 * everything; TryConsistSplice() re-reserves exactly what each half
	 * actually needs from a clean slate right after. See
	 * FEATURE_DESIGN_COUPLING_TOW.md. */
	FreeTrainTrackReservation(v);

	TryConsistSplice(DoCommandFlag::Execute, split_point, nullptr, true);

	/* The engine keeps the front of the list and the wagons it is leaving are
	 * everything behind it, so whichever way the train came in, the way out is
	 * the end the engine is on.
	 *
	 * That matters for a train that reversed in. It drove in led by its last
	 * vehicle, and going on being led by its last vehicle now means being led
	 * straight at the wagons it has just put down -- a headless rake, which the
	 * route search treats as solid, so no route out exists at all and the
	 * engine simply stands there waiting for a path that can never come.
	 * Leading with its own end instead takes it back out the way it came in,
	 * which is the only way out of a platform it backed into.
	 *
	 * Nothing is turned round on the ground by this. A train that reverses in
	 * is already sitting with its nose pointing back out; driving backwards is
	 * the state of leading with the other end, and that state is what ends
	 * here. For a train that arrived nose first there was never anything to
	 * change. Coupling settles the same question the same way at its end -- see
	 * CmdCoupleTrains(). */
	v->vehicle_flags.Reset(VehicleFlag::DrivingBackwards);
	v->flags.Reset(VehicleRailFlag::Reversing);
	v->ConsistChanged(CCF_TRACK);
	FreeTrainTrackReservation(v);
	v->ReserveTrackUnderConsist();

	Train *remainder = split_point->First();
	/* Nobody has spoken for these wagons yet; whatever this vehicle was doing
	 * in an earlier life is over. */
	remainder->couple_claim = VehicleID::Invalid();

	/* It leaves the same way round as the train it was part of. Which end leads
	 * is a property of the whole train, and the part being put down is still
	 * lying the way it was lying a moment ago. */
	remainder->vehicle_flags.Set(VehicleFlag::DrivingBackwards, was_driving_backwards);

	/* What is put down is not always wagons. Two little trains can join to run
	 * as one big one, and when they come apart the one that was travelling as
	 * wagons is a train again -- it kept its orders all along (see
	 * TryConsistSplice) and now goes back to working through them.
	 *
	 * The order it was standing on when it was picked up is done: it waited
	 * there, it was collected, and it has been carried to wherever it is now.
	 * So it moves on to the next one. And if that next order names the very
	 * station it has just been put down at, it works through it here rather
	 * than driving off to reach somewhere it has never left -- the same lap
	 * round the station that collecting used to make. */
	if (remainder->IsFrontEngine()) {
		remainder->ConsistChanged(CCF_TRACK);

		if (IsRailStationTile(remainder->tile) && remainder->GetNumOrders() != 0) {
			StationID station = GetStationIndex(remainder->tile);
			remainder->last_station_visited = station;
			remainder->DeleteUnreachedImplicitOrders();
			remainder->IncrementImplicitOrderIndex();
			remainder->UpdateRealOrderIndex();

			const Order *next = remainder->GetOrder(remainder->cur_real_order_index);
			if (next != nullptr && next->IsType(OT_GOTO_STATION) && next->GetDestination().ToStationID() == station &&
					IsRailStationTileOfStation(remainder->GetMovingFront()->tile, station)) {
				remainder->current_order = *next;
				remainder->current_order.SetGoToCouple(false);
				remainder->current_order.SetWaitForCouple(false);
				TrainEnterStation(remainder, station);
			} else {
				UpdateVehicleTimetable(remainder, true);
				remainder->current_order.Free();
				remainder->SetDestTile(INVALID_TILE);
			}
		} else {
			remainder->current_order.Free();
			remainder->SetDestTile(INVALID_TILE);
		}

		FreeTrainTrackReservation(remainder);
		remainder->ReserveTrackUnderConsist();
		return;
	}

	if (IsRailStationTile(remainder->tile)) {
		StationID station = GetStationIndex(remainder->tile);
		remainder->current_order.MakeGoToStation(station);
		/* The wagons keep being handled the way the train was told to handle
		 * them here. They are the same wagons at the same platform a moment
		 * later, so an order not to load that applied to them while they were
		 * coupled has to go on applying once they are not -- otherwise the
		 * engine departs under orders not to load and the wagons it left
		 * behind start filling up on their own.
		 *
		 * And that order is theirs to finish before they are anybody's to
		 * collect. Wagons told to fill up are doing a job here, not waiting;
		 * only once the job is done do they become something an engine can be
		 * sent for. Saying both at once -- filling up and waiting -- was wrong
		 * and is what let a half-loaded rake be carried off. See Train::Tick(),
		 * which is where the one turns into the other. */
		remainder->current_order.SetLoadType(load_type);
		remainder->current_order.SetUnloadType(unload_type);

		/* Told to fill up, the rake has a job to finish here and is not waiting
		 * for anybody until it is done. Told anything else, there is nothing to
		 * finish, so it is waiting from the moment it is put down. */
		remainder->current_order.SetWaitForCouple(!remainder->current_order.IsFullLoadOrder());

		/* And the rake gets a real pair of orders: what the engine left it doing
		 * here, and then waiting to be collected. Two orders, so the player can
		 * open the ordinary orders window and press the ordinary Skip button to
		 * go from the first to the second -- which is how you say "never mind
		 * the load, take them away" when the industry that was filling them has
		 * closed. No special button anywhere, and the same shape the reference
		 * has.
		 *
		 * Nothing works through this list on its own: only the head of a train
		 * is ever asked what to do next, and a rake has no engine at its head.
		 * It is there to be read, skipped, and edited. Train::Tick() moves it on
		 * from the first to the second when the loading really does finish. */
		/* An order list is a pooled object, and the pool insists on being asked
		 * whether it has room before anything is taken from it -- CanAllocateItem()
		 * is not advice, it is the permission the allocation itself checks for,
		 * and taking without asking is what brought the game down the moment an
		 * engine put its wagons down. Every other place that gives a vehicle its
		 * first order asks first (see CmdInsertOrder); this one has to as well,
		 * and if the answer is no the rake simply goes without a written list.
		 * It is still standing there waiting to be collected either way -- that
		 * lives on the order it is working from, not on the list. */
		if (remainder->orders == nullptr && OrderList::CanAllocateItem()) {
			Order job = remainder->current_order;
			job.SetWaitForCouple(false);

			Order waiting{};
			waiting.MakeGoToStation(station);
			waiting.SetLoadType(OrderLoadType::NoLoad);
			waiting.SetUnloadType(OrderUnloadType::NoUnload);
			waiting.SetWaitForCouple(true);

			InsertOrder(remainder, std::move(job), 0);
			InsertOrder(remainder, std::move(waiting), 1);
			remainder->cur_real_order_index = remainder->cur_implicit_order_index =
					remainder->current_order.ShouldWaitForCouple() ? 1 : 0;
		}

		TrainEnterStation(remainder, station);
	}
}

/**
 * Sell a (single) train wagon/engine.
 * @param flags type of operation
 * @param t     the train wagon to sell
 * @param sell_chain  the selling mode
 * - sell_chain = false: only sell the single dragged wagon/engine (and any belonging rear-engines)
 * - sell_chain = true:  sell the vehicle and all vehicles following it in the chain
 *                       if the wagon is dragged, don't delete the possibly belonging rear-engine to some front
 * @param backup_order make order backup?
 * @param user  the user for the order backup.
 * @return the cost of this operation or an error
 */
CommandCost CmdSellRailWagon(DoCommandFlags flags, Vehicle *t, bool sell_chain, bool backup_order, ClientID user)
{
	Train *v = Train::From(t)->GetFirstEnginePart();
	Train *first = v->First();

	if (v->IsRearDualheaded()) return CommandCost(STR_ERROR_REAR_ENGINE_FOLLOW_FRONT);

	/* First make a backup of the order of the train. That way we can do
	 * whatever we want with the order and later on easily revert. */
	TrainList original;
	MakeTrainBackup(original, first);

	/* We need to keep track of the new head and the head of what we're going to sell. */
	Train *new_head = first;
	Train *sell_head = nullptr;

	/* Split the train in the wanted way. */
	ArrangeTrains(&sell_head, nullptr, &new_head, v, sell_chain);

	/* We don't need to validate the second train; it's going to be sold. */
	CommandCost ret = ValidateTrains(nullptr, nullptr, first, new_head, !flags.Test(DoCommandFlag::AutoReplace));
	if (ret.Failed()) {
		/* Restore the train we had. */
		RestoreTrainBackup(original);
		return ret;
	}

	if (first->orders == nullptr && !OrderList::CanAllocateItem()) {
		/* Restore the train we had. */
		RestoreTrainBackup(original);
		return CommandCost(STR_ERROR_NO_MORE_SPACE_FOR_ORDERS);
	}

	CommandCost cost(ExpensesType::NewVehicles);
	for (Train *part = sell_head; part != nullptr; part = part->Next()) cost.AddCost(-part->value);

	/* do it? */
	if (flags.Test(DoCommandFlag::Execute)) {
		/* First normalise the sub types of the chain. */
		NormaliseSubtypes(new_head);

		if (v == first && !sell_chain && new_head != nullptr && new_head->IsFrontEngine()) {
			if (v->IsEngine()) {
				/* We are selling the front engine. In this case we want to
				 * 'give' the order, unit number and such to the new head. */
				new_head->orders = first->orders;
				new_head->AddToShared(first);
				DeleteVehicleOrders(first);

				/* Copy other important data from the front engine */
				new_head->CopyVehicleConfigAndStatistics(first);
			}
			GroupStatistics::CountVehicle(new_head, 1); // after copying over the profit, if required
		} else if (v->IsPrimaryVehicle() && backup_order) {
			OrderBackup::Backup(v, user);
		}

		/* We need to update the information about the train. */
		NormaliseTrainHead(new_head);

		/* We are undoubtedly changing something in the depot and train list. */
		InvalidateWindowData(WindowClass::VehicleDepot, v->tile);
		InvalidateWindowClassesData(WindowClass::TrainList, 0);

		/* Actually delete the sold 'goods' */
		delete sell_head;
	} else {
		/* We don't want to execute what we're just tried. */
		RestoreTrainBackup(original);
	}

	return cost;
}

void Train::UpdateDeltaXY()
{
	/* Set common defaults. */
	this->bounds = {{-1, -1, 0}, {3, 3, 6}, {}};

	/* Set if flipped and engine is NOT flagged with custom flip handling. */
	int flipped = this->flags.Test(VehicleRailFlag::Flipped) && !EngInfo(this->engine_type)->misc_flags.Test(EngineMiscFlag::RailFlips);
	/* If flipped and vehicle length is odd, we need to adjust the bounding box offset slightly. */
	int flip_offs = flipped && (this->gcache.cached_veh_length & 1);

	Direction dir = this->direction;
	if (flipped) dir = ReverseDir(dir);

	if (!IsDiagonalDirection(dir)) {
		static constexpr DiagDirectionIndexArray<Point> _sign_table{{{
			/* x, y */
			{-1, -1}, // DiagDirection::N
			{-1,  1}, // DiagDirection::E
			{ 1,  1}, // DiagDirection::S
			{ 1, -1}, // DiagDirection::W
		}}};

		int half_shorten = (VEHICLE_LENGTH - this->gcache.cached_veh_length + flipped) / 2;

		/* For all straight directions, move the bound box to the centre of the vehicle, but keep the size. */
		this->bounds.offset.x -= half_shorten * _sign_table[DirToDiagDir(dir)].x;
		this->bounds.offset.y -= half_shorten * _sign_table[DirToDiagDir(dir)].y;
	} else {
		switch (dir) {
				/* Shorten southern corner of the bounding box according the vehicle length
				 * and center the bounding box on the vehicle. */
			case Direction::NE:
				this->bounds.origin.x = -(this->gcache.cached_veh_length + 1) / 2 + flip_offs;
				this->bounds.extent.x = this->gcache.cached_veh_length;
				this->bounds.offset.x = 1;
				break;

			case Direction::NW:
				this->bounds.origin.y = -(this->gcache.cached_veh_length + 1) / 2 + flip_offs;
				this->bounds.extent.y = this->gcache.cached_veh_length;
				this->bounds.offset.y = 1;
				break;

				/* Move northern corner of the bounding box down according to vehicle length
				 * and center the bounding box on the vehicle. */
			case Direction::SW:
				this->bounds.origin.x = -(this->gcache.cached_veh_length) / 2 - flip_offs;
				this->bounds.extent.x = this->gcache.cached_veh_length;
				this->bounds.offset.x = 1 - (VEHICLE_LENGTH - this->gcache.cached_veh_length);
				break;

			case Direction::SE:
				this->bounds.origin.y = -(this->gcache.cached_veh_length) / 2 - flip_offs;
				this->bounds.extent.y = this->gcache.cached_veh_length;
				this->bounds.offset.y = 1 - (VEHICLE_LENGTH - this->gcache.cached_veh_length);
				break;

			default:
				NOT_REACHED();
		}
	}
}

/**
 * Mark a train as stuck and stop it if it isn't stopped right now.
 * @param consist %Train to mark as being stuck.
 */
static void MarkTrainAsStuck(Train *consist)
{
	if (!consist->flags.Test(VehicleRailFlag::Stuck)) {
		/* It is the first time the problem occurred, set the "train stuck" flag. */
		consist->flags.Set(VehicleRailFlag::Stuck);

		consist->wait_counter = 0;

		/* Stop train */
		consist->cur_speed = 0;
		consist->subspeed = 0;
		consist->SetLastSpeed();

		SetWindowWidgetDirty(WindowClass::VehicleView, consist->index, WID_VV_START_STOP);
	}
}

/**
 * Swap the two up/down flags in two ways:
 * - Swap values of \a swap_flag1 and \a swap_flag2, and
 * - If going up previously (#GroundVehicleFlag::GoingUp set), the #GroundVehicleFlag::GoingDown is set, and vice versa.
 * @param[in,out] swap_flag1 First train flag.
 * @param[in,out] swap_flag2 Second train flag.
 */
static void SwapTrainFlags(GroundVehicleFlags *swap_flag1, GroundVehicleFlags *swap_flag2)
{
	GroundVehicleFlags flag1 = *swap_flag1;
	GroundVehicleFlags flag2 = *swap_flag2;

	/* Reverse the rail-flags (if needed) */
	swap_flag2->Set(GroundVehicleFlag::GoingDown, flag1.Test(GroundVehicleFlag::GoingUp));
	swap_flag2->Set(GroundVehicleFlag::GoingUp, flag1.Test(GroundVehicleFlag::GoingDown));
	swap_flag1->Set(GroundVehicleFlag::GoingDown, flag2.Test(GroundVehicleFlag::GoingUp));
	swap_flag1->Set(GroundVehicleFlag::GoingUp, flag2.Test(GroundVehicleFlag::GoingDown));
}

/**
 * Updates some variables after swapping the vehicle.
 * @param v swapped vehicle
 * @param reverse Should we reverse the direction of the vehicle?
 */
static void UpdateStatusAfterSwap(Train *v, bool reverse = true)
{
	/* Maybe reverse the direction. */
	if (reverse) v->direction = ReverseDir(v->direction);

	/* Call the proper EnterTile function unless we are in a wormhole. */
	if (v->track != Track::Wormhole) {
		VehicleEnterTile(v, v->tile, v->x_pos, v->y_pos);
	} else {
		/* VehicleEnterTile_TunnelBridge() sets Track::Wormhole when the vehicle
		 * is on the last bit of the bridge head (frame == TILE_SIZE - 1).
		 * If we were swapped with such a vehicle, we have set Track::Wormhole,
		 * when we shouldn't have. Check if this is the case. */
		TileIndex vt = TileVirtXY(v->x_pos, v->y_pos);
		if (IsTileType(vt, TileType::TunnelBridge)) {
			VehicleEnterTile(v, vt, v->x_pos, v->y_pos);
			if (v->track != Track::Wormhole && IsBridgeTile(v->tile)) {
				/* We have just left the wormhole, possibly set the
				 * "goingdown" bit. UpdateInclination() can be used
				 * because we are at the border of the tile. */
				v->UpdatePosition();
				v->UpdateInclination(true, true);
				return;
			}
		}
	}

	v->UpdatePosition();
	v->UpdateViewport(true, true);
}

/**
 * Swap vehicles \a l and \a r in consist \a v, and reverse their direction.
 * UpdateStatusAfterSwap calls should be made after all ReverseTrainSwapVeh calls have been completed.
 * @param v Consist to change.
 * @param l %Vehicle index in the consist of the first vehicle.
 * @param r %Vehicle index in the consist of the second vehicle.
 */
static void ReverseTrainSwapVeh(Train *v, int l, int r)
{
	Train *a, *b;

	/* locate vehicles to swap */
	for (a = v; l != 0; l--) a = a->Next();
	for (b = v; r != 0; r--) b = b->Next();

	if (a != b) {
		/* swap the hidden bits */
		{
			bool a_hidden = a->vehstatus.Test(VehState::Hidden);
			bool b_hidden = b->vehstatus.Test(VehState::Hidden);
			b->vehstatus.Set(VehState::Hidden, a_hidden);
			a->vehstatus.Set(VehState::Hidden, b_hidden);
		}

		std::swap(a->track, b->track);
		std::swap(a->direction, b->direction);
		std::swap(a->x_pos, b->x_pos);
		std::swap(a->y_pos, b->y_pos);
		std::swap(a->tile,  b->tile);
		std::swap(a->z_pos, b->z_pos);

		SwapTrainFlags(&a->gv_flags, &b->gv_flags);
	} else {
		/* Swap GroundVehicleFlag::GoingUp/GroundVehicleFlag::GoingDown.
		 * This is a little bit redundant way, a->gv_flags will
		 * be (re)set twice, but it reduces code duplication */
		SwapTrainFlags(&a->gv_flags, &a->gv_flags);
	}
}

/**
 * Swap vehicles in chain starting from \a v, and reverse their direction.
 * @param v First vehicle in chain to change.
 */
void ReverseTrainSwapVehicles(Train *v)
{
	int r = CountVehiclesInChain(v) - 1;  // number of vehicles - 1

	/* swap start<>end, start+1<>end-1, ... */
	int l = 0;
	do {
		ReverseTrainSwapVeh(v, l++, r--);
	} while (l <= r);

	for (Train *u = v; u != nullptr; u = u->Next()) {
		UpdateStatusAfterSwap(u);
	}
}

/**
 * Check if the vehicle is a train
 * @param v vehicle on tile
 * @return true if v is a train
 */
static bool IsTrain(const Vehicle *v)
{
	return v->type == VehicleType::Train;
}

/**
 * Check if a level crossing tile has a train on it
 * @param tile tile to test
 * @return true if a train is on the crossing
 * @pre tile is a level crossing
 */
bool TrainOnCrossing(TileIndex tile)
{
	assert(IsLevelCrossingTile(tile));

	return HasVehicleOnTile(tile, IsTrain);
}

/**
 * Checks if a train is approaching a rail-road crossing
 * @param v vehicle on tile
 * @param tile tile with crossing we are testing
 * @return true if v is approaching a crossing
 */
static bool TrainApproachingCrossingEnum(const Vehicle *v, TileIndex tile)
{
	if (v->type != VehicleType::Train || v->vehstatus.Test(VehState::Crashed)) return false;

	const Train *t = Train::From(v);
	if (!t->IsMovingFront()) return false;

	return TrainApproachingCrossingTile(t) == tile;
}


/**
 * Finds a vehicle approaching rail-road crossing
 * @param tile tile to test
 * @return true if a vehicle is approaching the crossing
 * @pre tile is a rail-road crossing
 */
static bool TrainApproachingCrossing(TileIndex tile)
{
	assert(IsLevelCrossingTile(tile));

	DiagDirection dir = AxisToDiagDir(GetCrossingRailAxis(tile));
	TileIndex tile_from = tile + TileOffsByDiagDir(dir);

	if (HasVehicleOnTile(tile_from, [&](const Vehicle *v) {
			return TrainApproachingCrossingEnum(v, tile);
		})) return true;

	dir = ReverseDiagDir(dir);
	tile_from = tile + TileOffsByDiagDir(dir);

	return HasVehicleOnTile(tile_from, [&](const Vehicle *v) {
		return TrainApproachingCrossingEnum(v, tile);
	});
}

/**
 * Check if a level crossing should be barred.
 * @param tile The tile to check.
 * @return True if the crossing should be barred, else false.
 */
static inline bool CheckLevelCrossing(TileIndex tile)
{
	/* reserved || train on crossing || train approaching crossing */
	return HasCrossingReservation(tile) || TrainOnCrossing(tile) || TrainApproachingCrossing(tile);
}

/**
 * Sets a level crossing tile to the correct state.
 * @param tile Tile to update.
 * @param sound Should we play sound?
 * @param force_barred Should we set the crossing to barred?
 * @pre tile is a rail-road crossing.
 */
static void UpdateLevelCrossingTile(TileIndex tile, bool sound, bool force_barred)
{
	assert(IsLevelCrossingTile(tile));
	bool set_barred;

	/* We force the crossing to be barred when an adjacent crossing is barred, otherwise let it decide for itself. */
	set_barred = force_barred || CheckLevelCrossing(tile);

	/* The state has changed */
	if (set_barred != IsCrossingBarred(tile)) {
		if (set_barred && sound && _settings_client.sound.ambient) SndPlayTileFx(SND_0E_LEVEL_CROSSING, tile);
		SetCrossingBarred(tile, set_barred);
		MarkTileDirtyByTile(tile);
	}
}

/**
 * Update a level crossing to barred or open (crossing may include multiple adjacent tiles).
 * @param tile Tile which causes the update.
 * @param sound Should we play sound?
 * @param force_bar Should we force the crossing to be barred?
 */
void UpdateLevelCrossing(TileIndex tile, bool sound, bool force_bar)
{
	if (!IsLevelCrossingTile(tile)) return;

	bool forced_state = force_bar;

	Axis axis = GetCrossingRoadAxis(tile);
	DiagDirections diagdirs = AxisToDiagDirs(axis);

	/* Check if an adjacent crossing is barred. */
	for (DiagDirection dir : diagdirs) {
		for (TileIndex t = tile; !forced_state && t < Map::Size() && IsLevelCrossingTile(t) && GetCrossingRoadAxis(t) == axis; t = TileAddByDiagDir(t, dir)) {
			forced_state |= CheckLevelCrossing(t);
		}
	}

	/* Now that we know whether all tiles in this crossing should be barred or open,
	 * we need to update those tiles. We start with the tile itself, then look along the road axis. */
	UpdateLevelCrossingTile(tile, sound, forced_state);
	for (DiagDirection dir : diagdirs) {
		for (TileIndex t = TileAddByDiagDir(tile, dir); t < Map::Size() && IsLevelCrossingTile(t) && GetCrossingRoadAxis(t) == axis; t = TileAddByDiagDir(t, dir)) {
			UpdateLevelCrossingTile(t, sound, forced_state);
		}
	}
}

/**
 * Find adjacent level crossing tiles in this multi-track crossing and mark them dirty.
 * @param tile The tile which causes the update.
 * @param road_axis The road axis.
 */
void MarkDirtyAdjacentLevelCrossingTiles(TileIndex tile, Axis road_axis)
{
	for (DiagDirection dir : AxisToDiagDirs(road_axis)) {
		const TileIndex t = TileAddByDiagDir(tile, dir);
		if (t < Map::Size() && IsLevelCrossingTile(t) && GetCrossingRoadAxis(t) == road_axis) {
			MarkTileDirtyByTile(t);
		}
	}
}

/**
 * Update adjacent level crossing tiles in this multi-track crossing, due to removal of a level crossing tile.
 * @param tile The crossing tile which has been or is about to be removed, and which caused the update.
 * @param road_axis The road axis.
 */
void UpdateAdjacentLevelCrossingTilesOnLevelCrossingRemoval(TileIndex tile, Axis road_axis)
{
	for (DiagDirection dir : AxisToDiagDirs(road_axis)) {
		const TileIndexDiff diff = TileOffsByDiagDir(dir);
		bool occupied = false;
		for (TileIndex t = tile + diff; t < Map::Size() && IsLevelCrossingTile(t) && GetCrossingRoadAxis(t) == road_axis; t += diff) {
			occupied |= CheckLevelCrossing(t);
		}
		if (occupied) {
			/* Mark the immediately adjacent tile dirty */
			const TileIndex t = tile + diff;
			if (t < Map::Size() && IsLevelCrossingTile(t) && GetCrossingRoadAxis(t) == road_axis) {
				MarkTileDirtyByTile(t);
			}
		} else {
			/* Unbar the crossing tiles in this direction as necessary */
			for (TileIndex t = tile + diff; t < Map::Size() && IsLevelCrossingTile(t) && GetCrossingRoadAxis(t) == road_axis; t += diff) {
				if (IsCrossingBarred(t)) {
					/* The crossing tile is barred, unbar it and continue to check the next tile */
					SetCrossingBarred(t, false);
					MarkTileDirtyByTile(t);
				} else {
					/* The crossing tile is already unbarred, mark the tile dirty and stop checking */
					MarkTileDirtyByTile(t);
					break;
				}
			}
		}
	}
}

/**
 * Bars crossing and plays ding-ding sound if not barred already
 * @param tile tile with crossing
 * @pre tile is a rail-road crossing
 */
static inline void MaybeBarCrossingWithSound(TileIndex tile)
{
	if (!IsCrossingBarred(tile)) {
		SetCrossingReservation(tile, true);
		UpdateLevelCrossing(tile, true);
	}
}


/**
 * Advances wagons for train reversing, needed for variable length wagons.
 * This one is called before the train is reversed.
 * @param moving_front Moving front vehicle
 */
static void AdvanceWagonsBeforeSwap(Train *moving_front)
{
	Train *base = moving_front;
	Train *first = base; // first vehicle to move
	Train *last = moving_front->GetMovingBack(); // last vehicle to move
	uint length = CountVehiclesInChain(moving_front->First());

	while (length > 2) {
		last = last->GetMovingPrev();
		first = first->GetMovingNext();

		int differential = base->CalcNextVehicleOffset() - last->CalcNextVehicleOffset();

		/* do not update images now
		 * negative differential will be handled in AdvanceWagonsAfterSwap() */
		for (int i = 0; i < differential; i++) TrainController(first, last->GetMovingNext());

		base = first; // == base->GetMovingNext()
		length -= 2;
	}
}


/**
 * Advances wagons for train reversing, needed for variable length wagons.
 * This one is called after the train is reversed.
 * @param moving_front Moving front vehicle
 */
static void AdvanceWagonsAfterSwap(Train *moving_front)
{
	/* first of all, fix the situation when the train was entering a depot */
	Train *dep = moving_front; // last vehicle in front of just left depot
	while (dep->GetMovingNext() != nullptr && (dep->track == Track::Depot || dep->GetMovingNext()->track != Track::Depot)) {
		dep = dep->GetMovingNext(); // find first vehicle outside of a depot, with next vehicle inside a depot
	}

	Train *leave = dep->GetMovingNext(); // first vehicle in a depot we are leaving now

	if (leave != nullptr) {
		/* 'pull' next wagon out of the depot, so we won't miss it (it could stay in depot forever) */
		int d = TicksToLeaveDepot(dep);

		if (d <= 0) {
			leave->vehstatus.Reset(VehState::Hidden); // move it out of the depot
			leave->track = GetRailDepotTrack(leave->tile);
			for (int i = 0; i >= d; i--) TrainController(leave, nullptr); // maybe move it, and maybe let another wagon leave
		}
	} else {
		dep = nullptr; // no vehicle in a depot, so no vehicle leaving a depot
	}

	Train *base = moving_front;
	Train *first = base; // first vehicle to move
	Train *last = moving_front->GetMovingBack(); // last vehicle to move
	uint length = CountVehiclesInChain(moving_front->First());

	/* We have to make sure all wagons that leave a depot because of train reversing are moved correctly
	 * they have already correct spacing, so we have to make sure they are moved how they should */
	bool nomove = (dep == nullptr); // If there is no vehicle leaving a depot, limit the number of wagons moved immediately.

	while (length > 2) {
		/* we reached vehicle (originally) in front of a depot, stop now
		 * (we would move wagons that are already moved with new wagon length). */
		if (base == dep) break;

		/* the last wagon was that one leaving a depot, so do not move it anymore */
		if (last == dep) nomove = true;

		last = last->GetMovingPrev();
		first = first->GetMovingNext();

		int differential = last->CalcNextVehicleOffset() - base->CalcNextVehicleOffset();

		/* do not update images now */
		for (int i = 0; i < differential; i++) TrainController(first, (nomove ? last->GetMovingNext() : nullptr));

		base = first; // == base->GetMovingNext()
		length -= 2;
	}
}

bool IsWholeTrainInsideDepot(const Train *v)
{
	/* Whichever part of the train was handed in, the question is about all of
	 * it, so start from the head. Walking from somewhere in the middle would
	 * answer for the tail only -- and a train reversing into a depot puts its
	 * tail in first, so that answer comes back "yes, all in" while most of the
	 * train is still out on the track behind. */
	const Train *head = v->First();
	TileIndex tile = head->tile;
	for (const Train *u = head; u != nullptr; u = u->Next()) {
		if (u->track != Track::Depot) return false;
		if (u->tile != tile) return false;
	}
	return true;
}

/**
 * Is any part of this train still standing inside a depot?
 *
 * A train inside a depot is hidden on a single tile and has no extent, and a
 * train outside one lies along the track: those are two different worlds, and
 * a train leaving a depot is in both at once for a few ticks. Anything that
 * rearranges a train has to keep away from it while that lasts.
 */
static bool IsAnyPartInsideDepot(const Train *v)
{
	for (const Train *u = v; u != nullptr; u = u->Next()) {
		if (u->track == Track::Depot) return true;
	}
	return false;
}

/**
 * Would turning this train round drive it into a headless consist standing
 * right behind it?
 *
 * A train that cannot find a path is left stuck, and a train that stays stuck
 * long enough turns itself round to try the other way -- sound enough when
 * what is behind it is open track. It is not sound when what is behind it is
 * the rake of wagons the train itself has just uncoupled and left standing
 * there: turning round drives straight into them. The check that refuses to
 * enter a headless chain's tile cannot help here, because after decoupling the
 * two are touching and often share a tile, so there is no new tile to refuse.
 *
 * @param consist The stuck train.
 * @return true if there is a headless consist immediately behind it.
 */
static bool WouldReverseIntoFreeWagons(const Train *consist)
{
	const Train *back = consist->GetMovingBack();
	Trackdir td = back->GetVehicleTrackdir();
	if (td == Trackdir::Invalid) return false;

	for (const TileIndex tile : {back->tile, TileAddByDiagDir(back->tile, TrackdirToExitdir(ReverseTrackdir(td)))}) {
		for (const Vehicle *u : VehiclesOnTile(tile)) {
			if (u->type != VehicleType::Train) continue;
			const Train *t = Train::From(u)->First();
			if (t == consist->First() || t->IsFrontEngine()) continue;
			return true;
		}
	}
	return false;
}

/**
 * Turn a train around.
 * @param consist %Train to turn around.
 */
static void ReverseTrainDirection(Train *consist)
{
	Train *moving_front = consist->GetMovingFront();
	if (IsRailDepotTile(moving_front->tile)) {
		/* A train on its way out of a depot is half in a world where it has no
		 * extent and half in one where it lies along the track, and everything
		 * below assumes one or the other. Turning it round here mangles the
		 * consist: the vehicles still hidden inside end up on the wrong side of
		 * the ones already out, and the first of them to be moved cannot find
		 * any track that connects it to the vehicle ahead. Leave it alone and
		 * let it finish coming out -- there is nothing to gain by turning a
		 * train round at the exact moment it is leaving, and the player asked
		 * for it to leave this way. */
		/* Standing in a depot is the one place a train can be turned round for
		 * nothing, because it has no extent there. On its way out it does: the
		 * moment it is started, it is a train partly in a world where it lies
		 * along the track, even while every vehicle is still hidden on the
		 * depot tile waiting its turn to come out. Turning it then leaves the
		 * vehicles still inside on the wrong side of those already out, and it
		 * jams on the first tile. Being stopped is exactly the line between
		 * the two, and it is the line the player sees: it is when the train
		 * stops saying it is stopped that turning it stops being free. */
		if (IsAnyPartInsideDepot(consist) && !(IsWholeTrainInsideDepot(consist) && consist->vehstatus.Test(VehState::Stopped))) return;

		if (IsWholeTrainInsideDepot(consist)) {
			/* Everything below works on where vehicles sit along the track and
			 * which tiles they occupy, none of which means anything for a
			 * train that is entirely inside a depot: its vehicles are hidden,
			 * all on the one tile. Doing none of it used to mean the reverse
			 * button simply did nothing here, which is unhelpful, because a
			 * depot is the one place where turning a train round is trivially
			 * safe.
			 *
			 * What has to change is which end leads out -- but a depot has one
			 * way out, and a train parked in one is always left with its
			 * leading end facing that way, whichever end that is. Flipping
			 * which end leads without turning the vehicles round leaves the new
			 * leading end facing the dead end instead, and the train drives
			 * straight at the back wall and stops there. So both are done, the
			 * same pair of mirrored operations a depot entrance chooses
			 * between, here applied together: swap the leading end, and turn
			 * every vehicle round so that end still faces out. See
			 * FEATURE_DESIGN_COUPLING_TOW.md. */
			consist->vehicle_flags.Flip(VehicleFlag::DrivingBackwards);
			for (Train *u = consist; u != nullptr; u = u->Next()) {
				u->direction = ReverseDir(u->direction);
			}
			consist->flags.Flip(VehicleRailFlag::Reversed);
			consist->flags.Reset(VehicleRailFlag::Reversing);
			consist->ConsistChanged(CCF_TRACK);
			for (Train *u = consist; u != nullptr; u = u->Next()) u->UpdateViewport(false, false);
			InvalidateWindowData(WindowClass::VehicleDepot, moving_front->tile);
			SetWindowDirty(WindowClass::VehicleView, consist->index);
			return;
		}
		InvalidateWindowData(WindowClass::VehicleDepot, moving_front->tile);
	}

	/* Clear path reservation in front if train is not stuck. */
	if (!consist->flags.Test(VehicleRailFlag::Stuck)) FreeTrainTrackReservation(consist);

	/* Check if we were approaching a rail/road-crossing */
	TileIndex crossing = TrainApproachingCrossingTile(moving_front);

	/* Check if we should back up or flip the train. */
	if (consist->vehicle_flags.Test(VehicleFlag::DrivingBackwards) || _settings_game.difficulty.train_flip_reverse_allowed == TrainFlipReversingAllowed::None || consist->Last()->CanLeadTrain()) {
		/* The train will back up. */
		consist->vehicle_flags.Flip(VehicleFlag::DrivingBackwards);

		for (Train *u = consist; u != nullptr; u = u->Next()) {
			/* Invert going up/down */
			if (u->gv_flags.Any({GroundVehicleFlag::GoingUp, GroundVehicleFlag::GoingDown})) {
				u->gv_flags.Flip({GroundVehicleFlag::GoingUp, GroundVehicleFlag::GoingDown});
			}
			UpdateStatusAfterSwap(u, false);
		}
		/* We may have entered a depot and stopped driving backwards. */
		moving_front = consist->GetMovingFront();
	} else {
		/* The train will flip. */
		AdvanceWagonsBeforeSwap(moving_front);

		/* swap start<>end, start+1<>end-1, ... */
		ReverseTrainSwapVehicles(consist);

		AdvanceWagonsAfterSwap(moving_front);
	}

	if (IsRailDepotTile(moving_front->tile)) {
		InvalidateWindowData(WindowClass::VehicleDepot, moving_front->tile);
	}

	consist->flags.Flip(VehicleRailFlag::Reversed);
	consist->flags.Reset(VehicleRailFlag::Reversing);

	/* recalculate cached data */
	consist->ConsistChanged(CCF_TRACK);

	/* update all images */
	for (Train *u = consist; u != nullptr; u = u->Next()) u->UpdateViewport(false, false);

	/* update crossing we were approaching */
	if (crossing != INVALID_TILE) UpdateLevelCrossing(crossing);

	/* maybe we are approaching crossing now, after reversal */
	crossing = TrainApproachingCrossingTile(moving_front);
	if (crossing != INVALID_TILE) MaybeBarCrossingWithSound(crossing);

	/* If we are inside a depot after reversing, don't bother with path reserving. */
	if (moving_front->track == Track::Depot) {
		/* Can't be stuck here as inside a depot is always a safe tile. */
		if (consist->flags.Test(VehicleRailFlag::Stuck)) SetWindowWidgetDirty(WindowClass::VehicleView, consist->index, WID_VV_START_STOP);
		consist->flags.Reset(VehicleRailFlag::Stuck);
		return;
	}

	/* VehicleExitDir does not always produce the desired dir for depots and
	 * tunnels/bridges that is needed for UpdateSignalsOnSegment. */
	DiagDirection dir = VehicleExitDir(moving_front->GetMovingDirection(), moving_front->track);
	if (IsRailDepotTile(moving_front->tile) || IsTileType(moving_front->tile, TileType::TunnelBridge)) dir = DiagDirection::Invalid;

	if (UpdateSignalsOnSegment(moving_front->tile, dir, consist->owner) == SigSegState::Path || _settings_game.pf.reserve_paths) {
		/* If we are currently on a tile with conventional signals, we can't treat the
		 * current tile as a safe tile or we would enter a PBS block without a reservation. */
		bool first_tile_okay = !HasBlockSignalOnTrackdir(moving_front->tile, moving_front->GetVehicleTrackdir());

		/* If we are on a depot tile facing outwards, do not treat the current tile as safe. */
		if (IsRailDepotTile(moving_front->tile) && TrackdirToExitdir(moving_front->GetVehicleTrackdir()) == GetRailDepotDirection(moving_front->tile)) first_tile_okay = false;

		if (IsRailStationTile(moving_front->tile)) SetRailStationPlatformReservation(moving_front->tile, TrackdirToExitdir(moving_front->GetVehicleTrackdir()), true);
		if (TryPathReserve(consist, false, first_tile_okay)) {
			/* Do a look-ahead now in case our current tile was already a safe tile. */
			CheckNextTrainTile(consist);
		} else if (consist->current_order.GetType() != OT_LOADING) {
			/* Do not wait for a way out when we're still loading */
			MarkTrainAsStuck(consist);
		}
	} else if (consist->flags.Test(VehicleRailFlag::Stuck)) {
		/* A train not inside a PBS block can't be stuck. */
		consist->flags.Reset(VehicleRailFlag::Stuck);
		consist->wait_counter = 0;
	}
}

/**
 * Reverse train.
 * @param flags type of operation
 * @param veh_id train to reverse
 * @param reverse_single_veh if true, reverse a unit in a train (needs to be in a depot)
 * @return the cost of this operation or an error
 */
CommandCost CmdReverseTrainDirection(DoCommandFlags flags, VehicleID veh_id, bool reverse_single_veh)
{
	Train *v = Train::GetIfValid(veh_id);
	if (v == nullptr) return CMD_ERROR;

	CommandCost ret = CheckOwnership(v->owner);
	if (ret.Failed()) return ret;

	if (reverse_single_veh) {
		/* turn a single unit around */

		if (v->IsMultiheaded() || EngInfo(v->engine_type)->callback_mask.Test(VehicleCallbackMask::ArticEngine)) {
			return CommandCost(STR_ERROR_CAN_T_REVERSE_DIRECTION_RAIL_VEHICLE_MULTIPLE_UNITS);
		}

		Train *front = v->First();
		/* make sure the vehicle is stopped in the depot */
		if (!front->IsStoppedInDepot()) {
			return CommandCost(STR_ERROR_TRAINS_CAN_ONLY_BE_ALTERED_INSIDE_A_DEPOT);
		}

		if (flags.Test(DoCommandFlag::Execute)) {
			v->flags.Flip(VehicleRailFlag::Flipped);

			front->ConsistChanged(CCF_ARRANGE);
			SetWindowDirty(WindowClass::VehicleDepot, front->tile);
			SetWindowDirty(WindowClass::VehicleDetails, front->index);
			InvalidateWindowData(WindowClass::VehicleView, front->index);
			SetWindowClassesDirty(WindowClass::TrainList);
		}
	} else {
		/* turn the whole train around */
		if (!v->IsPrimaryVehicle()) return CMD_ERROR;
		if (v->vehstatus.Test(VehState::Crashed) || v->breakdown_ctr != 0) return CMD_ERROR;

		if (flags.Test(DoCommandFlag::Execute)) {
			/* Properly leave the station if we are loading and won't be loading anymore */
			if (v->current_order.IsType(OT_LOADING)) {
				const Vehicle *moving_back = v->GetMovingBack();

				/* not a station || different station --> leave the station */
				if (!IsTileType(moving_back->tile, TileType::Station) || GetStationIndex(moving_back->tile) != GetStationIndex(v->GetMovingFront()->tile)) {
					v->LeaveStation();
				}
			}

			/* We cancel any 'skip signal at dangers' here */
			v->force_proceed = TFP_NONE;
			InvalidateWindowData(WindowClass::VehicleView, v->index);

			if (_settings_game.vehicle.train_acceleration_model != AccelerationModel::Original && v->cur_speed != 0) {
				v->flags.Flip(VehicleRailFlag::Reversing);
			} else {
				v->cur_speed = 0;
				v->SetLastSpeed();
				HideFillingPercent(&v->fill_percent_te_id);
				ReverseTrainDirection(v);
			}

			/* Unbunching data is no longer valid. */
			v->ResetDepotUnbunching();
		}
	}
	return CommandCost();
}

/**
 * Determine to what force_proceed should be changed.
 * If we are forced to proceed, cancel that order.
 * If we are marked stuck we would want to force the train to
 * proceed to the next signal unless we are stuck just before
 * the next signal. In the other cases we would like to pass
 * the signal at danger and run till the next signal we encounter.
 * @param t The train to determine the new value of force_proceed for.
 * @return The next state of force_proceed.
 */
static TrainForceProceeding DetermineNextTrainForceProceeding(const Train *t)
{
	if (t->vehstatus.Test(VehState::Crashed) || t->force_proceed == TFP_SIGNAL) return TFP_NONE;
	if (!t->flags.Test(VehicleRailFlag::Stuck)) return t->IsChainInDepot() ? TFP_STUCK : TFP_SIGNAL;

	const Train *moving_front = t->GetMovingFront();
	TileIndex next_tile = TileAddByDiagDir(moving_front->tile, TrackdirToExitdir(moving_front->GetVehicleTrackdir()));
	if (next_tile == INVALID_TILE || !IsTileType(next_tile, TileType::Railway) || !HasSignals(next_tile)) return TFP_STUCK;
	TrackBits new_tracks = DiagdirReachesTracks(TrackdirToExitdir(moving_front->GetVehicleTrackdir())) & GetTrackBits(next_tile);
	return new_tracks.Any() && HasSignalOnTrack(next_tile, FindFirstTrack(new_tracks)) ? TFP_SIGNAL : TFP_STUCK;
}

/**
 * Force a train through a red signal
 * @param flags type of operation
 * @param veh_id train to ignore the red signal
 * @return the cost of this operation or an error
 */
CommandCost CmdForceTrainProceed(DoCommandFlags flags, VehicleID veh_id)
{
	Train *t = Train::GetIfValid(veh_id);
	if (t == nullptr) return CMD_ERROR;

	if (!t->IsPrimaryVehicle()) return CMD_ERROR;

	CommandCost ret = CheckOwnership(t->owner);
	if (ret.Failed()) return ret;


	if (flags.Test(DoCommandFlag::Execute)) {
		t->force_proceed = DetermineNextTrainForceProceeding(t);
		InvalidateWindowData(WindowClass::VehicleView, t->index);

		/* Unbunching data is no longer valid. */
		t->ResetDepotUnbunching();
	}

	return CommandCost();
}

/**
 * Try to find a depot nearby.
 * @param v %Train that wants a depot.
 * @param max_distance Maximal search distance.
 * @return Information where the closest train depot is located.
 * @pre The given vehicle must not be crashed!
 */
static FindDepotData FindClosestTrainDepot(Train *v, int max_distance)
{
	assert(!v->vehstatus.Test(VehState::Crashed));

	return YapfTrainFindNearestDepot(v, max_distance);
}

ClosestDepot Train::FindClosestDepot()
{
	FindDepotData tfdd = FindClosestTrainDepot(this, 0);
	if (tfdd.best_length == UINT_MAX) return ClosestDepot();

	return ClosestDepot(tfdd.tile, GetDepotIndex(tfdd.tile), tfdd.reverse);
}

void Train::PlayLeaveStationSound(bool force) const
{
	static const SoundFx sfx[] = {
		SND_04_DEPARTURE_STEAM,
		SND_0A_DEPARTURE_TRAIN,
		SND_0A_DEPARTURE_TRAIN,
		SND_47_DEPARTURE_MONORAIL,
		SND_41_DEPARTURE_MAGLEV
	};

	if (PlayVehicleSound(this, VSE_START, force)) return;

	SndPlayVehicleFx(sfx[to_underlying(RailVehInfo(this->engine_type)->engclass)], this);
}

/**
 * Check if the train is on the last reserved tile and try to extend the path then.
 * @param consist %Train that needs its path extended.
 */
static void CheckNextTrainTile(Train *consist)
{
	/* Don't do any look-ahead if path_backoff_interval is 255. */
	if (_settings_game.pf.path_backoff_interval == 255) return;

	const Train *moving_front = consist->GetMovingFront();

	/* Exit if we are inside a depot. */
	if (moving_front->track == Track::Depot) return;

	switch (consist->current_order.GetType()) {
		/* Exit if we reached our destination depot. */
		case OT_GOTO_DEPOT:
			if (moving_front->tile == consist->dest_tile) return;
			break;

		case OT_GOTO_WAYPOINT:
			/* If we reached our waypoint, make sure we see that. */
			if (IsRailWaypointTile(moving_front->tile) && GetStationIndex(moving_front->tile) == consist->current_order.GetDestination()) ProcessOrders(consist);
			break;

		case OT_NOTHING:
		case OT_LEAVESTATION:
		case OT_LOADING:
			/* Exit if the current order doesn't have a destination, but the train has orders. */
			if (consist->GetNumOrders() > 0) return;
			break;

		default:
			break;
	}
	/* Exit if we are on a station tile and are going to stop. */
	if (IsRailStationTile(moving_front->tile) && consist->current_order.ShouldStopAtStation(consist, GetStationIndex(moving_front->tile))) return;

	Trackdir td = moving_front->GetVehicleTrackdir();

	/* On a tile with a red non-pbs signal, don't look ahead. */
	if (HasBlockSignalOnTrackdir(moving_front->tile, td) && GetSignalStateByTrackdir(moving_front->tile, td) == SignalState::Red) return;

	CFollowTrackRail ft(consist);
	if (!ft.Follow(moving_front->tile, td)) return;

	if (!HasReservedTracks(ft.new_tile, TrackdirBitsToTrackBits(ft.new_td_bits))) {
		/* Next tile is not reserved. */
		if (ft.new_td_bits.Count() == 1) {
			if (HasPbsSignalOnTrackdir(ft.new_tile, FindFirstTrackdir(ft.new_td_bits))) {
				/* If the next tile is a PBS signal, try to make a reservation. */
				TrackBits tracks = TrackdirBitsToTrackBits(ft.new_td_bits);
				if (Rail90DegTurnDisallowed(GetTileRailType(ft.old_tile), GetTileRailType(ft.new_tile))) {
					tracks.Reset(TrackCrossesTracks(TrackdirToTrack(ft.old_td)));
				}
				ChooseTrainTrack(consist, ft.new_tile, ft.exitdir, tracks, false, nullptr, false);
			}
		}
	}
}

/**
 * Will the train stay in the depot the next tick?
 * @param v %Train to check.
 * @return True if it stays in the depot, false otherwise.
 */
static bool CheckTrainStayInDepot(Train *v)
{
	/* bail out if not all wagons are in the same depot or not in a depot at all */
	for (const Train *u = v; u != nullptr; u = u->Next()) {
		if (u->track != Track::Depot || u->tile != v->tile) return false;
	}

	/* if the train got no power, then keep it in the depot */
	if (v->gcache.cached_power == 0) {
		v->vehstatus.Set(VehState::Stopped);
		SetWindowDirty(WindowClass::VehicleDepot, v->tile);
		return true;
	}

	/* Check if we should wait here for unbunching. */
	if (v->IsWaitingForUnbunching()) return true;

	SigSegState seg_state;

	if (v->force_proceed == TFP_NONE) {
		/* force proceed was not pressed */
		if (++v->wait_counter < 37) {
			SetWindowClassesDirty(WindowClass::TrainList);
			return true;
		}

		v->wait_counter = 0;

		seg_state = _settings_game.pf.reserve_paths ? SigSegState::Path : UpdateSignalsOnSegment(v->tile, DiagDirection::Invalid, v->owner);
		if (seg_state == SigSegState::Full || HasDepotReservation(v->tile)) {
			/* Full and no PBS signal in block or depot reserved, can't exit. */
			SetWindowClassesDirty(WindowClass::TrainList);
			return true;
		}
	} else {
		seg_state = _settings_game.pf.reserve_paths ? SigSegState::Path : UpdateSignalsOnSegment(v->tile, DiagDirection::Invalid, v->owner);
	}

	/* We are leaving a depot, but have to go to the exact same one; re-enter. */
	if (v->current_order.IsType(OT_GOTO_DEPOT) && v->tile == v->dest_tile) {
		/* Service when depot has no reservation. */
		if (!HasDepotReservation(v->tile)) VehicleEnterDepot(v);
		return true;
	}

	/* Only leave when we can reserve a path to our destination. */
	if (seg_state == SigSegState::Path && !TryPathReserve(v) && v->force_proceed == TFP_NONE) {
		/* No path and no force proceed. */
		SetWindowClassesDirty(WindowClass::TrainList);
		MarkTrainAsStuck(v);
		return true;
	}

	SetDepotReservation(v->tile, true);
	if (_settings_client.gui.show_track_reservation) MarkTileDirtyByTile(v->tile);

	VehicleServiceInDepot(v);
	v->LeaveUnbunchingDepot();
	v->PlayLeaveStationSound();
	SetWindowClassesDirty(WindowClass::TrainList);

	/* Whichever end is going to lead is the one that comes out, which is not
	 * always the head of the chain: a train that reversed in leads with its
	 * other end. Giving the track to and unhiding the head regardless is what
	 * used to force every train to be turned round on the way in, so that the
	 * assumption held. Everything below this that is about the train as a whole
	 * -- its speed, its acceleration -- still belongs to the head. See
	 * FEATURE_DESIGN_COUPLING_TOW.md. */
	Train *moving_front = v->GetMovingFront();

	moving_front->track = AxisToTrack(DiagDirToAxis(DirToDiagDir(moving_front->direction)));

	moving_front->vehstatus.Reset(VehState::Hidden);
	v->cur_speed = 0;

	moving_front->UpdateViewport(true, true);
	moving_front->UpdatePosition();
	UpdateSignalsOnSegment(v->tile, DiagDirection::Invalid, v->owner);
	v->UpdateAcceleration();
	InvalidateWindowData(WindowClass::VehicleDepot, v->tile);

	return false;
}

/**
 * Clear the reservation of \a tile that was just left by a wagon on \a track_dir.
 * @param v %Train owning the reservation.
 * @param tile Tile with reservation to clear.
 * @param track_dir Track direction to clear.
 */
static bool IsRailStationPlatformOccupied(TileIndex tile, const Train *ignore = nullptr);

static void ClearPathReservation(const Train *v, TileIndex tile, Trackdir track_dir)
{
	DiagDirection dir = TrackdirToExitdir(track_dir);

	if (IsTileType(tile, TileType::TunnelBridge)) {
		/* Are we just leaving a tunnel/bridge? */
		if (GetTunnelBridgeDirection(tile) == ReverseDiagDir(dir)) {
			TileIndex end = GetOtherTunnelBridgeEnd(tile);

			if (TunnelBridgeIsFree(tile, end, v).Succeeded()) {
				/* Free the reservation only if no other train is on the tiles. */
				SetTunnelBridgeReservation(tile, false);
				SetTunnelBridgeReservation(end, false);

				if (_settings_client.gui.show_track_reservation) {
					if (IsBridge(tile)) {
						MarkBridgeDirty(tile);
					} else {
						MarkTileDirtyByTile(tile);
						MarkTileDirtyByTile(end);
					}
				}
			}
		}
	} else if (IsRailStationTile(tile)) {
		TileIndex new_tile = TileAddByDiagDir(tile, dir);
		/* If the new tile is not a further tile of the same station, we
		 * clear the reservation for the whole platform -- but only if no
		 * OTHER train is still standing on it (mirrors the "only if free"
		 * check just above for tunnels/bridges, and the same check used
		 * when a wagon is deleted, see IsRailStationPlatformOccupied()).
		 * Without this, a "free wagon" chain left behind by a decouple
		 * order loses its PBS reservation the instant the rest of the
		 * train it was split from leaves the platform, even though it is
		 * still physically standing there. See
		 * FEATURE_DESIGN_COUPLING_TOW.md.
		 *
		 * "Other" is the operative word: v itself is still passing
		 * through/leaving the platform when this runs (this is called
		 * per-tile as its rear vacates each one), so on a platform longer
		 * than one tile, v's own remaining wagons are still physically on
		 * other platform tiles at this exact moment. Without excluding v,
		 * the occupancy check would see "occupied" (by v itself, mid-
		 * departure) and never clear the reservation behind ANY train on
		 * a multi-tile platform -- not just decoupled ones. */
		if (!IsCompatibleTrainStationTile(new_tile, tile) && !IsRailStationPlatformOccupied(tile, v)) {
			SetRailStationPlatformReservation(tile, ReverseDiagDir(dir), false);
		}
	} else {
		/* Any other tile */
		UnreserveRailTrack(tile, TrackdirToTrack(track_dir));
	}
}

/**
 * Free the reserved path in front of a vehicle.
 * @param consist %Train owning the reserved path.
 */
void FreeTrainTrackReservation(const Train *consist)
{
	/* A headless "free wagon" chain left behind by a decouple order (see
	 * FEATURE_DESIGN_COUPLING_TOW.md) can now be found here via
	 * GetTrainForReservation() from track/tunnel/station edit commands --
	 * something that could never happen in vanilla, where every standalone
	 * train on open track was guaranteed to have a front engine. Such a
	 * chain isn't moving and isn't following a path forward; its own
	 * under-body reservation is set once by ReserveTrackUnderConsist() and
	 * isn't this function's concern, so just no-op instead of asserting. */
	if (!consist->IsFrontEngine()) return;

	const Train *moving_front = consist->GetMovingFront();
	TileIndex tile = moving_front->tile;
	Trackdir td = moving_front->GetVehicleTrackdir();
	bool free_tile = tile != moving_front->tile || !(IsRailStationTile(moving_front->tile) || IsTileType(moving_front->tile, TileType::TunnelBridge));
	StationID station_id = IsRailStationTile(moving_front->tile) ? GetStationIndex(moving_front->tile) : StationID::Invalid();

	/* Can't be holding a reservation if we enter a depot. */
	if (IsRailDepotTile(tile) && TrackdirToExitdir(td) != GetRailDepotDirection(tile)) return;
	if (moving_front->track == Track::Depot) {
		/* Front engine is in a depot. We enter if some part is not in the depot. */
		for (const Train *u = consist; u != nullptr; u = u->Next()) {
			if (u->track != Track::Depot || u->tile != consist->tile) return;
		}
	}
	/* Don't free reservation if it's not ours. */
	if (TracksOverlap(GetReservedTrackbits(tile) | TrackdirToTrack(td))) return;

	CFollowTrackRail ft(consist, GetAllCompatibleRailTypes(consist->railtypes));
	while (ft.Follow(tile, td)) {
		tile = ft.new_tile;
		TrackdirBits bits = ft.new_td_bits & TrackBitsToTrackdirBits(GetReservedTrackbits(tile));
		td = RemoveFirstTrackdir(bits);
		assert(bits.None());

		if (!IsValidTrackdir(td)) break;

		if (IsTileType(tile, TileType::Railway)) {
			if (HasSignalOnTrackdir(tile, td) && !IsPbsSignal(GetSignalType(tile, TrackdirToTrack(td)))) {
				/* Conventional signal along trackdir: remove reservation and stop. */
				UnreserveRailTrack(tile, TrackdirToTrack(td));
				break;
			}
			if (HasPbsSignalOnTrackdir(tile, td)) {
				if (GetSignalStateByTrackdir(tile, td) == SignalState::Red) {
					/* Red PBS signal? Can't be our reservation, would be green then. */
					break;
				} else {
					/* Turn the signal back to red. */
					SetSignalStateByTrackdir(tile, td, SignalState::Red);
					MarkTileDirtyByTile(tile);
				}
			} else if (HasPbsSignalOnTrackdir(tile, ReverseTrackdir(td))) {
				/* Reservation passes an opposing path signal. Mark signal for update to re-establish the proper default state. */
				AddSideToSignalBuffer(tile, TrackdirToExitdir(ReverseTrackdir(td)), consist->owner);
			} else if (HasSignalOnTrackdir(tile, ReverseTrackdir(td)) && IsOnewaySignal(tile, TrackdirToTrack(td))) {
				break;
			}
		}

		/* Don't free first station/bridge/tunnel if we are on it. */
		if (free_tile || (!(ft.is_station && GetStationIndex(ft.new_tile) == station_id) && !ft.is_tunnel && !ft.is_bridge)) ClearPathReservation(consist, tile, td);

		free_tile = true;
	}

	UpdateSignalsInBuffer();
}

/**
 * Perform pathfinding for a train.
 *
 * @param v The train
 * @param tile The tile the train is about to enter
 * @param enterdir Diagonal direction the train is coming from
 * @param tracks Usable tracks on the new tile
 * @param[out] path_found Whether a path has been found or not.
 * @param do_track_reservation Path reservation is requested
 * @param[out] dest State and destination of the requested path
 * @param[out] final_dest Final tile of the best path found
 * @return The best track the train should follow
 */
static Track DoTrainPathfind(const Train *v, TileIndex tile, DiagDirection enterdir, TrackBits tracks, bool &path_found, bool do_track_reservation, PBSTileInfo *dest, TileIndex *final_dest)
{
	if (final_dest != nullptr) *final_dest = INVALID_TILE;
	return YapfTrainChooseTrack(v, tile, enterdir, tracks, path_found, do_track_reservation, dest, final_dest);
}

/**
 * Extend a train path as far as possible. Stops on encountering a safe tile,
 * another reservation or a track choice.
 * @param v The train to extend the reservation for.
 * @param new_tracks Optional pointer to the track bits at the end of the reservation.
 * @param enterdir Optional pointer to the enter dir at the end of the reservation.
 * @return Information about the reservation, empty if no path could be found.
 */
static PBSTileInfo ExtendTrainReservation(const Train *v, TrackBits *new_tracks, DiagDirection *enterdir)
{
	PBSTileInfo origin = FollowTrainReservation(v);

	CFollowTrackRail ft(v);

	std::vector<std::pair<TileIndex, Trackdir>> signals_set_to_red;

	TileIndex tile = origin.tile;
	Trackdir  cur_td = origin.trackdir;
	while (ft.Follow(tile, cur_td)) {
		if (ft.new_td_bits.Count() == 1) {
			/* Possible signal tile. */
			if (HasOnewaySignalBlockingTrackdir(ft.new_tile, FindFirstTrackdir(ft.new_td_bits))) break;
		}

		if (Rail90DegTurnDisallowed(GetTileRailType(ft.old_tile), GetTileRailType(ft.new_tile))) {
			ft.new_td_bits.Reset(TrackdirCrossesTrackdirs(ft.old_td));
			if (ft.new_td_bits.None()) break;
		}

		/* Station, depot or waypoint are a possible target. */
		bool target_seen = ft.is_station || (IsTileType(ft.new_tile, TileType::Railway) && !IsPlainRail(ft.new_tile));
		if (target_seen || ft.new_td_bits.Count() > 1) {
			/* Choice found or possible target encountered.
			 * On finding a possible target, we need to stop and let the pathfinder handle the
			 * remaining path. This is because we don't know if this target is in one of our
			 * orders, so we might cause pathfinding to fail later on if we find a choice.
			 * This failure would cause a bogus call to TryReserveSafePath which might reserve
			 * a wrong path not leading to our next destination. */
			if (HasReservedTracks(ft.new_tile, TrackdirBitsToTrackBits(TrackdirReachesTrackdirs(ft.old_td)))) break;

			/* If we did skip some tiles, backtrack to the first skipped tile so the pathfinder
			 * actually starts its search at the first unreserved tile. */
			if (ft.tiles_skipped != 0) ft.new_tile -= TileOffsByDiagDir(ft.exitdir) * ft.tiles_skipped;

			/* Choice found, path valid but not okay. Save info about the choice tile as well. */
			if (new_tracks != nullptr) *new_tracks = TrackdirBitsToTrackBits(ft.new_td_bits);
			if (enterdir != nullptr) *enterdir = ft.exitdir;
			return PBSTileInfo(ft.new_tile, ft.old_td, false);
		}

		tile = ft.new_tile;
		cur_td = FindFirstTrackdir(ft.new_td_bits);

		Trackdir rev_td = ReverseTrackdir(cur_td);
		if (IsSafeWaitingPosition(v, tile, cur_td, true, _settings_game.pf.forbid_90_deg)) {
			bool wp_free = IsWaitingPositionFree(v, tile, cur_td, _settings_game.pf.forbid_90_deg);
			if (!(wp_free && TryReserveRailTrack(tile, TrackdirToTrack(cur_td)))) break;
			/* Green path signal opposing the path? Turn to red. */
			if (HasPbsSignalOnTrackdir(tile, rev_td) && GetSignalStateByTrackdir(tile, rev_td) == SignalState::Green) {
				signals_set_to_red.emplace_back(tile, rev_td);
				SetSignalStateByTrackdir(tile, rev_td, SignalState::Red);
				MarkTileDirtyByTile(tile);
			}
			/* Safe position is all good, path valid and okay. */
			return PBSTileInfo(tile, cur_td, true);
		}

		if (!TryReserveRailTrack(tile, TrackdirToTrack(cur_td))) break;

		/* Green path signal opposing the path? Turn to red. */
		if (HasPbsSignalOnTrackdir(tile, rev_td) && GetSignalStateByTrackdir(tile, rev_td) == SignalState::Green) {
			signals_set_to_red.emplace_back(tile, rev_td);
			SetSignalStateByTrackdir(tile, rev_td, SignalState::Red);
			MarkTileDirtyByTile(tile);
		}
	}

	if (ft.err == CFollowTrackRail::ErrorCode::Owner || ft.err == CFollowTrackRail::ErrorCode::NoWay) {
		/* End of line, path valid and okay. */
		return PBSTileInfo(ft.old_tile, ft.old_td, true);
	}

	/* Sorry, can't reserve path, back out. */
	tile = origin.tile;
	cur_td = origin.trackdir;
	TileIndex stopped = ft.old_tile;
	Trackdir  stopped_td = ft.old_td;
	while (tile != stopped || cur_td != stopped_td) {
		if (!ft.Follow(tile, cur_td)) break;

		if (Rail90DegTurnDisallowed(GetTileRailType(ft.old_tile), GetTileRailType(ft.new_tile))) {
			ft.new_td_bits.Reset(TrackdirCrossesTrackdirs(ft.old_td));
			assert(ft.new_td_bits.Any());
		}
		assert(ft.new_td_bits.Count() == 1);

		tile = ft.new_tile;
		cur_td = FindFirstTrackdir(ft.new_td_bits);

		UnreserveRailTrack(tile, TrackdirToTrack(cur_td));
	}

	/* Re-instate green signals we turned to red. */
	for (auto [sig_tile, td] : signals_set_to_red) {
		SetSignalStateByTrackdir(sig_tile, td, SignalState::Green);
	}

	/* Path invalid. */
	return PBSTileInfo();
}

/**
 * Release a reservation that starts at @p tile but that no train's own
 * reservation connects to, and which #FreeTrainTrackReservation therefore can
 * never reach: that function only ever walks forward from a train's own
 * position, so it cleans up nothing that isn't contiguous with the train.
 *
 * Normally no such reservation can exist -- the pathfinder is only ever asked
 * to reserve onward from a point #ExtendTrainReservation already reserved
 * contiguously from the train. This feature breaks that invariant on purpose
 * in one place (see #ChooseTrainTrack: when the quick, choice-free walk of
 * ExtendTrainReservation is blocked outright by a decoupled wagon chain, the
 * full pathfinder is asked to look for a way around starting from a tile the
 * train hasn't reached yet), so it also has to be able to undo it. Leaving it
 * behind is what showed up in-game as a phantom reserved track sticking out of
 * a station past decoupled wagons, with nothing standing on it. See
 * FEATURE_DESIGN_COUPLING_TOW.md.
 *
 * @param v The train the (failed) reservation was being made for.
 * @param tile First tile of the orphaned reservation.
 * @param enterdir Direction the path would have been entered from.
 */
static void FreeOrphanedReservation(const Train *v, TileIndex tile, DiagDirection enterdir)
{
	TrackBits res = GetReservedTrackbits(tile) & DiagdirReachesTracks(enterdir);
	if (res.None()) return;

	CFollowTrackRail ft(v);
	TileIndex cur_tile = tile;
	Trackdir cur_td = TrackEnterdirToTrackdir(FindFirstTrack(res), enterdir);

	/* Bounded purely as a belt-and-braces guard against a pathological
	 * looping layout; a reservation is always finite in practice. */
	for (uint safety = 0; safety < 1024; safety++) {
		/* Never strip the reservation off a tile a train is standing on.
		 * A stationary consist keeps its own tiles reserved from underneath
		 * itself (Train::ReserveTrackUnderConsist()), and that reservation is
		 * contiguous with whatever the speculative path reserved up to it, so
		 * without this the walk runs straight on under a waiting train and
		 * unreserves it tile by tile. The train stays where it is but becomes
		 * invisible to every signal and pathfinder that asks the map instead
		 * of the vehicle list: the platform reads free, the protecting signal
		 * turns green, and the next train is routed into it and crashes.
		 * Anything reserved from an occupied tile onwards was never ours
		 * anyway, so stopping here loses nothing. */
		if (EnsureNoTrainOnTrackBits(cur_tile, TrackBits{TrackdirToTrack(cur_td)}).Failed()) break;

		UnreserveRailTrack(cur_tile, TrackdirToTrack(cur_td));

		if (!ft.Follow(cur_tile, cur_td)) break;
		/* Stop at any choice: beyond a junction we can no longer tell
		 * which way (if any) the orphaned path actually went. */
		if (ft.new_td_bits.Count() != 1) break;

		cur_tile = ft.new_tile;
		cur_td = FindFirstTrackdir(ft.new_td_bits);

		/* Stop as soon as the path stops being reserved -- anything past
		 * that point was never ours. */
		if (!HasReservedTracks(cur_tile, TrackBits{TrackdirToTrack(cur_td)})) break;
	}
}

/**
 * Try to reserve any path to a safe tile, ignoring the vehicle's destination.
 * Safe tiles are tiles in front of a signal, depots and station tiles at end of line.
 *
 * @param v The vehicle.
 * @param tile The tile the search should start from.
 * @param td The trackdir the search should start from.
 * @param override_railtype Whether all physically compatible railtypes should be followed.
 * @return True if a path to a safe stopping tile could be reserved.
 */
static bool TryReserveSafeTrack(const Train *v, TileIndex tile, Trackdir td, bool override_railtype)
{
	return YapfTrainFindNearestSafeTile(v, tile, td, override_railtype);
}

/** This class will save the current order of a vehicle and restore it on destruction. */
class VehicleOrderSaver {
private:
	Train          *v;
	Order          old_order;
	TileIndex      old_dest_tile;
	StationID      old_last_station_visited;
	VehicleOrderID index;
	bool           suppress_implicit_orders;
	bool           restored;

public:
	VehicleOrderSaver(Train *_v) :
		v(_v),
		old_order(_v->current_order),
		old_dest_tile(_v->dest_tile),
		old_last_station_visited(_v->last_station_visited),
		index(_v->cur_real_order_index),
		suppress_implicit_orders(_v->gv_flags.Test(GroundVehicleFlag::SuppressImplicitOrders)),
		restored(false)
	{
	}

	/**
	 * Restore the saved order to the vehicle.
	 */
	void Restore()
	{
		this->v->current_order = this->old_order;
		this->v->dest_tile = this->old_dest_tile;
		this->v->last_station_visited = this->old_last_station_visited;
		this->v->gv_flags.Set(GroundVehicleFlag::SuppressImplicitOrders, suppress_implicit_orders);
		this->restored = true;
	}

	/**
	 * Restore the saved order to the vehicle, if Restore() has not already been called.
	 */
	~VehicleOrderSaver()
	{
		if (!this->restored) this->Restore();
	}

	/**
	 * Set the current vehicle order to the next order in the order list.
	 * @param skip_first Shall the first (i.e. active) order be skipped?
	 * @return True if a suitable next order could be found.
	 */
	bool SwitchToNextOrder(bool skip_first)
	{
		if (this->v->GetNumOrders() == 0) return false;

		if (skip_first) ++this->index;

		int depth = 0;

		do {
			/* Wrap around. */
			if (this->index >= this->v->GetNumOrders()) this->index = 0;

			Order *order = this->v->GetOrder(this->index);
			assert(order != nullptr);

			switch (order->GetType()) {
				case OT_GOTO_DEPOT:
					/* Skip service in depot orders when the train doesn't need service. */
					if (order->GetDepotOrderType().Test(OrderDepotTypeFlag::Service) && !this->v->NeedsServicing()) break;
					[[fallthrough]];
				case OT_GOTO_STATION:
				case OT_GOTO_WAYPOINT:
					this->v->current_order = *order;
					return UpdateOrderDest(this->v, order, 0, true);
				case OT_CONDITIONAL: {
					VehicleOrderID next = ProcessConditionalOrder(order, this->v);
					if (next != INVALID_VEH_ORDER_ID) {
						depth++;
						this->index = next;
						/* Don't increment next, so no break here. */
						continue;
					}
					break;
				}
				default:
					break;
			}
			/* Don't increment inside the while because otherwise conditional
			 * orders can lead to an infinite loop. */
			++this->index;
			depth++;
		} while (this->index != this->v->cur_real_order_index && depth < this->v->GetNumOrders());

		return false;
	}
};

/* choose a track */
static Track ChooseTrainTrack(Train *consist, TileIndex tile, DiagDirection enterdir, TrackBits tracks, bool force_res, bool *got_reservation, bool mark_stuck)
{
	Track best_track = Track::Invalid;
	bool do_track_reservation = _settings_game.pf.reserve_paths || force_res;
	bool changed_signal = false;
	TileIndex final_dest = INVALID_TILE;

	assert(tracks == (tracks & TRACK_BIT_ALL));

	if (got_reservation != nullptr) *got_reservation = false;

	/* ExtendTrainReservation() below OVERWRITES `tracks` with the track
	 * bits of whatever choice tile it stopped at, which is a *different*
	 * tile further along the path -- not `tile`. Our caller
	 * (TrainController) asserts that whatever Track we return actually
	 * exists on `tile`, so every "give up, but hand back a usable
	 * placeholder" return below has to use the original set, not the
	 * overwritten one. Vanilla never hit this because its only such return
	 * sits before the overwrite can happen (ExtendTrainReservation leaves
	 * `tracks` untouched when it fails outright); the extra give-up paths
	 * this feature added are all *after* it. See
	 * FEATURE_DESIGN_COUPLING_TOW.md. */
	const TrackBits tracks_on_tile = tracks;

	/* Set when we ask the pathfinder to reserve onward from a tile the
	 * train hasn't reached yet, leaving a reservation that isn't
	 * contiguous with the train. See where it is set, and
	 * FreeOrphanedReservation(). */
	bool speculative_reservation = false;

	/* Don't use tracks here as the setting to forbid 90 deg turns might have been switched between reservation and now. */
	TrackBits res_tracks = GetReservedTrackbits(tile) & DiagdirReachesTracks(enterdir);
	/* Do we have a suitable reserved track?
	 *
	 * This is what keeps a train on the path it reserved: having planned and
	 * reserved a route, at every tile of it the train simply takes the track
	 * that is already reserved instead of asking the pathfinder again. Falling
	 * through to the pathfinder here is not a cautious choice, it is a
	 * different route -- the search runs afresh, returns whatever looks best
	 * from this tile now, and the train drives off its own reserved path and
	 * onto a track nothing has secured for it. Which is how a train that had
	 * correctly reserved its way out of a station drove straight on into a
	 * one-way line and hit an oncoming one.
	 *
	 * So no condition belongs here. Refusing to enter a tile is a separate
	 * matter from choosing a track and is handled where the train physically
	 * enters one (see the free-wagon check in TrainController), which is the
	 * only place that can refuse without sending the train somewhere else
	 * instead. */
	if (res_tracks.Any()) return FindFirstTrack(res_tracks);

	/* Quick return in case only one possible track is available */
	if (tracks.Count() == 1) {
		Track track = FindFirstTrack(tracks);
		/* We need to check for signals only here, as a junction tile can't have signals. */
		if (IsValidTrack(track) && HasPbsSignalOnTrackdir(tile, TrackEnterdirToTrackdir(track, enterdir))) {
			do_track_reservation = true;
			changed_signal = true;
			SetSignalStateByTrackdir(tile, TrackEnterdirToTrackdir(track, enterdir), SignalState::Green);
		} else if (!do_track_reservation) {
			return track;
		}
		best_track = track;
	}

	const Train *moving_front = consist->GetMovingFront();

	PBSTileInfo   res_dest(tile, Trackdir::Invalid, false);
	DiagDirection dest_enterdir = enterdir;
	if (do_track_reservation) {
		res_dest = ExtendTrainReservation(consist, &tracks, &dest_enterdir);
		if (res_dest.tile == INVALID_TILE) {
			/* ExtendTrainReservation() only walks a single, choice-free
			 * track run and gives up outright the moment it can't
			 * reserve the very next tile -- by design, since in vanilla
			 * a blocked straight run with no junction in between
			 * genuinely has no alternative route. That assumption
			 * breaks for a headless "free wagon" chain (left behind by
			 * a decouple order) sitting directly ahead with no junction
			 * in between: there is often still a way around, via an
			 * earlier junction this quick walk never got the chance to
			 * consider. Retry from our actual current position with the
			 * full pathfinder (handled uniformly below, same as the
			 * "found a target, but it wasn't safe" case) instead of
			 * giving up immediately -- if that also fails, we still end
			 * up correctly marked stuck a few lines down. See
			 * FEATURE_DESIGN_COUPLING_TOW.md.
			 *
			 * This makes the pathfind below start from a tile the train
			 * has NOT reached yet, with nothing of ours reserved leading
			 * up to it -- so whatever it reserves is not contiguous with
			 * the train and FreeTrainTrackReservation() can't clean it up
			 * if we end up giving up. Remember that, so the give-up path
			 * can release it explicitly instead of leaving a phantom
			 * reserved track behind. */
			res_dest = PBSTileInfo(tile, Trackdir::Invalid, false);
			dest_enterdir = enterdir;
			speculative_reservation = true;
		} else if (res_dest.okay) {
			/* Got a valid reservation that ends at a safe target, quick exit. */
			if (got_reservation != nullptr) *got_reservation = true;
			if (changed_signal) MarkTileDirtyByTile(tile);
			TryReserveRailTrack(moving_front->tile, TrackdirToTrack(moving_front->GetVehicleTrackdir()));
			return best_track;
		}

		/* Check if the train needs service here, so it has a chance to always find a depot.
		 * Also check if the current order is a service order so we don't reserve a path to
		 * the destination but instead to the next one if service isn't needed. */
		CheckIfTrainNeedsService(consist);
		if (consist->current_order.IsType(OT_DUMMY) || consist->current_order.IsType(OT_CONDITIONAL) || consist->current_order.IsType(OT_GOTO_DEPOT)) ProcessOrders(consist);
	}

	/* Save the current train order. The destructor will restore the old order on function exit. */
	VehicleOrderSaver orders(consist);

	/* If the current tile is the destination of the current order and
	 * a reservation was requested, advance to the next order.
	 * Don't advance on a depot order as depots are always safe end points
	 * for a path and no look-ahead is necessary. This also avoids a
	 * problem with depot orders not part of the order list when the
	 * order list itself is empty. */
	if (consist->current_order.IsType(OT_LEAVESTATION)) {
		orders.SwitchToNextOrder(false);
	} else if (consist->current_order.IsType(OT_LOADING) || (!consist->current_order.IsType(OT_GOTO_DEPOT) && (
			consist->current_order.IsType(OT_GOTO_STATION) ?
			IsRailStationTile(moving_front->tile) && consist->current_order.GetDestination() == GetStationIndex(moving_front->tile) :
			moving_front->tile == consist->dest_tile))) {
		orders.SwitchToNextOrder(true);
	}

	if (res_dest.tile != INVALID_TILE && !res_dest.okay) {
		/* A "go to couple" order deliberately goes through the ordinary
		 * station pathfinder below, exactly like any other "go to station"
		 * order. What makes it find the partner rather than the first free
		 * platform is that the destination test itself is narrowed to the
		 * platform the partner stands on (see couple_at_dest_station in
		 * CYapfDestinationTileOrStationRailT, yapf_destrail.hpp) -- so the
		 * whole journey, across junctions and signals and however far away
		 * the station is, is planned by the one search that is actually
		 * built for travelling to a station. An earlier attempt to instead
		 * divert go-to-couple into a dedicated "stop next to a partner"
		 * search here was a dead end: that search looks for a tile
		 * immediately adjacent to the partner, which is a local
		 * nearest-safe-tile query and cannot navigate to a station on the
		 * other side of the map, so the train simply never set off. See
		 * FEATURE_DESIGN_COUPLING_TOW.md. */

		/* Pathfinders are able to tell that route was only 'guessed'. */
		bool      path_found = true;
		TileIndex new_tile = res_dest.tile;

		Track next_track = DoTrainPathfind(consist, new_tile, dest_enterdir, tracks, path_found, do_track_reservation, &res_dest, &final_dest);
		if (new_tile == tile) best_track = next_track;
		consist->HandlePathfindingResult(path_found);
	}

	/* No track reservation requested -> finished. */
	if (!do_track_reservation) return best_track;

	/* A path was found, but could not be reserved. */
	if (res_dest.tile != INVALID_TILE && !res_dest.okay) {
		if (mark_stuck) MarkTrainAsStuck(consist);
		FreeTrainTrackReservation(consist);
		if (speculative_reservation) FreeOrphanedReservation(consist, tile, enterdir);
		/* Use tracks_on_tile, not best_track and not the (by now
		 * overwritten) `tracks`: best_track can still be Track::Invalid
		 * here, and `tracks` may describe a different tile entirely --
		 * both fail our caller's "did we get a usable track" assert. */
		return FindFirstTrack(tracks_on_tile);
	}

	/* No possible reservation target found, we are probably lost. */
	if (res_dest.tile == INVALID_TILE) {
		/* Try to find any safe destination. */
		PBSTileInfo origin = FollowTrainReservation(consist);
		if (TryReserveSafeTrack(consist, origin.tile, origin.trackdir, false)) {
			TrackBits res = GetReservedTrackbits(tile) & DiagdirReachesTracks(enterdir);
			best_track = FindFirstTrack(res);
			TryReserveRailTrack(moving_front->tile, TrackdirToTrack(moving_front->GetVehicleTrackdir()));
			if (got_reservation != nullptr) *got_reservation = true;
			if (changed_signal) MarkTileDirtyByTile(tile);
			return best_track;
		}
		FreeTrainTrackReservation(consist);
		if (speculative_reservation) FreeOrphanedReservation(consist, tile, enterdir);
		if (mark_stuck) MarkTrainAsStuck(consist);
		/* See the tracks_on_tile comments above. */
		return FindFirstTrack(tracks_on_tile);
	}

	if (got_reservation != nullptr) *got_reservation = true;

	/* Reservation target found and free, check if it is safe. */
	while (!IsSafeWaitingPosition(consist, res_dest.tile, res_dest.trackdir, true, _settings_game.pf.forbid_90_deg)) {
		/* Extend reservation until we have found a safe position. */
		DiagDirection exitdir = TrackdirToExitdir(res_dest.trackdir);
		TileIndex next_tile = TileAddByDiagDir(res_dest.tile, exitdir);
		TrackBits reachable = TrackdirBitsToTrackBits(GetTileTrackStatus(next_tile, TransportType::Rail, RoadTramType::Invalid).trackdirs) & DiagdirReachesTracks(exitdir);
		if (Rail90DegTurnDisallowed(GetTileRailType(res_dest.tile), GetTileRailType(next_tile))) {
			reachable.Reset(TrackCrossesTracks(TrackdirToTrack(res_dest.trackdir)));
		}

		/* Get next order with destination. */
		if (orders.SwitchToNextOrder(true)) {
			PBSTileInfo cur_dest;
			bool path_found;
			DoTrainPathfind(consist, next_tile, exitdir, reachable, path_found, true, &cur_dest, nullptr);
			if (cur_dest.tile != INVALID_TILE) {
				res_dest = cur_dest;
				if (res_dest.okay) continue;
				/* Path found, but could not be reserved. */
				FreeTrainTrackReservation(consist);
				if (mark_stuck) MarkTrainAsStuck(consist);
				if (got_reservation != nullptr) *got_reservation = false;
				changed_signal = false;
				break;
			}
		}
		/* No order or no safe position found, try any position. */
		if (!TryReserveSafeTrack(consist, res_dest.tile, res_dest.trackdir, true)) {
			FreeTrainTrackReservation(consist);
			if (mark_stuck) MarkTrainAsStuck(consist);
			if (got_reservation != nullptr) *got_reservation = false;
			changed_signal = false;
		}
		break;
	}

	TryReserveRailTrack(moving_front->tile, TrackdirToTrack(moving_front->GetVehicleTrackdir()));

	if (changed_signal) MarkTileDirtyByTile(tile);

	orders.Restore();
	if (consist->current_order.IsType(OT_GOTO_DEPOT) &&
			consist->current_order.GetDepotActionType().Test(OrderDepotActionFlag::NearestDepot) &&
			final_dest != INVALID_TILE && IsRailDepotTile(final_dest)) {
		consist->current_order.SetDestination(GetDepotIndex(final_dest));
		consist->dest_tile = final_dest;
		SetWindowWidgetDirty(WindowClass::VehicleView, consist->index, WID_VV_START_STOP);
	}

	return best_track;
}

/**
 * Try to reserve a path to a safe position.
 *
 * @param consist The vehicle
 * @param mark_as_stuck Should the train be marked as stuck on a failed reservation?
 * @param first_tile_okay True if no path should be reserved if the current tile is a safe position.
 * @return True if a path could be reserved.
 */
bool TryPathReserve(Train *consist, bool mark_as_stuck, bool first_tile_okay)
{
	/* See the matching comment in FreeTrainTrackReservation(): a headless
	 * "free wagon" chain can reach here the same way (e.g. via
	 * GetTrainForReservation() when nearby track is edited). It isn't
	 * driving anywhere, so there's no path ahead of it to reserve --
	 * treat that as trivial success rather than asserting or marking it
	 * stuck. See FEATURE_DESIGN_COUPLING_TOW.md. */
	if (!consist->IsFrontEngine()) return true;

	const Train *moving_front = consist->GetMovingFront();

	/* We have to handle depots specially as the track follower won't look
	 * at the depot tile itself but starts from the next tile. If we are still
	 * inside the depot, a depot reservation can never be ours. */
	if (moving_front->track == Track::Depot) {
		if (HasDepotReservation(moving_front->tile)) {
			if (mark_as_stuck) MarkTrainAsStuck(consist);
			return false;
		} else {
			/* Depot not reserved, but the next tile might be. */
			TileIndex next_tile = TileAddByDiagDir(moving_front->tile, GetRailDepotDirection(moving_front->tile));
			if (HasReservedTracks(next_tile, DiagdirReachesTracks(GetRailDepotDirection(moving_front->tile)))) return false;
		}
	}

	Vehicle *other_train = nullptr;
	PBSTileInfo origin = FollowTrainReservation(consist, &other_train);
	/* The path we are driving on is already blocked by some other train.
	 * This can only happen in certain situations when mixing path and
	 * block signals or when changing tracks and/or signals.
	 * Exit here as doing any further reservations will probably just
	 * make matters worse. */
	if (other_train != nullptr && other_train->index != consist->index) {
		/* Blocked by the very train we were sent to fetch is not being blocked
		 * at all -- it is arriving. A casualty has an engine at its head, so
		 * "blocked by a real train, nothing to do but wait" is exactly what
		 * this reads as, and waiting is the one thing that cannot work: what
		 * is in the way is broken down and is never going to move. The engine
		 * was marked stuck at the last signal before it and stood there for
		 * good. Give the search below the same fresh chance a headless rake
		 * gets; it stops the path short of the casualty and coupling takes
		 * over from there. See FEATURE_DESIGN_COUPLING_TOW.md. */
		if (Train::From(other_train)->First()->IsFrontEngine() &&
				!IsValidCouplePartner(consist, Train::From(other_train)->First())) {
			/* Genuinely blocked by a real, driving train -- nothing
			 * useful to do but wait, exactly as before. */
			if (mark_as_stuck) MarkTrainAsStuck(consist);
			return false;
		}
		/* Blocked only by a headless "free wagon" chain left behind by
		 * a decouple order: unlike a real train, there is often a way
		 * around. Give the full pathfinder in ChooseTrainTrack() a
		 * fresh chance to find one from our actual current position
		 * (not the stale `origin` above, which can extend right into
		 * the wagon's own tiles, since reservation is a plain per-tile
		 * boolean with no owner and the wagon's reservation reads as
		 * contiguous with ours) -- it now treats a headless wagon's
		 * tile as effectively impassable during the search itself,
		 * instead of giving up immediately the way this function
		 * otherwise would. Mirrors the "reservation found, try to
		 * extend it" call a few lines below. See
		 * FEATURE_DESIGN_COUPLING_TOW.md. */
		DiagDirection exitdir = TrackdirToExitdir(moving_front->GetVehicleTrackdir());
		TileIndex new_tile = TileAddByDiagDir(moving_front->tile, exitdir);
		TrackBits reachable = TrackdirBitsToTrackBits(GetTileTrackStatus(new_tile, TransportType::Rail, RoadTramType::Invalid).trackdirs & DiagdirReachesTrackdirs(exitdir));
		if (Rail90DegTurnDisallowed(GetTileRailType(moving_front->tile), GetTileRailType(new_tile))) {
			reachable.Reset(TrackCrossesTracks(TrackdirToTrack(moving_front->GetVehicleTrackdir())));
		}

		bool res_made = false;
		ChooseTrainTrack(consist, new_tile, exitdir, reachable, true, &res_made, mark_as_stuck);
		if (!res_made) return false;

		if (consist->flags.Test(VehicleRailFlag::Stuck)) {
			consist->wait_counter = 0;
			SetWindowWidgetDirty(WindowClass::VehicleView, consist->index, WID_VV_START_STOP);
		}
		consist->flags.Reset(VehicleRailFlag::Stuck);
		return true;
	}
	/* If we have a reserved path and the path ends at a safe tile, we are finished already. */
	if (origin.okay && (moving_front->tile != origin.tile || first_tile_okay)) {
		/* Can't be stuck then. */
		if (consist->flags.Test(VehicleRailFlag::Stuck)) SetWindowWidgetDirty(WindowClass::VehicleView, consist->index, WID_VV_START_STOP);
		consist->flags.Reset(VehicleRailFlag::Stuck);
		return true;
	}

	/* If we are in a depot, tentatively reserve the depot. */
	if (moving_front->track == Track::Depot) {
		SetDepotReservation(moving_front->tile, true);
		if (_settings_client.gui.show_track_reservation) MarkTileDirtyByTile(moving_front->tile);
	}

	DiagDirection exitdir = TrackdirToExitdir(origin.trackdir);
	TileIndex new_tile = TileAddByDiagDir(origin.tile, exitdir);
	TrackBits reachable = TrackdirBitsToTrackBits(GetTileTrackStatus(new_tile, TransportType::Rail, RoadTramType::Invalid).trackdirs & DiagdirReachesTrackdirs(exitdir));

	if (Rail90DegTurnDisallowed(GetTileRailType(origin.tile), GetTileRailType(new_tile))) reachable.Reset(TrackCrossesTracks(TrackdirToTrack(origin.trackdir)));

	bool res_made = false;
	ChooseTrainTrack(consist, new_tile, exitdir, reachable, true, &res_made, mark_as_stuck);

	if (!res_made) {
		/* Free the depot reservation as well. */
		if (moving_front->track == Track::Depot) SetDepotReservation(moving_front->tile, false);
		return false;
	}

	if (consist->flags.Test(VehicleRailFlag::Stuck)) {
		consist->wait_counter = 0;
		SetWindowWidgetDirty(WindowClass::VehicleView, consist->index, WID_VV_START_STOP);
	}
	consist->flags.Reset(VehicleRailFlag::Stuck);
	return true;
}

/**
 * Can the train reverse?
 * @param consist The train to check.
 * @return \c true iff the train can be reversed.
 */
static bool CheckReverseTrain(const Train *consist)
{
	const Train *moving_front = consist->GetMovingFront();
	if (_settings_game.difficulty.train_flip_reverse_allowed == TrainFlipReversingAllowed::EndOfLineOnly ||
			moving_front->track == Track::Depot || moving_front->track == Track::Wormhole ||
			!IsDiagonalDirection(moving_front->GetMovingDirection())) {
		return false;
	}

	/* Checking the leading end alone is not enough any more. A train used to
	 * be turned round on its way into a depot so that it always came out
	 * leading with its head, and the head is what that check looks at; now
	 * that a train keeps the way round it went in, the end coming out first
	 * may be the far end of the chain while the head is still inside. Turning
	 * a train round in that state tears the consist apart, so wait until all
	 * of it is out. Which way round a train leaves a depot is the player's
	 * choice here, made with the order flag or the reverse button, and not
	 * something to be second-guessed on the way out. */
	if (IsAnyPartInsideDepot(consist)) return false;

	assert(moving_front->track.Any());

	return YapfTrainCheckReverse(consist);
}

/**
 * Get the location of the next station to visit.
 * @param station Next station to visit.
 * @return Location of the new station.
 */
TileIndex Train::GetOrderStationLocation(StationID station)
{
	if (station == this->last_station_visited) this->last_station_visited = StationID::Invalid();

	const Station *st = Station::Get(station);
	if (!st->facilities.Test(StationFacility::Train)) {
		/* The destination station has no trainstation tiles. */
		this->IncrementRealOrderIndex();
		return TileIndex{};
	}

	return st->xy;
}

/** Goods at the consist have changed, update the graphics, cargo, and acceleration. */
void Train::MarkDirty()
{
	Train *v = this;
	do {
		v->colourmap = PAL_NONE;
		v->UpdateViewport(true, false);
	} while ((v = v->Next()) != nullptr);

	/* need to update acceleration and cached values since the goods on the train changed. */
	this->CargoChanged();
	this->UpdateAcceleration();
}

/**
 * This function looks at the vehicle and updates its speed (cur_speed
 * and subspeed) variables. Furthermore, it returns the distance that
 * the train can drive this tick. #Vehicle::GetAdvanceDistance() determines
 * the distance to drive before moving a step on the map.
 * @return distance to drive.
 */
int Train::UpdateSpeed()
{
	switch (_settings_game.vehicle.train_acceleration_model) {
		default: NOT_REACHED();
		case AccelerationModel::Original:
			return this->DoUpdateSpeed(this->acceleration * (this->GetAccelerationStatus() == AS_BRAKE ? -4 : 2), 0, this->GetCurrentMaxSpeed());

		case AccelerationModel::Realistic:
			return this->DoUpdateSpeed(this->GetAcceleration(), this->GetAccelerationStatus() == AS_BRAKE ? 0 : 2, this->GetCurrentMaxSpeed());
	}
}

/**
 * Trains enters a station, send out a news item if it is the first train, and start loading.
 * @param consist Train that entered the station.
 * @param station Station visited.
 */
static void TrainEnterStation(Train *consist, StationID station)
{
	consist->last_station_visited = station;

	/* check if a train ever visited this station before */
	Station *st = Station::Get(station);
	if (!st->had_vehicle_of_type.Test(StationVehicleType::Train)) {
		st->had_vehicle_of_type.Set(StationVehicleType::Train);
		AddVehicleNewsItem(
			GetEncodedString(STR_NEWS_FIRST_TRAIN_ARRIVAL, st->index),
			consist->owner == _local_company ? NewsType::ArrivalCompany : NewsType::ArrivalOther,
			consist->index,
			st->index
		);
		AI::NewEvent(consist->owner, new ScriptEventStationFirstVehicle(st->index, consist->index));
		Game::NewEvent(new ScriptEventStationFirstVehicle(st->index, consist->index));
	}

	consist->force_proceed = TFP_NONE;
	InvalidateWindowData(WindowClass::VehicleView, consist->index);

	consist->BeginLoading();

	TileIndex tile = consist->GetMovingFront()->tile;
	TriggerStationRandomisation(st, tile, StationRandomTrigger::VehicleArrives);
	TriggerStationAnimation(st, tile, StationAnimationTrigger::VehicleArrives);
}

/**
 * Check if the vehicle is compatible with the specified tile.
 * @param v The train to check.
 * @param tile The tile to check.
 * @param check_railtype Should we check the railtype for compatibility?
 * @return \c true iff the tile is compatible with the train.
 */
static inline bool CheckCompatibleRail(const Train *v, TileIndex tile, bool check_railtype)
{
	return IsTileOwner(tile, v->owner) &&
			(!check_railtype || !v->IsFrontEngine() || v->compatible_railtypes.Test(GetRailType(tile)));
}

/** Data structure for storing engine speed changes of an acceleration type. */
struct AccelerationSlowdownParams {
	uint8_t small_turn; ///< Speed change due to a small turn.
	uint8_t large_turn; ///< Speed change due to a large turn.
	uint8_t z_up;       ///< Fraction to remove when moving up.
	uint8_t z_down;     ///< Fraction to add when moving down.
};

/** Speed update fractions for each acceleration type. */
static const AccelerationSlowdownParams _accel_slowdown[] = {
	/* normal accel */
	{256 / 4, 256 / 2, 256 / 4, 2}, ///< normal
	{256 / 4, 256 / 2, 256 / 4, 2}, ///< monorail
	{0,       256 / 2, 256 / 4, 2}, ///< maglev
};

/**
 * Modify the speed of the vehicle due to a change in altitude.
 * @param consist %Train to update.
 * @param z_diff Z difference new - old.
 */
static inline void AffectSpeedByZChange(Train *consist, int z_diff)
{
	if (z_diff == 0 || _settings_game.vehicle.train_acceleration_model != AccelerationModel::Original) return;

	const AccelerationSlowdownParams *asp = &_accel_slowdown[static_cast<int>(consist->GetAccelerationType())];

	if (z_diff > 0) {
		consist->cur_speed -= (consist->cur_speed * asp->z_up >> 8);
	} else {
		uint16_t spd = consist->cur_speed + asp->z_down;
		if (spd <= consist->gcache.cached_max_track_speed) consist->cur_speed = spd;
	}
}

static bool TrainMovedChangeSignals(TileIndex tile, DiagDirection dir)
{
	if (IsTileType(tile, TileType::Railway) &&
			GetRailTileType(tile) == RailTileType::Signals) {
		TrackdirBits tracks = TrackBitsToTrackdirBits(GetTrackBits(tile)) & DiagdirReachesTrackdirs(dir);
		Trackdir trackdir = FindFirstTrackdir(tracks);
		if (UpdateSignalsOnSegment(tile,  TrackdirToExitdir(trackdir), GetTileOwner(tile)) == SigSegState::Path && HasSignalOnTrackdir(tile, trackdir)) {
			/* A PBS block with a non-PBS signal facing us? */
			if (!IsPbsSignal(GetSignalType(tile, TrackdirToTrack(trackdir)))) return true;
		}
	}
	return false;
}

/** Tries to reserve track under whole train consist. */
void Train::ReserveTrackUnderConsist() const
{
	for (const Train *u = this; u != nullptr; u = u->Next()) {
		switch (u->track.base()) {
			case TrackBits{Track::Wormhole}.base():
				TryReserveRailTrack(u->tile, DiagDirToDiagTrack(GetTunnelBridgeDirection(u->tile)));
				break;
			case TrackBits{Track::Depot}.base():
				break;
			default:
				/* Diagnostic: TrackBitsToTrack() is shared by several call
				 * sites, so its own assert cannot say which one tripped.
				 * Assert here too, to name this one. */
				assert(u->track.Count() == 1 && !u->track.Any({Track::Wormhole, Track::Depot}));
				TryReserveRailTrack(u->tile, TrackBitsToTrack(u->track));
				break;
		}
	}
}

/**
 * The train vehicle crashed!
 * Update its status and other parts around it.
 * @param flooded Crash was caused by flooding.
 * @return Number of people killed.
 */
uint Train::Crash(bool flooded)
{
	uint victims = 0;
	if (this->IsFrontEngine()) {
		victims += 2; // driver

		/* Remove the reserved path in front of the train, and all the tracks it
		 * is standing on.
		 *
		 * Vanilla skips the path for a stuck train, on the grounds that a stuck
		 * train has no path to give back. That holds while being stuck is a
		 * moment's hesitation at a signal. It stops holding here, where a train
		 * that cannot find a route stays stuck for good and is left standing
		 * with whatever it had reserved -- and if it then crashes, that
		 * reservation outlives it. What is left is a stretch of track marked
		 * taken with nothing on it and nothing coming to release it, which is
		 * what a player sees as trains queueing between signals for no reason
		 * at all. Give it back whatever the train's state was. */
		FreeTrainTrackReservation(this);
		for (const Train *v = this; v != nullptr; v = v->Next()) {
			ClearPathReservation(v, v->tile, v->GetVehicleTrackdir());
			if (IsTileType(v->tile, TileType::TunnelBridge)) {
				/* ClearPathReservation will not free the wormhole exit
				 * if the train has just entered the wormhole. */
				SetTunnelBridgeReservation(GetOtherTunnelBridgeEnd(v->tile), false);
			}
		}

		/* we may need to update crossing we were approaching,
		 * but must be updated after the train has been marked crashed */
		TileIndex crossing = TrainApproachingCrossingTile(this->GetMovingFront());
		if (crossing != INVALID_TILE) UpdateLevelCrossing(crossing);

		/* Remove the loading indicators (if any) */
		HideFillingPercent(&this->fill_percent_te_id);
	}

	victims += this->GroundVehicleBase::Crash(flooded);

	this->crash_anim_pos = flooded ? 4000 : 1; // max 4440, disappear pretty fast when flooded
	return victims;
}

/**
 * Marks train as crashed and creates an AI event.
 * Doesn't do anything if the train is crashed already.
 * @param v first vehicle of chain
 * @return number of victims (including 2 drivers; zero if train was already crashed)
 */
static uint TrainCrashed(Train *v)
{
	uint victims = 0;

	/* do not crash train twice */
	if (!v->vehstatus.Test(VehState::Crashed)) {
		victims = v->Crash();
		TileIndex tile = v->GetMovingFront()->tile;
		AI::NewEvent(v->owner, new ScriptEventVehicleCrashed(v->index, tile, ScriptEventVehicleCrashed::CRASH_TRAIN, victims, v->owner));
		Game::NewEvent(new ScriptEventVehicleCrashed(v->index, tile, ScriptEventVehicleCrashed::CRASH_TRAIN, victims, v->owner));
	}

	/* Try to re-reserve track under already crashed train too.
	 * Crash() clears the reservation! */
	v->ReserveTrackUnderConsist();

	return victims;
}

/**
 * Collision test function.
 * @param v The %Train vehicle we may have collided with.
 * @param moving_front The %Train vehicle being examined.
 * @return Number of victims.
 */
static uint CheckTrainCollision(Vehicle *v, Train *moving_front)
{
	/* Make sure we are a train, and are not in a depot. */
	if (v->type != VehicleType::Train) return 0;

	/* We can't crash into trains in a depot. */
	if (Train::From(v)->track == Track::Depot) return 0;

	/* Do not crash into trains of another company. */
	if (v->owner != moving_front->First()->owner) return 0;

	/* Do not collide with our own wagons */
	if (v->First() == moving_front->First()) return 0;

	int x_diff = v->x_pos - moving_front->x_pos;
	int y_diff = v->y_pos - moving_front->y_pos;

	/* Do fast calculation to check whether trains are not in close vicinity
	 * and quickly reject trains distant enough for any collision.
	 * Differences are shifted by 7, mapping range [-7 .. 8] into [0 .. 15]
	 * Differences are then ORed and then we check for any higher bits */
	uint hash = (y_diff + 7) | (x_diff + 7);
	if (hash & ~15) return 0;

	/* Slower check using multiplication */
	int min_diff = (Train::From(v)->gcache.cached_veh_length + 1) / 2 + (moving_front->gcache.cached_veh_length + 1) / 2 - 1;
	if (x_diff * x_diff + y_diff * y_diff > min_diff * min_diff) return 0;

	/* Happens when there is a train under bridge next to bridge head */
	if (abs(v->z_pos - moving_front->z_pos) > 5) return 0;

	/* Reaching the consist we were deliberately sent to pick up is not a
	 * collision -- it is the whole point of the order. Coupling happens by
	 * touching, so the two necessarily end up within the proximity this
	 * function otherwise treats as a crash. Stop dead instead; TrainLocoHandler()
	 * performs the actual coupling on the next tick, once nothing is iterating
	 * over these vehicles any more (splicing two consists here, mid-collision-scan,
	 * would pull the list apart under the caller's feet).
	 *
	 * The question asked is the same one the coupling itself asks: are these two
	 * a pair that may couple, right now. Anything narrower and the two can
	 * disagree -- the crash is called off and then nothing couples, so the train
	 * quietly drives on through what it came for. Anything wider and trains that
	 * have no business with each other stop being able to crash at all. See
	 * FEATURE_DESIGN_COUPLING_TOW.md. */
	Train *first = moving_front->First();
	Train *other = Train::From(v)->First();
	if (other->cur_speed == 0 && (IsValidCouplePartner(first, other) || IsValidCouplePartner(other, first))) {
		first->cur_speed = 0;
		first->subspeed = 0;
		return 0;
	}

	/* Crash both trains. Two statements required to guarantee execution
	 * order because RandomRange() is involved. */
	uint num_victims = TrainCrashed(moving_front->First());
	return num_victims + TrainCrashed(Train::From(v)->First());
}

/**
 * Checks whether the specified train has a collision with another vehicle. If
 * so, destroys this vehicle, and the other vehicle if its subtype has TS_Front.
 * Reports the incident in a flashy news item, modifies station ratings and
 * plays a sound.
 * @param moving_front %Train to test.
 * @return \c true iff there has been a collision.
 */
static bool CheckTrainCollision(Train *moving_front)
{
	/* can't collide in depot */
	if (moving_front->track == Track::Depot) return false;

	assert(moving_front->track == Track::Wormhole || TileVirtXY(moving_front->x_pos, moving_front->y_pos) == moving_front->tile);

	uint num_victims = 0;

	/* find colliding vehicles */
	if (moving_front->track == Track::Wormhole) {
		for (Vehicle *u : VehiclesOnTile(moving_front->tile)) {
			num_victims += CheckTrainCollision(u, moving_front);
		}
		for (Vehicle *u : VehiclesOnTile(GetOtherTunnelBridgeEnd(moving_front->tile))) {
			num_victims += CheckTrainCollision(u, moving_front);
		}
	} else {
		for (Vehicle *u : VehiclesNearTileXY(moving_front->x_pos, moving_front->y_pos, 7)) {
			num_victims += CheckTrainCollision(u, moving_front);
		}
	}

	/* any dead -> no crash */
	if (num_victims == 0) return false;

	AddTileNewsItem(GetEncodedString(STR_NEWS_TRAIN_CRASH, num_victims), NewsType::Accident, moving_front->tile);

	ModifyStationRatingAround(moving_front->tile, moving_front->First()->owner, -160, 30);
	if (_settings_client.sound.disaster) SndPlayVehicleFx(SND_13_TRAIN_COLLISION, moving_front);
	return true;
}

/**
 * Move a vehicle chain one movement stop forwards.
 * @param v First vehicle to move.
 * @param nomove Stop moving this and all following vehicles.
 * @param reverse Set to false to not execute the vehicle reversing. This does not change any other logic.
 * @return True if the vehicle could be moved forward, false otherwise.
 */
bool TrainController(Train *v, Vehicle *nomove, bool reverse)
{
	Train *first = v->First();
	Train *prev;
	bool direction_changed = false; // has direction of any part changed?

	/* For every vehicle after and including the given vehicle */
	for (prev = v->GetMovingPrev(); v != nomove; prev = v, v = v->GetMovingNext()) {
		DiagDirection enterdir = DiagDirection::Begin;
		bool update_signals_crossing = false; // will we update signals or crossing state?

		GetNewVehiclePosResult gp = GetNewVehiclePos(v);
		if (v->track != Track::Wormhole) {
			/* Not inside tunnel */
			if (gp.old_tile == gp.new_tile) {
				/* Staying in the old tile */
				if (v->track == Track::Depot) {
					/* Inside depot */
					gp.x = v->x_pos;
					gp.y = v->y_pos;
				} else {
					/* Not inside depot */

					/* Reverse when we are at the end of the track already, do not move to the new position */
					if (v->IsMovingFront() && !TrainCheckIfLineEnds(v, reverse)) return false;

					auto vets = VehicleEnterTile(v, gp.new_tile, gp.x, gp.y);
					if (vets.Test(VehicleEnterTileState::CannotEnter)) {
						goto invalid_rail;
					}
					if (vets.Test(VehicleEnterTileState::EnteredStation)) {
						/* The new position is the end of the platform */
						TrainEnterStation(first, GetStationIndex(gp.new_tile));
					}
				}
			} else {
				/* A new tile is about to be entered. */

				/* Determine what direction we're entering the new tile from */
				enterdir = DiagdirBetweenTiles(gp.old_tile, gp.new_tile);
				assert(IsValidDiagDirection(enterdir));

				/* Get the status of the tracks in the new tile and mask
				 * away the bits that aren't reachable. */
				TrackStatus ts = GetTileTrackStatus(gp.new_tile, TransportType::Rail, RoadTramType::Invalid, ReverseDiagDir(enterdir));
				TrackdirBits reachable_trackdirs = DiagdirReachesTrackdirs(enterdir);

				TrackdirBits trackdirbits = ts.trackdirs & reachable_trackdirs;
				TrackBits red_signals = TrackdirBitsToTrackBits(ts.signals & reachable_trackdirs);

				TrackBits bits = TrackdirBitsToTrackBits(trackdirbits);
				if (Rail90DegTurnDisallowed(GetTileRailType(gp.old_tile), GetTileRailType(gp.new_tile)) && prev == nullptr) {
					/* We allow wagons to make 90 deg turns, because forbid_90_deg
					 * can be switched on halfway a turn */
					bits.Reset(TrackCrossesTracks(FindFirstTrack(v->track)));
				}

				if (bits.None()) goto invalid_rail;

				/* Check if the new tile constrains tracks that are compatible
				 * with the current train, if not, bail out. */
				if (!CheckCompatibleRail(v->First(), gp.new_tile, v->IsMovingFront())) goto invalid_rail;

				TrackBits chosen_track;
				if (v->IsMovingFront()) {
					/* Currently the locomotive is active. Determine which one of the
					 * available tracks to choose */
					bool was_stuck = first->flags.Test(VehicleRailFlag::Stuck);
					chosen_track = ChooseTrainTrack(first, gp.new_tile, enterdir, bits, false, nullptr, true);
					assert(chosen_track.Any(bits | GetReservedTrackbits(gp.new_tile)));

					/* When ChooseTrainTrack() cannot find or reserve a path it
					 * marks the train stuck and hands back a track only so that
					 * something valid comes out of it -- a placeholder, not a
					 * decision. Vanilla gets away with driving on regardless
					 * because the track it hands back is the one guarded by the
					 * signal that stopped it, so the red-signal check just below
					 * halts the train anyway. That does not hold once the
					 * placeholder is picked from the tile rather than from a
					 * failed path: it can be any track on the tile, including
					 * one that is clear and green, or one belonging to a
					 * one-way line pointing the other way. The train then
					 * drives somewhere it never chose and nothing has reserved
					 * -- straight into an occupied platform, in the case that
					 * showed this up. Being stuck means staying put, so stay
					 * put and let the retry in TrainLocoHandler sort it out. */
					if (!was_stuck && first->flags.Test(VehicleRailFlag::Stuck)) {
						first->cur_speed = 0;
						first->subspeed = 0;
						first->wait_counter = 0;
						return false;
					}

					/* Never physically enter a tile held by a headless "free
					 * wagon" chain left behind by a decouple order, even
					 * under Force Proceed. Force Proceed is meant to bypass
					 * being stuck on an overly-cautious PBS signal, at the
					 * well-understood vanilla risk of running into another
					 * train that happens to be genuinely in the way -- but a
					 * player has no way to anticipate which direction a
					 * train will approach a station from (it may route via
					 * a depot, or a less congested line, and arrive from an
					 * unexpected side), so a collision with wagons left
					 * behind by an earlier, unrelated decouple order would
					 * be baffling rather than an accepted risk. See
					 * FEATURE_DESIGN_COUPLING_TOW.md. */
					for (const Vehicle *u : VehiclesOnTile(gp.new_tile)) {
						if (u->type != VehicleType::Train) continue;
						const Train *t = Train::From(u)->First();
						if (t == first || t->IsFrontEngine()) continue;
						MarkTrainAsStuck(first);
						/* Reuse the same halt mechanism as every other
						 * "can't enter this tile" case in this function
						 * (goto invalid_rail below), instead of a bare
						 * `return false`: that path also zeroes cur_speed/
						 * subspeed, which matters here because Force
						 * Proceed keeps resetting VehicleRailFlag::Stuck
						 * every tick (see TrainLocoHandler) and the outer
						 * per-tick movement loop in TrainLocoHandler keeps
						 * calling TrainController based on cur_speed alone,
						 * ignoring TrainController's return value -- a bare
						 * `return false` that left speed untouched let the
						 * train's existing momentum carry it into the
						 * wagons on a later sub-step within the very same
						 * tick. */
						goto invalid_rail;
					}

					if (first->force_proceed != TFP_NONE && IsPlainRailTile(gp.new_tile) && HasSignals(gp.new_tile)) {
						/* For each signal we find decrease the counter by one.
						 * We start at two, so the first signal we pass decreases
						 * this to one, then if we reach the next signal it is
						 * decreased to zero and we won't pass that new signal. */
						Trackdir dir = FindFirstTrackdir(trackdirbits);
						if (HasSignalOnTrackdir(gp.new_tile, dir) ||
								(HasSignalOnTrackdir(gp.new_tile, ReverseTrackdir(dir)) &&
								GetSignalType(gp.new_tile, TrackdirToTrack(dir)) != SignalType::Path)) {
							/* However, we do not want to be stopped by PBS signals
							 * entered via the back. */
							first->force_proceed = (first->force_proceed == TFP_SIGNAL) ? TFP_STUCK : TFP_NONE;
							InvalidateWindowData(WindowClass::VehicleView, first->index);
						}
					}

					/* Check if it's a red signal and that force proceed is not clicked. */
					if (red_signals.Any(chosen_track) && first->force_proceed == TFP_NONE) {
						/* In front of a red signal */
						Trackdir i = FindFirstTrackdir(trackdirbits);

						/* Don't handle stuck trains here. */
						if (first->flags.Test(VehicleRailFlag::Stuck)) return false;

						if (!HasSignalOnTrackdir(gp.new_tile, ReverseTrackdir(i))) {
							first->cur_speed = 0;
							first->subspeed = 0;
							first->progress = 255; // make sure that every bit of acceleration will hit the signal again, so speed stays 0.
							if (!_settings_game.pf.reverse_at_signals || ++first->wait_counter < _settings_game.pf.wait_oneway_signal * Ticks::DAY_TICKS * 2) return false;
						} else if (HasSignalOnTrackdir(gp.new_tile, i)) {
							first->cur_speed = 0;
							first->subspeed = 0;
							first->progress = 255; // make sure that every bit of acceleration will hit the signal again, so speed stays 0.
							if (!_settings_game.pf.reverse_at_signals || ++first->wait_counter < _settings_game.pf.wait_twoway_signal * Ticks::DAY_TICKS * 2) {
								DiagDirection exitdir = TrackdirToExitdir(i);
								TileIndex o_tile = TileAddByDiagDir(gp.new_tile, exitdir);

								exitdir = ReverseDiagDir(exitdir);

								/* check if a train is waiting on the other side */
								if (!HasVehicleOnTile(o_tile, [&exitdir](const Vehicle *u) {
										if (u->type != VehicleType::Train || u->vehstatus.Test(VehState::Crashed)) return false;
										const Train *t = Train::From(u);

										/* not front engine of a train, inside wormhole or depot, crashed */
										if (!t->IsFrontEngine() || t->track.Any({Track::Wormhole, Track::Depot})) return false;

										if (t->cur_speed > 5 || VehicleExitDir(t->direction, t->track) != exitdir) return false;

										return true;
									})) return false;
							}
						}

						/* If we would reverse but are currently in a PBS block and
						 * reversing of stuck trains is disabled, don't reverse.
						 * This does not apply if the reason for reversing is a one-way
						 * signal blocking us, because a train would then be stuck forever. */
						if (!_settings_game.pf.reverse_at_signals && !HasOnewaySignalBlockingTrackdir(gp.new_tile, i) &&
								UpdateSignalsOnSegment(v->tile, enterdir, v->owner) == SigSegState::Path) {
							first->wait_counter = 0;
							return false;
						}
						goto reverse_train_direction;
					} else {
						/* Diagnostic: see the matching note in ReserveTrackUnderConsist(). */
						assert(chosen_track.Count() == 1 && !chosen_track.Any({Track::Wormhole, Track::Depot}));
						TryReserveRailTrack(gp.new_tile, TrackBitsToTrack(chosen_track), false);
					}
				} else {
					/* The wagon is active, simply follow the prev vehicle. */
					if (prev->tile == gp.new_tile) {
						/* Choose the same track as prev */
						if (prev->track == Track::Wormhole) {
							/* Vehicles entering tunnels enter the wormhole earlier than for bridges.
							 * However, just choose the track into the wormhole. */
							assert(IsTunnel(prev->tile));
							chosen_track = bits;
						} else {
							chosen_track = prev->track;
						}
					} else {
						/* Choose the track that leads to the tile where prev is.
						 * This case is active if 'prev' is already on the second next tile, when 'v' just enters the next tile.
						 * I.e. when the tile between them has only space for a single vehicle like
						 *  1) horizontal/vertical track tiles and
						 *  2) some orientations of tunnel entries, where the vehicle is already inside the wormhole at 8/16 from the tile edge.
						 *     Is also the train just reversing, the wagon inside the tunnel is 'on' the tile of the opposite tunnel entry.
						 */
						static const DiagDirectionIndexArray<DiagDirectionIndexArray<TrackBits>> _connecting_track{{{
							{{{Track::X,     Track::Lower, {},           Track::Left }}},
							{{{Track::Upper, Track::Y,     Track::Left,  {}          }}},
							{{{{},           Track::Right, Track::X,     Track::Upper}}},
							{{{Track::Right, {},           Track::Lower, Track::Y    }}}
						}}};
						DiagDirection exitdir = DiagdirBetweenTiles(gp.new_tile, prev->tile);
						assert(IsValidDiagDirection(exitdir));
						chosen_track = _connecting_track[enterdir][exitdir];
					}
					chosen_track &= bits;
				}

				/* Update XY to reflect the entrance to the new tile, and select the direction to use */
				/* Diagnostic: see the matching note in ReserveTrackUnderConsist(). */
				assert(chosen_track.Count() == 1 && !chosen_track.Any({Track::Wormhole, Track::Depot}));
				Direction chosen_dir = VehicleEnterTileCoordinates(gp, enterdir, TrackBitsToTrack(chosen_track));

				/* Call the landscape function and tell it that the vehicle entered the tile */
				auto vets = VehicleEnterTile(v, gp.new_tile, gp.x, gp.y);
				if (vets.Test(VehicleEnterTileState::CannotEnter)) {
					goto invalid_rail;
				}

				if (!vets.Test(VehicleEnterTileState::EnteredWormhole)) {
					Track track = FindFirstTrack(chosen_track);
					Trackdir tdir = TrackDirectionToTrackdir(track, chosen_dir);
					if (v->IsMovingFront() && HasPbsSignalOnTrackdir(gp.new_tile, tdir)) {
						SetSignalStateByTrackdir(gp.new_tile, tdir, SignalState::Red);
						MarkTileDirtyByTile(gp.new_tile);
					}

					/* Clear any track reservation when the last vehicle leaves the tile */
					if (v->GetMovingNext() == nullptr) ClearPathReservation(v, v->tile, v->GetVehicleTrackdir());

					v->tile = gp.new_tile;

					if (GetTileRailType(gp.new_tile) != GetTileRailType(gp.old_tile)) {
						first->ConsistChanged(CCF_TRACK);
					}

					v->track = chosen_track;
					assert(v->track.Any());
				}

				/* We need to update signal status, but after the vehicle position hash
				 * has been updated by UpdateInclination() */
				update_signals_crossing = true;

				if (chosen_dir != v->GetMovingDirection()) {
					if (prev == nullptr && _settings_game.vehicle.train_acceleration_model == AccelerationModel::Original) {
						const AccelerationSlowdownParams *asp = &_accel_slowdown[static_cast<int>(v->GetAccelerationType())];
						DirDiff diff = DirDifference(v->direction, chosen_dir);
						v->cur_speed -= (diff == DirDiff::Right45 || diff == DirDiff::Left45 ? asp->small_turn : asp->large_turn) * v->cur_speed >> 8;
					}
					direction_changed = true;
					v->SetMovingDirection(chosen_dir);
				}

				if (v->IsMovingFront()) {
					first->wait_counter = 0;

					/* If we are approaching a crossing that is reserved, play the sound now. */
					TileIndex crossing = TrainApproachingCrossingTile(v); // We know we are the moving front, so we can check v.
					if (crossing != INVALID_TILE && HasCrossingReservation(crossing) && _settings_client.sound.ambient) SndPlayTileFx(SND_0E_LEVEL_CROSSING, crossing);

					/* Always try to extend the reservation when entering a tile. */
					CheckNextTrainTile(first);
				}

				if (vets.Test(VehicleEnterTileState::EnteredStation)) {
					/* The new position is the location where we want to stop */
					TrainEnterStation(first, GetStationIndex(gp.new_tile));
				}
			}
		} else {
			if (IsTileType(gp.new_tile, TileType::TunnelBridge) && VehicleEnterTile(v, gp.new_tile, gp.x, gp.y).Test(VehicleEnterTileState::EnteredWormhole)) {
				/* Perform look-ahead on tunnel exit. */
				if (v->IsMovingFront()) {
					TryReserveRailTrack(gp.new_tile, DiagDirToDiagTrack(GetTunnelBridgeDirection(gp.new_tile)));
					CheckNextTrainTile(first);
				}
				/* Prevent v->UpdateInclination() being called with wrong parameters.
				 * This could happen if the train was reversed inside the tunnel/bridge. */
				if (gp.old_tile == gp.new_tile) {
					gp.old_tile = GetOtherTunnelBridgeEnd(gp.old_tile);
				}
			} else {
				v->x_pos = gp.x;
				v->y_pos = gp.y;
				v->UpdatePosition();
				if (!v->vehstatus.Test(VehState::Hidden)) v->Vehicle::UpdateViewport(true);
				continue;
			}
		}

		/* update image of train, as well as delta XY */
		v->UpdateDeltaXY();

		v->x_pos = gp.x;
		v->y_pos = gp.y;
		v->UpdatePosition();

		/* update the Z position of the vehicle */
		int old_z = v->UpdateInclination(gp.new_tile != gp.old_tile, false);

		if (prev == nullptr) {
			/* This is the first vehicle in the train */
			AffectSpeedByZChange(first, v->z_pos - old_z);
		}

		if (update_signals_crossing) {
			if (v->IsMovingFront()) {
				if (TrainMovedChangeSignals(gp.new_tile, enterdir)) {
					/* We are entering a block with PBS signals right now, but
					 * not through a PBS signal. This means we don't have a
					 * reservation right now. As a conventional signal will only
					 * ever be green if no other train is in the block, getting
					 * a path should always be possible. If the player built
					 * such a strange network that it is not possible, the train
					 * will be marked as stuck and the player has to deal with
					 * the problem. */
					if ((!HasReservedTracks(gp.new_tile, v->track) &&
							!TryReserveRailTrack(gp.new_tile, FindFirstTrack(v->track))) ||
							!TryPathReserve(first)) {
						MarkTrainAsStuck(first);
					}
				}
			}

			/* Signals can only change when the first
			 * (above) or the last vehicle moves. */
			if (v->GetMovingNext() == nullptr) {
				TrainMovedChangeSignals(gp.old_tile, ReverseDiagDir(enterdir));
				if (IsLevelCrossingTile(gp.old_tile)) UpdateLevelCrossing(gp.old_tile);
			}
		}

		/* Do not check on every tick to save some computing time. */
		if (v->IsMovingFront() && first->tick_counter % _settings_game.pf.path_backoff_interval == 0) CheckNextTrainTile(first);
	}

	if (direction_changed) first->tcache.cached_max_curve_speed = first->GetCurveSpeedLimit();

	return true;

invalid_rail:
	/* We've reached end of line?? */
	if (prev != nullptr) FatalError("Disconnecting train");

reverse_train_direction:
	if (reverse) {
		first->wait_counter = 0;
		first->cur_speed = 0;
		first->subspeed = 0;
		ReverseTrainDirection(first);
	}

	return false;
}

static bool IsRailStationPlatformOccupied(TileIndex tile, const Train *ignore)
{
	TileIndexDiff delta = TileOffsByAxis(GetRailStationAxis(tile));
	const Train *ignore_first = ignore != nullptr ? ignore->First() : nullptr;

	auto is_other_train = [ignore_first](const Vehicle *u) {
		return u->type == VehicleType::Train && Train::From(u)->First() != ignore_first;
	};

	for (TileIndex t = tile; IsCompatibleTrainStationTile(t, tile); t -= delta) {
		if (HasVehicleOnTile(t, is_other_train)) return true;
	}
	for (TileIndex t = tile + delta; IsCompatibleTrainStationTile(t, tile); t += delta) {
		if (HasVehicleOnTile(t, is_other_train)) return true;
	}

	return false;
}

/**
 * Deletes/Clears the last wagon of a crashed train. It takes the engine of the
 * train, then goes to the last wagon and deletes that. Each call to this function
 * will remove the last wagon of a crashed train. If this wagon was on a crossing,
 * or inside a tunnel/bridge, recalculate the signals as they might need updating
 * @param v the Vehicle of which last wagon is to be removed
 */
static void DeleteLastWagon(Train *v)
{
	Train *first = v->First();

	/* Go to the last wagon and delete the link pointing there
	 * new_last is then the one-before-last wagon, and v the last
	 * one which will physically be removed */
	Train *new_last = v;
	for (; v->Next() != nullptr; v = v->Next()) new_last = v;
	new_last->SetNext(nullptr);

	if (first != v) {
		/* Recalculate cached train properties */
		first->ConsistChanged(CCF_ARRANGE);
		/* Update the depot window in case a part of the consist is in a depot. */
		SetWindowDirty(WindowClass::VehicleDepot, first->tile);
		SetWindowDirty(WindowClass::VehicleDepot, v->tile);
	}

	/* 'v' shouldn't be accessed after it has been deleted */
	TrackBits trackbits = v->track;
	TileIndex tile = v->tile;
	Owner owner = v->owner;

	delete v;
	v = nullptr; // make sure nobody will try to read 'v' anymore

	if (trackbits == Track::Wormhole) {
		/* Vehicle is inside a wormhole, v->track contains no useful value then. */
		trackbits = DiagDirToDiagTrack(GetTunnelBridgeDirection(tile));
	} else if (trackbits == Track::Depot) {
		/* Nor inside a depot: "in a depot" is all that field says there, and
		 * asking it which single track that is brings the game down. A depot
		 * tile has exactly one, the one leading out of the door, so say so --
		 * the same answer the tunnel case above gives, for the same reason.
		 *
		 * Vanilla never had to: a wreck could not get into a depot, and the
		 * only way one can now is that something went and fetched it. The loop
		 * below already knew depots were possible here and stepped around them;
		 * this call is above it and did not. */
		trackbits = DiagDirToDiagTrack(GetRailDepotDirection(tile));
	}

	Track track = TrackBitsToTrack(trackbits);
	if (HasReservedTracks(tile, trackbits)) {
		UnreserveRailTrack(tile, track);

		/* If there are still crashed vehicles on the tile, give the track reservation to them */
		TrackBits remaining_trackbits{};
		for (const Vehicle *u : VehiclesOnTile(tile)) {
			if (u->type != VehicleType::Train || !u->vehstatus.Test(VehState::Crashed)) continue;
			TrackBits train_tbits = Train::From(u)->track;
			if (train_tbits == Track::Wormhole) {
				/* Vehicle is inside a wormhole, u->track contains no useful value then. */
				remaining_trackbits.Set(DiagDirToDiagTrack(GetTunnelBridgeDirection(u->tile)));
			} else if (train_tbits != Track::Depot) {
				remaining_trackbits.Set(train_tbits);
			}
		}

		/* It is important that these two are the first in the loop, as reservation cannot deal with every trackbit combination */
		assert(Track::Begin == Track::X && to_underlying(Track::Y) == to_underlying(Track::Begin) + 1);
		for (Track t : remaining_trackbits) TryReserveRailTrack(tile, t);
	}

	/* check if the wagon was on a road/rail-crossing */
	if (IsLevelCrossingTile(tile)) UpdateLevelCrossing(tile);

	if (IsRailStationTile(tile)) {
		bool occupied = IsRailStationPlatformOccupied(tile);
		DiagDirection dir = AxisToDiagDir(GetRailStationAxis(tile));
		SetRailStationPlatformReservation(tile, dir, occupied);
		SetRailStationPlatformReservation(tile, ReverseDiagDir(dir), occupied);
	}

	/* Update signals */
	if (IsTileType(tile, TileType::TunnelBridge) || IsRailDepotTile(tile)) {
		UpdateSignalsOnSegment(tile, DiagDirection::Invalid, owner);
	} else {
		SetSignalsOnBothDir(tile, track, owner);
	}
}

/**
 * Rotate all vehicles of a (crashed) train chain randomly to animate the crash.
 * @param v First crashed vehicle.
 */
static void ChangeTrainDirRandomly(Train *v)
{
	static const DirDiff delta[] = {
		DirDiff::Left45, DirDiff::Same, DirDiff::Same, DirDiff::Right45
	};

	do {
		/* We don't need to twist around vehicles if they're not visible */
		if (!v->vehstatus.Test(VehState::Hidden)) {
			v->direction = ChangeDir(v->direction, delta[GB(Random(), 0, 2)]);
			/* Refrain from updating the z position of the vehicle when on
			 * a bridge, because UpdateInclination() will put the vehicle under
			 * the bridge in that case */
			if (v->track != Track::Wormhole) {
				v->UpdatePosition();
				v->UpdateInclination(false, true);
			} else {
				v->UpdateViewport(false, true);
			}
		}
	} while ((v = v->Next()) != nullptr);
}

/**
 * Handle a crashed train.
 * @param v First train vehicle.
 * @return %Vehicle chain still exists.
 */
static bool HandleCrashedTrain(Train *v)
{
	int state = ++v->crash_anim_pos;

	if (state == 4 && !v->vehstatus.Test(VehState::Hidden)) {
		CreateEffectVehicleRel(v, 4, 4, 8, EV_EXPLOSION_LARGE);
	}

	uint32_t r;
	if (state <= 200 && Chance16R(1, 7, r)) {
		int index = (r * 10 >> 16);

		Vehicle *u = v;
		do {
			if (--index < 0) {
				r = Random();

				CreateEffectVehicleRel(u,
					GB(r,  8, 3) + 2,
					GB(r, 16, 3) + 2,
					GB(r,  0, 3) + 5,
					EV_EXPLOSION_SMALL);
				break;
			}
		} while ((u = u->Next()) != nullptr);
	}

	if (state <= 240 && !(v->tick_counter & 3)) ChangeTrainDirRandomly(v);

	if (state >= 4440 && !(v->tick_counter & 0x1F)) {
		/* Wagons vanishing one by one is what clears a wreck off the line when
		 * nothing else will. A wreck that is going to be fetched should arrive
		 * at the depot whole, though -- a player who set up a rescue engine
		 * should get the whole train towed away, not whatever is left of it by
		 * the time help arrives. So hold the counter here while the wait is
		 * still on, and let it run once the wait is given up. Same deadline as
		 * a breakdown, and given up in the same way. See
		 * FEATURE_DESIGN_COUPLING_TOW.md. */
		if (TrainAwaitsRescue(v)) {
			v->crash_anim_pos = state - 1;
			return true;
		}

		bool ret = v->Next() != nullptr;
		DeleteLastWagon(v);
		return ret;
	}

	return true;
}

/** Maximum speeds for train that is broken down or approaching line end */
static const uint16_t _breakdown_speeds[16] = {
	225, 210, 195, 180, 165, 150, 135, 120, 105, 90, 75, 60, 45, 30, 15, 15
};


/**
 * Train is approaching line end, slow down and possibly reverse
 *
 * @param moving_front moving front vehicle
 * @param signal not line end, just a red signal
 * @param reverse Set to false to not execute the vehicle reversing. This does not change any other logic.
 * @return true iff we did NOT have to reverse
 */
static bool TrainApproachingLineEnd(Train *moving_front, bool signal, bool reverse)
{
	/* Calc position within the current tile */
	uint x = moving_front->x_pos & 0xF;
	uint y = moving_front->y_pos & 0xF;

	Direction vdir = moving_front->GetMovingDirection();

	/* for diagonal directions, 'x' will be 0..15 -
	 * for other directions, it will be 1, 3, 5, ..., 15 */
	switch (vdir) {
		case Direction::N : x = ~x + ~y + 25; break;
		case Direction::NW: x = y;            [[fallthrough]];
		case Direction::NE: x = ~x + 16;      break;
		case Direction::E : x = ~x + y + 9;   break;
		case Direction::SE: x = y;            break;
		case Direction::S : x = x + y - 7;    break;
		case Direction::W : x = ~y + x + 9;   break;
		default: break;
	}

	Train *consist = moving_front->First();

	/* Do not reverse when approaching red signal. Make sure the vehicle's front
	 * does not cross the tile boundary when we do reverse, but as the vehicle's
	 * location is based on their center, use half a vehicle's length as offset.
	 * Multiply the half-length by two for straight directions to compensate that
	 * we only get odd x offsets there. */
	uint8_t rounding = moving_front->IsDrivingBackwards() ? 0 : 1;
	if (!signal && x + (moving_front->gcache.cached_veh_length + rounding) / 2 * (IsDiagonalDirection(vdir) ? 1 : 2) >= TILE_SIZE) {
		/* we are too near the tile end, reverse now */
		consist->cur_speed = 0;
		if (reverse) ReverseTrainDirection(consist);
		return false;
	}

	/* slow down */
	consist->vehstatus.Set(VehState::TrainSlowing);
	uint16_t break_speed = _breakdown_speeds[x & 0xF];
	if (break_speed < consist->cur_speed) consist->cur_speed = break_speed;

	return true;
}


/**
 * Determines whether train would like to leave the tile.
 * @param moving_front The moving front vehicle of the train.
 * @return true iff vehicle is NOT entering or inside a depot or tunnel/bridge.
 */
static bool TrainCanLeaveTile(const Train *moving_front)
{
	/* Exit if inside a tunnel/bridge or a depot */
	if (moving_front->track == Track::Wormhole || moving_front->track == Track::Depot) return false;

	TileIndex tile = moving_front->tile;

	/* entering a tunnel/bridge? */
	if (IsTileType(tile, TileType::TunnelBridge)) {
		DiagDirection dir = GetTunnelBridgeDirection(tile);
		if (DiagDirToDir(dir) == moving_front->GetMovingDirection()) return false;
	}

	/* entering a depot? */
	if (IsRailDepotTile(tile)) {
		DiagDirection dir = ReverseDiagDir(GetRailDepotDirection(tile));
		if (DiagDirToDir(dir) == moving_front->GetMovingDirection()) return false;
	}

	return true;
}


/**
 * Determines whether train is approaching a rail-road crossing
 *   (thus making it barred)
 * @param moving_front moving front of train
 * @return TileIndex of crossing the train is approaching, else INVALID_TILE
 * @pre v in non-crashed front engine
 */
static TileIndex TrainApproachingCrossingTile(const Train *moving_front)
{
	assert(moving_front->IsMovingFront());
	assert(!moving_front->First()->vehstatus.Test(VehState::Crashed));

	if (!TrainCanLeaveTile(moving_front)) return INVALID_TILE;

	DiagDirection dir = VehicleExitDir(moving_front->GetMovingDirection(), moving_front->track);
	TileIndex tile = moving_front->tile + TileOffsByDiagDir(dir);

	/* not a crossing || wrong axis || unusable rail (wrong type or owner) */
	if (!IsLevelCrossingTile(tile) || DiagDirToAxis(dir) == GetCrossingRoadAxis(tile) ||
			!CheckCompatibleRail(moving_front->First(), tile, true)) {
		return INVALID_TILE;
	}

	return tile;
}


/**
 * Checks for line end. Also, bars crossing at next tile if needed
 *
 * @param moving_front moving vehicle front we are checking
 * @param reverse Set to false to not execute the vehicle reversing. This does not change any other logic.
 * @return true iff we did NOT have to reverse
 */
static bool TrainCheckIfLineEnds(Train *moving_front, bool reverse)
{
	/* First, handle broken down train */

	Train *consist = moving_front->First();
	int t = consist->breakdown_ctr;
	if (t > 1) {
		consist->vehstatus.Set(VehState::TrainSlowing);

		uint16_t break_speed = _breakdown_speeds[GB(~t, 4, 4)];
		if (break_speed < consist->cur_speed) consist->cur_speed = break_speed;
	} else {
		consist->vehstatus.Reset(VehState::TrainSlowing);
	}

	if (!TrainCanLeaveTile(moving_front)) return true;

	/* Determine the non-diagonal direction in which we will exit this tile */
	DiagDirection dir = VehicleExitDir(moving_front->GetMovingDirection(), moving_front->track);
	/* Calculate next tile */
	TileIndex tile = moving_front->tile + TileOffsByDiagDir(dir);

	/* Determine the track status on the next tile */
	TrackStatus ts = GetTileTrackStatus(tile, TransportType::Rail, RoadTramType::Invalid, ReverseDiagDir(dir));
	TrackdirBits reachable_trackdirs = DiagdirReachesTrackdirs(dir);

	TrackdirBits trackdirbits = ts.trackdirs & reachable_trackdirs;
	TrackdirBits red_signals = ts.signals & reachable_trackdirs;

	/* We are sure the train is not entering a depot, it is detected above */

	/* mask unreachable track bits if we are forbidden to do 90deg turns */
	TrackBits bits = TrackdirBitsToTrackBits(trackdirbits);
	if (Rail90DegTurnDisallowed(GetTileRailType(moving_front->tile), GetTileRailType(tile))) {
		bits.Reset(TrackCrossesTracks(FindFirstTrack(moving_front->track)));
	}

	/* no suitable trackbits at all || unusable rail (wrong type or owner) */
	if (bits.None() || !CheckCompatibleRail(consist, tile, true)) {
		return TrainApproachingLineEnd(moving_front, false, reverse);
	}

	/* approaching red signal */
	if (trackdirbits.Any(red_signals)) return TrainApproachingLineEnd(moving_front, true, reverse);

	/* approaching a rail/road crossing? then make it red */
	if (IsLevelCrossingTile(tile)) MaybeBarCrossingWithSound(tile);

	return true;
}

/**
 * Per-tick handler of each front engine.
 * @param consist The front engine we are working with.
 * @param mode Set to \c True if we want the train to keep existing, \c False if we want to consider deleting it (it has been crashed).
 * @return \c true if we want the train to keep existing, \c False if we want to delete it (it has been crashed).
 */
static bool TrainLocoHandler(Train *consist, bool mode)
{
	/* train has crashed? */
	if (consist->vehstatus.Test(VehState::Crashed)) {
		return mode ? true : HandleCrashedTrain(consist); // 'this' can be deleted here
	}

	if (consist->force_proceed != TFP_NONE) {
		consist->flags.Reset(VehicleRailFlag::Stuck);
		SetWindowWidgetDirty(WindowClass::VehicleView, consist->index, WID_VV_START_STOP);
	}

	/* train is broken down? */
	if (consist->HandleBreakdown()) return true;

	/* A train on a couple order that has come to a stop against its
	 * partner couples with it now. This is the moment the two consists are
	 * physically touching but nothing is iterating over either of them, so
	 * it is safe to splice them (CheckTrainCollision deliberately only
	 * stops the train and defers to here -- see the matching comment
	 * there). Covers a train that drove here under a "go to couple" order
	 * as well as one already parked on "wait to couple" that a partner has
	 * since been pushed up against. See FEATURE_DESIGN_COUPLING_TOW.md. */
	if (consist->cur_speed == 0 && IsPartyToACoupling(consist) &&
			GetTrainCouplePartner(consist) != nullptr) {
		/* Commands check ownership against the company that is "current" right
		 * now, which during a vehicle tick is simply whatever ran last -- not
		 * necessarily the owner of the train being ticked. Without restoring
		 * it, CmdCoupleTrains() fails its very first check and does nothing,
		 * silently, on every single tick. Autoreplace and order refits back it
		 * up in exactly the same way for exactly this reason (see
		 * CallVehicleTicks and ProcessOrders in vehicle.cpp). */
		AutoRestoreBackup cur_company(_current_company, consist->owner);

		/* Only claim the tick if the coupling really happened. Returning
		 * unconditionally would leave a train that cannot couple for some
		 * other reason frozen here for good, doing nothing else ever again. */
		if (CmdCoupleTrains(DoCommandFlag::Execute, consist->index).Succeeded()) return true;
	}

	/* Everything a rescue engine does while standing in a depot happens here,
	 * and here on purpose: a moment in the tick when nothing is walking along
	 * the consist, so taking one apart is safe. Doing it as the train came
	 * through the depot door would be doing it from inside the very loop that
	 * is stepping over its vehicles. Putting a casualty down is exactly that
	 * kind of surgery, which is why coupling waits for this moment too. */
	if (consist->vehicle_flags.Test(VehicleFlag::RescueEngine) && consist->IsInDepot() && consist->cur_speed == 0) {
		if (consist->rescue_target != VehicleID::Invalid()) {
			HandleRescueEngineInDepot(consist);
			return true;
		}
		TryDispatchRescueEngine(consist);
	}

	if (consist->flags.Test(VehicleRailFlag::Reversing) && consist->cur_speed == 0) {
		ReverseTrainDirection(consist);
	}

	/* exit if train is stopped */
	if (consist->vehstatus.Test(VehState::Stopped) && consist->cur_speed == 0) return true;

	/* A train told to go and collect wagons does not set off until it has a
	 * rake to collect, and once it has one, that rake is nobody else's. Asked
	 * only of a train standing still, so an engine already on its way is never
	 * halted in the middle of the line by this; and the answer is remembered on
	 * the rake, so asking again on the next tick returns the same one. See
	 * FindOrClaimCoupleTarget(). */
	if (consist->current_order.ShouldGoToCouple() && consist->cur_speed == 0 &&
			FindOrClaimCoupleTarget(consist) == nullptr) {
		/* And it holds no track while it waits. Reserving first and choosing
		 * afterwards was the whole trouble: the path was held against every
		 * other train for as long as the wait lasted, and when the choice was
		 * finally made the train went to what it had reserved rather than to
		 * what it had chosen. Nothing is reserved until there is something to
		 * reserve it for. Not every tick -- there is nothing to release most of
		 * the time. */
		if ((consist->tick_counter & 0x1F) == 0) {
			FreeTrainTrackReservation(consist);
			consist->ReserveTrackUnderConsist();
		}
		return true;
	}

	bool valid_order = !consist->current_order.IsType(OT_NOTHING) && consist->current_order.GetType() != OT_CONDITIONAL;
	if (ProcessOrders(consist) && CheckReverseTrain(consist)) {
		consist->wait_counter = 0;
		consist->cur_speed = 0;
		consist->subspeed = 0;
		consist->flags.Reset(VehicleRailFlag::LeavingStation);
		ReverseTrainDirection(consist);
		return true;
	} else if (consist->flags.Test(VehicleRailFlag::LeavingStation)) {
		/* Try to reserve a path when leaving the station as we
		 * might not be marked as wanting a reservation, e.g.
		 * when an overlength train gets turned around in a station. */
		const Train *moving_front = consist->GetMovingFront();
		DiagDirection dir = VehicleExitDir(moving_front->GetMovingDirection(), moving_front->track);
		if (IsRailDepotTile(moving_front->tile) || IsTileType(moving_front->tile, TileType::TunnelBridge)) dir = DiagDirection::Invalid;

		if (UpdateSignalsOnSegment(moving_front->tile, dir, consist->owner) == SigSegState::Path || _settings_game.pf.reserve_paths) {
			TryPathReserve(consist, true, true);
		}
		consist->flags.Reset(VehicleRailFlag::LeavingStation);
	}

	consist->HandleLoading(mode);

	if (consist->current_order.IsType(OT_LOADING)) return true;

	if (CheckTrainStayInDepot(consist)) return true;

	if (!mode) consist->ShowVisualEffect();

	/* We had no order but have an order now, do look ahead. */
	if (!valid_order && !consist->current_order.IsType(OT_NOTHING)) {
		CheckNextTrainTile(consist);
	}

	/* Handle stuck trains. */
	if (!mode && consist->flags.Test(VehicleRailFlag::Stuck)) {
		++consist->wait_counter;

		/* Should we try reversing this tick if still stuck? */
		bool turn_around = consist->wait_counter % (_settings_game.pf.wait_for_pbs_path * Ticks::DAY_TICKS) == 0 && _settings_game.pf.reverse_at_signals;

		if (!turn_around && consist->wait_counter % _settings_game.pf.path_backoff_interval != 0 && consist->force_proceed == TFP_NONE) return true;
		if (!TryPathReserve(consist)) {
			/* Still stuck. */
			/* Turning round to try the other way is no use when the other
			 * way is blocked by the wagons this train has just left there. */
			if (turn_around && !WouldReverseIntoFreeWagons(consist)) ReverseTrainDirection(consist);

			if (consist->flags.Test(VehicleRailFlag::Stuck) && consist->wait_counter > 2 * _settings_game.pf.wait_for_pbs_path * Ticks::DAY_TICKS) {
				/* Show message to player. */
				if (_settings_client.gui.lost_vehicle_warn && consist->owner == _local_company) {
					AddVehicleAdviceNewsItem(AdviceType::TrainStuck, GetEncodedString(STR_NEWS_TRAIN_IS_STUCK, consist->index), consist->index);
				}
				consist->wait_counter = 0;
			}
			/* Exit if force proceed not pressed, else reset stuck flag anyway. */
			if (consist->force_proceed == TFP_NONE) return true;
			consist->flags.Reset(VehicleRailFlag::Stuck);
			consist->wait_counter = 0;
			SetWindowWidgetDirty(WindowClass::VehicleView, consist->index, WID_VV_START_STOP);
		}
	}

	if (consist->current_order.IsType(OT_LEAVESTATION)) {
		consist->current_order.Free();
		SetWindowWidgetDirty(WindowClass::VehicleView, consist->index, WID_VV_START_STOP);
		return true;
	}

	int j = consist->UpdateSpeed();

	/* we need to invalidate the widget if we are stopping from 'Stopping 0 km/h' to 'Stopped' */
	if (consist->cur_speed == 0 && consist->vehstatus.Test(VehState::Stopped)) {
		/* If we manually stopped, we're not force-proceeding anymore. */
		consist->force_proceed = TFP_NONE;
		InvalidateWindowData(WindowClass::VehicleView, consist->index);
	}

	Train* moving_front = consist->GetMovingFront();
	int adv_spd = moving_front->GetAdvanceDistance();
	if (j < adv_spd) {
		/* if the vehicle has speed 0, update the last_speed field. */
		if (consist->cur_speed == 0) consist->SetLastSpeed();
	} else {
		TrainCheckIfLineEnds(moving_front);
		moving_front = moving_front->GetMovingFront();
		/* Loop until the train has finished moving. */
		for (;;) {
			j -= adv_spd;
			TrainController(moving_front, nullptr);
			moving_front = moving_front->GetMovingFront();
			/* Don't continue to move if the train crashed. */
			if (CheckTrainCollision(moving_front)) break;
			/* Determine distance to next map position */
			adv_spd = moving_front->GetAdvanceDistance();

			/* No more moving this tick */
			if (j < adv_spd || consist->cur_speed == 0) break;

			OrderType order_type = consist->current_order.GetType();
			/* Do not skip waypoints (incl. 'via' stations) when passing through at full speed. */
			if ((order_type == OT_GOTO_WAYPOINT || order_type == OT_GOTO_STATION) &&
						consist->current_order.GetNonStopType().Test(OrderNonStopFlag::GoVia) &&
						IsTileType(moving_front->tile, TileType::Station) &&
						consist->current_order.GetDestination() == GetStationIndex(moving_front->tile)) {
				ProcessOrders(consist);
			}
		}
		consist->SetLastSpeed();
	}

	for (Train *u = consist; u != nullptr; u = u->Next()) {
		if (u->vehstatus.Test(VehState::Hidden)) continue;

		u->UpdateViewport(false, false);
	}

	if (consist->progress == 0) consist->progress = j; // Save unused spd for next time, if TrainController didn't set progress

	return true;
}

/**
 * Get running cost for the train consist.
 * @return Yearly running costs.
 */
Money Train::GetRunningCost() const
{
	Money cost = 0;
	const Train *v = this;

	do {
		const Engine *e = v->GetEngine();
		if (e->VehInfo<RailVehicleInfo>().running_cost_class == Price::Invalid) continue;

		uint cost_factor = GetVehicleProperty(v, PROP_TRAIN_RUNNING_COST_FACTOR, e->VehInfo<RailVehicleInfo>().running_cost);
		if (cost_factor == 0) continue;

		/* Halve running cost for multiheaded parts */
		if (v->IsMultiheaded()) cost_factor /= 2;

		cost += GetPrice(e->VehInfo<RailVehicleInfo>().running_cost_class, cost_factor, e->GetGRF());
	} while ((v = v->GetNextVehicle()) != nullptr);

	return cost;
}

/**
 * Update train vehicle data for a tick.
 * @return True if the vehicle still exists, false if it has ceased to exist (front of consists only).
 */
bool Train::Tick()
{
	this->tick_counter++;

	if (this->IsFrontEngine()) {
		PerformanceAccumulator framerate(PerformanceElement::GameLoopTrains);

		if (!this->vehstatus.Test(VehState::Stopped) || this->cur_speed > 0) this->running_ticks++;

		this->current_order_time++;

		if (!TrainLocoHandler(this, false)) return false;

		return TrainLocoHandler(this, true);
	} else if (this->IsFreeWagon() && !this->vehstatus.Test(VehState::Crashed)) {
		/* A rake of wagons left at a platform works through the order the engine
		 * left it with -- fill up, or take nothing -- and only when that is done
		 * does it become something waiting to be collected. Nobody else will
		 * notice that moment for it: a rake has no engine at its head, so none
		 * of the code that moves a train on from one thing to the next is ever
		 * asked about it. This is the one place it gets a look in. */
		if (this->current_order.IsType(OT_LOADING) && !this->current_order.ShouldWaitForCouple() &&
				this->vehicle_flags.Test(VehicleFlag::LoadingFinished)) {
			AdoptWagonRakeOrder(this, 1);
		}
	} else if (this->IsFreeWagon() && this->vehstatus.Test(VehState::Crashed)) {
		/* Delete flooded standalone wagon chain */
		if (++this->crash_anim_pos >= 4400) {
			delete this;
			return false;
		}
	}

	return true;
}

/**
 * Check whether a train needs service, and if so, find a depot or service it.
 * @param v %Train to check.
 */
static void CheckIfTrainNeedsService(Train *v)
{
	if (Company::Get(v->owner)->settings.vehicle.servint_trains == 0 || !v->NeedsAutomaticServicing()) return;
	if (v->IsChainInDepot()) {
		VehicleServiceInDepot(v);
		return;
	}

	uint max_penalty = _settings_game.pf.yapf.maximum_go_to_depot_penalty;

	FindDepotData tfdd = FindClosestTrainDepot(v, max_penalty);
	/* Only go to the depot if it is not too far out of our way. */
	if (tfdd.best_length == UINT_MAX || tfdd.best_length > max_penalty) {
		if (v->current_order.IsType(OT_GOTO_DEPOT)) {
			/* If we were already heading for a depot but it has
			 * suddenly moved farther away, we continue our normal
			 * schedule? */
			v->current_order.MakeDummy();
			SetWindowWidgetDirty(WindowClass::VehicleView, v->index, WID_VV_START_STOP);
		}
		return;
	}

	DepotID depot = GetDepotIndex(tfdd.tile);

	if (v->current_order.IsType(OT_GOTO_DEPOT) &&
			v->current_order.GetDestination() != depot &&
			!Chance16(3, 16)) {
		return;
	}

	v->gv_flags.Set(GroundVehicleFlag::SuppressImplicitOrders);
	v->current_order.MakeGoToDepot(depot, OrderDepotTypeFlag::Service, OrderNonStopFlag::NonStop, OrderDepotActionFlag::NearestDepot);
	v->dest_tile = tfdd.tile;
	SetWindowWidgetDirty(WindowClass::VehicleView, v->index, WID_VV_START_STOP);
}

/** Calendar day handler. */
void Train::OnNewCalendarDay()
{
	AgeVehicle(this);
}

/** Economy day handler. */
void Train::OnNewEconomyDay()
{
	EconomyAgeVehicle(this);

	if ((++this->day_counter & 7) == 0) DecreaseVehicleValue(this);

	if (this->IsFrontEngine()) {
		CheckVehicleBreakdown(this);

		CheckIfTrainNeedsService(this);

		CheckOrders(this);

		/* update destination */
		if (this->current_order.IsType(OT_GOTO_STATION)) {
			TileIndex tile = Station::Get(this->current_order.GetDestination().ToStationID())->train_station.tile;
			if (tile != INVALID_TILE) this->dest_tile = tile;
		}

		if (this->running_ticks != 0) {
			/* running costs */
			CommandCost cost(ExpensesType::TrainRun, this->GetRunningCost() * this->running_ticks / (CalendarTime::DAYS_IN_YEAR  * Ticks::DAY_TICKS));

			this->profit_this_year -= cost.GetCost();
			this->running_ticks = 0;

			SubtractMoneyFromCompanyFract(this->owner, cost);

			SetWindowDirty(WindowClass::VehicleDetails, this->index);
			SetWindowClassesDirty(WindowClass::TrainList);
		}
	}
}

/**
 * Get the tracks of the train vehicle.
 * @return Current tracks of the vehicle.
 */
Trackdir Train::GetVehicleTrackdir() const
{
	if (this->vehstatus.Test(VehState::Crashed)) return Trackdir::Invalid;

	if (this->track == Track::Depot) {
		/* We'll assume the train is facing outwards */
		return DiagDirToDiagTrackdir(GetRailDepotDirection(this->tile)); // Train in depot
	}

	if (this->track == Track::Wormhole) {
		/* train in tunnel or on bridge, so just use its direction and assume a diagonal track */
		return DiagDirToDiagTrackdir(DirToDiagDir(this->GetMovingDirection()));
	}

	return TrackDirectionToTrackdir(FindFirstTrack(this->track), this->GetMovingDirection());
}

uint16_t Train::GetMaxWeight() const
{
	uint16_t weight = CargoSpec::Get(this->cargo_type)->WeightOfNUnitsInTrain(this->GetEngine()->DetermineCapacity(this));

	/* Vehicle weight is not added for articulated parts. */
	if (!this->IsArticulatedPart()) {
		weight += GetVehicleProperty(this, PROP_TRAIN_WEIGHT, RailVehInfo(this->engine_type)->weight);
	}

	/* Powered wagons have extra weight added. */
	if (this->flags.Test(VehicleRailFlag::PoweredWagon)) {
		weight += RailVehInfo(this->gcache.first_engine)->pow_wag_weight;
	}

	return weight;
}
