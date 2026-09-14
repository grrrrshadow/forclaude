/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file newgrf_act0_signals.cpp NewGRF Action 0x00 handler for signals (feature 0E). */

#include "../stdafx.h"
#include "../debug.h"
#include "../newgrf_signals.h"
#include "newgrf_bytereader.h"
#include "newgrf_internal.h"
#include "newgrf_stringmapping.h"

#include "../safeguards.h"

/**
 * Define properties for signals.
 *
 * Every property here is one a set asked for by name (see
 * GRFFile::action0_property_remaps): plain OpenTTD has no properties for
 * feature 0E at all, and the patchpack's have no fixed numbers, so the number
 * in the sprite means nothing until it is looked up. A property asked for by
 * name also carries its own length, which is read and honoured -- a set may
 * write a longer value than this game knows what to do with, and the rest of
 * the sprite has to be stepped over either way.
 *
 * The Action 0 id is not used by any of them; a set writes whatever it likes
 * there. What the properties describe is the set's list of signal styles: each
 * one is opened by a number, and the properties after it belong to it.
 *
 * @param first Local ID of the first signal.
 * @param last Local ID of the last signal.
 * @param prop The property to change.
 * @param buf The property value.
 * @return ChangeInfoResult.
 */
static ChangeInfoResult SignalsChangeInfo(uint first, uint last, int prop, ByteReader &buf)
{
	ChangeInfoResult ret = ChangeInfoResult::Success;

	GRFFile::MappedProperty mapped = _cur_gps.grffile->GetMappedProperty(to_underlying(GrfSpecFeature::Signals), prop);

	for (uint id = first; id < last; ++id) {
		if (mapped == GRFFile::MappedProperty::None) {
			ret = ChangeInfoResult::Unknown;
			break;
		}

		/* A mapped property is written as its own length and then its data. */
		uint16_t size = buf.ReadExtendedByte();
		size_t after = buf.Remaining() - size;

		/* The style the properties after a "define style" belong to: the last
		 * one this set opened. */
		SignalStyle *style = nullptr;
		for (auto it = _signal_styles.rbegin(); it != _signal_styles.rend(); ++it) {
			if (it->grf == _cur_gps.grffile) { style = &*it; break; }
		}

		switch (mapped) {
			case GRFFile::MappedProperty::SignalsExtraAspects:
				/* Said once for the whole set, before it opens any style, so
				 * it is kept on the set and handed to every style it opens. */
				_cur_gps.grffile->signal_extra_aspects = buf.ReadByte();
				for (SignalStyle &st : _signal_styles) {
					if (st.grf == _cur_gps.grffile) st.extra_aspects = _cur_gps.grffile->signal_extra_aspects;
				}
				GrfMsg(2, "SignalsChangeInfo: set draws {} aspects beyond red and green", _cur_gps.grffile->signal_extra_aspects);
				break;

			case GRFFile::MappedProperty::SignalsDefineStyle: {
				/* The patchpack allows fifteen styles in a game. More than
				 * that and the rest are read and forgotten rather than
				 * refused: a set that defines many is offering a choice, and
				 * losing the tail of the list is better than losing the set. */
				uint8_t local = buf.ReadByte();
				if (_signal_styles.size() >= 15) {
					GrfMsg(1, "SignalsChangeInfo: no room for style {}, fifteen is the limit", local);
					break;
				}
				SignalStyle &st = _signal_styles.emplace_back();
				st.grf = _cur_gps.grffile;
				st.local_id = local;
				st.extra_aspects = _cur_gps.grffile->signal_extra_aspects;
				GrfMsg(2, "SignalsChangeInfo: style {} defined", local);
				break;
			}

			case GRFFile::MappedProperty::SignalsStyleName: {
				GRFStringID str{buf.ReadWord()};
				if (style == nullptr) {
					GrfMsg(1, "SignalsChangeInfo: a name before any style was defined, ignoring");
					break;
				}
				AddStringForMapping(str, &style->name);
				break;
			}

			case GRFFile::MappedProperty::SignalsStyleElectric:
				if (style == nullptr) {
					GrfMsg(1, "SignalsChangeInfo: a kind list before any style was defined, ignoring");
					break;
				}
				style->electric_enabled = buf.ReadDWord();
				break;

			default:
				ret = ChangeInfoResult::Unknown;
				break;
		}

		/* Whatever of the value was read, the rest of it is stepped over: the
		 * set says how long it is and a shorter reading here would put every
		 * property after it out of step. */
		while (buf.Remaining() > after) buf.ReadByte();
		(void)id;
	}

	return ret;
}

/** @copybrief GrfChangeInfoHandler::Reserve @return Always ChangeInfoResult::Unhandled. */
template <> ChangeInfoResult GrfChangeInfoHandler<GrfSpecFeature::Signals>::Reserve(uint, uint, int, ByteReader &) { return ChangeInfoResult::Unhandled; }
/** @copydoc GrfChangeInfoHandler::Activation */
template <> ChangeInfoResult GrfChangeInfoHandler<GrfSpecFeature::Signals>::Activation(uint first, uint last, int prop, ByteReader &buf) { return SignalsChangeInfo(first, last, prop, buf); }
