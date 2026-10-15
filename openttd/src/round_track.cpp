/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file round_track.cpp Round curves of rails, only how they look (round_track.h). */

#include "stdafx.h"
#include "round_track.h"
#include "rail_map.h"
#include "rail.h"
#include "newgrf_railtype.h"
#include "tile_map.h"
#include "map_func.h"
#include "settings_type.h"
#include "viewport_func.h"
#include "blitter/factory.hpp"
#include "zoom_func.h"
#include "landscape.h"
#include "gfx_func.h"
#include "vehicle_base.h"
#include "train.h"
#include "core/backup_type.hpp"
#include "table/sprites.h"

#include <cmath>
#include <map>
#include <unordered_map>
#include <unordered_set>

#include "safeguards.h"

/** The tangent of an arc reaches at most this far from its corner, world units: a tile, so an arc spans two. */
static constexpr double ROUND_TANGENT_MAX = TILE_SIZE;
/** A pixel this far from the centre line, world units, may be of the track; the picture of the straight track says whether it is. */
static constexpr double ROUND_HALF_WIDTH = 7.5;
/** The strip of a tile is drawn this far past its edges, world units. */
static constexpr double ROUND_EDGE = 1.0;
/** A pixel this much further from a piece than straight across from it is past the piece's end, world units. */
static constexpr double ROUND_BESIDE = 0.5;
/** The turn at a corner of the game's track. */
static const double ROUND_PHI = M_PI / 4;

/** Whether a renderer that draws the round curves is drawing now (RoundTrackPass). */
static bool _round_track_drawing = false;
/** The tiles whose track is drawn as a strip after the ground, this pass. */
static std::vector<TileIndex> _round_track_tiles;

/** The round curves are on: the setting, and a 32bpp blitter, whose pixels the strips are written into. */
bool RoundTrackEnabled()
{
	if (!_settings_client.gui.round_track_curves) return false;
	Blitter *b = BlitterFactory::GetCurrentBlitter();
	return b != nullptr && b->GetScreenDepth() == 32;
}

/** The round curves are on and the view being drawn draws them (RoundTrackPass). */
bool RoundTrackDrawing()
{
	return _round_track_drawing && RoundTrackEnabled();
}

namespace {

struct Vec {
	double x = 0, y = 0;
};
inline Vec operator+(Vec a, Vec b) { return {a.x + b.x, a.y + b.y}; }
inline Vec operator-(Vec a, Vec b) { return {a.x - b.x, a.y - b.y}; }
inline Vec operator*(Vec a, double k) { return {a.x * k, a.y * k}; }
inline double Dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y; }
inline double Cross(Vec a, Vec b) { return a.x * b.y - a.y * b.x; }
inline double Length(Vec a) { return std::sqrt(Dot(a, a)); }
inline Vec Unit(Vec a) { double l = Length(a); return l > 0 ? a * (1.0 / l) : a; }

/** The midpoint of a tile's edge, world units from its north corner. */
Vec EdgeMid(DiagDirection e)
{
	switch (e) {
		case DiagDirection::NE: return {0, 8};
		case DiagDirection::SE: return {8, 16};
		case DiagDirection::SW: return {16, 8};
		default:                return {8, 0}; // NW
	}
}

/** The two edges a piece of track joins, in the order of the edges. */
std::pair<DiagDirection, DiagDirection> PieceEdges(Track t)
{
	switch (t) {
		case Track::X:     return {DiagDirection::NE, DiagDirection::SW};
		case Track::Y:     return {DiagDirection::SE, DiagDirection::NW};
		case Track::Upper: return {DiagDirection::NE, DiagDirection::NW};
		case Track::Lower: return {DiagDirection::SE, DiagDirection::SW};
		case Track::Left:  return {DiagDirection::SW, DiagDirection::NW};
		default:           return {DiagDirection::NE, DiagDirection::SE}; // Right
	}
}

DiagDirection OtherEdge(Track t, DiagDirection e)
{
	auto [a, b] = PieceEdges(t);
	return e == a ? b : a;
}

/** A tile whose track may be part of a round curve: plain rail on flat ground, no signals; switches too. */
bool IsSimpleTile(TileIndex t)
{
	if (!IsValidTile(t) || !IsTileType(t, TileType::Railway) || !IsPlainRail(t)) return false;
	return !HasSignals(t) && GetTileSlope(t) == SLOPE_FLAT;
}

/** The way along a piece from one of its edges to the other, world units. */
Vec PieceVector(Track t, DiagDirection from)
{
	return EdgeMid(OtherEdge(t, from)) - EdgeMid(from);
}

TileIndex Neighbour(TileIndex t, DiagDirection e)
{
	int x = TileX(t), y = TileY(t);
	switch (e) {
		case DiagDirection::NE: x--; break;
		case DiagDirection::SE: y++; break;
		case DiagDirection::SW: x++; break;
		default:                y--; break;
	}
	if (x < 0 || y < 0 || x >= static_cast<int>(Map::SizeX()) || y >= static_cast<int>(Map::SizeY())) return INVALID_TILE;
	return TileXY(x, y);
}

enum class Turn { Straight, Corner, Other };

Turn TurnBetween(Vec a, Vec b)
{
	double c = Dot(Unit(a), Unit(b));
	if (c > 0.999) return Turn::Straight;
	if (std::abs(c - std::cos(ROUND_PHI)) < 0.01) return Turn::Corner;
	return Turn::Other;
}

/**
 * Whether a tile has a piece entered at an edge that goes on the way given: the straight way through a switch.
 * @param tile the tile
 * @param edge the edge it is entered at
 * @param dir the way, world units
 */
bool GoesStraightOn(TileIndex tile, DiagDirection edge, Vec dir)
{
	for (Track tr : GetTrackBits(tile)) {
		auto [a, b] = PieceEdges(tr);
		if ((a == edge || b == edge) && TurnBetween(dir, PieceVector(tr, edge)) == Turn::Straight) return true;
	}
	return false;
}

/** A piece of track walked over: from one edge of its tile to the other. */
struct Step {
	TileIndex tile;
	Track track;
	DiagDirection from;

	Vec Vector() const { return PieceVector(this->track, this->from); }
	Step Reversed() const { return {this->tile, this->track, OtherEdge(this->track, this->from)}; }
	/** The world point of the edge it leaves by. */
	Vec End() const { return Vec{static_cast<double>(TileX(this->tile) * TILE_SIZE), static_cast<double>(TileY(this->tile) * TILE_SIZE)} + EdgeMid(OtherEdge(this->track, this->from)); }
};

/**
 * The piece after a piece walked over, on the next simple tile at the same height. At a switch the way goes straight
 * on where it can: the straight way of a switch stays straight, only the way branching off it is round. Where it can
 * only turn, and either way, the walk ends.
 */
bool NextStep(const Step &s, Step &next)
{
	DiagDirection out = OtherEdge(s.track, s.from);
	TileIndex n = Neighbour(s.tile, out);
	if (n == INVALID_TILE || !IsSimpleTile(n) || TilePixelHeight(n) != TilePixelHeight(s.tile)) return false;
	DiagDirection in = ReverseDiagDir(out);
	Track found = Track::Invalid;
	int ways = 0;
	for (Track tr : GetTrackBits(n)) {
		auto [a, b] = PieceEdges(tr);
		if (a != in && b != in) continue;
		if (TurnBetween(s.Vector(), PieceVector(tr, in)) == Turn::Straight) {
			next = {n, tr, in};
			return true;
		}
		found = tr;
		ways++;
	}
	if (ways != 1) return false;
	next = {n, found, in};
	return true;
}

/** Which way a corner turns: +1 or -1, as the path is walked forward. */
int TurnSign(Vec a, Vec b)
{
	return Cross(a, b) > 0 ? 1 : -1;
}

/**
 * Two corners turning the same way with a straight between them no longer than this may be one arc: the plain curve
 * of the game and the wider ones, up to seven pieces of diagonal between the corners (the player: "when it is four
 * long and not two, it is straight").
 */
static const double ROUND_MERGE_RUN = 80.0;
/** The tangent of one arc of two corners, as a share of how far they are from where their straights meet: the circle nearest to the track, as near to its corners as to the middle of the straight between them. */
static const double ROUND_MERGE_TANGENT = 1.5;
/** One arc of two corners keeps at most this far from the track's centre line, world units; further, each corner is round alone. */
static const double ROUND_MERGE_DEVIATION = 5.0;

/** The straight from a corner along one side, up to where the path turns or the round curve ends. */
struct Side {
	double run = 0;      ///< its length
	bool corner = false; ///< it ends at another corner of 45 degrees
	int sign = 0;        ///< which way that corner turns, the path walked forward
	Vec dir;             ///< the direction of the path past that corner, the path walked forward
	Step last{};         ///< the last piece of the straight, walked the same way as the side
	Step beyond{};       ///< the piece past that corner, walked the same way as the side
};

/**
 * Walk the straight from a corner along one side.
 * @param first the first piece walked away from the corner
 * @param backward the side is walked against the path's direction
 */
Side WalkSide(Step first, bool backward)
{
	Side side;
	Vec dir = first.Vector();
	Step s = first;
	side.run = Length(dir);
	for (int i = 0; i < 12 && side.run <= ROUND_MERGE_RUN; i++) {
		Step n;
		if (!NextStep(s, n)) return side;
		Turn turn = TurnBetween(dir, n.Vector());
		if (turn == Turn::Straight) {
			side.run += Length(n.Vector());
			s = n;
			continue;
		}
		if (turn == Turn::Corner) {
			side.corner = true;
			int walked = TurnSign(dir, n.Vector());
			side.sign = backward ? -walked : walked;
			side.dir = backward ? n.Vector() * -1.0 : n.Vector();
			side.last = s;
			side.beyond = n;
		}
		return side;
	}
	return side;
}

/** How far an arc may reach along a side: up to the next corner, half the straight (the rest is that corner's); else all of it, not past where the round curve ends. */
double Avail(const Side &side)
{
	return side.corner ? side.run / 2 : side.run;
}

/** Which neighbouring corner a corner makes one arc with: -1 the one before, +1 the one after, 0 none -- the nearer of those turning the same way close enough. */
int Preference(const Side &back, const Side &fwd, int sign)
{
	bool b = back.corner && back.sign == sign && back.run <= ROUND_MERGE_RUN;
	bool f = fwd.corner && fwd.sign == sign && fwd.run <= ROUND_MERGE_RUN;
	if (b && f) return back.run < fwd.run ? -1 : 1;
	if (b) return -1;
	if (f) return 1;
	return 0;
}

/** An arc of the centre line, in the frame of a piece: the stretch of the path it replaces and the circle it lies on. */
struct Zone {
	double lo = 0, hi = 0; ///< the stretch of the path, along the piece from its first edge
	Vec c;                 ///< the arc's centre, world units
	double r = 0;          ///< and radius
	double a0 = 0;         ///< the angle of its start, radians
	double sweep = 0;      ///< how far it turns, signed
};

/** The arc tangent to a straight coming along a to a point q and one leaving it along b, reaching t along each, turning by phi. */
Zone MakeZone(Vec q, Vec a, Vec b, double t, double phi, double lo, double hi)
{
	a = Unit(a);
	b = Unit(b);
	double sigma = TurnSign(a, b);
	Zone z;
	z.lo = lo;
	z.hi = hi;
	z.r = t / std::tan(phi / 2);
	Vec p1 = q - a * t;
	z.c = p1 + Vec{-a.y, a.x} * (sigma * z.r);
	z.a0 = std::atan2(p1.y - z.c.y, p1.x - z.c.x);
	z.sweep = sigma * phi;
	return z;
}

/** The centre line of a piece of track, round where it is part of an arc. */
struct Curve {
	Vec v0;           ///< the midpoint of its first edge, world units
	Vec dir;          ///< its direction
	double len = 0;   ///< its length
	double lo = 0, hi = 0; ///< how far it is drawn: the piece, and on a switch past it over the straight way, as far as its arc goes there
	std::vector<Zone> zones;

	/** The point of the centre line a way along the piece, and its direction there. */
	void Eval(double s, Vec &q, Vec &tangent) const
	{
		for (const Zone &z : this->zones) {
			if (s < z.lo || s > z.hi) continue;
			double f = (s - z.lo) / (z.hi - z.lo);
			double ang = z.a0 + f * z.sweep;
			q = z.c + Vec{std::cos(ang), std::sin(ang)} * z.r;
			tangent = Vec{-std::sin(ang), std::cos(ang)} * (z.sweep > 0 ? 1.0 : -1.0);
			return;
		}
		q = this->v0 + this->dir * s;
		tangent = this->dir;
	}

	bool InZone(double s) const
	{
		for (const Zone &z : this->zones) {
			if (s >= z.lo && s <= z.hi) return true;
		}
		return false;
	}

	/**
	 * The nearest point of the centre line to a point.
	 * @param[out] s how far along the piece it is
	 * @param[out] d how far the point is from it, across, signed
	 * @return the distance
	 */
	double Nearest(Vec p, double &s, double &d) const
	{
		double best = 1e18;
		auto consider = [&](double cand) {
			Vec q, tg;
			this->Eval(cand, q, tg);
			double dist = Length(p - q);
			if (dist < best) {
				best = dist;
				s = cand;
				d = Cross(tg, p - q);
			}
		};
		/* The straight parts: the piece but its arcs. */
		std::vector<std::pair<double, double>> cut;
		for (const Zone &z : this->zones) cut.emplace_back(std::max(this->lo, z.lo), std::min(this->hi, z.hi));
		std::sort(cut.begin(), cut.end());
		double from = this->lo;
		auto straight = [&](double a, double b) {
			if (b <= a) return;
			consider(std::clamp(Dot(p - this->v0, this->dir), a, b));
		};
		for (const auto &[a, b] : cut) {
			straight(from, a);
			from = std::max(from, b);
		}
		straight(from, this->hi);
		/* The arcs. */
		for (const Zone &z : this->zones) {
			double lo = std::max(this->lo, z.lo), hi = std::min(this->hi, z.hi);
			if (hi <= lo) continue;
			double ang = std::atan2(p.y - z.c.y, p.x - z.c.x) - z.a0;
			while (ang > M_PI) ang -= 2 * M_PI;
			while (ang < -M_PI) ang += 2 * M_PI;
			double f = ang / z.sweep;
			consider(std::clamp(z.lo + f * (z.hi - z.lo), lo, hi));
		}
		return best;
	}
};

/** At a corner between two pieces walked forward: the tile past it goes straight on the way of the piece coming to it -- that way is the straight way of a switch. */
bool SwitchBehind(const Step &to, const Step &from)
{
	return GoesStraightOn(from.tile, from.from, to.Vector());
}

/** At a corner between two pieces walked forward: the tile before it goes straight on the way of the piece leaving it, walked back. */
bool SwitchAhead(const Step &to, const Step &from)
{
	return GoesStraightOn(to.tile, OtherEdge(to.track, to.from), from.Vector() * -1.0);
}

/** A side of a corner along the straight way of a switch: only its piece at the switch, and no corner past it. */
void KeepToPiece(Side &side, const Step &piece)
{
	side.run = std::min(side.run, Length(piece.Vector()));
	side.corner = false;
}

/**
 * The tangent of the one arc of two corners, a quarter turn: as far as the straights either side let it, and whether
 * the arc keeps near enough to the track -- to the middle of the straight between the corners and to the corners.
 * @param h how far the corners are from where the straights either side meet
 * @param more_back, more_fwd how far the arc may reach past the corners along the straights either side
 * @param[out] t the tangent
 */
bool MergedTangent(double h, double more_back, double more_fwd, double &t)
{
	t = std::max(h, std::min({std::max(ROUND_TANGENT_MAX, ROUND_MERGE_TANGENT * h), h + more_back, h + more_fwd}));
	/* Where the straights meet, the radius is t: the arc is (sqrt 2 - 1) t from that point, the straight between the
	 * corners h / sqrt 2; a corner is h from it along one straight, the arc's centre (t, t). */
	double middle = std::abs(h / std::sqrt(2.0) - (std::sqrt(2.0) - 1) * t);
	double corner = std::hypot(t - h, t) - t;
	return std::max(middle, corner) <= ROUND_MERGE_DEVIATION;
}

/** A corner of the path around a piece: where it is along the piece, and the pieces either side of it, walked forward. */
struct PathCorner {
	double s;  ///< along the piece from its first edge
	Vec pos;   ///< world units
	Step to;   ///< the piece coming to it
	Step from; ///< the piece leaving it
};

/**
 * The arc a corner is part of, in the frame of a piece, if any: one round the corner alone, or one with the next
 * or the previous corner (the plain curve of the game: two corners of 45 degrees with a short straight on the
 * diagonal between them, one quarter circle tangent to the straights either side, a tile from where they meet).
 * Everything is decided from the corner's own surroundings, so every piece it reaches gets the same arc.
 */
bool CornerZone(const PathCorner &pc, Zone &zone, bool *main_back = nullptr, bool *main_fwd = nullptr)
{
	Vec a = Unit(pc.to.Vector()), b = Unit(pc.from.Vector());
	int sign = TurnSign(a, b);
	Side back = WalkSide(pc.to.Reversed(), true);
	Side fwd = WalkSide(pc.from, false);
	/* A switch: the straight way through it stays straight, so an arc branching off it reaches no further along it
	 * than the piece at the switch, and makes no arc with a corner beyond that. */
	bool behind = SwitchBehind(pc.to, pc.from), ahead = SwitchAhead(pc.to, pc.from);
	if (main_back != nullptr) *main_back = behind;
	if (main_fwd != nullptr) *main_fwd = ahead;
	if (behind) KeepToPiece(back, pc.to);
	if (ahead) KeepToPiece(fwd, pc.from);
	int pref = Preference(back, fwd, sign);
	if (pref == 1) {
		/* The next corner: one arc with it if it would have this one too. */
		Side w_back;
		w_back.run = fwd.run;
		w_back.corner = true;
		w_back.sign = sign;
		Side w_fwd = WalkSide(fwd.beyond, false);
		if (SwitchAhead(fwd.last, fwd.beyond)) KeepToPiece(w_fwd, fwd.beyond);
		double d = fwd.run, h = d / std::sqrt(2.0), t;
		if (Preference(w_back, w_fwd, sign) == -1 && MergedTangent(h, Avail(back), Avail(w_fwd), t)) {
			Vec meet = pc.pos + a * h; // where the straights either side meet
			zone = MakeZone(meet, a, fwd.dir, t, 2 * ROUND_PHI, pc.s - (t - h), pc.s + d + (t - h));
			return true;
		}
	} else if (pref == -1) {
		Side u_fwd;
		u_fwd.run = back.run;
		u_fwd.corner = true;
		u_fwd.sign = sign;
		Side u_back = WalkSide(back.beyond, true);
		if (SwitchBehind(back.beyond.Reversed(), back.last.Reversed())) KeepToPiece(u_back, back.beyond);
		double d = back.run, h = d / std::sqrt(2.0), t;
		if (Preference(u_back, u_fwd, sign) == 1 && MergedTangent(h, Avail(u_back), Avail(fwd), t)) {
			Vec meet = pc.pos - b * h;
			zone = MakeZone(meet, back.dir, b, t, 2 * ROUND_PHI, pc.s - d - (t - h), pc.s + (t - h));
			return true;
		}
	}
	/* Alone: a turn of 45 degrees, reaching as far as the straights either side let it. */
	double t = std::min({ROUND_TANGENT_MAX, Avail(back), Avail(fwd)});
	if (t <= 0) return false;
	zone = MakeZone(pc.pos, a, b, t, ROUND_PHI, pc.s - t, pc.s + t);
	return true;
}

/** The centre line of a piece of a simple tile. */
Curve BuildCurve(TileIndex tile, Track track)
{
	Curve c;
	auto [e0, e1] = PieceEdges(track);
	Vec corner{static_cast<double>(TileX(tile) * TILE_SIZE), static_cast<double>(TileY(tile) * TILE_SIZE)};
	c.v0 = corner + EdgeMid(e0);
	Vec d = EdgeMid(e1) - EdgeMid(e0);
	c.len = Length(d);
	c.dir = Unit(d);
	c.lo = 0;
	c.hi = c.len;

	/* The path around the piece, walked forward, and its corners. An arc reaches at most a tile and a little
	 * past the corners of the widest curve it rounds as one, so this far either way is enough. */
	static const double REACH = 56;
	std::vector<PathCorner> corners;
	const Step self{tile, track, e0};
	{
		Step s = self;
		double at = c.len;
		for (int i = 0; i < 12 && at < c.len + REACH; i++) {
			Step n;
			if (!NextStep(s, n)) break;
			if (TurnBetween(s.Vector(), n.Vector()) == Turn::Corner) corners.push_back({at, s.End(), s, n});
			at += Length(n.Vector());
			s = n;
		}
	}
	{
		Step s = self.Reversed();
		double at = 0;
		for (int i = 0; i < 12 && at > -REACH; i++) {
			Step n;
			if (!NextStep(s, n)) break;
			/* Walked backward: the corner between n (coming to it, forward) and s (leaving it, forward). */
			Step to = n.Reversed(), from = s.Reversed();
			if (TurnBetween(to.Vector(), from.Vector()) == Turn::Corner) corners.push_back({at, s.End(), to, from});
			at -= Length(n.Vector());
			s = n;
		}
	}
	for (const PathCorner &pc : corners) {
		Zone z;
		bool main_back = false, main_fwd = false;
		if (!CornerZone(pc, z, &main_back, &main_fwd)) continue;
		if (z.hi <= 0 || z.lo >= c.len) continue; // does not reach this piece
		/* Where the piece branches off the straight way of a switch at its edge, it is drawn on over that way as far
		 * as its arc goes there: that piece is straight, not round. */
		if (main_back && pc.s <= 0.01) c.lo = std::min(c.lo, z.lo);
		if (main_fwd && pc.s >= c.len - 0.01) c.hi = std::max(c.hi, z.hi);
		bool known = false;
		for (const Zone &o : c.zones) {
			if (std::abs(o.lo - z.lo) < 0.01 && std::abs(o.hi - z.hi) < 0.01) known = true;
		}
		if (!known) c.zones.push_back(z);
	}
	return c;
}

/** The curves of the pieces this pass has asked for. */
std::unordered_map<uint64_t, Curve> _curves;

const Curve &CurveOf(TileIndex tile, Track track)
{
	uint64_t key = (static_cast<uint64_t>(tile.base()) << 3) | to_underlying(track);
	auto it = _curves.find(key);
	if (it != _curves.end()) return it->second;
	return _curves.emplace(key, BuildCurve(tile, track)).first->second;
}

/** The picture of the straight track (along x) of a rail type on its ground, and which of its pixels are track, at a zoom. */
struct Reference {
	int left = 0, top = 0;   ///< virtual coordinates of the top left pixel
	int w = 0, h = 0;
	int scale = 1;           ///< virtual units a pixel
	double band_lo = -1e9, band_hi = 1e9; ///< across the track, from its centre line: where its ballast is
	std::vector<uint32_t> track;
	std::vector<bool> on;    ///< the pixel is the track's, not its ground's

	/** The pixel at a world point of the reference tile (at the origin) and its index, or -1 outside. */
	int Index(double x, double y) const
	{
		double vx = (y - x) * 2 * ZOOM_BASE, vy = (y + x) * ZOOM_BASE;
		int px = static_cast<int>(std::floor((vx - this->left) / this->scale)), py = static_cast<int>(std::floor((vy - this->top) / this->scale));
		if (px < 0 || py < 0 || px >= this->w || py >= this->h) return -1;
		return py * this->w + px;
	}

	/** The track's pixel at a world point of the reference tile, or 0 where it is only ground. */
	uint32_t At(double x, double y) const
	{
		double across = y - TILE_SIZE / 2.0;
		if (across < this->band_lo || across > this->band_hi) return 0;
		int i = this->Index(x, y);
		if (i < 0 || !this->on[i]) return 0;
		return this->track[i];
	}
};

std::map<uint32_t, Reference> _references;

/** The ground of a tile of rail with no track: the sprite the game draws there. */
SpriteID PlainGround(TileIndex tile)
{
	switch (GetRailGroundType(tile)) {
		case RailGroundType::Barren: return SPR_FLAT_BARE_LAND;
		case RailGroundType::SnowOrDesert: return SPR_FLAT_SNOW_DESERT_TILE;
		default: return SPR_FLAT_GRASS_TILE;
	}
}

/**
 * The reference of a tile's rail type and ground. With an overlay (a set's rail type) the track is what the overlay
 * covers of the ground. The game's own rails are one picture with the grass; their track is told from the grass by
 * colour -- the grass of the rails is not the plain ground's grass pixel for pixel, but it is of the same colours --
 * and only within the band along the middle where the ballast is, so a speck of the grass's colours elsewhere is not
 * taken. The bare and the snowy rails take their pixels from their own picture and which are track from the grassy one.
 */
const Reference &ReferenceOf(TileIndex tile, ZoomLevel zoom)
{
	RailType rt = GetRailType(tile);
	RailGroundType rgt = GetRailGroundType(tile);
	uint variant = rgt == RailGroundType::Barren ? 1 : (rgt == RailGroundType::SnowOrDesert ? 2 : 0);
	uint32_t key = (static_cast<uint32_t>(rt) << 8) | (variant << 4) | to_underlying(zoom);
	auto it = _references.find(key);
	if (it != _references.end()) return it->second;

	Reference &ref = _references[key];
	int mask = ScaleByZoom(-1, zoom);
	ref.scale = ScaleByZoom(1, zoom);
	ref.left = (-40 * ZOOM_BASE) & mask;
	ref.top = (-24 * ZOOM_BASE) & mask;
	int width = ((40 * ZOOM_BASE - ref.left) + ~mask) & mask;
	int height = ((48 * ZOOM_BASE - ref.top) + ~mask) & mask;
	ref.w = UnScaleByZoom(width, zoom);
	ref.h = UnScaleByZoom(height, zoom);
	const size_t size = static_cast<size_t>(ref.w) * ref.h;

	auto draw = [&](SpriteID image, PaletteID pal, SpriteID over) {
		std::vector<uint32_t> into(size, 0);
		DrawPixelInfo dpi{
			.dst_ptr = into.data(),
			.left = ref.left,
			.top = ref.top,
			.width = width,
			.height = height,
			.pitch = ref.w,
			.zoom = zoom
		};
		AutoRestoreBackup dpi_backup(_cur_dpi, &dpi);
		AutoRestoreBackup anim_backup(_screen_disable_anim, true);
		DrawSpriteViewport(image, pal, 0, 0);
		if (over != 0) DrawSpriteViewport(over, PAL_NONE, 0, 0);
		return into;
	};

	const RailTypeInfo *rti = GetRailTypeInfo(rt);
	if (rti->UsesOverlay()) {
		bool no_combine = rti->flags.Test(RailTypeFlag::NoSpriteCombine);
		SpriteID ground = GetCustomRailSprite(rti, tile, no_combine ? RailSpriteType::GroundComplete : RailSpriteType::Ground);
		std::vector<uint32_t> plain = draw(PlainGround(tile), PAL_NONE, 0);
		ref.track = draw(PlainGround(tile), PAL_NONE, ground == 0 ? 0 : (no_combine ? ground : ground + RTO_X));
		ref.on.assign(size, false);
		for (size_t i = 0; i < size; i++) ref.on[i] = ref.track[i] != 0 && ref.track[i] != plain[i];
		return ref;
	}

	SpriteID image = rti->base_sprites.track_y + 1;
	std::vector<uint32_t> grassy = draw(image, PAL_NONE, 0);
	std::vector<uint32_t> grass = draw(SPR_FLAT_GRASS_TILE, PAL_NONE, 0);
	std::unordered_set<uint32_t> grass_colours;
	for (uint32_t c : grass) {
		if (c != 0) grass_colours.insert(c & 0xFFFFFF);
	}
	ref.on.assign(size, false);
	for (size_t i = 0; i < size; i++) ref.on[i] = grassy[i] != 0 && grass_colours.count(grassy[i] & 0xFFFFFF) == 0;

	/* The band: the lines along the track, from the middle out, on which the track's pixels are not few. */
	auto share = [&](double across) {
		int all = 0, track = 0;
		for (double x = 2; x <= TILE_SIZE - 2; x += 0.25) {
			int i = ref.Index(x, TILE_SIZE / 2.0 + across);
			if (i < 0 || grassy[i] == 0) continue;
			all++;
			if (ref.on[i]) track++;
		}
		return all == 0 ? 0.0 : static_cast<double>(track) / all;
	};
	const double step = 0.25, least = 0.1, half = TILE_SIZE / 2.0;
	ref.band_lo = 0;
	while (ref.band_lo > -half && share(ref.band_lo - step) >= least) ref.band_lo -= step;
	ref.band_hi = 0;
	while (ref.band_hi < half && share(ref.band_hi + step) >= least) ref.band_hi += step;
	ref.band_lo -= step / 2;
	ref.band_hi += step / 2;

	if (variant == 0) {
		ref.track = std::move(grassy);
	} else {
		ref.track = draw(variant == 2 ? image + rti->snow_offset : image, variant == 1 ? PALETTE_TO_BARE_LAND : PAL_NONE, 0);
	}
	return ref;
}

/** The pieces of the neighbouring tiles joining a piece across its edges (all of them at a switch). */
void JoiningPieces(TileIndex tile, Track track, std::vector<std::pair<TileIndex, Track>> &out)
{
	auto [a, b] = PieceEdges(track);
	for (DiagDirection e : {a, b}) {
		TileIndex n = Neighbour(tile, e);
		if (n == INVALID_TILE || !IsSimpleTile(n) || TilePixelHeight(n) != TilePixelHeight(tile)) continue;
		DiagDirection in = ReverseDiagDir(e);
		for (Track tr : GetTrackBits(n)) {
			auto [na, nb] = PieceEdges(tr);
			if (na == in || nb == in) out.emplace_back(n, tr);
		}
	}
}

/** Whether a piece of a neighbouring tile is drawn on over this tile: it branches off a switch at the edge between them. */
bool ReachesOver(TileIndex tile, TileIndex n, Track ntr)
{
	const Curve &c = CurveOf(n, ntr);
	if (c.lo >= 0 && c.hi <= c.len) return false;
	auto [e0, e1] = PieceEdges(ntr);
	return (c.lo < 0 && Neighbour(n, e0) == tile) || (c.hi > c.len && Neighbour(n, e1) == tile);
}

} // namespace

/** Whether the track of a tile is drawn round: a simple tile with a piece an arc reaches, or one a round piece branching off a switch goes on over. */
bool IsRoundTrackTile(TileIndex tile)
{
	if (!RoundTrackEnabled() || !IsSimpleTile(tile)) return false;
	for (Track tr : GetTrackBits(tile)) {
		if (!CurveOf(tile, tr).zones.empty()) return true;
	}
	std::vector<std::pair<TileIndex, Track>> joining;
	for (Track tr : GetTrackBits(tile)) JoiningPieces(tile, tr, joining);
	for (const auto &[n, ntr] : joining) {
		if (ReachesOver(tile, n, ntr)) return true;
	}
	return false;
}

/** Whether a round tile is drawn without its catenary: all its pieces are round; one keeping a straight piece, at a switch, keeps it. */
bool RoundTrackHidesCatenary(TileIndex tile)
{
	if (!IsRoundTrackTile(tile)) return false;
	for (Track tr : GetTrackBits(tile)) {
		if (CurveOf(tile, tr).zones.empty()) return false;
	}
	return true;
}

/** The track of a tile is drawn after the ground, as a strip along its curve (DrawRoundTrackStrips()). */
void RegisterRoundTrackTile(TileIndex tile)
{
	_round_track_tiles.push_back(tile);
}

/** The ground of a tile whose track is drawn as a strip: the ground the game draws under no track. */
void DrawRoundTrackGround(TileIndex tile)
{
	DrawGroundSprite(PlainGround(tile), PAL_NONE);
}

/**
 * Draw the track of the tiles collected this pass along their curves, into a block drawn facing north (the screen,
 * or the turned view's picture of the ground): each pixel of such a tile near the centre line of one of its pieces,
 * or of a piece joining it across an edge (an arc crosses the edge off its midpoint), takes the pixel of the
 * straight track of its rail type the same way along and across that piece.
 */
void DrawRoundTrackStrips(const DrawPixelInfo &dpi)
{
	if (_round_track_tiles.empty() || !RoundTrackEnabled()) return;
	const int scale = ScaleByZoom(1, dpi.zoom);
	const int pw = UnScaleByZoom(dpi.width, dpi.zoom), ph = UnScaleByZoom(dpi.height, dpi.zoom);
	uint32_t *dst = static_cast<uint32_t *>(dpi.dst_ptr);

	std::sort(_round_track_tiles.begin(), _round_track_tiles.end());
	_round_track_tiles.erase(std::unique(_round_track_tiles.begin(), _round_track_tiles.end()), _round_track_tiles.end());
	for (TileIndex tile : _round_track_tiles) {
		if (!IsRoundTrackTile(tile)) continue;
		const int x0 = TileX(tile) * TILE_SIZE, y0 = TileY(tile) * TILE_SIZE, z = TilePixelHeight(tile);

		/* The pieces a pixel of the tile may be on: its own, and those joining it across an edge. */
		std::vector<std::pair<TileIndex, Track>> joining;
		for (Track tr : GetTrackBits(tile)) {
			joining.emplace_back(tile, tr);
			JoiningPieces(tile, tr, joining);
		}
		std::sort(joining.begin(), joining.end());
		joining.erase(std::unique(joining.begin(), joining.end()), joining.end());
		std::vector<const Curve *> pieces;
		for (const auto &[pt, ptr] : joining) pieces.push_back(&CurveOf(pt, ptr));
		const Reference &ref = ReferenceOf(tile, dpi.zoom);

		/* The tile's pixels in the block. */
		Point n = RemapCoords(x0, y0, z), w = RemapCoords(x0 + TILE_SIZE, y0, z), e = RemapCoords(x0, y0 + TILE_SIZE, z), s = RemapCoords(x0 + TILE_SIZE, y0 + TILE_SIZE, z);
		int vx0 = w.x, vx1 = e.x, vy0 = n.y, vy1 = s.y;
		const int pad = 4 * ZOOM_BASE; // the edge's width on the screen and some
		int px0 = std::max(0, (vx0 - pad - dpi.left) / scale - 1), px1 = std::min(pw - 1, (vx1 + pad - dpi.left) / scale + 1);
		int py0 = std::max(0, (vy0 - pad - dpi.top) / scale - 1), py1 = std::min(ph - 1, (vy1 + pad - dpi.top) / scale + 1);
		for (int py = py0; py <= py1; py++) {
			double vy = dpi.top + (py + 0.5) * scale;
			for (int px = px0; px <= px1; px++) {
				double vx = dpi.left + (px + 0.5) * scale;
				double X = vx / (2.0 * ZOOM_BASE), Y = vy / ZOOM_BASE + z;
				Vec p{(Y - X) / 2, (Y + X) / 2};
				/* The tile and a little past its edges: the ground of the tile drawn after its neighbour covers the
				 * neighbour's pixels on the edge, and the track goes on there. */
				if (p.x < x0 - ROUND_EDGE || p.x >= x0 + TILE_SIZE + ROUND_EDGE || p.y < y0 - ROUND_EDGE || p.y >= y0 + TILE_SIZE + ROUND_EDGE) continue;
				/* Of the pieces the pixel is beside (not past their ends), the nearest whose track is there: where two
				 * overlap, at a switch, both are drawn. Along a piece the picture of the straight track goes on tile
				 * after tile, also where a piece is drawn on past its edge. */
				double best = 1e18;
				uint32_t colour = 0;
				for (const Curve *c : pieces) {
					double cs = 0, cd = 0;
					double dist = c->Nearest(p, cs, cd);
					if (dist >= best || std::abs(cd) > ROUND_HALF_WIDTH || dist > std::abs(cd) + ROUND_BESIDE) continue;
					double along = std::fmod(cs, c->len);
					if (along < 0) along += c->len;
					uint32_t found = ref.At(along / c->len * TILE_SIZE, TILE_SIZE / 2.0 + cd);
					if (found == 0) continue;
					best = dist;
					colour = found;
				}
				if (colour != 0) dst[static_cast<size_t>(py) * dpi.pitch + px] = colour;
			}
		}
	}
}

/** The piece of track a train's piece is on, or Track::Invalid. */
static Track TrainPiece(const Vehicle *v)
{
	if (v == nullptr || v->type != VehicleType::Train) return Track::Invalid;
	const Train *t = Train::From(v);
	/* Only plain rail has its pieces in the map (GetTrackBits()): a piece of the train still in a station, on a
	 * level crossing or on a bridge's ramp is none (the colleague's crash report, a train leaving a station). */
	if (t->track.Test(Track::Wormhole) || t->track.Test(Track::Depot) || !IsPlainRailTile(v->tile)) return Track::Invalid;
	for (Track tr : t->track) {
		if (GetTrackBits(v->tile).Test(tr)) return tr;
		break;
	}
	return Track::Invalid;
}

/**
 * Where a train's piece on a round curve is drawn: on the arc, turned along it. Not where its piece is straight:
 * there it is drawn as always -- but on the straight way of a switch next to a round piece branching off it, while
 * the train's piece before or after it is on that round piece, it is on that round piece's arc.
 */
bool GetRoundTrackPose(const Vehicle *v, RoundTrackPose &pose)
{
	if (!RoundTrackDrawing() || v->type != VehicleType::Train || v->direction == Direction::Invalid) return false;
	if (!IsSimpleTile(v->tile)) return false;
	Track track = TrainPiece(v);
	if (track == Track::Invalid) return false;
	const Vec at{static_cast<double>(v->x_pos), static_cast<double>(v->y_pos)};
	const Curve *c = &CurveOf(v->tile, track);
	double s = std::clamp(Dot(at - c->v0, c->dir), 0.0, c->len);
	if (!c->InZone(s)) {
		const Curve &own = *c;
		c = nullptr;
		for (const Vehicle *u : {v->Previous(), v->Next()}) {
			Track utr = TrainPiece(u);
			if (utr == Track::Invalid || u->tile == v->tile || !IsSimpleTile(u->tile) || !ReachesOver(v->tile, u->tile, utr)) continue;
			const Curve &uc = CurveOf(u->tile, utr);
			/* How far along the way it is: past the edge between the tiles by as much as it is along its piece. */
			bool before = Neighbour(u->tile, PieceEdges(utr).first) == v->tile;
			Vec edge = before ? uc.v0 : uc.v0 + uc.dir * uc.len;
			double past = std::abs(Dot(at - edge, own.dir));
			double us = std::clamp(before ? -past : uc.len + past, uc.lo, uc.hi);
			if (!uc.InZone(us)) continue;
			c = &uc;
			s = us;
			break;
		}
		if (c == nullptr) return false;
	}
	Vec q, tg;
	c->Eval(s, q, tg);
	static const int8_t dir_dx[8] = {-1, -1, -1, 0, 1, 1, 1, 0};
	static const int8_t dir_dy[8] = {-1, 0, 1, 1, 1, 0, -1, -1};
	int di = to_underlying(v->direction) & 7;
	if (Dot(tg, Vec{static_cast<double>(dir_dx[di]), static_cast<double>(dir_dy[di])}) < 0) tg = tg * -1.0;
	pose.x = q.x;
	pose.y = q.y;
	pose.azimuth = static_cast<float>(std::atan2(tg.y, tg.x) * 180.0 / M_PI - 135.0);
	return true;
}

RoundTrackPass::RoundTrackPass(bool active) : was(_round_track_drawing)
{
	_round_track_drawing = active;
	_round_track_tiles.clear();
	_curves.clear();
}

RoundTrackPass::~RoundTrackPass()
{
	_round_track_drawing = this->was;
	_round_track_tiles.clear();
	_curves.clear();
}
