/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file green_load.cpp The loaded pictures of the marijuana wagons and lorries, see green_load.h. */

#include "stdafx.h"
#include "green_load.h"
#include "spritecache.h"

#include "table/sprites.h"

#include <map>

#include "safeguards.h"

/** Every loaded picture drawn green: the base set's loaded picture, and the green one made from it. */
static std::vector<std::pair<SpriteID, SpriteID>> _green_loads;
/** A set's load layers drawn green so far: the set's layer, and the green one made from it (GreenLayerSprite()). */
static std::map<SpriteID, SpriteID> _green_layers;
/** A set's loaded wagons drawn with the load green so far: loaded and empty picture, and the green one (GreenLoadedSprite()). */
static std::map<std::pair<SpriteID, SpriteID>, SpriteID> _green_loaded;
/** The next free sprite in the block after SPR_GREEN_LAYER_BASE, shared by the two above. */
static SpriteID _green_layer_next = SPR_GREEN_LAYER_BASE;

/**
 * Make the loaded pictures of the marijuana wagons and lorries, in the block
 * after SPR_GREEN_LOAD_BASE: one for every direction of every picture they
 * use. Called once the base set is loaded and before the NewGRFs, so they are
 * made from the base set's pictures.
 */
void SetupGreenLoadSprites()
{
	_green_loads.clear();
	_green_layers.clear();
	_green_loaded.clear();
	_green_layer_next = SPR_GREEN_LAYER_BASE;
	SpriteID next = SPR_GREEN_LOAD_BASE;
	for (const auto &[type, image] : MarijuanaEngineImages()) {
		for (const auto &[empty, full] : type == VehicleType::Train ? WagonLoadPictures(image) : RoadVehicleLoadPictures(image)) {
			if (std::ranges::find(_green_loads, full, &std::pair<SpriteID, SpriteID>::first) != std::end(_green_loads)) continue;
			assert(next < SPR_GREEN_LOAD_BASE + GREEN_LOAD_SPRITE_COUNT);
			SetGreenLoadSprite(next, full, empty);
			_green_loads.emplace_back(full, next);
			next++;
		}
	}
}

/**
 * The picture to draw for something loaded with marijuana.
 * @param full the loaded picture of the base set
 * @return the same with the load green, or the picture itself where it has no green one
 */
SpriteID GreenLoadSprite(SpriteID full)
{
	auto it = std::ranges::find(_green_loads, full, &std::pair<SpriteID, SpriteID>::first);
	return it == std::end(_green_loads) ? full : it->second;
}

/**
 * The load layer of a set's wagon, drawn green: for a wagon that draws its load
 * as a picture of its own over the wagon -- the St of CZTR Wagons, whose coal
 * is a layer -- carrying marijuana (IsGreenLayerWagon()). The set knows
 * nothing of marijuana and draws coal; this is the same coal in the leaf
 * greens of the palette, by the rule the game's own marijuana wagons are drawn
 * by, so it comes out as the patch drawn for it by hand (the grrrrf repository,
 * vagony-mari), and follows the set if it ever draws its coal anew.
 *
 * Made the first time each layer is drawn, in the block after
 * SPR_GREEN_LAYER_BASE; a layer past the end of it is drawn as it is.
 * @param layer the set's load layer
 * @return the same in green
 */
SpriteID GreenLayerSprite(SpriteID layer)
{
	auto it = _green_layers.find(layer);
	if (it != _green_layers.end()) return it->second;
	if (_green_layer_next >= SPR_GREEN_LAYER_BASE + GREEN_LAYER_SPRITE_COUNT) return layer;
	SpriteID green = _green_layer_next++;
	SetGreenLayerSprite(green, layer);
	_green_layers.emplace(layer, green);
	return green;
}

/**
 * The loaded picture of a set's wagon with its load drawn green, for a wagon
 * whose set draws the load into the wagon's own picture rather than as a layer
 * over it -- the St of CZTR Wagons 1.0.0. Every pixel the loaded picture does
 * not share with the empty one is the load (SetGreenLoadSprite()), the rule
 * the game's own marijuana wagons are drawn by.
 * @param full the loaded picture
 * @param empty the same wagon empty
 * @return the loaded picture with the load green
 */
SpriteID GreenLoadedSprite(SpriteID full, SpriteID empty)
{
	auto it = _green_loaded.find({full, empty});
	if (it != _green_loaded.end()) return it->second;
	if (_green_layer_next >= SPR_GREEN_LAYER_BASE + GREEN_LAYER_SPRITE_COUNT) return full;
	SpriteID green = _green_layer_next++;
	SetGreenLoadSprite(green, full, empty);
	_green_loaded.emplace(std::pair<SpriteID, SpriteID>{full, empty}, green);
	return green;
}

/**
 * The set's load layers drawn green so far, for the rig.
 * @return the set's layer and the green one, pair by pair
 */
std::vector<std::pair<SpriteID, SpriteID>> GreenLayerSprites()
{
	std::vector<std::pair<SpriteID, SpriteID>> made(_green_layers.begin(), _green_layers.end());
	for (const auto &[pictures, green] : _green_loaded) made.emplace_back(pictures.first, green);
	return made;
}

/**
 * Every loaded picture drawn green, for the rig.
 * @return the base set's loaded picture and the green one, pair by pair
 */
std::vector<std::pair<SpriteID, SpriteID>> GreenLoadSprites()
{
	return _green_loads;
}
