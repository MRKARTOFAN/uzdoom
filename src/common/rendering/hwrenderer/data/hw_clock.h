/*
** hw_clock.h
**
** Hardware render profiling info
**
**---------------------------------------------------------------------------
**
** Copyright 2007-2018 Christoph Oelckers
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

#ifndef __GL_CLOCK_H
#define __GL_CLOCK_H

#include "stats.h"
#include "m_fixed.h"

extern glcycle_t RenderWall,SetupWall,ClipWall;
extern glcycle_t RenderFlat,SetupFlat;
extern glcycle_t RenderSprite,SetupSprite;
extern glcycle_t All, Finish, PortalAll, Bsp;
extern glcycle_t ProcessAll, PostProcess;
extern glcycle_t RenderAll;
extern glcycle_t Dirty;
extern glcycle_t drawcalls, twoD, Flush3D;
extern glcycle_t MTWait, WTTotal;

struct HWFinishProfile
{
	uint64_t TotalNS = 0;
	uint64_t AcquireNS = 0;
	uint64_t SubmitNS = 0;
	uint64_t LimitNS = 0;
	uint64_t PresentNS = 0;
	uint64_t FenceNS = 0;
	uint64_t CleanupNS = 0;
	uint64_t ReuseWaitNS = 0;
	uint64_t ReuseWaitCount = 0;
	int Submits = 0;
	int Fences = 0;
	bool Active = false;
	bool Fullscreen = false;
	bool VSync = false;
	int MaxFPS = 0;
	bool WallCache = false;
};

extern HWFinishProfile FinishProfile;
extern bool FinishProfileAvailable;
bool IsFinishProfilingEnabled();
void RecordFinishProfile(const uint64_t *ticks, int submits, int fences, uint64_t reuseNS, uint64_t reuseCount, bool fullscreen, bool vsync);

enum HWGPUCategory
{
	GPUCommands, GPUFrameSpan, GPUBloom, GPUBlur, GPULens, GPUFXAA,
	GPUExposure, GPUColorMap, GPUToneMap, GPUSSAO, GPUShadowMap, GPUCustom,
	GPUWorldOpaque, GPUWorldMasked, GPUWorldTranslucent,
	GPUWorldGenerate, GPUWorldScan, GPUWorldScatter, GPUWorldDraw, GPUCategoryCount
};

struct HWGPUProfile
{
	bool Enabled = false;
	bool Available = false;
	uint32_t Dropped = 0;
	uint64_t ReadNS = 0;
	uint64_t DurationNS[GPUCategoryCount] = {};
	uint32_t Samples[GPUCategoryCount] = {};
};

extern HWGPUProfile GPUProfile;
bool IsGPUProfilingEnabled();

struct HWClipperProfile
{
	uint64_t VisibilityCalls = 0;
	uint64_t VisibilityVisits = 0;
	uint64_t AddCalls = 0;
	uint64_t ContainmentVisits = 0;
	uint64_t OverlapVisits = 0;
	uint64_t MergeVisits = 0;
	uint64_t InsertVisits = 0;
};

extern HWClipperProfile ClipperProfile;
extern bool ClipperProfiling;

struct HWSceneTraversalProfile
{
	uint64_t Views = 0;
	uint64_t ClipPortalViews = 0;
	uint64_t ThreadedViews = 0;
	uint64_t Nodes = 0;
	uint64_t BoxChecks = 0;
	uint64_t BoxRejected = 0;
	uint64_t Subsectors = 0;
	uint64_t SubsectorRejected = 0;
	uint64_t SubsectorEntered = 0;
	uint64_t UniqueSectors = 0;
	uint64_t Lines = 0;
	uint64_t PortalRejected = 0;
	uint64_t BackfaceRejected = 0;
	uint64_t Minisegs = 0;
	uint64_t RangeRejected = 0;
	uint64_t EmptyTextureRejected = 0;
	uint64_t DuplicateLines = 0;
	uint64_t DisabledWalls = 0;
	uint64_t WallWork = 0;
	uint64_t PolyWallWork = 0;
	uint64_t ViewAreaChecks = 0;
	uint64_t FakeBackCalls = 0;
	uint64_t ClipChecks = 0;
	uint64_t RadarLines = 0;
	uint64_t DistanceChecks = 0;
	uint64_t DistanceCulled = 0;
	uint64_t MLMappedWrites = 0;
	uint64_t SubsectorDrawnWrites = 0;
	uint64_t SectorDrawnWrites = 0;
	uint64_t DitherCandidates = 0;
	uint64_t DitherTargetsSet = 0;
	uint64_t DitherTraceNS = 0;
	uint64_t PortalRegistrations = 0;
	uint64_t Jobs[6] = {};
};

struct HWSceneWorkerProfile
{
	uint64_t Runs = 0;
	uint64_t EmptyPolls = 0;
	uint64_t Jobs[6] = {};
};

enum EOwnedAdmissionReason : unsigned
{
	OAR_Admitted = 0,
	OAR_Culled,
	OAR_FakeSectorMismatch,
	OAR_NonOrdinarySector,
	OAR_SideLineMismatch,
	OAR_AttachedDecals,
	OAR_Lightmap,
	OAR_BlockFlags,
	OAR_VisualOrTransferredPortal,
	OAR_HorizonOrMirror,
	OAR_SkyPlane,
	OAR_MaskedTexture,
	OAR_TranslucentTexture,
	OAR_TwoSidedMidTexture,
	OAR_DegeneratePlane,
	OAR_Transdoor,
	OAR_OneSidedMissingMid,
	OAR_ThreeDMidtex,
	OAR_MissingTopBottom,
	OAR_RetainedRecipeUnavailable,
	OAR_ColdRecipe,
	OAR_ReasonCount
};

enum EGpuOwnerRejectReason : unsigned
{
	GOR_Unavailable = 0,
	GOR_Culled,
	GOR_Identity,
	GOR_PortalView,
	GOR_DynamicLight,
	GOR_SlopeOrSpecial,
	GOR_Sky,
	GOR_SkyFront,
	GOR_SkyBack,
	GOR_PlaneEffect,
	GOR_Texture,
	GOR_WallGlow,
	GOR_Degenerate,
	GOR_LevelMismatch,
	GOR_Other,
	GOR_ReasonCount
};

struct HWSceneSurfaceProfile
{
	uint64_t OwnedViews = 0;
	uint64_t OwnedAdmissionCalls = 0;
	uint64_t OwnedAdmissionNS = 0;
	uint64_t OwnedAdmissionReasons[OAR_ReasonCount] = {};
	uint64_t OwnedCandidates = 0;
	uint64_t OwnedSelections = 0;
	uint64_t OwnedEmptySelections = 0;
	uint64_t OwnedRejected = 0;
	uint64_t OwnedFallbackProcessCalls = 0;
	uint64_t OwnedPrepareNS = 0;
	uint64_t OwnedSelectionNS = 0;
	uint64_t OwnedFallbackNS = 0;
	uint64_t OwnedColdCaptures = 0;
	uint64_t OwnedVisibleAssemblyNS = 0;
	uint64_t OwnedOrderNS = 0;
	uint64_t OwnedSubmissionBytes = 0;
	uint64_t OwnedParameterReuses = 0;
	uint64_t OwnedDrawFallbacks = 0;
	uint64_t OwnedInlineReplayFallbacks = 0;
	uint64_t OwnedSkySelections = 0;
	uint64_t OwnedSkyRows = 0;
	uint64_t ProcessCalls = 0;
	uint64_t GpuPersistentRequests = 0;
	uint64_t GpuPersistentHits = 0;
	uint64_t GpuPersistentRebuilds = 0;
	uint64_t GpuPersistentResident = 0;
	uint64_t GpuPersistentEmpty = 0;
	uint64_t GpuOwnerRejects[GOR_ReasonCount] = {};
	uint64_t GpuOwnerAdmissionNS = 0;      // whole TryGpuPersistentWall call
	uint64_t GpuOwnerReplayNS = 0;         // warm-output block inside it (subset of admission)
	uint64_t GpuOwnerCaptureNS = 0;        // cold/invalidated owner capture
	uint64_t GpuOwnerCaptureRejected = 0;  // captures abandoned because a part was unsupported
	uint64_t GpuOwnerMultiPartCalls = 0;   // warm calls emitting >=2 parts (no double-count of calls)
	uint64_t GpuAppearanceRowNS = 0;       // indexed appearance-row construction (main only)
	uint64_t GpuCandidateBuildNS = 0;      // candidate/draw construction for GPU-driven walls (main only)
	uint64_t PreparedHits = 0;
	uint64_t PreparedRebuilds = 0;
	uint64_t PreparedFallbacks = 0;
	uint64_t PreparedVerified = 0;
	uint64_t PreparedValidationNS = 0;
	uint64_t PreparedLookupNS = 0;
	uint64_t PreparedReplayNS = 0;
	uint64_t PreparedAppearanceUpdates = 0;
	uint64_t PreparedAppearanceReuses = 0;
	uint64_t PreparedTextureReuses = 0;
	uint64_t RetainedSectorRequests = 0;
	uint64_t RetainedSectorRefreshes = 0;
	uint64_t RetainedAppearanceRequests = 0;
	uint64_t RetainedAppearanceRefreshes = 0;
	uint64_t RetainedGlowRequests = 0;
	uint64_t RetainedGlowRefreshes = 0;
	uint64_t RetainedPlaneRequests = 0;
	uint64_t RetainedPlaneRefreshes = 0;
	uint64_t RetainedTextureRequests = 0;
	uint64_t RetainedTextureRefreshes = 0;
	uint64_t RetainedSnapshotSectorShortcuts = 0;
	uint64_t RetainedSnapshotTextureShortcuts = 0;
	uint64_t RetainedFogChecks = 0;
	uint64_t RetainedSectorBytes = 0;
	uint64_t RetainedSectorHotBytes = 0;
	uint64_t RetainedSectorColdBytes = 0;
	uint64_t RetainedSectorLayout = 0;
	uint64_t RetainedTextureBytes = 0;
	uint64_t PreparedBuildNS = 0;
	uint64_t PreparedUploadBytes = 0;
	uint64_t RetainedFallbackReasons[7] = {};
	uint64_t RetainedWallQuads = 0;
	uint64_t RetainedFlatDraws = 0;
	uint64_t RetainedBatchCalls = 0;
	uint64_t RetainedTriangles = 0;
	uint64_t RetainedParameterBytes = 0;
	uint64_t RetainedParameterUploads = 0;
	uint64_t RetainedParameterReuses = 0;
	uint64_t RetainedParameterCrossFrameReuses = 0;
	uint64_t RetainedParameterVerified = 0;
	uint64_t RetainedContextSnapshots = 0;
	uint64_t RetainedContextShortcuts = 0;
	uint64_t RetainedContextChecks = 0;
	uint64_t RetainedContextFull = 0;
	uint64_t RetainedContextBytes = 0;
	uint64_t RetainedContextNS = 0;
	uint64_t RetainedBufferBytes = 0;
	uint64_t RetainedParameterCPUBytes = 0;
	uint64_t RetainedGPUFallbacks = 0;
	uint64_t RetainedStateApplies = 0;
	uint64_t RetainedStateReuses = 0;
	uint64_t RetainedStateEmptyBatch = 0;
	uint64_t RetainedStateBoundaries = 0;
	uint64_t RetainedVariantSwitches = 0;
	uint64_t RetainedVariantOnlyBreaks = 0;
	uint64_t RetainedMaterialChanges = 0;
	uint64_t RetainedBufferChanges = 0;
	uint64_t RetainedHelperBoundaries = 0;
	uint64_t RetainedOwnedTimerDraws = 0;
	uint64_t PreparedDrawPackets = 0;
	uint64_t PreparedDrawRuns = 0;
	uint64_t PreparedDrawFallbacks = 0;
	uint64_t PreparedDrawRejectedGeometry = 0;
	uint64_t PreparedDrawRejectedMask = 0;
	uint64_t PreparedDrawRejectedState = 0;
	uint64_t PreparedDrawRejectedMaterial = 0;
	uint64_t RetainedAppearanceBuilds = 0;
	uint64_t RetainedAppearanceReuses = 0;
	uint64_t RetainedAppearanceFallbacks = 0;
	uint64_t RetainedAppearanceVerified = 0;
	uint64_t RetainedAppearanceNS = 0;
	uint64_t RetainedLightingTotalNS = 0;
	uint64_t RetainedGatherNS = 0;
	uint64_t RetainedCollectNS = 0;
	uint64_t RetainedStateNS = 0;
	uint64_t RetainedIndexNS = 0;
	uint64_t RetainedLightingKeys = 0;
	uint64_t RetainedLightingDeferred = 0;
	uint64_t RetainedParameterDeferred = 0;
	uint64_t SortWallsNS = 0;
	uint64_t SortFlatsNS = 0;
	uint64_t OneSided = 0;
	uint64_t ProcessCallsWithWallSubmission = 0;
	uint64_t ProcessCallsWithoutWallSubmission = 0;
	uint64_t ProcessWallSubmissions = 0;
	uint64_t ProcessCallsWithOrdinaryOpaque = 0;
	uint64_t ProcessOrdinaryOpaqueParts = 0;
	uint64_t OutOrdinaryTop = 0;
	uint64_t OutOrdinaryBottom = 0;
	uint64_t OutOrdinaryM1S = 0;
	uint64_t OutM2S = 0;
	uint64_t CensusMidNativeSides = 0;
	uint64_t CensusMidNativeTop = 0;
	uint64_t CensusMidNativeBottom = 0;
	uint64_t CensusMidOnlySides = 0;
	uint64_t CensusProcessNS = 0;
	uint64_t CensusEmptyProcessNS = 0;
	uint64_t CensusMidNativeProcessNS = 0;
	uint64_t CensusEmptyEqualCandidate = 0;
	uint64_t CensusEmptyEqualCandidateNS = 0;
	uint64_t ProcessTwoSidedCalls = 0;
	uint64_t Polyobjects = 0;
	uint64_t VisualPortals = 0;
	uint64_t FakeSectors = 0;
	uint64_t Slopes = 0;
	uint64_t ThreeDFloors = 0;
	uint64_t HeightSectors = 0;
	uint64_t Submitted = 0;
	uint64_t Translucent = 0;
	uint64_t Masked = 0;
	uint64_t OpaqueRegular = 0;
	uint64_t WithDecals = 0;
	uint64_t DrawCalls = 0;
	uint64_t UniformRequests = 0;
	uint64_t UniformUploads = 0;
	uint64_t UniformReuses = 0;
	uint64_t GPUDraws = 0;
	uint64_t QuadBatchQuads = 0;
	uint64_t QuadBatchCalls = 0;
	uint64_t PipelineBinds = 0;
	uint64_t DescriptorBinds = 0;
	uint64_t PushWrites = 0;
	uint64_t BufferBinds = 0;
	uint64_t StreamWaits = 0;

	uint64_t VertexRequests = 0;
	uint64_t VertexReused = 0;
	uint64_t AllocatedVertices = 0;
	uint64_t GeneratedVertices = 0;
	uint64_t VertexBytes = 0;
	uint64_t SplitBuilds = 0;
	uint64_t CacheEligible = 0;
	uint64_t CacheRequests = 0;
	uint64_t CacheHits = 0;
	uint64_t CacheMisses = 0;
	uint64_t CacheFull = 0;
	uint64_t CacheVerified = 0;
	uint64_t CacheWrittenBytes = 0;
};

uint64_t GetRetainedWorldMemory();
extern bool SceneProfiling;
extern HWSceneTraversalProfile SceneTraversalProfile;
extern HWSceneWorkerProfile SceneWorkerProfile;
extern HWSceneSurfaceProfile SceneMainSurfaceProfile;
extern HWSceneSurfaceProfile SceneWorkerSurfaceProfile;
HWSceneSurfaceProfile &GetSceneSurfaceProfile();

class HWSceneScopeTimer
{
public:
	HWSceneScopeTimer(uint64_t& output) : Output(output), Enabled(SceneProfiling)
	{
		if (Enabled) Timer.ResetAndClock();
	}
	~HWSceneScopeTimer()
	{
		if (Enabled)
		{
			Timer.Unclock();
			Output += uint64_t(Timer.Time() * 1e9);
		}
	}

private:
	cycle_t Timer;
	uint64_t& Output;
	bool Enabled;
};

extern int iter_dlightf, iter_dlight, draw_dlight, draw_dlightf;
extern int rendered_lines,rendered_flats,rendered_sprites,rendered_decals,render_vertexsplit,render_texsplit;
extern int rendered_portals;

extern int vertexcount, flatvertices, flatprimitives;

void ResetProfilingData();
void CheckBench();
void  checkBenchActive();

extern int doBench;


#endif
