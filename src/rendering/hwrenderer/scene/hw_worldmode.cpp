// [Karto] Replacement world mode: producers and draw code run once per level with draw capture on; frames then replay
// the captured world from resident buffers with a CPU cluster frustum cull.

#include "g_levellocals.h"
#include "texturemanager.h"
#include "hwrenderer/scene/hw_drawstructs.h"
#include "hwrenderer/scene/hw_drawinfo.h"
#include "hwrenderer/scene/hw_fakeflat.h"
#include "hw_renderstate.h"
#include "hw_walldispatcher.h"
#include "flatvertices.h"
#include "v_video.h"
#include "r_vanillatrans.h"
#include "i_time.h"
#include "printf.h"
#include "c_dispatch.h"
#include "c_cvars.h"
#include "hw_clock.h"
#include "p_effect.h"
#include "hw_vertexbuilder.h"
#include "animations.h"
#include "hw_material.h"
#include "p_lnspec.h"
#include "hw_lighting.h"
#include "hw_portal.h"
#include "a_lights.h"
#include "r_utility.h"
#include "d_player.h"
#include "p_pspr.h"
#include "a_sharedglobal.h"
#include "a_dynlight.h"
#include "p_effect.h"
#include "p_visualthinker.h"
#include "p_local.h"
#include "models.h"
#include "hw_spriterecord.h"
#include "hw_dynlightdata.h"
#include "hw_lightbuffer.h"
#include "hw_shadowmap.h"
#include "hw_lighting.h"
#include <algorithm>
#include <map>
#include <memory>
#if defined(__SSE2__) || defined(_M_X64)
#include <immintrin.h>
#endif
#include <array>
#include <bit>
#include <cfloat>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include "d_ticworker.h"
#include "absl/container/flat_hash_map.h"
#include <vector>
#include <condition_variable>
#include <mutex>
#include <thread>

CVAR(Bool, r_worldmode, true, 0)
CVAR(Int, r_worldmode_sideeffects, 0, 0)
CVAR(Bool, r_worldmode_gpuvis, true, 0)
CVAR(Int, r_worldmode_gpuvis_interval, 1, 0)
CVAR(Bool, r_worldmode_gpuvis_inpass, true, 0)
CVAR(Float, r_worldmode_nearradius, 512.f, 0)
CVAR(Int, r_worldmode_spriterings, 2, 0)
CVAR(Bool, r_worldmode_params, true, 0)
CVAR(Bool, r_worldmode_planeheights, true, 0)
CVAR(Bool, r_worldmode_wallheights, true, 0)

bool R_PlaneInterpolationStart(sector_t* sector, bool ceiling, double& d, double& texz);
bool R_ScrollInterpolationStart(sector_t* sector, bool ceiling, double& x, double& y);
EXTERN_CVAR(Bool, r_worldmode_snapshot)
EXTERN_CVAR(Bool, gl_seamless)
EXTERN_CVAR(Bool, vk_gpu_driven_bake)
EXTERN_CVAR(Bool, r_particles)
EXTERN_CVAR(Bool, r_worldmode_thingsplit)

static bool WorldModePublishing()
{
	return r_worldmode_snapshot && r_worldmode_planeheights && r_worldmode_wallheights;
}
CVAR(Bool, r_worldmode_interpskip, true, 0)
CVAR(Bool, r_worldmode_occlusion_shadow, false, 0)
CVAR(Float, r_worldmode_occlusion_slack, 8.f, 0)
CVAR(Bool, r_worldmode_hotgate, true, 0)
CVAR(Int, r_worldmode_sky, 1, 0)
// [Karto] World mode draws every baked surface, while the classic BSP emits only those facing the viewer; culling hides
// back faces. Cull_CW matches the producers' winding.
CVAR(Int, r_worldmode_cull, 2, 0)

int WorldModeCullMode()
{
	return r_worldmode_cull == 1 ? Cull_CCW : r_worldmode_cull == 2 ? Cull_CW : Cull_None;
}
EXTERN_CVAR(Float, gl_mask_threshold)
EXTERN_CVAR(Bool, gl_render_things)
EXTERN_CVAR(Bool, r_worldmode_occlusion)

namespace
{
	struct FWorldModeSurface
	{
		uint32_t FirstIndex = 0, IndexCount = 0;
		float Min[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, Max[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
		bool Alpha = false;
		uint32_t Row = 0;
		int Line = -1, Sector = -1, Subsector = -1, Section = -1;
		uint32_t Unit = UINT32_MAX, FirstVertex = 0, VertexCount = 0;
		uint32_t Param = UINT32_MAX;
		uint8_t DynKind = 0;
		int32_t DynRef = -1;
	};
	enum { WorldModeDynNone, WorldModeDynWall, WorldModeDynFloor, WorldModeDynCeiling };

	// [Karto] One side tier (side * 3 + tier). Produced texture coordinates are linear in the tier's offsets
	// (HWWall::SetWallCoordinates), so an offset change is a coordinate shift.
	struct FWorldModeTier
	{
		enum { None, Param, Producer };	// no surfaces; every surface follows the record; some surface needs the producers
		uint8_t State = None;
		bool ClampX = false, ClampY = false;
		float Kx = 0.f, Ky = 0.f;	// texture coordinate per unit of x / y offset
		double BaseX = 0., BaseY = 0.;	// offsets the surfaces were produced with
	};

	struct FWorldModeAnimRow
	{
		uint32_t Row = 0;
		int Raw = -1, ScaleFlags = 0, ClampMode = 0, Translation = 0;
		size_t IDOffset = 0;
		FGameTexture* Shown = nullptr;
	};

	struct FWorldModeSurfaceInfo
	{
		int Line = -1, Sector = -1, Subsector = -1, Section = -1;
		uint8_t DynKind = 0;	// FWorldModeSurface::DynKind
		int32_t DynRef = -1;
	};

	struct FWorldModeLightRecipe
	{
		enum { None, Wall, Floor, Ceiling };
		int Kind = None, Sector = -1, Side = -1, Tier = 0;
		bool Additive = false;
		float Alpha = 1.f;
		int Class[4] = {};
	};

	struct FWorldModeLightRow
	{
		uint32_t Row = 0, Unit = 0;
		FWorldModeLightRecipe Recipe;
	};

	struct FWorldModeFlashRow
	{
		uint32_t Row = 0, Unit = 0;
		FWorldModeLightRecipe Recipe;
		FLightCallTrace Trace;
	};

	struct FWorldModeSkyWall
	{
		int Side = -1;
		HWWall Wall;
		float Bounds[6] = {};	// memory order x, height, y
	};

	struct FWorldModeSkyQuad
	{
		float Bounds[6] = {};
		FFlatVertex Quad[4] = {};
		sector_t* SkySector = nullptr;
		int SkyPlane = 0, SkyPortalPlane = -1;
		uint32_t Fade = 0;
		bool Splittable = false, Prepared = false;
	};

	// [Karto] The clamped texture matches the producer only while every vertex's v stays in [0, 1]; the guard lets the
	// CPU re-test that after a move.
	struct FWorldModeClampGuard
	{
		uint32_t Side = 0, Top = 0, Bottom = 0, Anchor = 0;
		float Scale = 0.f, AnchorZ = 0.f;
		float X[4] = {}, Y[4] = {}, Z[4] = {}, V[4] = {};
	};

	struct FWorldModeBake : FWorldCaptureSink
	{
		std::vector<FFlatVertex> Vertices;
		std::vector<float> SurfaceIDs;
		std::vector<uint32_t> Indices;
		std::vector<FWorldModeSurface> Surfaces;
		std::vector<uint8_t> Rows;
		std::unordered_map<std::string, uint32_t> RowIndex;
		size_t RowBytes = 0;
		bool CurrentAlpha = false;
		// [Karto] Ceilings use the floors' vertex order but face the other way; reversed at capture so one winding culls both.
		bool CurrentFlip = false;
		int CurrentLine = -1, CurrentSector = -1, CurrentSubsector = -1, CurrentSection = -1;
		uint64_t Refusals[RefuseCount] = {};
		uint64_t Captured = 0;
		bool Pool = false;
		std::vector<seg_t*>* MissingSegs = nullptr;
		std::vector<HWWall>* Horizons = nullptr;
		std::vector<seg_t*>* TranslucentSegs = nullptr;
		uint32_t RowCount = 0, CurrentUnit = UINT32_MAX;
		uint8_t CurrentDynKind = 0;
		int32_t CurrentDynRef = -1;
		int CurrentRaw = -1;
		uint32_t CurrentParam = UINT32_MAX;
		bool CurrentHeights = false;
		uint32_t CurrentHeightTop = UINT32_MAX, CurrentHeightBottom = UINT32_MAX, CurrentHeightAnchor = UINT32_MAX;
		float CurrentVScale = 0.f, CurrentAnchorTexZ = 0.f;
		bool CurrentWall = false;
		std::vector<uint32_t> VertexRefs;	// four per vertex, parallel to Vertices
		bool CurrentClampY = false;
		std::vector<FWorldModeClampGuard> ClampGuards;
		std::vector<uint32_t> UncoveredUnits;	// side units with a tier the shader cannot follow
		std::vector<uint8_t> UncoveredReasons;	// parallel: why (see WORLDMODEUNCOVERED)
		std::vector<uint8_t> UncoveredDeps;	// parallel: planes the tier depends on (WorldModeDep bits)
		uint8_t CurrentReason = 0, CurrentDeps = 0;
		FWorldModeLightRecipe CurrentLight;
		std::vector<FWorldModeSkyWall> Sky;
		std::vector<FWorldModeLightRow> LightRows;	// candidates, verified after each producer draw
		std::vector<uint32_t> LightFailUnits;
		FLightCallTrace Trace;
		std::vector<FWorldModeFlashRow> FlashRows;
		const std::vector<uint8_t>* AnimPics = nullptr;
		std::vector<FWorldModeAnimRow> Anim;

		uint32_t RowFor(const FWorldCaptureDraw& draw, bool animated, bool flash)
		{
			RowBytes = draw.RowBytes;
			if (!Pool)
			{
				// [Karto] Identical rows are stored once. An animated row also keys its raw texture: two raw textures
				// can show the same frame now and different frames later.
				std::string key(static_cast<const char*>(draw.Row), draw.RowBytes);
				if (animated) key.append(reinterpret_cast<const char*>(&CurrentRaw), sizeof(CurrentRaw));
				// [Karto] A row a light-only change may rewrite must not be shared with surfaces lit from elsewhere.
				if (CurrentLight.Kind != FWorldModeLightRecipe::None)
				{
					const int ints[6] = { CurrentLight.Kind, CurrentLight.Sector, CurrentLight.Class[0], CurrentLight.Class[1],
						CurrentLight.Class[2], CurrentLight.Class[3] };
					key.append(reinterpret_cast<const char*>(ints), sizeof(ints));
					key.append(reinterpret_cast<const char*>(&CurrentLight.Alpha), sizeof(CurrentLight.Alpha));
					key.push_back(char(CurrentLight.Additive));
				}
				if (flash)
				{
					const auto& c = Trace.Color;
					const auto& f = Trace.Fog;
					const int ints[22] = { int(c.Mode), c.Level, c.Rel, c.Fullbright, c.Weapon, int(c.Cm.LightColor.d), int(c.Cm.FadeColor.d),
						c.Cm.Desaturation, c.Cm.BlendFactor, c.Cm.FogDensity, int(f.Mode), f.Level, f.Rel, f.Fullbright, f.HasCm, f.Additive,
						int(f.Cm.LightColor.d), int(f.Cm.FadeColor.d), f.Cm.Desaturation, f.Cm.BlendFactor, f.Cm.FogDensity, Trace.FogLast };
					key.append(reinterpret_cast<const char*>(ints), sizeof(ints));
					key.append(reinterpret_cast<const char*>(&c.Alpha), sizeof(c.Alpha));
				}
				auto found = RowIndex.find(key);
				if (found != RowIndex.end()) return found->second;
				RowIndex.emplace(std::move(key), RowCount);
			}
			Rows.insert(Rows.end(), static_cast<const uint8_t*>(draw.Row), static_cast<const uint8_t*>(draw.Row) + draw.RowBytes);
			if (animated)
				Anim.push_back({ RowCount, CurrentRaw, draw.ScaleFlags, draw.ClampMode, draw.Translation, draw.TextureIDOffset,
					TexMan.GetGameTexture(FSetTextureID(CurrentRaw), true) });
			return RowCount++;
		}

		void Capture(const FWorldCaptureDraw& draw) override
		{
			auto data = screen->mVertexData;
			if (draw.Count < 3 || (draw.Indexed && size_t(draw.Index) + draw.Count > data->ibo_data.Size()))
			{
				Refuse(RefuseVertexSource);
				return;
			}
			FWorldModeSurface surface;
			surface.FirstIndex = uint32_t(Indices.size());
			surface.Alpha = CurrentAlpha;
			const uint32_t base = uint32_t(Vertices.size());
			if (draw.Indexed)
			{
				for (int i = 0; i < draw.Count; ++i)
				{
					const uint32_t source = data->ibo_data[draw.Index + i];
					if (source >= data->vbo_shadowdata.Size())
					{
						Vertices.resize(base);
						Indices.resize(surface.FirstIndex);
						Refuse(RefuseVertexSource);
						return;
					}
					Vertices.push_back(data->vbo_shadowdata[source]);
					Indices.push_back(base + i);
				}
			}
			else
			{
				const FFlatVertex* source = data->GetBuffer(draw.Index);
				Vertices.insert(Vertices.end(), source, source + draw.Count);
				for (int i = 1; i + 1 < draw.Count; ++i)
				{
					Indices.push_back(base);
					Indices.push_back(base + i);
					Indices.push_back(base + i + 1);
				}
			}
			const bool animated = AnimPics && CurrentRaw >= 0 && size_t(CurrentRaw) < AnimPics->size() && (*AnimPics)[CurrentRaw];
			const bool flash = Trace.HasColor && Trace.HasFog && !Trace.Color.Fullbright &&
				(!isSoftwareLighting(Trace.Color.Mode) || Trace.Color.Cm.BlendFactor > 0);
			const uint32_t rowCount = RowCount;
			surface.Row = RowFor(draw, animated, flash);
			if (CurrentLight.Kind != FWorldModeLightRecipe::None) LightRows.push_back({ surface.Row, CurrentUnit, CurrentLight });
			if (flash && RowCount != rowCount) FlashRows.push_back({ surface.Row, CurrentUnit, CurrentLight, Trace });
			surface.Unit = CurrentUnit;
			surface.Param = CurrentParam;
			surface.DynKind = CurrentDynKind;
			surface.DynRef = CurrentDynRef;
			surface.FirstVertex = base;
			surface.VertexCount = uint32_t(Vertices.size()) - base;
			surface.Line = CurrentLine;
			surface.Sector = CurrentSector;
			surface.Subsector = CurrentSubsector;
			surface.Section = CurrentSection;
			// [Karto] The vertex carries the surface index (exact as float below 2^24); the shader maps it to a row.
			const float id = float(Surfaces.size());
			for (size_t v = base; v < Vertices.size(); ++v)
			{
				// [Karto] FFlatVertex is laid out x, z, y; bounds must use the memory order the shader and view matrix see.
				const float position[3] = { Vertices[v].x, Vertices[v].z, Vertices[v].y };
				for (int k = 0; k < 3; ++k)
				{
					surface.Min[k] = std::min(surface.Min[k], position[k]);
					surface.Max[k] = std::max(surface.Max[k], position[k]);
				}
				SurfaceIDs.push_back(id);
			}
			{
				const uint32_t count = uint32_t(Vertices.size()) - base;
				const bool refs = CurrentWall && CurrentHeights && !draw.Indexed && count == 4;
				uint32_t scale, anchorZ;
				memcpy(&scale, &CurrentVScale, sizeof(scale));
				memcpy(&anchorZ, &CurrentAnchorTexZ, sizeof(anchorZ));
				for (uint32_t v = 0; v < count; ++v)
				{
					const bool top = v == 1 || v == 2;
					VertexRefs.push_back(refs ? (top ? CurrentHeightTop : CurrentHeightBottom) : UINT32_MAX);
					VertexRefs.push_back(refs ? CurrentHeightAnchor : UINT32_MAX);
					VertexRefs.push_back(refs ? scale : 0);
					VertexRefs.push_back(refs ? anchorZ : 0);
				}
				if (refs && CurrentClampY && CurrentUnit != UINT32_MAX)
				{
					FWorldModeClampGuard guard;
					guard.Side = CurrentUnit;	// unit; made a side index when distributed
					guard.Top = CurrentHeightTop;
					guard.Bottom = CurrentHeightBottom;
					guard.Anchor = CurrentHeightAnchor;
					guard.Scale = CurrentVScale;
					guard.AnchorZ = CurrentAnchorTexZ;
					for (int v = 0; v < 4; ++v)
					{
						const auto& vertex = Vertices[base + v];
						guard.X[v] = vertex.x;
						guard.Y[v] = vertex.y;
						guard.Z[v] = vertex.z;
						guard.V[v] = vertex.v;
					}
					ClampGuards.push_back(guard);
				}
				if (CurrentWall && !refs && CurrentUnit != UINT32_MAX)
				{
					UncoveredUnits.push_back(CurrentUnit);
					UncoveredReasons.push_back(CurrentHeights ? 2 : CurrentReason);
					UncoveredDeps.push_back(CurrentDeps);
				}
			}
			surface.IndexCount = uint32_t(Indices.size()) - surface.FirstIndex;
			if (CurrentFlip)
				for (size_t t = surface.FirstIndex; t + 2 < Indices.size(); t += 3) std::swap(Indices[t + 1], Indices[t + 2]);
			Surfaces.push_back(surface);
			++Captured;
		}

		void Refuse(ERefusal reason) override { ++Refusals[reason]; }
	};

	// [Karto] The sector fields the producers read, grouped by reader so a change re-produces only its readers. Exact
	// copies rather than a hash: two changes can never cancel out.
	struct FWorldModeSectorSnap
	{
		// Both sides of every line read these: tier heights, sky hacks, glow and fog boundaries.
		struct
		{
			secplane_t Planes[2];
			double TexZ[2];
			FTextureID Texture[2];
			int PlaneFlags[2];
			PalEntry GlowColor[2];
			float GlowHeight[2];
			PalEntry FadeColor;
			int Flags;
			uint32_t MoreFlags, Maps[3];
			const sector_t* Heightsec;
			bool Transdoor;
		} Geometry;
		// Only the sector's own flats read these; plane scrollers change them every tic.
		struct
		{
			FTransform Xform[2];
			double Alpha[2];
			float Reflect[2];
			TextureManipulation TextureFx[2];
		} Flat;
		// The sector's own flats and the sides facing it read these; a neighbour's walls take light from their own sector.
		struct
		{
			int LightLevel;
			int PlaneLight[2];
			PalEntry Special[5], Additive[5];
			FColormap Colormap;
		} Light;
	};

	struct FWorldModeState
	{
		const void* Level = nullptr;
		const void* Sectors = nullptr;
		unsigned SectorCount = 0, LineCount = 0;
		int StartTime = 0;
		std::vector<FWorldModeSurfaceInfo> SurfaceInfo;
		std::vector<uint32_t> SectorStamp;	// frame stamp of the last GPU report that saw the sector
		std::vector<uint32_t> SpriteStamp;
		std::vector<int> SpriteFrontier, SpriteNext;
		std::vector<uint32_t> NeighbourStart;
		std::vector<int> Neighbours;
		uint32_t VisibleStamp = 0, LastVisibleSurfaces = 0, LastVisibleSectors = 0;
		uint64_t VisibilityNS = 0, VisibilityFrames = 0, FlagWrites = 0, LastSprites = 0;
		bool Ready = false;
		static constexpr uint32_t ClusterSize = 64;
		std::vector<float> ClusterBounds;	// 6 floats per cluster
		std::vector<uint8_t> Visible;
		uint32_t Clusters = 0, OpaqueClusters = 0, LastVisible = 0;
		uint64_t BakeMS = 0, Walls = 0, TranslucentWalls = 0, PortalWalls = 0, Flats = 0, MaskedFlats = 0;
		uint64_t Surfaces = 0, Rows = 0, Vertices = 0, IndexCount = 0, Refusals[FWorldCaptureSink::RefuseCount] = {};
		uint64_t CullNS = 0, CullFrames = 0;
		std::vector<float> SectorBounds;	// 4 floats per sector: min x, min y, max x, max y (map units)
		int LastSideEffectTic = -1;
		uint64_t SideEffectPasses = 0, SpriteNS = 0, SpriteFrames = 0;
		uint32_t LastSpriteSectors = 0;
		float SpriteCamX = 0.f, SpriteCamY = 0.f;
		bool SpriteGpuSet = false;

		struct FPoolSlot
		{
			uint32_t Unit = 0, OpaqueFirst = 0, OpaqueCount = 0, AlphaFirst = 0, AlphaCount = 0, Surfaces = 0;
			uint32_t VertexBase = 0, IndexBase = 0, SurfaceBase = 0, VertexCount = 0, IndexCount = 0;
			uint32_t VertexCap = 0, IndexCap = 0, SurfaceCap = 0;
			int Sector = -1;	// the sector whose view draws this unit, for occlusion
			float Bounds[6] = {};
			std::vector<FWorldModeAnimRow> Anim;
		};
		const side_t* SidesBase = nullptr;
		uint32_t StaticVertices = 0, StaticIndices = 0, StaticSurfaces = 0, StaticRows = 0, RowBytes = 0;
		uint32_t PoolVertexCap = 0, PoolIndexCap = 0, PoolSurfaceCap = 0, PoolVertexNext = 0, PoolIndexNext = 0, PoolSurfaceNext = 0;
		std::vector<uint32_t> SurfaceFirst, SurfaceCount;	// each baked surface's range in the clustered index buffer
		std::vector<uint32_t> StaticIndexData;	// clustered baked indices, to restore a unit the pool cannot hold
		std::vector<uint32_t> UnitStart, UnitSurfaces;	// unit -> baked surfaces
		std::vector<FWorldModeSectorSnap> SectorSnap;
		std::vector<int> HotSectors;	// sectors with an active interpolation; checked every frame while seen
		std::vector<uint8_t> HotFlag;	// current hot state per sector; HotSectors may hold stale entries until a pass ends
		std::vector<uint8_t> UnitDirty, UnitGeometry, SideDirty, AnimPics;
		std::vector<int32_t> KeyOffset, SurfaceLight;
		std::vector<uint32_t> KeyCapacity, DirtyKeys;
		std::vector<uint8_t> KeyDirty;
		uint32_t LightFrame = 0, LightNext = 0;
		bool SurfaceTableDirty = false, LightCompact = false;
		std::vector<std::pair<uint32_t, uint32_t>> SurfaceTableRanges;
		FLevelLocals* LightLevel = nullptr;
		uint32_t LightScanStamp = 0, LightScanKey = 0;
		uint64_t LightFullScans = 0;
		uint64_t LightFrames = 0, LightListsBuilt = 0, LightListsTiled = 0, LightNS = 0, DynLightChanges = 0, SurfaceTableWrites = 0, LightOverflows = 0;
		std::vector<uint32_t> DirtyUnits;
		std::vector<uint32_t> DeferredUnits;
		std::vector<int> DirtySides;
		std::vector<int32_t> UnitSlot;
		std::vector<FPoolSlot> Slots;
		std::vector<FWorldModeAnimRow> StaticAnim;
		std::vector<uint32_t> PoolFirst, PoolCount;
		std::vector<float> PoolBounds;
		std::vector<int> PoolSectors;	// per pool cluster, its slot's sector
		std::vector<uint32_t> ClusterSectorStart;
		std::vector<int> ClusterSectors;
		std::vector<uint32_t> SectorClusterStart, SectorClusters;
		bool PoolBoundsDirty = false;
		uint64_t HeightOnlySectors = 0;
		std::vector<uint8_t> SideDeps;
		std::vector<uint32_t> LineOrder;
		uint64_t WallSidesKept = 0, WallSidesReproduced = 0, LineCrossings = 0;
		std::vector<uint8_t> SideUncovered;	// per side: 0 covered, else the first reason found
		std::vector<std::vector<FWorldModeClampGuard>> SideGuards;	// per side, its clamped quads on the records
		FLevelLocals* PublishedLevel = nullptr;
		int PublishedStartTime = -1, PublishedMapTime = -1;
		std::vector<uint8_t> PlaneBlending;
		std::vector<int> BlendingPlanes;
		struct FScrollBlend { uint32_t Slot; float Prev[16]; float Cur[16]; };
		std::vector<FScrollBlend> ScrollBlends;
		uint64_t ScrollBlendsPublished = 0;
		uint64_t Publishes = 0, PublishNS = 0, PublishedPlaneWrites = 0;
		uint64_t ClampGuardChecks = 0, ClampGuardFires = 0;
		uint64_t UncoveredReproduced[9] = {};
		uint64_t OccludedClusters = 0, OcclusionFrames = 0, OcclusionStale = 0;
		std::vector<uint32_t> SurfaceCluster;	// baked surface -> its baked cluster
		struct FShadowDecision { uint32_t Stamp = 0; std::vector<uint8_t> Cull; };
		FShadowDecision Shadow[4];
		uint32_t ShadowNext = 0;
		uint64_t ShadowReports = 0, ShadowMissSurfaces = 0, ShadowMissReports = 0, ShadowSeenSurfaces = 0, ShadowMaxMiss = 0;
		uint32_t PoolOpaque = 0;
		uint32_t ScanCursor = 0;
		uint64_t LastUpdateNS = 0, ScanFrames = 0, ScannedSectors = 0, HotChecks = 0, HotDeferred = 0;
		uint64_t UnitsDeferred = 0, UnitsReleased = 0, DeferFlushes = 0;
		uint64_t SidesNS = 0, AnimNS = 0, SubmitNS = 0, WriteCount = 0, HotNS = 0, SelectNS = 0;
		std::vector<uint8_t> SideRecordKinds, SidePending;
		std::vector<uint64_t> SideTestedFrame;
		std::vector<int> PendingSides;
		std::vector<int> SideSectors;	// 2 per side: its own sector, the sector across its line (-1 for none)
		std::vector<uint64_t> SeenFrame;
		std::vector<uint8_t> SeenValue;
		uint64_t RecordsDeferred = 0, RecordsReleased = 0;
		uint64_t ScanNS = 0, Scans = 0, UpdateNS = 0, UpdateFrames = 0, ReproduceNS = 0, ReproducedUnits = 0, ReproduceFrames = 0;
		uint64_t AnimPatches = 0, AnimFailures = 0, WriteBytes = 0, Compactions = 0, Overflows = 0, Skipped = 0;
		uint64_t ProduceNS = 0, RowOnlyUnits = 0, InPlaceUnits = 0;
		uint64_t GeometryFields[11] = {};
		uint64_t SideChanges = 0;
		std::vector<uint16_t> SideKinds;
		uint64_t SideKindCalls[side_t::RenderChangeKinds] = {};
		uint64_t OffsetOnlySides = 0, OffsetMid2sSides = 0, StructuralSides = 0;
		uint64_t WallProcessNS = 0, WallDrawNS = 0, FlatProcessNS = 0, FlatDrawNS = 0, VertexUpdateNS = 0;
		std::vector<FWorldModeTier> Tiers;
		std::vector<std::vector<FWorldModeLightRow>> UnitLight;
		std::vector<uint8_t> UnitLightOK;
		// [Karto] Rows whose light the shader cannot lift by the weapon flash; the CPU rewrites them when it changes.
		std::vector<FWorldModeFlashRow> FlashRows;
		std::vector<std::vector<FWorldModeFlashRow>> UnitFlash;	// pool rows, per unit
		int FlashApplied = 0;
		bool FlashPlaced = false;
		std::vector<uint8_t> LightSector;
		std::vector<std::vector<FWorldModeSkyWall>> SkyWalls;	// per side
		std::vector<std::vector<FWorldModeSkyQuad>> SkyQuads;	// per side, parallel to SkyWalls
		std::vector<int> SkySides;	// sides with sky walls, possibly stale or repeated entries
		std::vector<uint64_t> SkySideFrame;	// frame a side was last replayed, so a repeated entry is skipped
		std::vector<float> SkySideBounds;
		uint64_t SkyFrames = 0, SkyReplayed = 0, SkyCulled = 0, SkyNS = 0;
		uint32_t SkyLastWalls = 0;
		uint64_t LightSectors = 0, LightRowWrites = 0, LightFallbacks = 0, LightVerified = 0, LightRejected = 0, FlashRejected = 0;
		struct FTierTic { double PrevX = 0., PrevY = 0., CurX = 0., CurY = 0.; int Tic = -1; };
		std::vector<FTierTic> TierTics;
		// [Karto] What the playsim side hook records until the hand-over moves it to the frame-side state (at publish,
		// or at the start of a frame not drawn from a publish), so the hook never touches render state while frames
		// run.
		std::vector<uint16_t> PlaySideKinds;
		std::vector<uint8_t> PlaySideDirty;
		std::vector<int> PlayDirtySides;
		std::vector<FTierTic> PlayTierTics;
		std::vector<int> BlendSides;	// sides whose records carry motion for BlendTic
		std::vector<int> SideBlendTic;
		int BlendTic = -1;
		uint64_t ParamSides = 0, ParamWrites = 0, ParamFallbacks = 0;
		std::vector<uint8_t> PlaneState, PlaneShifted;
		uint64_t PlaneSectors = 0, PlaneWrites = 0, PlaneFallbacks = 0;
		uint64_t PlaneReasons[5] = {};
		uint32_t LastDirtySectors = 0, LastDirtyUnits = 0;
		uint64_t GeometryChanges = 0, FlatChanges = 0, LightChanges = 0, LightCrossings = 0, HotChanges = 0;
		std::vector<seg_t*> MissingSegs, MissingVisible;
		std::vector<HWWall> Horizons;	// the producer's generic horizon walls; the frame builds their portals
		std::vector<seg_t*> TranslucentSegs;	// segs with a translucent wall at bake; the frame re-produces those in view
		std::vector<uint8_t> MissingListed, MovedListed;
		std::unique_ptr<std::atomic<uint8_t>[]> PlanesMoved;
	} WorldMode;

	// [Karto] The side's box is one unit larger than its walls so its frustum test, which may round differently where
	// inlined, never rejects a side whose own wall test would pass.
	void AddWorldModeSkyWall(FWorldModeSkyWall&& sky)
	{
		auto& walls = WorldMode.SkyWalls[sky.Side];
		float* box = &WorldMode.SkySideBounds[size_t(sky.Side) * 6];
		const bool first = walls.empty();
		if (first) WorldMode.SkySides.push_back(sky.Side);
		FWorldModeSkyQuad quad;
		memcpy(quad.Bounds, sky.Bounds, sizeof(quad.Bounds));
		FFlatVertex* cursor = quad.Quad;
		sky.Wall.CreateVertices(cursor, false);
		const seg_t* seg = sky.Wall.seg;
		quad.SkySector = sky.Wall.skysector;
		quad.SkyPlane = sky.Wall.skyplane;
		quad.SkyPortalPlane = sky.Wall.skyportalplane;
		quad.Fade = sky.Wall.Colormap.FadeColor.d;
		quad.Splittable = seg && seg->sidedef && !(seg->sidedef->Flags & WALLF_POLYOBJ) && !(sky.Wall.flags & HWWall::HWF_NOSPLIT);
		quad.Prepared = sky.Wall.vertcount != 0;
		WorldMode.SkyQuads[sky.Side].push_back(quad);
		for (int k = 0; k < 3; ++k)
		{
			box[k] = first ? sky.Bounds[k] - 1.f : std::min(box[k], sky.Bounds[k] - 1.f);
			box[k + 3] = first ? sky.Bounds[k + 3] + 1.f : std::max(box[k + 3], sky.Bounds[k + 3] + 1.f);
		}
		walls.push_back(std::move(sky));
	}

	bool SameLevel(FLevelLocals* level)
	{
		// [Karto] starttime changes on every level load, so a same-map reload that reuses the sector allocation still rebakes.
		return WorldMode.Level == level && WorldMode.Sectors == level->sectors.Data() && WorldMode.StartTime == level->starttime &&
			WorldMode.SectorCount == level->sectors.Size() && WorldMode.LineCount == level->lines.Size();
	}

	struct alignas(16) FActorSnapshotBlock { uint8_t Bytes[16]; };
	struct FActorSnapshot
	{
		FLevelLocals* Level = nullptr;
		int StartTime = -1, MapTime = -1;
		std::vector<FActorSnapshotBlock> Arena;
		std::vector<uint32_t> ListedStart, PortalStart;	// per sector, CSR into Listed / Portal
		std::vector<FSnapshotThing> Listed;
		std::vector<AActor*> Portal;
		std::vector<std::pair<AActor*, size_t>> Copies;	// copied live actors and the arena block of each header
		uint32_t Stamp = 0;	// this publish's AActor::PublishStamp
		std::vector<uint8_t> Included;	// per sector: its lists were published
		alignas(player_t) uint8_t PlayerBytes[sizeof(player_t)];
		bool PlayerValid = false;
		std::vector<FActorSnapshotBlock> PspriteArena;
		// [Karto] The console player's weapon bob of this tic: P_BobWeapon rewrites the live entry in the next tic's
		// player think on the worker, so frames of the published tic read this copy.
		FPlayerBob PlayerBob;
		std::vector<int> PlayerSectors;
		std::vector<FActorSnapshotBlock> QuakeArena;
		std::vector<DEarthquake*> Quakes;
		std::vector<FActorSnapshotBlock> LightArena;
		std::vector<FDynamicLight*> Lights;
		absl::flat_hash_map<const FDynamicLight*, FDynamicLight*> LightCopy;
		// [Karto] What the frame's light scan needs from the live lights, taken while the world is idle: the worker
		// relinks, creates and frees lights while frames draw. Indexed like Lights; surface keys as the scan numbers them.
		std::vector<FDynamicLight*> LightLive;
		std::vector<AActor*> LightOwner;
		std::vector<uint32_t> LightTouchStart, LightTouchKeys;
		std::vector<uint32_t> KeyLightStart, KeyLights;
		std::vector<uint32_t> SpotLights;
		FLightGrid LightGrid;
		std::vector<particle_t> Particles;
		std::vector<uint16_t> ParticleHeads;
		std::vector<uint16_t> ParticleTails;
		uint32_t ParticleHeadAtCopy = NO_PARTICLE, ParticleOldestAtCopy = NO_PARTICLE;
		uint32_t ParticleHeadEntry = 0;
		std::vector<FActorSnapshotBlock> VisualArena;
		std::vector<uint32_t> VisualSubsectors;	// sorted; VisualCopies[i] is in subsector VisualSubsectors[i]
		std::vector<DVisualThinker*> VisualCopies;
		uint64_t CopiedLights = 0, CopiedParticles = 0;
		// [Karto] Play's client clock at publish: frames animate and scroll with it, not with the live clock play may
		// have advanced.
		int LocalWorldTimer = 0;
		std::vector<FActorSnapshotBlock> SectorArena;
		std::vector<int32_t> SectorCopy;
		struct FBlend { int Sector; int Plane; double PrevD, PrevTexZ, CurD, CurTexZ; };
		std::vector<FBlend> Blends;
		std::vector<FSpriteRecord> SpriteRecords;
		std::vector<FSpriteInstance> SpriteInstances;
		std::vector<FSpriteAppearanceKey> SpriteKeys;
		std::vector<uint32_t> SpriteKeyTable;
		std::vector<int32_t> ListedOrder, PortalOrder;
		std::vector<uint64_t> PendingGpuFrames;
		uint64_t RecordedActors = 0;
		uint64_t CopiedSectors = 0;
		uint64_t Publishes = 0, PublishNS = 0, CopiedActors = 0, CopiedBytes = 0, IncludedSectors = 0;
		uint64_t SnapshotSectors = 0, FallbackSectors = 0;	// sprite-pass sector visits served by the snapshot / by live lists
	};
	FActorSnapshot ActorSnapshots[2];
	FActorSnapshot* FrontSnapshot = &ActorSnapshots[0];
	FActorSnapshot& BackSnapshot() { return FrontSnapshot == &ActorSnapshots[0] ? ActorSnapshots[1] : ActorSnapshots[0]; }
	// [Karto] Stamps stay unique across both snapshots: a live actor's PublishStamp names the publish it is in.
	uint32_t PublishStampCounter = 0;

	uint32_t Spread(uint32_t v)
	{
		v &= 0xffff;
		v = (v | (v << 8)) & 0x00ff00ff;
		v = (v | (v << 4)) & 0x0f0f0f0f;
		v = (v | (v << 2)) & 0x33333333;
		return (v | (v << 1)) & 0x55555555;
	}
}

// [Karto] A baked light-list index lives only in the frame that uploaded it, so without the GPU light path a level with
// dynamic lights draws natively.
CVAR(Bool, r_worldmode_lightfallback, true, 0)
CVAR(Bool, r_worldmode_lights, true, 0)
CVAR(Bool, r_worldmode_lightsoff, false, 0)

bool WorldModeActive(FLevelLocals* level)
{
	// [Karto] A stale bake draws nothing, so the frame must fall back to the native renderer rather than skip the world.
	return r_worldmode && WorldMode.Ready && SameLevel(level) && !screen->RenderState()->IsWorldModeStale() &&
		!(r_worldmode_lightfallback && !r_worldmode_lights && level->HasDynamicLights);
}

CVAR(Bool, r_worldmode_snapshot, true, 0)
CVAR(Bool, r_sprite_records, true, 0)
CVAR(Bool, r_sprite_gpu, true, 0)
CVAR(Bool, r_sprite_gpu_trans, true, 0)
CVAR(Bool, r_sprite_roll, true, 0)
CVAR(Bool, r_sprite_billboardflags, true, 0)
CVAR(Bool, r_sprite_gpu_soft, true, 0)
CVAR(Bool, r_sprite_gpu_styles, true, 0)
CVAR(Bool, r_sprite_gpu_billboard, true, 0)
CVAR(Bool, r_sprite_stealth, true, 0)
CVAR(Bool, r_sprite_lightowners, true, 0)
CVAR(Bool, r_particle_gpu, true, 0)

static bool WorldModeSectorSpriteGlow(sector_t* sector)
{
	const auto c = sector->planes[sector_t::floor].GlowColor;
	if (c == 0)
	{
		auto tex = TexMan.GetGameTexture(sector->GetTexture(sector_t::floor));
		return tex != nullptr && tex->isGlowing();
	}
	return c != ~0u;
}

// [Karto] Entries are assigned by the main thread after a publish and filled with bindless IDs at draw time (they need
// the render state); a bindless epoch change invalidates every filled entry. A publish on the tic worker only reads
// Index, which changes only in WorldModeFinishPublish.
struct FGpuSpriteFrameTable
{
	std::unordered_map<uint64_t, uint32_t> Index;
	std::vector<uint32_t> Words;
	std::vector<uint8_t> State;	// 0 assigned, 1 filled, 2 failed (no bindless IDs: those sprites return to the CPU)
	uint64_t Epoch = ~0ull;
	uint64_t Version = 1;
} GpuSpriteFrames;

static std::vector<FSpriteFrameEntry> SpriteFrameTable;
static std::vector<uint8_t> SpriteFrameBuilt;

// [Karto] The publish's view of the frame entries: built ones only. Building reads textures, which frames do
// concurrently, so only the main thread builds them.
static const FSpriteFrameEntry* WorldModeBuiltSpriteFrameEntry(uint32_t index)
{
	return index < SpriteFrameBuilt.size() && SpriteFrameBuilt[index] ? &SpriteFrameTable[index] : nullptr;
}

const FSpriteFrameEntry* WorldModeSpriteFrameEntry(uint32_t index)
{
	if (index >= SpriteFrames.Size()) return nullptr;
	if (SpriteFrameTable.size() != SpriteFrames.Size())
	{
		SpriteFrameTable.assign(SpriteFrames.Size(), FSpriteFrameEntry{});
		SpriteFrameBuilt.assign(SpriteFrames.Size(), 0);
	}
	auto& e = SpriteFrameTable[index];
	if (!SpriteFrameBuilt[index])
	{
		SpriteFrameBuilt[index] = 1;
		const spriteframe_t& f = SpriteFrames[index];
		e.Flip = f.Flip;
		e.SameFirst = f.Texture[0] == f.Texture[1];
		e.Usable = true;
		e.Translucent = false;
		e.TranslucentRotations = 0;
		e.FullbrightDisabled = 0;
		for (int rot = 0; rot < 16; ++rot)
		{
			FGameTexture* tex = f.Texture[rot].isValid() ? TexMan.GetGameTexture(f.Texture[rot], false) : nullptr;
			e.Texture[rot] = tex;
			if (!tex || !tex->isValid() || tex->isHardwareCanvas())
			{
				e.Usable = false;
				continue;
			}
			// [Karto] Positioning first: its border trim decodes the image and records the translucency (CheckTrans),
			// sparing the translucency test a second decode.
			auto& spi = tex->GetSpritePositioning(true);
			e.Translucent |= tex->GetTranslucency();
			if (tex->GetTranslucency()) e.TranslucentRotations |= uint16_t(1u << rot);
			if (tex->isFullbrightDisabled()) e.FullbrightDisabled |= uint16_t(1u << rot);
			e.UL[rot] = spi.GetSpriteUL();
			e.UR[rot] = spi.GetSpriteUR();
			e.VT[rot] = spi.GetSpriteVT();
			e.VB[rot] = spi.GetSpriteVB();
			e.Rect[rot] = spi.GetSpriteRect();
		}
	}
	return &e;
}

// [Karto] Every branch of HWSprite::Process the record path skips must be impossible for these actors for the whole
// tic.
EXTERN_CVAR(Bool, r_sprite_gpu_rejectstats)
static std::unordered_map<const PClass*, std::array<uint32_t, 16>> RecordRefusals;

static bool WorldModeSpriteRecordEligible(AActor* a)
{
	auto Refuse = [&](int reason)
	{
		if (r_sprite_gpu_rejectstats) ++RecordRefusals[a->GetClass()][reason];
		return false;
	};
	if (a->sprite == 0 || a->picnum.isValid() || a->player != nullptr || a->Sector == nullptr) return Refuse(1);
	if ((a->renderflags & RF_SPRITETYPEMASK) != RF_FACESPRITE) return Refuse(2);
	if (a->renderflags & (RF_INVISIBLE | RF_MASKROTATION | RF_INTERPOLATEANGLES | RF_MAYBEINVISIBLE)) return Refuse(3);
	if (!r_sprite_roll && (a->renderflags & (RF_ROLLSPRITE | RF_ROLLCENTER))) return Refuse(4);
	if (!r_sprite_billboardflags && (a->renderflags & (RF_FORCEYBILLBOARD | RF_FORCEXYBILLBOARD))) return Refuse(5);
	if (a->renderflags2 & (RF2_ISOMETRICSPRITES | RF2_SQUAREPIXELS | RF2_ANGLEDROLL | RF2_BILLBOARDFACECAMERA |
		RF2_BILLBOARDNOFACECAMERA | RF2_INVISIBLEINMIRRORS | RF2_ONLYVISIBLEINMIRRORS)) return Refuse(6);
	if (a->flags8 & MF8_MASTERNOSEE) return Refuse(7);
	if (!r_sprite_stealth && (a->flags & MF_STEALTH)) return Refuse(8);
	if ((a->flags & MF_MISSILE) && !(a->flags7 & MF7_FLYCHEAT) && a->target != nullptr && a->target->player != nullptr) return Refuse(9);
	if (a->VisibleToTeam != 0 || a->GetInfo()->VisibleToPlayerClass.Size() != 0 || !a->ShouldRenderLocally()) return Refuse(10);
	if (a->Sector->e->XFloor.lightlist.Size() != 0) return Refuse(11);
	// [Karto] A light copy points at its owner's copy; an owner drawn from a record has none, so the copy keeps the
	// live actor, which the tic worker writes while frames draw. Frames use it only as an identity, but a light whose
	// settings live in the owner's body is excluded.
	if (a->AttachedLights.Size() != 0)
	{
		if (!r_sprite_lightowners) return Refuse(12);
		const char* from = reinterpret_cast<const char*>(a);
		const char* end = from + a->GetClass()->Size;
		auto inside = [&](const void* p) { return static_cast<const char*>(p) >= from && static_cast<const char*>(p) < end; };
		for (auto light : a->AttachedLights)
			if (inside(light->pArgs) || inside(light->pLightFlags) || inside(light->pSpotInnerAngle) || inside(light->pSpotOuterAngle)) return Refuse(13);
	}
	if (R_ShouldDrawSpriteShadow(a)) return Refuse(14);
	return FindModelFrame(a, a->sprite, a->frame, !!(a->flags & MF_DROPPED)) == nullptr || Refuse(15);
}

static void WorldModePublishSectors(FLevelLocals* level)
{
	if (!WorldModePublishing() || !r_worldmode || !WorldMode.Ready || !SameLevel(level)) return;
	auto* di = HWDrawInfo::StartDrawInfo(level, nullptr, r_viewpoint, nullptr);
	di->UpdateWorldMode(true);
	di->EndDrawInfo();
}

extern bool DecouplePublishHeld;

// [Karto] The live maptime is not read: it races a worker writing it.
int WorldModeFrameTic(FLevelLocals* level)
{
	const auto& snap = (*FrontSnapshot);
	if (r_worldmode_snapshot && snap.Level == level && snap.StartTime == level->starttime) return snap.MapTime;
	return level->maptime;
}

// [Karto] Not the braced std::max form, which called MSVC's vectorized min/max routine on every test; same value.
static inline float WorldModeGap(float lo, float hi, float p)
{
	return std::max(std::max(lo - p, 0.f), p - hi);
}

static bool WorldModeSpriteSectorWanted(uint32_t s, const uint8_t* entered, float camX, float camY)
{
	auto& wm = WorldMode;
	if (entered == nullptr || entered[s] || wm.SpriteStamp[s] == wm.VisibleStamp) return true;
	const float* b = &wm.SectorBounds[size_t(s) * 4];
	const float dx = WorldModeGap(b[0], b[2], camX), dy = WorldModeGap(b[1], b[3], camY);
	return dx * dx + dy * dy <= float(r_worldmode_nearradius) * float(r_worldmode_nearradius);
}

static void WorldModePublishIncluded(FLevelLocals* level, std::vector<uint8_t>& included)
{
	const uint32_t sectors = level->sectors.Size();
	auto& wm = WorldMode;
	const bool selective = wm.SpriteGpuSet && wm.SpriteStamp.size() == sectors && wm.SectorBounds.size() == size_t(sectors) * 4;
	included.assign(sectors, selective ? 0 : 1);
	if (selective)
	{
		const uint8_t* entered = nullptr;
		if (!WorldModeWalkSectors(level, entered)) entered = nullptr;
		for (uint32_t s = 0; s < sectors; ++s) included[s] = WorldModeSpriteSectorWanted(s, entered, wm.SpriteCamX, wm.SpriteCamY);
	}
}

static void WorldModeCopyParticles(FLevelLocals* level, FActorSnapshot& snap)
{
	const uint32_t subsectors = level->subsectors.Size();
	snap.Particles.clear();
	snap.ParticleHeads.assign(subsectors, NO_PARTICLE);
	snap.ParticleTails.assign(subsectors, NO_PARTICLE);
	snap.ParticleHeadAtCopy = level->ActiveParticles;
	snap.ParticleOldestAtCopy = level->OldestParticle;
	snap.ParticleHeadEntry = 0;
	if (r_particles)
	{
		for (uint16_t i = level->ActiveParticles; i != NO_PARTICLE && snap.Particles.size() < NO_PARTICLE; i = level->Particles[i].tnext)
		{
			auto& live = level->Particles[i];
			if (live.subsector == nullptr) live.subsector = level->PointInRenderSubsector(live.Pos);
			const uint32_t ss = live.subsector->Index();
			snap.Particles.push_back(live);
			snap.Particles.back().snext = snap.ParticleHeads[ss];
			snap.ParticleHeads[ss] = uint16_t(snap.Particles.size() - 1);
			if (snap.ParticleTails[ss] == NO_PARTICLE) snap.ParticleTails[ss] = snap.ParticleHeads[ss];
		}
	}
	snap.CopiedParticles += snap.Particles.size();
}

EXTERN_CVAR(Int, r_worldmode_publishthreads)
void WorldModeParallelFor(int parts, void (*job)(void* context, int part, int parts), void* context);

static size_t PublishedLightOrdinal(const FActorSnapshot& snap, const FDynamicLight* copy)
{
	constexpr size_t stride = (sizeof(FDynamicLight) + 15) / 16 * sizeof(FActorSnapshotBlock);
	return size_t(reinterpret_cast<const char*>(copy) - reinterpret_cast<const char*>(snap.LightArena.data())) / stride;
}

static void WorldModePublishLights(FLevelLocals* level, FActorSnapshot& snap)
{
	auto copyOf = [&](AActor* actor) { return reinterpret_cast<AActor*>(&snap.Arena[size_t(actor->PublishIndex) + 1]); };
	auto copied = [&](AActor* live) { return live != nullptr && live->PublishStamp == snap.Stamp && live->PublishIndex >= 0; };
// [Karto] A copy whose owner was copied points at that copy, and its settings pointers into the owner (map lights keep
// args, flags and spot angles there) move onto it; GLDEFS lights keep pointing at their shared defaults.
	snap.Lights.clear();
	snap.LightCopy.clear();
	snap.LightTouchKeys.clear();
	snap.SpotLights.clear();
	{
		static std::vector<FDynamicLight*> live;
		live.clear();
		for (auto light = level->lights; light; light = light->next) live.push_back(light);
		constexpr size_t lightBlocks = (sizeof(FDynamicLight) + 15) / 16;
		const size_t lightCount = live.size();
		snap.LightArena.resize(lightCount * lightBlocks);
		snap.Lights.resize(lightCount);
		snap.LightLive.assign(live.begin(), live.end());
		snap.LightOwner.resize(lightCount);
		snap.LightTouchStart.resize(lightCount + 1);
		snap.LightTouchStart[0] = 0;
		static std::vector<uint32_t> partKeys[8], partSpots[8];
		const uint32_t sides = level->sides.Size();
		auto copyLights = [&](int part, int parts)
		{
			auto& keys = partKeys[part];
			auto& spots = partSpots[part];
			keys.clear();
			spots.clear();
			for (size_t i = lightCount * part / parts, end = lightCount * (part + 1) / parts; i < end; ++i)
			{
				FDynamicLight* light = live[i];
				auto* copy = reinterpret_cast<FDynamicLight*>(&snap.LightArena[i * lightBlocks]);
				memcpy((void*)copy, (const void*)light, sizeof(FDynamicLight));
				AActor* owner = light->target;
				copy->PublishedCopy = true;
				copy->PublishedOwnerAlpha = owner != nullptr && (owner->renderflags2 & RF2_LIGHTMULTALPHA) ? (float)owner->Alpha : 1.f;
				if (copied(owner))
				{
					AActor* ownerCopy = copyOf(owner);
					*reinterpret_cast<AActor**>(&copy->target) = ownerCopy;
					const char* from = reinterpret_cast<const char*>(owner);
					const char* end = from + owner->GetClass()->Size;
					char* to = reinterpret_cast<char*>(ownerCopy);
					auto rebase = [&](auto*& pointer)
					{
						const char* at = reinterpret_cast<const char*>(pointer);
						if (at >= from && at < end) pointer = reinterpret_cast<std::remove_reference_t<decltype(pointer)>>(to + (at - from));
					};
					rebase(copy->pSpotInnerAngle);
					rebase(copy->pSpotOuterAngle);
					rebase(copy->pArgs);
					rebase(copy->pLightFlags);
				}
				if (copy->IsSpot()) spots.push_back(uint32_t(i));
				snap.Lights[i] = copy;
				snap.LightOwner[i] = owner;
				const size_t before = keys.size();
				for (side_t* side : light->touchlists.wall_tlist) keys.push_back(uint32_t(side->Index()));
				for (FSection* section : light->touchlists.flat_tlist)
				{
					keys.push_back(sides + uint32_t(section->Index()) * 2);
					keys.push_back(sides + uint32_t(section->Index()) * 2 + 1);
				}
				snap.LightTouchStart[i + 1] = uint32_t(keys.size() - before);
			}
		};
		const int parts = lightCount >= 512 ? std::clamp(*r_worldmode_publishthreads, 1, 8) : 1;
		WorldModeParallelFor(parts, [](void* context, int part, int parts) { (*static_cast<decltype(copyLights)*>(context))(part, parts); }, &copyLights);
		for (size_t i = 0; i < lightCount; ++i) snap.LightTouchStart[i + 1] += snap.LightTouchStart[i];
		for (int part = 0; part < parts; ++part)
		{
			snap.LightTouchKeys.insert(snap.LightTouchKeys.end(), partKeys[part].begin(), partKeys[part].end());
			snap.SpotLights.insert(snap.SpotLights.end(), partSpots[part].begin(), partSpots[part].end());
		}
		snap.LightCopy.reserve(lightCount);
		for (size_t i = 0; i < lightCount; ++i) snap.LightCopy.emplace(live[i], snap.Lights[i]);
		const size_t keys = size_t(level->sides.Size()) + size_t(level->sections.allSections.Size()) * 2;
		snap.KeyLightStart.assign(keys + 1, 0);
		for (uint32_t key : snap.LightTouchKeys) if (key < keys) ++snap.KeyLightStart[key + 1];
		for (size_t k = 0; k < keys; ++k) snap.KeyLightStart[k + 1] += snap.KeyLightStart[k];
		snap.KeyLights.resize(snap.KeyLightStart[keys]);
		std::vector<uint32_t> fill(snap.KeyLightStart.begin(), snap.KeyLightStart.end() - 1);
		for (uint32_t i = 0; i + 1 < snap.LightTouchStart.size(); ++i)
			for (uint32_t t = snap.LightTouchStart[i]; t < snap.LightTouchStart[i + 1]; ++t)
				if (snap.LightTouchKeys[t] < keys) snap.KeyLights[fill[snap.LightTouchKeys[t]]++] = i;
		absl::erase_if(snap.LightGrid.cells, [&](const auto& published)
		{
			auto found = level->lightgrid.cells.find(published.first);
			return found == level->lightgrid.cells.end() || found->second.lights.Size() == 0;
		});
		for (auto& [key, cell] : level->lightgrid.cells)
		{
			if (cell.lights.Size() == 0) continue;
			auto& published = snap.LightGrid.cells[key];
			published.lights.Clear();
			for (auto light : cell.lights)
			{
				auto copy = snap.LightCopy.find(light);
				if (copy != snap.LightCopy.end()) published.lights.Push(copy->second);
			}
		}
		snap.CopiedLights += snap.Lights.size();
	}
}

// [Karto] The particles the client pass spawned since the copy. NewParticle only pushes onto the active head and only
// SPF_REPLACE evicts (the oldest), so an unchanged oldest particle means the new ones sit at the end of each subsector
// chain, newest last. Anything else copies them all.
static uint64_t ParticleDeltaAppended, ParticleDeltaRebuilds;
static size_t ParticleDeltaLast;

static void WorldModePublishParticleDelta(FLevelLocals* level, FActorSnapshot& snap)
{
	ParticleDeltaLast = 0;
	if (!r_particles || level->ActiveParticles == snap.ParticleHeadAtCopy) return;
	static std::vector<uint32_t> fresh;
	fresh.clear();
	uint32_t i = level->ActiveParticles;
	while (i != NO_PARTICLE && i != snap.ParticleHeadAtCopy && fresh.size() <= NO_PARTICLE)
	{
		fresh.push_back(i);
		i = level->Particles[i].tnext;
	}
	const bool intact = i == snap.ParticleHeadAtCopy &&
		(snap.ParticleOldestAtCopy == NO_PARTICLE || level->OldestParticle == snap.ParticleOldestAtCopy) &&
		snap.ParticleTails.size() == snap.ParticleHeads.size() && snap.Particles.size() + fresh.size() <= NO_PARTICLE;
	if (!intact)
	{
		WorldModeCopyParticles(level, snap);
		++ParticleDeltaRebuilds;
		return;
	}
	if (fresh.size() >= 512)
	{
		struct FLocate { FLevelLocals* Level; const uint32_t* Fresh; size_t Count; } job = { level, fresh.data(), fresh.size() };
		WorldModeParallelFor(std::clamp(*r_worldmode_publishthreads, 1, 8), [](void* context, int part, int parts)
		{
			auto& j = *static_cast<FLocate*>(context);
			for (size_t k = j.Count * part / parts, end = j.Count * (part + 1) / parts; k < end; ++k)
			{
				auto& live = j.Level->Particles[j.Fresh[k]];
				if (live.subsector == nullptr) live.subsector = j.Level->PointInRenderSubsector(live.Pos);
			}
		}, &job);
	}
	// [Karto] NewParticle's push also changes the old head's tprev; the copy began at that head, so it is the first
	// entry.
	if (snap.ParticleHeadAtCopy != NO_PARTICLE && snap.ParticleHeadEntry < snap.Particles.size())
		snap.Particles[snap.ParticleHeadEntry].tprev = level->Particles[snap.ParticleHeadAtCopy].tprev;
	for (size_t k = fresh.size(); k-- > 0;)
	{
		auto& live = level->Particles[fresh[k]];
		if (live.subsector == nullptr) live.subsector = level->PointInRenderSubsector(live.Pos);
		const uint32_t ss = live.subsector->Index();
		const uint16_t index = uint16_t(snap.Particles.size());
		snap.Particles.push_back(live);
		snap.Particles.back().snext = NO_PARTICLE;
		if (snap.ParticleTails[ss] == NO_PARTICLE) snap.ParticleHeads[ss] = index;
		else snap.Particles[snap.ParticleTails[ss]].snext = index;
		snap.ParticleTails[ss] = index;
	}
	snap.CopiedParticles += fresh.size();
	ParticleDeltaAppended += fresh.size();
	ParticleDeltaLast = fresh.size();
}

CVAR(Bool, r_particle_stagecopy, true, 0)
static bool WorldModeHasMap(const FLevelLocals* level);
static std::vector<particle_t> ParticleStage;
static std::vector<uint16_t> ParticleStageIndex;
static FLevelLocals* ParticleStageLevel = nullptr;
static int ParticleStageTime = 0;
static uint16_t ParticleStageHead = NO_PARTICLE, ParticleStageOldest = NO_PARTICLE;
static bool ParticleStageValid = false;

bool WorldModeSkipParticleStage = false;

std::vector<particle_t>* WorldModeParticleStageBegin(FLevelLocals* level, std::vector<uint16_t>*& indices)
{
	ParticleStageValid = false;
	if (WorldModeSkipParticleStage || !r_particle_stagecopy || !r_particles || !r_worldmode || !r_worldmode_snapshot ||
		!WorldModeHasMap(level)) return nullptr;
	ParticleStage.clear();
	ParticleStageIndex.clear();
	ParticleStageLevel = level;
	indices = &ParticleStageIndex;
	return &ParticleStage;
}

void WorldModeParticleStageEnd(FLevelLocals* level)
{
	ParticleStageHead = uint16_t(level->ActiveParticles);
	ParticleStageOldest = uint16_t(level->OldestParticle);
	ParticleStageTime = level->maptime;
	ParticleStageValid = true;
}

static void WorldModePublishStagedParticles(FLevelLocals* level, FActorSnapshot& snap)
{
	const bool usable = ParticleStageValid && ParticleStageLevel == level && level->maptime == ParticleStageTime + 1;
	ParticleStageValid = false;
	if (!usable)
	{
		WorldModeCopyParticles(level, snap);
		return;
	}
	const uint32_t subsectors = level->subsectors.Size();
	std::swap(snap.Particles, ParticleStage);
	snap.ParticleHeads.assign(subsectors, NO_PARTICLE);
	snap.ParticleTails.assign(subsectors, NO_PARTICLE);
	snap.ParticleHeadAtCopy = ParticleStageHead;
	snap.ParticleOldestAtCopy = ParticleStageOldest;
	snap.ParticleHeadEntry = 0;
	uint16_t* heads = snap.ParticleHeads.data();
	uint16_t* tails = snap.ParticleTails.data();
	const size_t count = snap.Particles.size();
	for (size_t k = 0; k < count; ++k)
	{
		auto& p = snap.Particles[k];
		p.tnext = k + 1 < count ? ParticleStageIndex[k + 1] : level->Particles[ParticleStageIndex[k]].tnext;
		const uint32_t ss = p.subsector->Index();
		p.snext = heads[ss];
		heads[ss] = uint16_t(k);
		if (tails[ss] == NO_PARTICLE) tails[ss] = uint16_t(k);
	}
	snap.CopiedParticles += snap.Particles.size();
	const uint64_t rebuilds = ParticleDeltaRebuilds;
	WorldModePublishParticleDelta(level, snap);
	if (ParticleDeltaRebuilds == rebuilds)
	{
		snap.ParticleHeadAtCopy = level->ActiveParticles;
		snap.ParticleOldestAtCopy = level->OldestParticle;
		snap.ParticleHeadEntry = ParticleDeltaLast > 0 ? uint32_t(snap.Particles.size() - 1) : 0;
	}
}

static void WorldModeCopyPlayer(FActorSnapshot& snap, player_t* player)
{
	auto copyOf = [&](AActor* actor) { return reinterpret_cast<AActor*>(&snap.Arena[size_t(actor->PublishIndex) + 1]); };
	auto copied = [&](AActor* live) { return live != nullptr && live->PublishStamp == snap.Stamp && live->PublishIndex >= 0; };
	auto* copy = reinterpret_cast<player_t*>(snap.PlayerBytes);
	memcpy((void*)copy, (const void*)player, sizeof(player_t));
	AActor* body = copyOf(player->mo);
	AActor* camera = player->camera != nullptr && copied(player->camera.Get()) ? copyOf(player->camera) : body;
	copy->mo = body;
	*reinterpret_cast<AActor**>(&copy->camera) = camera;
	body->player = copy;
	if (player->camera != nullptr && player->camera->player == player) camera->player = copy;
	size_t pspBlocks = 0;
	for (DPSprite* psp = player->psprites; psp != nullptr; psp = psp->GetNext()) pspBlocks += (size_t(psp->GetClass()->Size) + 15) / 16;
	snap.PspriteArena.resize(pspBlocks);
	DPSprite** link = reinterpret_cast<DPSprite**>(&copy->psprites);
	size_t at = 0;
	for (DPSprite* psp = player->psprites; psp != nullptr; psp = psp->GetNext())
	{
		auto* pspCopy = reinterpret_cast<DPSprite*>(&snap.PspriteArena[at]);
		memcpy((void*)pspCopy, (const void*)psp, psp->GetClass()->Size);
		at += (size_t(psp->GetClass()->Size) + 15) / 16;
		pspCopy->Owner = copy;
		AActor* caller = psp->Caller;
		if (caller != nullptr) *reinterpret_cast<AActor**>(&pspCopy->Caller) = copyOf(caller);
		*link = pspCopy;
		link = reinterpret_cast<DPSprite**>(&pspCopy->Next);
		// [Karto] One-shot handshake the renderer consumes on its first frame: consumed on the live sprite here, or
		// every publish would hand it out again.
		psp->firstTic = false;
	}
	*link = nullptr;
	if (player->camera != nullptr) player->camera->renderflags &= ~RF_NOINTERPOLATEVIEW;
	snap.PlayerBob = PlayerBob[consoleplayer];
	snap.PlayerSectors.clear();
	for (auto node = player->mo->touching_rendersectors; node; node = node->m_tnext) snap.PlayerSectors.push_back(node->m_sector->Index());
	snap.PlayerValid = true;
}

// [Karto] r_worldmode_publishthreads: the actor copy is split over this many threads (1 copies alone). Safe because the
// copy runs between tics and each actor's arena place is fixed beforehand, so each thread writes only its own part.
CVAR(Int, r_worldmode_publishthreads, 8, 0)

namespace
{
	class FPublishPool
	{
	public:
		// Runs job(part, parts) for every part, part 0 on the calling thread; returns once all have finished.
		void Run(int parts, void (*job)(void* context, int part, int parts), void* context)
		{
			parts = std::clamp(parts, 1, MaxParts);
			if (parts > 1) Start(parts - 1);
			{
				std::lock_guard<std::mutex> guard(Lock);
				Job = job;
				Context = context;
				Parts = parts;
				Pending = parts - 1;
				++Generation;
			}
			Wake.notify_all();
			job(context, 0, parts);
			std::unique_lock<std::mutex> guard(Lock);
			Done.wait(guard, [this] { return Pending == 0; });
		}

	private:
		static constexpr int MaxParts = 8;

		void Start(int helpers)
		{
			// [Karto] Never joined, as the tic worker: a joinable std::thread destroyed at exit() calls std::terminate.
			while (int(Threads.size()) < helpers)
			{
				const int index = int(Threads.size()) + 1;
				Threads.push_back(new std::thread([this, index] { Helper(index); }));
				Threads.back()->detach();
			}
		}

		void Helper(int index)
		{
			uint64_t seen = 0;
			for (;;)
			{
				void (*job)(void*, int, int);
				void* context;
				int parts;
				{
					std::unique_lock<std::mutex> guard(Lock);
					Wake.wait(guard, [&] { return Generation != seen; });
					seen = Generation;
					if (index >= Parts) continue;
					job = Job;
					context = Context;
					parts = Parts;
				}
				job(context, index, parts);
				{
					std::lock_guard<std::mutex> guard(Lock);
					--Pending;
				}
				Done.notify_all();
			}
		}

		std::mutex Lock;
		std::condition_variable Wake, Done;
		std::vector<std::thread*> Threads;
		void (*Job)(void*, int, int) = nullptr;
		void* Context = nullptr;
		int Parts = 0, Pending = 0;
		uint64_t Generation = 0;
	};

	FPublishPool PublishPool;
}

// [Karto] r_interp_dense: the playsim's use of the publish helpers (P_Ticker's interpolation sweep). The pool serves
// one caller at a time; the sweep and the publish run on the thread running the tic.
void WorldModeParallelFor(int parts, void (*job)(void* context, int part, int parts), void* context)
{
	PublishPool.Run(parts, job, context);
}

template<class Job> static void WorldModeRunParts(int parts, Job& job)
{
	PublishPool.Run(parts, [](void* context, int part, int parts) { (*static_cast<Job*>(context))(part, parts); }, &job);
}

// [Karto] Copies are invisible to the GC: a pointer to an actor destroyed before this publish would dangle once a later
// GC step frees it. Null target, master and tracer here as the GC would; objects destroyed later cannot be freed before
// the next publish replaces these copies.
static void WorldModeDropDestroyedRefs(AActor* copy)
{
	for (TObjPtr<AActor*>* field : { &copy->target, &copy->master, &copy->tracer })
	{
		AActor* other = field->ForceGet();
		if (other != nullptr && (other->ObjectFlags & OF_EuthanizeMe)) *reinterpret_cast<AActor**>(field) = nullptr;
	}
}

static void WorldModeCopyActorsPart(void* context, int part, int parts)
{
	auto& snap = *static_cast<FActorSnapshot*>(context);
	const size_t count = snap.Copies.size();
	const size_t begin = count * part / parts, end = count * (part + 1) / parts;
	for (size_t i = begin; i < end; ++i)
	{
		const auto& [actor, offset] = snap.Copies[i];
		memcpy(snap.Arena[offset].Bytes, &actor, sizeof(actor));
		memcpy((void*)&snap.Arena[offset + 1], (const void*)actor, actor->GetClass()->Size);
		WorldModeDropDestroyedRefs(reinterpret_cast<AActor*>(&snap.Arena[offset + 1]));
	}
}

CVAR(Bool, r_sprite_gpu_opaquetrans, true, 0)

CVAR(Bool, r_sprite_gpu_rejectstats, false, 0)
enum { GR_Instanced, GR_Geometry, GR_Style, GR_RenderFilter, GR_Offset, GR_Glow, GR_Frame, GR_Pending, GR_Unusable, GR_SoftMixed,
	GR_BrightMixed, GR_Count };
static const char* const GpuRejectNames[GR_Count] = { "instanced", "geometry", "style", "renderfilter", "offset", "glow", "frame",
	"pending", "unusable", "softmixed", "brightmixed" };
static std::unordered_map<const PClass*, std::array<uint64_t, GR_Count>> GpuRejects;
static uint64_t GpuRejectPublishes;
static void GpuRejectCount(const PClass* cls, int reason) { ++GpuRejects[cls][reason]; }
static std::unordered_map<uint64_t, uint64_t> GpuRejectStyles;

CCMD(sprite_gpu_rejects)
{
	if (argv.argc() == 2 && stricmp(argv[1], "reset") == 0) { GpuRejects.clear(); GpuRejectStyles.clear(); RecordRefusals.clear(); GpuRejectPublishes = 0; return; }
	const double p = GpuRejectPublishes ? double(GpuRejectPublishes) : 1.0;
	std::array<uint64_t, GR_Count> total = {};
	std::vector<std::pair<uint64_t, const PClass*>> order;
	for (auto& [cls, counts] : GpuRejects)
	{
		uint64_t cpu = 0;
		for (int k = 0; k < GR_Count; ++k) { total[k] += counts[k]; if (k != GR_Instanced) cpu += counts[k]; }
		order.push_back({ cpu, cls });
	}
	Printf("GPUREJECTS publishes=%llu", (unsigned long long)GpuRejectPublishes);
	for (int k = 0; k < GR_Count; ++k) Printf(" %s=%.1f", GpuRejectNames[k], total[k] / p);
	Printf("\n");
	std::vector<std::pair<uint64_t, uint64_t>> styles(GpuRejectStyles.begin(), GpuRejectStyles.end());
	std::sort(styles.begin(), styles.end(), [](auto& a, auto& b) { return a.second > b.second; });
	for (size_t i = 0; i < styles.size() && i < 8; ++i)
		Printf("GPUREJECTSTYLE style=%08x alpha=%.2f zdoomtrans=%d count=%.1f\n", uint32_t(styles[i].first >> 32), int((styles[i].first >> 1) & 0xffff) / 100.0,
			int(styles[i].first & 1), styles[i].second / p);
	std::sort(order.begin(), order.end(), [](auto& a, auto& b) { return a.first > b.first; });
	for (size_t i = 0; i < order.size() && i < 15 && order[i].first > 0; ++i)
	{
		Printf("GPUREJECTCLASS %s", order[i].second->TypeName.GetChars());
		const auto& counts = GpuRejects[order[i].second];
		for (int k = 0; k < GR_Count; ++k) if (counts[k]) Printf(" %s=%.1f", GpuRejectNames[k], counts[k] / p);
		Printf("\n");
	}
	std::vector<std::pair<uint64_t, const PClass*>> refused;
	for (auto& [cls, counts] : RecordRefusals)
	{
		uint64_t sum = 0;
		for (uint32_t c : counts) sum += c;
		refused.push_back({ sum, cls });
	}
	std::sort(refused.begin(), refused.end(), [](auto& a, auto& b) { return a.first > b.first; });
	for (size_t i = 0; i < refused.size() && i < 12; ++i)
	{
		Printf("GPUNORECORD %s", refused[i].second->TypeName.GetChars());
		const auto& counts = RecordRefusals[refused[i].second];
		for (int k = 0; k < 16; ++k) if (counts[k]) Printf(" test%d=%.1f", k, counts[k] / p);
		Printf("\n");
	}
}

CVAR(Bool, r_worldmode_publishonewalk, true, 0)
CVAR(Bool, r_worldmode_publishkeyhash, true, 0)
CVAR(Bool, r_worldmode_publishwalkverify, false, 0)
static uint64_t PublishWalkChecks = 0, PublishWalkMismatches = 0, PublishKeyChecks = 0, PublishKeyMismatches = 0;

static bool SameSpriteKey(const FSpriteAppearanceKey& k, sector_t* sector, int lightlevel, bool fullbright, uint8_t blend,
	float alpha, bool soft, bool shaded, uint32_t fill)
{
	return k.Sector == sector && k.LightLevel == lightlevel && k.Fullbright == fullbright && k.Blend == blend && k.Alpha == alpha &&
		k.SoftTexture == soft && k.Shaded == shaded && k.FillColor == fill;
}

CVAR(Bool, r_worldmode_publishsplit, true, 0)
struct FPublishWalkEntry
{
	AActor* Actor;
	sector_t* Sector;
	bool Eligible;
	bool Stamped;	// already copied before the walk
	bool Multi;		// in more than one sector list, so a later visit may find it decided
};
struct FPublishStampWrite
{
	AActor* Actor;
	int32_t Index;
};
static std::vector<FPublishStampWrite> PublishStampWrites;
static constexpr int PublishWalkMaxParts = 8;
static std::vector<uint32_t> PublishWalkSectors, PublishWalkCounts;
static std::vector<FPublishWalkEntry> PublishWalkParts[PublishWalkMaxParts];
static std::vector<FSpriteInstance> InstanceParts[PublishWalkMaxParts];
static std::vector<FSpriteAppearanceKey> InstanceKeyParts[PublishWalkMaxParts];
static std::vector<uint64_t> PendingFrameParts[PublishWalkMaxParts];

static void WorldModeBuildPublish(FLevelLocals* level, FActorSnapshot& snap, const std::vector<uint8_t>& included)
{
	const uint64_t started = I_nsTime();
	const uint32_t sectors = level->sectors.Size();
	snap.Copies.clear();
	if (++PublishStampCounter == 0) PublishStampCounter = 1;
	snap.Stamp = PublishStampCounter;
	snap.Listed.clear();
	snap.Portal.clear();
	snap.ListedStart.assign(sectors + 1, 0);
	snap.PortalStart.assign(sectors + 1, 0);
	snap.Included = included;
	// [Karto] Size the arena first so copies never move while frames hold pointers into it.
	size_t blocks = 0;
	auto reserve = [&](AActor* actor)
	{
		if (actor->PublishStamp == snap.Stamp) return;
		actor->PublishStamp = snap.Stamp;
		actor->PublishIndex = int32_t(blocks);
		snap.Copies.emplace_back(actor, blocks);
		blocks += 1 + (size_t(actor->GetClass()->Size) + 15) / 16;
		snap.CopiedBytes += actor->GetClass()->Size;
	};
	player_t* player = &players[consoleplayer];
	const bool publishPlayer = player->mo != nullptr && player->mo->Level == level;
	if (publishPlayer)
	{
		reserve(player->mo);
		if (player->camera != nullptr && player->camera->Level == level) reserve(player->camera);
		for (DPSprite* psp = player->psprites; psp != nullptr; psp = psp->GetNext())
		{
			AActor* caller = psp->Caller;
			if (caller != nullptr) reserve(caller);
		}
	}
	std::vector<DEarthquake*> quakes;
	{
		auto iterator = level->GetThinkerIterator<DEarthquake>(NAME_None, STAT_EARTHQUAKE);
		for (DEarthquake* quake; (quake = iterator.Next()) != nullptr;)
		{
			quakes.push_back(quake);
			AActor* spot = quake->m_Spot;
			if (spot != nullptr) reserve(spot);
		}
	}
	snap.SpriteRecords.clear();
	const bool oneWalk = r_worldmode_publishonewalk;
	snap.ListedOrder.clear();
	snap.PortalOrder.clear();
	for (uint32_t s = 0; s < sectors; ++s)
	{
		if (!snap.Included[s]) continue;
		++snap.IncludedSectors;
		for (auto node = level->sectors[s].sectorportal_thinglist; node; node = node->m_snext) reserve(node->m_thing);
	}
	// [Karto] r_worldmode_publishsplit: helpers answer the record test for undecided actors over their sector ranges;
	// the walk below makes the first-visit decisions in list order, so records and copies come out as before. With
	// reject stats on, the walk stays on this thread.
	const bool split = r_worldmode_publishsplit;
	const bool splitWalk = split && r_sprite_records && !r_sprite_gpu_rejectstats;
	const size_t copiesBefore = snap.Copies.size();
	if (splitWalk)
	{
		PublishWalkSectors.clear();
		for (uint32_t s = 0; s < sectors; ++s) if (snap.Included[s]) PublishWalkSectors.push_back(s);
		PublishWalkCounts.resize(PublishWalkSectors.size());
		const uint32_t stamp = snap.Stamp;
		auto gather = [&](int part, int parts)
		{
			const size_t count = PublishWalkSectors.size();
			auto& out = PublishWalkParts[part];
			out.clear();
			for (size_t i = count * part / parts, end = count * (part + 1) / parts; i < end; ++i)
			{
				uint32_t n = 0;
				for (auto node = level->sectors[PublishWalkSectors[i]].touching_renderthings; node; node = node->m_snext, ++n)
				{
					AActor* actor = node->m_thing;
					const bool stamped = actor->PublishStamp == stamp;
					out.push_back({ actor, actor->Sector, !stamped && WorldModeSpriteRecordEligible(actor), stamped,
						node->m_tprev != nullptr || node->m_tnext != nullptr });
				}
				PublishWalkCounts[i] = n;
			}
		};
		if (PublishWalkSectors.size() >= 64)
			WorldModeRunParts(std::clamp(*r_worldmode_publishthreads, 1, PublishWalkMaxParts), gather);
		else
		{
			auto& all = PublishWalkParts[0];
			all.clear();
			for (size_t i = 0; i < PublishWalkSectors.size(); ++i)
			{
				uint32_t n = 0;
				for (auto node = level->sectors[PublishWalkSectors[i]].touching_renderthings; node; node = node->m_snext, ++n)
					all.push_back({ node->m_thing, nullptr, false, false, node->m_tprev != nullptr || node->m_tnext != nullptr });
				PublishWalkCounts[i] = n;
			}
			for (int part = 1; part < PublishWalkMaxParts; ++part) PublishWalkParts[part].clear();
			auto test = [&](int part, int parts)
			{
				for (size_t i = all.size() * part / parts, end = all.size() * (part + 1) / parts; i < end; ++i)
				{
					auto& entry = all[i];
					AActor* actor = entry.Actor;
					entry.Sector = actor->Sector;
					entry.Stamped = actor->PublishStamp == stamp;
					entry.Eligible = !entry.Stamped && WorldModeSpriteRecordEligible(actor);
				}
			};
			WorldModeRunParts(std::clamp(all.size() >= 1024 ? *r_worldmode_publishthreads : 1, 1, PublishWalkMaxParts), test);
		}
	}
	size_t walkSector = 0, walkPart = 0, walkAt = 0;
	auto visitListed = [&](AActor* actor, int eligible)
	{
		if (actor->PublishStamp != snap.Stamp)
		{
			if (eligible < 0 ? r_sprite_records && WorldModeSpriteRecordEligible(actor) : eligible != 0)
			{
				actor->PublishStamp = snap.Stamp;
				actor->PublishIndex = -1 - int32_t(snap.SpriteRecords.size());
				auto& r = snap.SpriteRecords.emplace_back();
				r.Live = actor;
				r.Sector = actor->Sector;
			}
			else reserve(actor);
		}
		if (oneWalk) snap.ListedOrder.push_back(actor->PublishIndex);
	};
	PublishStampWrites.clear();
	auto visitEntry = [&](const FPublishWalkEntry& entry)
	{
		if (entry.Multi || entry.Stamped || !entry.Eligible)
		{
			visitListed(entry.Actor, entry.Eligible);
			return;
		}
		const int32_t index = -1 - int32_t(snap.SpriteRecords.size());
		auto& r = snap.SpriteRecords.emplace_back();
		r.Live = entry.Actor;
		r.Sector = entry.Sector;
		PublishStampWrites.push_back({ entry.Actor, index });
		if (oneWalk) snap.ListedOrder.push_back(index);
	};
	for (uint32_t s = 0; s < sectors; ++s)
	{
		if (oneWalk) snap.ListedStart[s] = uint32_t(snap.ListedOrder.size()), snap.PortalStart[s] = uint32_t(snap.PortalOrder.size());
		if (!snap.Included[s]) continue;
		if (splitWalk)
		{
			for (uint32_t n = PublishWalkCounts[walkSector++]; n > 0; --n)
			{
				while (walkAt == PublishWalkParts[walkPart].size()) ++walkPart, walkAt = 0;
				visitEntry(PublishWalkParts[walkPart][walkAt++]);
			}
		}
		else for (auto node = level->sectors[s].touching_renderthings; node; node = node->m_snext) visitListed(node->m_thing, -1);
		if (oneWalk)
		{
			for (auto node = level->sectors[s].sectorportal_thinglist; node; node = node->m_snext)
				snap.PortalOrder.push_back(node->m_thing->PublishIndex);
		}
	}
	{
		const uint32_t stamp = snap.Stamp;
		auto writeStamps = [&](int part, int parts)
		{
			const size_t count = PublishStampWrites.size();
			for (size_t i = count * part / parts, end = count * (part + 1) / parts; i < end; ++i)
			{
				PublishStampWrites[i].Actor->PublishStamp = stamp;
				PublishStampWrites[i].Actor->PublishIndex = PublishStampWrites[i].Index;
			}
		};
		WorldModeRunParts(std::clamp(PublishStampWrites.size() >= 1024 ? *r_worldmode_publishthreads : 1, 1, PublishWalkMaxParts), writeStamps);
	}
	if (splitWalk && r_worldmode_publishwalkverify)
	{
		std::unordered_set<AActor*> seen;
		for (size_t i = 0; i < copiesBefore; ++i) seen.insert(snap.Copies[i].first);
		size_t record = 0, copy = copiesBefore;
		bool same = true;
		for (uint32_t s = 0; s < sectors; ++s)
		{
			if (!snap.Included[s]) continue;
			for (auto node = level->sectors[s].touching_renderthings; node; node = node->m_snext)
			{
				AActor* actor = node->m_thing;
				if (!seen.insert(actor).second) continue;
				if (WorldModeSpriteRecordEligible(actor)) same = same && record < snap.SpriteRecords.size() && snap.SpriteRecords[record++].Live == actor;
				else same = same && copy < snap.Copies.size() && snap.Copies[copy++].first == actor;
			}
		}
		++PublishWalkChecks;
		PublishWalkMismatches += !(same && record == snap.SpriteRecords.size() && copy == snap.Copies.size());
	}
	snap.Arena.resize(blocks);
	PublishPool.Run(snap.Copies.size() >= 1024 ? *r_worldmode_publishthreads : 1, WorldModeCopyActorsPart, &snap);
	auto copyOf = [&](AActor* actor) { return reinterpret_cast<AActor*>(&snap.Arena[size_t(actor->PublishIndex) + 1]); };
	auto copied = [&](AActor* live) { return live != nullptr && live->PublishStamp == snap.Stamp && live->PublishIndex >= 0; };
	auto listWalk = [&](decltype(snap.Listed)& listedOut, decltype(snap.Portal)& portalOut, std::vector<uint32_t>& listedStart,
		std::vector<uint32_t>& portalStart)
	{
		for (uint32_t s = 0; s < sectors; ++s)
		{
			auto& sector = level->sectors[s];
			listedStart[s] = uint32_t(listedOut.size());
			portalStart[s] = uint32_t(portalOut.size());
			if (!snap.Included[s]) continue;
			for (auto node = sector.touching_renderthings; node; node = node->m_snext)
			{
				AActor* actor = node->m_thing;
				if (actor->PublishIndex < 0) listedOut.push_back({ nullptr, &snap.SpriteRecords[size_t(-1 - actor->PublishIndex)] });
				else listedOut.push_back({ copyOf(actor), nullptr });
			}
			for (auto node = sector.sectorportal_thinglist; node; node = node->m_snext) portalOut.push_back(copyOf(node->m_thing));
		}
		listedStart[sectors] = uint32_t(listedOut.size());
		portalStart[sectors] = uint32_t(portalOut.size());
	};
	if (oneWalk)
	{
		for (int32_t index : snap.ListedOrder)
		{
			if (index < 0) snap.Listed.push_back({ nullptr, &snap.SpriteRecords[size_t(-1 - index)] });
			else snap.Listed.push_back({ reinterpret_cast<AActor*>(&snap.Arena[size_t(index) + 1]), nullptr });
		}
		for (int32_t index : snap.PortalOrder) snap.Portal.push_back(reinterpret_cast<AActor*>(&snap.Arena[size_t(index) + 1]));
		snap.ListedStart[sectors] = uint32_t(snap.Listed.size());
		snap.PortalStart[sectors] = uint32_t(snap.Portal.size());
		if (r_worldmode_publishwalkverify)
		{
			decltype(snap.Listed) listed;
			decltype(snap.Portal) portal;
			std::vector<uint32_t> listedStart(sectors + 1), portalStart(sectors + 1);
			listWalk(listed, portal, listedStart, portalStart);
			++PublishWalkChecks;
			bool same = listed.size() == snap.Listed.size() && portal == snap.Portal && listedStart == snap.ListedStart && portalStart == snap.PortalStart;
			for (size_t i = 0; same && i < listed.size(); ++i)
				same = memcmp(&listed[i], &snap.Listed[i], sizeof(listed[i])) == 0;
			PublishWalkMismatches += !same;
		}
	}
	else listWalk(snap.Listed, snap.Portal, snap.ListedStart, snap.PortalStart);
	snap.CopiedActors += snap.Copies.size();
	snap.RecordedActors += snap.SpriteRecords.size();
	snap.PlayerValid = false;
	if (publishPlayer) WorldModeCopyPlayer(snap, player);
	size_t quakeBlocks = 0;
	for (auto quake : quakes) quakeBlocks += (size_t(quake->GetClass()->Size) + 15) / 16;
	snap.QuakeArena.resize(quakeBlocks);
	snap.Quakes.clear();
	quakeBlocks = 0;
	for (auto quake : quakes)
	{
		auto* quakeCopy = reinterpret_cast<DEarthquake*>(&snap.QuakeArena[quakeBlocks]);
		memcpy((void*)quakeCopy, (const void*)quake, quake->GetClass()->Size);
		quakeBlocks += (size_t(quake->GetClass()->Size) + 15) / 16;
		AActor* spot = quake->m_Spot;
		*reinterpret_cast<AActor**>(&quakeCopy->m_Spot) = spot != nullptr ? copyOf(spot) : nullptr;
		snap.Quakes.push_back(quakeCopy);
	}


	{
		const uint32_t subsectors = level->subsectors.Size();
		WorldModePublishStagedParticles(level, snap);
		size_t visualBlocks = 0;
		for (auto sp = level->VisualThinkerHead; sp != nullptr; sp = sp->GetNext()) visualBlocks += (size_t(sp->GetClass()->Size) + 15) / 16;
		snap.VisualArena.resize(visualBlocks);
		std::vector<std::pair<uint32_t, DVisualThinker*>> visuals;
		visualBlocks = 0;
		for (auto sp = level->VisualThinkerHead; sp != nullptr; sp = sp->GetNext())
		{
			if (!sp->PT.subsector) sp->PT.subsector = level->PointInRenderSubsector(sp->PT.Pos);
			auto* copy = reinterpret_cast<DVisualThinker*>(&snap.VisualArena[visualBlocks]);
			memcpy((void*)copy, (const void*)sp, sp->GetClass()->Size);
			visualBlocks += (size_t(sp->GetClass()->Size) + 15) / 16;
			visuals.emplace_back(sp->PT.subsector->Index(), copy);
		}
		std::stable_sort(visuals.begin(), visuals.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
		snap.VisualSubsectors.clear();
		snap.VisualCopies.clear();
		for (auto& [ss, copy] : visuals)
		{
			snap.VisualSubsectors.push_back(ss);
			snap.VisualCopies.push_back(copy);
		}
	}

	snap.SectorCopy.assign(sectors, -1);
	std::vector<int> wanted;
	auto want = [&](const sector_t* sector)
	{
		if (!sector || sector < level->sectors.Data() || sector >= level->sectors.Data() + sectors) return;
		const int index = sector->Index();
		if (snap.SectorCopy[index] == -1)
		{
			snap.SectorCopy[index] = -2;
			wanted.push_back(index);
		}
	};
	for (uint32_t s = 0; s < sectors; ++s)
		if (snap.Included[s]) want(&level->sectors[s]);
	for (auto& [actor, offset] : snap.Copies) want(actor->Sector);
	for (auto& r : snap.SpriteRecords) want(r.Sector);
	const size_t sectorBlocks = (sizeof(sector_t) + 15) / 16;
	snap.SectorArena.resize(wanted.size() * sectorBlocks);
	for (size_t i = 0; i < wanted.size(); ++i)
	{
		snap.SectorCopy[wanted[i]] = int32_t(i * sectorBlocks);
		memcpy((void*)&snap.SectorArena[i * sectorBlocks], (const void*)&level->sectors[wanted[i]], sizeof(sector_t));
	}
	snap.CopiedSectors += wanted.size();
	auto sectorCopyOf = [&](sector_t* sector) -> sector_t*
	{
		if (!sector || sector < level->sectors.Data() || sector >= level->sectors.Data() + sectors) return sector;
		const int32_t at = snap.SectorCopy[sector->Index()];
		return at >= 0 ? reinterpret_cast<sector_t*>(&snap.SectorArena[at]) : sector;
	};
	for (auto& [actor, offset] : snap.Copies)
	{
		auto* copy = reinterpret_cast<AActor*>(&snap.Arena[offset + 1]);
		copy->Sector = sectorCopyOf(actor->Sector);
	}
	auto fillRecords = [&](int part, int parts)
	{
		const size_t count = snap.SpriteRecords.size();
		for (size_t ri = count * part / parts, end = count * (part + 1) / parts; ri < end; ++ri)
		{
			auto& r = snap.SpriteRecords[ri];
			AActor* actor = r.Live;
			r.Sector = sectorCopyOf(actor->Sector);
			r.section = actor->section;
			r.PortalGroup = actor->Sector->PortalGroup;
			r.Pos = actor->Pos();
			r.Prev = actor->Prev;
			r.WorldOffset = actor->WorldOffset;
			r.Scale = actor->Scale;
			r.PrevScale = actor->PrevScale;
			r.SpriteOffset = actor->SpriteOffset;
			r.Alpha = actor->Alpha;
			r.PrevAlpha = actor->PrevAlpha;
			r.Floorclip = actor->Floorclip;
			r.Height = actor->Height;
			r.floorz = actor->floorz;
			r.ceilingz = actor->ceilingz;
			r.radius = actor->radius;
			r.RenderRadius = actor->RenderRadius();
			r.MapSection = actor->subsector->mapsection;
			r.validcount = 0;
			r.FloatBobStrength = actor->FloatBobStrength;
			r.FloatBobFactor = actor->FloatBobFactor;
			r.Angles = actor->Angles;
			r.SpriteAngle = actor->SpriteAngle;
			r.SpriteRotation = actor->SpriteRotation;
			r.Class = actor->GetClass();
			r.RenderStyle = actor->RenderStyle;
			r.Translation = actor->Translation;
			r.fillcolor = actor->fillcolor;
			r.RenderRequired = actor->RenderRequired;
			r.RenderHidden = actor->RenderHidden;
			r.SpawnOrder = actor->SpawnOrder;
			r.renderflags = actor->renderflags;
			r.renderflags2 = actor->renderflags2;
			r.flags = actor->flags;
			r.flags2 = actor->flags2;
			r.flags3 = actor->flags3;
			r.flags5 = actor->flags5;
			r.flags7 = actor->flags7;
			r.flags8 = actor->flags8;
			r.flags9 = actor->flags9;
			r.sprite = actor->sprite;
			r.waterlevel = actor->waterlevel;
			r.LightLevel = actor->LightLevel;
			r.frame = actor->frame;
			r.FloatBobPhase = actor->FloatBobPhase;
			r.Moving = !actor->Vel.isZero();
			r.IsInventory = actor->IsKindOf(NAME_Inventory);
			r.ClientSide = actor->IsClientSide();
			r.Instance = -1;
		}
	};
	WorldModeRunParts(split && snap.SpriteRecords.size() >= 2048 ? *r_worldmode_publishthreads : 1, fillRecords);
	snap.SpriteInstances.clear();
	snap.SpriteKeys.clear();
	snap.PendingGpuFrames.clear();
	const bool keyHash = r_worldmode_publishkeyhash;
	uint32_t keyMask = 0;
	if (keyHash && r_sprite_gpu)
	{
		size_t size = 64;
		while (size < snap.SpriteRecords.size() * 2) size <<= 1;
		snap.SpriteKeyTable.assign(size, UINT32_MAX);
		keyMask = uint32_t(size - 1);
	}
	if (r_sprite_gpu)
	{
		const bool rejectStats = r_sprite_gpu_rejectstats;
		if (rejectStats) ++GpuRejectPublishes;
		const int parts = rejectStats || !split || snap.SpriteRecords.size() < 2048 ? 1 : std::clamp(*r_worldmode_publishthreads, 1, PublishWalkMaxParts);
		auto build = [&](int part, int parts)
		{
			auto& instOut = InstanceParts[part];
			auto& keysOut = InstanceKeyParts[part];
			auto& pendingOut = PendingFrameParts[part];
			instOut.clear();
			keysOut.clear();
			pendingOut.clear();
			const size_t count = snap.SpriteRecords.size();
			for (size_t ri = count * part / parts, end = count * (part + 1) / parts; ri < end; ++ri)
			{
				auto& r = snap.SpriteRecords[ri];
				sector_t* live = r.Live->Sector;
				auto reject = [&](int reason) { if (rejectStats) GpuRejectCount(r.Class, reason); };
				if ((r.renderflags2 & (RF2_INTERPOLATESCALE | RF2_INTERPOLATEALPHA)) || live->e->XFloor.ffloors.Size() != 0 ||
					live->GetHeightSec() != nullptr) { reject(GR_Geometry); continue; }
				uint8_t blend = 0;
				bool shaded = false;
				const bool vanilla = r_UseVanillaTransparency && (r.renderflags & RF_ZDOOMTRANS);
				// [Karto] STYLE_Normal carries STYLEF_Alpha1: ProcessRecord takes its alpha as 1 whatever the actor's, so
				// any alpha draws opaque.
				if (r.RenderStyle == LegacyRenderStyles[STYLE_Normal] && (r.Alpha >= 1.0 || r_sprite_gpu_opaquetrans)) blend = 0;
				else if (r_sprite_gpu_opaquetrans && r.RenderStyle == LegacyRenderStyles[STYLE_Translucent] && r.Alpha >= 1.0 - FLT_EPSILON &&
					!vanilla) blend = 0;
				else if (r_sprite_gpu_trans && r.RenderStyle == LegacyRenderStyles[STYLE_Translucent] && r.Alpha > 0.0 &&
					r.Alpha < 1.0 - FLT_EPSILON && !vanilla) blend = 1;
				else if (r_sprite_gpu_trans && r.RenderStyle == LegacyRenderStyles[STYLE_Add] && r.Alpha > 0.0 &&
					(r.Alpha <= 1.0 || r_sprite_gpu_styles) && !vanilla) blend = 2;
				else if (r_sprite_gpu_trans && r_sprite_gpu_styles && r.RenderStyle == LegacyRenderStyles[STYLE_Shaded] && r.Alpha > 0.0 && !vanilla)
					blend = 1, shaded = true;
				else if (r_sprite_gpu_trans && r_sprite_gpu_styles && r.RenderStyle == LegacyRenderStyles[STYLE_AddShaded] && r.Alpha > 0.0 && !vanilla)
					blend = 2, shaded = true;
				else
				{
					reject(GR_Style);
					if (rejectStats)
						++GpuRejectStyles[(uint64_t(r.RenderStyle.AsDWORD) << 32) | (uint64_t(std::clamp(int(r.Alpha * 100 + 0.5), 0, 0x7fff)) << 1) |
							((r.renderflags & RF_ZDOOMTRANS) ? 1 : 0)];
					continue;
				}
				if (r.RenderRequired != 0 || r.RenderHidden != 0) { reject(GR_RenderFilter); continue; }
				if (!r_sprite_gpu_billboard && !r.SpriteOffset.isZero()) { reject(GR_Offset); continue; }
				if (WorldModeSectorSpriteGlow(live)) { reject(GR_Glow); continue; }
				const bool xyBillboard = (r.renderflags & (RF_FORCEXYBILLBOARD | RF_FORCEYBILLBOARD)) == RF_FORCEXYBILLBOARD;
				if (!r_sprite_gpu_billboard && xyBillboard) { reject(GR_Offset); continue; }
				const spritedef_t& def = sprites[r.sprite];
				if (unsigned(r.frame) >= def.numframes) { reject(GR_Frame); continue; }
				const uint32_t frameIndex = def.spriteframes + r.frame;
				const uint64_t gpuKey = (uint64_t(uint32_t(r.Translation.index())) << 32) | frameIndex;
				// [Karto] No built entry or table slot yet: the main thread adds both after this publish. A slot whose
				// bindless IDs failed is found at draw time, since frames write that state while a worker publish runs.
				const FSpriteFrameEntry* entry = WorldModeBuiltSpriteFrameEntry(frameIndex);
				auto gpuFrame = GpuSpriteFrames.Index.find(gpuKey);
				if (entry == nullptr || gpuFrame == GpuSpriteFrames.Index.end())
				{
					if (entry == nullptr || entry->Usable) pendingOut.push_back(gpuKey);
					reject(GR_Pending);
					continue;
				}
				if (!entry->Usable) { reject(GR_Unusable); continue; }
				const bool softTexture = entry->TranslucentRotations != 0;
				if (!shaded && softTexture && (!r_sprite_gpu_soft || entry->TranslucentRotations != 0xffff)) { reject(GR_SoftMixed); continue; }
				const bool soft = shaded || softTexture;
				const bool promoted = soft && blend == 0;
				if (promoted) blend = 1;
				if ((r.renderflags & RF_FULLBRIGHT) && entry->FullbrightDisabled != 0 && entry->FullbrightDisabled != 0xffff) { reject(GR_BrightMixed); continue; }
				FSpriteInstance in = {};
				for (int k = 0; k < 3; ++k)
				{
					in.Prev[k] = float(r.Prev[k]);
					in.Pos[k] = float(r.Pos[k]);
					in.WorldOffset[k] = float(r.WorldOffset[k]);
				}
				in.Scale[0] = float(r.Scale.X);
				in.Scale[1] = float(r.Scale.Y);
				in.Floorclip = float(r.Floorclip);
				in.Height = float(r.Height);
				in.BobPhase = float(r.FloatBobPhase);
				in.BobFactor = float(r.FloatBobFactor);
				in.BobStrength = float(r.FloatBobStrength);
				in.Angle = float(((r.flags7 & MF7_SPRITEANGLE) ? r.SpriteAngle : r.Angles.Yaw + r.SpriteRotation).Degrees());
				in.ClipBottom = float(r.Sector->floorplane.ZatPoint(r.Pos.XY()) - r.Floorclip);
				const secplane_t& ceiling = r.Sector->ceilingplane;
				in.Ceiling[0] = float(ceiling.Normal().X);
				in.Ceiling[1] = float(ceiling.Normal().Y);
				in.Ceiling[2] = float(ceiling.negiC);
				in.Ceiling[3] = float(ceiling.D);
				const secplane_t& floor = r.Sector->floorplane;
				in.Floor[0] = float(floor.Normal().X);
				in.Floor[1] = float(floor.Normal().Y);
				in.Floor[2] = float(floor.negiC);
				in.Floor[3] = float(floor.D);
				in.Radius = float(r.radius);
				in.Frame = frameIndex;
				const bool clipThing = ((r.flags3 & MF3_ISMONSTER) || r.IsInventory) && ((r.flags & MF_ICECORPSE) || !(r.flags & MF_CORPSE));
				in.Flags = ((r.renderflags & RF_DONTINTERPOLATE) ? SIF_DontInterpolate : 0) | ((r.flags7 & MF7_SPRITEANGLE) ? SIF_SpriteAngle : 0) |
					((r.renderflags & RF_XFLIP) ? SIF_XFlip : 0) | ((r.renderflags & RF_SPRITEFLIP) ? SIF_SpriteFlip : 0) |
					((r.renderflags & RF_YFLIP) ? SIF_YFlip : 0) | ((r.flags2 & MF2_FLOATBOB) ? SIF_FloatBob : 0) |
					(r.ClientSide ? SIF_ClientSide : 0) | (clipThing ? SIF_ClipThing : 0) | ((r.flags & MF_CORPSE) ? SIF_Corpse : 0) |
					((r.flags3 & MF3_ISMONSTER) ? SIF_Monster : 0) | ((r.flags & MF_NOGRAVITY) ? SIF_NoGravity : 0) |
					(r.Moving ? SIF_Moving : 0) | (r.waterlevel >= 1 && r.waterlevel <= 2 ? SIF_ShallowWater : 0);
				if (r.renderflags & RF_ROLLSPRITE)
				{
					in.Flags |= SIF_Roll | ((r.renderflags & RF_ROLLCENTER) ? SIF_RollCenter : 0) |
						((r.renderflags2 & RF2_STRETCHPIXELS) ? 0 : SIF_RollScale);
					in.Roll = float(r.Angles.Roll.Degrees());
				}
				if (xyBillboard) in.Flags |= SIF_XYBillboard;
				in.Offset[0] = float(r.renderflags2 & RF2_FLIPSPRITEOFFSETX ? r.SpriteOffset.X : -r.SpriteOffset.X);
				in.Offset[1] = float(r.renderflags2 & RF2_FLIPSPRITEOFFSETY ? r.SpriteOffset.Y : -r.SpriteOffset.Y);
				in.Translation = r.Translation.index();
				in.Record = int32_t(ri);
				in.GpuFrame = gpuFrame->second;
				const bool fullbright = (r.flags5 & MF5_BRIGHT) || ((r.renderflags & RF_FULLBRIGHT) && entry->FullbrightDisabled == 0);
				in.LightZ = float(r.Pos.Z + r.Height / 2);
				if (fullbright || (r.renderflags2 & RF2_NODYNAMICLIGHTING)) in.Flags |= SIF_NoDynLight;
				int lightlevel = 255;
				if (!fullbright)
				{
					int level = r.Sector->GetSpriteLight();
					if (r.flags8 & MF8_ADDLIGHTLEVEL) level += r.LightLevel;
					else if (r.LightLevel > -1) level = r.LightLevel;
					lightlevel = RescaleLightLevel(level);
				}
				const float alpha = blend == 0 || promoted || (blend == 1 && r.Alpha >= 1.0 - FLT_EPSILON) ? 1.f : float(r.Alpha);
				const uint32_t fill = shaded ? uint32_t(r.fillcolor) : 0;
				keysOut.push_back({ r.Sector, lightlevel, fullbright, blend, alpha, soft, shaded, fill });
				r.Instance = int32_t(instOut.size());
				instOut.push_back(in);
				reject(GR_Instanced);
			}
		};
		WorldModeRunParts(parts, build);
		auto assignKey = [&](const FSpriteAppearanceKey& k) -> uint32_t
		{
			auto linearKey = [&]()
			{
				uint32_t key = 0;
				while (key < snap.SpriteKeys.size() && !SameSpriteKey(snap.SpriteKeys[key], k.Sector, k.LightLevel, k.Fullbright, k.Blend, k.Alpha,
					k.SoftTexture, k.Shaded, k.FillColor)) ++key;
				return key;
			};
			uint32_t key;
			// [Karto] NaN alpha never compares equal, so it always gets a new key, as in the linear search; -0 and +0
			// compare equal, so they hash alike.
			if (keyHash && k.Alpha == k.Alpha)
			{
				uint32_t alphaBits = 0;
				if (k.Alpha != 0) memcpy(&alphaBits, &k.Alpha, sizeof(alphaBits));
				uint64_t h = uint64_t(uintptr_t(k.Sector)) * 0x9E3779B97F4A7C15ull;
				h ^= (uint64_t(uint32_t(k.LightLevel)) << 32 | alphaBits) * 0xC2B2AE3D27D4EB4Full;
				h ^= (uint64_t(k.FillColor) << 8 | uint64_t(k.Fullbright) | uint64_t(k.Blend) << 1 | uint64_t(k.SoftTexture) << 3 | uint64_t(k.Shaded) << 4) * 0x165667B19E3779F9ull;
				h ^= h >> 31;
				uint32_t slot = uint32_t(h) & keyMask;
				for (;; slot = (slot + 1) & keyMask)
				{
					const uint32_t at = snap.SpriteKeyTable[slot];
					if (at == UINT32_MAX)
					{
						key = uint32_t(snap.SpriteKeys.size());
						snap.SpriteKeyTable[slot] = key;
						break;
					}
					if (SameSpriteKey(snap.SpriteKeys[at], k.Sector, k.LightLevel, k.Fullbright, k.Blend, k.Alpha, k.SoftTexture, k.Shaded, k.FillColor))
					{
						key = at;
						break;
					}
				}
				if (r_worldmode_publishwalkverify)
				{
					++PublishKeyChecks;
					PublishKeyMismatches += linearKey() != key;
				}
			}
			else key = k.Alpha == k.Alpha ? linearKey() : uint32_t(snap.SpriteKeys.size());
			if (key == snap.SpriteKeys.size()) snap.SpriteKeys.push_back(k);
			return key;
		};
		uint32_t bases[PublishWalkMaxParts] = {};
		for (int part = 0; part < parts; ++part)
		{
			snap.PendingGpuFrames.insert(snap.PendingGpuFrames.end(), PendingFrameParts[part].begin(), PendingFrameParts[part].end());
			bases[part] = uint32_t(snap.SpriteInstances.size());
			const auto& keys = InstanceKeyParts[part];
			for (size_t i = 0; i < keys.size(); ++i)
			{
				snap.SpriteInstances.push_back(InstanceParts[part][i]);
				snap.SpriteInstances.back().Appearance = assignKey(keys[i]);
			}
		}
		auto rebase = [&](int part, int parts)
		{
			if (bases[part] == 0) return;
			for (const auto& in : InstanceParts[part]) snap.SpriteRecords[size_t(in.Record)].Instance += int32_t(bases[part]);
		};
		if (parts > 1) WorldModeRunParts(parts, rebase);
	}
	snap.Level = level;
	snap.StartTime = level->starttime;
	snap.MapTime = level->maptime;
	++snap.Publishes;
	snap.PublishNS += I_nsTime() - started;
}

static uint64_t GpuParticleSerial = 1;

// [Karto] Frames draw the published copies, so at the swap the last drawn corners go back into the live Vert and the
// new copy's Prev; otherwise PSPF_INTERPOLATE layers shrink each tic.
CVAR(Bool, r_worldmode_publishpsprev, true, 0)

static void WorldModeCarryWeaponCorners(FActorSnapshot& oldFront, FActorSnapshot& newFront)
{
	if (!r_worldmode_publishpsprev || !oldFront.PlayerValid || !newFront.PlayerValid) return;
	auto* oldPlayer = reinterpret_cast<player_t*>(oldFront.PlayerBytes);
	auto* newPlayer = reinterpret_cast<player_t*>(newFront.PlayerBytes);
	for (DPSprite* drawn = oldPlayer->psprites; drawn != nullptr; drawn = drawn->GetNext())
	{
		const int id = drawn->GetID();
		for (DPSprite* copy = newPlayer->psprites; copy != nullptr; copy = copy->GetNext())
			if (copy->GetID() == id) { copy->Prev = drawn->Vert; break; }
		for (DPSprite* live = players[consoleplayer].psprites; live != nullptr; live = live->GetNext())
			if (live->GetID() == id) { live->Vert = drawn->Vert; break; }
	}
}

static void WorldModeFinishPublish(FLevelLocals* level)
{
	WorldModePublishSectors(level);
	FActorSnapshot* const oldFront = FrontSnapshot;
	// [Karto] The last frame's view state points into the old front snapshot, so resolve it to live actors before the
	// swap. An old copy's live actor may be freed: compare by address only.
	AActor* const viewCamera = WorldModeLiveActor(r_viewpoint.camera);
	AActor* const viewActor = WorldModeLiveActor(r_viewpoint.ViewActor);
	const bool viewWasCamera = r_viewpoint.ViewActor != nullptr && r_viewpoint.ViewActor == r_viewpoint.camera;
	FrontSnapshot = &BackSnapshot();
	auto& snap = *FrontSnapshot;
	if (oldFront != FrontSnapshot) WorldModeCarryWeaponCorners(*oldFront, snap);
	// [Karto] Found by address in the new snapshot's copy list, never by reading the live actor: it may have been freed
	// (map reload, or collected since the last publish), and reading its stamp would be a use after free.
	auto newCopyOf = [&](AActor* live) -> AActor*
	{
		if (live == nullptr) return nullptr;
		for (auto& [actor, offset] : snap.Copies)
			if (actor == live) return reinterpret_cast<AActor*>(&snap.Arena[offset + 1]);
		return nullptr;
	};
	AActor* const cameraCopy = newCopyOf(viewCamera);
	AActor* const viewActorCopy = newCopyOf(viewActor);
	const bool cameraCopied = cameraCopy != nullptr, viewActorCopied = viewActorCopy != nullptr;
	if (cameraCopied) r_viewpoint.camera = cameraCopy;
	if (viewActorCopied) r_viewpoint.ViewActor = viewActorCopy;
	player_t* player = &players[consoleplayer];
	if (!cameraCopied || (!viewActorCopied && r_viewpoint.ViewActor != nullptr))
	{
		AActor* fallback = snap.PlayerValid ? reinterpret_cast<player_t*>(snap.PlayerBytes)->camera.Get() : player->camera.Get();
		if (!cameraCopied) r_viewpoint.camera = fallback;
		if (!viewActorCopied && r_viewpoint.ViewActor != nullptr) r_viewpoint.ViewActor = viewWasCamera ? r_viewpoint.camera : nullptr;
	}
	const uint32_t sectors = level->sectors.Size();
	snap.Blends.clear();
	for (int slot : WorldMode.BlendingPlanes)
	{
		const int s = slot / 2, plane = slot & 1;
		if (s >= int(sectors) || s >= int(snap.SectorCopy.size()) || snap.SectorCopy[s] < 0) continue;
		double d, texz;
		auto& live = level->sectors[s];
		if (!R_PlaneInterpolationStart(&live, plane == sector_t::ceiling, d, texz)) continue;
		const secplane_t& p = plane == sector_t::ceiling ? live.ceilingplane : live.floorplane;
		snap.Blends.push_back({ s, plane, d, texz, p.fD(), live.GetPlaneTexZ(plane) });
	}
	snap.LocalWorldTimer = level->LocalWorldTimer;
	WorldModePublishLights(level, snap);
	WorldModePublishParticleDelta(level, snap);
	++GpuParticleSerial;
	for (uint64_t key : snap.PendingGpuFrames)
	{
		const FSpriteFrameEntry* entry = WorldModeSpriteFrameEntry(uint32_t(key & 0xffffffffu));
		if (entry == nullptr || !entry->Usable) continue;
		auto& t = GpuSpriteFrames;
		if (t.Index.emplace(key, uint32_t(t.State.size())).second)
		{
			t.State.push_back(0);
			t.Words.resize(t.State.size() * GpuSpriteFrameWords, 0);
			++t.Version;
		}
	}
}

// [Karto] The title screen and demo loop tick before any map is loaded: the level then has no subsectors and no
// particle pool, so the actor copy would read past empty arrays.
static bool WorldModeHasMap(const FLevelLocals* level)
{
	return level != nullptr && level->subsectors.Size() > 0 && level->Particles.Size() > 0;
}

void WorldModePublishActors(FLevelLocals* level)
{
	if (DecouplePublishHeld) return;
	if (!r_worldmode_snapshot || !r_worldmode || !WorldModeHasMap(level))
	{
		WorldModePublishSectors(level);
		ActorSnapshots[0].Level = ActorSnapshots[1].Level = nullptr;
		return;
	}
	static std::vector<uint8_t> included;
	WorldModePublishIncluded(level, included);
	WorldModeBuildPublish(level, BackSnapshot(), included);
	WorldModeFinishPublish(level);
}

// [Karto] Anything that changes published state on the main thread after the world tick (client logic, prediction)
// sends the publish back to the main thread.
CVAR(Bool, r_worldmode_publishworker, true, 0)
static std::vector<uint8_t> WorkerPublishIncluded;
extern uint64_t LightRecreates;	// dthinker.cpp: SetDynamicLights calls of the client pass
static uint64_t WorkerPublishRecreates = 0;
static FLevelLocals* WorkerPublishLevel = nullptr;
static bool WorkerPublishBuilt = false;
static uint64_t WorkerPublishes, WorkerPublishFallbacks, BusyFallbackSectors;

static bool WorldModeMainOnlyState(FLevelLocals* level)
{
	if (level->VisualThinkerHead != nullptr) return true;
	auto it = level->GetClientSideThinkerIterator<DThinker>();
	return it.Next() != nullptr;
}

bool WorldModePrepareWorkerPublish(FLevelLocals* level)
{
	WorkerPublishBuilt = false;
	WorkerPublishLevel = nullptr;
	if (!r_worldmode_publishworker || DecouplePublishHeld || !r_worldmode_snapshot || !r_worldmode || !WorldModeHasMap(level) ||
		WorldModeMainOnlyState(level)) return false;
	WorldModePublishIncluded(level, WorkerPublishIncluded);
	WorkerPublishLevel = level;
	WorkerPublishRecreates = LightRecreates;
	return true;
}

void WorldModeWorkerPublish()
{
	if (WorkerPublishLevel == nullptr) return;
	WorldModeBuildPublish(WorkerPublishLevel, BackSnapshot(), WorkerPublishIncluded);
	WorkerPublishBuilt = true;
}

CVAR(Bool, r_worldmode_publishworker_verify, false, 0)
static uint64_t WorkerVerifyRuns, WorkerVerifyMismatches[8];

static void WorldModeVerifyWorkerPublish(FLevelLocals* level)
{
	static FActorSnapshot check;
	const auto& got = *FrontSnapshot;
	WorldModeBuildPublish(level, check, WorkerPublishIncluded);
	check.LocalWorldTimer = level->LocalWorldTimer;
	WorldModePublishLights(level, check);
	++WorkerVerifyRuns;
	auto bad = [](int kind) { ++WorkerVerifyMismatches[kind]; };
	// 0: copied actors and their bytes
	if (got.Copies.size() != check.Copies.size() || got.Arena.size() != check.Arena.size()) bad(0);
	else
	{
		for (size_t i = 0; i < got.Copies.size(); ++i)
		{
			const auto& [live, offset] = got.Copies[i];
			if (live != check.Copies[i].first || offset != check.Copies[i].second) { bad(0); break; }
			const size_t size = live->GetClass()->Size;
			std::vector<uint8_t> a((const uint8_t*)&got.Arena[offset + 1], (const uint8_t*)&got.Arena[offset + 1] + size);
			std::vector<uint8_t> b((const uint8_t*)&check.Arena[offset + 1], (const uint8_t*)&check.Arena[offset + 1] + size);
			auto mask = [&](std::vector<uint8_t>& bytes, const void* field, size_t n)
			{
				const size_t at = (const uint8_t*)field - (const uint8_t*)live;
				memset(bytes.data() + at, 0, n);
			};
			for (auto* bytes : { &a, &b })
			{
				mask(*bytes, &live->PublishStamp, sizeof(live->PublishStamp));
				mask(*bytes, &live->PublishIndex, sizeof(live->PublishIndex));
				mask(*bytes, &live->Sector, sizeof(live->Sector));
				mask(*bytes, &live->player, sizeof(live->player));
			}
			if (a != b) { bad(0); break; }
		}
	}
	// 1: per-sector lists, as arena offsets and record indexes
	if (got.ListedStart != check.ListedStart || got.PortalStart != check.PortalStart || got.Listed.size() != check.Listed.size() ||
		got.Portal.size() != check.Portal.size()) bad(1);
	else
	{
		auto where = [](const FActorSnapshot& snap, const FSnapshotThing& t) -> int64_t
		{
			if (t.Record) return -1 - int64_t(t.Record - snap.SpriteRecords.data());
			return int64_t((const FActorSnapshotBlock*)t.Copy - snap.Arena.data());
		};
		for (size_t i = 0; i < got.Listed.size(); ++i)
			if (where(got, got.Listed[i]) != where(check, check.Listed[i])) { bad(1); break; }
		for (size_t i = 0; i < got.Portal.size(); ++i)
			if ((const FActorSnapshotBlock*)got.Portal[i] - got.Arena.data() != (const FActorSnapshotBlock*)check.Portal[i] - check.Arena.data()) { bad(1); break; }
	}
	// 2: sprite records
	if (got.SpriteRecords.size() != check.SpriteRecords.size()) bad(2);
	else
	{
		for (size_t i = 0; i < got.SpriteRecords.size(); ++i)
		{
			const auto& x = got.SpriteRecords[i];
			const auto& y = check.SpriteRecords[i];
			if (x.Live != y.Live || x.Sector->sectornum != y.Sector->sectornum || x.Pos != y.Pos || x.Prev != y.Prev || x.sprite != y.sprite ||
				x.frame != y.frame || x.renderflags != y.renderflags || x.flags != y.flags || x.Translation.index() != y.Translation.index() || x.Scale != y.Scale ||
				x.Alpha != y.Alpha || x.Angles.Yaw != y.Angles.Yaw || x.SpawnOrder != y.SpawnOrder || x.floorz != y.floorz ||
				x.RenderStyle.AsDWORD != y.RenderStyle.AsDWORD || x.Moving != y.Moving) { bad(2); break; }
		}
	}
	// 3: particles, chain by chain (the delta appends where today's copy would have pushed first)
	if (got.Particles.size() != check.Particles.size() || got.ParticleHeads.size() != check.ParticleHeads.size())
		bad(3);
	else
	{
		bool same = true;
		for (size_t ss = 0; ss < got.ParticleHeads.size() && same; ++ss)
		{
			uint16_t a = got.ParticleHeads[ss], b = check.ParticleHeads[ss];
			while (same && (a != NO_PARTICLE || b != NO_PARTICLE))
			{
				if (a == NO_PARTICLE || b == NO_PARTICLE) { same = false; break; }
				// [Karto] Raw bytes (a struct copy need not carry padding), with the chain link masked.
				uint8_t x[sizeof(particle_t)], y[sizeof(particle_t)];
				memcpy(x, &got.Particles[a], sizeof(particle_t));
				memcpy(y, &check.Particles[b], sizeof(particle_t));
				const size_t link = (const uint8_t*)&got.Particles[a].snext - (const uint8_t*)&got.Particles[a];
				memset(x + link, 0, sizeof(particle_t::snext));
				memset(y + link, 0, sizeof(particle_t::snext));
				same = memcmp(x, y, sizeof(particle_t)) == 0;
				static int reported = 0;
				if (!same && reported < 6)
				{
					size_t at = 0;
					while (at < sizeof(particle_t) && x[at] == y[at]) ++at;
					++reported;
					Printf("PUBLISHVERIFYPARTICLE ss=%zu got=%u check=%u appended=%d byte=%zu of %zu gotcount=%zu\n", ss, unsigned(a), unsigned(b),
						int(a >= got.Particles.size() - ParticleDeltaLast), at, sizeof(particle_t), got.Particles.size());
				}
				a = got.Particles[a].snext;
				b = check.Particles[b].snext;
			}
		}
		if (!same) bad(3);
	}
	// 4: sector copies
	if (got.SectorCopy != check.SectorCopy) bad(4);
	// 5: lights, quakes, visual thinkers
	bool lightsSame = got.Lights.size() == check.Lights.size() && got.Quakes.size() == check.Quakes.size() &&
		got.VisualCopies.size() == check.VisualCopies.size();
	for (size_t i = 0; lightsSame && i < got.Lights.size(); ++i)
		lightsSame = got.Lights[i]->Pos == check.Lights[i]->Pos && got.Lights[i]->radius == check.Lights[i]->radius &&
			got.Lights[i]->m_currentRadius == check.Lights[i]->m_currentRadius && got.Lights[i]->m_lastUpdate == check.Lights[i]->m_lastUpdate &&
			got.Lights[i]->IsActive() == check.Lights[i]->IsActive() && got.Lights[i]->GetLightType() == check.Lights[i]->GetLightType();
	if (!lightsSame) bad(5);
	// 6: player
	if (got.PlayerValid != check.PlayerValid) bad(6);
	else if (got.PlayerValid)
	{
		const auto* gp = reinterpret_cast<const player_t*>(got.PlayerBytes);
		const auto* cp = reinterpret_cast<const player_t*>(check.PlayerBytes);
		uint8_t a[sizeof(player_t)], b[sizeof(player_t)];
		memcpy(a, gp, sizeof(a));
		memcpy(b, cp, sizeof(b));
		for (auto* bytes : { a, b })
		{
			memset(bytes + ((const uint8_t*)&gp->mo - (const uint8_t*)gp), 0, sizeof(gp->mo));
			memset(bytes + ((const uint8_t*)&gp->camera - (const uint8_t*)gp), 0, sizeof(gp->camera));
			memset(bytes + ((const uint8_t*)&gp->psprites - (const uint8_t*)gp), 0, sizeof(gp->psprites));
		}
		bool same = memcmp(a, b, sizeof(a)) == 0 && memcmp(&got.PlayerBob, &check.PlayerBob, sizeof(FPlayerBob)) == 0;
		auto raw = [](const void* field) { return *reinterpret_cast<DPSprite* const*>(field); };
		const DPSprite* x = raw(&gp->psprites);
		const DPSprite* y = raw(&cp->psprites);
		for (; same && x != nullptr && y != nullptr; x = raw(&x->Next), y = raw(&y->Next))
		{
			const size_t size = x->GetClass()->Size;
			if (size != y->GetClass()->Size) { same = false; break; }
			std::vector<uint8_t> u((const uint8_t*)x, (const uint8_t*)x + size), v((const uint8_t*)y, (const uint8_t*)y + size);
			for (auto* bytes : { &u, &v })
			{
				memset(bytes->data() + ((const uint8_t*)&x->Owner - (const uint8_t*)x), 0, sizeof(x->Owner));
				memset(bytes->data() + ((const uint8_t*)&x->Next - (const uint8_t*)x), 0, sizeof(x->Next));
				memset(bytes->data() + ((const uint8_t*)&x->Caller - (const uint8_t*)x), 0, sizeof(x->Caller));
				// [Karto] Prev and Vert are the drawn corners: frames write Vert, and the swap carries them into the
				// new copy's Prev and the live Vert (WorldModeCarryWeaponCorners), so a rebuild differs there.
				memset(bytes->data() + ((const uint8_t*)&x->Prev - (const uint8_t*)x), 0, sizeof(x->Prev));
				memset(bytes->data() + ((const uint8_t*)&x->Vert - (const uint8_t*)x), 0, sizeof(x->Vert));
			}
			same = u == v;
		}
		if (!same || x != nullptr || y != nullptr) bad(6);
	}
	// 7: clocks and identity
	if (got.Level != check.Level || got.MapTime != check.MapTime || got.StartTime != check.StartTime || got.LocalWorldTimer != check.LocalWorldTimer) bad(7);
}

// [Karto] r_worldmode_publishpredict: P_PredictClient moves only the console player, so those parts are re-copied into
// the worker's publish instead of rebuilding it on the main thread.
CVAR(Bool, r_worldmode_publishpredict, true, 0)
static uint64_t PredictPatches, PredictPatchFallbacks;

static bool WorldModePatchPredictedPlayer(FLevelLocals* level, FActorSnapshot& snap)
{
	player_t* player = &players[consoleplayer];
	AActor* mo = player->mo;
	if (!snap.PlayerValid || mo == nullptr || mo->Level != level || mo->touching_sectorportallist != nullptr) return false;
	auto copied = [&](AActor* live) { return live != nullptr && live->PublishStamp == snap.Stamp && live->PublishIndex >= 0; };
	if (!copied(mo)) return false;
	for (DPSprite* psp = player->psprites; psp != nullptr; psp = psp->GetNext())
		if (psp->Caller != nullptr && !copied(psp->Caller)) return false;
	std::vector<int> sectors = snap.PlayerSectors;
	for (auto node = mo->touching_rendersectors; node; node = node->m_tnext)
	{
		const int s = node->m_sector->Index();
		if (std::find(sectors.begin(), sectors.end(), s) == sectors.end()) sectors.push_back(s);
	}
	for (int s : sectors)
	{
		if (size_t(s) >= snap.Included.size() || !snap.Included[s]) continue;
		uint32_t count = 0;
		for (auto node = level->sectors[s].touching_renderthings; node; node = node->m_snext)
		{
			if (node->m_thing->PublishStamp != snap.Stamp) return false;
			++count;
		}
		if (count != snap.ListedStart[s + 1] - snap.ListedStart[s]) return false;
	}
	auto recopy = [&](AActor* live)
	{
		auto* copy = reinterpret_cast<AActor*>(&snap.Arena[size_t(live->PublishIndex) + 1]);
		memcpy((void*)copy, (const void*)live, live->GetClass()->Size);
		WorldModeDropDestroyedRefs(copy);
		sector_t* sector = live->Sector;
		if (sector && sector >= level->sectors.Data() && sector < level->sectors.Data() + level->sectors.Size() &&
			size_t(sector->Index()) < snap.SectorCopy.size() && snap.SectorCopy[sector->Index()] >= 0)
			copy->Sector = reinterpret_cast<sector_t*>(&snap.SectorArena[snap.SectorCopy[sector->Index()]]);
	};
	recopy(mo);
	if (player->camera != nullptr && player->camera.Get() != mo && copied(player->camera.Get())) recopy(player->camera.Get());
	for (DPSprite* psp = player->psprites; psp != nullptr; psp = psp->GetNext())
		if (psp->Caller != nullptr && psp->Caller != mo) recopy(psp->Caller);
	for (int s : sectors)
	{
		if (size_t(s) >= snap.Included.size() || !snap.Included[s]) continue;
		uint32_t at = snap.ListedStart[s];
		for (auto node = level->sectors[s].touching_renderthings; node; node = node->m_snext)
		{
			const int32_t index = node->m_thing->PublishIndex;
			if (index < 0) snap.Listed[at++] = { nullptr, &snap.SpriteRecords[size_t(-1 - index)] };
			else snap.Listed[at++] = { reinterpret_cast<AActor*>(&snap.Arena[size_t(index) + 1]), nullptr };
		}
	}
	WorldModeCopyPlayer(snap, player);
	return true;
}

void WorldModePublishActorsAfterWorker(FLevelLocals* level, bool predicted)
{
	bool usable = WorkerPublishBuilt && WorkerPublishLevel == level && !DecouplePublishHeld &&
		r_worldmode_snapshot && r_worldmode && !WorldModeMainOnlyState(level) && LightRecreates == WorkerPublishRecreates;
	if (usable && predicted)
	{
		if (r_worldmode_publishpredict && WorldModePatchPredictedPlayer(level, BackSnapshot())) ++PredictPatches;
		else
		{
			usable = false;
			++PredictPatchFallbacks;
		}
	}
	WorkerPublishBuilt = false;
	WorkerPublishLevel = nullptr;
	if (usable)
	{
		WorldModeFinishPublish(level);
		++WorkerPublishes;
		if (r_worldmode_publishworker_verify) WorldModeVerifyWorkerPublish(level);
	}
	else
	{
		WorldModePublishActors(level);
		++WorkerPublishFallbacks;
	}
}

bool WorldModeSnapshotThings(FLevelLocals* level, const sector_t* sector, const FSnapshotThing*& listed, uint32_t& listedCount,
	AActor* const*& portal, uint32_t& portalCount)
{
	auto& snap = (*FrontSnapshot);
	if (!r_worldmode_snapshot || snap.Level != level || snap.StartTime != level->starttime || snap.MapTime != WorldModeFrameTic(level) ||
		snap.ListedStart.size() != size_t(level->sectors.Size()) + 1) return false;
	const uint32_t s = sector->Index();
	if (!snap.Included[s])
	{
		++snap.FallbackSectors;
		// [Karto] The live lists belong to the worker while it ticks, so such a sector shows no things for this frame
		// instead of racing it (counted).
		if (r_worldmode_publishworker && TicWorkerBusy)
		{
			++BusyFallbackSectors;
			listed = nullptr;
			listedCount = 0;
			portal = nullptr;
			portalCount = 0;
			return true;
		}
		return false;
	}
	++snap.SnapshotSectors;
	listed = snap.Listed.data() + snap.ListedStart[s];
	listedCount = snap.ListedStart[s + 1] - snap.ListedStart[s];
	portal = snap.Portal.data() + snap.PortalStart[s];
	portalCount = snap.PortalStart[s + 1] - snap.PortalStart[s];
	return true;
}

sector_t* WorldModePublishedSector(sector_t* sector)
{
	auto& snap = (*FrontSnapshot);
	if (!r_worldmode_snapshot || !sector || !snap.Level || snap.StartTime != snap.Level->starttime || snap.MapTime != WorldModeFrameTic(snap.Level)) return sector;
	auto* level = snap.Level;
	if (sector < level->sectors.Data() || sector >= level->sectors.Data() + level->sectors.Size()) return sector;
	const int index = sector->Index();
	if (index >= int(snap.SectorCopy.size()) || snap.SectorCopy[index] < 0) return sector;
	return reinterpret_cast<sector_t*>(&snap.SectorArena[snap.SectorCopy[index]]);
}

static bool WorldModeSnapshotCurrent(FLevelLocals* level)
{
	const auto& snap = (*FrontSnapshot);
	return r_worldmode_snapshot && level && snap.Level == level && snap.StartTime == level->starttime && snap.MapTime == WorldModeFrameTic(level);
}

int WorldModeFrameTimer(FLevelLocals* level)
{
	return WorldModeSnapshotCurrent(level) ? (*FrontSnapshot).LocalWorldTimer : level->LocalWorldTimer;
}

FLightGrid* WorldModeLightGrid(FLevelLocals* level)
{
	return WorldModeSnapshotCurrent(level) ? &(*FrontSnapshot).LightGrid : &level->lightgrid;
}

void WorldModePublishedLights(FLevelLocals* level, FDynamicLight* const*& lights, int& count)
{
	lights = nullptr;
	count = -1;
	if (!WorldModeSnapshotCurrent(level)) return;
	lights = (*FrontSnapshot).Lights.data();
	count = int((*FrontSnapshot).Lights.size());
}

FDynamicLight* WorldModePublishedLight(FDynamicLight* light)
{
	if (!light || !WorldModeSnapshotCurrent(light->Level)) return light;
	auto found = (*FrontSnapshot).LightCopy.find(light);
	return found != (*FrontSnapshot).LightCopy.end() ? found->second : light;
}

bool WorldModeSubsectorParticles(FLevelLocals* level, const subsector_t* sub, particle_t*& particles, int& first,
	DVisualThinker* const*& visuals, unsigned& visualCount)
{
	const uint32_t ss = sub->Index();
	auto& snap = (*FrontSnapshot);
	if (WorldModeSnapshotCurrent(level) && ss < snap.ParticleHeads.size())
	{
		particles = snap.Particles.data();
		first = snap.ParticleHeads[ss];
		visuals = nullptr;
		visualCount = 0;
		if (!snap.VisualSubsectors.empty())
		{
			auto from = std::lower_bound(snap.VisualSubsectors.begin(), snap.VisualSubsectors.end(), ss);
			auto to = std::upper_bound(from, snap.VisualSubsectors.end(), ss);
			visuals = snap.VisualCopies.data() + (from - snap.VisualSubsectors.begin());
			visualCount = unsigned(to - from);
		}
	}
	else
	{
		particles = level->Particles.Data();
		first = ss < level->ParticlesInSubsec.Size() ? level->ParticlesInSubsec[ss] : NO_PARTICLE;
		visuals = sub->sprites.Data();
		visualCount = sub->sprites.Size();
	}
	return first != NO_PARTICLE || visualCount > 0;
}

// [Karto] The particle expansion shader (vk_worldmode.cpp) spells these out.
static_assert(SPF_NOTIMEFREEZE == 32 && SPF_ROLL == 64 && SPF_NO_XY_BILLBOARD == 256 && SPF_FACECAMERA == 2048 &&
	SPF_NOFACECAMERA == 4096 && SPF_ROLLCENTER == 8192, "GPU particle flags (ParticleExpandSource)");

bool WorldModeGpuParticleData(FLevelLocals* level, const float*& data, uint32_t& count, uint64_t& serial)
{
	if (!WorldModeSnapshotCurrent(level)) return false;
	static std::vector<float> words;
	static uint64_t builtSerial = 0;
	const auto& parts = FrontSnapshot->Particles;
	if (builtSerial != GpuParticleSerial)
	{
		words.resize(parts.size() * GpuParticleWords);
		float* w = words.data();
		for (const particle_t& p : parts)
		{
			w[0] = float(p.Pos.X); w[1] = float(p.Pos.Y); w[2] = float(p.Pos.Z); w[3] = p.size;
			w[4] = p.Vel.X; w[5] = p.Vel.Y; w[6] = p.Vel.Z; w[7] = p.alpha;
			const uint32_t color = (uint32_t(p.color) & 0xffffffu) | (p.texture.isValid() ? 0x1000000u : 0u);
			memcpy(&w[8], &color, 4);
			memcpy(&w[9], &p.flags, 4);
			w[10] = p.Roll; w[11] = p.RollVel;
			w += GpuParticleWords;
		}
		builtSerial = GpuParticleSerial;
	}
	data = words.data();
	count = uint32_t(parts.size());
	serial = builtSerial;
	return true;
}

void WorldModePublishedQuakes(FLevelLocals* level, DEarthquake* const*& quakes, int& count)
{
	const auto& snap = (*FrontSnapshot);
	quakes = nullptr;
	count = -1;
	if (!r_worldmode_snapshot || snap.Level != level || snap.StartTime != level->starttime || snap.MapTime != WorldModeFrameTic(level)) return;
	quakes = snap.Quakes.data();
	count = int(snap.Quakes.size());
}

int WorldModeInterpolationSkip(FLevelLocals* level)
{
	auto& wm = WorldMode;
	const auto& snap = (*FrontSnapshot);
	if (!r_worldmode_interpskip || !WorldModePublishing() || !wm.Ready || !WorldModeActive(level) || wm.PublishedLevel != level ||
		wm.PublishedStartTime != level->starttime || wm.PublishedMapTime != WorldModeFrameTic(level) || snap.Level != level ||
		snap.StartTime != level->starttime || snap.MapTime != WorldModeFrameTic(level)) return 0;
	return ISKIP_All;
}

static void BlendWorldModePublishedSectors(FLevelLocals* level)
{
	auto& snap = (*FrontSnapshot);
	if (snap.Blends.empty() || snap.Level != level || snap.MapTime != WorldModeFrameTic(level)) return;
	const double frac = level->interpolator.frac;
	for (const auto& b : snap.Blends)
	{
		if (b.Sector >= int(snap.SectorCopy.size()) || snap.SectorCopy[b.Sector] < 0) continue;
		auto* copy = reinterpret_cast<sector_t*>(&snap.SectorArena[snap.SectorCopy[b.Sector]]);
		secplane_t& p = b.Plane == sector_t::ceiling ? copy->ceilingplane : copy->floorplane;
		p.setD(b.PrevD + (b.CurD - b.PrevD) * frac);
		copy->planes[b.Plane].TexZ = b.PrevTexZ + (b.CurTexZ - b.PrevTexZ) * frac;
	}
}

player_t* WorldModeRenderPlayer(int pnum)
{
	auto& snap = (*FrontSnapshot);
	player_t* live = &players[pnum];
	if (!r_worldmode_snapshot || pnum != consoleplayer || !snap.PlayerValid || live->mo == nullptr) return live;
	FLevelLocals* level = live->mo->Level;
	if (snap.Level != level || snap.StartTime != level->starttime || snap.MapTime != WorldModeFrameTic(level)) return live;
	return reinterpret_cast<player_t*>(snap.PlayerBytes);
}

// [Karto] The players[] slot of a player as frames see it: the published copy is not inside players[], so code indexing
// per-player tables (PlayerBob) or comparing with consoleplayer must not subtract pointers.
int WorldModePlayerIndex(const player_t* player)
{
	if (player == reinterpret_cast<const player_t*>((*FrontSnapshot).PlayerBytes)) return consoleplayer;
	return int(player - players);
}

CVAR(Bool, r_worldmode_publishbob, true, 0)

const FPlayerBob& WorldModePlayerBob(const player_t* player)
{
	const auto& snap = (*FrontSnapshot);
	if (snap.PlayerValid && player == reinterpret_cast<const player_t*>(snap.PlayerBytes))
		return r_worldmode_publishbob ? snap.PlayerBob : PlayerBob[consoleplayer];
	return PlayerBob[int(player - players)];
}

AActor* WorldModeLiveActor(AActor* actor)
{
	auto& snap = (*FrontSnapshot);
	if (snap.Arena.empty()) return actor;
	const auto* p = reinterpret_cast<const FActorSnapshotBlock*>(actor);
	if (p <= snap.Arena.data() || p >= snap.Arena.data() + snap.Arena.size()) return actor;
	AActor* live;
	memcpy(&live, (p - 1)->Bytes, sizeof(live));
	return live;
}

bool WorldModeGpuSprites(FLevelLocals* level, const FSpriteInstance*& instances, uint32_t& count, const FSpriteAppearanceKey*& keys,
	uint32_t& keyCount)
{
	auto& snap = *FrontSnapshot;
	if (!r_worldmode_snapshot || snap.Level != level || snap.StartTime != level->starttime || snap.MapTime != WorldModeFrameTic(level))
		return false;
	instances = snap.SpriteInstances.data();
	count = uint32_t(snap.SpriteInstances.size());
	keys = snap.SpriteKeys.data();
	keyCount = uint32_t(snap.SpriteKeys.size());
	return true;
}

EXTERN_CVAR(Bool, gl_light_shadowmap)

// [Karto] Pack layout in 32-bit words: a header, per grid cell its offset and count, the cells' light indices in grid
// order, then 16 words per light. Flags: 1 spot, 2 not its owner, 4 only its owner, 8 shadow-tested, 16 ownerless.
static bool BuildGpuLightPack(FLevelLocals* level, std::vector<uint32_t>& pack, bool& shadows)
{
	auto& snap = *FrontSnapshot;
	pack.clear();
	shadows = false;
	if (level->LightProbes.Size() > 0 || level->Displacements.size > 1) return false;
	static std::unordered_map<const AActor*, int32_t> owners;
	owners.clear();
	for (auto& [key, cell] : snap.LightGrid.cells)
		for (auto light : cell.lights)
			if (light && ((*light->pLightFlags) & (LF_DONTLIGHTSELF | LF_DONTLIGHTOTHERS))) owners.emplace(light->target.ForceGet(), -1);
	if (!owners.empty())
	{
		for (size_t i = 0; i < snap.SpriteRecords.size(); ++i)
		{
			auto found = owners.find(snap.SpriteRecords[i].Live);
			if (found != owners.end()) found->second = int32_t(i);
		}
	}
	const bool shadowTest = gl_light_shadowmap && screen->mShadowMap.Enabled();
	int minX = INT_MAX, minY = INT_MAX, maxX = INT_MIN, maxY = INT_MIN;
	for (auto& [key, cell] : snap.LightGrid.cells)
	{
		if (cell.lights.Size() == 0) continue;
		const int x = int32_t(uint32_t(key >> 32)), y = int32_t(uint32_t(key));
		minX = std::min(minX, x); maxX = std::max(maxX, x);
		minY = std::min(minY, y); maxY = std::max(maxY, y);
	}
	auto word = [](float f) { uint32_t u; memcpy(&u, &f, 4); return u; };
	const bool empty = minX > maxX;
	const int64_t width = empty ? 0 : int64_t(maxX) - minX + 1, height = empty ? 0 : int64_t(maxY) - minY + 1;
	if (width * height > (1 << 20)) return false;
	pack.assign(8 + size_t(width * height) * 2, 0u);
	pack[0] = uint32_t(minX); pack[1] = uint32_t(minY); pack[2] = uint32_t(width); pack[3] = uint32_t(height); pack[4] = 8;
	static std::vector<uint32_t> indices, lights;
	indices.clear();
	lights.clear();
	static std::vector<int32_t> slot;
	slot.assign(snap.Lights.size(), -2);
	auto lightSlot = [&](FDynamicLight* light) -> int32_t
	{
		int32_t& found = slot[PublishedLightOrdinal(snap, light)];
		if (found != -2) return found;
		int32_t index = -1;
		if (light->IsVisibleToPlayer() && light->IsActive() && !((*light->pLightFlags) & LF_DONTLIGHTACTORS))
		{
			index = int32_t(lights.size() / 16);
			float lr = light->GetRed() / 255.0f, lg = light->GetGreen() / 255.0f, lb = light->GetBlue() / 255.0f;
			if (const float alpha = light->OwnerLightAlpha(); alpha != 1.f)
			{
				lr *= alpha;
				lg *= alpha;
				lb *= alpha;
			}
			lr *= light->GetLightDefIntensity();
			lg *= light->GetLightDefIntensity();
			lb *= light->GetLightDefIntensity();
			if (light->IsSubtractive())
			{
				float bright = (float)FVector3(lr, lg, lb).Length();
				lr = (bright - lr) * -1;
				lg = (bright - lg) * -1;
				lb = (bright - lb) * -1;
			}
			float radius = light->GetRadius();
			radius *= radius;
			uint32_t flags = 0;
			float dx = 0, dy = 0, dz = 0, outer = 0, inner = 0;
			if (light->IsSpot())
			{
				flags |= 1;
				float negPitch = (float)-light->Pitch.Degrees();
				float Angle = (float)light->Yaw.Degrees();
				float xyLen = -float_fastcosdeg(negPitch);
				dx = float_fastcosdeg(Angle) * xyLen;
				dy = float_fastsindeg(Angle) * xyLen;
				dz = -float_fastsindeg(negPitch);
				outer = float_fastcosdeg((float)light->pSpotOuterAngle->Degrees());
				inner = float_fastcosdeg((float)light->pSpotInnerAngle->Degrees());
			}
			if ((*light->pLightFlags) & LF_DONTLIGHTSELF) flags |= 2;
			if ((*light->pLightFlags) & LF_DONTLIGHTOTHERS) flags |= 4;
			// [Karto] Particles light as a null actor (GetDynSpriteLight(NULL, ...)): an owner flag then asks only whether
			// the light has an owner at all.
			if ((flags & 6) && light->target.ForceGet() == nullptr) flags |= 16;
			if (shadowTest && light->IsShadowMapped())
			{
				flags |= 8;
				shadows = true;
			}
			const int32_t ownerRecord = (flags & 6) ? owners[light->target.ForceGet()] : -1;
			const uint32_t entry[16] = { word((float)light->X()), word((float)light->Y()), word((float)light->Z()), word(radius),
				word(lr), word(lg), word(lb), flags, word(dx), word(dy), word(dz), word(outer), word(inner), uint32_t(ownerRecord), 0, 0 };
			lights.insert(lights.end(), entry, entry + 16);
		}
		found = index;
		return index;
	};
	for (auto& [key, cell] : snap.LightGrid.cells)
	{
		if (cell.lights.Size() == 0) continue;
		const int64_t x = int32_t(uint32_t(key >> 32)) - int64_t(minX), y = int32_t(uint32_t(key)) - int64_t(minY);
		uint32_t* c = &pack[8 + size_t(y * width + x) * 2];
		c[0] = uint32_t(indices.size());
		for (auto light : cell.lights)
		{
			if (light == nullptr) continue;
			const int32_t index = lightSlot(light);
			if (index >= 0) indices.push_back(uint32_t(index));
		}
		c[1] = uint32_t(indices.size()) - c[0];
	}
	pack[5] = uint32_t(pack.size());
	for (size_t i = 8; i < 8 + size_t(width * height) * 2; i += 2) pack[i] += pack[5];
	pack.insert(pack.end(), indices.begin(), indices.end());
	pack[6] = uint32_t(pack.size());
	pack.insert(pack.end(), lights.begin(), lights.end());
	pack[7] = shadows ? screen->mShadowMap.NodesCount() : 0;
	return true;
}

bool WorldModeGpuLightPack(FLevelLocals* level, FGpuLightPack& out)
{
	static std::vector<uint32_t> pack;
	static uint32_t builtStamp = 0;
	static bool built = false, shadows = false;
	const uint32_t stamp = FrontSnapshot->Stamp;
	if (stamp != builtStamp)
	{
		builtStamp = stamp;
		built = BuildGpuLightPack(level, pack, shadows);
	}
	out = {};
	if (!built) return false;
	out.Words = pack.data();
	out.Count = uint32_t(pack.size());
	out.Serial = stamp;
	out.Shadows = shadows;
	return true;
}

uint32_t WorldModeGpuSpriteSerial()
{
	return FrontSnapshot->Stamp;
}

void WorldModeFillSpriteFrameTable(FRenderState& state, const uint32_t*& words, uint32_t& wordCount, uint64_t& version)
{
	auto& t = GpuSpriteFrames;
	const uint64_t epoch = state.GetGpuTextureEpoch();
	if (epoch != t.Epoch)
	{
		for (auto& s : t.State) if (s == 1) s = 0;
		t.Epoch = epoch;
	}
	for (auto& [key, index] : t.Index)
	{
		if (t.State[index] != 0) continue;
		const uint32_t frameIndex = uint32_t(key & 0xffffffffu);
		const int translation = int(int32_t(key >> 32));
		const FSpriteFrameEntry* e = WorldModeSpriteFrameEntry(frameIndex);
		uint32_t* w = &t.Words[size_t(index) * GpuSpriteFrameWords];
		bool ok = e != nullptr && e->Usable;
		for (int rot = 0; ok && rot < 16; ++rot)
		{
			FGameTexture* tex = e->Texture[rot];
			state.SetMaterial(tex, UF_Sprite, CTF_Expand, tex->isNoMipmap() ? CLAMP_XY_NOMIP : CLAMP_XY, translation, -1, nullptr);
			uint32_t* r = w + 4 + rot * 12;
			ok = state.GetCurrentMaterialGpuIDs(r);
			memcpy(r + 4, &e->UL[rot], 4);
			memcpy(r + 5, &e->UR[rot], 4);
			memcpy(r + 6, &e->VT[rot], 4);
			memcpy(r + 7, &e->VB[rot], 4);
			memcpy(r + 8, &e->Rect[rot], 16);
		}
		w[0] = (ok ? uint32_t(e->Flip) | (e->SameFirst ? 0x10000u : 0u) : 0x20000u);
		t.State[index] = ok ? 1 : 2;
		++t.Version;
	}
	words = t.Words.data();
	wordCount = uint32_t(t.Words.size());
	version = t.Version;
}

int WorldModeGpuSpriteBlend(int32_t instance)
{
	const FSpriteInstance* in = WorldModeSpriteInstance(instance);
	if (in == nullptr || GpuSpriteFrames.State[in->GpuFrame] == 2) return -1;
	return FrontSnapshot->SpriteKeys[in->Appearance].Blend;
}

namespace
{
	uint64_t OccupiedSerial = 0, SplitSerial = 0;
	std::vector<uint32_t> OccupiedSectors, OccupiedSubStart, OccupiedSubs;
	std::vector<uint32_t> SplitStart;		// per sector: start in SplitEntries + 1, 0 = not written yet
	std::vector<uint32_t> SplitGpuEnd, SplitEnd;
	std::vector<FThingSplitEntry> SplitEntries;
}

bool WorldModeOccupiedSectors(FLevelLocals* level, const uint32_t*& sectors, uint32_t& count, const uint32_t*& subStart,
	const uint32_t*& subs)
{
	auto& snap = *FrontSnapshot;
	if (!WorldModeSnapshotCurrent(level) || snap.ListedStart.size() != size_t(level->sectors.Size()) + 1 ||
		snap.ParticleHeads.size() != size_t(level->subsectors.Size())) return false;
	if (OccupiedSerial != GpuParticleSerial)
	{
		OccupiedSerial = GpuParticleSerial;
		OccupiedSectors.clear();
		OccupiedSubStart.clear();
		OccupiedSubs.clear();
		// [Karto] A sector's subsectors are in ascending index order (renderinfo.cpp groups them by render_sector), so
		// sorting the occupied ones by (render sector, index) gives the walk's order.
		static std::vector<uint32_t> found;
		found.clear();
		const uint16_t* heads = snap.ParticleHeads.data();
		const size_t subsectorCount = snap.ParticleHeads.size();
		size_t visual = 0;
		for (size_t ss = 0; ss < subsectorCount; ++ss)
		{
			bool any = heads[ss] != NO_PARTICLE;
			while (visual < snap.VisualSubsectors.size() && snap.VisualSubsectors[visual] < ss) ++visual;
			if (visual < snap.VisualSubsectors.size() && snap.VisualSubsectors[visual] == ss) any = true;
			if (any) found.push_back(uint32_t(ss));
		}
		auto sectorOf = [&](uint32_t ss) { return uint32_t(level->subsectors[ss].render_sector->Index()); };
		std::sort(found.begin(), found.end(), [&](uint32_t a, uint32_t b)
		{
			const uint32_t sa = sectorOf(a), sb = sectorOf(b);
			return sa != sb ? sa < sb : a < b;
		});
		size_t at = 0;
		for (uint32_t s = 0; s < level->sectors.Size(); ++s)
		{
			const size_t before = OccupiedSubs.size();
			while (at < found.size() && sectorOf(found[at]) == s) OccupiedSubs.push_back(found[at++]);
			// [Karto] A sector the publish left out is read from its live lists, so it stays in the walk regardless.
			const bool things = !snap.Included[s] || snap.ListedStart[s + 1] != snap.ListedStart[s] ||
				snap.PortalStart[s + 1] != snap.PortalStart[s];
			if (!things && OccupiedSubs.size() == before) continue;
			OccupiedSectors.push_back(s);
			OccupiedSubStart.push_back(uint32_t(before));
		}
		OccupiedSubStart.push_back(uint32_t(OccupiedSubs.size()));
	}
	sectors = OccupiedSectors.data();
	count = uint32_t(OccupiedSectors.size());
	subStart = OccupiedSubStart.data();
	subs = OccupiedSubs.data();
	return true;
}

bool WorldModeThingSplit(FLevelLocals* level, const sector_t* sector, const FThingSplitEntry*& entries, uint32_t& gpuCount,
	uint32_t& count)
{
	auto& snap = *FrontSnapshot;
	if (!WorldModeSnapshotCurrent(level) || snap.ListedStart.size() != size_t(level->sectors.Size()) + 1) return false;
	if (SplitSerial != GpuParticleSerial)
	{
		SplitSerial = GpuParticleSerial;
		SplitEntries.clear();
		SplitStart.assign(level->sectors.Size(), 0);
		SplitGpuEnd.assign(level->sectors.Size(), 0);
		SplitEnd.assign(level->sectors.Size(), 0);
	}
	const uint32_t s = sector->Index();
	if (!snap.Included[s]) return false;
	if (SplitStart[s] == 0)
	{
		SplitStart[s] = uint32_t(SplitEntries.size()) + 1;
		const uint32_t first = snap.ListedStart[s], last = snap.ListedStart[s + 1];
		for (uint32_t i = first; i < last; ++i)
		{
			const FSpriteRecord* r = snap.Listed[i].Record;
			if (r == nullptr || r->Instance < 0 || r->Class->ActorInfo()->distancecheck != nullptr) continue;
			const FSpriteInstance& in = snap.SpriteInstances[r->Instance];
			SplitEntries.push_back({ uint32_t(r->Instance), in.GpuFrame, uint32_t(r->MapSection), i - first, snap.SpriteKeys[in.Appearance].Blend });
		}
		SplitGpuEnd[s] = uint32_t(SplitEntries.size());
		for (uint32_t i = first; i < last; ++i)
		{
			const FSpriteRecord* r = snap.Listed[i].Record;
			if (r == nullptr || r->Instance < 0 || r->Class->ActorInfo()->distancecheck != nullptr)
				SplitEntries.push_back({ 0, 0, 0, i - first, 0xff });
		}
		SplitEnd[s] = uint32_t(SplitEntries.size());
	}
	entries = SplitEntries.data() + SplitStart[s] - 1;
	gpuCount = SplitGpuEnd[s] - (SplitStart[s] - 1);
	count = SplitEnd[s] - (SplitStart[s] - 1);
	return true;
}

bool WorldModeGpuFrameFailed(uint32_t gpuFrame)
{
	return GpuSpriteFrames.State[gpuFrame] == 2;
}

uint32_t WorldModeGpuInstanceCount()
{
	return uint32_t(FrontSnapshot->SpriteInstances.size());
}

const FSpriteInstance* WorldModeSpriteInstance(int32_t index)
{
	auto& snap = (*FrontSnapshot);
	return index >= 0 && size_t(index) < snap.SpriteInstances.size() ? &snap.SpriteInstances[index] : nullptr;
}

const FSpriteRecord* WorldModeSpriteRecord(int32_t index)
{
	auto& snap = (*FrontSnapshot);
	return index >= 0 && size_t(index) < snap.SpriteRecords.size() ? &snap.SpriteRecords[index] : nullptr;
}

CCMD(worldmode_republish)
{
	if (primaryLevel) WorldModePublishActors(primaryLevel);
}

CCMD(worldmode_snapshot_stats)
{
	const bool reset = argv.argc() == 2 && stricmp(argv[1], "reset") == 0;
	Printf("WORLDMODEPUBLISHWALK checks=%llu mismatches=%llu key_checks=%llu key_mismatches=%llu\n",
		(unsigned long long)PublishWalkChecks, (unsigned long long)PublishWalkMismatches, (unsigned long long)PublishKeyChecks,
		(unsigned long long)PublishKeyMismatches);
	FActorSnapshot snapSum;
	for (auto& one : ActorSnapshots)
	{
		snapSum.Publishes += one.Publishes;
		snapSum.IncludedSectors += one.IncludedSectors;
		snapSum.CopiedActors += one.CopiedActors;
		snapSum.CopiedBytes += one.CopiedBytes;
		snapSum.PublishNS += one.PublishNS;
		snapSum.SnapshotSectors += one.SnapshotSectors;
		snapSum.FallbackSectors += one.FallbackSectors;
		snapSum.CopiedSectors += one.CopiedSectors;
		snapSum.CopiedLights += one.CopiedLights;
		snapSum.CopiedParticles += one.CopiedParticles;
	}
	auto& snap = snapSum;
	Printf("WORLDMODEPUBLISHPREDICT patched=%llu fallbacks=%llu\n", (unsigned long long)PredictPatches, (unsigned long long)PredictPatchFallbacks);
	Printf("WORLDMODEPUBLISHWORKER enabled=%d worker_publishes=%llu main_fallbacks=%llu busy_fallback_sectors=%llu\n",
		int(r_worldmode_publishworker), (unsigned long long)WorkerPublishes, (unsigned long long)WorkerPublishFallbacks,
		(unsigned long long)BusyFallbackSectors);
	extern uint64_t WorkerLightRecreates, DeferredLightMisses;
	Printf("WORLDMODEPUBLISHDELTA particles_appended=%llu particle_rebuilds=%llu light_recreates=%llu worker_light_recreates=%llu deferred_misses=%llu\n",
		(unsigned long long)ParticleDeltaAppended, (unsigned long long)ParticleDeltaRebuilds, (unsigned long long)LightRecreates,
		(unsigned long long)WorkerLightRecreates, (unsigned long long)DeferredLightMisses);
	Printf("WORLDMODEPUBLISHVERIFY runs=%llu copies=%llu lists=%llu records=%llu particles=%llu sectors=%llu lights=%llu player=%llu clock=%llu\n",
		(unsigned long long)WorkerVerifyRuns, (unsigned long long)WorkerVerifyMismatches[0], (unsigned long long)WorkerVerifyMismatches[1],
		(unsigned long long)WorkerVerifyMismatches[2], (unsigned long long)WorkerVerifyMismatches[3], (unsigned long long)WorkerVerifyMismatches[4],
		(unsigned long long)WorkerVerifyMismatches[5], (unsigned long long)WorkerVerifyMismatches[6], (unsigned long long)WorkerVerifyMismatches[7]);
	const double n = snap.Publishes ? double(snap.Publishes) : 1.0;
	Printf("WORLDMODESNAPSHOT enabled=%d publishes=%llu sectors_per_publish=%.1f actors_per_publish=%.1f kb_per_publish=%.1f "
		"us_per_publish=%.1f snapshot_visits=%llu fallback_visits=%llu sectors_copied_per_publish=%.1f lights_per_publish=%.1f particles_per_publish=%.1f\n", *r_worldmode_snapshot ? 1 : 0,
		(unsigned long long)snap.Publishes, snap.IncludedSectors / n, snap.CopiedActors / n, snap.CopiedBytes / n / 1024.0,
		snap.PublishNS / n / 1000.0, (unsigned long long)snap.SnapshotSectors, (unsigned long long)snap.FallbackSectors, snap.CopiedSectors / n, snap.CopiedLights / n,
		snap.CopiedParticles / n);
	if (reset)
	{
		for (auto& one : ActorSnapshots)
		{
			one.Publishes = one.IncludedSectors = one.CopiedActors = one.CopiedBytes = one.PublishNS = 0;
			one.SnapshotSectors = one.FallbackSectors = one.CopiedSectors = one.CopiedLights = one.CopiedParticles = 0;
		}
		PublishWalkChecks = PublishWalkMismatches = PublishKeyChecks = PublishKeyMismatches = 0;
	}
}

bool SideRenderTracking = false;

void NoteSideRenderChange(side_t* side, unsigned change)
{
	// [Karto] Pointer range, not just the index: a reused level object may report sides of the map that replaced it.
	auto& wm = WorldMode;
	if (!wm.Ready || side < wm.SidesBase || side >= wm.SidesBase + wm.SideDirty.size()) return;
	const size_t index = size_t(side - wm.SidesBase);
	for (int k = 0; k < side_t::RenderChangeKinds; ++k) wm.SideKindCalls[k] += (change >> k) & 1;
	wm.PlaySideKinds[index] |= uint16_t(change);
	if (index * 3 + 2 < wm.PlayTierTics.size() && side->sector)
	{
		const int tic = side->sector->Level->maptime;
		for (int t = 0; t < 3; ++t)
		{
			if (!(change & (1u << t))) continue;
			auto& tt = wm.PlayTierTics[index * 3 + t];
			if (tt.Tic != tic)
			{
				tt.PrevX = tt.CurX;
				tt.PrevY = tt.CurY;
				tt.Tic = tic;
			}
			tt.CurX = side->GetTextureXOffset(t);
			tt.CurY = side->GetTextureYOffset(t);
		}
	}
	if (wm.PlaySideDirty[index]) return;
	wm.PlaySideDirty[index] = 1;
	wm.PlayDirtySides.push_back(int(index));
}

static void HandOverWorldModeSideChanges()
{
	auto& wm = WorldMode;
	for (int index : wm.PlayDirtySides)
	{
		wm.PlaySideDirty[index] = 0;
		wm.SideKinds[index] |= wm.PlaySideKinds[index];
		wm.PlaySideKinds[index] = 0;
		for (int t = 0; t < 3; ++t) wm.TierTics[size_t(index) * 3 + t] = wm.PlayTierTics[size_t(index) * 3 + t];
		if (!wm.SideDirty[index])
		{
			wm.SideDirty[index] = 1;
			wm.DirtySides.push_back(index);
		}
	}
	wm.PlayDirtySides.clear();
}

void ForgetWorldMode()
{
	WorldModeWalkerStop();
	SideRenderTracking = false;
	if (!WorldMode.Level) return;
	// [Karto] Disabling recycles bindless slots each frame, so a later enable must bake fresh texture IDs.
	screen->RenderState()->ReleaseWorldMode();
	WorldMode = {};
}

// [Karto] Survives ForgetWorldMode so a level whose textures keep being recycled cannot rebake forever.
static struct { const void* Level = nullptr; int StartTime = 0; int Rebakes = 0; uint64_t Total = 0; } WorldModeRebake;
static constexpr int WorldModeMaxRebakes = 3;

static constexpr uint32_t WorldModeSlotCapacity = 16384;

// [Karto] ZScript can write lightlevel directly, so sectors are compared rather than hooked. SECMF_DRAWN is excluded
// because the automap side effect sets it while drawing.
static void WorldModeSectorSnap(const sector_t& s, FWorldModeSectorSnap& out)
{
	memset(&out, 0, sizeof(out));
	auto& g = out.Geometry;
	g.Planes[0] = s.floorplane;
	g.Planes[1] = s.ceilingplane;
	for (int i = 0; i < 2; ++i)
	{
		auto& plane = s.planes[i];
		g.TexZ[i] = plane.TexZ;
		g.Texture[i] = plane.Texture;
		g.PlaneFlags[i] = plane.Flags;
		g.GlowColor[i] = plane.GlowColor;
		g.GlowHeight[i] = plane.GlowHeight;
		out.Flat.Xform[i] = plane.xform;
		out.Flat.Alpha[i] = plane.alpha;
		out.Flat.Reflect[i] = s.reflect[i];
		out.Flat.TextureFx[i] = plane.TextureFx;
		out.Light.PlaneLight[i] = plane.Light;
	}
	g.FadeColor = s.Colormap.FadeColor;
	g.Flags = s.Flags;
	g.MoreFlags = s.MoreFlags & ~SECMF_DRAWN;
	g.Maps[0] = s.bottommap;
	g.Maps[1] = s.midmap;
	g.Maps[2] = s.topmap;
	g.Heightsec = s.heightsec;
	g.Transdoor = s.transdoor;
	out.Light.LightLevel = s.lightlevel;
	memcpy(out.Light.Special, s.SpecialColors, sizeof(out.Light.Special));
	memcpy(out.Light.Additive, s.AdditiveColors, sizeof(out.Light.Additive));
	out.Light.Colormap = s.Colormap;
}

static int WorldModeWallRaw(const HWWall& wall)
{
	if (!wall.seg || !wall.seg->sidedef) return -1;
	switch (wall.type)
	{
	case RENDERWALL_TOP: return wall.seg->sidedef->GetTexture(side_t::top).GetIndex();
	case RENDERWALL_BOTTOM: return wall.seg->sidedef->GetTexture(side_t::bottom).GetIndex();
	case RENDERWALL_M1S: case RENDERWALL_M2S: case RENDERWALL_M2SNF: return wall.seg->sidedef->GetTexture(side_t::mid).GetIndex();
	default: return -1;
	}
}

static uint32_t WorldModeWallParam(HWDrawInfo* di, const HWWall& wall)
{
	auto& wm = WorldMode;
	side_t* sd = wall.seg ? wall.seg->sidedef : nullptr;
	if (!sd || wm.Tiers.size() != size_t(di->Level->sides.Size()) * 3) return UINT32_MAX;
	int tier;
	switch (wall.type)
	{
	case RENDERWALL_TOP: tier = side_t::top; break;
	case RENDERWALL_BOTTOM: tier = side_t::bottom; break;
	case RENDERWALL_M1S: case RENDERWALL_M2S: case RENDERWALL_M2SNF: case RENDERWALL_FFBLOCK: tier = side_t::mid; break;
	default: return UINT32_MAX;
	}
	const uint32_t slot = uint32_t(sd->Index()) * 3 + tier;	// CPU tier index; the GPU record is slot + 1
	auto& t = wm.Tiers[slot];
	const bool linear = (wall.type == RENDERWALL_TOP || wall.type == RENDERWALL_BOTTOM || wall.type == RENDERWALL_M1S) &&
		wall.texture && !wall.texture->isHardwareCanvas();
	if (!linear || t.State == FWorldModeTier::Producer)
	{
		t.State = FWorldModeTier::Producer;
		return UINT32_MAX;
	}
	FTexCoordInfo tci;
	tci.GetFromTexture(wall.texture, (float)sd->GetTextureXScale(tier), (float)sd->GetTextureYScale(tier),
		!!(di->Level->flags3 & LEVEL3_FORCEWORLDPANNING));
	t.State = FWorldModeTier::Param;
	t.ClampX |= (wall.flags & HWWall::HWF_CLAMPX) != 0;
	t.ClampY |= (wall.flags & HWWall::HWF_CLAMPY) != 0;
	t.Kx = tci.FloatToTexU(tci.TextureOffset(1.f));
	t.Ky = tci.FloatToTexV(tci.RowOffset(1.f));
	t.BaseX = sd->GetTextureXOffset(tier);
	t.BaseY = sd->GetTextureYOffset(tier);
	return slot + 1;
}

static constexpr uint32_t WorldModePlaneRef = 0x80000000u;

static uint32_t WorldModeFlatParam(HWDrawInfo* di, const HWFlat& flat)
{
	auto& wm = WorldMode;
	if (!flat.sector || wm.PlaneState.size() != size_t(di->Level->sectors.Size()) * 2) return UINT32_MAX;
	auto& sector = di->Level->sectors[flat.sector->Index()];
	const int which = flat.ceiling ? sector_t::ceiling : sector_t::floor;
	const uint32_t slot = uint32_t(sector.Index()) * 2 + which;
	HWSectorPlane own;
	own.GetFromSector(&sector, which);
	const bool own_transform = !flat.hacktype && sector.special != GLSector_Skybox && !sector.GetHeightSec() &&
		sector.e->XFloor.ffloors.Size() == 0 && own.Offs == flat.plane.Offs && own.Scale == flat.plane.Scale &&
		own.Angle == flat.plane.Angle && own.texture == flat.plane.texture;
	if (!own_transform)
	{
		const int reason = flat.hacktype ? 0 : sector.special == GLSector_Skybox ? 1 : sector.GetHeightSec() ? 2 : sector.e->XFloor.ffloors.Size() ? 3 : 4;
		++wm.PlaneReasons[reason];
	}
	if (!own_transform || wm.PlaneState[slot] == FWorldModeTier::Producer)
	{
		wm.PlaneState[slot] = FWorldModeTier::Producer;
		return UINT32_MAX;
	}
	wm.PlaneState[slot] = FWorldModeTier::Param;
	return slot | WorldModePlaneRef;
}

static constexpr size_t WorldModeHeightFloats = 12;
enum : uint8_t { WorldModeDepFrontCeiling = 1, WorldModeDepFrontFloor = 2, WorldModeDepBackCeiling = 4, WorldModeDepBackFloor = 8, WorldModeDepAll = 15 };

// [Karto] Which of the front/back floor and ceiling is above which at each line end. Tier existence and the plane
// each edge follows are decided by these comparisons (HWWall::PrepareWall), so equal bits mean the shader stays exact.
static uint32_t WorldModeLineOrder(const line_t& line)
{
	const sector_t* f = line.frontsector;
	const sector_t* b = line.backsector;
	uint32_t bits = 0;
	int bit = 0;
	for (const vertex_t* v : { line.v1, line.v2 })
	{
		const double fc = f->ceilingplane.ZatPoint(v), ff = f->floorplane.ZatPoint(v);
		if (!b)
		{
			bits |= uint32_t(fc > ff) << bit++;
			bits |= uint32_t(fc < ff) << bit++;
			continue;
		}
		const double z[4] = { fc, ff, b->ceilingplane.ZatPoint(v), b->floorplane.ZatPoint(v) };
		for (int i = 0; i < 4; ++i)
			for (int j = i + 1; j < 4; ++j)
			{
				bits |= uint32_t(z[i] > z[j]) << bit++;
				bits |= uint32_t(z[i] < z[j]) << bit++;
			}
	}
	return bits;
}
static void WorldModePlaneHeight(sector_t& sector, int which, float out[WorldModeHeightFloats])
{
	const secplane_t& plane = which == sector_t::ceiling ? sector.ceilingplane : sector.floorplane;
	out[0] = out[4] = float(plane.Normal().X);
	out[1] = out[5] = float(plane.Normal().Y);
	out[2] = out[6] = float(plane.negiC);
	out[3] = out[7] = float(plane.fD());
	out[8] = out[9] = float(sector.GetPlaneTexZ(which));
	out[10] = out[11] = 0.f;
}

static void WorldModePlaneMatrixAt(sector_t& sector, int which, double x, double y, float out[16])
{
	HWSectorPlane plane;
	plane.GetFromSector(&sector, which);
	plane.Offs.X = float(x);
	plane.Offs.Y = float(y + sector.planes[which].xform.baseyOffs);
	auto texture = TexMan.GetGameTexture(plane.texture, true);
	VSMatrix matrix(1);
	if (!texture || !hw_SetPlaneTextureRotation(&plane, texture, matrix)) matrix.loadIdentity();
	matrix.copy(out);
}

static void WorldModePlaneMatrix(sector_t& sector, int which, float out[16])
{
	WorldModePlaneMatrixAt(sector, which, sector.GetXOffset(which), sector.GetYOffset(which, false), out);
}

// [Karto] A plane slot's record is this tic's matrix then the last tic's; a still plane writes the same matrix twice so
// the shader's blend returns it exactly.
static void WorldModePlaneMatrixPair(sector_t& sector, int which, float out[32])
{
	WorldModePlaneMatrix(sector, which, out);
	memcpy(out + 16, out, 16 * sizeof(float));
}

// [Karto] Replays a surface's light the way its producer set it: walls SetFog then SetColor (RenderTexturedWall), flats
// SetColor then SetFog (DrawFlat); the order decides whether a bright-fog soft light survives.
static bool WorldModeLightPatch(HWDrawInfo* di, FRenderState& state, const FWorldModeLightRecipe& r, int extra, FWorldModeLightPatch& patch)
{
	auto& sector = di->Level->sectors[r.Sector];
	const FColormap& cm = sector.Colormap;
	if (r.Kind == FWorldModeLightRecipe::Wall)
	{
		const bool foggy = !cm.FadeColor.isBlack() || (di->Level->flags & LEVEL_HASFADETABLE);
		const int original = hw_ClampLight(sector.lightlevel);
		int rel = 0;
		const int level = RescaleLightLevel(di->Level->sides[r.Side].GetLightLevel(foggy, original, r.Tier, false, &rel));
		const int relLight = CalcRelLight(level, original, rel) + extra;
		SetFog(state, di->Level, di->lightmode, level, relLight, false, &cm, r.Additive);
		SetColor(state, di->Level, di->lightmode, level, relLight, false, cm, r.Alpha);
	}
	else
	{
		const int level = RescaleLightLevel(r.Kind == FWorldModeLightRecipe::Ceiling ? sector.GetCeilingLight() : sector.GetFloorLight());
		SetColor(state, di->Level, di->lightmode, level, extra, false, cm, r.Alpha);
		SetFog(state, di->Level, di->lightmode, level, extra, false, &cm, false);
	}
	return state.GetWorldModeLightPatch(patch);
}

static bool WorldModeFlashPatch(HWDrawInfo* di, FRenderState& state, const FWorldModeFlashRow& r, int extra, FWorldModeLightPatch& patch)
{
	if (r.Recipe.Kind != FWorldModeLightRecipe::None) return WorldModeLightPatch(di, state, r.Recipe, extra, patch);
	const auto& c = r.Trace.Color;
	const auto& f = r.Trace.Fog;
	auto color = [&] { SetColor(state, di->Level, c.Mode, c.Level, c.Rel + extra, c.Fullbright, c.Cm, c.Alpha, c.Weapon); };
	// [Karto] Fog without a colormap ignores light (M2SNF walls pass 255 and 0), so it gets no flash.
	auto fog = [&] { SetFog(state, di->Level, f.Mode, f.Level, f.Rel + (f.HasCm ? extra : 0), f.Fullbright, f.HasCm ? &f.Cm : nullptr, f.Additive); };
	if (r.Trace.FogLast) { color(); fog(); }
	else { fog(); color(); }
	return state.GetWorldModeLightPatch(patch);
}

static void VerifyWorldModeFlash(HWDrawInfo* di, FRenderState& state, FWorldModeBake& bake)
{
	FWorldModeLightPatch patch;
	size_t kept = 0;
	for (const auto& candidate : bake.FlashRows)
	{
		bool same = WorldModeFlashPatch(di, state, candidate, 0, patch) && size_t(candidate.Row + 1) * bake.RowBytes <= bake.Rows.size();
		size_t at = 0;
		for (int k = 0; same && k < patch.Count; ++k)
		{
			same = !memcmp(&bake.Rows[size_t(candidate.Row) * bake.RowBytes + patch.Range[k].Offset], patch.Data + at, patch.Range[k].Bytes);
			at += patch.Range[k].Bytes;
		}
		if (same) bake.FlashRows[kept++] = candidate;
		else ++WorldMode.FlashRejected;
	}
	bake.FlashRows.resize(kept);
}

static void VerifyWorldModeLight(HWDrawInfo* di, FRenderState& state, FWorldModeBake& bake, size_t first)
{
	if (first >= bake.LightRows.size()) return;
	FWorldModeLightPatch patch;
	const bool built = WorldModeLightPatch(di, state, bake.LightRows[first].Recipe, 0, patch);
	size_t kept = first;
	for (size_t i = first; i < bake.LightRows.size(); ++i)
	{
		const auto& candidate = bake.LightRows[i];
		bool same = built && size_t(candidate.Row + 1) * bake.RowBytes <= bake.Rows.size();
		size_t at = 0;
		for (int k = 0; same && k < patch.Count; ++k)
		{
			same = !memcmp(&bake.Rows[size_t(candidate.Row) * bake.RowBytes + patch.Range[k].Offset], patch.Data + at, patch.Range[k].Bytes);
			at += patch.Range[k].Bytes;
		}
		if (same)
		{
			bake.LightRows[kept++] = candidate;
			++WorldMode.LightVerified;
		}
		else
		{
			bake.LightFailUnits.push_back(candidate.Unit);
			++WorldMode.LightRejected;
		}
	}
	bake.LightRows.resize(kept);
}

struct FWorldModeProductionScope
{
	int Saved = r_viewpoint.extralight;
	FWorldModeProductionScope(FWorldModeBake& bake) { r_viewpoint.extralight = 0; hw_LightTrace = &bake.Trace; }
	~FWorldModeProductionScope() { r_viewpoint.extralight = Saved; hw_LightTrace = nullptr; }
};

static void BeginWorldModeProduction(FRenderState& state)
{
	state.SetRenderStyle(STYLE_Source);
	state.EnableFog(true);
	state.EnableTexture(true);
	state.EnableBrightmap(true);
}

static void ProcessWorldModeSeg(HWDrawInfo* di, HWWallDispatcher& sweep, seg_t& seg)
{
	HWWall wall;
	wall.sub = seg.Subsector;
	sector_t* front = hw_FakeFlat(seg.frontsector, di->in_area, false);
	sector_t* back = nullptr;
	if (seg.backsector)
	{
		if (front->sectornum == seg.backsector->sectornum || (seg.sidedef->Flags & WALLF_POLYOBJ)) back = front;
		else back = hw_FakeFlat(seg.backsector, di->in_area, true);
	}
	wall.Process(&sweep, &seg, front, back, false);
}

static void ProduceWorldModeWalls(HWDrawInfo* di, FWorldModeBake& bake, FRenderState& state, seg_t* const* segs, size_t count)
{
	// Walls: the native producer into a private mesh, so no portal or draw-list side effects reach this frame.
	HWMeshHelper mesh;
	HWWallDispatcher sweep(di->Level, &mesh, di->lightmode);
	const uint64_t processStarted = I_nsTime();
	for (size_t i = 0; i < count; ++i)
	{
		auto& seg = *segs[i];
		if (!seg.sidedef || !seg.frontsector) continue;
		const unsigned missing = mesh.lower.Size() + mesh.upper.Size(), translucent = mesh.translucent.Size();
		ProcessWorldModeSeg(di, sweep, seg);
		if (bake.MissingSegs && mesh.lower.Size() + mesh.upper.Size() != missing) bake.MissingSegs->push_back(&seg);
		if (bake.TranslucentSegs && mesh.translucent.Size() != translucent) bake.TranslucentSegs->push_back(&seg);
	}
	const uint64_t drawStarted = I_nsTime();
	if (bake.Pool) WorldMode.WallProcessNS += drawStarted - processStarted;
	HWWallDispatcher drawer(di);
	auto vertices = screen->mVertexData;
	const unsigned cursor = vertices->mCurIndex;
	const uint32_t sectors = di->Level->sectors.Size();
	for (auto& wall : mesh.list)
	{
		const bool masked = (HWWall::passflag[wall.type] != 1 && wall.texture && wall.texture->isMasked()) ||
			((wall.flags & HWWall::HWF_SKYHACK) && wall.type == RENDERWALL_M2S);
		bake.CurrentAlpha = masked;
		const bool dynWall = wall.seg && wall.seg->sidedef && wall.texture != nullptr && !(wall.seg->sidedef->Flags & WALLF_POLYOBJ) &&
			wall.type != RENDERWALL_FOGBOUNDARY && wall.type != RENDERWALL_MIRRORSURFACE && wall.type != RENDERWALL_COLOR &&
			!(wall.RenderStyle == STYLE_Add && !di->Level->lightadditivesurfaces);
		bake.CurrentDynKind = dynWall ? WorldModeDynWall : WorldModeDynNone;
		bake.CurrentDynRef = dynWall ? wall.seg->sidedef->Index() : -1;
		bake.CurrentLine = wall.seg && wall.seg->linedef ? wall.seg->linedef->Index() : -1;
		bake.CurrentSector = wall.seg && wall.seg->frontsector ? wall.seg->frontsector->Index() : -1;
		bake.CurrentSubsector = wall.seg && wall.seg->Subsector ? wall.seg->Subsector->Index() : -1;
		bake.CurrentSection = -1;
		bake.CurrentFlip = false;
		bake.CurrentUnit = wall.seg && wall.seg->sidedef ? sectors + uint32_t(wall.seg->sidedef->Index()) : UINT32_MAX;
		bake.CurrentRaw = WorldModeWallRaw(wall);
		bake.CurrentParam = WorldModeWallParam(di, wall);
		bake.CurrentWall = true;
		bake.CurrentClampY = (wall.flags & HWWall::HWF_CLAMPY) != 0;
		bake.CurrentHeights = wall.HeightValid && wall.HeightTop >= 0 && wall.HeightBottom >= 0 && wall.HeightAnchor >= 0;
		bake.CurrentDeps = WorldModeDepAll;
		if (wall.HeightKnown && wall.seg && wall.seg->frontsector)
		{
			const int front = wall.seg->frontsector->Index();
			auto dep = [&](int slot) -> uint8_t
			{
				if (slot < 0) return WorldModeDepAll;
				const bool ceiling = (slot & 1) != 0;
				if (slot / 2 == front) return ceiling ? WorldModeDepFrontCeiling : WorldModeDepFrontFloor;
				return ceiling ? WorldModeDepBackCeiling : WorldModeDepBackFloor;
			};
			bake.CurrentDeps = uint8_t(dep(wall.HeightTop) | dep(wall.HeightBottom) | dep(wall.HeightAnchor));
		}
		bake.CurrentReason = wall.type == RENDERWALL_M2S ? 3 : (wall.flags & HWWall::HWF_CLAMPY) ? 4 : (wall.flags & HWWall::HWF_GLOW) ? 5 :
			wall.lightlist ? 6 : wall.type == RENDERWALL_FFBLOCK ? 7 : 1;
		if (bake.CurrentHeights)
		{
			bake.CurrentHeightTop = uint32_t(wall.HeightTop);
			bake.CurrentHeightBottom = uint32_t(wall.HeightBottom);
			bake.CurrentHeightAnchor = uint32_t(wall.HeightAnchor);
			bake.CurrentVScale = wall.HeightVScale;
			bake.CurrentAnchorTexZ = float(di->Level->sectors[wall.HeightAnchor / 2].GetPlaneTexZ(wall.HeightAnchor & 1));
		}
		bake.CurrentLight = {};
		if (wall.seg && wall.seg->sidedef && wall.frontsector && !wall.lightlist && !di->isFullbrightScene() &&
			(wall.type == RENDERWALL_TOP || wall.type == RENDERWALL_BOTTOM || wall.type == RENDERWALL_M1S || wall.type == RENDERWALL_M2S))
		{
			auto& sector = di->Level->sectors[wall.frontsector->Index()];
			if (WorldMode.LightSector[sector.Index()] && !sector.GetHeightSec() && sector.e->XFloor.lightlist.Size() == 0 &&
				wall.seg->sidedef->sector == &sector)
			{
				auto& light = bake.CurrentLight;
				light.Kind = FWorldModeLightRecipe::Wall;
				light.Sector = sector.Index();
				light.Side = wall.seg->sidedef->Index();
				light.Tier = wall.type == RENDERWALL_TOP ? side_t::top : wall.type == RENDERWALL_BOTTOM ? side_t::bottom : side_t::mid;
				light.Additive = wall.RenderStyle == STYLE_Add;
				light.Alpha = fabsf(wall.alpha);
				const auto& sd = *wall.seg->sidedef;
				const bool foggy = !sector.Colormap.FadeColor.isBlack() || (di->Level->flags & LEVEL_HASFADETABLE);
				int rel = 0;
				sd.GetLightLevel(foggy, hw_ClampLight(sector.lightlevel), light.Tier, false, &rel);
				light.Class[0] = ((sd.Flags & WALLF_ABSLIGHTING) ? 1 : 0) | ((sd.Flags & (WALLF_ABSLIGHTING_TIER << light.Tier)) ? 2 : 0) |
					((sd.Flags & WALLF_LIGHT_FOG) ? 4 : 0);
				light.Class[1] = sd.Light;
				light.Class[2] = sd.TierLights[light.Tier];
				light.Class[3] = rel;
			}
		}
		const size_t lightFirst = bake.LightRows.size();
		state.AlphaFunc(Alpha_GEqual, masked ? (float)gl_mask_threshold : 0.f);
		wall.DrawWall(&drawer, state, false);
		if (bake.CurrentLight.Kind != FWorldModeLightRecipe::None) VerifyWorldModeLight(di, state, bake, lightFirst);
		else if (bake.CurrentUnit != UINT32_MAX) bake.LightFailUnits.push_back(bake.CurrentUnit);
		bake.CurrentLight = {};
		vertices->mCurIndex = cursor;
	}
	const sector_t* first = di->Level->sectors.Data(), * last = first + di->Level->sectors.Size();
	for (auto& wall : mesh.portals)
	{
		if (bake.Horizons && !wall.skysector && wall.seg && wall.seg->linedef && wall.seg->linedef->special == Line_Horizon)
			bake.Horizons->push_back(wall);
		if (!wall.skysector || wall.skysector < first || wall.skysector >= last || !wall.seg || !wall.seg->sidedef) continue;
		FWorldModeSkyWall sky;
		sky.Side = wall.seg->sidedef->Index();
		sky.Wall = wall;
		const float zlo = std::min({ wall.zbottom[0], wall.zbottom[1], wall.ztop[0], wall.ztop[1] });
		const float zhi = std::max({ wall.zbottom[0], wall.zbottom[1], wall.ztop[0], wall.ztop[1] });
		sky.Bounds[0] = std::min(wall.glseg.x1, wall.glseg.x2); sky.Bounds[3] = std::max(wall.glseg.x1, wall.glseg.x2);
		sky.Bounds[1] = zlo; sky.Bounds[4] = zhi;
		sky.Bounds[2] = std::min(wall.glseg.y1, wall.glseg.y2); sky.Bounds[5] = std::max(wall.glseg.y1, wall.glseg.y2);
		bake.Sky.push_back(std::move(sky));
	}
	if (bake.Pool)
	{
		WorldMode.WallDrawNS += I_nsTime() - drawStarted;
		return;
	}
	WorldMode.Walls += mesh.list.Size();
	WorldMode.TranslucentWalls += mesh.translucent.Size();
	WorldMode.PortalWalls += mesh.portals.Size();
}

static void ProduceWorldModeFlats(HWDrawInfo* di, FWorldModeBake& bake, FRenderState& state, FSection* const* sections, size_t count)
{
	// Flats: a far-above / far-below bake viewpoint admits both planes through the native facing tests.
	const DVector3 savedPos = di->Viewpoint.Pos;
	const uint64_t processStarted = I_nsTime();
	const int savedFlats = rendered_flats;
	for (size_t i = 0; i < count; ++i)
	{
		auto& section = *sections[i];
		if (!section.sector) continue;
		HWFlat floor;
		floor.section = &section;
		di->Viewpoint.Pos.Z = 1e9;
		floor.ProcessSector(di, section.sector, SSRF_RENDERFLOOR);
		HWFlat ceiling;
		ceiling.section = &section;
		di->Viewpoint.Pos.Z = -1e9;
		ceiling.ProcessSector(di, section.sector, SSRF_RENDERCEILING);
	}
	rendered_flats = savedFlats;
	di->Viewpoint.Pos = savedPos;
	const uint64_t drawStarted = I_nsTime();
	if (bake.Pool) WorldMode.FlatProcessNS += drawStarted - processStarted;
	for (int list : { GLDL_PLAINFLATS, GLDL_MASKEDFLATS })
	{
		bake.CurrentAlpha = list == GLDL_MASKEDFLATS;
		state.AlphaFunc(Alpha_GEqual, bake.CurrentAlpha ? (float)gl_mask_threshold : 0.f);
		for (auto flat : di->drawlists[list].flats)
		{
			bake.CurrentLine = -1;
			bake.CurrentParam = WorldModeFlatParam(di, *flat);
			bake.CurrentWall = false;
			bake.CurrentHeights = false;
			bake.CurrentLight = {};
			if (flat->sector && !flat->hacktype && !di->isFullbrightScene())
			{
				auto& sector = di->Level->sectors[flat->sector->Index()];
				if (WorldMode.LightSector[sector.Index()] && !sector.GetHeightSec() && sector.e->XFloor.ffloors.Size() == 0 &&
					sector.special != GLSector_Skybox)
				{
					auto& light = bake.CurrentLight;
					light.Kind = flat->ceiling ? FWorldModeLightRecipe::Ceiling : FWorldModeLightRecipe::Floor;
					light.Sector = sector.Index();
					light.Alpha = flat->alpha;
				}
			}
			const size_t lightFirst = bake.LightRows.size();
			bake.CurrentSubsector = -1;
			bake.CurrentSector = flat->sector ? flat->sector->Index() : -1;
			bake.CurrentSection = flat->section ? int(di->Level->sections.SectionIndex(flat->section)) : -1;
			bake.CurrentFlip = flat->ceiling;
			{
				const bool dynFlat = flat->section && flat->sector && !(flat->renderstyle == STYLE_Add && !di->Level->lightadditivesurfaces);
				bake.CurrentDynKind = !dynFlat ? WorldModeDynNone : flat->ceiling ? WorldModeDynCeiling : WorldModeDynFloor;
				bake.CurrentDynRef = dynFlat ? flat->section->Index() : -1;
			}
			bake.CurrentUnit = flat->sector ? uint32_t(flat->sector->Index()) : UINT32_MAX;
			bake.CurrentRaw = flat->sector ? flat->sector->GetTexture(flat->ceiling ? sector_t::ceiling : sector_t::floor).GetIndex() : -1;
			flat->DrawFlat(di, state, false);
			if (bake.CurrentLight.Kind != FWorldModeLightRecipe::None) VerifyWorldModeLight(di, state, bake, lightFirst);
			else if (bake.CurrentUnit != UINT32_MAX) bake.LightFailUnits.push_back(bake.CurrentUnit);
			bake.CurrentLight = {};
		}
		if (!bake.Pool) (list == GLDL_PLAINFLATS ? WorldMode.Flats : WorldMode.MaskedFlats) += di->drawlists[list].flats.Size();
	}
	if (bake.Pool) WorldMode.FlatDrawNS += I_nsTime() - drawStarted;
	for (auto& list : di->drawlists) list.Reset();
}

static std::vector<uint8_t> WorldModeAnimatedPics()
{
	std::vector<uint8_t> pics(size_t(std::max(TexMan.NumTextures(), 0)), 0);
	for (auto& anim : TexAnim.GetAnimations())
	{
		const int base = anim.BasePic.GetIndex();
		const int frames = anim.bDiscrete ? 1 : anim.NumFrames;
		for (int i = 0; i < frames; ++i)
			if (base + i >= 0 && size_t(base + i) < pics.size()) pics[base + i] = 1;
	}
	return pics;
}

static const char* WorldModeUnsupportedFeature(FLevelLocals* level)
{
	if (level->linePortals.Size()) return "line portals";
	if (level->sectorPortals.Size() && level->sectorPortals[0].mSkybox != nullptr) return "a default skybox";
	for (auto& line : level->lines)
		if (line.special == Line_Mirror) return "mirror lines";
	for (auto& sec : level->sectors)
		for (int plane = 0; plane < 2; ++plane)
		{
			if (sec.Portals[plane] != 0) return "sector portals or skyboxes";
			if (sec.GetReflect(plane) != 0) return "reflective planes";
			auto tex = TexMan.GetGameTexture(sec.GetTexture(plane));
			if (tex && tex->isHardwareCanvas()) return "camera textures";
		}
	for (auto& sd : level->sides)
		for (int t = 0; t < 3; ++t)
		{
			auto tex = TexMan.GetGameTexture(sd.GetTexture(t));
			if (tex && tex->isHardwareCanvas()) return "camera textures";
		}
	return nullptr;
}

void HWDrawInfo::BakeWorldMode()
{
	auto& renderState = *screen->RenderState();
	if (SameLevel(Level))
	{
		if (!WorldMode.Ready || !renderState.IsWorldModeStale()) return;
		// [Karto] A wipe frame before precaching can bake bindless IDs that precaching then recycles; rebake instead of
		// drawing nothing. After a few attempts WorldModeActive falls back to native.
		if (WorldModeRebake.Level != Level || WorldModeRebake.StartTime != Level->starttime)
			WorldModeRebake = { Level, Level->starttime, 0, WorldModeRebake.Total };
		if (WorldModeRebake.Rebakes >= WorldModeMaxRebakes) return;
		++WorldModeRebake.Rebakes;
		++WorldModeRebake.Total;
		DPrintf(DMSG_NOTIFY, "World mode: baked texture IDs were invalidated; rebaking (%d of %d for this level)\n", WorldModeRebake.Rebakes, WorldModeMaxRebakes);
	}
	ForgetWorldMode();
	WorldMode.Level = Level;
	WorldMode.Sectors = Level->sectors.Data();
	WorldMode.SectorCount = Level->sectors.Size();
	WorldMode.LineCount = Level->lines.Size();
	WorldMode.StartTime = Level->starttime;
	if (const char* feature = WorldModeUnsupportedFeature(Level))
	{
		Printf("World mode: this map uses %s; ordinary renderer stays active for this level.\n", feature);
		return;
	}
	const uint64_t started = I_msTime();
	auto& state = renderState;
	FWorldModeBake bake;
	WorldMode.AnimPics = WorldModeAnimatedPics();
	WorldMode.Tiers.assign(size_t(Level->sides.Size()) * 3, {});
	WorldMode.LightSector.assign(Level->sectors.Size(), 0);
	{
		auto iterator = Level->GetThinkerIterator<DLighting>(NAME_None, STAT_LIGHT);
		while (auto effect = iterator.Next())
			if (effect->GetSector()) WorldMode.LightSector[effect->GetSector()->Index()] = 1;
	}
	WorldMode.PlaneState.assign(size_t(Level->sectors.Size()) * 2, FWorldModeTier::None);
	WorldMode.PlaneShifted.assign(size_t(Level->sectors.Size()) * 2, 0);
	bake.AnimPics = &WorldMode.AnimPics;
	FWorldModeProductionScope production(bake);
	if (!state.BeginWorldCapture(&bake))
	{
		Printf("World mode: capture unavailable (bindless textures required); ordinary renderer stays active.\n");
		return;
	}
	BeginWorldModeProduction(state);
	std::vector<seg_t*> segs;
	segs.reserve(Level->segs.Size());
	for (auto& seg : Level->segs) segs.push_back(&seg);
	bake.MissingSegs = &WorldMode.MissingSegs;
	bake.Horizons = &WorldMode.Horizons;
	bake.TranslucentSegs = &WorldMode.TranslucentSegs;
	ProduceWorldModeWalls(this, bake, state, segs.data(), segs.size());
	bake.MissingSegs = nullptr;
	bake.Horizons = nullptr;
	bake.TranslucentSegs = nullptr;
	// [Karto] The classic walk draws a line's walls once per frame from whichever seg it reaches (whole-line walls), so a
	// split line keeps one seg.
	{
		std::vector<uint8_t> seen(Level->sides.Size(), 0);
		auto& t = WorldMode.TranslucentSegs;
		t.erase(std::remove_if(t.begin(), t.end(), [&](seg_t* seg) { return seen[seg->sidedef->Index()]++ != 0; }), t.end());
		std::fill(seen.begin(), seen.end(), 0);
		auto& h = WorldMode.Horizons;
		h.erase(std::remove_if(h.begin(), h.end(), [&](const HWWall& w) { return seen[w.seg->sidedef->Index()]++ != 0; }), h.end());
	}
	WorldMode.MissingListed.assign(Level->segs.Size(), 0);
	for (auto seg : WorldMode.MissingSegs) WorldMode.MissingListed[seg->Index()] = 1;
	WorldMode.MovedListed.assign(Level->sectors.Size(), 0);
	WorldMode.PlanesMoved = std::make_unique<std::atomic<uint8_t>[]>(Level->sectors.Size());
	std::vector<FSection*> sections;
	sections.reserve(Level->sections.allSections.Size());
	for (auto& section : Level->sections.allSections) sections.push_back(&section);
	ProduceWorldModeFlats(this, bake, state, sections.data(), sections.size());
	state.EndWorldCapture();
	VerifyWorldModeFlash(this, state, bake);
	state.AlphaFunc(Alpha_GEqual, 0.f);
	memcpy(WorldMode.Refusals, bake.Refusals, sizeof(bake.Refusals));
	const uint64_t refused = bake.Refusals[0] + bake.Refusals[1] + bake.Refusals[2] + bake.Refusals[3];
	if (refused)
	{
		Printf("World mode: %llu surfaces need the ordinary renderer (custom shaders or unsupported materials); it stays active for this level.\n",
			(unsigned long long)refused);
		return;
	}

	// Clusters: opaque before alpha-tested, each ordered along a horizontal Morton curve for tight bounds.
	float lo[2] = { FLT_MAX, FLT_MAX }, hi[2] = { -FLT_MAX, -FLT_MAX };
	for (auto& s : bake.Surfaces)
	{
		lo[0] = std::min(lo[0], s.Min[0]); hi[0] = std::max(hi[0], s.Max[0]);
		lo[1] = std::min(lo[1], s.Min[2]); hi[1] = std::max(hi[1], s.Max[2]);
	}
	std::vector<std::pair<uint64_t, uint32_t>> order;
	order.reserve(bake.Surfaces.size());
	for (uint32_t i = 0; i < bake.Surfaces.size(); ++i)
	{
		auto& s = bake.Surfaces[i];
		auto quantize = [&](int axis, int k) { return uint32_t(65535.f * ((s.Min[k] + s.Max[k]) * 0.5f - lo[axis]) / std::max(1.f, hi[axis] - lo[axis])); };
		// [Karto] Memory axes 0 and 2 are map x/y; axis 1 is height and would split stacked surfaces apart.
		order.push_back({ (uint64_t(s.Alpha) << 32) | (Spread(quantize(0, 0)) | (Spread(quantize(1, 2)) << 1)), i });
	}
	std::sort(order.begin(), order.end());
	std::vector<uint32_t> indices, firstIndex, indexCount;
	indices.reserve(bake.Indices.size());
	WorldMode.ClusterSectorStart.assign(1, 0);
	std::vector<int> clusterSectors;
	WorldMode.SurfaceFirst.assign(bake.Surfaces.size(), 0);
	WorldMode.SurfaceCount.assign(bake.Surfaces.size(), 0);
	WorldMode.SurfaceCluster.assign(bake.Surfaces.size(), 0);
	for (size_t start = 0; start < order.size();)
	{
		const bool alpha = bake.Surfaces[order[start].second].Alpha;
		size_t end = start;
		float bounds[6] = { FLT_MAX, FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX };
		firstIndex.push_back(uint32_t(indices.size()));
		while (end < order.size() && end - start < FWorldModeState::ClusterSize && bake.Surfaces[order[end].second].Alpha == alpha)
		{
			auto& s = bake.Surfaces[order[end].second];
			WorldMode.SurfaceFirst[order[end].second] = uint32_t(indices.size());
			WorldMode.SurfaceCluster[order[end].second] = uint32_t(firstIndex.size() - 1);
			WorldMode.SurfaceCount[order[end].second] = s.IndexCount;
			indices.insert(indices.end(), bake.Indices.begin() + s.FirstIndex, bake.Indices.begin() + s.FirstIndex + s.IndexCount);
			for (int k = 0; k < 3; ++k) { bounds[k] = std::min(bounds[k], s.Min[k]); bounds[3 + k] = std::max(bounds[3 + k], s.Max[k]); }
			if (s.Sector >= 0) clusterSectors.push_back(s.Sector);
			++end;
		}
		std::sort(clusterSectors.begin(), clusterSectors.end());
		clusterSectors.erase(std::unique(clusterSectors.begin(), clusterSectors.end()), clusterSectors.end());
		WorldMode.ClusterSectors.insert(WorldMode.ClusterSectors.end(), clusterSectors.begin(), clusterSectors.end());
		WorldMode.ClusterSectorStart.push_back(uint32_t(WorldMode.ClusterSectors.size()));
		clusterSectors.clear();
		indexCount.push_back(uint32_t(indices.size()) - firstIndex.back());
		WorldMode.ClusterBounds.insert(WorldMode.ClusterBounds.end(), bounds, bounds + 6);
		if (!alpha) ++WorldMode.OpaqueClusters;
		start = end;
	}
	WorldMode.Clusters = uint32_t(firstIndex.size());
	WorldMode.Visible.assign(WorldMode.Clusters, 1);
	WorldMode.SectorBounds.assign(size_t(Level->sectors.Size()) * 4, 0.f);
	for (auto& sector : Level->sectors)
	{
		float* b = &WorldMode.SectorBounds[size_t(sector.Index()) * 4];
		b[0] = b[1] = FLT_MAX;
		b[2] = b[3] = -FLT_MAX;
		for (auto line : sector.Lines)
		{
			for (auto v : { line->v1, line->v2 })
			{
				b[0] = std::min(b[0], float(v->fX())); b[1] = std::min(b[1], float(v->fY()));
				b[2] = std::max(b[2], float(v->fX())); b[3] = std::max(b[3], float(v->fY()));
			}
		}
	}
	auto& wm = WorldMode;
	wm.StaticVertices = uint32_t(bake.Vertices.size());
	wm.StaticIndices = uint32_t(indices.size());
	wm.StaticSurfaces = uint32_t(bake.Surfaces.size());
	wm.StaticRows = bake.RowCount;
	wm.RowBytes = uint32_t(bake.RowBytes);
	wm.PoolVertexCap = std::max(262144u, wm.StaticVertices / 2);
	wm.PoolIndexCap = std::max(786432u, wm.StaticIndices / 2);
	wm.PoolSurfaceCap = std::max(16384u, wm.StaticSurfaces / 8);
	const uint32_t surfaceCapacity = wm.StaticSurfaces + wm.PoolSurfaceCap;
	std::vector<uint32_t> surfaceRows(surfaceCapacity), surfaceParams(surfaceCapacity, UINT32_MAX);
	wm.SurfaceInfo.assign(surfaceCapacity, {});
	const uint32_t units = Level->sectors.Size() + Level->sides.Size();
	wm.UnitStart.assign(units + 1, 0);
	for (size_t i = 0; i < bake.Surfaces.size(); ++i)
	{
		auto& s = bake.Surfaces[i];
		surfaceRows[i] = s.Row;
		surfaceParams[i] = s.Param;
		wm.SurfaceInfo[i] = { s.Line, s.Sector, s.Subsector, s.Section, s.DynKind, s.DynRef };
		if (s.Unit < units) ++wm.UnitStart[s.Unit + 1];
	}
	for (uint32_t k = 0; k < wm.PoolSurfaceCap; ++k) surfaceRows[wm.StaticSurfaces + k] = wm.StaticRows + k;
	for (uint32_t u = 0; u < units; ++u) wm.UnitStart[u + 1] += wm.UnitStart[u];
	wm.UnitSurfaces.assign(wm.UnitStart[units], 0);
	{
		std::vector<uint32_t> fill(wm.UnitStart.begin(), wm.UnitStart.end() - 1);
		for (uint32_t i = 0; i < wm.StaticSurfaces; ++i)
			if (bake.Surfaces[i].Unit < units) wm.UnitSurfaces[fill[bake.Surfaces[i].Unit]++] = i;
	}
	wm.SectorStamp.assign(Level->sectors.Size(), 0);
	wm.SpriteStamp.assign(Level->sectors.Size(), 0);
	{
		const unsigned sectorCount = Level->sectors.Size();
		wm.NeighbourStart.assign(size_t(sectorCount) + 1, 0);
		wm.Neighbours.clear();
		std::vector<uint32_t> listed(sectorCount, ~0u);
		for (unsigned s = 0; s < sectorCount; ++s)
		{
			auto& sector = Level->sectors[s];
			listed[s] = s;
			for (auto line : sector.Lines)
			{
				const sector_t* other = line->frontsector == &sector ? line->backsector : line->frontsector;
				if (other == nullptr || listed[other->Index()] == s) continue;
				listed[other->Index()] = s;
				wm.Neighbours.push_back(other->Index());
			}
			wm.NeighbourStart[s + 1] = uint32_t(wm.Neighbours.size());
		}
	}
	wm.Surfaces = bake.Surfaces.size();
	wm.Rows = bake.RowCount;
	wm.Vertices = bake.Vertices.size();
	wm.IndexCount = indices.size();
	wm.StaticIndexData = indices;
	wm.StaticAnim = std::move(bake.Anim);

	bake.Vertices.resize(size_t(wm.StaticVertices) + wm.PoolVertexCap);
	bake.SurfaceIDs.resize(bake.Vertices.size(), 0.f);
	indices.resize(size_t(wm.StaticIndices) + wm.PoolIndexCap, 0);
	bake.Rows.resize(size_t(wm.StaticRows + wm.PoolSurfaceCap) * wm.RowBytes, 0);
	FWorldModeUpload upload;
	upload.Vertices = bake.Vertices.data();
	upload.VertexCount = bake.Vertices.size();
	upload.SurfaceIDs = bake.SurfaceIDs.data();
	upload.Indices = indices.data();
	upload.IndexCount = indices.size();
	upload.Rows = bake.Rows.data();
	upload.RowCount = wm.StaticRows + wm.PoolSurfaceCap;
	upload.RowBytes = bake.RowBytes;
	upload.ClusterFirstIndex = firstIndex.data();
	upload.ClusterIndexCount = indexCount.data();
	upload.ClusterCount = wm.Clusters;
	upload.OpaqueClusters = wm.OpaqueClusters;
	upload.SurfaceRows = surfaceRows.data();
	upload.SurfaceCount = surfaceRows.size();
	upload.PoolClusterCapacity = 2 * WorldModeSlotCapacity;
	std::vector<float> params((wm.Tiers.size() + 1) * 4, 0.f);
	params[0] = 1.f;
	upload.SurfaceParams = surfaceParams.data();
	upload.Params = params.data();
	upload.ParamCount = wm.Tiers.size() + 1;
	std::vector<float> planeMatrices(wm.PlaneState.size() * 32, 0.f);
	for (auto& sector : Level->sectors)
		for (int which = 0; which < 2; ++which)
			WorldModePlaneMatrixPair(sector, which, &planeMatrices[(size_t(sector.Index()) * 2 + which) * 32]);
	upload.PlaneMatrices = planeMatrices.data();
	upload.PlaneCount = wm.PlaneState.size();
	std::vector<float> planeHeights(wm.PlaneState.size() * WorldModeHeightFloats, 0.f);
	for (auto& sector : Level->sectors)
		for (int which = 0; which < 2; ++which)
			WorldModePlaneHeight(sector, which, &planeHeights[(size_t(sector.Index()) * 2 + which) * WorldModeHeightFloats]);
	upload.PlaneHeights = planeHeights.data();
	bake.VertexRefs.resize(bake.Vertices.size() * 4, UINT32_MAX);
	upload.VertexRefs = bake.VertexRefs.data();
	wm.SideDeps.assign(Level->sides.Size(), 0);
	wm.SideGuards.assign(Level->sides.Size(), {});
	for (auto& guard : bake.ClampGuards)
	{
		if (guard.Side < Level->sectors.Size() || guard.Side - Level->sectors.Size() >= wm.SideGuards.size()) continue;
		guard.Side -= Level->sectors.Size();
		wm.SideGuards[guard.Side].push_back(guard);
	}
	wm.SideUncovered.assign(Level->sides.Size(), 0);
	for (size_t i = 0; i < bake.UncoveredUnits.size(); ++i)
	{
		const uint32_t unit = bake.UncoveredUnits[i];
		if (unit >= Level->sectors.Size() && unit - Level->sectors.Size() < wm.SideUncovered.size() && !wm.SideUncovered[unit - Level->sectors.Size()])
			wm.SideUncovered[unit - Level->sectors.Size()] = bake.UncoveredReasons[i];
	}
	for (auto& sky : bake.Sky)
		if (size_t(sky.Side) < wm.SideUncovered.size()) wm.SideUncovered[sky.Side] = 8;
	for (auto& sky : bake.Sky)
		if (size_t(sky.Side) < wm.SideDeps.size()) wm.SideDeps[sky.Side] = WorldModeDepAll;
	for (size_t i = 0; i < bake.UncoveredUnits.size(); ++i)
	{
		const uint32_t unit = bake.UncoveredUnits[i];
		if (unit >= Level->sectors.Size() && unit - Level->sectors.Size() < wm.SideDeps.size()) wm.SideDeps[unit - Level->sectors.Size()] |= bake.UncoveredDeps[i];
	}
	wm.LineOrder.resize(Level->lines.Size());
	for (uint32_t l = 0; l < Level->lines.Size(); ++l) wm.LineOrder[l] = WorldModeLineOrder(Level->lines[l]);
	{
		std::vector<std::vector<uint32_t>> lists(Level->sectors.Size());
		for (size_t i = 0; i < bake.Surfaces.size(); ++i)
		{
			const auto& s = bake.Surfaces[i];
			if (s.Line >= 0 && s.Unit >= Level->sectors.Size())
			{
				const line_t& line = Level->lines[s.Line];
				for (const sector_t* side : { line.frontsector, line.backsector })
					if (side) lists[side->Index()].push_back(wm.SurfaceCluster[i]);
				continue;
			}
			if (s.Sector < 0 || s.Param == UINT32_MAX || !(s.Param & WorldModePlaneRef)) continue;
			auto& list = lists[s.Sector];
			if (list.empty() || list.back() != wm.SurfaceCluster[i]) list.push_back(wm.SurfaceCluster[i]);
		}
		wm.SectorClusterStart.assign(1, 0);
		wm.SectorClusters.clear();
		for (auto& list : lists)
		{
			std::sort(list.begin(), list.end());
			list.erase(std::unique(list.begin(), list.end()), list.end());
			wm.SectorClusters.insert(wm.SectorClusters.end(), list.begin(), list.end());
			wm.SectorClusterStart.push_back(uint32_t(wm.SectorClusters.size()));
		}
	}
	wm.Ready = wm.Clusters && state.UploadWorldMode(upload);
	if (wm.Ready)
	{
		wm.SidesBase = Level->sides.Data();
		wm.UnitLight.assign(units, {});
		wm.SkyWalls.assign(Level->sides.Size(), {});
		wm.SkyQuads.assign(Level->sides.Size(), {});
		wm.SkySideFrame.assign(Level->sides.Size(), UINT64_MAX);
		wm.SkySideBounds.assign(size_t(Level->sides.Size()) * 6, 0.f);
		wm.SkySides.clear();
		for (auto& sky : bake.Sky) AddWorldModeSkyWall(std::move(sky));
		wm.UnitLightOK.assign(units, 1);
		wm.FlashRows = std::move(bake.FlashRows);
		wm.UnitFlash.assign(units, {});
		wm.FlashApplied = 0;
		for (auto& row : bake.LightRows)
			if (row.Unit < units) wm.UnitLight[row.Unit].push_back(row);
		for (uint32_t unit : bake.LightFailUnits)
			if (unit < units) wm.UnitLightOK[unit] = 0;
		for (auto& list : wm.UnitLight)
		{
			std::sort(list.begin(), list.end(), [](const FWorldModeLightRow& a, const FWorldModeLightRow& b) { return a.Row < b.Row; });
			list.erase(std::unique(list.begin(), list.end(), [](const FWorldModeLightRow& a, const FWorldModeLightRow& b) { return a.Row == b.Row; }), list.end());
		}
		wm.SideDirty.assign(Level->sides.Size(), 0);
		wm.SideKinds.assign(Level->sides.Size(), 0);
		wm.TierTics.assign(size_t(Level->sides.Size()) * 3, {});
		for (auto& side : Level->sides)
		{
			for (int t = 0; t < 3; ++t)
			{
				auto& tt = wm.TierTics[size_t(side.Index()) * 3 + t];
				tt.PrevX = tt.CurX = side.GetTextureXOffset(t);
				tt.PrevY = tt.CurY = side.GetTextureYOffset(t);
			}
		}
		wm.PlaySideKinds.assign(Level->sides.Size(), 0);
		wm.PlaySideDirty.assign(Level->sides.Size(), 0);
		wm.PlayDirtySides.clear();
		wm.PlayTierTics = wm.TierTics;
		wm.SideBlendTic.assign(Level->sides.Size(), -1);
		wm.BlendSides.clear();
		wm.BlendTic = -1;
		wm.SideRecordKinds.assign(Level->sides.Size(), 0);
		wm.SidePending.assign(Level->sides.Size(), 0);
		wm.SideTestedFrame.assign(Level->sides.Size(), 0);
		wm.SideSectors.assign(size_t(Level->sides.Size()) * 2, -1);
		for (auto& side : Level->sides)
		{
			const size_t i = size_t(side.Index()) * 2;
			wm.SideSectors[i] = side.sector ? side.sector->Index() : -1;
			const line_t* line = side.linedef;
			const sector_t* other = line ? (line->frontsector == side.sector ? line->backsector : line->frontsector) : nullptr;
			wm.SideSectors[i + 1] = other ? other->Index() : -1;
		}
		wm.SeenFrame.assign(Level->sectors.Size(), UINT64_MAX);
		wm.SeenValue.assign(Level->sectors.Size(), 0);
		wm.UnitDirty.assign(units, 0);
		wm.UnitGeometry.assign(units, 0);
		wm.UnitSlot.assign(units, -1);
		wm.SectorSnap.resize(Level->sectors.Size());
		for (auto& sector : Level->sectors) WorldModeSectorSnap(sector, wm.SectorSnap[sector.Index()]);
		wm.ScanCursor = 0;
		wm.HotFlag.assign(Level->sectors.Size(), 0);
		SideRenderTracking = true;
	}
	WorldMode.BakeMS = I_msTime() - started;
	DPrintf(DMSG_NOTIFY, "WORLDMODEBAKE ready=%d ms=%llu walls=%llu translucent_walls=%llu portal_walls=%llu flats=%llu masked_flats=%llu "
		"surfaces=%llu rows=%llu clusters=%u opaque_clusters=%u vertices=%llu indices=%llu refused_material=%llu refused_state=%llu "
		"refused_primitive=%llu refused_source=%llu\n", int(WorldMode.Ready), (unsigned long long)WorldMode.BakeMS,
		(unsigned long long)WorldMode.Walls, (unsigned long long)WorldMode.TranslucentWalls, (unsigned long long)WorldMode.PortalWalls,
		(unsigned long long)WorldMode.Flats, (unsigned long long)WorldMode.MaskedFlats, (unsigned long long)WorldMode.Surfaces,
		(unsigned long long)WorldMode.Rows, WorldMode.Clusters, WorldMode.OpaqueClusters, (unsigned long long)WorldMode.Vertices,
		(unsigned long long)WorldMode.IndexCount, (unsigned long long)WorldMode.Refusals[0], (unsigned long long)WorldMode.Refusals[1],
		(unsigned long long)WorldMode.Refusals[2], (unsigned long long)WorldMode.Refusals[3]);
	if (!WorldMode.Ready) Printf("World mode: upload refused; ordinary renderer stays active for this level.\n");
}

static void WorldModePlanes(const HWViewpointUniforms& uniforms, float planes[24])
{
	VSMatrix vpm = uniforms.ProjectionMatrix;
	vpm.multMatrix(uniforms.ViewMatrix);
	const float* matrix = vpm.get();
	for (int p = 0; p < 6; ++p)
	{
		const float sign = (p & 1) ? -1.0f : 1.0f;
		const int row = p >> 1;
		for (int k = 0; k < 4; ++k) planes[p * 4 + k] = matrix[k * 4 + 3] + sign * matrix[k * 4 + row];
	}
}

static bool WorldModeBoxVisible(const float planes[24], const float bounds[6])
{
	for (int p = 0; p < 6; ++p)
	{
		const float* plane = planes + p * 4;
		const float d = plane[0] * (plane[0] >= 0 ? bounds[3] : bounds[0]) + plane[1] * (plane[1] >= 0 ? bounds[4] : bounds[1]) +
			plane[2] * (plane[2] >= 0 ? bounds[5] : bounds[2]) + plane[3];
		if (d < 0.f) return false;
	}
	return true;
}

bool WorldModeSideEffectsDue(FLevelLocals* level, bool mainView)
{
	const int interval = r_worldmode_sideeffects;
	if (!mainView || interval <= 0 || !WorldModeActive(level)) return false;
	const int tic = WorldModeFrameTic(level);
	if (WorldMode.LastSideEffectTic >= 0 && tic - WorldMode.LastSideEffectTic < interval && tic >= WorldMode.LastSideEffectTic) return false;
	WorldMode.LastSideEffectTic = tic;
	++WorldMode.SideEffectPasses;
	return true;
}

// [Karto] A frame drawn from the publish must not write level flags while a tic could run, so automap marks seen by
// such frames wait here and are applied right before the next tic batch (WorldModeApplyMapFlags), which sees the same
// flags it saw when frames set them directly.
static struct FPendingMapFlags
{
	FLevelLocals* Level = nullptr;
	int StartTime = 0;
	std::vector<int> Lines, Subsectors, Sectors;
} PendingMapFlags;

static bool QueueMapFlag(FLevelLocals* level, std::vector<int> FPendingMapFlags::* list, int index)
{
	if (WorldModeInterpolationSkip(level) == 0) return false;
	auto& p = PendingMapFlags;
	if (p.Level != level || p.StartTime != level->starttime)
	{
		p.Lines.clear(); p.Subsectors.clear(); p.Sectors.clear();
		p.Level = level;
		p.StartTime = level->starttime;
	}
	(p.*list).push_back(index);
	return true;
}

void WorldModeMarkLine(FLevelLocals* level, int index)
{
	if (!QueueMapFlag(level, &FPendingMapFlags::Lines, index)) level->lines[index].flags |= ML_MAPPED;
}

void WorldModeMarkSubsector(FLevelLocals* level, int index)
{
	if (!QueueMapFlag(level, &FPendingMapFlags::Subsectors, index)) level->subsectors[index].flags |= SSECMF_DRAWN;
}

void WorldModeMarkSector(FLevelLocals* level, int index)
{
	if (!QueueMapFlag(level, &FPendingMapFlags::Sectors, index)) level->sectors[index].MoreFlags |= SECMF_DRAWN;
}

void WorldModeApplyMapFlags(FLevelLocals* level)
{
	auto& p = PendingMapFlags;
	if (p.Level == level && level != nullptr && p.StartTime == level->starttime)
	{
		for (int i : p.Lines) level->lines[i].flags |= ML_MAPPED;
		for (int i : p.Subsectors) level->subsectors[i].flags |= SSECMF_DRAWN;
		for (int i : p.Sectors) level->sectors[i].MoreFlags |= SECMF_DRAWN;
	}
	p.Lines.clear(); p.Subsectors.clear(); p.Sectors.clear();
}

static bool ConsumeWorldModeVisibility(FLevelLocals* level)
{
	if (!r_worldmode_gpuvis) return false;
	const uint32_t* stamps = nullptr;
	size_t count = 0;
	uint32_t stamp = 0;
	if (!screen->RenderState()->GetWorldModeVisibility(stamps, count, stamp) || count != WorldMode.SurfaceInfo.size()) return false;
	if (stamp == WorldMode.VisibleStamp) return true;	// already applied; reuse the same sector set
	const uint64_t started = I_nsTime();
	uint32_t surfaces = 0, sectors = 0;
	const bool writeFlags = !WorldModeAutomapFromWalker();
	auto& wm = WorldMode;
	const std::vector<uint8_t>* shadow = nullptr;
	for (auto& decision : wm.Shadow)
		if (decision.Stamp == stamp && decision.Cull.size() == wm.Clusters) shadow = &decision.Cull;
	uint64_t misses = 0;
	wm.SpriteFrontier.clear();
	auto visit = [&](size_t i)
	{
		++surfaces;
		if (shadow && i < wm.SurfaceCluster.size()) misses += (*shadow)[wm.SurfaceCluster[i]];
		const auto& info = WorldMode.SurfaceInfo[i];
		if (writeFlags && info.Line >= 0)
		{
			if (!(level->lines[info.Line].flags & ML_MAPPED)) { WorldModeMarkLine(level, info.Line); ++WorldMode.FlagWrites; }
		}
		if (writeFlags && info.Subsector >= 0)
		{
			if (!(level->subsectors[info.Subsector].flags & SSECMF_DRAWN)) { WorldModeMarkSubsector(level, info.Subsector); ++WorldMode.FlagWrites; }
		}
		if (writeFlags && info.Section >= 0)
		{
			for (auto sub : level->sections.allSections[info.Section].subsectors)
				if (!(sub->flags & SSECMF_DRAWN)) { WorldModeMarkSubsector(level, sub->Index()); ++WorldMode.FlagWrites; }
		}
		if (info.Sector >= 0 && WorldMode.SectorStamp[info.Sector] != stamp)
		{
			auto& sector = level->sectors[info.Sector];
			if (writeFlags && !(sector.MoreFlags & SECMF_DRAWN)) { sector.MoreFlags |= SECMF_DRAWN; ++WorldMode.FlagWrites; }
			WorldMode.SectorStamp[info.Sector] = stamp;
			wm.SpriteFrontier.push_back(info.Sector);
			++sectors;
		}
	};
	size_t i = 0;
#if defined(__SSE2__) || defined(_M_X64)
	const __m128i want = _mm_set1_epi32(int(stamp));
	for (; i + 8 <= count; i += 8)
	{
		const __m128i a = _mm_cmpeq_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(stamps + i)), want);
		const __m128i b = _mm_cmpeq_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(stamps + i + 4)), want);
		unsigned mask = unsigned(_mm_movemask_ps(_mm_castsi128_ps(a))) | (unsigned(_mm_movemask_ps(_mm_castsi128_ps(b))) << 4);
		while (mask)
		{
			const int bit = std::countr_zero(mask);
			mask &= mask - 1;
			visit(i + bit);
		}
	}
#endif
	for (; i < count; ++i)
		if (stamps[i] == stamp) visit(i);
	if (shadow)
	{
		++wm.ShadowReports;
		wm.ShadowSeenSurfaces += surfaces;
		wm.ShadowMissSurfaces += misses;
		wm.ShadowMissReports += misses != 0;
		wm.ShadowMaxMiss = std::max(wm.ShadowMaxMiss, misses);
		if (misses) Printf("WORLDMODESHADOWMISS tic=%d stamp=%u missed=%llu seen=%u x=%.1f y=%.1f angle=%.1f\n", level->maptime, stamp,
			(unsigned long long)misses, surfaces, players[consoleplayer].camera ? players[consoleplayer].camera->X() : 0.0,
			players[consoleplayer].camera ? players[consoleplayer].camera->Y() : 0.0,
			players[consoleplayer].camera ? players[consoleplayer].camera->Angles.Yaw.Degrees() : 0.0);
	}
	wm.SpriteStamp.resize(level->sectors.Size(), 0);
	for (int s : wm.SpriteFrontier) wm.SpriteStamp[s] = stamp;
	for (int ring = 0; ring < r_worldmode_spriterings && !wm.SpriteFrontier.empty(); ++ring)
	{
		wm.SpriteNext.clear();
		for (int s : wm.SpriteFrontier)
		{
			for (uint32_t k = wm.NeighbourStart[s], end = wm.NeighbourStart[s + 1]; k < end; ++k)
			{
				const int other = wm.Neighbours[k];
				if (wm.SpriteStamp[other] == stamp) continue;
				wm.SpriteStamp[other] = stamp;
				wm.SpriteNext.push_back(other);
			}
		}
		std::swap(wm.SpriteFrontier, wm.SpriteNext);
	}
	WorldMode.VisibleStamp = stamp;
	WorldMode.LastVisibleSurfaces = surfaces;
	WorldMode.LastVisibleSectors = sectors;
	WorldMode.VisibilityNS += I_nsTime() - started;
	++WorldMode.VisibilityFrames;
	return true;
}

void HWDrawInfo::DrawWorldModeVisibilityPass(FRenderState& state)
{
	if (!r_worldmode_gpuvis) return;
	static unsigned visibilityFrame = 0;
	const unsigned interval = unsigned(std::max(1, *r_worldmode_gpuvis_interval));
	if (visibilityFrame++ % interval != 0) return;
	// [Karto] After the opaque/masked world pass the depth buffer holds each pixel's nearest surface, so LEqual with
	// writes off passes only for the surface that won the pixel; colour stays untouched.
	state.SetColorMask(false, false, false, false);
	state.SetDepthMask(false);
	state.SetDepthFunc(DF_LEqual);
	state.AlphaFunc(Alpha_GEqual, 0.f);
	state.SetCulling(ECull(WorldModeCullMode()));
	state.DrawWorldModeVisibility();
	state.SetCulling(Cull_None);
	state.SetDepthFunc(DF_Less);
	state.SetDepthMask(true);
	state.SetColorMask(true, true, true, true);
	// [Karto] Models are drawn next and inherit the masked pass's alpha test; leaving it at 0 draws their cut-out texels
	// as solid black.
	state.AlphaFunc(Alpha_GEqual, gl_mask_threshold);
}

void HWDrawInfo::CollectWorldModeSprites()
{
	if (WorldMode.SectorBounds.size() != size_t(Level->sectors.Size()) * 4) return;
	const bool gpuSet = ConsumeWorldModeVisibility(Level) && WorldMode.VisibleStamp;
	if (!gl_render_things) return;
	const uint64_t started = I_nsTime();
	float planes[24];
	WorldModePlanes(VPUniforms, planes);
	const float camX = float(Viewpoint.Pos.X), camY = float(Viewpoint.Pos.Y);
	const uint8_t* entered = nullptr;
	if (!WorldModeWalkSectors(Level, entered)) entered = nullptr;
	BlendWorldModePublishedSectors(Level);
	WorldMode.SpriteCamX = camX;
	WorldMode.SpriteCamY = camY;
	WorldMode.SpriteGpuSet = gpuSet;
	uint32_t sectors = 0;
	const uint32_t* occupied = nullptr;
	const uint32_t* subStart = nullptr;
	const uint32_t* subs = nullptr;
	uint32_t occupiedCount = 0;
	const bool useOccupied = r_worldmode_thingsplit && WorldModeOccupiedSectors(Level, occupied, occupiedCount, subStart, subs);
	const uint32_t walk = useOccupied ? occupiedCount : Level->sectors.Size();
	for (uint32_t w = 0; w < walk; ++w)
	{
		auto& sector = Level->sectors[useOccupied ? occupied[w] : w];
		if (!sector.subsectorcount) continue;
		const float* b = &WorldMode.SectorBounds[size_t(sector.Index()) * 4];
		if (gpuSet && !WorldModeSpriteSectorWanted(sector.Index(), entered, camX, camY)) continue;
		const float box[6] = { b[0], -1e9f, b[1], b[2], 1e9f, b[3] };
		if (!WorldModeBoxVisible(planes, box)) continue;
		sector_t* fake = hw_FakeFlat(&sector, in_area, false);
		if (fake == &sector) fake = WorldModePublishedSector(&sector);
		bool any = false;
		if (sector.touching_renderthings || sector.sectorportal_thinglist)
		{
			SetupSprite.Clock();
			UseActorSnapshot = true;
			RenderThings(sector.subsectors[0], fake);
			UseActorSnapshot = false;
			SetupSprite.Unclock();
			any = true;
		}
		const int subCount = useOccupied ? int(subStart[w + 1] - subStart[w]) : sector.subsectorcount;
		for (int i = 0; i < subCount; ++i)
		{
			auto sub = useOccupied ? &Level->subsectors[subs[subStart[w] + i]] : sector.subsectors[i];
			particle_t* particles;
			int first;
			DVisualThinker* const* visuals;
			unsigned visualCount;
			if (WorldModeSubsectorParticles(Level, sub, particles, first, visuals, visualCount))
			{
				RenderParticles(sub, fake);
				any = true;
			}
		}
		sectors += any;
	}
	WorldMode.LastSpriteSectors = sectors;
	WorldMode.SpriteNS += I_nsTime() - started;
	++WorldMode.SpriteFrames;
}

void HWDrawInfo::CullWorldMode(FRenderState& state)
{
	const uint64_t started = I_nsTime();
	float planes[24];
	WorldModePlanes(VPUniforms, planes);
	uint32_t visible = 0;
	auto& wm = WorldMode;
	const uint32_t total = wm.Clusters + uint32_t(wm.PoolCount.size());
	wm.Visible.resize(total);
	// [Karto] The all-around walk is up to a tic old, so geometry revealed by movement or a door can be missing for
	// about a tic.
	const uint8_t* open = nullptr;
	DVector2 from;
	bool occlude = WorldModeOpenSectors(Level, open, from) && wm.ClusterSectorStart.size() == size_t(wm.Clusters) + 1 &&
		wm.PoolSectors.size() == wm.PoolCount.size();
	if (occlude && (Viewpoint.Pos.XY() - from).LengthSquared() > double(r_worldmode_occlusion_slack) * r_worldmode_occlusion_slack)
	{
		occlude = false;
		++wm.OcclusionStale;
	}
	const float camX = float(Viewpoint.Pos.X), camY = float(Viewpoint.Pos.Y), nearRadius = r_worldmode_nearradius;
	auto sectorOpen = [&](int s) { return s < 0 || open[s] || (wm.VisibleStamp && wm.SectorStamp[s] == wm.VisibleStamp); };
	auto nearCamera = [&](const float* b)
	{
		const float dx = WorldModeGap(b[0], b[3], camX), dy = WorldModeGap(b[2], b[5], camY);
		return dx * dx + dy * dy <= nearRadius * nearRadius;
	};
	uint32_t occluded = 0;
	const bool shadow = occlude && r_worldmode_occlusion_shadow;
	std::vector<uint8_t>* shadowCull = nullptr;
	if (shadow)
	{
		auto& decision = wm.Shadow[wm.ShadowNext++ % 4];
		decision.Stamp = state.PeekWorldModeStamp();
		decision.Cull.assign(wm.Clusters, 0);
		shadowCull = &decision.Cull;
	}
	const float* bounds = wm.ClusterBounds.data();
	for (uint32_t c = 0; c < wm.Clusters; ++c, bounds += 6)
	{
		bool inside = WorldModeBoxVisible(planes, bounds);
		if (inside && occlude && !nearCamera(bounds))
		{
			bool any = false;
			for (uint32_t k = wm.ClusterSectorStart[c]; k < wm.ClusterSectorStart[c + 1] && !any; ++k) any = sectorOpen(wm.ClusterSectors[k]);
			occluded += !any;
			if (shadow) (*shadowCull)[c] = !any;
			else inside = any;
		}
		wm.Visible[c] = inside;
		visible += inside;
	}
	bounds = wm.PoolBounds.data();
	for (uint32_t c = wm.Clusters; c < total; ++c, bounds += 6)
	{
		bool inside = WorldModeBoxVisible(planes, bounds);
		if (inside && occlude && !nearCamera(bounds) && !sectorOpen(wm.PoolSectors[c - wm.Clusters]))
		{
			inside = shadow;
			++occluded;
		}
		wm.Visible[c] = inside;
		visible += inside;
	}
	wm.OccludedClusters += occluded;
	wm.OcclusionFrames += occlude;
	WorldMode.LastVisible = visible;
	state.SetWorldModeVisibility(WorldMode.Visible.data(), total);
	WorldMode.CullNS += I_nsTime() - started;
	++WorldMode.CullFrames;
}

// [Karto] geometry=false promises the unit's vertices and indices are unchanged, so only its rows need writing.
static void MarkWorldModeUnit(uint32_t unit, bool geometry)
{
	auto& wm = WorldMode;
	if (unit >= wm.UnitDirty.size()) return;
	wm.UnitGeometry[unit] |= uint8_t(geometry);
	if (wm.UnitDirty[unit]) return;
	wm.UnitDirty[unit] = 1;
	wm.DirtyUnits.push_back(unit);
}

static void MarkWorldModeSector(FLevelLocals* level, sector_t& sector, bool neighbourSides, bool flats = true)
{
	if (flats) MarkWorldModeUnit(uint32_t(sector.Index()), neighbourSides);
	const uint32_t sectors = level->sectors.Size();
	for (auto line : sector.Lines)
		for (auto side : line->sidedef)
			if (side && (neighbourSides || side->sector == &sector)) MarkWorldModeUnit(sectors + uint32_t(side->Index()), neighbourSides);
	++WorldMode.LastDirtySectors;
}

static uint64_t WorldModeTargetBytes[FWorldModeWrite::TargetCount], WorldModeSlowUpdates, WorldModeSlowUpdateNS;

struct FWorldModeWriteList
{
	std::vector<uint8_t> Arena;
	std::vector<FWorldModeWrite> Writes;

	void Add(int target, size_t offset, const void* data, size_t bytes)
	{
		if (!bytes) return;
		const size_t at = Arena.size();
		if (data) Arena.insert(Arena.end(), static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + bytes);
		else Arena.resize(at + bytes, 0);
		FWorldModeWrite write;
		write.Target = target;
		write.Offset = offset;
		write.Bytes = bytes;
		write.Data = reinterpret_cast<const void*>(at);
		Writes.push_back(write);
	}

	bool Submit(FRenderState& state)
	{
		if (Writes.empty()) return true;
		for (auto& write : Writes) write.Data = Arena.data() + reinterpret_cast<size_t>(write.Data);
		return state.WriteWorldMode(Writes.data(), Writes.size());
	}
};

static void PatchWorldModeAnimation(FRenderState& state, FWorldModeAnimRow& anim, FWorldModeWriteList& writes)
{
	auto shown = TexMan.GetGameTexture(FSetTextureID(anim.Raw), true);
	if (shown == anim.Shown) return;
	anim.Shown = shown;
	FMaterialState material;
	material.Reset();
	material.mMaterial = shown ? FMaterial::ValidateTexture(shown, anim.ScaleFlags) : nullptr;
	material.mClampMode = anim.ClampMode;
	material.mTranslation = anim.Translation;
	uint32_t ids[4];
	if (!material.mMaterial || !state.GetWorldModeTextureIDs(material, ids))
	{
		++WorldMode.AnimFailures;
		return;
	}
	writes.Add(FWorldModeWrite::Rows, size_t(anim.Row) * WorldMode.RowBytes + anim.IDOffset, ids, sizeof(ids));
	++WorldMode.AnimPatches;
}

static void RestoreWorldModeUnit(uint32_t unit, FWorldModeWriteList& writes)
{
	auto& wm = WorldMode;
	for (uint32_t k = wm.UnitStart[unit]; k < wm.UnitStart[unit + 1]; ++k)
	{
		const uint32_t s = wm.UnitSurfaces[k];
		writes.Add(FWorldModeWrite::Indices, size_t(wm.SurfaceFirst[s]) * sizeof(uint32_t), &wm.StaticIndexData[wm.SurfaceFirst[s]],
			size_t(wm.SurfaceCount[s]) * sizeof(uint32_t));
	}
}

static void RebuildWorldModePoolClusters(FRenderState& state)
{
	auto& wm = WorldMode;
	wm.PoolFirst.clear();
	wm.PoolCount.clear();
	wm.PoolBounds.clear();
	wm.PoolSectors.clear();
	for (int alpha = 0; alpha < 2; ++alpha)
	{
		for (auto& slot : wm.Slots)
		{
			const uint32_t count = alpha ? slot.AlphaCount : slot.OpaqueCount;
			if (!count) continue;
			wm.PoolFirst.push_back(alpha ? slot.AlphaFirst : slot.OpaqueFirst);
			wm.PoolCount.push_back(count);
			wm.PoolBounds.insert(wm.PoolBounds.end(), slot.Bounds, slot.Bounds + 6);
			wm.PoolSectors.push_back(slot.Sector);
		}
		if (!alpha) wm.PoolOpaque = uint32_t(wm.PoolFirst.size());
	}
	state.SetWorldModePoolClusters(wm.PoolFirst.data(), wm.PoolCount.data(), wm.PoolOpaque, uint32_t(wm.PoolFirst.size()));
}

static void ReproduceWorldModeUnits(HWDrawInfo* di, FRenderState& state, FWorldModeWriteList& writes)
{
	auto& wm = WorldMode;
	const uint64_t started = I_nsTime();
	const uint32_t sectors = di->Level->sectors.Size();
	FWorldModeBake cap;
	cap.Pool = true;
	cap.AnimPics = &wm.AnimPics;
	FWorldModeProductionScope production(cap);
	if (!state.BeginWorldCapture(&cap)) return;
	BeginWorldModeProduction(state);
	std::vector<uint32_t> units;
	units.swap(wm.DirtyUnits);
	std::vector<uint32_t> unitEnd;
	std::vector<uint8_t> unitGeometry;
	unitEnd.reserve(units.size());
	unitGeometry.reserve(units.size());
	std::vector<FSection*> sections;
	for (uint32_t unit : units)
	{
		wm.UnitDirty[unit] = 0;
		unitGeometry.push_back(wm.UnitGeometry[unit] || unit >= sectors);
		wm.UnitGeometry[unit] = 0;
		if (unit < sectors)
		{
			auto& sector = di->Level->sectors[unit];
			const uint64_t vertexStarted = I_nsTime();
			CheckUpdate(screen->mVertexData, &sector);
			wm.VertexUpdateNS += I_nsTime() - vertexStarted;
			sections.clear();
			for (auto& section : di->Level->sections.SectionsForSector(&sector)) sections.push_back(&section);
			for (int which = 0; which < 2; ++which)
			{
				wm.PlaneState[size_t(unit) * 2 + which] = FWorldModeTier::None;
				wm.PlaneShifted[size_t(unit) * 2 + which] = 0;
			}
			ProduceWorldModeFlats(di, cap, state, sections.data(), sections.size());
			float matrices[64];
			WorldModePlaneMatrixPair(sector, sector_t::floor, matrices);
			WorldModePlaneMatrixPair(sector, sector_t::ceiling, matrices + 32);
			writes.Add(FWorldModeWrite::PlaneMatrices, size_t(unit) * 2 * 32 * sizeof(float), matrices, sizeof(matrices));
			float heights[WorldModeHeightFloats * 2];
			WorldModePlaneHeight(sector, sector_t::floor, heights);
			WorldModePlaneHeight(sector, sector_t::ceiling, heights + WorldModeHeightFloats);
			writes.Add(FWorldModeWrite::PlaneHeights, size_t(unit) * 2 * WorldModeHeightFloats * sizeof(float), heights, sizeof(heights));
		}
		else
		{
			auto& side = di->Level->sides[unit - sectors];
			const size_t tier = size_t(unit - sectors) * 3;
			for (size_t t = 0; t < 3; ++t) wm.Tiers[tier + t] = {};
			if (unit - sectors < wm.SkyWalls.size())
			{
				wm.SkyWalls[unit - sectors].clear();
				wm.SkyQuads[unit - sectors].clear();
			}
			static const float zeros[12] = {};
			writes.Add(FWorldModeWrite::Params, (tier + 1) * 4 * sizeof(float), zeros, sizeof(zeros));
			if (side.segs && side.numsegs > 0) ProduceWorldModeWalls(di, cap, state, side.segs, size_t(side.numsegs));
			if (unit - sectors < wm.SideDeps.size()) wm.SideDeps[unit - sectors] = 0;
			if (unit - sectors < wm.SideGuards.size()) wm.SideGuards[unit - sectors].clear();
			if (side.linedef && uint32_t(side.linedef->Index()) < wm.LineOrder.size()) wm.LineOrder[side.linedef->Index()] = WorldModeLineOrder(*side.linedef);
		}
		unitEnd.push_back(uint32_t(cap.Surfaces.size()));
	}
	state.EndWorldCapture();
	VerifyWorldModeFlash(di, state, cap);
	state.AlphaFunc(Alpha_GEqual, 0.f);
	wm.ProduceNS += I_nsTime() - started;
	for (auto& guard : cap.ClampGuards)
	{
		if (guard.Side < sectors || guard.Side - sectors >= wm.SideGuards.size()) continue;
		guard.Side -= sectors;
		wm.SideGuards[guard.Side].push_back(guard);
	}
	for (size_t i = 0; i < cap.UncoveredUnits.size(); ++i)
	{
		const uint32_t unit = cap.UncoveredUnits[i];
		if (unit >= sectors && unit - sectors < wm.SideDeps.size())
		{
			wm.SideDeps[unit - sectors] |= cap.UncoveredDeps[i];
			if (unit - sectors < wm.SideUncovered.size()) wm.SideUncovered[unit - sectors] = cap.UncoveredReasons[i];
		}
	}
	for (auto& sky : cap.Sky)
	{
		if (size_t(sky.Side) < wm.SideDeps.size()) wm.SideDeps[sky.Side] = WorldModeDepAll;
		if (size_t(sky.Side) < wm.SideUncovered.size()) wm.SideUncovered[sky.Side] = 8;
		if (size_t(sky.Side) >= wm.SkyWalls.size()) continue;
		AddWorldModeSkyWall(std::move(sky));
	}

	const bool compacting = wm.PoolVertexNext == 0 && wm.PoolIndexNext == 0 && wm.PoolSurfaceNext == 0 && !wm.Slots.empty();
	bool layoutChanged = false, full = false;
	std::sort(cap.LightFailUnits.begin(), cap.LightFailUnits.end());
	auto placeLight = [&](uint32_t unit, uint32_t begin, uint32_t end, uint32_t rowBase)
	{
		if (unit >= wm.UnitLight.size()) return;
		auto& list = wm.UnitLight[unit];
		list.clear();
		for (const auto& row : cap.LightRows)
		{
			if (row.Row < begin || row.Row >= end) continue;
			list.push_back(row);
			list.back().Row = rowBase + (row.Row - begin);
		}
		wm.UnitLightOK[unit] = !std::binary_search(cap.LightFailUnits.begin(), cap.LightFailUnits.end(), unit);
		auto& flash = wm.UnitFlash[unit];
		flash.clear();
		for (const auto& row : cap.FlashRows)
		{
			if (row.Row < begin || row.Row >= end) continue;
			flash.push_back(row);
			flash.back().Row = rowBase + (row.Row - begin);
			wm.FlashPlaced = true;
		}
	};
	std::vector<FFlatVertex> vertices;
	std::vector<float> ids;
	std::vector<uint32_t> refs;
	std::vector<uint32_t> indices, offsets, params;
	uint32_t begin = 0;
	for (size_t n = 0; n < units.size(); begin = unitEnd[n], ++n)
	{
		const uint32_t unit = units[n], end = unitEnd[n], surfaceCount = end - begin;
		uint32_t vertexCount = 0, indexCount = 0;
		for (uint32_t i = begin; i < end; ++i)
		{
			vertexCount += cap.Surfaces[i].VertexCount;
			indexCount += cap.Surfaces[i].IndexCount;
		}
		const int32_t slotIndex = wm.UnitSlot[unit];
		if (slotIndex >= 0 && !compacting && !unitGeometry[n])
		{
			auto& old = wm.Slots[slotIndex];
			uint32_t opaqueCount = 0;
			for (uint32_t i = begin; i < end; ++i)
				if (!cap.Surfaces[i].Alpha) opaqueCount += cap.Surfaces[i].IndexCount;
			if (old.Surfaces == surfaceCount && old.VertexCount == vertexCount && old.IndexCount == indexCount && old.OpaqueCount == opaqueCount)
			{
				const uint32_t rowBase = wm.StaticRows + old.SurfaceBase;
				writes.Add(FWorldModeWrite::Rows, size_t(rowBase) * wm.RowBytes, cap.Rows.data() + size_t(begin) * wm.RowBytes, size_t(surfaceCount) * wm.RowBytes);
				placeLight(unit, begin, end, rowBase);
				old.Anim.clear();
				for (auto& anim : cap.Anim)
				{
					if (anim.Row < begin || anim.Row >= end) continue;
					old.Anim.push_back(anim);
					old.Anim.back().Row = rowBase + (anim.Row - begin);
				}
				++wm.RowOnlyUnits;
				++wm.ReproducedUnits;
				continue;
			}
		}
		// [Karto] Same or smaller than the space it already has: overwrite in place instead of growing the tail. Not while
		// compacting, when the tail restarts at zero over space that older slots still hold.
		const bool inPlace = slotIndex >= 0 && !compacting && vertexCount <= wm.Slots[slotIndex].VertexCap &&
			indexCount <= wm.Slots[slotIndex].IndexCap && surfaceCount <= wm.Slots[slotIndex].SurfaceCap;
		const bool slotAvailable = slotIndex >= 0 || wm.Slots.size() < WorldModeSlotCapacity;
		const bool fits = inPlace || (slotAvailable && wm.PoolVertexNext + vertexCount <= wm.PoolVertexCap &&
			wm.PoolIndexNext + indexCount <= wm.PoolIndexCap && wm.PoolSurfaceNext + surfaceCount <= wm.PoolSurfaceCap);
		if (!fits)
		{
			if (!compacting && slotAvailable)
			{
				full = true;
				MarkWorldModeUnit(unit, true);
				continue;
			}
			++wm.Overflows;
			if (unit < wm.UnitLightOK.size()) wm.UnitLightOK[unit] = 0;
			if (slotIndex >= 0)
			{
				RestoreWorldModeUnit(unit, writes);
				const int32_t last = int32_t(wm.Slots.size()) - 1;
				if (slotIndex != last)
				{
					wm.Slots[slotIndex] = std::move(wm.Slots[last]);
					wm.UnitSlot[wm.Slots[slotIndex].Unit] = slotIndex;
				}
				wm.Slots.pop_back();
				wm.UnitSlot[unit] = -1;
				layoutChanged = true;
			}
			continue;
		}
		FWorldModeState::FPoolSlot slot;
		if (inPlace)
		{
			const auto& old = wm.Slots[slotIndex];
			slot.VertexBase = old.VertexBase;
			slot.IndexBase = old.IndexBase;
			slot.SurfaceBase = old.SurfaceBase;
			slot.VertexCap = old.VertexCap;
			slot.IndexCap = old.IndexCap;
			slot.SurfaceCap = old.SurfaceCap;
			++wm.InPlaceUnits;
		}
		else
		{
			slot.VertexBase = wm.PoolVertexNext;
			slot.IndexBase = wm.PoolIndexNext;
			slot.SurfaceBase = wm.PoolSurfaceNext;
			slot.VertexCap = vertexCount;
			slot.IndexCap = indexCount;
			slot.SurfaceCap = surfaceCount;
			wm.PoolVertexNext += vertexCount;
			wm.PoolIndexNext += indexCount;
			wm.PoolSurfaceNext += surfaceCount;
		}
		const uint32_t vertexBase = wm.StaticVertices + slot.VertexBase;
		const uint32_t indexBase = wm.StaticIndices + slot.IndexBase;
		const uint32_t surfaceBase = wm.StaticSurfaces + slot.SurfaceBase;
		const uint32_t rowBase = wm.StaticRows + slot.SurfaceBase;
		slot.Unit = unit;
		slot.Sector = unit < sectors ? int(unit) : (di->Level->sides[unit - sectors].sector ? di->Level->sides[unit - sectors].sector->Index() : -1);
		slot.Surfaces = surfaceCount;
		slot.VertexCount = vertexCount;
		slot.IndexCount = indexCount;
		for (int k = 0; k < 3; ++k) { slot.Bounds[k] = FLT_MAX; slot.Bounds[3 + k] = -FLT_MAX; }
		vertices.clear();
		ids.clear();
		indices.clear();
		offsets.clear();
		for (uint32_t i = begin; i < end; ++i)
		{
			const auto& s = cap.Surfaces[i];
			offsets.push_back(uint32_t(vertices.size()));
			vertices.insert(vertices.end(), cap.Vertices.begin() + s.FirstVertex, cap.Vertices.begin() + s.FirstVertex + s.VertexCount);
			ids.insert(ids.end(), s.VertexCount, float(surfaceBase + (i - begin)));
			for (int k = 0; k < 3; ++k) { slot.Bounds[k] = std::min(slot.Bounds[k], s.Min[k]); slot.Bounds[3 + k] = std::max(slot.Bounds[3 + k], s.Max[k]); }
			wm.SurfaceInfo[surfaceBase + (i - begin)] = { s.Line, s.Sector, s.Subsector, s.Section, s.DynKind, s.DynRef };
		}
		if (end > begin) wm.SurfaceTableRanges.push_back({ surfaceBase, surfaceBase + (end - begin) });
		for (int alpha = 0; alpha < 2; ++alpha)
		{
			const uint32_t first = uint32_t(indices.size());
			for (uint32_t i = begin; i < end; ++i)
			{
				const auto& s = cap.Surfaces[i];
				if (s.Alpha != bool(alpha)) continue;
				for (uint32_t t = 0; t < s.IndexCount; ++t)
					indices.push_back(cap.Indices[s.FirstIndex + t] - s.FirstVertex + vertexBase + offsets[i - begin]);
			}
			(alpha ? slot.AlphaFirst : slot.OpaqueFirst) = indexBase + first;
			(alpha ? slot.AlphaCount : slot.OpaqueCount) = uint32_t(indices.size()) - first;
		}
		writes.Add(FWorldModeWrite::Vertices, size_t(vertexBase) * sizeof(FFlatVertex), vertices.data(), vertices.size() * sizeof(FFlatVertex));
		writes.Add(FWorldModeWrite::SurfaceIDs, size_t(vertexBase) * sizeof(float), ids.data(), ids.size() * sizeof(float));
		refs.clear();
		for (uint32_t i = begin; i < end; ++i)
		{
			const auto& s = cap.Surfaces[i];
			refs.insert(refs.end(), cap.VertexRefs.begin() + size_t(s.FirstVertex) * 4, cap.VertexRefs.begin() + size_t(s.FirstVertex + s.VertexCount) * 4);
		}
		writes.Add(FWorldModeWrite::VertexRefs, size_t(vertexBase) * 4 * sizeof(uint32_t), refs.data(), refs.size() * sizeof(uint32_t));
		writes.Add(FWorldModeWrite::Indices, size_t(indexBase) * sizeof(uint32_t), indices.data(), indices.size() * sizeof(uint32_t));
		writes.Add(FWorldModeWrite::Rows, size_t(rowBase) * wm.RowBytes, cap.Rows.data() + size_t(begin) * wm.RowBytes, size_t(surfaceCount) * wm.RowBytes);
		placeLight(unit, begin, end, rowBase);
		params.clear();
		for (uint32_t i = begin; i < end; ++i) params.push_back(cap.Surfaces[i].Param);
		writes.Add(FWorldModeWrite::SurfaceParams, size_t(surfaceBase) * sizeof(uint32_t), params.data(), params.size() * sizeof(uint32_t));
		for (auto& anim : cap.Anim)
		{
			if (anim.Row < begin || anim.Row >= end) continue;
			slot.Anim.push_back(anim);
			slot.Anim.back().Row = rowBase + (anim.Row - begin);
		}
		if (slotIndex < 0)
		{
			for (uint32_t k = wm.UnitStart[unit]; k < wm.UnitStart[unit + 1]; ++k)
			{
				const uint32_t s = wm.UnitSurfaces[k];
				writes.Add(FWorldModeWrite::Indices, size_t(wm.SurfaceFirst[s]) * sizeof(uint32_t), nullptr, size_t(wm.SurfaceCount[s]) * sizeof(uint32_t));
			}
			wm.UnitSlot[unit] = int32_t(wm.Slots.size());
			wm.Slots.push_back(std::move(slot));
		}
		else wm.Slots[slotIndex] = std::move(slot);
		layoutChanged = true;
		++wm.ReproducedUnits;
	}
	if (full)
	{
		++wm.Compactions;
		wm.PoolVertexNext = wm.PoolIndexNext = wm.PoolSurfaceNext = 0;
		for (auto& slot : wm.Slots) MarkWorldModeUnit(slot.Unit, true);
	}
	if (layoutChanged) RebuildWorldModePoolClusters(state);
	wm.ReproduceNS += I_nsTime() - started;
	++wm.ReproduceFrames;
}

static bool WriteWorldModeSideParams(const side_t& sd, uint32_t side, unsigned kinds, int tic, FWorldModeWriteList& writes)
{
	auto& wm = WorldMode;
	float record[3][4] = {};
	bool moving = false;
	for (int t = 0; t < 3; ++t)
	{
		if (!(kinds & (1u << t))) continue;
		const auto& tier = wm.Tiers[size_t(side) * 3 + t];
		if (tier.State == FWorldModeTier::None) continue;
		if (tier.State == FWorldModeTier::Producer) return false;
		const auto& tt = wm.TierTics[size_t(side) * 3 + t];
		const double dx = tt.CurX - tier.BaseX, dy = tt.CurY - tier.BaseY;
		// [Karto] Only a tier the interpolator moves blends, and only in frames right after the tic its change belongs
		// to. Thinkers change offsets at maptime M and P_Ticker then advances it, so those frames see M + 1; comparing
		// with M would never blend.
		const bool blend = tt.Tic + 1 == tic && sd.textures[t].interpolation != nullptr;
		const double mx = blend ? tt.CurX - tt.PrevX : 0., my = blend ? tt.CurY - tt.PrevY : 0.;
		if (((dx != 0. || mx != 0.) && tier.ClampX) || ((dy != 0. || my != 0.) && tier.ClampY)) return false;
		// [Karto] Shifts are taken from the full offset and wrapped to one texture repeat so a long-running scroller
		// does not grow coordinates until float precision shows; the motion term stays unwrapped so the blend never
		// jumps across a repeat.
		const double u = dx * tier.Kx, v = dy * tier.Ky;
		record[t][0] = float(u - floor(u));
		record[t][1] = float(v - floor(v));
		record[t][2] = float(mx * tier.Kx);
		record[t][3] = float(my * tier.Ky);
		moving |= mx != 0. || my != 0.;
	}
	for (int t = 0; t < 3; ++t)
	{
		if (!(kinds & (1u << t)) || wm.Tiers[size_t(side) * 3 + t].State != FWorldModeTier::Param) continue;
		writes.Add(FWorldModeWrite::Params, (size_t(side) * 3 + t + 1) * sizeof(record[t]), record[t], sizeof(record[t]));
		++wm.ParamWrites;
	}
	if (moving && wm.SideBlendTic[side] != tic)
	{
		wm.SideBlendTic[side] = tic;
		wm.BlendSides.push_back(int(side));
	}
	return true;
}

static bool WriteWorldModePlaneRecords(sector_t& sector, FWorldModeWriteList& writes)
{
	auto& wm = WorldMode;
	const size_t slot = size_t(sector.Index()) * 2;
	if (wm.PlaneState[slot] == FWorldModeTier::Producer || wm.PlaneState[slot + 1] == FWorldModeTier::Producer) return false;
	for (int which = 0; which < 2; ++which)
	{
		if (wm.PlaneState[slot + which] != FWorldModeTier::Param) continue;
		float matrix[32];
		WorldModePlaneMatrixPair(sector, which, matrix);
		writes.Add(FWorldModeWrite::PlaneMatrices, (slot + which) * sizeof(matrix), matrix, sizeof(matrix));
		wm.PlaneShifted[slot + which] = 1;
		++wm.PlaneWrites;
	}
	return true;
}

static void WriteWorldModePlaneHeights(sector_t& sector, FWorldModeWriteList& writes)
{
	float heights[WorldModeHeightFloats * 2];
	WorldModePlaneHeight(sector, sector_t::floor, heights);
	WorldModePlaneHeight(sector, sector_t::ceiling, heights + WorldModeHeightFloats);
	writes.Add(FWorldModeWrite::PlaneHeights, size_t(sector.Index()) * 2 * WorldModeHeightFloats * sizeof(float), heights, sizeof(heights));
}

// [Karto] v of a guarded quad's vertices at the current planes, as the vertex shader computes it; the clamp the producer
// chose stays right only while all of them are within [0, 1] (CheckTexturePosition).
static bool WorldModeClampGuardHolds(FLevelLocals* level, const FWorldModeClampGuard& guard)
{
	auto plane = [&](uint32_t slot) -> const secplane_t& { auto& s = level->sectors[slot / 2]; return slot & 1 ? s.ceilingplane : s.floorplane; };
	const double anchor = level->sectors[guard.Anchor / 2].GetPlaneTexZ(int(guard.Anchor & 1));
	constexpr double slack = 1. / 4096.;
	for (int v = 0; v < 4; ++v)
	{
		const bool top = v == 1 || v == 2;
		const double z = plane(top ? guard.Top : guard.Bottom).ZatPoint(double(guard.X[v]), double(guard.Y[v]));
		const double tv = guard.V[v] + guard.Scale * ((anchor - guard.AnchorZ) - (z - guard.Z[v]));
		if (tv < -slack || tv > 1. + slack) return false;
	}
	return true;
}

static void MarkWorldModeMovedSides(FLevelLocals* level, sector_t& sector, bool floorMoved, bool ceilingMoved)
{
	auto& wm = WorldMode;
	const uint32_t sectors = level->sectors.Size();
	for (auto line : sector.Lines)
	{
		const uint32_t index = line->Index();
		const bool crossed = index < wm.LineOrder.size() && WorldModeLineOrder(*line) != wm.LineOrder[index];
		wm.LineCrossings += crossed;
		for (auto side : line->sidedef)
		{
			if (!side) continue;
			const uint32_t s = side->Index();
			const bool front = side->sector == &sector;
			const uint8_t moved = uint8_t((ceilingMoved ? (front ? WorldModeDepFrontCeiling : WorldModeDepBackCeiling) : 0) |
				(floorMoved ? (front ? WorldModeDepFrontFloor : WorldModeDepBackFloor) : 0));
			bool clampLost = false;
			if (!crossed && s < wm.SideGuards.size())
			{
				for (const auto& guard : wm.SideGuards[s])
				{
					++wm.ClampGuardChecks;
					if (!WorldModeClampGuardHolds(level, guard))
					{
						clampLost = true;
						++wm.ClampGuardFires;
						break;
					}
				}
			}
			if (crossed || clampLost || s >= wm.SideDeps.size() || (wm.SideDeps[s] & moved))
			{
				MarkWorldModeUnit(sectors + s, true);
				++wm.WallSidesReproduced;
				if (!crossed && s < wm.SideUncovered.size()) ++wm.UncoveredReproduced[wm.SideUncovered[s] < 9 ? wm.SideUncovered[s] : 0];
			}
			else ++wm.WallSidesKept;
		}
	}
	++wm.LastDirtySectors;
}

static void WriteWorldModePublishedPlane(sector_t& sector, int which, FWorldModeWriteList& writes)
{
	auto& wm = WorldMode;
	float rec[WorldModeHeightFloats];
	WorldModePlaneHeight(sector, which, rec);
	double d, texz;
	const bool blending = R_PlaneInterpolationStart(&sector, which == sector_t::ceiling, d, texz) &&
		(float(d) != rec[3] || float(texz) != rec[8]);
	if (blending)
	{
		rec[7] = float(d);
		rec[9] = float(texz);
	}
	const size_t slot = size_t(sector.Index()) * 2 + which;
	writes.Add(FWorldModeWrite::PlaneHeights, slot * WorldModeHeightFloats * sizeof(float), rec, sizeof(rec));
	++wm.PublishedPlaneWrites;
	if (slot < wm.PlaneBlending.size())
	{
		if (blending && !wm.PlaneBlending[slot]) wm.BlendingPlanes.push_back(int(slot));
		wm.PlaneBlending[slot] = blending;
	}
}

static void GrowWorldModeSectorBounds(sector_t& sector)
{
	auto& wm = WorldMode;
	const uint32_t s = uint32_t(sector.Index());
	if (wm.SectorClusterStart.size() <= s + 1 || wm.SectorBounds.size() < size_t(s + 1) * 4) return;
	const float* b = &wm.SectorBounds[size_t(s) * 4];
	float zlo = FLT_MAX, zhi = -FLT_MAX;
	for (const secplane_t* plane : { &sector.floorplane, &sector.ceilingplane })
		for (int corner = 0; corner < 4; ++corner)
		{
			const float z = float(plane->ZatPoint(double(b[corner & 1 ? 2 : 0]), double(b[corner & 2 ? 3 : 1])));
			zlo = std::min(zlo, z);
			zhi = std::max(zhi, z);
		}
	for (uint32_t k = wm.SectorClusterStart[s]; k < wm.SectorClusterStart[s + 1]; ++k)
	{
		float* c = &wm.ClusterBounds[size_t(wm.SectorClusters[k]) * 6];
		c[1] = std::min(c[1], zlo);
		c[4] = std::max(c[4], zhi);
	}
	auto grow = [&](uint32_t unit)
	{
		if (unit >= wm.UnitSlot.size() || wm.UnitSlot[unit] < 0) return;
		auto& slot = wm.Slots[wm.UnitSlot[unit]];
		slot.Bounds[1] = std::min(slot.Bounds[1], zlo);
		slot.Bounds[4] = std::max(slot.Bounds[4], zhi);
		wm.PoolBoundsDirty = true;
	};
	grow(s);
	const uint32_t sectors = uint32_t(wm.SectorClusterStart.size() - 1);
	for (auto line : sector.Lines)
		for (auto side : line->sidedef)
			if (side) grow(sectors + uint32_t(side->Index()));
}

static bool WorldModeSectorSeenIndex(const HWDrawInfo* di, int index)
{
	auto& wm = WorldMode;
	if (wm.SectorStamp[index] == wm.VisibleStamp) return true;
	if (wm.SectorBounds.size() != wm.SectorStamp.size() * 4) return true;
	if (index < int(wm.SeenFrame.size()) && wm.SeenFrame[index] == wm.UpdateFrames) return wm.SeenValue[index];
	const float* b = &wm.SectorBounds[size_t(index) * 4];
	const float camX = float(di->Viewpoint.Pos.X), camY = float(di->Viewpoint.Pos.Y), nearRadius = r_worldmode_nearradius;
	const float dx = WorldModeGap(b[0], b[2], camX), dy = WorldModeGap(b[1], b[3], camY);
	const bool seen = dx * dx + dy * dy <= nearRadius * nearRadius;
	if (index < int(wm.SeenFrame.size()))
	{
		wm.SeenFrame[index] = wm.UpdateFrames;
		wm.SeenValue[index] = seen;
	}
	return seen;
}

static bool WorldModeSectorSeen(const HWDrawInfo* di, const sector_t& sector)
{
	return WorldModeSectorSeenIndex(di, sector.Index());
}

static bool WorldModeHotSeen(const HWDrawInfo* di, const sector_t& sector)
{
	if (!r_worldmode_hotgate || !r_worldmode_gpuvis || !WorldMode.VisibleStamp) return true;
	if (WorldModeSectorSeen(di, sector)) return true;
	const auto& wm = WorldMode;
	const int s = sector.Index();
	for (uint32_t k = wm.NeighbourStart[s], end = wm.NeighbourStart[s + 1]; k < end; ++k)
		if (WorldModeSectorSeenIndex(di, wm.Neighbours[k])) return true;
	return false;
}

static bool WorldModeUnitSeen(const HWDrawInfo* di, uint32_t unit)
{
	const uint32_t sectors = di->Level->sectors.Size();
	if (unit < sectors) return WorldModeHotSeen(di, di->Level->sectors[unit]);
	if (!r_worldmode_hotgate || !r_worldmode_gpuvis || !WorldMode.VisibleStamp) return true;
	const size_t i = size_t(unit - sectors) * 2;
	if (i + 1 >= WorldMode.SideSectors.size()) return true;
	const int own = WorldMode.SideSectors[i], other = WorldMode.SideSectors[i + 1];
	return (own >= 0 && WorldModeSectorSeenIndex(di, own)) || (other >= 0 && WorldModeSectorSeenIndex(di, other));
}

// [Karto] Re-produce only what can be seen; unseen dirty units wait. A pool compaction moves every slot, so deferred
// slot units must be re-produced with it or their old ranges would draw other units' vertices.
static void SelectWorldModeUnits(const HWDrawInfo* di)
{
	auto& wm = WorldMode;
	const bool compacting = wm.PoolVertexNext == 0 && wm.PoolIndexNext == 0 && wm.PoolSurfaceNext == 0 && !wm.Slots.empty();
	if (compacting || !r_worldmode_hotgate)
	{
		if (!wm.DeferredUnits.empty()) ++wm.DeferFlushes;
		wm.UnitsReleased += wm.DeferredUnits.size();
		wm.DirtyUnits.insert(wm.DirtyUnits.end(), wm.DeferredUnits.begin(), wm.DeferredUnits.end());
		wm.DeferredUnits.clear();
		return;
	}
	std::vector<uint32_t> fresh, waiting;
	fresh.swap(wm.DirtyUnits);
	waiting.swap(wm.DeferredUnits);
	auto route = [&](uint32_t unit, bool isFresh)
	{
		if (WorldModeUnitSeen(di, unit))
		{
			wm.DirtyUnits.push_back(unit);
			wm.UnitsReleased += !isFresh;
		}
		else
		{
			wm.DeferredUnits.push_back(unit);
			wm.UnitsDeferred += isFresh;
		}
	};
	for (uint32_t unit : fresh) route(unit, true);
	for (uint32_t unit : waiting) route(unit, false);
}

static void AddWorldModeLightPatch(FWorldModeWriteList& writes, uint32_t row, size_t rowBytes, const FWorldModeLightPatch& patch)
{
	size_t at = 0;
	for (int k = 0; k < patch.Count; ++k)
	{
		writes.Add(FWorldModeWrite::Rows, size_t(row) * rowBytes + patch.Range[k].Offset, patch.Data + at, patch.Range[k].Bytes);
		at += patch.Range[k].Bytes;
	}
}

static void WriteWorldModeFlash(HWDrawInfo* di, FRenderState& state, FWorldModeWriteList& writes)
{
	auto& wm = WorldMode;
	const int extra = getExtraLight();
	const bool placed = wm.FlashPlaced;
	wm.FlashPlaced = false;
	if (extra == wm.FlashApplied && !(placed && extra != 0)) return;
	wm.FlashApplied = extra;
	FWorldModeLightPatch patch;
	auto write = [&](const FWorldModeFlashRow& row)
	{
		if (WorldModeFlashPatch(di, state, row, extra, patch)) AddWorldModeLightPatch(writes, row.Row, wm.RowBytes, patch);
	};
	for (const auto& row : wm.FlashRows) write(row);
	for (const auto& list : wm.UnitFlash)
		for (const auto& row : list) write(row);
}

static bool WriteWorldModeLight(HWDrawInfo* di, FRenderState& state, sector_t& sector, FWorldModeWriteList& writes)
{
	auto& wm = WorldMode;
	if (!r_worldmode_params || di->isFullbrightScene() || wm.UnitLight.empty()) return false;
	if (!WorldModeHotSeen(di, sector)) return false;
	const uint32_t sectors = di->Level->sectors.Size();
	uint32_t units[64];
	int count = 0;
	units[count++] = uint32_t(sector.Index());
	for (auto line : sector.Lines)
	{
		for (auto side : line->sidedef)
		{
			if (!side || side->sector != &sector) continue;
			if (count == 64) return false;
			units[count++] = sectors + uint32_t(side->Index());
		}
	}
	for (int i = 0; i < count; ++i)
	{
		if (!wm.UnitLightOK[units[i]])
		{
			++wm.LightFallbacks;
			return false;
		}
	}
	FWorldModeLightPatch patch;
	const int extra = !isSoftwareLighting(di->lightmode) || sector.Colormap.BlendFactor > 0 ? wm.FlashApplied : 0;
	for (int i = 0; i < count; ++i)
	{
		for (const auto& row : wm.UnitLight[units[i]])
		{
			if (!WorldModeLightPatch(di, state, row.Recipe, extra, patch)) return false;
			AddWorldModeLightPatch(writes, row.Row, wm.RowBytes, patch);
			++wm.LightRowWrites;
		}
	}
	++wm.LightSectors;
	return true;
}

// [Karto] A horizon's split depends on the view height (DoHorizon), so the frame registers the portals the classic walk
// would.
void HWDrawInfo::ReplayWorldModeHorizons()
{
	auto& wm = WorldMode;
	if (wm.Horizons.empty()) return;
	float planes[24];
	WorldModePlanes(VPUniforms, planes);
	HWWallDispatcher dispatcher(this);
	const DVector2 view = Viewpoint.Pos.XY();
	for (auto& stored : wm.Horizons)
	{
		seg_t* seg = stored.seg;
		const DVector2 v1 = seg->v1->fPos(), d = seg->v2->fPos() - v1;
		if ((view.Y - v1.Y) * d.X > (view.X - v1.X) * d.Y) continue;
		const float box[6] = { std::min(stored.glseg.x1, stored.glseg.x2) - 1.f, -1e9f, std::min(stored.glseg.y1, stored.glseg.y2) - 1.f,
			std::max(stored.glseg.x1, stored.glseg.x2) + 1.f, 1e9f, std::max(stored.glseg.y1, stored.glseg.y2) + 1.f };
		if (!WorldModeBoxVisible(planes, box)) continue;
		HWWall wall = stored;
		wall.DoHorizon(&dispatcher, seg, hw_FakeFlat(seg->frontsector, in_area, false), seg->v1, seg->v2);
	}
}

// [Karto] Translucent walls sort with sprites, so they are not baked; re-producing them keeps script texture changes.
void HWDrawInfo::ReplayWorldModeTranslucentWalls()
{
	auto& wm = WorldMode;
	if (wm.TranslucentSegs.empty()) return;
	float planes[24];
	WorldModePlanes(VPUniforms, planes);
	const uint8_t* open = nullptr;
	DVector2 from;
	const bool occlude = WorldModeOpenSectors(Level, open, from);
	const DVector2 view = Viewpoint.Pos.XY();
	HWMeshHelper mesh;
	HWWallDispatcher sweep(Level, &mesh, lightmode);
	for (auto seg : wm.TranslucentSegs)
	{
		if (occlude && !open[seg->frontsector->Index()]) continue;
		const DVector2 v1 = seg->v1->fPos(), d = seg->v2->fPos() - v1;
		if ((view.Y - v1.Y) * d.X > (view.X - v1.X) * d.Y) continue;
		const DVector2 l1 = seg->linedef->v1->fPos(), l2 = seg->linedef->v2->fPos();
		const float box[6] = { float(std::min(l1.X, l2.X)) - 1.f, -1e9f, float(std::min(l1.Y, l2.Y)) - 1.f,
			float(std::max(l1.X, l2.X)) + 1.f, 1e9f, float(std::max(l1.Y, l2.Y)) + 1.f };
		if (!WorldModeBoxVisible(planes, box)) continue;
		ProcessWorldModeSeg(this, sweep, *seg);
	}
	HWWallDispatcher dispatcher(this);
	for (auto& wall : mesh.translucent) wall.PutWall(&dispatcher, true);
}

void HWDrawInfo::CollectWorldModeMissingTextures()
{
	auto& wm = WorldMode;
	if (!wm.PlanesMoved || wm.MovedListed.size() != Level->sectors.Size() || wm.MissingListed.size() != Level->segs.Size()) return;
	auto untextured = [](side_t* side)
	{
		for (int tier : { side_t::top, side_t::bottom })
		{
			auto tex = TexMan.GetGameTexture(side->GetTexture(tier), true);
			if (!tex || !tex->isValid()) return true;
		}
		return false;
	};
	for (unsigned i = 0; i < Level->sectors.Size(); ++i)
	{
		if (wm.MovedListed[i] || !wm.PlanesMoved[i].load(std::memory_order_relaxed)) continue;
		wm.MovedListed[i] = 1;
		for (auto line : Level->sectors[i].Lines)
		{
			if (!line->sidedef[1]) continue;
			for (auto side : line->sidedef)
			{
				if (!untextured(side)) continue;
				for (int j = 0; j < side->numsegs; ++j)
				{
					seg_t* seg = side->segs[j];
					if (wm.MissingListed[seg->Index()]) continue;
					wm.MissingListed[seg->Index()] = 1;
					wm.MissingSegs.push_back(seg);
				}
			}
		}
	}
	const uint8_t* open = nullptr;
	DVector2 from;
	const bool occlude = WorldModeOpenSectors(Level, open, from);
	const DVector2 view = Viewpoint.Pos.XY();
	wm.MissingVisible.clear();
	for (auto seg : wm.MissingSegs)
	{
		if (!seg->frontsector || !seg->backsector || !seg->PartnerSeg) continue;
		if (occlude && !open[seg->frontsector->Index()]) continue;
		const DVector2 v1 = seg->v1->fPos(), d = seg->v2->fPos() - v1;
		if ((view.Y - v1.Y) * d.X > (view.X - v1.X) * d.Y) continue;
		wm.MissingVisible.push_back(seg);
	}
	if (wm.MissingVisible.empty()) return;
	// [Karto] A worker tic may be running: sector heights and textures are plain values, but 3D floor lists are rebuilt,
	// so only those are read under the rebuild lock.
	std::shared_lock<std::shared_mutex> xfloors(XFloorRebuildLock);
	HWMeshHelper mesh;
	HWWallDispatcher sweep(Level, &mesh, lightmode);
	for (auto seg : wm.MissingVisible) ProcessWorldModeSeg(this, sweep, *seg);
	for (auto& m : mesh.lower) AddLowerMissingTexture(m.side, m.sub, float(m.plane));
	for (auto& m : mesh.upper) AddUpperMissingTexture(m.side, m.sub, float(m.plane));
}

// [Karto] No traversal registers sky portals in world mode, so sky walls do it each frame; unlike the classic walk,
// every frustum-visible sky wall registers.
void HWDrawInfo::ReplayWorldModeSky()
{
	auto& wm = WorldMode;
	if (wm.SkyWalls.size() != Level->sides.Size() || r_worldmode_sky <= 0) return;
	const uint64_t started = I_nsTime();
	float planes[24];
	WorldModePlanes(VPUniforms, planes);
	HWWallDispatcher dispatcher(this);
	const uint8_t* entered = nullptr;
	if (!WorldModeWalkSectors(Level, entered)) entered = nullptr;
	// [Karto] HWSkyInfo::init reads a sector only through its sky transfer and its plane's sky textures, so walls equal
	// in those (and fade) share a portal, found once per frame.
	struct FCachedSky { int Transfer, Plane, Tex1, Tex2; uint32_t Fade; HWPortal* Portal; };
	FCachedSky cached[16];
	int cachedCount = 0;
	size_t kept = 0;
	uint32_t replayed = 0;
	for (int side : wm.SkySides)
	{
		auto& walls = wm.SkyWalls[side];
		if (walls.empty() || wm.SkySideFrame[side] == wm.SkyFrames) continue;
		wm.SkySideFrame[side] = wm.SkyFrames;
		wm.SkySides[kept++] = side;
		if (!WorldModeBoxVisible(planes, &wm.SkySideBounds[size_t(side) * 6]))
		{
			wm.SkyCulled += walls.size();
			continue;
		}
		const size_t pair = size_t(side) * 2;
		const bool walked = entered && pair + 1 < wm.SideSectors.size() &&
			((wm.SideSectors[pair] >= 0 && entered[wm.SideSectors[pair]]) || (wm.SideSectors[pair + 1] >= 0 && entered[wm.SideSectors[pair + 1]]));
		if (r_worldmode_sky == 1 && !walked && !WorldModeUnitSeen(this, Level->sectors.Size() + uint32_t(side)))
		{
			wm.SkyCulled += walls.size();
			continue;
		}
		const auto& quads = wm.SkyQuads[side];
		for (size_t w = 0; w < quads.size(); ++w)
		{
			const auto& q = quads[w];
			if (!WorldModeBoxVisible(planes, q.Bounds))
			{
				++wm.SkyCulled;
				continue;
			}
			const auto& skyplane = q.SkySector->planes[q.SkyPlane];
			const int transfer = q.SkySector->skytransfer, tex1 = skyplane.skytexture[0].GetIndex(), tex2 = skyplane.skytexture[1].GetIndex();
			HWPortal* portal = nullptr;
			for (int i = 0; i < cachedCount && !portal; ++i)
			{
				const auto& c = cached[i];
				if (c.Transfer == transfer && c.Plane == q.SkyPlane && c.Tex1 == tex1 && c.Tex2 == tex2 && c.Fade == q.Fade) portal = c.Portal;
			}
			if (portal && !q.Prepared && !(gl_seamless && q.Splittable) && !vk_gpu_driven_bake)
			{
				auto alloc = screen->mVertexData->AllocVertices(4);
				memcpy(alloc.first, q.Quad, sizeof(q.Quad));
				portal->AddSkyPrim(alloc.second, 4, q.Quad[0].x, q.Quad[0].y, q.Quad[2].x, q.Quad[2].y);
				if (q.SkyPortalPlane != -1) portal->planesused |= (1 << q.SkyPortalPlane);
				++wm.SkyReplayed;
				++replayed;
				continue;
			}
			HWWall& stored = walls[w].Wall;
			if (portal)
			{
				const auto keptIndex = stored.vertindex;
				const auto keptCount = stored.vertcount;
				stored.MakeVertices(false);
				portal->AddSkyPrim(stored.vertindex, stored.vertcount, stored);
				stored.vertindex = keptIndex;
				stored.vertcount = keptCount;
				if (stored.skyportalplane != -1) portal->planesused |= (1 << stored.skyportalplane);
			}
			else
			{
				HWWall wall = stored;
				wall.ReplaySky(&dispatcher);
				HWSkyInfo skyinfo;
				skyinfo.init(this, wall.skysector, wall.skyplane, wall.skysector->skytransfer, wall.Colormap.FadeColor);
				portal = FindPortal(portalState.UniqueSkies.Get(&skyinfo));
				if (portal && portal->LineCount() <= 1) portal->mSkyPrims.Grow(2 * wm.SkyLastWalls);
				if (portal && cachedCount < 16) cached[cachedCount++] = { transfer, wall.skyplane, tex1, tex2, q.Fade, portal };
			}
			++wm.SkyReplayed;
			++replayed;
		}
	}
	wm.SkyLastWalls = replayed;
	wm.SkySides.resize(kept);
	++wm.SkyFrames;
	wm.SkyNS += I_nsTime() - started;
}

static bool WorldModeDrawListsEmpty(HWDrawInfo* di)
{
	for (auto& list : di->drawlists)
		if (list.Size() || list.flats.Size()) return false;
	return true;
}

// [Karto] Lists are built in the same order and with the same tests as HWWall::SetupLights and HWFlat::SetupLights.
// Keys: side s is s; section k's floor and ceiling are sides + 2k and sides + 2k + 1.
namespace
{
	struct FWorldModeLightRecord
	{
		float Pack[16] = {};
		int Array = -1;
		uint32_t Seen = 0, LinkSerial = 0;
		std::vector<uint32_t> Keys;
		std::vector<std::pair<int, int32_t>> Slots;
		DVector3 Pos;
		float Radius = 0.f;
	};
	std::unordered_map<FDynamicLight*, FWorldModeLightRecord> WorldModeLightRecords;
	std::vector<int32_t> WorldModeFreeLightSlots;
	std::vector<uint8_t> WorldModeKeyTiled;
	uint64_t WorldModeSlotWrites, WorldModeFlipTests, WorldModeFlipDirty;
	uint64_t WorldModeScanNS, WorldModeRebuildNS, WorldModePlainBytes, WorldModeTiledBytes, WorldModeTiledDirty, WorldModeKeysPlaced, WorldModeTableRanges, WorldModeTableFull, WorldModeTableDiffs;
}

struct FWorldModeLightTarget
{
	Plane P;
	const secplane_t* Flat = nullptr;
	int Group = 0;
	bool Ceiling = false;
};

static bool WorldModeLightTargetFor(FLevelLocals* level, uint32_t key, FWorldModeLightTarget& t)
{
	const uint32_t sides = level->sides.Size();
	if (key < sides)
	{
		side_t& side = level->sides[key];
		line_t* line = side.linedef;
		const bool front = line->sidedef[0] == &side;
		const vertex_t* v1 = front ? line->v1 : line->v2;
		const vertex_t* v2 = front ? line->v2 : line->v1;
		const float x1 = v1->fX(), y1 = v1->fY(), x2 = v2->fX(), y2 = v2->fY();
		const float nx = y2 - y1, ny = x1 - x2, il = 1.f / sqrtf(nx * nx + ny * ny);
		const FVector3 normal(nx * il, 0, ny * il);
		t.P.Set(normal, -normal.X * x1 - normal.Z * y1);
		t.Flat = nullptr;
		t.Group = side.sector->PortalGroup;
		return side.dlist.Size() > 0;
	}
	FSection& section = level->sections.allSections[(key - sides) / 2];
	sector_t* sector = section.sector;
	t.Ceiling = ((key - sides) & 1) != 0;
	t.Flat = t.Ceiling ? &sector->ceilingplane : &sector->floorplane;
	t.P.Set(t.Flat->Normal(), t.Flat->fD());
	t.Group = sector->PortalGroup;
	return section.dlist.Size() > 0;
}

#ifdef _MSC_VER
__declspec(noinline)
#else
__attribute__((noinline))
#endif
static bool WorldModeLightReaches(FWorldModeLightTarget& t, const DVector3& pos, const DVector3& rel, float radius)
{
	if (t.Flat)
	{
		const double planeh = t.Flat->ZatPoint(pos);
		if ((planeh < pos.Z && t.Ceiling) || (planeh > pos.Z && !t.Ceiling)) return false;
	}
	return LightReachesPlane(t.P, rel, radius, !t.Flat);
}

// [Karto] The radius a light's lists and tiles are decided by: the largest it can reach with its current settings (a
// flicker or pulse light keeps its link radius), so a radius change is only a slot write. A list may hold a light
// beyond its current radius; the shader reads the current radius and such a light adds exactly zero.
static float WorldModeLightReach(const FDynamicLight* light)
{
	const float current = light->GetRadius();
	return current > 0.f ? std::max(current, light->radius) : 0.f;
}

static void WorldModeLightKeyDirty(uint32_t key)
{
	auto& wm = WorldMode;
	if (key >= wm.KeyDirty.size() || wm.KeyDirty[key]) return;
	wm.KeyDirty[key] = 1;
	wm.DirtyKeys.push_back(key);
}

static void WorldModeLightSectorMoved(FLevelLocals* level, sector_t& sector)
{
	auto& wm = WorldMode;
	if (wm.KeyDirty.empty()) return;
	const uint32_t sides = level->sides.Size();
	for (auto& section : level->sections.SectionsForSector(&sector))
	{
		WorldModeLightKeyDirty(sides + uint32_t(section.Index()) * 2);
		WorldModeLightKeyDirty(sides + uint32_t(section.Index()) * 2 + 1);
	}
}

static void ResetWorldModeLights(FLevelLocals* level)
{
	auto& wm = WorldMode;
	const size_t keys = size_t(level->sides.Size()) + size_t(level->sections.allSections.Size()) * 2;
	wm.KeyOffset.assign(keys, -1);
	wm.KeyCapacity.assign(keys, 0);
	wm.KeyDirty.assign(keys, 0);
	wm.DirtyKeys.clear();
	wm.LightNext = 0;
	wm.SurfaceTableDirty = true;
	WorldModeLightRecords.clear();
	WorldModeFreeLightSlots.clear();
	WorldModeKeyTiled.assign(keys, 0);
}

static int32_t AllocWorldModeLightSlot()
{
	auto& wm = WorldMode;
	if (!WorldModeFreeLightSlots.empty())
	{
		const int32_t slot = WorldModeFreeLightSlots.back();
		WorldModeFreeLightSlots.pop_back();
		return slot;
	}
	if (wm.LightNext + 4 > WorldModeLightCapacity)
	{
		wm.LightCompact = true;
		++wm.LightOverflows;
		return -1;
	}
	const int32_t slot = int32_t(wm.LightNext);
	wm.LightNext += 4;
	return slot;
}

static void WriteWorldModeLightSlot(int32_t slot, int group, FDynamicLight* drawn, FWorldModeWriteList& writes)
{
	static FDynLightData one;
	one.Clear();
	AddLightToList(one, group, drawn, false);
	for (int a = 0; a < 3; ++a)
	{
		if (one.arrays[a].Size() == 0) continue;
		writes.Add(FWorldModeWrite::WorldLights, size_t(slot) * 16, &one.arrays[a][0], 16 * sizeof(float));
		++WorldModeSlotWrites;
		return;
	}
}

static int32_t WorldModeLightSlot(FWorldModeLightRecord& record, int group, FDynamicLight* drawn, FWorldModeWriteList& writes)
{
	for (const auto& s : record.Slots)
		if (s.first == group) return s.second;
	const int32_t slot = AllocWorldModeLightSlot();
	if (slot < 0) return -1;
	record.Slots.push_back({ group, slot });
	WriteWorldModeLightSlot(slot, group, drawn, writes);
	return slot;
}

static void FreeWorldModeLightSlots(FWorldModeLightRecord& record)
{
	for (const auto& s : record.Slots) WorldModeFreeLightSlots.push_back(s.second);
	record.Slots.clear();
}

// [Karto] A light left out of a tile is beyond its radius for every fragment there, so it contributes exactly zero.
CVAR(Bool, r_worldmode_lighttiles, true, 0)
static constexpr uint32_t WorldModeLightTileMin = 16;

static bool BuildWorldModeLightTiles(const FDynLightData& lightdata, std::vector<float>& out)
{
	float minX = FLT_MAX, minY = FLT_MAX, maxX = -FLT_MAX, maxY = -FLT_MAX;
	for (int a = 0; a < 3; ++a)
	{
		for (unsigned j = 0; j < lightdata.arrays[a].Size(); j += 16)
		{
			const float* p = &lightdata.arrays[a][j];
			const float reach = p[3] + 1.f;
			minX = std::min(minX, p[0] - reach); maxX = std::max(maxX, p[0] + reach);
			minY = std::min(minY, p[2] - reach); maxY = std::max(maxY, p[2] + reach);
		}
	}
	if (minX > maxX) return false;
	float size = 256.f;
	int width, height;
	for (;;)
	{
		width = std::max(1, int(ceilf((maxX - minX) / size)));
		height = std::max(1, int(ceilf((maxY - minY) / size)));
		if (width * height <= 4096) break;
		size *= 2.f;
	}
	if (width * height == 1) return false;
	const int tiles = width * height;
	static std::vector<uint32_t> members[3];
	static std::vector<uint32_t> counts[3], starts[3];
	for (int a = 0; a < 3; ++a)
	{
		counts[a].assign(tiles + 1, 0);
		members[a].clear();
	}
	auto overlaps = [&](const float* p, int tx, int ty, float reach)
	{
		const float x0 = minX + tx * size, y0 = minY + ty * size;
		const float dx = WorldModeGap(x0, x0 + size, p[0]), dy = WorldModeGap(y0, y0 + size, p[2]);
		return dx * dx + dy * dy <= reach * reach;
	};
	auto tileRange = [&](const float* p, float reach, int& tx0, int& tx1, int& ty0, int& ty1)
	{
		tx0 = std::clamp(int(floorf((p[0] - reach - minX) / size)), 0, width - 1);
		tx1 = std::clamp(int(floorf((p[0] + reach - minX) / size)), 0, width - 1);
		ty0 = std::clamp(int(floorf((p[2] - reach - minY) / size)), 0, height - 1);
		ty1 = std::clamp(int(floorf((p[2] + reach - minY) / size)), 0, height - 1);
	};
	for (int a = 0; a < 3; ++a)
	{
		for (unsigned j = 0; j < lightdata.arrays[a].Size(); j += 16)
		{
			const float* p = &lightdata.arrays[a][j];
			const float reach = p[3] + 1.f;
			int tx0, tx1, ty0, ty1;
			tileRange(p, reach, tx0, tx1, ty0, ty1);
			for (int ty = ty0; ty <= ty1; ++ty)
				for (int tx = tx0; tx <= tx1; ++tx)
					if (overlaps(p, tx, ty, reach)) ++counts[a][ty * width + tx + 1];
		}
		for (int t = 0; t < tiles; ++t) counts[a][t + 1] += counts[a][t];
		starts[a] = counts[a];
		members[a].resize(counts[a][tiles]);
		for (unsigned j = 0; j < lightdata.arrays[a].Size(); j += 16)
		{
			const float* p = &lightdata.arrays[a][j];
			const float reach = p[3] + 1.f;
			int tx0, tx1, ty0, ty1;
			tileRange(p, reach, tx0, tx1, ty0, ty1);
			for (int ty = ty0; ty <= ty1; ++ty)
				for (int tx = tx0; tx <= tx1; ++tx)
					if (overlaps(p, tx, ty, reach)) members[a][starts[a][ty * width + tx]++] = j;
		}
	}
	const size_t slotVec4s = (size_t(tiles) + 3) / 4;
	out.assign((2 + slotVec4s) * 4, -1.f);
	out[1] = minX; out[2] = minY; out[3] = float(width);
	out[4] = float(height); out[5] = size; out[6] = 0.f; out[7] = 0.f;
	for (int t = 0; t < tiles; ++t)
	{
		uint32_t n[3];
		for (int a = 0; a < 3; ++a) n[a] = counts[a][t + 1] - counts[a][t];
		if (n[0] + n[1] + n[2] == 0) continue;
		out[8 + t] = float(out.size() / 4);
		out.push_back(0.f);
		out.push_back(float(n[0]));
		out.push_back(float(n[0] + n[1]));
		out.push_back(float(n[0] + n[1] + n[2]));
		for (int a = 0; a < 3; ++a)
			for (uint32_t m = counts[a][t]; m < counts[a][t + 1]; ++m)
				out.insert(out.end(), { lightdata.arrays[a][members[a][m] + 4], 0.f, 0.f, 0.f });
	}
	return true;
}

static std::vector<uint16_t> WorldModeKeyLights;

static void PrintWorldModeListLengths()
{
	if (!primaryLevel) return;
	int lights = 0;
	for (auto light = primaryLevel->lights; light; light = light->next) lights += light->IsActive();
	const uint32_t sides = primaryLevel->sides.Size();
	uint64_t count[2] = {}, sum[2] = {}, maxLights[2] = {}, over[2][3] = {};
	for (size_t key = 0; key < WorldModeKeyLights.size(); ++key)
	{
		const uint16_t n = WorldModeKeyLights[key];
		if (n == 0) continue;
		const int flat = key >= sides ? 1 : 0;
		++count[flat];
		sum[flat] += n;
		maxLights[flat] = std::max<uint64_t>(maxLights[flat], n);
		over[flat][0] += n >= 8;
		over[flat][1] += n >= 32;
		over[flat][2] += n >= 128;
	}
	Printf("WMLIGHTSTATS active_lights=%d\n", lights);
	for (int f = 0; f < 2; ++f)
		Printf("WMLIGHTSTATS %s lit_lists=%llu mean=%.1f max=%llu ge8=%llu ge32=%llu ge128=%llu\n", f ? "flats" : "walls", (unsigned long long)count[f],
			count[f] ? double(sum[f]) / count[f] : 0.0, (unsigned long long)maxLights[f], (unsigned long long)over[f][0], (unsigned long long)over[f][1],
			(unsigned long long)over[f][2]);
}

CVAR(Bool, r_worldmode_lightspertic, true, 0)
EXTERN_CVAR(Bool, gl_light_shadowmap)

CVAR(Bool, r_worldmode_lightcensus, false, 0)
struct FWorldModeLightCensus { uint64_t Changes = 0, Lists = 0, Link = 0, Position = 0, Radius = 0, Colour = 0, Other = 0; };
static std::map<std::pair<int, FName>, FWorldModeLightCensus> WorldModeLightCensus;
static uint64_t WorldModeLightCensusTics;

static void WorldModeLightCensusAdd(FDynamicLight* light, const FWorldModeLightRecord& record, int array, const float* pack)
{
	auto& c = WorldModeLightCensus[{ int(light->GetLightType()), light->target ? light->target->GetClass()->TypeName : NAME_None }];
	++c.Changes;
	c.Lists += light->touchlists.wall_tlist.Size() + light->touchlists.flat_tlist.Size() * 2;
	const bool fresh = record.Array != array;
	c.Link += !fresh && record.LinkSerial != light->LinkSerial;
	c.Position += fresh || memcmp(record.Pack, pack, 3 * sizeof(float)) != 0;
	c.Radius += !fresh && record.Pack[3] != pack[3];
	c.Colour += !fresh && memcmp(record.Pack + 4, pack + 4, 3 * sizeof(float)) != 0;
	c.Other += !fresh && memcmp(record.Pack + 7, pack + 7, 9 * sizeof(float)) != 0;
}

CCMD(worldmode_light_census)
{
	static const char* types[] = { "point", "pulse", "flicker", "randomflicker", "sector", "dummy", "colorpulse", "colorflicker", "randomcolorflicker" };
	std::vector<std::pair<std::pair<int, FName>, FWorldModeLightCensus>> rows(WorldModeLightCensus.begin(), WorldModeLightCensus.end());
	std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second.Lists > b.second.Lists; });
	uint64_t changes = 0, lists = 0;
	for (const auto& row : rows) changes += row.second.Changes, lists += row.second.Lists;
	Printf("WMLIGHTCENSUS full_scans=%llu changes=%llu lists=%llu\n", (unsigned long long)WorldModeLightCensusTics, (unsigned long long)changes,
		(unsigned long long)lists);
	for (size_t i = 0; i < rows.size() && i < 25; ++i)
	{
		const auto& r = rows[i].second;
		const int type = rows[i].first.first;
		Printf("WMLIGHTCENSUS type=%s owner=%s changes=%llu lists=%llu link=%llu position=%llu radius=%llu colour=%llu other=%llu\n",
			type >= 0 && type < 9 ? types[type] : "?", rows[i].first.second.GetChars(), (unsigned long long)r.Changes, (unsigned long long)r.Lists,
			(unsigned long long)r.Link, (unsigned long long)r.Position, (unsigned long long)r.Radius, (unsigned long long)r.Colour,
			(unsigned long long)r.Other);
	}
	if (argv.argc() == 2 && !stricmp(argv[1], "reset")) WorldModeLightCensus.clear(), WorldModeLightCensusTics = 0;
}

static void UpdateWorldModeLights(HWDrawInfo* di, FWorldModeWriteList& writes)
{
	auto& wm = WorldMode;
	const size_t surfaces = wm.SurfaceInfo.size();
	if (!r_worldmode_lights || surfaces == 0)
	{
		if (!wm.SurfaceTableRanges.empty()) wm.SurfaceTableDirty = true;
		wm.SurfaceTableRanges.clear();
		return;
	}
	const uint64_t started = I_nsTime();
	auto* level = di->Level;
	const uint32_t sides = level->sides.Size();
	const size_t keys = size_t(sides) + size_t(level->sections.allSections.Size()) * 2;
	static bool tiles = r_worldmode_lighttiles;
	if (tiles != r_worldmode_lighttiles)
	{
		tiles = r_worldmode_lighttiles;
		wm.LightCompact = true;
	}
	bool reset = false;
	if (wm.KeyOffset.size() != keys || wm.LightLevel != level || wm.LightCompact)
	{
		ResetWorldModeLights(level);
		wm.LightLevel = level;
		wm.LightCompact = false;
		reset = true;
	}
	const bool lit = level->HasDynamicLights && !di->isFullbrightScene() && !r_worldmode_lightsoff;
	const uint32_t scanKey = (lit ? 1u : 0u) | (screen->mShadowMap.Enabled() ? 2u : 0u) | (gl_light_shadowmap ? 4u : 0u);
	const bool current = r_worldmode_lightspertic && WorldModeSnapshotCurrent(level);
	const uint32_t stamp = current ? WorldModeGpuSpriteSerial() : 0;
	const bool fullScan = !current || reset || stamp != wm.LightScanStamp || scanKey != wm.LightScanKey;
	wm.LightScanStamp = stamp;
	wm.LightScanKey = scanKey;
	const uint32_t frame = fullScan ? ++wm.LightFrame : wm.LightFrame;
	// [Karto] With a current snapshot every light read below goes to it: the tic worker owns the live lights and their
	// link lists while frames draw, and a light it is still setting up can look like a spot light with no angles yet.
	const bool snapped = WorldModeSnapshotCurrent(level);
	const auto& snap = *FrontSnapshot;
	auto touchKeys = [&](uint32_t index, FDynamicLight* light, std::vector<uint32_t>& out)
	{
		out.clear();
		if (snapped)
		{
			out.assign(snap.LightTouchKeys.begin() + snap.LightTouchStart[index], snap.LightTouchKeys.begin() + snap.LightTouchStart[index + 1]);
			return;
		}
		for (side_t* side : light->touchlists.wall_tlist) out.push_back(uint32_t(side->Index()));
		for (FSection* section : light->touchlists.flat_tlist)
		{
			out.push_back(sides + uint32_t(section->Index()) * 2);
			out.push_back(sides + uint32_t(section->Index()) * 2 + 1);
		}
	};
	static FDynLightData pack;
	auto check = [&](uint32_t index, FDynamicLight* light, FDynamicLight* drawn)
	{
		if (!drawn->IsActive() || drawn->DontLightMap()) return;
		// [Karto] Not linked into the world yet (spawned this tic): it touches no list, and PosRelative would follow its
		// null sector. The native code only reaches lights through the link lists, so it never meets these.
		if (!drawn->Sector) return;
		pack.Clear();
		// [Karto] The published copy is what this frame draws: it carries this frame's shadowmap index (CollectLights
		// indexes the copies) and per-frame spotlight aim.
		AddLightToList(pack, drawn->Sector->PortalGroup, drawn, false);
		int array = 0;
		while (array < 3 && pack.arrays[array].Size() == 0) ++array;
		if (array == 3) return;
		auto& record = WorldModeLightRecords[light];
		const bool changed = record.Array != array || record.LinkSerial != drawn->LinkSerial ||
			memcmp(record.Pack, &pack.arrays[array][0], sizeof(record.Pack)) != 0;
		record.Seen = frame;
		if (!changed) return;
		if (r_worldmode_lightcensus) WorldModeLightCensusAdd(drawn, record, array, &pack.arrays[array][0]);
		const float* now = &pack.arrays[array][0];
		const bool relinked = record.Array != array || record.LinkSerial != drawn->LinkSerial;
		const float reach = WorldModeLightReach(drawn);
		const bool moved = memcmp(record.Pack, now, 3 * sizeof(float)) != 0 || reach != record.Radius;
		for (const auto& s : record.Slots) WriteWorldModeLightSlot(s.second, s.first, drawn, writes);
		if (relinked)
		{
			for (uint32_t key : record.Keys) WorldModeLightKeyDirty(key);
			touchKeys(index, light, record.Keys);
			for (uint32_t key : record.Keys) WorldModeLightKeyDirty(key);
		}
		else if (moved)
		{
			const DVector3 shift = drawn->Pos - record.Pos;
			FWorldModeLightTarget target;
			for (uint32_t key : record.Keys)
			{
				if (key < WorldModeKeyTiled.size() && WorldModeKeyTiled[key])
				{
					++WorldModeTiledDirty;
					WorldModeLightKeyDirty(key);
					continue;
				}
				if (!WorldModeLightTargetFor(level, key, target)) continue;
				++WorldModeFlipTests;
				const DVector3 rel = drawn->PosRelative(target.Group);
				if (WorldModeLightReaches(target, drawn->Pos, rel, reach) != WorldModeLightReaches(target, record.Pos, rel - shift, record.Radius))
				{
					WorldModeLightKeyDirty(key);
					++WorldModeFlipDirty;
				}
			}
		}
		record.Array = array;
		record.LinkSerial = drawn->LinkSerial;
		memcpy(record.Pack, now, sizeof(record.Pack));
		record.Pos = drawn->Pos;
		record.Radius = reach;
		++wm.DynLightChanges;
	};
	if (lit && fullScan)
	{
		if (snapped)
		{
			for (uint32_t i = 0; i < snap.Lights.size(); ++i) check(i, snap.LightLive[i], snap.Lights[i]);
		}
		else
		{
			for (auto light = level->lights; light; light = light->next) check(0, light, light);
		}
		++wm.LightFullScans;
		WorldModeLightCensusTics += bool(r_worldmode_lightcensus);
	}
	else if (lit && di->Viewpoint.camera)
	{
		if (snapped)
		{
			AActor* camera = WorldModeLiveActor(di->Viewpoint.camera);
			for (uint32_t i : snap.SpotLights)
				if (snap.LightOwner[i] == camera) check(i, snap.LightLive[i], snap.Lights[i]);
		}
		else
		{
			for (auto light : di->Viewpoint.camera->AttachedLights)
				if (light && light->IsSpot()) check(0, light, light);
		}
	}
	// [Karto] Only a full scan sees every light, so only it can tell which are gone.
	for (auto it = WorldModeLightRecords.begin(); fullScan && it != WorldModeLightRecords.end();)
	{
		if (it->second.Seen == frame) { ++it; continue; }
		for (uint32_t key : it->second.Keys) WorldModeLightKeyDirty(key);
		FreeWorldModeLightSlots(it->second);
		it = WorldModeLightRecords.erase(it);
	}
	const uint64_t rebuildStarted = I_nsTime();
	WorldModeScanNS += rebuildStarted - started;
	std::vector<float> data;
	for (uint32_t key : wm.DirtyKeys)
	{
		wm.KeyDirty[key] = 0;
		lightdata.Clear();
		FWorldModeLightTarget target;
		if (lit && WorldModeLightTargetFor(level, key, target))
		{
			auto add = [&](uint32_t index, FDynamicLight* live, FDynamicLight* light)
			{
				const DVector3 rel = light->PosRelative(target.Group);
				const float reach = WorldModeLightReach(light);
				if (!WorldModeLightReaches(target, light->Pos, rel, reach)) return;
				auto& record = WorldModeLightRecords[live];
				if (record.Array < 0)
				{
					pack.Clear();
					AddLightToList(pack, target.Group, light, false);
					int array = 0;
					while (array < 3 && pack.arrays[array].Size() == 0) ++array;
					if (array == 3)
					{
						WorldModeLightRecords.erase(live);
						return;
					}
					record.Array = array;
					memcpy(record.Pack, &pack.arrays[array][0], sizeof(record.Pack));
					record.LinkSerial = light->LinkSerial;
					record.Pos = light->Pos;
					record.Radius = reach;
					touchKeys(index, live, record.Keys);
				}
				const int32_t slot = WorldModeLightSlot(record, target.Group, light, writes);
				if (slot < 0) return;
				float* e = &lightdata.arrays[record.Array][lightdata.arrays[record.Array].Reserve(16)];
				memset(e, 0, 16 * sizeof(float));
				e[0] = float(rel.X); e[1] = float(rel.Z); e[2] = float(rel.Y); e[3] = reach; e[4] = float(slot);
			};
			if (snapped)
			{
				if (key + 1 < snap.KeyLightStart.size())
				{
					for (uint32_t t = snap.KeyLightStart[key]; t < snap.KeyLightStart[key + 1]; ++t)
					{
						const uint32_t i = snap.KeyLights[t];
						FDynamicLight* light = snap.Lights[i];
						if (light->IsActive() && !light->DontLightMap()) add(i, snap.LightLive[i], light);
					}
				}
			}
			else if (key < sides)
			{
				for (FDynamicLight* light : level->sides[key].dlist)
				{
					if (light->IsActive() && !light->DontLightMap()) add(0, light, light);
				}
			}
			else
			{
				for (FDynamicLight* light : level->sections.allSections[(key - sides) / 2].dlist)
				{
					if (light->IsActive() && !light->DontLightMap()) add(0, light, light);
				}
			}
		}
		const uint32_t size0 = lightdata.arrays[0].Size() / 16, size1 = lightdata.arrays[1].Size() / 16, size2 = lightdata.arrays[2].Size() / 16;
		const uint32_t total = size0 + size1 + size2;
		if (WorldModeKeyLights.size() != keys) WorldModeKeyLights.assign(keys, 0);
		WorldModeKeyLights[key] = uint16_t(std::min<uint32_t>(total, 65535));
		if (key < WorldModeKeyTiled.size()) WorldModeKeyTiled[key] = 0;
		if (total == 0)
		{
			if (wm.KeyOffset[key] >= 0)
			{
				static const float empty[4] = {};
				writes.Add(FWorldModeWrite::WorldLights, size_t(wm.KeyOffset[key]) * 16, empty, sizeof(empty));
			}
			continue;
		}
		static std::vector<float> tiled;
		const bool isTiled = tiles && key >= sides && total >= WorldModeLightTileMin && BuildWorldModeLightTiles(lightdata, tiled);
		if (isTiled) WorldModeKeyTiled[key] = 1;
		const uint32_t need = isTiled ? uint32_t(tiled.size() / 4) : total + 1;
		if (wm.KeyOffset[key] < 0 || need > wm.KeyCapacity[key])
		{
			uint32_t capacity = 8;
			while (capacity < need) capacity *= 2;
			if (wm.LightNext + capacity > WorldModeLightCapacity)
			{
				wm.LightCompact = true;
				++wm.LightOverflows;
				continue;
			}
			wm.KeyOffset[key] = int32_t(wm.LightNext);
			wm.KeyCapacity[key] = capacity;
			wm.LightNext += capacity;
			wm.SurfaceTableDirty = true;
			++WorldModeKeysPlaced;
		}
		if (isTiled)
		{
			const size_t slots = size_t(tiled[3]) * size_t(tiled[4]);
			for (size_t t = 0; t < slots; ++t)
				if (tiled[8 + t] >= 0.f) tiled[8 + t] += float(wm.KeyOffset[key]);
			writes.Add(FWorldModeWrite::WorldLights, size_t(wm.KeyOffset[key]) * 16, tiled.data(), tiled.size() * sizeof(float));
			WorldModeTiledBytes += tiled.size() * sizeof(float);
			++wm.LightListsBuilt;
			++wm.LightListsTiled;
			continue;
		}
		data.resize(size_t(need) * 4);
		data[0] = 0.f;
		data[1] = float(size0);
		data[2] = float(size0 + size1);
		data[3] = float(total);
		size_t at = 4;
		for (int a = 0; a < 3; ++a)
		{
			for (unsigned j = 0; j < lightdata.arrays[a].Size(); j += 16, at += 4)
			{
				data[at] = lightdata.arrays[a][j + 4];
				data[at + 1] = data[at + 2] = data[at + 3] = 0.f;
			}
		}
		writes.Add(FWorldModeWrite::WorldLights, size_t(wm.KeyOffset[key]) * 16, data.data(), data.size() * sizeof(float));
		WorldModePlainBytes += data.size() * sizeof(float);
		++wm.LightListsBuilt;
	}
	wm.DirtyKeys.clear();
	WorldModeRebuildNS += I_nsTime() - rebuildStarted;
	auto tableEntry = [&](size_t s)
	{
		const auto& info = wm.SurfaceInfo[s];
		size_t key = SIZE_MAX;
		if (info.DynKind == WorldModeDynWall) key = size_t(info.DynRef);
		else if (info.DynKind == WorldModeDynFloor) key = size_t(sides) + size_t(info.DynRef) * 2;
		else if (info.DynKind == WorldModeDynCeiling) key = size_t(sides) + size_t(info.DynRef) * 2 + 1;
		wm.SurfaceLight[s] = key < wm.KeyOffset.size() ? wm.KeyOffset[key] : -1;
	};
	if (wm.SurfaceLight.size() != surfaces || (wm.SurfaceTableDirty && reset))
	{
		++WorldModeTableFull;
		wm.SurfaceTableDirty = false;
		wm.SurfaceLight.resize(surfaces);
		for (size_t s = 0; s < surfaces; ++s) tableEntry(s);
		writes.Add(FWorldModeWrite::SurfaceLights, 0, wm.SurfaceLight.data(), surfaces * sizeof(int32_t));
		++wm.SurfaceTableWrites;
	}
	else if (wm.SurfaceTableDirty)
	{
		wm.SurfaceTableDirty = false;
		size_t runStart = SIZE_MAX, runEnd = 0;
		auto flush = [&]()
		{
			if (runStart == SIZE_MAX) return;
			writes.Add(FWorldModeWrite::SurfaceLights, runStart * sizeof(int32_t), &wm.SurfaceLight[runStart], (runEnd - runStart) * sizeof(int32_t));
			++WorldModeTableRanges;
			runStart = SIZE_MAX;
		};
		for (size_t s = 0; s < surfaces; ++s)
		{
			const int32_t was = wm.SurfaceLight[s];
			tableEntry(s);
			if (wm.SurfaceLight[s] == was) continue;
			if (runStart != SIZE_MAX && s > runEnd + 16) flush();
			if (runStart == SIZE_MAX) runStart = s;
			runEnd = s + 1;
		}
		flush();
		++WorldModeTableDiffs;
	}
	else
	{
		for (const auto& range : wm.SurfaceTableRanges)
		{
			const uint32_t end = std::min<uint32_t>(range.second, uint32_t(surfaces));
			if (range.first >= end) continue;
			for (uint32_t s = range.first; s < end; ++s) tableEntry(s);
			writes.Add(FWorldModeWrite::SurfaceLights, size_t(range.first) * sizeof(int32_t), &wm.SurfaceLight[range.first], size_t(end - range.first) * sizeof(int32_t));
			++WorldModeTableRanges;
		}
	}
	wm.SurfaceTableRanges.clear();
	wm.LightNS += I_nsTime() - started;
	++wm.LightFrames;
}

CCMD(worldmode_light_stats)
{
	auto& wm = WorldMode;
	const double n = wm.LightFrames ? double(wm.LightFrames) : 1.0;
	Printf("WORLDMODELIGHTS on=%d frames=%llu lists_per_frame=%.1f changes_per_frame=%.1f us_per_frame=%.1f table_writes=%llu "
		"overflows=%llu slots_used=%u\n", int(r_worldmode_lights), (unsigned long long)wm.LightFrames, wm.LightListsBuilt / n,
		wm.DynLightChanges / n, wm.LightNS / n / 1000.0, (unsigned long long)wm.SurfaceTableWrites, (unsigned long long)wm.LightOverflows,
		wm.LightNext);
	Printf("WORLDMODELIGHTS tiles=%d tiled_lists_per_frame=%.1f pertic=%d full_scans_per_frame=%.2f\n", int(r_worldmode_lighttiles),
		wm.LightListsTiled / n, int(r_worldmode_lightspertic), wm.LightFullScans / n);
	Printf("WORLDMODELIGHTS slot_writes=%llu flip_tests=%llu flip_dirty=%llu records=%zu free_slots=%zu\n", (unsigned long long)WorldModeSlotWrites,
		(unsigned long long)WorldModeFlipTests, (unsigned long long)WorldModeFlipDirty, WorldModeLightRecords.size(), WorldModeFreeLightSlots.size());
	Printf("WORLDMODELIGHTS bytes slots=%llu plain=%llu tiled=%llu tiled_dirty=%llu keys_placed=%llu table_ranges=%llu table_full=%llu table_diffs=%llu "
		"table_bytes=%zu scan_ms=%.1f rebuild_ms=%.1f\n", (unsigned long long)WorldModeSlotWrites * 64, (unsigned long long)WorldModePlainBytes, (unsigned long long)WorldModeTiledBytes,
		(unsigned long long)WorldModeTiledDirty, (unsigned long long)WorldModeKeysPlaced, (unsigned long long)WorldModeTableRanges,
		(unsigned long long)WorldModeTableFull, (unsigned long long)WorldModeTableDiffs, wm.SurfaceInfo.size() * sizeof(int32_t), WorldModeScanNS / 1e6, WorldModeRebuildNS / 1e6);
	PrintWorldModeListLengths();
	if (argv.argc() == 2 && !stricmp(argv[1], "reset")) wm.LightFrames = wm.LightListsBuilt = wm.LightListsTiled = wm.LightFullScans = wm.LightNS = wm.DynLightChanges = 0;
}

void HWDrawInfo::UpdateWorldMode(bool publish)
{
	auto& wm = WorldMode;
	if (!wm.Ready || wm.SectorSnap.size() != Level->sectors.Size()) return;
	const uint64_t started = I_nsTime();
	wm.LastDirtySectors = 0;
	FWorldModeSectorSnap now;
	auto& state = *screen->RenderState();
	FWorldModeWriteList writes;
	auto check = [&](sector_t& sector, bool hot)
	{
		auto& was = wm.SectorSnap[sector.Index()];
		WorldModeSectorSnap(sector, now);
		const bool geometry = memcmp(&now.Geometry, &was.Geometry, sizeof(now.Geometry)) != 0;
		const bool flat = memcmp(&now.Flat, &was.Flat, sizeof(now.Flat)) != 0;
		const bool light = memcmp(&now.Light, &was.Light, sizeof(now.Light)) != 0;
		if (!geometry && !flat && !light) return;
		const auto& ng = now.Geometry;
		const auto& wg = was.Geometry;
		const bool planesMoved = geometry && memcmp(ng.Planes, wg.Planes, sizeof(ng.Planes)) != 0;
		if (planesMoved && r_worldmode_lights) WorldModeLightSectorMoved(Level, sector);
		if (planesMoved && wm.PlanesMoved) wm.PlanesMoved[sector.Index()].store(1, std::memory_order_relaxed);
		const bool floorMoved = geometry && (memcmp(&ng.Planes[0], &wg.Planes[0], sizeof(secplane_t)) != 0 || ng.TexZ[0] != wg.TexZ[0]);
		const bool ceilingMoved = geometry && (memcmp(&ng.Planes[1], &wg.Planes[1], sizeof(secplane_t)) != 0 || ng.TexZ[1] != wg.TexZ[1]);
		const bool heightOnly = geometry && memcmp(ng.Texture, wg.Texture, sizeof(ng.Texture)) == 0 &&
			memcmp(ng.PlaneFlags, wg.PlaneFlags, sizeof(ng.PlaneFlags)) == 0 && memcmp(ng.GlowColor, wg.GlowColor, sizeof(ng.GlowColor)) == 0 &&
			memcmp(ng.GlowHeight, wg.GlowHeight, sizeof(ng.GlowHeight)) == 0 && ng.FadeColor == wg.FadeColor && ng.Flags == wg.Flags &&
			ng.MoreFlags == wg.MoreFlags && memcmp(ng.Maps, wg.Maps, sizeof(ng.Maps)) == 0 && ng.Heightsec == wg.Heightsec &&
			ng.Transdoor == wg.Transdoor;
		// [Karto] A neighbour's fog boundary tests this sector's light against 248 (CheckFog), so crossing it is geometry.
		const bool crossing = light && (now.Light.LightLevel >= 248) != (was.Light.LightLevel >= 248);
		wm.GeometryChanges += geometry;
		if (geometry)
		{
			auto& a = now.Geometry;
			auto& b = was.Geometry;
			const bool fields[11] = { memcmp(a.Planes, b.Planes, sizeof(a.Planes)) != 0, memcmp(a.TexZ, b.TexZ, sizeof(a.TexZ)) != 0,
				memcmp(a.Texture, b.Texture, sizeof(a.Texture)) != 0, memcmp(a.PlaneFlags, b.PlaneFlags, sizeof(a.PlaneFlags)) != 0,
				memcmp(a.GlowColor, b.GlowColor, sizeof(a.GlowColor)) != 0 || memcmp(a.GlowHeight, b.GlowHeight, sizeof(a.GlowHeight)) != 0,
				a.FadeColor != b.FadeColor, a.Flags != b.Flags, a.MoreFlags != b.MoreFlags, memcmp(a.Maps, b.Maps, sizeof(a.Maps)) != 0,
				a.Heightsec != b.Heightsec, a.Transdoor != b.Transdoor };
			for (int i = 0; i < 11; ++i) wm.GeometryFields[i] += fields[i];
		}
		wm.FlatChanges += flat;
		wm.LightChanges += light;
		wm.LightCrossings += crossing;
		wm.HotChanges += hot;
		const bool lightOnly = light && !flat && !geometry && !crossing &&
			memcmp(now.Light.Special, was.Light.Special, sizeof(now.Light.Special)) == 0 &&
			memcmp(now.Light.Additive, was.Light.Additive, sizeof(now.Light.Additive)) == 0 &&
			memcmp(&now.Light.Colormap, &was.Light.Colormap, sizeof(now.Light.Colormap)) == 0;
		const bool xformOnly = flat && memcmp(now.Flat.Xform, was.Flat.Xform, sizeof(now.Flat.Xform)) != 0 &&
			memcmp(now.Flat.Alpha, was.Flat.Alpha, sizeof(now.Flat.Alpha)) == 0 &&
			memcmp(now.Flat.Reflect, was.Flat.Reflect, sizeof(now.Flat.Reflect)) == 0 &&
			memcmp(now.Flat.TextureFx, was.Flat.TextureFx, sizeof(now.Flat.TextureFx)) == 0;
		was = now;
		if (planesMoved)
		{
			if (publish)
			{
				WriteWorldModePublishedPlane(sector, sector_t::floor, writes);
				WriteWorldModePublishedPlane(sector, sector_t::ceiling, writes);
			}
			else WriteWorldModePlaneHeights(sector, writes);
		}
		const size_t planeSlot = size_t(sector.Index()) * 2;
		if (r_worldmode_planeheights && heightOnly && !flat && !light && wm.PlaneState[planeSlot] != FWorldModeTier::Producer &&
			wm.PlaneState[planeSlot + 1] != FWorldModeTier::Producer)
		{
			if (r_worldmode_wallheights) MarkWorldModeMovedSides(Level, sector, floorMoved, ceilingMoved);
			else MarkWorldModeSector(Level, sector, true, false);
			GrowWorldModeSectorBounds(sector);
			++wm.HeightOnlySectors;
		}
		else if (geometry || crossing) MarkWorldModeSector(Level, sector, true);
		else if (light)
		{
			if (!(lightOnly && WriteWorldModeLight(this, state, sector, writes))) MarkWorldModeSector(Level, sector, false);
		}
		else if (r_worldmode_params && xformOnly && WriteWorldModePlaneRecords(sector, writes)) ++wm.PlaneSectors;
		else
		{
			wm.PlaneFallbacks += xformOnly;
			MarkWorldModeUnit(uint32_t(sector.Index()), false);
			++wm.LastDirtySectors;
		}
	};
	if (publish)
	{
		HandOverWorldModeSideChanges();
		const uint32_t count = Level->sectors.Size();
		if (wm.PlaneBlending.size() != size_t(count) * 2)
		{
			wm.PlaneBlending.assign(size_t(count) * 2, 0);
			wm.BlendingPlanes.clear();
		}
		wm.HotSectors.clear();
		for (uint32_t s = 0; s < count; ++s)
		{
			auto& sector = Level->sectors[s];
			check(sector, false);
			bool hot = false;
			for (auto& interpolation : sector.interpolations)
				if (interpolation.ForceGet()) { hot = true; break; }
			wm.HotFlag[s] = hot;
			if (hot) wm.HotSectors.push_back(int(s));
		}
		size_t kept = 0;
		for (int slot : wm.BlendingPlanes)
		{
			if (!wm.PlaneBlending[slot]) continue;
			double d, texz;
			auto& sector = Level->sectors[slot / 2];
			float rec[WorldModeHeightFloats];
			WorldModePlaneHeight(sector, slot & 1, rec);
			const bool still = R_PlaneInterpolationStart(&sector, (slot & 1) != 0, d, texz) && (float(d) != rec[3] || float(texz) != rec[8]);
			if (still)
			{
				wm.BlendingPlanes[kept++] = slot;
				continue;
			}
			writes.Add(FWorldModeWrite::PlaneHeights, size_t(slot) * WorldModeHeightFloats * sizeof(float), rec, sizeof(rec));
			wm.PlaneBlending[slot] = 0;
		}
		wm.BlendingPlanes.resize(kept);
		std::vector<uint32_t> blended;
		for (const auto& b : wm.ScrollBlends) blended.push_back(b.Slot);
		wm.ScrollBlends.clear();
		if (r_worldmode_params)
		{
			for (int s : wm.HotSectors)
			{
				auto& sector = Level->sectors[s];
				const uint32_t slot = uint32_t(s) * 2;
				if (wm.PlaneState[slot] == FWorldModeTier::Producer || wm.PlaneState[slot + 1] == FWorldModeTier::Producer) continue;
				for (int which = 0; which < 2; ++which)
				{
					double x, y;
					if (wm.PlaneState[slot + which] != FWorldModeTier::Param ||
						!R_ScrollInterpolationStart(&sector, which == sector_t::ceiling, x, y)) continue;
					decltype(wm.ScrollBlends)::value_type b;
					b.Slot = slot + which;
					WorldModePlaneMatrix(sector, which, b.Cur);
					WorldModePlaneMatrixAt(sector, which, x, y, b.Prev);
					if (memcmp(b.Prev, b.Cur, sizeof(b.Cur)) != 0) wm.ScrollBlends.push_back(b);
				}
			}
		}
		wm.ScrollBlendsPublished += wm.ScrollBlends.size();
		for (const auto& b : wm.ScrollBlends)
		{
			float pair[32];
			memcpy(pair, b.Cur, sizeof(b.Cur));
			memcpy(pair + 16, b.Prev, sizeof(b.Prev));
			writes.Add(FWorldModeWrite::PlaneMatrices, size_t(b.Slot) * sizeof(pair), pair, sizeof(pair));
			wm.PlaneShifted[b.Slot] = 1;
			++wm.PlaneWrites;
		}
		for (uint32_t slot : blended)
		{
			bool still = false;
			for (const auto& b : wm.ScrollBlends) still |= b.Slot == slot;
			if (still) continue;
			float matrix[32];
			WorldModePlaneMatrixPair(Level->sectors[slot / 2], int(slot & 1), matrix);
			writes.Add(FWorldModeWrite::PlaneMatrices, size_t(slot) * sizeof(matrix), matrix, sizeof(matrix));
			wm.PlaneShifted[slot] = 1;
		}
		wm.PublishedLevel = Level;
		wm.PublishedStartTime = Level->starttime;
		wm.PublishedMapTime = Level->maptime;
		++wm.Publishes;
		wm.ScanNS += I_nsTime() - started;
		writes.Submit(state);
		wm.PublishNS += I_nsTime() - started;
		return;
	}
	const bool published = WorldModePublishing() && wm.PublishedLevel == Level && wm.PublishedStartTime == Level->starttime &&
		wm.PublishedMapTime == WorldModeFrameTic(Level);
	if (!published)
	{
		HandOverWorldModeSideChanges();
		// [Karto] Thinkers change sectors at most once per tic, so each frame checks the share of sectors that matches the
		// time since the previous frame: the map is covered once per tic without one large frame per tic. A change waits
		// at most one tic to be seen.
		const uint32_t count = Level->sectors.Size();
		const double share = wm.LastUpdateNS ? std::min(1.0, double(started - wm.LastUpdateNS) * TICRATE / 1e9) : 1.0;
		uint32_t todo = std::min(count, uint32_t(ceil(share * count)));
		wm.LastUpdateNS = started;
		wm.ScannedSectors += todo;
		++wm.ScanFrames;
		for (; todo; --todo)
		{
			auto& sector = Level->sectors[wm.ScanCursor];
			check(sector, false);
			bool hot = false;
			for (auto& interpolation : sector.interpolations)
			{
				if (interpolation.ForceGet())
				{
					hot = true;
					break;
				}
			}
			if (hot && !wm.HotFlag[wm.ScanCursor]) wm.HotSectors.push_back(int(wm.ScanCursor));
			wm.HotFlag[wm.ScanCursor] = hot;
			if (++wm.ScanCursor == count)
			{
				wm.ScanCursor = 0;
				++wm.Scans;
				wm.HotSectors.clear();
				for (uint32_t i = 0; i < count; ++i)
					if (wm.HotFlag[i]) wm.HotSectors.push_back(int(i));
			}
		}
		const uint64_t hotStarted = I_nsTime();
		wm.ScanNS += hotStarted - started;
		for (int index : wm.HotSectors)
		{
			if (!wm.HotFlag[index]) continue;
			auto& sector = Level->sectors[index];
			if (!WorldModeHotSeen(this, sector))
			{
				++wm.HotDeferred;
				continue;
			}
			++wm.HotChecks;
			check(sector, true);
		}
		wm.HotNS += I_nsTime() - hotStarted;
	}
	const uint32_t sectors = Level->sectors.Size();
	const uint64_t sidesStarted = I_nsTime();
	{
		const float frac[4] = { float(Level->interpolator.frac), 0.f, 0.f, 0.f };
		writes.Add(FWorldModeWrite::Params, 0, frac, sizeof(frac));
	}
	// [Karto] Side records blend relative to the tic frames draw (WorldModeFrameTic), which play may be ahead of.
	const int frameTic = WorldModeFrameTic(Level);
	if (frameTic != wm.BlendTic)
	{
		for (int side : wm.BlendSides)
		{
			if (wm.SideBlendTic[side] == frameTic) continue;
			wm.SideKinds[side] |= uint16_t(side_t::RenderOffsetTop | side_t::RenderOffsetMid | side_t::RenderOffsetBottom);
			if (!wm.SideDirty[side])
			{
				wm.SideDirty[side] = 1;
				wm.DirtySides.push_back(side);
			}
		}
		wm.BlendSides.clear();
		wm.BlendTic = frameTic;
	}
	wm.SideChanges += wm.DirtySides.size();
	for (int side : wm.DirtySides)
	{
		const unsigned kinds = wm.SideKinds[side];
		wm.SideKinds[side] = 0;
		const auto& sd = Level->sides[side];
		const bool twoSided = sd.linedef && sd.linedef->sidedef[1];
		if (kinds & ~unsigned(side_t::RenderOffsetTop | side_t::RenderOffsetMid | side_t::RenderOffsetBottom)) ++wm.StructuralSides;
		else if ((kinds & side_t::RenderOffsetMid) && twoSided) ++wm.OffsetMid2sSides;
		else ++wm.OffsetOnlySides;
		wm.SideDirty[side] = 0;
		const bool offsetOnly = !(kinds & ~unsigned(side_t::RenderOffsetTop | side_t::RenderOffsetMid | side_t::RenderOffsetBottom));
		if (r_worldmode_params && offsetOnly)
		{
			if (r_worldmode_hotgate && !WorldModeUnitSeen(this, sectors + uint32_t(side)))
			{
				wm.SideRecordKinds[side] |= uint8_t(kinds);
				wm.SideTestedFrame[side] = wm.UpdateFrames;
				if (!wm.SidePending[side])
				{
					wm.SidePending[side] = 1;
					wm.PendingSides.push_back(side);
				}
				++wm.RecordsDeferred;
				continue;
			}
			const unsigned all = kinds | wm.SideRecordKinds[side];
			wm.SideRecordKinds[side] = 0;
			if (WriteWorldModeSideParams(sd, uint32_t(side), all, frameTic, writes))
			{
				++wm.ParamSides;
				continue;
			}
		}
		++wm.ParamFallbacks;
		wm.SideRecordKinds[side] = 0;
		MarkWorldModeUnit(sectors + uint32_t(side), true);
	}
	wm.DirtySides.clear();
	{
		size_t kept = 0;
		for (int side : wm.PendingSides)
		{
			const unsigned kinds = wm.SideRecordKinds[side];
			if (!kinds)
			{
				wm.SidePending[side] = 0;
				continue;
			}
			if (wm.SideTestedFrame[side] == wm.UpdateFrames || (r_worldmode_hotgate && !WorldModeUnitSeen(this, sectors + uint32_t(side))))
			{
				wm.PendingSides[kept++] = side;
				continue;
			}
			wm.SideRecordKinds[side] = 0;
			wm.SidePending[side] = 0;
			++wm.RecordsReleased;
			if (!WriteWorldModeSideParams(Level->sides[side], uint32_t(side), kinds, frameTic, writes)) MarkWorldModeUnit(sectors + uint32_t(side), true);
		}
		wm.PendingSides.resize(kept);
	}

	const uint64_t animStarted = I_nsTime();
	wm.SidesNS += animStarted - sidesStarted;
	for (auto& anim : wm.StaticAnim) PatchWorldModeAnimation(state, anim, writes);
	for (auto& slot : wm.Slots)
		for (auto& anim : slot.Anim) PatchWorldModeAnimation(state, anim, writes);
	wm.AnimNS += I_nsTime() - animStarted;
	const uint64_t selectStarted = I_nsTime();
	SelectWorldModeUnits(this);
	wm.SelectNS += I_nsTime() - selectStarted;
	wm.LastDirtyUnits = uint32_t(wm.DirtyUnits.size());
	if (!wm.DirtyUnits.empty())
	{
		// [Karto] Re-production borrows this view's draw lists; anything already queued would be swept away with it.
		if (WorldModeDrawListsEmpty(this)) ReproduceWorldModeUnits(this, state, writes);
		else ++wm.Skipped;
	}
	WriteWorldModeFlash(this, state, writes);
	if (wm.PoolBoundsDirty)
	{
		wm.PoolBoundsDirty = false;
		RebuildWorldModePoolClusters(state);
	}
	UpdateWorldModeLights(this, writes);
	wm.WriteBytes += writes.Arena.size();
	for (const auto& write : writes.Writes) WorldModeTargetBytes[write.Target] += write.Bytes;
	const uint64_t submitStarted = I_nsTime();
	wm.WriteCount += writes.Writes.size();
	writes.Submit(state);
	wm.SubmitNS += I_nsTime() - submitStarted;
	const uint64_t updateNS = I_nsTime() - started;
	wm.UpdateNS += updateNS;
	if (updateNS >= 1000000) ++WorldModeSlowUpdates, WorldModeSlowUpdateNS += updateNS;
	++wm.UpdateFrames;
}

CCMD(worldmode_change_stats)
{
	auto& wm = WorldMode;
	auto avg = [](uint64_t ns, uint64_t n) { return n ? double(ns) / n / 1000.0 : 0.0; };
	Printf("WORLDMODESCAN scan_frame_us=%.3f sectors_per_frame=%.1f passes=%llu hot_checks=%llu hot_deferred=%llu hotgate=%d "
		"units_deferred=%llu units_released=%llu deferred_now=%zu defer_flushes=%llu sides_us=%.3f anim_us=%.3f submit_us=%.3f writes_per_frame=%.1f hot_us=%.3f select_us=%.3f records_deferred=%llu records_released=%llu pending_now=%zu\n",
		avg(wm.ScanNS, wm.ScanFrames), wm.ScanFrames ? double(wm.ScannedSectors) / wm.ScanFrames : 0.0, (unsigned long long)wm.Scans,
		(unsigned long long)wm.HotChecks, (unsigned long long)wm.HotDeferred, int(r_worldmode_hotgate),
		(unsigned long long)wm.UnitsDeferred, (unsigned long long)wm.UnitsReleased, wm.DeferredUnits.size(), (unsigned long long)wm.DeferFlushes,
		avg(wm.SidesNS, wm.UpdateFrames), avg(wm.AnimNS, wm.UpdateFrames), avg(wm.SubmitNS, wm.UpdateFrames),
		wm.UpdateFrames ? double(wm.WriteCount) / wm.UpdateFrames : 0.0, avg(wm.HotNS, wm.UpdateFrames), avg(wm.SelectNS, wm.UpdateFrames),
		(unsigned long long)wm.RecordsDeferred, (unsigned long long)wm.RecordsReleased, wm.PendingSides.size());
	Printf("WORLDMODECHANGE update_us_avg=%.3f update_frames=%llu scan_us_avg=%.3f scans=%llu reproduce_us_avg=%.3f reproduce_frames=%llu "
		"reproduced_units=%llu last_dirty_sectors=%u last_dirty_units=%u hot_sectors=%zu slots=%zu pool_surfaces=%u/%u pool_vertices=%u/%u "
		"pool_indices=%u/%u anim_rows=%zu anim_patches=%llu anim_failures=%llu write_bytes=%llu compactions=%llu overflows=%llu skipped=%llu "
		"geometry_changes=%llu flat_changes=%llu light_changes=%llu light_crossings=%llu hot_changes=%llu produce_us_avg=%.3f "
		"row_only_units=%llu in_place_units=%llu\n",
		avg(wm.UpdateNS, wm.UpdateFrames), (unsigned long long)wm.UpdateFrames, avg(wm.ScanNS, wm.Scans), (unsigned long long)wm.Scans,
		avg(wm.ReproduceNS, wm.ReproduceFrames), (unsigned long long)wm.ReproduceFrames, (unsigned long long)wm.ReproducedUnits,
		wm.LastDirtySectors, wm.LastDirtyUnits, wm.HotSectors.size(), wm.Slots.size(), wm.PoolSurfaceNext, wm.PoolSurfaceCap,
		wm.PoolVertexNext, wm.PoolVertexCap, wm.PoolIndexNext, wm.PoolIndexCap, wm.StaticAnim.size(),
		(unsigned long long)wm.AnimPatches, (unsigned long long)wm.AnimFailures, (unsigned long long)wm.WriteBytes,
		(unsigned long long)wm.Compactions, (unsigned long long)wm.Overflows, (unsigned long long)wm.Skipped,
		(unsigned long long)wm.GeometryChanges, (unsigned long long)wm.FlatChanges, (unsigned long long)wm.LightChanges,
		(unsigned long long)wm.LightCrossings, (unsigned long long)wm.HotChanges, avg(wm.ProduceNS, wm.ReproduceFrames),
		(unsigned long long)wm.RowOnlyUnits, (unsigned long long)wm.InPlaceUnits);
	const double frames = double(std::max<uint64_t>(wm.ReproduceFrames, 1)) * 1000.0;
	Printf("WORLDMODEWRITES vertices=%llu surface_ids=%llu indices=%llu rows=%llu surface_params=%llu params=%llu plane_matrices=%llu "
		"plane_heights=%llu vertex_refs=%llu surface_lights=%llu world_lights=%llu slow_updates=%llu slow_update_ms_avg=%.3f\n",
		(unsigned long long)WorldModeTargetBytes[0], (unsigned long long)WorldModeTargetBytes[1], (unsigned long long)WorldModeTargetBytes[2],
		(unsigned long long)WorldModeTargetBytes[3], (unsigned long long)WorldModeTargetBytes[4], (unsigned long long)WorldModeTargetBytes[5],
		(unsigned long long)WorldModeTargetBytes[6], (unsigned long long)WorldModeTargetBytes[7], (unsigned long long)WorldModeTargetBytes[8],
		(unsigned long long)WorldModeTargetBytes[9], (unsigned long long)WorldModeTargetBytes[10], (unsigned long long)WorldModeSlowUpdates,
		WorldModeSlowUpdates ? WorldModeSlowUpdateNS / 1e6 / WorldModeSlowUpdates : 0.0);
	Printf("WORLDMODEPARAMS enabled=%d sides=%llu writes=%llu fallbacks=%llu plane_sectors=%llu plane_writes=%llu plane_fallbacks=%llu\n",
		int(r_worldmode_params), (unsigned long long)wm.ParamSides, (unsigned long long)wm.ParamWrites, (unsigned long long)wm.ParamFallbacks,
		(unsigned long long)wm.PlaneSectors, (unsigned long long)wm.PlaneWrites, (unsigned long long)wm.PlaneFallbacks);
	Printf("WORLDMODESKY sides=%zu frames=%llu replayed_per_frame=%.1f culled_per_frame=%.1f us_per_frame=%.1f\n", wm.SkySides.size(),
		(unsigned long long)wm.SkyFrames, wm.SkyFrames ? double(wm.SkyReplayed) / wm.SkyFrames : 0.0,
		wm.SkyFrames ? double(wm.SkyCulled) / wm.SkyFrames : 0.0, wm.SkyFrames ? double(wm.SkyNS) / wm.SkyFrames / 1000.0 : 0.0);
	Printf("WORLDMODELIGHT sectors=%llu row_writes=%llu fallbacks=%llu verified=%llu rejected=%llu flash_rows=%zu flash_rejected=%llu\n",
		(unsigned long long)wm.LightSectors, (unsigned long long)wm.LightRowWrites, (unsigned long long)wm.LightFallbacks,
		(unsigned long long)wm.LightVerified, (unsigned long long)wm.LightRejected, wm.FlashRows.size(), (unsigned long long)wm.FlashRejected);
	Printf("WORLDMODEPLANES hack=%llu skybox=%llu heightsec=%llu xfloor=%llu mismatch=%llu height_only_sectors=%llu "
		"wall_sides_kept=%llu wall_sides_reproduced=%llu line_crossings=%llu\n", (unsigned long long)wm.PlaneReasons[0],
		(unsigned long long)wm.PlaneReasons[1], (unsigned long long)wm.PlaneReasons[2], (unsigned long long)wm.PlaneReasons[3],
		(unsigned long long)wm.PlaneReasons[4], (unsigned long long)wm.HeightOnlySectors, (unsigned long long)wm.WallSidesKept,
		(unsigned long long)wm.WallSidesReproduced, (unsigned long long)wm.LineCrossings);
	Printf("WORLDMODEPUBLISH publishes=%llu us_per_publish=%.1f plane_writes=%llu blending_planes=%zu scroll_blends=%llu\n",
		(unsigned long long)wm.Publishes, wm.Publishes ? double(wm.PublishNS) / wm.Publishes / 1000.0 : 0.0,
		(unsigned long long)wm.PublishedPlaneWrites, wm.BlendingPlanes.size(), (unsigned long long)wm.ScrollBlendsPublished);
	Printf("WORLDMODECLAMPGUARD guarded_sides=%zu checks=%llu fires=%llu\n", size_t(std::count_if(wm.SideGuards.begin(), wm.SideGuards.end(),
		[](const auto& g) { return !g.empty(); })), (unsigned long long)wm.ClampGuardChecks, (unsigned long long)wm.ClampGuardFires);
	Printf("WORLDMODEUNCOVERED lost=%llu unarmed=%llu nonquad=%llu mid2s=%llu clampy=%llu glow=%llu lightsplit=%llu ffblock=%llu sky=%llu\n",
		(unsigned long long)wm.UncoveredReproduced[0], (unsigned long long)wm.UncoveredReproduced[1], (unsigned long long)wm.UncoveredReproduced[2],
		(unsigned long long)wm.UncoveredReproduced[3], (unsigned long long)wm.UncoveredReproduced[4], (unsigned long long)wm.UncoveredReproduced[5],
		(unsigned long long)wm.UncoveredReproduced[6], (unsigned long long)wm.UncoveredReproduced[7], (unsigned long long)wm.UncoveredReproduced[8]);
	Printf("WORLDMODEPRODUCE wall_process_us=%.1f wall_draw_us=%.1f flat_process_us=%.1f flat_draw_us=%.1f vertex_update_us=%.1f\n",
		wm.WallProcessNS / frames, wm.WallDrawNS / frames, wm.FlatProcessNS / frames, wm.FlatDrawNS / frames, wm.VertexUpdateNS / frames);
	Printf("WORLDMODESIDES offset_only=%llu offset_mid2s=%llu structural=%llu calls offset_top=%llu offset_mid=%llu offset_bottom=%llu "
		"scale=%llu texture=%llu light=%llu alpha=%llu flags=%llu color=%llu fx=%llu init=%llu\n",
		(unsigned long long)wm.OffsetOnlySides, (unsigned long long)wm.OffsetMid2sSides, (unsigned long long)wm.StructuralSides,
		(unsigned long long)wm.SideKindCalls[0], (unsigned long long)wm.SideKindCalls[1], (unsigned long long)wm.SideKindCalls[2],
		(unsigned long long)wm.SideKindCalls[3], (unsigned long long)wm.SideKindCalls[4], (unsigned long long)wm.SideKindCalls[5],
		(unsigned long long)wm.SideKindCalls[6], (unsigned long long)wm.SideKindCalls[7], (unsigned long long)wm.SideKindCalls[8],
		(unsigned long long)wm.SideKindCalls[9], (unsigned long long)wm.SideKindCalls[10]);
	Printf("WORLDMODEGEOMETRY planes=%llu texz=%llu texture=%llu planeflags=%llu glow=%llu fade=%llu flags=%llu moreflags=%llu maps=%llu "
		"heightsec=%llu transdoor=%llu side_changes=%llu\n", (unsigned long long)wm.GeometryFields[0], (unsigned long long)wm.GeometryFields[1],
		(unsigned long long)wm.GeometryFields[2], (unsigned long long)wm.GeometryFields[3], (unsigned long long)wm.GeometryFields[4],
		(unsigned long long)wm.GeometryFields[5], (unsigned long long)wm.GeometryFields[6], (unsigned long long)wm.GeometryFields[7],
		(unsigned long long)wm.GeometryFields[8], (unsigned long long)wm.GeometryFields[9], (unsigned long long)wm.GeometryFields[10], (unsigned long long)wm.SideChanges);
}

CCMD(worldmode_stats)
{
	Printf("WORLDMODESTATS enabled=%d ready=%d bake_ms=%llu surfaces=%llu rows=%llu clusters=%u opaque_clusters=%u visible_clusters=%u "
		"vertices=%llu indices=%llu cull_us_avg=%.3f cull_frames=%llu occlusion_frames=%llu occluded_per_frame=%.1f occlusion_stale=%llu\n", int(r_worldmode), int(WorldMode.Ready),
		(unsigned long long)WorldMode.BakeMS, (unsigned long long)WorldMode.Surfaces, (unsigned long long)WorldMode.Rows,
		WorldMode.Clusters, WorldMode.OpaqueClusters, WorldMode.LastVisible, (unsigned long long)WorldMode.Vertices,
		(unsigned long long)WorldMode.IndexCount, WorldMode.CullFrames ? double(WorldMode.CullNS) / WorldMode.CullFrames / 1000.0 : 0.0,
		(unsigned long long)WorldMode.CullFrames, (unsigned long long)WorldMode.OcclusionFrames,
		WorldMode.OcclusionFrames ? double(WorldMode.OccludedClusters) / WorldMode.OcclusionFrames : 0.0,
		(unsigned long long)WorldMode.OcclusionStale);
	Printf("WORLDMODESHADOW enabled=%d reports=%llu seen_surfaces=%llu missed_surfaces=%llu reports_with_misses=%llu max_missed=%llu\n",
		int(r_worldmode_occlusion_shadow), (unsigned long long)WorldMode.ShadowReports, (unsigned long long)WorldMode.ShadowSeenSurfaces,
		(unsigned long long)WorldMode.ShadowMissSurfaces, (unsigned long long)WorldMode.ShadowMissReports, (unsigned long long)WorldMode.ShadowMaxMiss);
	Printf("WORLDMODESPRITES sideeffect_interval=%d sideeffect_passes=%llu sprite_sectors=%u sprite_us_avg=%.3f sprite_frames=%llu\n",
		int(r_worldmode_sideeffects), (unsigned long long)WorldMode.SideEffectPasses, WorldMode.LastSpriteSectors,
		WorldMode.SpriteFrames ? double(WorldMode.SpriteNS) / WorldMode.SpriteFrames / 1000.0 : 0.0, (unsigned long long)WorldMode.SpriteFrames);
	Printf("WORLDMODEVIS gpuvis=%d stamp=%u visible_surfaces=%u visible_sectors=%u scan_us_avg=%.3f scan_frames=%llu flag_writes=%llu rebakes=%llu\n",
		int(r_worldmode_gpuvis), WorldMode.VisibleStamp, WorldMode.LastVisibleSurfaces, WorldMode.LastVisibleSectors,
		WorldMode.VisibilityFrames ? double(WorldMode.VisibilityNS) / WorldMode.VisibilityFrames / 1000.0 : 0.0,
		(unsigned long long)WorldMode.VisibilityFrames, (unsigned long long)WorldMode.FlagWrites, (unsigned long long)WorldModeRebake.Total);
	C_DoCommand("worldmode_walker_stats");
	C_DoCommand("worldmode_backend_stats");
	C_DoCommand("worldmode_change_stats");
}
