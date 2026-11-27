/*
** p_islands.cpp
**
** [Karto] Islands: groups of subsectors reachable without crossing a sight-blocking wall (after Helion's IslandClassifier).
** Actors on different islands never see each other, so P_CheckSight answers them without a traversal.
**
**---------------------------------------------------------------------------
**
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#include "doomdef.h"
#include "g_levellocals.h"
#include "p_blockmap.h"
#include "c_dispatch.h"
#include "c_cvars.h"
#include "printf.h"
#include "doomstat.h"
#include "actor.h"
#include "p_lnspec.h"
#include "vm.h"
#include <algorithm>

// [Karto] Opt-in because it changes gameplay. Read at map load only; netgames and demos always keep the original AI.
CVAR(Bool, p_monstercloset, false, CVAR_ARCHIVE)

static int FindIsland(TArray<int>& parent, int i)
{
	while (parent[i] != i)
	{
		parent[i] = parent[parent[i]];
		i = parent[i];
	}
	return i;
}

static void JoinIslands(TArray<int>& parent, int a, int b)
{
	a = FindIsland(parent, a);
	b = FindIsland(parent, b);
	if (a != b) parent[std::max(a, b)] = std::min(a, b);
}

//==========================================================================
//
// [Karto] Sealed regions (p_sight_sealed): subsectors joined across every boundary except a two-sided line closed right
// now. Only the game tic uses it (the renderer calls P_CheckSight from another thread).
//
//==========================================================================

CVAR(Int, p_sight_sealed, 0, 0)

thread_local bool P_InWorldTick = false;

namespace
{
	FLevelLocals* SealedLevel = nullptr;
	int SealedBuiltTime = 0;
	bool SealedPortals = false;
	TArray<uint8_t> SealedInBlockmap;
	TArray<int> SealedRegion;			// per subsector
	TArray<uint32_t> SealedCutStart;	// per region, CSR into SealedCuts (line indices)
	TArray<uint32_t> SealedCuts;
}

static bool SealedPlanesFlat(const sector_t* s)
{
	return !s->floorplane.isSlope() && !s->ceilingplane.isSlope();
}

static bool SealedCuttable(const line_t* line)
{
	return line->backsector != nullptr && line->frontsector != line->backsector && !(line->flags & ML_PORTALCONNECT) &&
		SealedInBlockmap[line->Index()] && SealedPlanesFlat(line->frontsector) && SealedPlanesFlat(line->backsector);
}

static bool SealedClosed(const line_t* line)
{
	const double x = line->v1->fX(), y = line->v1->fY();
	const double top = std::min(line->frontsector->ceilingplane.ZatPoint(x, y), line->backsector->ceilingplane.ZatPoint(x, y));
	const double bottom = std::max(line->frontsector->floorplane.ZatPoint(x, y), line->backsector->floorplane.ZatPoint(x, y));
	return !(bottom < top);
}

static void BuildSealedRegions(FLevelLocals* level)
{
	auto& subsectors = level->subsectors;
	const unsigned count = subsectors.Size();
	if (SealedLevel != level)
	{
		SealedLevel = level;
		SealedPortals = level->linePortals.Size() != 0;
		for (auto& sector : level->sectors)
		{
			if ((sector.planes[sector_t::floor].Flags & PLANEF_LINKED) || (sector.planes[sector_t::ceiling].Flags & PLANEF_LINKED))
				SealedPortals = true;
		}
		SealedInBlockmap.Resize(level->lines.Size());
		memset(SealedInBlockmap.Data(), 0, SealedInBlockmap.Size());
		auto& bmap = level->blockmap;
		if (bmap.blockmaplump != nullptr)
		{
			for (int y = 0; y < bmap.bmapheight; y++)
				for (int x = 0; x < bmap.bmapwidth; x++)
					for (int* list = bmap.GetLines(x, y); *list != -1; list++)
						if ((unsigned)*list < SealedInBlockmap.Size()) SealedInBlockmap[*list] = 1;
		}
	}
	SealedBuiltTime = level->maptime;

	TArray<int> parent(count, true);
	for (unsigned i = 0; i < count; i++) parent[i] = i;
	struct FCut { int a, b; uint32_t line; };
	TArray<FCut> cuts;
	for (unsigned i = 0; i < count; i++)
	{
		auto& ss = subsectors[i];
		for (uint32_t j = 0; j < ss.numlines; j++)
		{
			seg_t* seg = ss.firstline + j;
			line_t* line = seg->linedef;
			if (seg->PartnerSeg != nullptr && seg->PartnerSeg->Subsector != nullptr)
			{
				const int other = seg->PartnerSeg->Subsector->Index();
				if (line != nullptr && SealedCuttable(line) && SealedClosed(line))
				{
					cuts.Push({ int(i), other, uint32_t(line->Index()) });
					continue;
				}
				JoinIslands(parent, i, other);
				continue;
			}
			bool solid = line != nullptr && line->backsector == nullptr && SealedInBlockmap[line->Index()] &&
				!(line->sidedef[0] != nullptr && (line->sidedef[0]->Flags & WALLF_POLYOBJ));
			if (solid) continue;
			DVector2 a = seg->v1->fPos(), b = seg->v2->fPos();
			DVector2 dir = b - a;
			double len = dir.Length();
			if (len <= 0) continue;
			DVector2 mid = (a + b) * 0.5;
			DVector2 normal(dir.Y / len, -dir.X / len);
			for (double side : { -1. / 256, 1. / 256 })
			{
				auto other = level->PointInRenderSubsector(mid + normal * side);
				if (other != nullptr) JoinIslands(parent, i, other->Index());
			}
		}
	}

	SealedRegion.Resize(count);
	TArray<int> regionIndex(count, true);
	for (unsigned i = 0; i < count; i++) regionIndex[i] = -1;
	int regions = 0;
	for (unsigned i = 0; i < count; i++)
	{
		int root = FindIsland(parent, i);
		if (regionIndex[root] < 0) regionIndex[root] = regions++;
		SealedRegion[i] = regionIndex[root];
	}
	TArray<std::pair<int, uint32_t>> pairs;
	for (auto& c : cuts)
	{
		const int ra = SealedRegion[c.a], rb = SealedRegion[c.b];
		if (ra == rb) continue;
		pairs.Push({ ra, c.line });
		pairs.Push({ rb, c.line });
	}
	std::sort(pairs.begin(), pairs.end());
	SealedCutStart.Resize(regions + 1);
	SealedCuts.Clear();
	unsigned at = 0;
	for (int r = 0; r < regions; r++)
	{
		SealedCutStart[r] = SealedCuts.Size();
		for (; at < pairs.Size() && pairs[at].first == r; at++)
			if (SealedCuts.Size() == SealedCutStart[r] || SealedCuts.Last() != pairs[at].second) SealedCuts.Push(pairs[at].second);
	}
	SealedCutStart[regions] = SealedCuts.Size();
}

bool P_SightSealed(FLevelLocals* level, const subsector_t* a, const subsector_t* b)
{
	if (!P_InWorldTick) return false;
	if (SealedLevel != level || SealedRegion.Size() != level->subsectors.Size() || level->maptime - SealedBuiltTime >= TICRATE ||
		level->maptime < SealedBuiltTime)
		BuildSealedRegions(level);
	if (SealedPortals) return false;
	for (int pass = 0; pass < 2; pass++)
	{
		const int ra = SealedRegion[a->Index()];
		if (ra == SealedRegion[b->Index()]) return false;
		bool sealed = true;
		for (uint32_t i = SealedCutStart[ra]; i < SealedCutStart[ra + 1] && sealed; i++)
			sealed = SealedClosed(&level->lines[SealedCuts[i]]);
		if (sealed)
		{
			return true;
		}
		BuildSealedRegions(level);
	}
	return false;
}

void P_ClassifyIslands(FLevelLocals* level)
{
	SealedLevel = nullptr;
	SealedRegion.Clear();
	auto& subsectors = level->subsectors;
	const unsigned count = subsectors.Size();
	level->IslandCount = 0;
	level->SightIslands = false;
	for (auto& ss : subsectors) ss.island = 0;
	if (count == 0) return;

	// [Karto] Sight passes linked portals; line portals are static after load, so one test holds for the whole level.
	bool portals = level->linePortals.Size() != 0;
	for (auto& sector : level->sectors)
	{
		if ((sector.planes[sector_t::floor].Flags & PLANEF_LINKED) || (sector.planes[sector_t::ceiling].Flags & PLANEF_LINKED))
			portals = true;
	}

	// [Karto] A one-sided line a custom blockmap leaves out is see-through in practice, so it must join its sides.
	TArray<uint8_t> inBlockmap(level->lines.Size(), true);
	memset(inBlockmap.Data(), 0, inBlockmap.Size());
	auto& bmap = level->blockmap;
	if (bmap.blockmaplump != nullptr)
	{
		for (int y = 0; y < bmap.bmapheight; y++)
		{
			for (int x = 0; x < bmap.bmapwidth; x++)
			{
				for (int* list = bmap.GetLines(x, y); *list != -1; list++)
				{
					if ((unsigned)*list < inBlockmap.Size()) inBlockmap[*list] = 1;
				}
			}
		}
	}

	TArray<int> parent(count, true);
	for (unsigned i = 0; i < count; i++) parent[i] = i;

	for (unsigned i = 0; i < count; i++)
	{
		auto& ss = subsectors[i];
		for (uint32_t j = 0; j < ss.numlines; j++)
		{
			seg_t* seg = ss.firstline + j;
			if (seg->PartnerSeg != nullptr && seg->PartnerSeg->Subsector != nullptr)
			{
				JoinIslands(parent, i, seg->PartnerSeg->Subsector->Index());
				continue;
			}
			line_t* line = seg->linedef;
			bool solid = line != nullptr && line->backsector == nullptr && inBlockmap[line->Index()] &&
				!(line->sidedef[0] != nullptr && (line->sidedef[0]->Flags & WALLF_POLYOBJ));
			if (solid) continue;

			// [Karto] No partner info (broken GL nodes, moved polyobject seat): find the neighbour by point; a wrong subsector only merges islands.
			DVector2 a = seg->v1->fPos(), b = seg->v2->fPos();
			DVector2 dir = b - a;
			double len = dir.Length();
			if (len <= 0) continue;
			DVector2 mid = (a + b) * 0.5;
			DVector2 normal(dir.Y / len, -dir.X / len);
			for (double side : { -1. / 256, 1. / 256 })
			{
				auto other = level->PointInRenderSubsector(mid + normal * side);
				if (other != nullptr) JoinIslands(parent, i, other->Index());
			}
		}
	}

	TArray<int> islandIndex(count, true);
	for (unsigned i = 0; i < count; i++) islandIndex[i] = -1;
	int islands = 0;
	for (unsigned i = 0; i < count; i++)
	{
		int root = FindIsland(parent, i);
		if (islandIndex[root] < 0) islandIndex[root] = islands++;
		subsectors[i].island = islandIndex[root];
	}
	level->IslandCount = islands;
	level->SightIslands = !portals && islands > 1;
}

static bool ClosetLineSpecial(int special)
{
	switch (special)
	{
	case 0:
	case Teleport:
	case Teleport_NoFog:
	case Teleport_NoStop:
	case Teleport_Line:
	case TeleportOther:
	case TeleportGroup:
	case TeleportInSector:
	case Scroll_Floor:
	case Scroll_Ceiling:
		return true;
	default:
		return false;
	}
}

// [Karto] The spawn loop must do nothing but A_Look, so replacing it skips no other behaviour.
static bool LooksOnlyInSpawn(AActor* actor)
{
	static VMFunction* lookFunc = nullptr;
	if (lookFunc == nullptr) PClass::FindFunction(&lookFunc, NAME_Actor, FName("A_Look"));
	FState* spawn = actor->SpawnState;
	if (spawn == nullptr || lookFunc == nullptr) return false;
	FState* state = spawn;
	bool current = false;
	for (int i = 0; i < 20; i++)
	{
		if (state->ActionFunc != nullptr && state->ActionFunc != lookFunc) return false;
		if (state->GetTics() < 0) return false;
		current |= state == actor->state;
		state = state->GetNextState();
		if (state == nullptr) return false;
		if (state == spawn) return current;
	}
	return false;
}

void P_ClassifyClosets(FLevelLocals* level)
{
	level->ClosetIslands.Clear();
	if (level->IslandCount <= 1 || level->IsReentering()) return;

	TArray<uint8_t>& closet = level->ClosetIslands;
	closet.Resize(level->IslandCount);
	memset(closet.Data(), 1, closet.Size());
	TArray<uint8_t> hasMonster(level->IslandCount, true);
	memset(hasMonster.Data(), 0, hasMonster.Size());
	for (auto& ss : level->subsectors)
	{
		for (uint32_t j = 0; j < ss.numlines; j++)
		{
			line_t* line = ss.firstline[j].linedef;
			if (line != nullptr && !ClosetLineSpecial(line->special)) closet[ss.island] = 0;
		}
	}
	auto it = level->GetThinkerIterator<AActor>();
	for (AActor* actor; (actor = it.Next()) != nullptr;)
	{
		if (actor->subsector == nullptr) continue;
		int island = actor->subsector->island;
		if (actor->flags & MF_COUNTKILL) hasMonster[island] = 1;
		else closet[island] = 0;
	}
	auto noCloset = [&](const FPlayerStart& start)
	{
		if (auto ss = level->PointInRenderSubsector(start.pos.XY())) closet[ss->island] = 0;
	};
	for (auto& start : level->AllPlayerStarts) noCloset(start);
	for (auto& start : level->deathmatchstarts) noCloset(start);
	for (unsigned i = 0; i < closet.Size(); i++) closet[i] &= hasMonster[i];

	if (!p_monstercloset || netgame || demoplayback || demorecording) return;
	unsigned stagger = 0;
	it = level->GetThinkerIterator<AActor>();
	for (AActor* actor; (actor = it.Next()) != nullptr;)
	{
		if (actor->subsector == nullptr || !closet[actor->subsector->island]) continue;
		if ((actor->flags & MF_FRIENDLY) || !LooksOnlyInSpawn(actor)) continue;
		actor->ClosetFlags = CLOSET_MONSTER;
		actor->tics = 1 + (stagger++ & 15);
	}
}

void P_LeaveCloset(AActor* actor)
{
	bool chasing = actor->ClosetFlags & CLOSET_CHASE;
	actor->ClosetFlags = 0;
	if (chasing && actor->SeeState != nullptr) actor->SetState(actor->SeeState);
}

void P_PlayerEnteredIsland(AActor* player)
{
	auto level = player->Level;
	if (player->subsector == nullptr || (unsigned)player->subsector->island >= level->ClosetIslands.Size()) return;
	int island = player->subsector->island;
	if (!level->ClosetIslands[island]) return;
	level->ClosetIslands[island] = 0;
	auto it = level->GetThinkerIterator<AActor>();
	for (AActor* actor; (actor = it.Next()) != nullptr;)
	{
		if (actor->ClosetFlags && actor->subsector && actor->subsector->island == island) P_LeaveCloset(actor);
	}
}

CCMD(islands)
{
	if (primaryLevel == nullptr) return;
	auto level = primaryLevel;
	TArray<int> sizes(level->IslandCount, true);
	for (auto& s : sizes) s = 0;
	for (auto& ss : level->subsectors) if (ss.island < (int)sizes.Size()) sizes[ss.island]++;
	int single = 0;
	for (auto s : sizes) if (s == 1) single++;
	Printf("%d islands over %u subsectors (%d of one subsector), sight early-out %s\n", level->IslandCount,
		level->subsectors.Size(), single, level->SightIslands ? "usable" : "off for this map");
	int closets = 0, monsters = 0, chasing = 0, inside = 0;
	for (auto c : level->ClosetIslands) closets += c;
	auto it = level->GetThinkerIterator<AActor>();
	for (AActor* actor; (actor = it.Next()) != nullptr;)
	{
		if (actor->ClosetFlags) monsters++;
		if (actor->ClosetFlags & CLOSET_CHASE) chasing++;
		if ((actor->flags & MF_COUNTKILL) && actor->health > 0 && actor->subsector &&
			(unsigned)actor->subsector->island < level->ClosetIslands.Size() && level->ClosetIslands[actor->subsector->island]) inside++;
	}
	Printf("%d monster closets, %d closet monsters (%d walking), %d live monsters still on closet islands\n", closets,
		monsters, chasing, inside);
}
