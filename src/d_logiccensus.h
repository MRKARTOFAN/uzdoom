/*
** d_logiccensus.h
**
** [Karto] Test-only counters that size logic-optimization candidates before any is built.
**
**   logic_census on | off | print
*/

#pragma once

#include <cstdint>

class AActor;

extern bool LogicCensusArmed;

struct FLogicCensus
{
	uint64_t Tics, Particles, ParticlesUnmoved, ParticlesSameSubsector;
	uint64_t Chases, ChaseMoves, ChaseFirstFailed, MoveTries, MoveFailed, MoveDuplicates;
	uint64_t ChaseTriesHist[6];	// 0, 1, 2, 3-5, 6-9, 10+
	uint64_t CheckPositions, TestMobjZ, CallActions;
	uint64_t BlockStarts, BlockNodeVisits, BlockSpanVisits, BlockReturns;
	uint64_t CheckPosWalks, CheckPosNodeVisits, CheckPosCandidates, CheckPosSurvivors;
	uint64_t NodesLinked, NodesUnlinked, NodeUpdates;
};
extern FLogicCensus LogicCensus;

void LogicCensusMoveBegin(AActor* actor);
void LogicCensusMoveEnd(AActor* actor, bool moved);
void LogicCensusChaseBegin();
void LogicCensusChaseEnd();

struct FLogicChaseScope
{
	bool Active;
	FLogicChaseScope() : Active(LogicCensusArmed) { if (Active) LogicCensusChaseBegin(); }
	~FLogicChaseScope() { if (Active) LogicCensusChaseEnd(); }
};
