/*
** dthinker.cpp
**
** Implements the base class for almost anything in a level that might think
**
**---------------------------------------------------------------------------
**
** Copyright 1998-2016 Marisa Heit
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Code written prior to 2026 is also licensed under:
**
** SPDX-License-Identifier: BSD-3-Clause
**
**---------------------------------------------------------------------------
**
*/

#include <algorithm>
#include <vector>
#include "dthinker.h"
#include "printf.h"
#include "stats.h"
#include "p_local.h"
#include "serializer_doom.h"
#include "d_player.h"
#include "vm.h"
#include "c_cvars.h"
#include "c_dispatch.h"
#include "v_text.h"
#include "g_levellocals.h"
#include "a_dynlight.h"
#include "v_video.h"
#include "g_cvars.h"
#include "d_main.h"
#include "r_utility.h"

#include "p_visualthinker.h"
#include "d_framestages.h"

static int ThinkCount, ClientSideThinkCount;
CVAR(Int, p_recreate_list, 1, 0)
CVAR(Int, p_recreate_replay, 1, 0)
static uint64_t RecreateReplayPasses = 0, RecreateReplayMissed = 0;
// [Karto] True only while RunThinkers ticks play thinkers, so client-side actors (walked separately) are never entered.
static bool CollectRecreate;
static cycle_t ThinkCycles, ClientSideThinkCycles;
static double ThinkWindowMS = 0, ThinkWindowMaxMS = 0;
static int ThinkWindowTics = 0;
extern cycle_t BotSupportCycles;
extern cycle_t ActionCycles;
extern int BotWTG;

IMPLEMENT_CLASS(DThinker, false, false)

struct ProfileInfo
{
	int numcalls = 0;
	cycle_t timer;

	ProfileInfo()
	{
		timer.Reset();
	}
};

static TMap<FName, ProfileInfo> Profiles, ClientSideProfiles;
static unsigned int profilethinkers, profilelimit;
static unsigned int csprofilethinkers, csprofilelimit;
DThinker *NextToThink;

//==========================================================================
//
//
//
//==========================================================================

FThinkerCollection::FThinkerCollection(bool trackActors) : TrackActors(trackActors)
{
	for (int i = 0; i <= MAX_STATNUM + 1; ++i) Thinkers[i].Owner = this, Thinkers[i].Stat = i;
	for (int i = 0; i <= MAX_STATNUM; ++i) FreshThinkers[i].Owner = this, FreshThinkers[i].Stat = i;
}

void FThinkerCollection::DenseAdd(DThinker* thinker, int stat)
{
	if (!TrackActors || stat < STAT_FIRST_THINKING || stat > MAX_STATNUM || thinker->DenseOwner != nullptr ||
		!thinker->IsKindOf(RUNTIME_CLASS(AActor))) return;
	thinker->DenseOwner = this;
	thinker->DenseIndex = int(DenseActors.Size());
	DenseActors.Push(static_cast<AActor*>(thinker));
}

void FThinkerCollection::DenseRemove(DThinker* thinker)
{
	FThinkerCollection* owner = thinker->DenseOwner;
	if (owner == nullptr) return;
	AActor* last = owner->DenseActors.Last();
	owner->DenseActors[thinker->DenseIndex] = last;
	last->DenseIndex = thinker->DenseIndex;
	owner->DenseActors.Pop();
	thinker->DenseOwner = nullptr;
	thinker->DenseIndex = -1;
}

void FThinkerCollection::Link(DThinker *thinker, int statnum)
{
	FThinkerList *list;
	if ((thinker->ObjectFlags & OF_JustSpawned) && statnum >= STAT_FIRST_THINKING)
	{
		list = &FreshThinkers[statnum];
	}
	else
	{
		if (statnum != STAT_TRAVELLING) thinker->ObjectFlags &= ~OF_JustSpawned;
		list = &Thinkers[statnum];
	}
	list->AddTail(thinker);
}

//==========================================================================
//
//
//
//==========================================================================

void FThinkerCollection::RunThinkers(FLevelLocals *Level)
{
	int i, count;

	ThinkCount = 0;
	ThinkCycles.Reset();
	BotSupportCycles.Reset();
	ActionCycles.Reset();
	BotWTG = 0;

	ThinkCycles.Clock();

	CollectRecreate = p_recreate_list != 0 && !profilethinkers;
	Level->RecreateTicked = true;
	Level->RecreateReplayValid = false;
	Level->RecreateReplay.Clear();
	if (profilethinkers) Level->RecreateListValid = false;
	if (!profilethinkers)
	{
		// Tick every thinker left from last time
		for (i = STAT_FIRST_THINKING; i <= MAX_STATNUM; ++i)
		{
			Thinkers[i].TickThinkers(nullptr, ThinkCount);
		}

		// Keep ticking the fresh thinkers until there are no new ones.
		do
		{
			count = 0;
			for (i = STAT_FIRST_THINKING; i <= MAX_STATNUM; ++i)
			{
				count += FreshThinkers[i].TickThinkers(&Thinkers[i], ThinkCount);
			}
		} while (count != 0);
	}
	else
	{
		Profiles.Clear();
		// Tick every thinker left from last time
		for (i = STAT_FIRST_THINKING; i <= MAX_STATNUM; ++i)
		{
			Thinkers[i].ProfileThinkers(nullptr, ThinkCount, Profiles);
		}

		// Keep ticking the fresh thinkers until there are no new ones.
		do
		{
			count = 0;
			for (i = STAT_FIRST_THINKING; i <= MAX_STATNUM; ++i)
			{
				count += FreshThinkers[i].ProfileThinkers(&Thinkers[i], ThinkCount, Profiles);
			}
		} while (count != 0);

		struct SortedProfileInfo
		{
			const char* className;
			int numcalls;
			double time;
		};

		TArray<SortedProfileInfo> sorted;
		sorted.Grow(Profiles.CountUsed());

		auto it = TMap<FName, ProfileInfo>::Iterator(Profiles);
		TMap<FName, ProfileInfo>::Pair *pair;
		while (it.NextPair(pair))
		{
			sorted.Push({ pair->Key.GetChars(), pair->Value.numcalls, pair->Value.timer.TimeMS() });
		}

		std::sort(sorted.begin(), sorted.end(), [](const SortedProfileInfo& left, const SortedProfileInfo& right)
		{
			switch (profilethinkers)
			{
			case 1: // by name, from A to Z
				return stricmp(left.className, right.className) < 0;
			case 2: // by name, from Z to A
				return stricmp(right.className, left.className) < 0;
			case 3: // number of calls, ascending
				return left.numcalls < right.numcalls;
			case 4: // number of calls, descending
				return right.numcalls < left.numcalls;
			case 5: // average time, ascending
				return left.time / left.numcalls < right.time / right.numcalls;
			case 6: // average time, descending
				return right.time / right.numcalls < left.time / left.numcalls;
			case 7: // total time, ascending
				return left.time < right.time;
			default: // total time, descending
				return right.time < left.time;
			}
		});

		Printf(TEXTCOLOR_YELLOW "Total, ms   Averg, ms   Calls   Actor class\n");
		Printf(TEXTCOLOR_YELLOW "----------  ----------  ------  --------------------\n");

		const unsigned count = min(profilelimit > 0 ? profilelimit : UINT_MAX, sorted.Size());

		for (unsigned i = 0; i < count; ++i)
		{
			const SortedProfileInfo& info = sorted[i];
			Printf("%s%10.6f  %s%10.6f  %s%6d  %s%s\n",
				profilethinkers >= 7 ? TEXTCOLOR_YELLOW : TEXTCOLOR_WHITE, info.time,
				profilethinkers == 5 || profilethinkers == 6 ? TEXTCOLOR_YELLOW : TEXTCOLOR_WHITE, info.time / info.numcalls,
				profilethinkers == 3 || profilethinkers == 4 ? TEXTCOLOR_YELLOW : TEXTCOLOR_WHITE, info.numcalls,
				profilethinkers == 1 || profilethinkers == 2 ? TEXTCOLOR_YELLOW : TEXTCOLOR_WHITE, info.className);
		}

		profilethinkers = 0;
	}

	ThinkCycles.Unclock();
	const double thinkMS = ThinkCycles.TimeMS();
	ThinkWindowMS += thinkMS;
	ThinkWindowMaxMS = max(ThinkWindowMaxMS, thinkMS);
	++ThinkWindowTics;
}

CCMD(playsim_timing)
{
	Printf("PLAYSIMTIMING tics=%d think_ms_per_tic=%.4f think_ms_max=%.4f\n", ThinkWindowTics,
		ThinkWindowTics ? ThinkWindowMS / ThinkWindowTics : 0.0, ThinkWindowMaxMS);
}

//==========================================================================
//
// This version doesn't modify the level since that's already been done by
// the networked ticking. This also runs while the player is predicting
// to make sure it keeps ticking regardless of network game status.
//
//==========================================================================

static uint64_t RecreatePasses, RecreateListPasses, RecreateCandidatesSeen, RecreateVerifyMissed;

CCMD(recreate_stats)
{
	Printf("RECREATESTATS passes=%llu list_passes=%llu candidates_per_pass=%.2f verify_missed=%llu replay_passes=%llu replay_missed=%llu\n",
		(unsigned long long)RecreatePasses, (unsigned long long)RecreateListPasses,
		RecreatePasses ? double(RecreateCandidatesSeen) / RecreatePasses : 0.0, (unsigned long long)RecreateVerifyMissed,
		(unsigned long long)RecreateReplayPasses, (unsigned long long)RecreateReplayMissed);
	RecreatePasses = RecreateListPasses = RecreateCandidatesSeen = RecreateVerifyMissed = RecreateReplayPasses = RecreateReplayMissed = 0;
}

void AActor::QueueRecreate()
{
	if (p_recreate_list == 0 || RecreateStamp == Level->RecreateSerial) return;
	RecreateStamp = Level->RecreateSerial;
	Level->RecreateCandidates.Push(MakeObjPtr<AActor*>(this));
}

static bool NeedsRecreatePass(AActor* mobj, bool frozen)
{
	return (mobj->flags8 & MF8_RECREATELIGHTS) || ((mobj->effects || mobj->fountaincolor) && !frozen && mobj->ShouldRenderLocally());
}

// [Karto] Recreated lights change their owner's AttachedLights after the worker copied it, so such a tic publishes on the main thread.
uint64_t LightRecreates;

extern bool DeferLightLinks;
extern TArray<FDynamicLight*> DeferredLightLinks;
static unsigned DeferredLightCursor;
// [Karto] The level the list was made for: after a level change its lights may be freed, so it is dropped unread.
static FLevelLocals* DeferredLightLevel;
uint64_t WorkerLightRecreates, DeferredLightMisses;

static void CompleteDeferredLights(AActor* mobj)
{
	while (DeferredLightCursor < DeferredLightLinks.Size() && DeferredLightLinks[DeferredLightCursor]->target == mobj)
	{
		FDynamicLight* light = DeferredLightLinks[DeferredLightCursor++];
		// [Karto] ApplyProperties read the client clock for pulse lights, which has advanced since.
		if (light->GetLightType() == PulseLight) light->m_lastUpdate = light->GetTimer();
		light->UpdateLocation();
	}
}

static void FinishDeferredLights(FLevelLocals* Level)
{
	if (Level != DeferredLightLevel)
	{
		DeferredLightLinks.Clear();
		DeferredLightCursor = 0;
		return;
	}
	if (DeferredLightCursor < DeferredLightLinks.Size())
	{
		++DeferredLightMisses;
		for (unsigned i = DeferredLightCursor; i < DeferredLightLinks.Size(); ++i) DeferredLightLinks[i]->UpdateLocation();
	}
	DeferredLightLinks.Clear();
	DeferredLightCursor = 0;
}

// [Karto] A reload (restart on death) keeps the same FLevelLocals, so the level check above misses it: the list would
// update freed lights of the old actors.
void P_DropDeferredLights()
{
	DeferredLightLinks.Clear();
	DeferredLightCursor = 0;
	DeferredLightLevel = nullptr;
}

// [Karto] The list holds object pointers that must not survive the GC step that follows the batch.
void P_DropRecreateReplay()
{
	for (auto Level : AllLevels())
	{
		Level->RecreateReplay.Clear();
		Level->RecreateReplayValid = false;
	}
}

void P_WorkerRecreateLights(FLevelLocals* Level)
{
	DeferredLightLinks.Clear();
	DeferredLightCursor = 0;
	DeferredLightLevel = Level;
	if (!r_dynlights) return;
	DeferLightLinks = true;
	auto recreate = [](AActor* ac)
	{
		if (!(ac->flags8 & MF8_RECREATELIGHTS)) return;
		ac->flags8 &= ~MF8_RECREATELIGHTS;
		++WorkerLightRecreates;
		ac->SetDynamicLights();
	};
	if (p_recreate_list == 1 && Level->RecreateListValid && Level->RecreateTicked)
	{
		for (auto& candidate : Level->RecreateCandidates)
		{
			if (AActor* ac = candidate) recreate(ac);
		}
	}
	else
	{
		auto it = Level->GetThinkerIterator<AActor>();
		while (auto ac = it.Next()) recreate(ac);
	}
	DeferLightLinks = false;
}

static void RecreateDynamicLights(AActor* mobj, bool dolights, bool frozen)
{
	if (DeferredLightCursor < DeferredLightLinks.Size() && mobj->Level == DeferredLightLevel) CompleteDeferredLights(mobj);
	if (mobj->flags8 & MF8_RECREATELIGHTS)
	{
		mobj->flags8 &= ~MF8_RECREATELIGHTS;
		if (dolights)
		{
			++LightRecreates;
			mobj->SetDynamicLights();
		}
	}
	// This was merged from P_RunEffects to eliminate the costly duplicate ThinkerIterator loop.
	if ((mobj->effects || mobj->fountaincolor) && !frozen && mobj->ShouldRenderLocally())
	{
		P_RunEffect(mobj, mobj->effects);
	}
}

void FThinkerCollection::RunClientSideThinkers(FLevelLocals* Level)
{
	int i, count;

	ClientSideThinkCount = 0;
	ClientSideThinkCycles.Reset();
	CollectRecreate = false;

	ClientSideThinkCycles.Clock();

	bool dolights;
	if (r_dynlights)
	{
		dolights = true;// Level->lights || (Level->flags3 & LEVEL3_LIGHTCREATED);
	}
	else
	{
		dolights = false;
	}

	const bool paused = WorldPaused(false);
	Level->flags3 &= ~LEVEL3_LIGHTCREATED;
	Level->LocalWorldTimer += !paused;
	++Level->LocalTimer;

	auto recreateLights = [=]() {
		// Set dynamic lights at the end of the tick, so that this catches all changes being made through the last
		// frame.
		const bool frozen = paused || Level->isFrozen();
		const int mode = p_recreate_list;
		RecreatePasses++;
		RecreateCandidatesSeen += Level->RecreateCandidates.Size();
		const bool ticked = Level->RecreateTicked;
		Level->RecreateTicked = false;
		const int replay = p_recreate_replay;
		if (mode == 1 && Level->RecreateListValid && ticked)
		{
			RecreateListPasses++;
			for (auto& candidate : Level->RecreateCandidates)
			{
				if (AActor* ac = candidate) RecreateDynamicLights(ac, dolights, frozen);
			}
			if (replay)
			{
				Level->RecreateReplay = std::move(Level->RecreateCandidates);
				Level->RecreateReplaySerial = Level->RecreateSerial;
				Level->RecreateReplayValid = true;
			}
		}
		else if (mode == 1 && replay && !ticked && Level->RecreateReplayValid && Level->RecreateListValid)
		{
			RecreateReplayPasses++;
			if (replay == 2)
			{
				auto it = Level->GetThinkerIterator<AActor>();
				while (auto ac = it.Next())
				{
					if (ac->RecreateStamp != Level->RecreateReplaySerial && NeedsRecreatePass(ac, frozen)) RecreateReplayMissed++;
				}
			}
			for (auto& candidate : Level->RecreateReplay)
			{
				if (AActor* ac = candidate) RecreateDynamicLights(ac, dolights, frozen);
			}
		}
		else
		{
			auto it = Level->GetThinkerIterator<AActor>();
			while (auto ac = it.Next())
			{
				if (mode == 2 && ticked && ac->RecreateStamp != Level->RecreateSerial && NeedsRecreatePass(ac, frozen))
					RecreateVerifyMissed++;
				RecreateDynamicLights(ac, dolights, frozen);
			}
			Level->RecreateListValid = mode != 0;
		}
		Level->RecreateCandidates.Clear();
		if (++Level->RecreateSerial == 0) Level->RecreateSerial = 1;
		FinishDeferredLights(Level);

		auto it = Level->GetClientSideThinkerIterator<AActor>();
		while (auto ac = it.Next())
		{
			RecreateDynamicLights(ac, dolights, frozen);
		}
	};

	// Tick every thinker left from last time
	if (!paused)
	{
		if (!csprofilethinkers)
		{
			for (i = STAT_FIRST_THINKING; i <= MAX_STATNUM; ++i)
			{
				Thinkers[i].TickThinkers(nullptr, ClientSideThinkCount);
			}

			// Keep ticking the fresh thinkers until there are no new ones.
			do
			{
				count = 0;
				for (i = STAT_FIRST_THINKING; i <= MAX_STATNUM; ++i)
				{
					count += FreshThinkers[i].TickThinkers(&Thinkers[i], ClientSideThinkCount);
				}
			} while (count != 0);
		}
		else
		{
			ClientSideProfiles.Clear();
			// Tick every thinker left from last time
			for (i = STAT_FIRST_THINKING; i <= MAX_STATNUM; ++i)
			{
				Thinkers[i].ProfileThinkers(nullptr, ClientSideThinkCount, ClientSideProfiles);
			}

			// Keep ticking the fresh thinkers until there are no new ones.
			do
			{
				count = 0;
				for (i = STAT_FIRST_THINKING; i <= MAX_STATNUM; ++i)
				{
					count += FreshThinkers[i].ProfileThinkers(&Thinkers[i], ClientSideThinkCount, ClientSideProfiles);
				}
			} while (count != 0);
		}
	}

	{
		FFrameStageScope stage(FS_RecreateLights);
		recreateLights();
	}
	if (dolights && !paused)
	{
		if (!csprofilethinkers)
		{
			for (auto light = Level->lights; light;)
			{
				auto next = light->next;
				light->Tick();
				light = next;
			}
		}
		else
		{
			// Also profile the internal dynamic lights, even though they are not implemented as thinkers.
			auto& prof = ClientSideProfiles[NAME_InternalDynamicLight];
			prof.timer.Clock();
			for (auto light = Level->lights; light;)
			{
				++prof.numcalls;
				auto next = light->next;
				light->Tick();
				light = next;
			}
			prof.timer.Unclock();
		}
	}

	if (!paused && csprofilethinkers)
	{
		struct SortedProfileInfo
		{
			const char* className;
			int numcalls;
			double time;
		};

		TArray<SortedProfileInfo> sorted;
		sorted.Grow(ClientSideProfiles.CountUsed());

		auto it = TMap<FName, ProfileInfo>::Iterator(ClientSideProfiles);
		TMap<FName, ProfileInfo>::Pair *pair;
		while (it.NextPair(pair))
		{
			sorted.Push({ pair->Key.GetChars(), pair->Value.numcalls, pair->Value.timer.TimeMS() });
		}

		std::sort(sorted.begin(), sorted.end(), [](const SortedProfileInfo& left, const SortedProfileInfo& right)
		{
			switch (csprofilethinkers)
			{
			case 1: // by name, from A to Z
				return stricmp(left.className, right.className) < 0;
			case 2: // by name, from Z to A
				return stricmp(right.className, left.className) < 0;
			case 3: // number of calls, ascending
				return left.numcalls < right.numcalls;
			case 4: // number of calls, descending
				return right.numcalls < left.numcalls;
			case 5: // average time, ascending
				return left.time / left.numcalls < right.time / right.numcalls;
			case 6: // average time, descending
				return right.time / right.numcalls < left.time / left.numcalls;
			case 7: // total time, ascending
				return left.time < right.time;
			default: // total time, descending
				return right.time < left.time;
			}
		});

		Printf(TEXTCOLOR_YELLOW "Total, ms   Averg, ms   Calls   Actor class\n");
		Printf(TEXTCOLOR_YELLOW "----------  ----------  ------  --------------------\n");

		const unsigned count = min(csprofilelimit > 0 ? csprofilelimit : UINT_MAX, sorted.Size());

		for (unsigned i = 0; i < count; ++i)
		{
			const SortedProfileInfo& info = sorted[i];
			Printf("%s%10.6f  %s%10.6f  %s%6d  %s%s\n",
				csprofilethinkers >= 7 ? TEXTCOLOR_YELLOW : TEXTCOLOR_WHITE, info.time,
				csprofilethinkers == 5 || csprofilethinkers == 6 ? TEXTCOLOR_YELLOW : TEXTCOLOR_WHITE, info.time / info.numcalls,
				csprofilethinkers == 3 || csprofilethinkers == 4 ? TEXTCOLOR_YELLOW : TEXTCOLOR_WHITE, info.numcalls,
				csprofilethinkers == 1 || csprofilethinkers == 2 ? TEXTCOLOR_YELLOW : TEXTCOLOR_WHITE, info.className);
		}

		csprofilethinkers = 0;
	}

	ClientSideThinkCycles.Unclock();
}

//==========================================================================
//
// Destroy every thinker
//
//==========================================================================

void FThinkerCollection::DestroyAllThinkers(bool fullgc)
{
	// If something was destroyed, run it again to make sure nothing got spawned and moved
	// to a previous stat num in the iteration process. This guarantees nothing gets skipped.
	bool destroyed = false;
	do
	{
		destroyed = false;
		bool error = false;

		bool didDestroy = false;
		for (int i = 0; i <= MAX_STATNUM; i++)
		{
			if (i != STAT_TRAVELLING && i != STAT_STATIC)
			{
				error |= Thinkers[i].DoDestroyThinkers(didDestroy);
				destroyed |= didDestroy;
				error |= FreshThinkers[i].DoDestroyThinkers(didDestroy);
				destroyed |= didDestroy;
			}
		}
		error |= Thinkers[MAX_STATNUM + 1].DoDestroyThinkers(didDestroy);
		destroyed |= didDestroy;
		if (fullgc) GC::FullGC();
		if (error)
		{
			ClearGlobalVMStack();
			if (fullgc) I_Error("DestroyAllThinkers failed");
			else I_FatalError("DestroyAllThinkers failed");
		}
	} while (destroyed);
}

//==========================================================================
//
//
//
//==========================================================================

void FThinkerCollection::CleanUpTravellers(bool saveGame)
{
	DestroyThinkersInList(STAT_TRAVELLING);
	for (size_t i = 0u; i <= MAX_STATNUM; ++i)
	{
		FreshThinkers[i].RemoveTravellers(saveGame);
		Thinkers[i].RemoveTravellers(saveGame);
	}
}

//==========================================================================
//
//
//
//==========================================================================

void FThinkerCollection::OnLoad()
{
	for (auto& list : FreshThinkers)
		list.OnLoad();
	for (auto& list : Thinkers)
		list.OnLoad();
}

//==========================================================================
//
//
//
//==========================================================================

void FThinkerCollection::SerializeThinkers(FSerializer &arc, bool hubLoad)
{
	//DThinker *thinker;
	//uint8_t stat;
	//int statcount;
	int i;

	if (arc.isWriting())
	{
		arc.BeginArray("thinkers");
		for (i = 0; i <= MAX_STATNUM; i++)
		{
			arc.BeginArray(nullptr);
			Thinkers[i].SaveList(arc);
			FreshThinkers[i].SaveList(arc);
			arc.EndArray();
		}
		arc.EndArray();
	}
	else
	{
		if (arc.BeginArray("thinkers"))
		{
			for (i = 0; i <= MAX_STATNUM; i++)
			{

				if (arc.BeginArray(nullptr))
				{
					if (!hubLoad || i != STAT_STATIC)	// do not load static thinkers in a hub transition because they'd just duplicate the active ones.
					{
						int size = arc.ArraySize();
						for (int j = 0; j < size; j++)
						{
							DThinker *thinker = nullptr;
							arc(nullptr, thinker);
							if (thinker != nullptr)
							{
								// This may be a player stored in their ancillary list. Remove
								// them first before inserting them into the new list.
								if (thinker->NextThinker != nullptr)
								{
									thinker->Remove();
								}
								// Thinkers with the OF_JustSpawned flag set go in the FreshThinkers
								// list. Anything else goes in the regular Thinkers list.
								if (thinker->ObjectFlags & OF_EuthanizeMe)
								{
									// This thinker was destroyed during the loading process. Do
									// not link it into any list.
								}
								else if (thinker->ObjectFlags & OF_JustSpawned)
								{
									FreshThinkers[i].AddTail(thinker);
									thinker->PostSerialize();
								}
								else
								{
									Thinkers[i].AddTail(thinker);
									thinker->PostSerialize();
								}
							}
						}
					}
					arc.EndArray();
				}
			}
			arc.EndArray();
		}
	}
}


//==========================================================================
//
//
//
//==========================================================================

void FThinkerList::AddTail(DThinker *thinker)
{
	assert(thinker->PrevThinker == nullptr && thinker->NextThinker == nullptr);
	assert(!(thinker->ObjectFlags & OF_EuthanizeMe));
	if (Sentinel == nullptr)
	{
		// This cannot use CreateThinker because it must not be added to the list automatically.
		Sentinel = (DThinker*)RUNTIME_CLASS(DThinker)->CreateNew();
		Sentinel->ObjectFlags |= OF_Sentinel;
		Sentinel->NextThinker = Sentinel;
		Sentinel->PrevThinker = Sentinel;
		GC::WriteBarrier(Sentinel);
	}
	DThinker *tail = Sentinel->PrevThinker;
	assert(tail->NextThinker == Sentinel);
	thinker->PrevThinker = tail;
	thinker->NextThinker = Sentinel;
	tail->NextThinker = thinker;
	Sentinel->PrevThinker = thinker;
	if (Owner != nullptr) Owner->DenseAdd(thinker, Stat);
	GC::WriteBarrier(thinker, tail);
	GC::WriteBarrier(thinker, Sentinel);
	GC::WriteBarrier(tail, thinker);
	GC::WriteBarrier(Sentinel, thinker);
}

//==========================================================================
//
//
//
//==========================================================================

DThinker *FThinkerCollection::FirstThinker(int statnum)
{
	DThinker *node;

	if ((unsigned)statnum > MAX_STATNUM)
	{
		statnum = MAX_STATNUM;
	}
	node = Thinkers[statnum].GetHead();
	if (node == nullptr)
	{
		node = FreshThinkers[statnum].GetHead();
		if (node == nullptr)
		{
			return nullptr;
		}
	}
	return node;
}

//==========================================================================
//
// Mark the first thinker of each list
//
//==========================================================================

void FThinkerCollection::MarkRoots()
{
	for (int i = 0; i <= MAX_STATNUM; ++i)
	{
		GC::Mark(Thinkers[i].Sentinel);
		GC::Mark(FreshThinkers[i].Sentinel);
	}
	GC::Mark(Thinkers[MAX_STATNUM + 1].Sentinel);
}

//==========================================================================
//
//
//
//==========================================================================

DThinker *FThinkerList::GetHead() const
{
	if (Sentinel == nullptr || Sentinel->NextThinker == Sentinel)
	{
		return nullptr;
	}
	assert(Sentinel->NextThinker->PrevThinker == Sentinel);
	return Sentinel->NextThinker;
}

//==========================================================================
//
//
//
//==========================================================================

DThinker *FThinkerList::GetTail() const
{
	if (Sentinel == nullptr || Sentinel->PrevThinker == Sentinel)
	{
		return nullptr;
	}
	return Sentinel->PrevThinker;
}

//==========================================================================
//
//
//
//==========================================================================

bool FThinkerList::IsEmpty() const
{
	return Sentinel == nullptr || Sentinel->NextThinker == nullptr;
}

//==========================================================================
//
//
//
//==========================================================================

void FThinkerList::DestroyThinkers()
{
	bool destroyed = false;
	do
	{
		if (DoDestroyThinkers(destroyed))
		{
			I_Error("DestroyThinkers failed");
		}
	} while (destroyed);
}

//==========================================================================
//
//
//
//==========================================================================

bool FThinkerList::DoDestroyThinkers(bool& destroyed)
{
	destroyed = false;
	bool error = false;
	if (Sentinel != nullptr)
	{
		// Taking down the linked list live is far too dangerous in case something goes wrong. So first copy all elements into an array, take down the list and then destroy them.
		destroyed = true;
		TArray<DThinker *> toDelete;
		DThinker *node = Sentinel->NextThinker;
		while (node != Sentinel)
		{
			assert(node != nullptr);
			auto next = node->NextThinker;
			toDelete.Push(node);
			node->NextThinker = node->PrevThinker = nullptr;	// clear the links
			// [Karto] Unlinked here without Remove, so OnDestroy will not take it out of the dense list.
			FThinkerCollection::DenseRemove(node);
			node = next;
		}
		Sentinel->NextThinker = Sentinel->PrevThinker = nullptr;
		Sentinel->Destroy();
		Sentinel = nullptr;
		for (auto node : toDelete)
		{
			// We must intercept all exceptions so that we can continue deleting the list.
			try
			{
				node->Destroy();
			}
			catch (CVMAbortException &exception)
			{
				Printf("VM exception in DestroyThinkers:\n");
				exception.MaybePrintMessage();
				Printf(static_cast<PrintFlag>(PRINT_NONOTIFY | PRINT_BOLD), "%s", exception.stacktrace.GetChars());
				// forcibly delete this. Cleanup may be incomplete, though.
				node->ObjectFlags |= OF_YesReallyDelete;
				delete node;
				error = true;
			}
			catch (CRecoverableError &exception)
			{
				Printf(static_cast<PrintFlag>(PRINT_NONOTIFY | PRINT_BOLD), "Error in DestroyThinkers: %s\n", exception.GetMessage());
				// forcibly delete this. Cleanup may be incomplete, though.
				node->ObjectFlags |= OF_YesReallyDelete;
				delete node;
				error = true;
			}
		}
	}
	return error;
}

//==========================================================================
//
//
//
//==========================================================================

void FThinkerList::SaveList(FSerializer &arc)
{
	auto node = GetHead();
	if (node != nullptr)
	{
		while (!(node->ObjectFlags & OF_Sentinel))
		{
			assert(node->NextThinker != nullptr && !(node->NextThinker->ObjectFlags & OF_EuthanizeMe));
			::Serialize<DThinker>(arc, nullptr, node, nullptr);
			node = node->NextThinker;
		}
	}
}

//==========================================================================
//
//
//
//==========================================================================

void FThinkerList::RemoveTravellers(bool saveGame)
{
	DThinker* node = GetHead();
	if (node == nullptr)
		return;

	while (node != Sentinel)
	{
		NextToThink = node->NextThinker;
		if ((node->ObjectFlags & OF_Travelling) && !(node->ObjectFlags & OF_EuthanizeMe))
		{
			if (saveGame)
				node->ObjectFlags &= ~OF_Travelling;
			else
				node->Destroy();
		}
		node = NextToThink;
	}
}

//==========================================================================
//
//
//
//==========================================================================

void FThinkerList::OnLoad()
{
	DThinker* node = GetHead();
	if (node == nullptr)
		return;

	while (node != Sentinel)
	{
		NextToThink = node->NextThinker;
		if (!(node->ObjectFlags & OF_EuthanizeMe))
		{
			IFOVERRIDENVIRTUALPTRNAME(node, NAME_Thinker, OnLoad)
				VMCallVoid<DThinker*>(func, node);
		}
		node = NextToThink;
	}
}

//==========================================================================
//
//
//
//==========================================================================

EXTERN_CVAR(Int, p_rest_tick)
bool P_RestTickEligible(AActor* a);

static bool NativeThinkerTick(DThinker* node)
{
	static unsigned VIndex = ~0u;
	static VMFunction* Base = nullptr;
	static VMFunction* ActorBase = nullptr;
	if (VIndex == ~0u)
	{
		VIndex = GetVirtualIndex(RUNTIME_CLASS(DThinker), "Tick");
		Base = RUNTIME_CLASS(DThinker)->Virtuals[VIndex];
		ActorBase = RUNTIME_CLASS(AActor)->Virtuals.Size() > VIndex ? RUNTIME_CLASS(AActor)->Virtuals[VIndex] : nullptr;
	}
	auto clss = node->GetClass();
	VMFunction* func = clss->Virtuals.Size() > VIndex ? clss->Virtuals[VIndex] : nullptr;
	return func == nullptr || func == Base || func == ActorBase;
}

CVAR(Bool, p_thinker_prefetch, true, 0)

static void PrefetchThinkersAhead(DThinker *next, DThinker *after)
{
	PrefetchL3(after);
	const AActor *actor = static_cast<const AActor *>(next);
	const char *first = reinterpret_cast<const char *>(&actor->__Pos);
	const char *last = reinterpret_cast<const char *>(&actor->target + 1);
	for (const char *line = first; line < last; line += 64) PrefetchL3(line);
}

int FThinkerList::TickThinkers(FThinkerList *dest, int& counter)
{
	int count = 0;
	DThinker *node = GetHead();

	if (node == nullptr)
	{
		return 0;
	}

	const bool prefetch = p_thinker_prefetch;
	while (node != Sentinel)
	{
		++count;
		NextToThink = node->NextThinker;
		if (prefetch && NextToThink != Sentinel) PrefetchThinkersAhead(NextToThink, NextToThink->NextThinker);
		if (node->ObjectFlags & OF_JustSpawned)
		{
			// Leave OF_JustSpawn set until after Tick() so the ticker can check it.
			if (dest != nullptr)
			{ // Move thinker from this list to the destination list
				node->Remove();
				dest->AddTail(node);
			}
			node->CallPostBeginPlay();
		}
		else if (dest != nullptr)
		{
			I_Error("There is a thinker in the fresh list that has already ticked.\n");
		}

		if (!(node->ObjectFlags & OF_EuthanizeMe))
		{ // Only tick thinkers not scheduled for destruction
			++counter;
			bool rested = false;
			if (p_rest_tick == 1 && NativeThinkerTick(node))
			{
				if (auto ac = dyn_cast<AActor>(node); ac != nullptr && P_RestTickEligible(ac))
				{
					if (ac->tics != -1) --ac->tics;
					rested = true;
				}
			}
			if (!rested) node->CallTick();
			node->ObjectFlags &= ~OF_JustSpawned;
			if (CollectRecreate && !(node->ObjectFlags & OF_EuthanizeMe))
			{
				if (auto ac = dyn_cast<AActor>(node))
				{
					if ((ac->flags8 & MF8_RECREATELIGHTS) || ac->effects || ac->fountaincolor) ac->QueueRecreate();
				}
			}
		}
		node = NextToThink;
	}
	return count;
}

//==========================================================================
//
//
//
//==========================================================================
int FThinkerList::ProfileThinkers(FThinkerList *dest, int& counter, TMap<FName, ProfileInfo>& profiles)
{
	int count = 0;
	DThinker *node = GetHead();

	if (node == nullptr)
	{
		return 0;
	}

	while (node != Sentinel)
	{
		++count;
		NextToThink = node->NextThinker;
		if (node->ObjectFlags & OF_JustSpawned)
		{
			// Leave OF_JustSpawn set until after Tick() so the ticker can check it.
			if (dest != nullptr)
			{ // Move thinker from this list to the destination list
				node->Remove();
				dest->AddTail(node);
			}
			node->CallPostBeginPlay();
		}
		else if (dest != nullptr)
		{
			I_Error("There is a thinker in the fresh list that has already ticked.\n");
		}

		if (!(node->ObjectFlags & OF_EuthanizeMe))
		{ // Only tick thinkers not scheduled for destruction
			++counter;

			auto &prof = profiles[node->GetClass()->TypeName];
			prof.numcalls++;
			prof.timer.Clock();
			node->CallTick();
			prof.timer.Unclock();
			node->ObjectFlags &= ~OF_JustSpawned;
		}
		node = NextToThink;
	}
	return count;
}


//==========================================================================
//
//
//
//==========================================================================

DThinker::~DThinker()
{
	assert(NextThinker == nullptr && PrevThinker == nullptr);
}

void DThinker::OnDestroy()
{
	assert((NextThinker != nullptr && PrevThinker != nullptr) ||
		   (NextThinker == nullptr && PrevThinker == nullptr));
	if (NextThinker != nullptr)
	{
		Remove();
	}
	_statNum = -1;
	Super::OnDestroy();
}

void DThinker::Serialize(FSerializer &arc)
{
	Super::Serialize(arc);
	arc("level", Level)
		("statnum", _statNum);
}

//==========================================================================
//
//
//
//==========================================================================

static int GetStatNum(DThinker* self)
{
	return self->GetStatNum();
}

DEFINE_ACTION_FUNCTION_NATIVE(DThinker, GetStatNum, GetStatNum)
{
	PARAM_SELF_PROLOGUE(DThinker);
	ACTION_RETURN_INT(self->GetStatNum());
}

//==========================================================================
//
//
//
//==========================================================================

void DThinker::Remove()
{
	if (this == NextToThink)
	{
		NextToThink = NextThinker;
	}
	DThinker *prev = PrevThinker;
	DThinker *next = NextThinker;
	if (prev == nullptr && next == nullptr) return;	// This was already removed earlier.

	assert((ObjectFlags & OF_Sentinel) || (prev != this && next != this));
	assert(prev->NextThinker == this);
	assert(next->PrevThinker == this);
	prev->NextThinker = next;
	next->PrevThinker = prev;
	GC::WriteBarrier(prev, next);
	GC::WriteBarrier(next, prev);
	NextThinker = nullptr;
	PrevThinker = nullptr;
	FThinkerCollection::DenseRemove(this);
}

//==========================================================================
//
//
//
//==========================================================================

void DThinker::PostBeginPlay()
{
}

static void NativePostBeginPlay(DThinker* self)
{
	self->PostBeginPlay();
}

DEFINE_ACTION_FUNCTION_NATIVE(DThinker, PostBeginPlay, NativePostBeginPlay)
{
	PARAM_SELF_PROLOGUE(DThinker);
	self->PostBeginPlay();
	return 0;
}

void DThinker::CallPostBeginPlay()
{
	ObjectFlags |= OF_Spawned;
	IFVIRTUAL(DThinker, PostBeginPlay)
		VMCallVoid<DThinker*>(func, this);
	else
		PostBeginPlay();
}

//==========================================================================
//
//
//
//==========================================================================

void DThinker::PostSerialize()
{
}

//==========================================================================
//
//
//
//==========================================================================

DThinker *FLevelLocals::FirstThinker(int statnum)
{
	return Thinkers.FirstThinker(statnum);
}

DThinker* FLevelLocals::FirstClientSideThinker(int statnum)
{
	return ClientSideThinkers.FirstThinker(statnum);
}

//==========================================================================
//
//
//
//==========================================================================

void DThinker::ChangeStatNum(int statnum)
{
	if ((unsigned)statnum > MAX_STATNUM)
		statnum = MAX_STATNUM;
	Remove();
	if (IsClientSide())
		Level->ClientSideThinkers.Link(this, statnum);
	else
		Level->Thinkers.Link(this, statnum);
	// Let us relink it back properly when we're done travelling.
	if (statnum != STAT_TRAVELLING)
		_statNum = statnum;
}

static void ChangeStatNum(DThinker *self, int statnum)
{
	if (self->ObjectFlags & OF_Travelling)
	{
		Printf(TEXTCOLOR_RED "Thinkers cannot be moved while travelling\n");
		return;
	}

	// This will always break Actors, they should use STAT_TRAVELLING instead to
	// transition between levels.
	if (statnum == STAT_STATIC && self->IsKindOf(NAME_Actor))
	{
		Printf(TEXTCOLOR_RED "Actors cannot be added to STAT_STATIC\n");
		return;
	}
	else if (statnum == STAT_TRAVELLING)
	{
		Printf(TEXTCOLOR_RED "Thinkers cannot be added to STAT_TRAVELLING manually\n");
		return;
	}

	self->ChangeStatNum(statnum);
}

DEFINE_ACTION_FUNCTION_NATIVE(DThinker, ChangeStatNum, ChangeStatNum)
{
	PARAM_SELF_PROLOGUE(DThinker);
	PARAM_INT(stat);
	ChangeStatNum(self, stat);
	return 0;
}

//==========================================================================
//
//
//
//==========================================================================

bool bTravelling = false;

static void AddToTravellingList(DThinker* self)
{
	if (!bTravelling)
	{
		Printf(TEXTCOLOR_RED "Thinkers can only be set to travel on level change\n");
		return;
	}
	// These should be handled by the owning Actor, otherwise they'll lose them and become useless anyway.
	if (self->IsKindOf(NAME_Inventory) && self->PointerVar<AActor>(NAME_Owner) != nullptr)
	{
		Printf(TEXTCOLOR_RED "Owned Inventory items must travel with their owner on level change\n");
		return;
	}
	if (self->IsKindOf(NAME_Bot))
	{
		Printf(TEXTCOLOR_RED "Bot Thinkers must travel with their owner on level change\n");
		return;
	}
	auto mo = dyn_cast<AActor>(self);
	if (mo != nullptr && (mo->flags & MF_UNMORPHED))
	{
		Printf(TEXTCOLOR_RED "Unmorphed Actors must travel with their owner on level change\n");
		return;
	}
	// These need to be locked down since they have native fields that won't be cleared
	// properly at the moment.
	auto cls = self->GetClass()->NativeClass();
	if (cls->TypeName != NAME_Thinker && cls->TypeName != NAME_Actor)
	{
		Printf(TEXTCOLOR_RED "Native thinkers cannot travel\n");
		return;
	}

	self->Level->AddToTravellingList(self);
}

DEFINE_ACTION_FUNCTION_NATIVE(DThinker, AddToTravellingList, AddToTravellingList)
{
	PARAM_SELF_PROLOGUE(DThinker);
	AddToTravellingList(self);
	return 0;
}

//==========================================================================
//
//
//
//==========================================================================

CCMD(profilethinkers)
{
	const int argc = argv.argc();

	if (argc == 2 || argc == 3)
	{
		const char *str = argv[1];
		bool ascend = true;

		if (*str == '+')
		{
			++str;
		}
		else if (*str == '-')
		{
			ascend = false;
			++str;
		}

		int mode = 0;

		switch (*str)
		{
		case 't': mode = ascend ? 7 : 8; break;
		case 'a': mode = ascend ? 5 : 6; break;
		case '#': mode = ascend ? 3 : 4; break;
		case 'c': mode = ascend ? 1 : 2; break;
		default: mode = atoi(str); break;
		}

		profilethinkers = mode;
		profilelimit = argc == 3 ? atoi(argv[2]) : 0;
	}
	else
	{
		Printf(
			"Usage: profilethinkers [+|-][t|a|#|c] [limit]\n"
			"       profilethinkers [1..8] [limit]\n\n"
			"Sorting modes:\n"
			TEXTCOLOR_YELLOW "c +c 1  " TEXTCOLOR_NORMAL "actor class, ascending\n"
			TEXTCOLOR_YELLOW "  -c 2  " TEXTCOLOR_NORMAL "actor class, descending\n"
			TEXTCOLOR_YELLOW "# +# 3  " TEXTCOLOR_NORMAL "number of calls, ascending\n"
			TEXTCOLOR_YELLOW "  -# 4  " TEXTCOLOR_NORMAL "number of calls, descending\n"
			TEXTCOLOR_YELLOW "a +a 5  " TEXTCOLOR_NORMAL "average time, ascending\n"
			TEXTCOLOR_YELLOW "  -a 6  " TEXTCOLOR_NORMAL "average time, descending\n"
			TEXTCOLOR_YELLOW "t +t 7  " TEXTCOLOR_NORMAL "total time, ascending\n"
			TEXTCOLOR_YELLOW "  -t 8  " TEXTCOLOR_NORMAL "total time, descending\n");
	}
}

CCMD(profilecsthinkers)
{
	const int argc = argv.argc();

	if (argc == 2 || argc == 3)
	{
		const char *str = argv[1];
		bool ascend = true;

		if (*str == '+')
		{
			++str;
		}
		else if (*str == '-')
		{
			ascend = false;
			++str;
		}

		int mode = 0;

		switch (*str)
		{
		case 't': mode = ascend ? 7 : 8; break;
		case 'a': mode = ascend ? 5 : 6; break;
		case '#': mode = ascend ? 3 : 4; break;
		case 'c': mode = ascend ? 1 : 2; break;
		default: mode = atoi(str); break;
		}

		csprofilethinkers = mode;
		csprofilelimit = argc == 3 ? atoi(argv[2]) : 0;
	}
	else
	{
		Printf(
			"Usage: profilecsthinkers [+|-][t|a|#|c] [limit]\n"
			"       profilecsthinkers [1..8] [limit]\n\n"
			"Sorting modes:\n"
			TEXTCOLOR_YELLOW "c +c 1  " TEXTCOLOR_NORMAL "actor class, ascending\n"
			TEXTCOLOR_YELLOW "  -c 2  " TEXTCOLOR_NORMAL "actor class, descending\n"
			TEXTCOLOR_YELLOW "# +# 3  " TEXTCOLOR_NORMAL "number of calls, ascending\n"
			TEXTCOLOR_YELLOW "  -# 4  " TEXTCOLOR_NORMAL "number of calls, descending\n"
			TEXTCOLOR_YELLOW "a +a 5  " TEXTCOLOR_NORMAL "average time, ascending\n"
			TEXTCOLOR_YELLOW "  -a 6  " TEXTCOLOR_NORMAL "average time, descending\n"
			TEXTCOLOR_YELLOW "t +t 7  " TEXTCOLOR_NORMAL "total time, ascending\n"
			TEXTCOLOR_YELLOW "  -t 8  " TEXTCOLOR_NORMAL "total time, descending\n");
	}
}

//==========================================================================
//
//
//
//==========================================================================

void DThinker::Tick()
{
}

static void NativeTick(DThinker* self)
{
	self->Tick();
}

DEFINE_ACTION_FUNCTION_NATIVE(DThinker, Tick, NativeTick)
{
	PARAM_SELF_PROLOGUE(DThinker);
	self->Tick();
	return 0;
}

CVAR(Bool, p_tick_direct, true, 0)

void DThinker::CallTick()
{
	static unsigned VIndex = ~0u;
	static VMFunction *Base = nullptr;
	if (VIndex == ~0u)
	{
		VIndex = GetVirtualIndex(RUNTIME_CLASS(DThinker), "Tick");
		assert(VIndex != ~0u);
		Base = RUNTIME_CLASS(DThinker)->Virtuals[VIndex];
	}
	auto clss = GetClass();
	VMFunction *func = clss->Virtuals.Size() > VIndex ? clss->Virtuals[VIndex] : nullptr;
	if (func != nullptr && !(p_tick_direct && func == Base))
		VMCallVoid<DThinker*>(func, this);
	else
		Tick();
}

//==========================================================================
//
//
//
//==========================================================================

size_t DThinker::PropagateMark()
{
	// Do not choke on partially initialized objects (as happens when loading a savegame fails)
	if (NextThinker != nullptr || PrevThinker != nullptr)
	{
		assert(NextThinker != nullptr && !(NextThinker->ObjectFlags & OF_EuthanizeMe));
		assert(PrevThinker != nullptr && !(PrevThinker->ObjectFlags & OF_EuthanizeMe));
	}
	GC::Mark(NextThinker);
	GC::Mark(PrevThinker);
	return Super::PropagateMark();
}

//==========================================================================
//
//
//
//==========================================================================

FThinkerIterator::FThinkerIterator (FLevelLocals *l, const PClass *type, int statnum, bool clientside) : Level(l)
{
	m_ThinkerPool = clientside ? &Level->ClientSideThinkers : &Level->Thinkers;
	if ((unsigned)statnum > MAX_STATNUM)
	{
		m_Stat = STAT_FIRST_THINKING;
		m_SearchStats = true;
	}
	else
	{
		m_Stat = statnum;
		m_SearchStats = false;
	}
	m_ParentType = type;
	Reinit();
}

//==========================================================================
//
//
//
//==========================================================================

FThinkerIterator::FThinkerIterator (FLevelLocals *l, const PClass *type, int statnum, DThinker *prev, bool clientside) : Level(l)
{
	m_ThinkerPool = clientside ? &Level->ClientSideThinkers : &Level->Thinkers;
	if ((unsigned)statnum > MAX_STATNUM)
	{
		m_Stat = STAT_FIRST_THINKING;
		m_SearchStats = true;
	}
	else
	{
		m_Stat = statnum;
		m_SearchStats = false;
	}
	m_ParentType = type;
	if (prev == nullptr || (prev->NextThinker->ObjectFlags & OF_Sentinel))
	{
		Reinit();
	}
	else
	{
		m_CurrThinker = prev->NextThinker;
		m_SearchingFresh = false;
	}
}

//==========================================================================
//
//
//
//==========================================================================

void FThinkerIterator::Reinit ()
{
	m_CurrThinker = m_ThinkerPool->Thinkers[m_Stat].GetHead();
	m_SearchingFresh = false;
}

//==========================================================================
//
//
//
//==========================================================================

DThinker *FThinkerIterator::Next (bool exact)
{
	if (m_ParentType == nullptr)
	{
		return nullptr;
	}
	do
	{
		do
		{
			if (m_CurrThinker != nullptr)
			{
				while (!(m_CurrThinker->ObjectFlags & OF_Sentinel))
				{
					DThinker *thinker = m_CurrThinker;
					m_CurrThinker = thinker->NextThinker;
					if (exact)
					{
						if (thinker->IsA(m_ParentType)) return thinker;
					}
					else if (thinker->IsKindOf(m_ParentType))
					{
						return thinker;
					}
					// This can actually happen when a Destroy call on 'thinker' happens to destroy 'm_CurrThinker'.
					// In that case there is no chance to recover, we have to terminate the iteration of this list.
					if (m_CurrThinker == nullptr) break;
				}
			}
			if ((m_SearchingFresh = !m_SearchingFresh))
			{
				m_CurrThinker = m_ThinkerPool->FreshThinkers[m_Stat].GetHead();
			}
		} while (m_SearchingFresh);
		if (m_SearchStats)
		{
			m_Stat++;
			if (m_Stat > MAX_STATNUM)
			{
				m_Stat = STAT_FIRST_THINKING;
			}
		}
		m_CurrThinker = m_ThinkerPool->Thinkers[m_Stat].GetHead();
		m_SearchingFresh = false;
	} while (m_SearchStats && m_Stat != STAT_FIRST_THINKING);
	return nullptr;
}

//==========================================================================
//
//
//
//==========================================================================

ADD_STAT (think)
{
	FString out;
	out.Format ("Think time = %04.2f ms - %d thinkers, Client-side think time = %04.2f ms - %d thinkers\nAction = %04.2f ms",
		ThinkCycles.TimeMS(), ThinkCount, ClientSideThinkCycles.TimeMS(), ClientSideThinkCount, ActionCycles.TimeMS());
	return out;
}
