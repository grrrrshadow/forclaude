/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file cztr_wagons.cpp Fetching CZTR Wagons-Cargo 1.0.0 when a game plays FIRS 5 with another release of the set. */

#include "stdafx.h"
#include "cztr_wagons.h"
#include "newgrf.h"
#include "newgrf_config.h"
#include "network/network.h"
#include "network/network_content.h"
#include "openttd.h"
#include "debug.h"

#include "safeguards.h"

/**
 * Put CZTR Wagons-Cargo 1.0.0, now on the disk, in the other release's place:
 * in the sets of the next game, and in the game being played, which loads its
 * sets anew for it -- the swap itself happens as they load (LoadNewGRF()).
 */
static void ActivateCztrWagonsForFirs5()
{
	SwapInCztrWagonsForFirs5(_grfconfig_newgame);
	if (_game_mode == GameMode::Menu || _networking) return;
	if (!CztrWagonsForFirs5Wanted(_grfconfig)) return;
	Debug(net, 1, "CZTR Wagons-Cargo 1.0.0: loading the game's sets anew with it");
	ReloadNewGRFData();
}

/**
 * The player's word on FIRS 5 and the CZTR wagons: everything works with CZTR
 * Wagons-Cargo 1.0.0, all cargoes, so a game that starts with FIRS 5 and
 * another release of the wagons is not to stop and show a message -- it
 * fetches 1.0.0 and plays it in the other release's place. That release is a
 * superseded one, kept by the content service for savegames only, so it is
 * asked for by checksum, the way a savegame asks for what it names
 * (SavegameContentFetchWindow); the service knows the first four bytes of the
 * checksum (CztrWagonsForFirs5Identifier()).
 *
 * No window: the game is in its menu or already playing, and the player said
 * he would not be looking. Once the set is down the disk is scanned again and
 * the release put in the other's place (ActivateCztrWagonsForFirs5()).
 * Whatever goes wrong -- no connection, no answer, the service has not got it
 * -- it gives up quietly; the game asks again the next time it starts.
 */
class CztrWagonsFetcher : public ContentCallback, NewGRFScanCallback {
	ContentID wanted = INVALID_CONTENT_ID; ///< The release's id on the server, once it said.
	bool downloaded = false; ///< On the disk; waiting for the scan of the disk.

public:
	/** The fetch under way, if there is one. */
	static inline CztrWagonsFetcher *running = nullptr;

	CztrWagonsFetcher()
	{
		running = this;
		_network_content_client.AddCallback(this);

		GRFIdentifier id = CztrWagonsForFirs5Identifier();
		ContentVector cv;
		auto ci = std::make_unique<ContentInfo>();
		ci->type = ContentType::NewGRF;
		ci->state = ContentInfo::State::DoesNotExist;
		ci->unique_id = std::byteswap(id.grfid);
		ci->md5sum = id.md5sum;
		cv.push_back(std::move(ci));
		_network_content_client.RequestContentList(&cv, true);
		Debug(net, 1, "CZTR Wagons-Cargo 1.0.0: asking the content server for it");
	}

	~CztrWagonsFetcher() override
	{
		_network_content_client.RemoveCallback(this);
		running = nullptr;
	}

	void OnConnect(bool success) override
	{
		if (success) return;
		Debug(net, 1, "CZTR Wagons-Cargo 1.0.0: no connection to the content server");
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
		if (ci.type != ContentType::NewGRF || !IsCztrWagonsForFirs5(std::byteswap(ci.unique_id), ci.md5sum)) return;
		if (!ci.IsValid() || ci.state != ContentInfo::State::Unselected) {
			/* Here already, or not on the server. */
			Debug(net, 1, "CZTR Wagons-Cargo 1.0.0: the content server has not got it");
			delete this;
			return;
		}
		this->wanted = ci.id;
		_network_content_client.Select(ci.id);
		uint files, bytes;
		_network_content_client.DownloadSelectedContent(files, bytes);
		Debug(net, 1, "CZTR Wagons-Cargo 1.0.0: downloading {} ({} bytes)", ci.name, bytes);
	}

	void OnDownloadComplete(ContentID cid) override
	{
		if (cid != this->wanted || this->downloaded) return;
		this->downloaded = true;
		Debug(net, 1, "CZTR Wagons-Cargo 1.0.0: downloaded");
		/* On the disk now, but the game's list of what is on the disk was made
		 * before it was; it is put in place once that list has been made again. */
		if (!RequestNewGRFScan(this)) delete this;
	}

	void OnNewGRFsScanned() override
	{
		ActivateCztrWagonsForFirs5();
		delete this;
	}
};

/**
 * Fetch CZTR Wagons-Cargo 1.0.0 when the sets of the next game, or of the game
 * being played, play FIRS 5 with another release of the wagons and the disk
 * has not got 1.0.0. Called as the game starts and as a game is started or
 * loaded, so that the first start with such sets fetches it -- and every start
 * after one that could not.
 */
void FetchCztrWagonsForFirs5IfMissing()
{
	if (!_network_available || CztrWagonsFetcher::running != nullptr) return;
	bool wanted = CztrWagonsForFirs5Missing(_grfconfig_newgame) || (_game_mode != GameMode::Menu && CztrWagonsForFirs5Missing(_grfconfig));
	if (!wanted) return;
	new CztrWagonsFetcher();
}

/**
 * Is CZTR Wagons-Cargo 1.0.0 on its way from the content server? The message
 * on the release it will stand in for says so (LoadNewGRF()).
 */
bool IsFetchingCztrWagonsForFirs5()
{
	return CztrWagonsFetcher::running != nullptr;
}
