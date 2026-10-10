/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file mars_houses.cpp Fetching the Mars houses from the content server when the game does not have them. */

#include "stdafx.h"
#include "mars_houses.h"
#include "newgrf_config.h"
#include "network/network.h"
#include "network/network_content.h"
#include "openttd.h"
#include "settings_type.h"
#include "debug.h"

#include "safeguards.h"

/**
 * Does the game have the Mars houses to put into a new game?
 * @return whether a usable copy of the set was found on disk
 */
bool HaveMarsHouses()
{
	return FindGRFConfig(MARS_HOUSES_GRFID, FindGRFConfigMode::NewestValid) != nullptr;
}

/**
 * Asks the content server for the Mars houses and downloads them, with no
 * window: the game is in its menu or already playing, and a player who never
 * looks at the Mars towns should not be asked anything. Once they are down,
 * the sets on disk are scanned again, so the next new game has them.
 * Whatever goes wrong -- no connection, no such set, a failed download -- it
 * gives up quietly; the game asks again the next time it starts.
 */
class MarsHousesFetcher : public ContentCallback {
	ContentID wanted = INVALID_CONTENT_ID; ///< The Mars houses' id on the server, once it said.

public:
	/** The fetch under way, if there is one. */
	static inline MarsHousesFetcher *running = nullptr;

	MarsHousesFetcher()
	{
		running = this;
		_network_content_client.AddCallback(this);

		/* Asked for by the set's id alone, without a checksum: the server
		 * answers with the release it offers for new games. */
		ContentVector cv;
		auto ci = std::make_unique<ContentInfo>();
		ci->type = ContentType::NewGRF;
		ci->unique_id = std::byteswap(MARS_HOUSES_GRFID);
		cv.push_back(std::move(ci));
		_network_content_client.RequestContentList(&cv, false);
	}

	~MarsHousesFetcher() override
	{
		_network_content_client.RemoveCallback(this);
		running = nullptr;
	}

	void OnConnect(bool success) override
	{
		if (success) return;
		Debug(net, 1, "Mars houses: no connection to the content server");
		delete this;
	}

	void OnDisconnect() override
	{
		/* The connection closes when it has been idle for a while, which is
		 * also how an answer that never came ends. Once the download is under
		 * way it may close too -- the file comes over HTTP -- and the fetch
		 * then waits for the download to finish. */
		if (this->wanted == INVALID_CONTENT_ID) delete this;
	}

	void OnReceiveContentInfo(const ContentInfo &ci) override
	{
		if (this->wanted != INVALID_CONTENT_ID) return;
		if (ci.type != ContentType::NewGRF || ci.unique_id != std::byteswap(MARS_HOUSES_GRFID)) return;
		if (ci.state != ContentInfo::State::Unselected) {
			/* Here already, or not on the server. */
			delete this;
			return;
		}
		this->wanted = ci.id;
		_network_content_client.Select(ci.id);
		uint files, bytes;
		_network_content_client.DownloadSelectedContent(files, bytes);
		Debug(net, 1, "Mars houses: downloading {} ({} bytes)", ci.name, bytes);
	}

	void OnDownloadComplete(ContentID cid) override
	{
		if (cid != this->wanted) return;
		Debug(net, 1, "Mars houses: downloaded");
		RequestNewGRFScan();
		delete this;
	}
};

/**
 * Fetch the Mars houses when the game does not have them and wants them: a
 * new game makes Mars towns (economy.mars_towns). Called as the game starts,
 * so that the first start of this build fetches them -- and every start after
 * one that could not.
 */
void FetchMarsHousesIfMissing()
{
	if (!_network_available || MarsHousesFetcher::running != nullptr) return;
	if (_settings_newgame.economy.mars_towns == 0 || HaveMarsHouses()) return;
	new MarsHousesFetcher();
}
