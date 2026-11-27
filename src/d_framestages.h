/*
** d_framestages.h
**
** [Karto] Test-only per-stage frame timers, reported separately for tic and non-tic frames.
*/

#pragma once

#include <cstdint>

enum EFrameStage
{
	FS_Tic,				// G_Ticker
	FS_Publish,			// end-of-tics publish and HUD record
	FS_Display,			// D_Display
	FS_UpdateWorldMode,
	FS_Walker,			// WorldModeWalkerFrame
	FS_Sprites,			// CollectWorldModeSprites (includes HWSprite::Process)
	FS_Sky,				// ReplayWorldModeSky
	FS_CreateScene,		// whole scene build, contains the four above
	FS_RenderScene,		// opaque pass
	FS_SolidSprites,
	FS_SortedList,		// sorted translucent list
	FS_AddPass,			// r_sprite_addpass list
	FS_Translucent,		// whole translucent pass, contains the three above
	FS_Update,			// End2DAndUpdate, inside D_Display: submit, present, waiting on the GPU
	FS_GC,				// GC::CheckGC
	FS_Net,				// NetUpdate, MakeConsistencies
	FS_Predict,			// P_UnPredictClient, P_PredictClient
	FS_ClientLogic,		// P_RunClientSideLogic
	FS_Sounds,			// S_UpdateSounds, VerifyPredictedEntities
	FS_CSThinkers,		// client-side thinkers incl. their interpolation reset (inside clientlogic)
	FS_RecreateLights,	// per-tic pass over every actor for lights and effects (inside cs thinkers)
	FS_StatusBar,		// StatusBar->CallTick (inside clientlogic)
	FS_SolidSort,
	FS_AlphaPass,
	FS_ParticlePass,
	FS_ParticleCollect,	// RenderParticles, per subsector
	FS_ParticleKeys,	// key build and grouping
	FS_ParticleBuild,	// vertex stream build
	FS_ParticleUpload,	// vertex stream SetData
	FS_ParticleDraw,	// the runs' DrawSprite and draws
	FS_Count
};

extern bool FrameStagesArmed;

void FrameStageBegin(EFrameStage stage);
void FrameStageEnd(EFrameStage stage);
void FrameStagesFrameStart();
void FrameStagesFrameEnd();

struct FFrameStageScope
{
	EFrameStage Stage;
	bool Active;
	explicit FFrameStageScope(EFrameStage stage) : Stage(stage), Active(FrameStagesArmed)
	{
		if (Active) FrameStageBegin(stage);
	}
	~FFrameStageScope()
	{
		if (Active) FrameStageEnd(Stage);
	}
};
