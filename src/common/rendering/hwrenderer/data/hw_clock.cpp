/*
** hw_clock.cpp
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


#include "c_console.h"
#include "c_dispatch.h"
#include "v_video.h"
#include "hw_clock.h"
#include "i_time.h"
#include "flatvertices.h"
#include "i_interface.h"
#include "printf.h"
#include <cstdio>
#include <vector>
#include <chrono>

glcycle_t RenderWall,SetupWall,ClipWall;
glcycle_t RenderFlat,SetupFlat;
glcycle_t RenderSprite,SetupSprite;
glcycle_t All, Finish, PortalAll, Bsp;
glcycle_t ProcessAll, PostProcess;
glcycle_t RenderAll;
glcycle_t Dirty;
glcycle_t drawcalls;
glcycle_t twoD, Flush3D;
glcycle_t MTWait, WTTotal;
int vertexcount, flatvertices, flatprimitives;

int rendered_lines,rendered_flats,rendered_sprites,render_vertexsplit,render_texsplit,rendered_decals, rendered_portals, rendered_commandbuffers;
int iter_dlightf, iter_dlight, draw_dlight, draw_dlightf;

CVAR(Bool, gl_profile_finish, false, 0)
CVAR(Bool, gl_profile_gpu, false, 0)
EXTERN_CVAR(Int, vid_maxfps)
EXTERN_CVAR(Bool, gl_wallcache)
EXTERN_CVAR(Bool, vk_drawownership)
EXTERN_CVAR(Bool, vk_retainedparams)
extern bool AppActive;

HWFinishProfile FinishProfile;
bool FinishProfileAvailable = false;
HWGPUProfile GPUProfile;
static HWGPUProfile CompletedGPUProfile;

struct HWGPUTraceRecord
{
	HWGPUProfile Profile;
	uint64_t EndNS;
	size_t Frame;
};

struct HWFinishTraceRecord
{
	HWFinishProfile Profile;
	uint64_t EndNS;
	uint64_t IntervalNS;
};

static constexpr size_t MaxFinishTraceFrames = 65536;
static std::vector<HWFinishTraceRecord> FinishTrace;
static std::vector<HWGPUTraceRecord> GPUTrace;
static bool FinishTraceActive = false;
static uint64_t FinishTracePreviousNS = 0;
static uint64_t FinishTraceDropped = 0;

bool IsFinishProfilingEnabled()
{
	return gl_profile_finish || FinishTraceActive;
}

bool IsGPUProfilingEnabled()
{
	return gl_profile_gpu && IsFinishProfilingEnabled();
}

void RecordFinishProfile(const uint64_t *ticks, int submits, int fences, uint64_t reuseNS, uint64_t reuseCount, bool fullscreen, bool vsync)
{
	FinishProfile = { ticks[6] - ticks[0], ticks[1] - ticks[0], ticks[2] - ticks[1],
		ticks[3] - ticks[2], ticks[4] - ticks[3], ticks[5] - ticks[4], ticks[6] - ticks[5],
		reuseNS, reuseCount, submits, fences, AppActive, fullscreen, vsync, int(vid_maxfps), bool(gl_wallcache) };
	FinishProfileAvailable = true;
	CompletedGPUProfile = GPUProfile;
	if (FinishTraceActive)
	{
		if (FinishTrace.size() < MaxFinishTraceFrames)
		{
			FinishTrace.push_back({ FinishProfile, ticks[6], FinishTracePreviousNS ? ticks[6] - FinishTracePreviousNS : 0 });
			if (GPUProfile.Enabled) GPUTrace.push_back({ GPUProfile, ticks[6], FinishTrace.size() - 1 });
		}
		else ++FinishTraceDropped;
		FinishTracePreviousNS = ticks[6];
	}
}

CCMD(finishtrace)
{
	if (argv.argc() == 2 && stricmp(argv[1], "start") == 0)
	{
		FinishTrace.clear();
		FinishTrace.reserve(MaxFinishTraceFrames);
		GPUTrace.clear();
		if (gl_profile_gpu) GPUTrace.reserve(MaxFinishTraceFrames);
		FinishTracePreviousNS = 0;
		FinishTraceDropped = 0;
		FinishTraceActive = true;
		uint64_t engineNS = I_nsTime();
		uint64_t counterNS = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
		Printf("UZDoom dev: finish trace recording. clock_engine_ns=%llu clock_counter_ns=%llu\n",
			static_cast<unsigned long long>(engineNS), static_cast<unsigned long long>(counterNS));
	}
	else if (argv.argc() == 2 && stricmp(argv[1], "stop") == 0)
	{
		if (!FinishTraceActive)
		{
			Printf("No finish trace is recording.\n");
			return;
		}
		FinishTraceActive = false;
		FILE *file = fopen("finish-trace.csv", "wt");
		if (!file)
		{
			Printf("Could not save finish-trace.csv.\n");
			return;
		}
		fprintf(file, "frame,end_ns,interval_ns,total_ns,acquire_ns,submit_ns,limit_ns,present_ns,fence_ns,cleanup_ns,reuse_wait_ns,reuse_wait_count,submits,fences,active,fullscreen,vsync,maxfps,wallcache\n");
		for (size_t i = 0; i < FinishTrace.size(); ++i)
		{
			const auto &record = FinishTrace[i];
			const auto &profile = record.Profile;
			fprintf(file, "%zu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%d,%d,%d,%d,%d,%d,%d\n", i,
				static_cast<unsigned long long>(record.EndNS), static_cast<unsigned long long>(record.IntervalNS),
				static_cast<unsigned long long>(profile.TotalNS), static_cast<unsigned long long>(profile.AcquireNS),
				static_cast<unsigned long long>(profile.SubmitNS), static_cast<unsigned long long>(profile.LimitNS),
				static_cast<unsigned long long>(profile.PresentNS), static_cast<unsigned long long>(profile.FenceNS),
				static_cast<unsigned long long>(profile.CleanupNS), static_cast<unsigned long long>(profile.ReuseWaitNS),
				static_cast<unsigned long long>(profile.ReuseWaitCount), profile.Submits, profile.Fences,
				int(profile.Active), int(profile.Fullscreen), int(profile.VSync), profile.MaxFPS, int(profile.WallCache));
		}
		bool failed = ferror(file) != 0;
		failed = fclose(file) != 0 || failed;
		Printf("UZDoom dev: finish trace %s: frames=%zu dropped=%llu.\n", failed ? "save failed" : "saved", FinishTrace.size(), static_cast<unsigned long long>(FinishTraceDropped));
		std::vector<HWFinishTraceRecord>().swap(FinishTrace);
		file = fopen("gpu-trace.csv", "wt");
		if (!file)
		{
			Printf("Could not save gpu-trace.csv.\n");
			return;
		}
		fprintf(file, "frame,end_ns,available,dropped,read_ns,category,duration_ns,samples\n");
		for (const auto &record : GPUTrace)
		{
			const auto &profile = record.Profile;
			for (int category = 0; category < GPUCategoryCount; ++category)
			{
				fprintf(file, "%zu,%llu,%d,%u,%llu,%d,%llu,%u\n", record.Frame,
					static_cast<unsigned long long>(record.EndNS), int(profile.Available), profile.Dropped,
					static_cast<unsigned long long>(profile.ReadNS), category,
					static_cast<unsigned long long>(profile.DurationNS[category]), profile.Samples[category]);
			}
		}
		failed = ferror(file) != 0;
		failed = fclose(file) != 0 || failed;
		Printf("UZDoom dev: GPU trace %s: frames=%zu.\n", failed ? "save failed" : "saved", GPUTrace.size());
		std::vector<HWGPUTraceRecord>().swap(GPUTrace);
	}
	else Printf("Usage: finishtrace start|stop\n");
}

CVAR(Bool, gl_profile_clipper, false, 0)
HWClipperProfile ClipperProfile;
bool ClipperProfiling = false;

CVAR(Bool, gl_profile_scene, false, 0)
CVAR(Bool, gl_profile_force, false, 0)
bool SceneProfiling = false;
HWSceneTraversalProfile SceneTraversalProfile;
alignas(64) HWSceneWorkerProfile SceneWorkerProfile;
alignas(64) HWSceneSurfaceProfile SceneMainSurfaceProfile;
alignas(64) HWSceneSurfaceProfile SceneWorkerSurfaceProfile;

CCMD(profiler)
{
	bool enabled = true;
	if (argv.argc() == 2 && stricmp(argv[1], "off") == 0) enabled = false;
	else if (argv.argc() > 2 || (argv.argc() == 2 && stricmp(argv[1], "on") != 0))
	{
		Printf("Usage: profiler [on|off]\n");
		return;
	}
	gl_profile_scene = enabled;
	Printf("UZDoom dev: scene profiling %s (scene counters v1).\n", enabled ? "enabled" : "disabled");
	if (enabled) Printf("Use bench to save a snapshot, or run the MAP08 capture script.\n");
}

void ResetProfilingData()
{
	ClipperProfiling = gl_profile_clipper && glcycle_t::active;
	ClipperProfile = {};
	SceneProfiling = gl_profile_scene && glcycle_t::active;
	SceneTraversalProfile = {};
	SceneWorkerProfile = {};
	SceneMainSurfaceProfile = {};
	SceneWorkerSurfaceProfile = {};
	All.Reset();
	All.Clock();
	Bsp.Reset();
	PortalAll.Reset();
	RenderAll.Reset();
	ProcessAll.Reset();
	PostProcess.Reset();
	RenderWall.Reset();
	SetupWall.Reset();
	ClipWall.Reset();
	RenderFlat.Reset();
	SetupFlat.Reset();
	RenderSprite.Reset();
	SetupSprite.Reset();
	drawcalls.Reset();
	MTWait.Reset();
	WTTotal.Reset();

	flatvertices=flatprimitives=vertexcount=0;
	render_texsplit=render_vertexsplit=rendered_lines=rendered_flats=rendered_sprites=rendered_decals=rendered_portals = 0;
}

//-----------------------------------------------------------------------------
//
// Rendering statistics
//
//-----------------------------------------------------------------------------

static void AppendSceneProfile(FString &str)
{
	auto append = [&](const char *name, uint64_t value)
	{
		str.AppendFormat("%s=%llu, ", name, static_cast<unsigned long long>(value));
	};
	const auto &traversal = SceneTraversalProfile;
	str << "SceneTraversal: ";
	append("views", traversal.Views);
	append("clip_portal_views", traversal.ClipPortalViews);
	append("threaded_views", traversal.ThreadedViews);
	append("nodes", traversal.Nodes);
	append("box_checks", traversal.BoxChecks);
	append("box_rejected", traversal.BoxRejected);
	append("subsectors", traversal.Subsectors);
	append("subsector_rejected", traversal.SubsectorRejected);
	append("subsector_entered", traversal.SubsectorEntered);
	append("unique_sectors", traversal.UniqueSectors);
	append("lines", traversal.Lines);
	append("portal_rejected", traversal.PortalRejected);
	append("backface_rejected", traversal.BackfaceRejected);
	append("minisegs", traversal.Minisegs);
	append("range_rejected", traversal.RangeRejected);
	append("empty_texture_rejected", traversal.EmptyTextureRejected);
	append("duplicate_lines", traversal.DuplicateLines);
	append("disabled_walls", traversal.DisabledWalls);
	append("wall_work", traversal.WallWork);
	append("poly_wall_work", traversal.PolyWallWork);
	append("view_area_checks", traversal.ViewAreaChecks);
	append("fake_back_calls", traversal.FakeBackCalls);
	append("clip_checks", traversal.ClipChecks);
	append("radar_lines", traversal.RadarLines);
	append("distance_checks", traversal.DistanceChecks);
	append("distance_culled", traversal.DistanceCulled);
	append("ml_mapped_writes", traversal.MLMappedWrites);
	append("subsector_drawn_writes", traversal.SubsectorDrawnWrites);
	append("sector_drawn_writes", traversal.SectorDrawnWrites);
	append("dither_candidates", traversal.DitherCandidates);
	append("dither_targets_set", traversal.DitherTargetsSet);
	append("dither_trace_ns", traversal.DitherTraceNS);
	append("portal_registrations", traversal.PortalRegistrations);
	str << "\nSceneQueue: ";
	append("worker_runs", SceneWorkerProfile.Runs);
	append("empty_polls", SceneWorkerProfile.EmptyPolls);
	const char *jobNames[] = { "flat", "wall", "sprite", "particle", "portal", "terminate" };
	for (unsigned i = 0; i < 6; ++i)
	{
		FString name;
		name.Format("enqueued_%s", jobNames[i]);
		append(name.GetChars(), traversal.Jobs[i]);
		name.Format("consumed_%s", jobNames[i]);
		append(name.GetChars(), SceneWorkerProfile.Jobs[i]);
	}
	const auto &main = SceneMainSurfaceProfile;
	const auto &worker = SceneWorkerSurfaceProfile;
	// [Karto] Aggregate only after traversal workers join so snapshots cannot race their writes.
	str << "\nSceneSurface: ";
	append("drawownership", bool(vk_drawownership));
	append("owned_pipeline_version", 2);
	append("owned_views", main.OwnedViews);
	append("owned_admission_calls", main.OwnedAdmissionCalls + worker.OwnedAdmissionCalls);
	append("owned_admission_ns", main.OwnedAdmissionNS + worker.OwnedAdmissionNS);
	append("owned_producer_admission_calls", main.OwnedAdmissionCalls);
	append("owned_producer_admission_ns", main.OwnedAdmissionNS);
	append("owned_worker_admission_calls", worker.OwnedAdmissionCalls);
	append("owned_worker_admission_ns", worker.OwnedAdmissionNS);
	append("owned_admission_version", 2);
	const char* admissionNames[] = {
		"admitted", "culled", "fake_sector_mismatch", "non_ordinary_sector", "side_line_mismatch",
		"attached_decals", "lightmap", "block_flags", "visual_or_transferred_portal", "horizon_or_mirror",
		"sky_plane", "masked_texture", "translucent_texture", "two_sided_mid_texture", "degenerate_plane",
		"transdoor", "one_sided_missing_mid", "three_d_midtex", "missing_top_bottom", "retained_recipe_unavailable", "cold_recipe"
	};
	static_assert(sizeof(admissionNames) / sizeof(admissionNames[0]) == OAR_ReasonCount);
	for (unsigned i = 0; i < OAR_ReasonCount; ++i)
	{
		FString name;
		name.Format("owned_admission_reason_%s", admissionNames[i]);
		append(name.GetChars(), main.OwnedAdmissionReasons[i] + worker.OwnedAdmissionReasons[i]);
	}
	append("owned_candidates", main.OwnedCandidates + worker.OwnedCandidates);
	append("owned_selections", main.OwnedSelections + worker.OwnedSelections);
	append("owned_empty_selections", main.OwnedEmptySelections + worker.OwnedEmptySelections);
	append("owned_rejected", main.OwnedRejected + worker.OwnedRejected);
	append("owned_fallback_process_calls", main.OwnedFallbackProcessCalls + worker.OwnedFallbackProcessCalls);
	append("owned_worker_fallback_process_calls", worker.OwnedFallbackProcessCalls);
	append("owned_prepare_ns", main.OwnedPrepareNS + worker.OwnedPrepareNS);
	append("owned_selection_ns", main.OwnedSelectionNS + worker.OwnedSelectionNS);
	append("owned_fallback_ns", main.OwnedFallbackNS + worker.OwnedFallbackNS);
	append("owned_cold_captures", main.OwnedColdCaptures + worker.OwnedColdCaptures);
	append("owned_visible_assembly_ns", main.OwnedVisibleAssemblyNS);
	append("owned_order_ns", main.OwnedOrderNS);
	append("owned_submission_bytes", main.OwnedSubmissionBytes);
	append("owned_parameter_reuses", main.OwnedParameterReuses);
	append("owned_draw_fallbacks", main.OwnedDrawFallbacks);
	append("owned_inline_replay_fallbacks", main.OwnedInlineReplayFallbacks + worker.OwnedInlineReplayFallbacks);
	append("owned_sky_selections", main.OwnedSkySelections + worker.OwnedSkySelections);
	append("owned_sky_rows", main.OwnedSkyRows + worker.OwnedSkyRows);
	append("process_calls", main.ProcessCalls + worker.ProcessCalls);
	append("prepared_hits", main.PreparedHits + worker.PreparedHits);
	append("prepared_rebuilds", main.PreparedRebuilds + worker.PreparedRebuilds);
	append("prepared_fallbacks", main.PreparedFallbacks + worker.PreparedFallbacks);
	append("prepared_verified", main.PreparedVerified + worker.PreparedVerified);
	append("prepared_validation_ns", main.PreparedValidationNS + worker.PreparedValidationNS);
	append("prepared_replay_ns", main.PreparedReplayNS + worker.PreparedReplayNS);
	append("prepared_appearance_updates", main.PreparedAppearanceUpdates + worker.PreparedAppearanceUpdates);
	append("prepared_appearance_reuses", main.PreparedAppearanceReuses + worker.PreparedAppearanceReuses);
	append("prepared_texture_reuses", main.PreparedTextureReuses + worker.PreparedTextureReuses);
	append("retained_sector_requests", main.RetainedSectorRequests + worker.RetainedSectorRequests);
	append("retained_sector_refreshes", main.RetainedSectorRefreshes + worker.RetainedSectorRefreshes);
	append("retained_appearance_requests", main.RetainedAppearanceRequests + worker.RetainedAppearanceRequests);
	append("retained_appearance_refreshes", main.RetainedAppearanceRefreshes + worker.RetainedAppearanceRefreshes);
	append("retained_glow_requests", main.RetainedGlowRequests + worker.RetainedGlowRequests);
	append("retained_glow_refreshes", main.RetainedGlowRefreshes + worker.RetainedGlowRefreshes);
	append("retained_plane_requests", main.RetainedPlaneRequests + worker.RetainedPlaneRequests);
	append("retained_plane_refreshes", main.RetainedPlaneRefreshes + worker.RetainedPlaneRefreshes);
	append("retained_texture_requests", main.RetainedTextureRequests + worker.RetainedTextureRequests);
	append("retained_texture_refreshes", main.RetainedTextureRefreshes + worker.RetainedTextureRefreshes);
	append("retained_snapshot_sector_shortcuts", main.RetainedSnapshotSectorShortcuts + worker.RetainedSnapshotSectorShortcuts);
	append("retained_snapshot_texture_shortcuts", main.RetainedSnapshotTextureShortcuts + worker.RetainedSnapshotTextureShortcuts);
	append("retained_fog_checks", main.RetainedFogChecks + worker.RetainedFogChecks);
	append("retained_sector_bytes", std::max(main.RetainedSectorBytes, worker.RetainedSectorBytes));
	append("retained_sector_hot_bytes", std::max(main.RetainedSectorHotBytes, worker.RetainedSectorHotBytes));
	append("retained_sector_cold_bytes", std::max(main.RetainedSectorColdBytes, worker.RetainedSectorColdBytes));
	append("retained_sector_layout", std::max(main.RetainedSectorLayout, worker.RetainedSectorLayout));
	append("retained_texture_bytes", std::max(main.RetainedTextureBytes, worker.RetainedTextureBytes));
	append("prepared_build_ns", main.PreparedBuildNS + worker.PreparedBuildNS);
	append("prepared_upload_bytes", main.PreparedUploadBytes + worker.PreparedUploadBytes);
	append("retained_wall_quads", main.RetainedWallQuads);
	append("retained_flat_draws", main.RetainedFlatDraws);
	append("retained_batch_calls", main.RetainedBatchCalls);
	append("retained_triangles", main.RetainedTriangles);
	append("retained_parameter_bytes", main.RetainedParameterBytes);
	append("retained_parameter_uploads", main.RetainedParameterUploads);
	append("retained_parameter_reuses", main.RetainedParameterReuses);
	append("retained_parameter_cross_frame_reuses", main.RetainedParameterCrossFrameReuses);
	append("retained_parameter_verified", main.RetainedParameterVerified);
	append("retained_context_snapshots", main.RetainedContextSnapshots);
	append("retained_context_shortcuts", main.RetainedContextShortcuts);
	append("retained_context_checks", main.RetainedContextChecks);
	append("retained_context_full", main.RetainedContextFull);
	append("retained_context_bytes", main.RetainedContextBytes);
	append("retained_context_ns", main.RetainedContextNS);
	append("retained_buffer_bytes", main.RetainedBufferBytes);
	append("retained_parameter_cpu_bytes", main.RetainedParameterCPUBytes);
	append("retained_flat_setup_ns", screen->mVertexData->RetainedFlatSetupNS);
	append("retained_flat_metadata_bytes", screen->mVertexData->RetainedFlatMetadataBytes);
	append("retained_gpu_fallbacks", main.RetainedGPUFallbacks);
	append("retained_state_applies", main.RetainedStateApplies);
	append("retained_state_reuses", main.RetainedStateReuses);
	append("retained_boundary_version", 1);
	append("retained_params_enabled", bool(vk_retainedparams));
	append("retained_state_empty_batch", main.RetainedStateEmptyBatch);
	append("retained_state_boundaries", main.RetainedStateBoundaries);
	append("retained_variant_switches", main.RetainedVariantSwitches);
	append("retained_variant_only_breaks", main.RetainedVariantOnlyBreaks);
	append("retained_material_changes", main.RetainedMaterialChanges);
	append("retained_buffer_changes", main.RetainedBufferChanges);
	append("retained_helper_boundaries", main.RetainedHelperBoundaries);
	append("retained_owned_timer_draws", main.RetainedOwnedTimerDraws);
	append("prepared_draw_packets", main.PreparedDrawPackets);
	append("prepared_draw_runs", main.PreparedDrawRuns);
	append("prepared_draw_fallbacks", main.PreparedDrawFallbacks);
	append("prepared_draw_rejected_geometry", main.PreparedDrawRejectedGeometry);
	append("prepared_draw_rejected_mask", main.PreparedDrawRejectedMask);
	append("prepared_draw_rejected_state", main.PreparedDrawRejectedState);
	append("prepared_draw_rejected_material", main.PreparedDrawRejectedMaterial);
	append("retained_appearance_builds", main.RetainedAppearanceBuilds);
	append("retained_appearance_reuses", main.RetainedAppearanceReuses);
	append("retained_appearance_fallbacks", main.RetainedAppearanceFallbacks);
	append("retained_appearance_verified", main.RetainedAppearanceVerified);
	append("retained_appearance_ns", main.RetainedAppearanceNS);
	append("retained_lighting_total_ns", main.RetainedLightingTotalNS);
	append("retained_gather_ns", main.RetainedGatherNS);
	append("retained_collect_ns", main.RetainedCollectNS);
	append("retained_state_ns", main.RetainedStateNS);
	append("retained_index_ns", main.RetainedIndexNS);
	append("retained_lighting_keys", main.RetainedLightingKeys);
	append("retained_lighting_deferred", main.RetainedLightingDeferred);
	append("retained_parameter_deferred", main.RetainedParameterDeferred);
	append("sort_walls_ns", main.SortWallsNS);
	append("sort_flats_ns", main.SortFlatsNS);
	append("prepared_lookup_ns", main.PreparedLookupNS + worker.PreparedLookupNS);
	append("retained_cpu_bytes", GetRetainedWorldMemory());
	const char* fallbackNames[] = { "no_dispatcher", "view", "side", "fog", "sector", "material", "capacity" };
	for (unsigned i = 0; i < 7; ++i)
	{
		FString name;
		name.Format("retained_fallback_%s", fallbackNames[i]);
		append(name.GetChars(), main.RetainedFallbackReasons[i] + worker.RetainedFallbackReasons[i]);
	}
	append("worker_process_calls", worker.ProcessCalls);
	append("one_sided", main.OneSided + worker.OneSided);
	append("process_calls_with_wall_submission", main.ProcessCallsWithWallSubmission + worker.ProcessCallsWithWallSubmission);
	append("process_calls_without_wall_submission", main.ProcessCallsWithoutWallSubmission + worker.ProcessCallsWithoutWallSubmission);
	append("process_wall_submissions", main.ProcessWallSubmissions + worker.ProcessWallSubmissions);
	append("process_calls_with_ordinary_opaque", main.ProcessCallsWithOrdinaryOpaque + worker.ProcessCallsWithOrdinaryOpaque);
	append("process_ordinary_opaque_parts", main.ProcessOrdinaryOpaqueParts + worker.ProcessOrdinaryOpaqueParts);
	append("process_two_sided_calls", main.ProcessTwoSidedCalls + worker.ProcessTwoSidedCalls);
	append("out_ordinary_top", main.OutOrdinaryTop + worker.OutOrdinaryTop);
	append("out_ordinary_bottom", main.OutOrdinaryBottom + worker.OutOrdinaryBottom);
	append("out_ordinary_m1s", main.OutOrdinaryM1S + worker.OutOrdinaryM1S);
	append("out_m2s", main.OutM2S + worker.OutM2S);
	append("census_mid_native_sides", main.CensusMidNativeSides + worker.CensusMidNativeSides);
	append("census_mid_native_top", main.CensusMidNativeTop + worker.CensusMidNativeTop);
	append("census_mid_native_bottom", main.CensusMidNativeBottom + worker.CensusMidNativeBottom);
	append("census_mid_only_sides", main.CensusMidOnlySides + worker.CensusMidOnlySides);
	append("census_process_ns", main.CensusProcessNS + worker.CensusProcessNS);
	append("census_empty_process_ns", main.CensusEmptyProcessNS + worker.CensusEmptyProcessNS);
	append("census_mid_native_process_ns", main.CensusMidNativeProcessNS + worker.CensusMidNativeProcessNS);
	append("census_empty_equal_candidate", main.CensusEmptyEqualCandidate + worker.CensusEmptyEqualCandidate);
	append("census_empty_equal_candidate_ns", main.CensusEmptyEqualCandidateNS + worker.CensusEmptyEqualCandidateNS);
	append("polyobjects", main.Polyobjects + worker.Polyobjects);
	append("visual_portals", main.VisualPortals + worker.VisualPortals);
	append("fake_sectors", main.FakeSectors + worker.FakeSectors);
	append("slopes", main.Slopes + worker.Slopes);
	append("three_d_floors", main.ThreeDFloors + worker.ThreeDFloors);
	append("height_sectors", main.HeightSectors + worker.HeightSectors);
	append("submitted", main.Submitted + worker.Submitted);
	append("translucent", main.Translucent + worker.Translucent);
	append("masked", main.Masked + worker.Masked);
	append("opaque_regular", main.OpaqueRegular + worker.OpaqueRegular);
	append("with_decals", main.WithDecals + worker.WithDecals);
	append("draw_calls", main.DrawCalls + worker.DrawCalls);
	append("uniform_requests", main.UniformRequests + worker.UniformRequests);
	append("uniform_uploads", main.UniformUploads + worker.UniformUploads);
	append("uniform_reuses", main.UniformReuses + worker.UniformReuses);
	append("gpu_draws", main.GPUDraws + worker.GPUDraws);
	append("quad_batch_quads", main.QuadBatchQuads + worker.QuadBatchQuads);
	append("quad_batch_calls", main.QuadBatchCalls + worker.QuadBatchCalls);
	append("pipeline_binds", main.PipelineBinds + worker.PipelineBinds);
	append("descriptor_binds", main.DescriptorBinds + worker.DescriptorBinds);
	append("push_writes", main.PushWrites + worker.PushWrites);
	append("buffer_binds", main.BufferBinds + worker.BufferBinds);
	append("stream_waits", main.StreamWaits + worker.StreamWaits);
	append("gpu_persistent_requests", main.GpuPersistentRequests + worker.GpuPersistentRequests);
	append("gpu_persistent_hits", main.GpuPersistentHits + worker.GpuPersistentHits);
	append("gpu_persistent_rebuilds", main.GpuPersistentRebuilds + worker.GpuPersistentRebuilds);
	append("gpu_persistent_resident", main.GpuPersistentResident + worker.GpuPersistentResident);
	append("gpu_persistent_empty", main.GpuPersistentEmpty + worker.GpuPersistentEmpty);
	const char* gpuOwnerRejectNames[] = {
		"unavailable", "culled", "identity", "portal_view", "dynamic_light", "slope_or_special",
		"sky", "sky_front", "sky_back", "plane_effect", "texture", "wall_glow", "degenerate", "level_mismatch", "other"
	};
	static_assert(sizeof(gpuOwnerRejectNames) / sizeof(gpuOwnerRejectNames[0]) == GOR_ReasonCount);
	for (unsigned i = 0; i < GOR_ReasonCount; ++i)
	{
		FString name;
		name.Format("gpu_owner_reject_%s", gpuOwnerRejectNames[i]);
		append(name.GetChars(), main.GpuOwnerRejects[i] + worker.GpuOwnerRejects[i]);
	}
	append("gpu_owner_admission_ns", main.GpuOwnerAdmissionNS + worker.GpuOwnerAdmissionNS);
	append("gpu_owner_replay_ns", main.GpuOwnerReplayNS + worker.GpuOwnerReplayNS);
	append("gpu_owner_capture_ns", main.GpuOwnerCaptureNS + worker.GpuOwnerCaptureNS);
	append("gpu_owner_capture_rejected", main.GpuOwnerCaptureRejected + worker.GpuOwnerCaptureRejected);
	append("gpu_owner_multipart_calls", main.GpuOwnerMultiPartCalls + worker.GpuOwnerMultiPartCalls);
	append("gpu_appearance_row_ns", main.GpuAppearanceRowNS);
	append("gpu_candidate_build_ns", main.GpuCandidateBuildNS);
	append("vertex_requests", main.VertexRequests + worker.VertexRequests);
	append("vertex_reused", main.VertexReused + worker.VertexReused);
	append("allocated_vertices", main.AllocatedVertices + worker.AllocatedVertices);
	append("generated_vertices", main.GeneratedVertices + worker.GeneratedVertices);
	append("vertex_bytes", main.VertexBytes + worker.VertexBytes);
	append("split_builds", main.SplitBuilds + worker.SplitBuilds);
	append("cache_eligible", main.CacheEligible + worker.CacheEligible);
	append("cache_requests", main.CacheRequests + worker.CacheRequests);
	append("cache_hits", main.CacheHits + worker.CacheHits);
	append("cache_misses", main.CacheMisses + worker.CacheMisses);
	append("cache_full", main.CacheFull + worker.CacheFull);
	append("cache_verified", main.CacheVerified + worker.CacheVerified);
	append("cache_written_bytes", main.CacheWrittenBytes + worker.CacheWrittenBytes);
	str << "\n";
}

static void AppendRenderTimes(FString &str)
{
	double setupwall = SetupWall.TimeMS();
	double clipwall = ClipWall.TimeMS();
	double bsp = Bsp.TimeMS() - ClipWall.TimeMS();

	str.AppendFormat("BSP = %2.3f, Clip=%2.3f\n"
		"W: Render=%2.3f, Setup=%2.3f\n"
		"F: Render=%2.3f, Setup=%2.3f\n"
		"S: Render=%2.3f, Setup=%2.3f\n"
		"2D: %2.3f Finish3D: %2.3f\n"
		"Main thread total=%2.3f, Main thread waiting=%2.3f Worker thread total=%2.3f, Worker thread waiting=%2.3f\n"
		"All=%2.3f, Render=%2.3f, Setup=%2.3f, Portal=%2.3f, Drawcalls=%2.3f, Postprocess=%2.3f, Finish=%2.3f\n",
		bsp, clipwall,
		RenderWall.TimeMS(), setupwall,
		RenderFlat.TimeMS(), SetupFlat.TimeMS(),
		RenderSprite.TimeMS(), SetupSprite.TimeMS(),
		twoD.TimeMS(), Flush3D.TimeMS() - twoD.TimeMS(),
		MTWait.TimeMS() + Bsp.TimeMS(), MTWait.TimeMS(), WTTotal.TimeMS(), WTTotal.TimeMS() - setupwall - SetupFlat.TimeMS() - SetupSprite.TimeMS(),
		All.TimeMS() + Finish.TimeMS(), RenderAll.TimeMS(),	ProcessAll.TimeMS(), PortalAll.TimeMS(), drawcalls.TimeMS(), PostProcess.TimeMS(), Finish.TimeMS());

	{
		double gpu = 0.0;
		if (CompletedGPUProfile.Enabled && CompletedGPUProfile.Available) gpu = CompletedGPUProfile.DurationNS[GPUCommands] / 1e6;
		str.AppendFormat("Renaudit: CPU=%2.3f, GPU=%2.3f, CULL=%2.3f, PROCESS=%2.3f, DRAW=%2.3f, Finish=%2.3f, gpu_draws=%llu, visible=%llu\n",
			All.TimeMS() + Finish.TimeMS(), gpu, Bsp.TimeMS(), ProcessAll.TimeMS(), RenderAll.TimeMS(), Finish.TimeMS(),
			static_cast<unsigned long long>(SceneMainSurfaceProfile.GPUDraws + SceneWorkerSurfaceProfile.GPUDraws),
			static_cast<unsigned long long>(SceneMainSurfaceProfile.OpaqueRegular + SceneWorkerSurfaceProfile.OpaqueRegular));
	}

	if (ClipperProfiling)
	{
		str.AppendFormat("Clipper: visibility_calls=%llu, visibility_visits=%llu, add_calls=%llu, "
			"containment_visits=%llu, overlap_visits=%llu, merge_visits=%llu, insert_visits=%llu\n",
			static_cast<unsigned long long>(ClipperProfile.VisibilityCalls),
			static_cast<unsigned long long>(ClipperProfile.VisibilityVisits),
			static_cast<unsigned long long>(ClipperProfile.AddCalls),
			static_cast<unsigned long long>(ClipperProfile.ContainmentVisits),
			static_cast<unsigned long long>(ClipperProfile.OverlapVisits),
			static_cast<unsigned long long>(ClipperProfile.MergeVisits),
			static_cast<unsigned long long>(ClipperProfile.InsertVisits));
	}
	if (SceneProfiling) AppendSceneProfile(str);
	if (FinishProfileAvailable && IsFinishProfilingEnabled())
	{
		const auto &profile = FinishProfile;
		str.AppendFormat("FrameFinish: total=%2.6f, acquire=%2.6f, submit=%2.6f, limit=%2.6f, present=%2.6f, fence=%2.6f, cleanup=%2.6f, "
			"reuse_wait=%2.6f, reuse_count=%llu, submits=%d, fences=%d, active=%d, fullscreen=%d, vsync=%d, maxfps=%d, wallcache=%d\n",
			profile.TotalNS / 1e6, profile.AcquireNS / 1e6, profile.SubmitNS / 1e6, profile.LimitNS / 1e6,
			profile.PresentNS / 1e6, profile.FenceNS / 1e6, profile.CleanupNS / 1e6, profile.ReuseWaitNS / 1e6,
			static_cast<unsigned long long>(profile.ReuseWaitCount), profile.Submits, profile.Fences,
			int(profile.Active), int(profile.Fullscreen), int(profile.VSync), profile.MaxFPS, int(profile.WallCache));
		if (CompletedGPUProfile.Enabled)
		{
			str.AppendFormat("FrameGPU: available=%d, dropped=%u, commands=%2.6f, span=%2.6f, read=%2.6f, batches=%u\n",
				int(CompletedGPUProfile.Available), CompletedGPUProfile.Dropped, CompletedGPUProfile.DurationNS[GPUCommands] / 1e6,
				CompletedGPUProfile.DurationNS[GPUFrameSpan] / 1e6, CompletedGPUProfile.ReadNS / 1e6, CompletedGPUProfile.Samples[GPUCommands]);
		}
	}
}

static void AppendRenderStats(FString &out)
{
	out.AppendFormat("Walls: %d (%d splits, %d t-splits, %d vertices)\n"
		"Flats: %d (%d primitives, %d vertices)\n"
		"Sprites: %d, Decals=%d, Portals: %d, Command buffers: %d\n",
		rendered_lines, render_vertexsplit, render_texsplit, vertexcount, rendered_flats, flatprimitives, flatvertices, rendered_sprites,rendered_decals, rendered_portals, rendered_commandbuffers );
}

static void AppendLightStats(FString &out)
{
	out.AppendFormat("DLight - Walls: %d processed, %d rendered - Flats: %d processed, %d rendered\n",
		iter_dlight, draw_dlight, iter_dlightf, draw_dlightf );
}

ADD_STAT_ONOFF(rendertimes)
{
	static FString buff;
	static int64_t lasttime=0;
	int64_t t=I_msTime();
	if (t-lasttime>1000)
	{
		buff.Truncate(0);
		AppendRenderTimes(buff);
		lasttime=t;
	}
	return buff;
}

STAT_ON(rendertimes)
{
	doBench++;
}

STAT_OFF(rendertimes)
{
	doBench--;
}

ADD_STAT(renderstats)
{
	FString out;
	AppendRenderStats(out);
	return out;
}

ADD_STAT(lightstats)
{
	FString out;
	AppendLightStats(out);
	return out;
}

static int printstats;
static bool switchfps;
static bool benchWarmingUp;
static uint64_t waitstart;
EXTERN_CVAR(Bool, vid_fps)

void CheckBench()
{
	if (!printstats) return;
	if (ConsoleState != c_up)
	{
		benchWarmingUp = false;
		return;
	}
	if (!benchWarmingUp)
	{
		waitstart = I_msTime();
		benchWarmingUp = true;
	}
	if (I_msTime() - waitstart < 5000) return;

	FString compose;

	if (sysCallbacks.GetLocationDescription) compose = sysCallbacks.GetLocationDescription();

	AppendRenderStats(compose);
	AppendRenderTimes(compose);
	AppendLightStats(compose);
	compose << "\n\n\n";

	FILE *f = fopen("benchmarks.txt", "at");
	if (f != NULL)
	{
		fputs(compose.GetChars(), f);
		fclose(f);
	}
	Printf("Benchmark info saved\n");
	if (switchfps) vid_fps = false;
	printstats = false;
}

CCMD(bench)
{
	if (!printstats) switchfps = false;
	printstats = true;
	benchWarmingUp = false;
	if (vid_fps == 0)
	{
		vid_fps = 1;
		switchfps = true;
	}
	C_HideConsole ();
}

CCMD(scenestats)
{
	FString compose;
	Printf("PROFILE scene=%d force=%d sceneact=%d active=%d\n", int(gl_profile_scene), int(gl_profile_force), int(SceneProfiling), int(glcycle_t::active));
	if (sysCallbacks.GetLocationDescription) compose = sysCallbacks.GetLocationDescription();
	AppendRenderStats(compose);
	AppendRenderTimes(compose);
	Printf("%s\n", compose.GetChars());
}

bool glcycle_t::active = false;

int doBench = 0;

void  checkBenchActive()
{
	glcycle_t::active = (doBench || printstats || gl_profile_force);
}
