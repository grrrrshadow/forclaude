/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file agent_api.h The game's interface for a program that plays it (an AI agent, a test): a small HTTP server on this
 * computer alone, off till the console's command "api" turns it on.
 *
 * The player, 17. 11.: "make an interface for an agent; we take that game as an example" (OpenSC2K's remote API, the
 * notes' hra/INSPIRACE.md). What it gives: the state of the game, the tiles of the map as data, a picture of the
 * screen, and the console -- every command of it, the rig's "test3d" too, with what it printed.
 */

#ifndef AGENT_API_H
#define AGENT_API_H

#include <cstdint>
#include <string>

/** The port it listens on when none is given. */
static constexpr uint16_t AGENT_API_DEFAULT_PORT = 9241;

/**
 * Start it on a port of 127.0.0.1 (stopped first if it runs).
 * @return why not, empty when it listens
 */
std::string AgentApiStart(uint16_t port);

/** Stop it; nothing when it does not run. */
void AgentApiStop();

/** The port it listens on, 0 when it does not. */
uint16_t AgentApiPort();

/** What its requests wait for done, on the game's thread (GameLoop()). */
void AgentApiPump();

#endif /* AGENT_API_H */
