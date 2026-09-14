/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file newgrf_signals.cpp Signal graphics that come out of a NewGRF (feature 0E). */

#include "stdafx.h"
#include "debug.h"
#include "newgrf.h"
#include "newgrf_signals.h"
#include "newgrf_spritegroup.h"
#include "newgrf_railtype.h"
#include "rail.h"

#include "safeguards.h"

/** Every style every loaded set has defined, in the order they were read. */
std::vector<SignalStyle> _signal_styles;

/** Which of them is drawn with; worked out once per set of loaded files. */
static uint _signal_style_in_use = UINT_MAX;

/** Forget every style; a new game reads them again from the sets it loads. */
void ResetSignalStyles()
{
	_signal_styles.clear();
	/* Room for the whole list before anything is put in it. A style's name
	 * arrives later, through a string mapping that was handed the address of
	 * the style's own field; a vector that grew afterwards moved the styles
	 * out from under those addresses and the names of everything but the last
	 * few came out as "(undefined string)". Fifteen is the limit, so the room
	 * is taken once and never moves. */
	_signal_styles.reserve(15);
	_signal_style_in_use = UINT_MAX;
}

/**
 * Which style the signals are drawn in.
 *
 * The first one that draws the aspect between red and green -- what this game
 * shows as yellow -- with a picture of its own. A set offers several styles
 * and some of them draw two aspects only: the one the set this was built
 * against calls "Two Aspect" answers the yellow with its green, and a game
 * drawn in that style never shows a yellow at all, which is the whole reason
 * for reading the set. So the list is asked rather than taken in order, and
 * only a set that draws no third aspect anywhere falls back to its first
 * style.
 *
 * There is nothing to choose here and no window to choose it in: the styles
 * are not kinds of signal a player builds, they are pictures for the six
 * kinds this game already has.
 *
 * The answer depends on nothing but the loaded files, so it is worked out
 * once and kept.
 *
 * @return the index into #_signal_styles, or #_signal_styles.size() for the
 *         built-in pictures
 */
uint GetSignalStyleInUse()
{
	if (_signal_style_in_use != UINT_MAX) return _signal_style_in_use;
	if (_signal_styles.empty()) return 0;

	const RailTypeInfo *rti = GetRailTypeInfo(RAILTYPE_RAIL);
	uint first_drawing = _signal_styles.size();
	for (uint i = 0; i < _signal_styles.size(); i++) {
		SpriteID green = GetCustomSignalStyleSprite(i, rti, INVALID_TILE, SignalType::Path, SignalVariant::Electric, SignalAspect::Green, true);
		if (green == 0) continue;
		if (first_drawing == _signal_styles.size()) first_drawing = i;
		SpriteID yellow = GetCustomSignalStyleSprite(i, rti, INVALID_TILE, SignalType::Path, SignalVariant::Electric, SignalAspect::Warning, true);
		if (yellow != 0 && yellow != green) {
			_signal_style_in_use = i;
			return i;
		}
	}
	_signal_style_in_use = first_drawing;
	return first_drawing;
}

/** Resolver for a signal drawn in a style a set defined. */
struct SignalScopeResolver : public RailTypeScopeResolver {
	uint8_t style; ///< The style's number inside the set that defined it.

	SignalScopeResolver(ResolverObject &ro, const RailTypeInfo *rti, TileIndex tile, uint8_t style)
		: RailTypeScopeResolver(ro, rti, tile, TileContext::Normal), style(style)
	{
	}

	uint32_t GetVariable(uint8_t variable, uint32_t parameter, bool &available) const override
	{
		/* The one variable a signal has that a railtype has not: which style
		 * is being drawn. The set reads it under a number of its own choosing
		 * and says so in an 'A2VM' block; the read is turned into this one
		 * where the group is parsed (see newgrf_act2.cpp). */
		if (variable == NEWGRF_SIGNAL_STYLE_VAR) return this->style;
		return RailTypeScopeResolver::GetVariable(variable, parameter, available);
	}
};

/** Resolver object for signals drawn out of a set. */
struct SignalResolverObject : public ResolverObject {
	SignalScopeResolver signal_scope;

	SignalResolverObject(const GRFFile *grffile, const SpriteGroup *group, const RailTypeInfo *rti, TileIndex tile, uint8_t style, uint32_t param1, uint32_t param2)
		: ResolverObject(grffile, CBID_NO_CALLBACK, param1, param2), signal_scope(*this, rti, tile, style)
	{
		this->root_spritegroup = group;
	}

	ScopeResolver *GetScope(VarSpriteGroupScope scope = VarSpriteGroupScope::Self, uint8_t relative = 0) override
	{
		switch (scope) {
			case VarSpriteGroupScope::Self: return &this->signal_scope;
			default: return ResolverObject::GetScope(scope, relative);
		}
	}

	GrfSpecFeature GetFeature() const override { return GrfSpecFeature::Signals; }
	uint32_t GetDebugID() const override { return this->signal_scope.style; }
};

/**
 * The picture a set draws for a signal in one of its styles.
 *
 * What is asked and how is the same as for a railtype's own signal sprites
 * (see GetCustomSignalSprite()): the kind, the variant and the aspect go in
 * the second callback parameter, with nought red, one green and from two up
 * the aspects between them. What is different is where the answer comes from
 * -- the set's own signal group rather than a railtype's -- and that the style
 * is handed to it as well, since one set draws several and answers for all of
 * them out of the same group.
 *
 * @param style which of #_signal_styles, out of range for the built-in ones
 * @param rti the rail type the signal stands on
 * @param tile the tile it stands on, INVALID_TILE in a window
 * @param type the kind of signal
 * @param variant electric or semaphore
 * @param aspect what it is showing
 * @param gui drawn in a window rather than on the map
 * @return the sprite, or 0 for "this set has nothing for that"
 */
SpriteID GetCustomSignalStyleSprite(uint style, const RailTypeInfo *rti, TileIndex tile, SignalType type, SignalVariant variant, SignalAspect aspect, bool gui)
{
	if (style >= _signal_styles.size()) return 0;
	const SignalStyle &st = _signal_styles[style];
	if (st.grf == nullptr || st.grf->signal_group == nullptr) return 0;

	/* A style names the kinds of signal it draws, one bit each and in this
	 * game's own order, and does it separately for the two variants. Asked
	 * for one it never said it draws, it answers with something -- the set
	 * that prompted this drew no semaphores at all and handed back its
	 * electric picture for them, which would have turned every semaphore on
	 * the map into a lamp. What a set does not draw is left to the base set. */
	uint32_t drawn = variant == SignalVariant::Semaphore ? st.semaphore_enabled : st.electric_enabled;
	if (!HasBit(drawn, to_underlying(type))) return 0;

	/* A style says how many aspects it draws; asked for one it has not got,
	 * it would answer with whatever its green happens to resolve to. */
	uint8_t asked = to_underlying(aspect);
	if (asked > 1 && st.extra_aspects + 1 < asked) asked = to_underlying(SignalAspect::Green);

	uint32_t param1 = gui ? 0x10 : 0x00;
	uint32_t param2 = (to_underlying(type) << 16) | (to_underlying(variant) << 8) | asked;
	SignalResolverObject object(st.grf, st.grf->signal_group, rti, tile, st.local_id, param1, param2);

	const auto *group = object.Resolve<ResultSpriteGroup>();
	if (group == nullptr || group->num_sprites == 0) return 0;

	return group->sprite;
}
