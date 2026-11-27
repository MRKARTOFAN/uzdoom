/*
** p_tick.cpp
**
** Ticker.
**
**---------------------------------------------------------------------------
**
** Copyright 1993-1996 id Software
** Copyright 1999-2016 Marisa Heit
** Copyright 2002-2016 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#include "dynarrayindex.h"
#include <cstddef>
#if defined(__GNUC__) || defined(__clang__)
#define InterpPrefetch(p) __builtin_prefetch(p)
#elif defined(_M_X64) || defined(_M_IX86)
#include <immintrin.h>
#define InterpPrefetch(p) _mm_prefetch((const char*)(p), _MM_HINT_T0)
#else
#define InterpPrefetch(p) ((void)(p))
#endif
#include "i_time.h"
#include "c_dispatch.h"
#include "p_local.h"
#include "p_effect.h"
#include "c_console.h"
#include "b_bot.h"
#include "doomstat.h"
#include "sbar.h"
#include "r_data/r_interpolate.h"
#include "d_player.h"
#include "r_utility.h"
#include "p_spec.h"
#include "g_levellocals.h"
#include "events.h"
#include "actorinlines.h"
#include "g_game.h"
#include "am_map.h"
#include "i_interface.h"
#include "d_ticworker.h"
#include "d_framestages.h"

EXTERN_CVAR(Int, p_rest_tick)

extern gamestate_t wipegamestate;
extern uint8_t globalfreeze, globalchangefreeze;

void C_Ticker();
void M_Ticker();
void D_RunCutscene();

//==========================================================================
//
// P_RunClientsideLogic
//
// Handles all logic that should be ran every tick including while
// predicting. Only put non-playsim behaviors in here to avoid desyncs
// when playing online.
//
//==========================================================================

void P_RunClientSideLogic()
{
	C_Ticker();
	M_Ticker();

	// [ZZ] also tick the UI part of the events
	primaryLevel->localEventManager->UiTick();

	if (gamestate == GS_LEVEL || gamestate == GS_TITLELEVEL)
	{
		if (!WorldPaused(false))
		{
			for (unsigned int i = 0; i < MAXPLAYERS; ++i)
			{
				if (playeringame[i] && players[i].inventorytics > 0)
					--players[i].inventorytics;
			}
		}

		for (auto level : AllLevels())
		{
			FFrameStageScope stage(FS_CSThinkers);
			auto it = level->GetClientSideThinkerIterator<AActor>();
			AActor* ac = nullptr;
			while ((ac = it.Next()) != nullptr)
			{
				ac->ClearInterpolation();
				ac->ClearFOVInterpolation();
			}

			level->ClientSideThinkers.RunClientSideThinkers(level);
		}

		{
			FFrameStageScope stage(FS_StatusBar);
			StatusBar->CallTick();
		}

		// TODO: Should this be called on all maps...?
		if (gamestate == GS_LEVEL)
			primaryLevel->automap->Ticker();
	}
	else if (gamestate == GS_CUTSCENE || gamestate == GS_INTRO)
	{
		D_RunCutscene();
	}

	// [MK] Additional ticker for UI events right after all others
	primaryLevel->localEventManager->PostUiTick();
}

//==========================================================================
//
// P_CheckTickerPaused
//
// Returns true if the ticker should be paused. In that case, it also
// pauses sound effects and possibly music. If the ticker should not be
// paused, then it returns false but does not unpause anything.
//
//==========================================================================

bool P_CheckTickerPaused ()
{
	// pause if in menu or console and at least one tic has been run
	if ( !netgame
		 && gamestate != GS_TITLELEVEL
		 && ((menuactive != MENU_Off && menuactive != MENU_OnNoPause && menuactive != MENU_GameplayMenu) ||
			 ConsoleState == c_down || ConsoleState == c_falling)
		 && !demoplayback
		 && !demorecording
		 && players[consoleplayer].viewz != NO_VALUE
		 && wipegamestate == gamestate)
	{
		// Only the current UI level's settings are relevant for sound.
		S_PauseSound (!(primaryLevel->flags2 & LEVEL2_PAUSE_MUSIC_IN_MENUS), false);
		return true;
	}
	return false;
}

void P_ClearLevelInterpolation()
{
	for (auto Level : AllLevels())
	{
		Level->interpolator.UpdateInterpolations();

		auto it = Level->GetThinkerIterator<AActor>();
		AActor* ac;

		while ((ac = it.Next()))
		{
			if (p_rest_tick && ac->Prev == ac->Pos() && ac->PrevAngles == ac->Angles && ac->PrevScale == ac->Scale &&
				ac->PrevAlpha == ac->Alpha && ac->PrevPortalGroup == (ac->Sector ? ac->Sector->PortalGroup : 0) && ac->player == nullptr &&
				ac->PrevFOV == DAngle::fromDeg(ac->CameraFOV))
				continue;
			ac->ClearInterpolation();
			ac->ClearFOVInterpolation();
		}
	}

	for (unsigned int i = 0; i < MAXPLAYERS; i++)
	{
		if (playeringame[i])
		{
			DPSprite* pspr = players[i].psprites;
			while (pspr)
			{
				pspr->ResetInterpolation();

				pspr = pspr->Next;
			}
		}
	}

	R_ClearInterpolationPath();
	if (StatusBar != nullptr)
		StatusBar->ClearInterpolation();
}

//
// P_Ticker
//
EXTERN_CVAR(Float, r_test_ticfrac)

void P_TickerOnMainBeforeWorker()
{
	unsigned int i;
	for (i = 0; i < MAXPLAYERS; i++)
	{
		if (playeringame[i] && players[i].timefreezer != 0)
			break;
	}
	if (i == MAXPLAYERS)
		S_ResumeSound(false);
}

void P_FinishDeferredLightReleases();

void P_TickerOnMainAfterWorker()
{
	P_FinishDeferredLightReleases();
	R_ClearInterpolationPath();
	r_NoInterpolate = false;
}

CVAR(Bool, r_interp_dense, true, 0)
CVAR(Bool, r_interp_dense_prefetch, true, 0)
CVAR(Bool, r_interp_dense_verify, false, 0)
static uint64_t InterpClearNS = 0, InterpClears = 0, InterpVerifyRuns = 0, InterpVerifyMismatches = 0;

CVAR(Int, r_interp_dense_threads, 4, 0)
void WorldModeParallelFor(int parts, void (*job)(void* context, int part, int parts), void* context);

static void InterpClearPart(void* context, int part, int parts)
{
	auto& dense = *static_cast<TArray<AActor*>*>(context);
	const unsigned total = dense.Size(), count = unsigned(uint64_t(total) * (part + 1) / parts);
	for (unsigned i = unsigned(uint64_t(total) * part / parts); i < count; ++i)
	{
		if (r_interp_dense_prefetch && i + 8 < count)
		{
			const char* ahead = reinterpret_cast<const char*>(dense[i + 8]);
			InterpPrefetch(ahead + offsetof(AActor, __Pos));
			InterpPrefetch(ahead + offsetof(AActor, Prev));
			InterpPrefetch(ahead + offsetof(AActor, Prev) + 64);
			InterpPrefetch(ahead + offsetof(AActor, CameraFOV));
		}
		AActor* ac = dense[i];
		ac->ClearInterpolation();
		ac->ClearFOVInterpolation();
	}
}

static void InterpDenseVerify(FLevelLocals* Level)
{
	++InterpVerifyRuns;
	unsigned count = 0;
	bool same = true;
	auto it = Level->GetThinkerIterator<AActor>();
	for (AActor* ac; (ac = it.Next()) != nullptr; ++count)
	{
		const unsigned index = unsigned(ac->DenseIndex);
		if (index >= Level->Thinkers.DenseActors.Size() || Level->Thinkers.DenseActors[index] != ac) same = false;
	}
	InterpVerifyMismatches += !same || count != Level->Thinkers.DenseActors.Size();
}

CCMD(interp_dense_stats)
{
	Printf("INTERPDENSE dense=%d clears=%llu us_per_clear=%.1f verify_runs=%llu verify_mismatches=%llu\n", int(r_interp_dense),
		(unsigned long long)InterpClears, InterpClears ? double(InterpClearNS) / InterpClears / 1000.0 : 0.0,
		(unsigned long long)InterpVerifyRuns, (unsigned long long)InterpVerifyMismatches);
	if (argv.argc() == 2 && stricmp(argv[1], "reset") == 0) InterpClearNS = InterpClears = InterpVerifyRuns = InterpVerifyMismatches = 0;
}

void P_Ticker (void)
{
	unsigned int i;

	ArrayIndexTic();

	const bool holdInterpolation = r_test_ticfrac >= 0 && (paused || P_CheckTickerPaused());
	// [Karto] On the worker, renderer globals and the sound engine stay with the main thread (P_TickerOnMain*).
	const bool onWorker = TicWorkerOnThread();
	if (!holdInterpolation)
	{
		for (auto Level : AllLevels())
		{
			Level->interpolator.UpdateInterpolations();
		}
		if (!onWorker) r_NoInterpolate = true;
	}

	if (!demoplayback)
	{
		// This is a separate slot from the wipe in D_Display(), because this
		// is delayed slightly due to latency. (Even on a singleplayer game!)
//		GSnd->SetSfxPaused(!!playerswiping, 2);
	}

	// run the tic
	if (paused || P_CheckTickerPaused())
	{
		// This must run even when the game is paused to catch changes from netevents before the frame is rendered.
		for (auto Level : AllLevels())
		{
			auto it = Level->GetThinkerIterator<AActor>();
			AActor* ac;

			while ((ac = it.Next()))
			{
				if (ac->flags8 & MF8_RECREATELIGHTS)
				{
					ac->flags8 &= ~MF8_RECREATELIGHTS;
					ac->SetDynamicLights();
				}
			}
		}
		return;
	}

	DPSprite::NewTick();

	// [RH] Frozen mode is only changed every 4 tics, to make it work with A_Tracer().
	// This may not be perfect but it is not really relevant for sublevels that tracer homing behavior is preserved.
	if ((primaryLevel->maptime & 3) == 0)
	{
		if (globalchangefreeze)
		{
			globalfreeze ^= 1;
			globalchangefreeze = 0;
			for (auto Level : AllLevels())
			{
				Level->frozenstate = (Level->frozenstate & ~2) | (2 * globalfreeze);
			}
		}
	}

	// [BC] Do a quick check to see if anyone has the freeze time power. If they do,
	// then don't resume the sound, since one of the effects of that power is to shut
	// off the music.
	for (i = 0; i < MAXPLAYERS; i++ )
	{
		if (playeringame[i] && players[i].timefreezer != 0)
			break;
	}

	if ( i == MAXPLAYERS && !onWorker )
		S_ResumeSound (false);

	P_ResetSightCounters (false);
	if (!onWorker) R_ClearInterpolationPath();

	// Since things will be moving, it's okay to interpolate them in the renderer.
	if (!onWorker) r_NoInterpolate = false;

	// Reset all actor interpolations on all levels before the current thinking turn so that indirect actor movement gets properly interpolated.
	for (auto Level : AllLevels())
	{
		const uint64_t clearStart = I_nsTime();
		if (r_interp_dense && Level->Thinkers.TracksActors())
		{
			auto& dense = Level->Thinkers.DenseActors;
			if (r_interp_dense_threads > 1 && dense.Size() >= 2048) WorldModeParallelFor(*r_interp_dense_threads, InterpClearPart, &dense);
			else InterpClearPart(&dense, 0, 1);
			if (r_interp_dense_verify) InterpDenseVerify(Level);
		}
		else
		{
			// todo: set up a sandbox for secondary levels here.
			auto it = Level->GetThinkerIterator<AActor>();
			AActor *ac;

			while ((ac = it.Next()))
			{
				ac->ClearInterpolation();
				ac->ClearFOVInterpolation();
			}
		}
		InterpClearNS += I_nsTime() - clearStart;
		++InterpClears;

		P_ThinkParticles(Level);	// [RH] make the particles think

		for (i = 0; i < MAXPLAYERS; i++)
			if (Level->PlayerInGame(i))
				P_PlayerThink(Level->Players[i]);

		// [ZZ] call the WorldTick hook
		Level->localEventManager->WorldTick();
		Level->Tick();			// [RH] let the level tick
		Level->Thinkers.RunThinkers(Level);

		//if added by MC: Freeze mode.
		if (!Level->isFrozen())
		{
			P_UpdateSpecials(Level);
		}

		// for par times
		Level->time++;
		Level->maptime++;
		Level->totaltime++;
	}
}
