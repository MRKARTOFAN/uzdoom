/*
** hw_drawinfo.h
**
** Basic scene draw info management class
**
**---------------------------------------------------------------------------
**
** Copyright 2000-2018 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#pragma once

#include <atomic>
#include <functional>
#include "vectors.h"
#include "r_defs.h"
#include "r_utility.h"
#include "hw_viewpointuniforms.h"
#include "v_video.h"
#include "hw_weapon.h"
#include "hw_drawlist.h"

bool hw_IsLineDistanceCulled(const line_t* line, const DVector2& viewpoint, double cullDistanceSquared);

enum EDrawMode
{
	DM_MAINVIEW,
	DM_OFFSCREEN,
	DM_PORTAL,
	DM_SKYPORTAL
};

struct FSectorPortalGroup;
struct FLinePortalSpan;
struct FFlatVertex;
class HWWall;
class HWFlat;
class HWSprite;
struct HWDecal;
class IShadowMap;
struct particle_t;
struct FDynLightData;
struct HUDSprite;
class ACorona;
class Clipper;
class HWPortal;
class FFlatVertexBuffer;
class IRenderQueue;
class HWScenePortalBase;
class FRenderState;

//==========================================================================
//
// these are used to link faked planes due to missing textures to a sector
//
//==========================================================================
struct gl_subsectorrendernode
{
	gl_subsectorrendernode *	next;
	subsector_t *				sub;
	int							lightindex;
	int							vertexindex;
};

struct gl_floodrendernode
{
	gl_floodrendernode * next;
	seg_t *seg;
	int vertexindex;
	// This will use the light list of the originating sector.
};

enum area_t : int;

enum SectorRenderFlags
{
	// This is used to merge several subsectors into a single draw item
	SSRF_RENDERFLOOR = 1,
	SSRF_RENDERCEILING = 2,
	SSRF_RENDER3DPLANES = 4,
	SSRF_RENDERALL = 7,
	SSRF_PROCESSED = 8,
	SSRF_SEEN = 16,
	SSRF_PLANEHACK = 32,
	SSRF_FLOODHACK = 64
};

enum EPortalClip
{
	PClip_InFront,
	PClip_Inside,
	PClip_Behind,
};

enum DrawListType
{
	GLDL_PLAINWALLS,
	GLDL_PLAINFLATS,
	GLDL_MASKEDWALLS,
	GLDL_MASKEDFLATS,
	GLDL_MASKEDWALLSOFS,
	GLDL_MODELS,

	GLDL_TRANSLUCENT,
	GLDL_TRANSLUCENTBORDER,
	GLDL_SOLIDSPRITES,
	GLDL_ADDSPRITES,
	GLDL_ALPHASPRITES,
	GLDL_PARTICLES,
	GLDL_SOLIDPARTICLES,
	GLDL_ADDPARTICLES,

	GLDL_TYPES,
};


bool WorldModeActive(FLevelLocals* level);
int WorldModeCullMode();
bool WorldModeSnapshotThings(FLevelLocals* level, const sector_t* sector, const struct FSnapshotThing*& listed, uint32_t& listedCount,
	AActor* const*& portal, uint32_t& portalCount);
AActor* WorldModeLiveActor(AActor* actor);
player_t* WorldModeRenderPlayer(int pnum);
int WorldModePlayerIndex(const player_t* player);
struct FPlayerBob;
const FPlayerBob& WorldModePlayerBob(const player_t* player);
sector_t* WorldModePublishedSector(sector_t* sector);
int WorldModeInterpolationSkip(FLevelLocals* level);
void WorldModeMarkLine(FLevelLocals* level, int index);
void WorldModeMarkSubsector(FLevelLocals* level, int index);
void WorldModeMarkSector(FLevelLocals* level, int index);
void WorldModeApplyMapFlags(FLevelLocals* level);
int WorldModeFrameTic(FLevelLocals* level);
struct FLightGrid;
struct FDynamicLight;
struct particle_t;
class DVisualThinker;
FLightGrid* WorldModeLightGrid(FLevelLocals* level);
int WorldModeFrameTimer(FLevelLocals* level);
void WorldModePublishedLights(FLevelLocals* level, FDynamicLight* const*& lights, int& count);
uint32_t WorldModeGpuSpriteSerial();
FDynamicLight* WorldModePublishedLight(FDynamicLight* light);
constexpr uint32_t GpuParticleWords = 12;
bool WorldModeGpuParticleData(FLevelLocals* level, const float*& data, uint32_t& count, uint64_t& serial);
bool WorldModeSubsectorParticles(FLevelLocals* level, const subsector_t* sub, particle_t*& particles, int& first,
	DVisualThinker* const*& visuals, unsigned& visualCount);
bool WorldModeSideEffectsDue(FLevelLocals* level, bool mainView);
void WorldModeWalkerWait();
void WorldModeWalkerStop();
bool WorldModeAutomapFromWalker();
bool WorldModeOpenSectors(FLevelLocals* level, const uint8_t*& open, DVector2& from);
bool WorldModeWalkSectors(FLevelLocals* level, const uint8_t*& entered);
void ForgetWorldMode();
struct FGpuParticleIndices;
void GpuParticleProgramStore(FGpuParticleIndices& out);

struct HWDrawInfo
{
	struct wallseg
	{
		float x1, y1, z1, x2, y2, z2;
	};

	struct MissingTextureInfo
	{
		seg_t * seg;
		subsector_t * sub;
		float Planez;
		float Planezfront;
	};

	struct MissingSegInfo
	{
		seg_t * seg;
		int MTI_Index;    // tells us which MissingTextureInfo represents this seg.
	};

	struct SubsectorHackInfo
	{
		subsector_t * sub;
		uint8_t flags;
	};

	enum EFullbrightFlags
	{
		Fullbright = 1,
		Nightvision = 2,
		StealthVision = 4
	};

	bool isFullbrightScene() const { return !!(FullbrightFlags & Fullbright); }
	bool isNightvision() const { return !!(FullbrightFlags & Nightvision); }
	bool isStealthVision() const { return !!(FullbrightFlags & StealthVision); }

	unsigned OwnedNextOrder = 1;
	bool OwnOpaqueWalls = false;
	HWDrawList drawlists[GLDL_TYPES];
	int vpIndex;
	ELightMode lightmode;

	FLevelLocals *Level;
	HWDrawInfo * outer = nullptr;
	int FullbrightFlags;
	std::atomic<int> spriteindex;
	HWPortal *mClipPortal;
	HWPortal *mCurrentPortal;
	bool GpuSprites = false;
	// [Karto] Instances the BSP traversal reached (filled by the render job thread), per blend class.
	std::vector<int32_t> GpuSpriteVisible[3];
	bool UseGpuSprites();
	struct FGpuParticleRange { uint32_t First, Count; };
	static constexpr uint32_t GpuParticleProgramRange = 1u << 31;
	bool GpuParticles = false;
	uint64_t GpuParticleSerial = 0;	// the published particles' serial this scene draws
	std::vector<FGpuParticleRange> GpuParticleRanges;
	std::vector<uint16_t> GpuParticleIndices;	// per range, First..First+Count: the leader, then its followers
	bool UseGpuParticles();
	void DrawGpuSprites(FRenderState &state, int blend);
	//FRotator mAngles;
	Clipper *mClipper;
	Clipper *vClipper; // Vertical clipper
	Clipper *rClipper; // Radar clipper
	FRenderViewpoint Viewpoint;
	HWViewpointUniforms VPUniforms;	// per-viewpoint uniform state
	VSMatrix ProjectionMatrix2;
	TArray<HWPortal *> Portals;
	TArray<HWDecal *> Decals[2];	// the second slot is for mirrors which get rendered in a separate pass.
	TArray<HUDSprite> hudsprites;	// These may just be stored by value.
	//TArray<ACorona*> Coronas;
	uint64_t LastFrameTime = 0;

	TArray<MissingTextureInfo> MissingUpperTextures;
	TArray<MissingTextureInfo> MissingLowerTextures;

	TArray<MissingSegInfo> MissingUpperSegs;
	TArray<MissingSegInfo> MissingLowerSegs;

	TArray<SubsectorHackInfo> SubsectorHacks;

	TMap<int, gl_subsectorrendernode*> otherFloorPlanes;
	TMap<int, gl_subsectorrendernode*> otherCeilingPlanes;
	TMap<int, gl_floodrendernode*> floodFloorSegs;
	TMap<int, gl_floodrendernode*> floodCeilingSegs;

	//TArray<sector_t *> CeilingStacks;
	//TArray<sector_t *> FloorStacks;

	TArray<subsector_t *> HandledSubsectors;

	TArray<uint8_t> section_renderflags;
	TArray<uint8_t> ss_renderflags;
	TArray<uint8_t> no_renderflags;

	// This is needed by the BSP traverser.
	BitArray CurrentMapSections;	// this cannot be a single number, because a group of portals with the same displacement may link different sections.
	area_t	in_area;
	fixed_t viewx, viewy;	// since the nodes are still fixed point, keeping the view position  also fixed point for node traversal is faster.
	bool multithread;
	const void* BakeSweptLevel = nullptr;

private:
	// For ProcessLowerMiniseg
	bool inview;
	subsector_t * viewsubsector;
	TArray<seg_t *> lowersegs;

	subsector_t *currentsubsector;	// used by the line processing code.
	sector_t *currentsector;

	void WorkerThread();

	void UnclipSubsector(subsector_t *sub);

	bool IsDistanceCulled(seg_t *line);
	void AddLine(seg_t *seg, bool portalclip);
	void PolySubsector(subsector_t * sub);
	void RenderPolyBSPNode(void *node);
	void AddPolyobjs(subsector_t *sub);
	void AddLines(subsector_t * sub, sector_t * sector);
	void AddSpecialPortalLines(subsector_t * sub, sector_t * sector, linebase_t *line);
	public:
	bool UseActorSnapshot = false;
	void RenderThings(subsector_t * sub, sector_t * sector);
	void RenderParticles(subsector_t *sub, sector_t *front);
	void RenderGpuParticles(particle_t* particles, int first, sector_t* front, subsector_t* sub);
	void DoSubsector(subsector_t * sub);
	int SetupLightsForOtherPlane(subsector_t * sub, FDynLightData &lightdata, const secplane_t *plane);
	int CreateOtherPlaneVertices(subsector_t *sub, const secplane_t *plane);
	void DrawPSprite(HUDSprite *huds, FRenderState &state);
	WeaponLighting GetWeaponLighting(sector_t *viewsector, const DVector3 &pos, int cm, area_t in_area, const DVector3 &playerpos, bool weaponPureLightLevel);

	void PreparePlayerSprites2D(sector_t * viewsector, area_t in_area);
	void PreparePlayerSprites3D(sector_t * viewsector, area_t in_area);
public:

	void SetCameraPos(const DVector3 &pos)
	{
		VPUniforms.mCameraPos = { (float)pos.X, (float)pos.Z, (float)pos.Y, 0.f };
	}

	void SetClipHeight(float h, float d)
	{
		VPUniforms.mClipHeight = h;
		VPUniforms.mClipHeightDirection = d;
		VPUniforms.mClipLine.X = -1000001.f;
	}

	void SetClipLine(line_t *line)
	{
		VPUniforms.mClipLine = { (float)line->v1->fX(), (float)line->v1->fY(), (float)line->Delta().X, (float)line->Delta().Y };
		VPUniforms.mClipHeight = 0;
		VPUniforms.mClipHeightDirection = 0.f;
	}

	HWPortal * FindPortal(const void * src);
	void RenderBSPNode(void *node);
	void RenderOrthoNoFog();
	void RenderBSP(void *node, bool drawpsprites);
	void GPUBakeSweepWalls();
	void BakeWorldMode();
	void UpdateWorldMode(bool publish = false);
	void ReplayWorldModeSky();
	void ReplayWorldModeHorizons();
	void ReplayWorldModeTranslucentWalls();
	void CullWorldMode(FRenderState& state);
	void CollectWorldModeSprites();
	void CollectWorldModeMissingTextures();
	void DrawWorldModeVisibilityPass(FRenderState& state);
	void WorldModeWalkerFrame();

	static HWDrawInfo *StartDrawInfo(FLevelLocals *lev, HWDrawInfo *parent, FRenderViewpoint &parentvp, HWViewpointUniforms *uniforms);
	void StartScene(FRenderViewpoint &parentvp, HWViewpointUniforms *uniforms);
	void ClearBuffers();
	HWDrawInfo *EndDrawInfo();
	void SetViewArea();
	int SetFullbrightFlags(player_t *player);

	void DrawScene(int drawmode);
	void CreateScene(bool drawpsprites);
	void RenderScene(FRenderState &state);
	void RenderTranslucent(FRenderState &state);
	void RenderPortal(HWPortal *p, FRenderState &state, bool usestencil);
	void RenderLateSkyPortal(HWPortal *p, FRenderState &state);
	void EndDrawScene(sector_t * viewsector, FRenderState &state);
	void DrawEndScene2D(sector_t * viewsector, FRenderState &state);
	void Set3DViewport(FRenderState &state);
	void ProcessScene(bool toscreen);

	bool DoOneSectorUpper(subsector_t * subsec, float planez, area_t in_area);
	bool DoOneSectorLower(subsector_t * subsec, float planez, area_t in_area);
	bool DoFakeBridge(subsector_t * subsec, float planez, area_t in_area);
	bool DoFakeCeilingBridge(subsector_t * subsec, float planez, area_t in_area);

	bool CheckAnchorFloor(subsector_t * sub);
	bool CollectSubsectorsFloor(subsector_t * sub, sector_t * anchor);
	bool CheckAnchorCeiling(subsector_t * sub);
	bool CollectSubsectorsCeiling(subsector_t * sub, sector_t * anchor);
	void CollectSectorStacksCeiling(subsector_t * sub, sector_t * anchor, area_t in_area);
	void CollectSectorStacksFloor(subsector_t * sub, sector_t * anchor, area_t in_area);

	void DispatchRenderHacks();
	void AddUpperMissingTexture(side_t * side, subsector_t *sub, float backheight);
	void AddLowerMissingTexture(side_t * side, subsector_t *sub, float backheight);
	void HandleMissingTextures(area_t in_area);
	void PrepareUnhandledMissingTextures();
	void PrepareUpperGap(seg_t * seg);
	void PrepareLowerGap(seg_t * seg);
	void CreateFloodPoly(wallseg * ws, FFlatVertex *vertices, float planez, sector_t * sec, bool ceiling);
	void CreateFloodStencilPoly(wallseg * ws, FFlatVertex *vertices);

	void AddHackedSubsector(subsector_t * sub);
	void HandleHackedSubsectors();
	void AddFloorStack(sector_t * sec);
	void AddCeilingStack(sector_t * sec);
	void ProcessSectorStacks(area_t in_area);

	void ProcessActorsInPortal(FLinePortalSpan *glport, area_t in_area);

	void AddOtherFloorPlane(int sector, gl_subsectorrendernode * node);
	void AddOtherCeilingPlane(int sector, gl_subsectorrendernode * node);

	void GetDynSpriteLight(AActor *self, float x, float y, float z, FSection *sec, int portalgroup, float *out);
	void GetDynSpriteLight(AActor *thing, particle_t *particle, float *out);
	void GetDynSpriteLight(const struct FSpriteRecord& record, float *out);

	void PreparePlayerSprites(sector_t * viewsector, area_t in_area);
	void PrepareTargeterSprites(double ticfrac);

	void UpdateCurrentMapSection();
	void SetViewMatrix(const FRotator &angles, float vx, float vy, float vz, bool mirror, bool planemirror);
	void SetupView(FRenderState &state, float vx, float vy, float vz, bool mirror, bool planemirror);
	angle_t FrustumAngle();

	void DrawDecals(FRenderState &state, TArray<HWDecal *> &decals);
	void DrawPlayerSprites(bool hudModelStep, FRenderState &state);
	void DrawCoronas(FRenderState& state);
	void DrawCorona(FRenderState& state, ACorona* corona, double dist);

	void SetDitherTransFlags(AActor* actor);

	void ProcessLowerMinisegs(TArray<seg_t *> &lowersegs);
	void AddSubsectorToPortal(FSectorPortalGroup *portal, subsector_t *sub);

	void AddWall(HWWall *w, bool retained = false, unsigned order = 0, unsigned part = 0, bool ownedBatch = false);
	void AddMirrorSurface(HWWall *w);
	void AddFlat(HWFlat *flat, bool fog);
	void AddSprite(HWSprite *sprite, bool translucent);


	HWDecal *AddDecal(bool onmirror);

	void SetFallbackLightMode()
	{
		lightmode = ELightMode::Doom;
	}

};

void CleanSWDrawer();
sector_t* RenderViewpoint(FRenderViewpoint& mainvp, AActor* camera, IntRect* bounds, float fov, float ratio, float fovratio, bool mainview, bool toscreen);
void WriteSavePic(player_t* player, FileWriter* file, int width, int height);
sector_t* RenderView(player_t* player);


inline bool isSoftwareLighting(ELightMode lightmode)
{
	return lightmode == ELightMode::ZDoomSoftware || lightmode == ELightMode::DoomSoftware || lightmode == ELightMode::Build;
}

inline bool isBuildSoftwareLighting(ELightMode lightmode)
{
	return lightmode == ELightMode::Build;
}

inline bool isDoomSoftwareLighting(ELightMode lightmode)
{
	return lightmode == ELightMode::ZDoomSoftware || lightmode == ELightMode::DoomSoftware;
}

inline bool isDarkLightMode(ELightMode lightmode)
{
	return lightmode == ELightMode::Doom || lightmode == ELightMode::DoomDark;
}

int CalcLightLevel(ELightMode lightmode, int lightlevel, int rellight, bool weapon, int blendfactor, bool weaponPureLightLevel = false);
PalEntry CalcLightColor(ELightMode lightmode, int light, PalEntry pe, int blendfactor);
float GetFogDensity(FLevelLocals* Level, ELightMode lightmode, int lightlevel, PalEntry fogcolor, int sectorfogdensity, int blendfactor);
bool CheckFog(FLevelLocals* Level, sector_t* frontsector, sector_t* backsector, ELightMode lightmode);
void SetColor(FRenderState& state, FLevelLocals* Level, ELightMode lightmode, int sectorlightlevel, int rellight, bool fullbright, const FColormap& cm, float alpha, bool weapon = false);
void SetShaderLight(FRenderState& state, FLevelLocals* Level, float level, float olight);
void SetFog(FRenderState& state, FLevelLocals* Level, ELightMode lightmode, int lightlevel, int rellight, bool fullbright, const FColormap* cmap, bool isadditive);

struct FLightCallTrace
{
	struct { ELightMode Mode; int Level, Rel; bool Fullbright, Weapon; FColormap Cm; float Alpha; } Color = {};
	struct { ELightMode Mode; int Level, Rel; bool Fullbright, HasCm, Additive; FColormap Cm; } Fog = {};
	bool HasColor = false, HasFog = false, FogLast = false;
};
extern FLightCallTrace* hw_LightTrace;
