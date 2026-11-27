/*
** d_logiccensus.cpp
**
** [Karto] Test-only logic counters (d_logiccensus.h); tics never overlap, so plain counters suffice.
*/

#include <cstring>

#include "c_dispatch.h"
#include "printf.h"
#include "actor.h"
#include "d_logiccensus.h"

bool LogicCensusArmed = false;
FLogicCensus LogicCensus;

namespace
{
	AActor* LastFailedActor = nullptr;
	int LastFailedDir = -1;
	double LastFailedX = 0, LastFailedY = 0;
	uint64_t LastFailedTic = ~0ull;
	uint64_t ChaseStartTries = 0;
	bool ChaseFirst = false, InChase = false;
}

void LogicCensusMoveBegin(AActor* actor)
{
	++LogicCensus.MoveTries;
	if (InChase && ChaseFirst) ++LogicCensus.ChaseMoves;
	if (actor == LastFailedActor && actor->movedir == LastFailedDir && actor->X() == LastFailedX && actor->Y() == LastFailedY &&
		LogicCensus.Tics == LastFailedTic)
		++LogicCensus.MoveDuplicates;
}

void LogicCensusMoveEnd(AActor* actor, bool moved)
{
	if (InChase && ChaseFirst)
	{
		if (!moved) ++LogicCensus.ChaseFirstFailed;
		ChaseFirst = false;
	}
	if (moved) return;
	++LogicCensus.MoveFailed;
	LastFailedActor = actor;
	LastFailedDir = actor->movedir;
	LastFailedX = actor->X();
	LastFailedY = actor->Y();
	LastFailedTic = LogicCensus.Tics;
}

void LogicCensusChaseBegin()
{
	++LogicCensus.Chases;
	ChaseStartTries = LogicCensus.MoveTries;
	ChaseFirst = true;
	InChase = true;
}

void LogicCensusChaseEnd()
{
	const uint64_t tries = LogicCensus.MoveTries - ChaseStartTries;
	const int bucket = tries == 0 ? 0 : tries == 1 ? 1 : tries == 2 ? 2 : tries <= 5 ? 3 : tries <= 9 ? 4 : 5;
	++LogicCensus.ChaseTriesHist[bucket];
	InChase = false;
	ChaseFirst = false;
}

CCMD(logic_census)
{
	auto& c = LogicCensus;
	if (argv.argc() == 2 && stricmp(argv[1], "on") == 0)
	{
		memset(&c, 0, sizeof(c));
		LastFailedActor = nullptr;
		LogicCensusArmed = true;
		Printf("LOGICCENSUS armed\n");
	}
	else if (argv.argc() == 2 && stricmp(argv[1], "off") == 0)
	{
		LogicCensusArmed = false;
		Printf("LOGICCENSUS disarmed\n");
	}
	else if (argv.argc() == 2 && stricmp(argv[1], "print") == 0)
	{
		const double t = c.Tics ? double(c.Tics) : 1.0;
		Printf("LOGICCENSUS tics=%llu particles_per_tic=%.1f unmoved=%.4f same_subsector=%.4f\n", (unsigned long long)c.Tics,
			c.Particles / t, c.Particles ? double(c.ParticlesUnmoved) / c.Particles : 0.0,
			c.Particles ? double(c.ParticlesSameSubsector) / c.Particles : 0.0);
		Printf("LOGICCENSUSCHASE chases_per_tic=%.1f move_tries_per_tic=%.1f tries_per_chase=%.2f first_move_failed=%.4f failed=%.4f duplicates_per_tic=%.1f\n",
			c.Chases / t, c.MoveTries / t, c.Chases ? double(c.MoveTries) / c.Chases : 0.0,
			c.ChaseMoves ? double(c.ChaseFirstFailed) / c.ChaseMoves : 0.0, c.MoveTries ? double(c.MoveFailed) / c.MoveTries : 0.0,
			c.MoveDuplicates / t);
		Printf("LOGICCENSUSHIST tries0=%llu tries1=%llu tries2=%llu tries3_5=%llu tries6_9=%llu tries10plus=%llu\n",
			(unsigned long long)c.ChaseTriesHist[0], (unsigned long long)c.ChaseTriesHist[1], (unsigned long long)c.ChaseTriesHist[2],
			(unsigned long long)c.ChaseTriesHist[3], (unsigned long long)c.ChaseTriesHist[4], (unsigned long long)c.ChaseTriesHist[5]);
		Printf("LOGICCENSUSCALLS checkposition_per_tic=%.1f testmobjz_per_tic=%.1f callaction_per_tic=%.1f\n",
			c.CheckPositions / t, c.TestMobjZ / t, c.CallActions / t);
		const double w = c.CheckPosWalks ? double(c.CheckPosWalks) : 1.0;
		Printf("LOGICCENSUSWALK blocks_per_tic=%.1f nodes_per_block=%.2f span_share=%.4f returns_per_tic=%.1f nodes_per_tic=%.1f\n",
			c.BlockStarts / t, c.BlockStarts ? double(c.BlockNodeVisits) / c.BlockStarts : 0.0,
			c.BlockNodeVisits ? double(c.BlockSpanVisits) / c.BlockNodeVisits : 0.0, c.BlockReturns / t, c.BlockNodeVisits / t);
		Printf("LOGICCENSUSCHECKPOS walks_per_tic=%.1f nodes_per_walk=%.2f candidates_per_walk=%.2f survivors_per_walk=%.3f\n",
			c.CheckPosWalks / t, c.CheckPosNodeVisits / w, c.CheckPosCandidates / w, c.CheckPosSurvivors / w);
		Printf("LOGICCENSUSLINKS linked_per_tic=%.1f unlinked_per_tic=%.1f node_updates_per_tic=%.1f\n",
			c.NodesLinked / t, c.NodesUnlinked / t, c.NodeUpdates / t);
	}
	else Printf("Usage: logic_census on|off|print\n");
}
