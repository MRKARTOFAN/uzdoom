/*
** hw_drawstructs.h
**
** One wall segment in the draw list
**
**---------------------------------------------------------------------------
**
** Copyright 2018-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#pragma once

#include "r_defs.h"
#include "renderstyle.h"
#include "textures.h"
#include "r_data/colormaps.h"

#ifdef _MSC_VER
#pragma warning(disable:4244)
#endif

bool CheckFog(FLevelLocals* Level, sector_t* frontsector, sector_t* backsector, ELightMode lightmode);

struct HWHorizonInfo;
struct HWSkyInfo;
struct F3DFloor;
class FMaterial;
class IVertexBuffer;
struct FTexCoordInfo;
struct FSectorPortalGroup;
struct FFlatVertex;
struct HWPreparedWall;
struct HWDrawInfo;
void BeginRetainedWorldView(FLevelLocals* level);
bool CanOwnOpaqueWalls(HWDrawInfo* di);
void ClearRetainedWorld();
struct FLinePortalSpan;
struct FDynLightData;
class VSMatrix;
struct FSpriteModelFrame;
struct particle_t;
class FRenderState;
struct HWDecal;
struct FSection;
enum area_t : int;

enum HWRenderStyle
{
	STYLEHW_Normal,			// default
	STYLEHW_Solid,			// drawn solid (needs special treatment for sprites)
	STYLEHW_NoAlphaTest,	// disable alpha test
};

enum WallTypes
{
	RENDERWALL_NONE,
	RENDERWALL_TOP,
	RENDERWALL_M1S,
	RENDERWALL_M2S,
	RENDERWALL_BOTTOM,
	RENDERWALL_FOGBOUNDARY,
	RENDERWALL_MIRRORSURFACE,
	RENDERWALL_M2SNF,
	RENDERWALL_COLOR,
	RENDERWALL_FFBLOCK,
	// Insert new types at the end!
};

enum PortalTypes
{
	PORTALTYPE_SKY,
	PORTALTYPE_HORIZON,
	PORTALTYPE_SKYBOX,
	PORTALTYPE_SECTORSTACK,
	PORTALTYPE_PLANEMIRROR,
	PORTALTYPE_MIRROR,
	PORTALTYPE_LINETOLINE,
};

//==========================================================================
//
// One sector plane, still in fixed point
//
//==========================================================================

struct HWSectorPlane
{
	FTextureID texture;
	secplane_t plane;
	float Texheight;
	float	Angle;
	FVector2 Offs;
	FVector2 Scale;

	void GetFromSector(sector_t * sec, int ceiling)
	{
		Offs.X = (float)sec->GetXOffset(ceiling);
		Offs.Y = (float)sec->GetYOffset(ceiling);
		Scale.X = (float)sec->GetXScale(ceiling);
		Scale.Y = (float)sec->GetYScale(ceiling);
		Angle = (float)sec->GetAngle(ceiling).Degrees();
		texture = sec->GetTexture(ceiling);
		plane = sec->GetSecPlane(ceiling);
		Texheight = (float)((ceiling == sector_t::ceiling)? plane.fD() : -plane.fD());
	}
};

struct HWSeg
{
	float x1,x2;
	float y1,y2;
	float fracleft, fracright;	// fractional offset of the 2 vertices on the linedef

	FVector3 Normal() const
	{
		// we do not use the vector math inlines here because they are not optimized for speed but accuracy in the playsim and this is called quite frequently.
		float x = y2 - y1;
		float y = x1 - x2;
		float ilength = 1.f / sqrtf(x*x + y*y);
		return FVector3(x * ilength, 0, y * ilength);
	}
};

struct texcoord
{
	float u,v;
};

struct HWDrawInfo;
class HWWall;

// this only exists to keep HWWallDispatcher trivial

struct HWWallDispatcher;

struct HWPreparedWallRun
{
	FGameTexture* Texture = nullptr;
	IVertexBuffer* Buffer = nullptr;
	int ClampMode = -1;
};

struct HWRetainedSectorAppearance
{
	FColormap Colormap;
	int WallLight;
	int PlaneLight[2];
	HWSectorPlane Planes[2];
	float TopGlow[4];
	float BottomGlow[4];
	bool Glowing;
};

bool RetainedWorldShadersCompatible();
const HWRetainedSectorAppearance* QueryRetainedSectorAppearance(sector_t* sector, int plane = -1);
bool ApplyRetainedWorldLighting(HWDrawInfo* di, FRenderState& state, unsigned& slot,
	int light, int relativeLight, const FColormap& colormap, float alpha, bool additive, bool fogFirst,
	HWPreparedWall* owned = nullptr, uint64_t ownerVersion = 0);
bool ApplyRetainedSectorLighting(HWDrawInfo* di, FRenderState& state, sector_t* sector, int plane,
	int light, int relativeLight, const FColormap& colormap, float alpha);

struct FGpuPersistentWall;

class HWWall
{
public:
	HWPreparedWall* Prepared = nullptr;
	FGpuPersistentWall* GpuOwnerCandidate = nullptr;
	uint64_t GpuResidentEpoch = 0;
	uint32_t GpuResidentRow = UINT32_MAX;
	uint32_t GpuAppearanceId = UINT32_MAX;
	uint64_t GpuAppearanceRevision = 0;
	int32_t GpuOwnerSlot = -1;
	uint32_t GpuBakeRow = UINT32_MAX;
	static const char passflag[];

	enum
	{
		HWF_CLAMPX = 1,
		HWF_CLAMPY = 2,
		HWF_SKYHACK = 4,
		HWF_GLOW = 8,		// illuminated by glowing flats
		HWF_NOSPLITUPPER = 16,
		HWF_NOSPLITLOWER = 32,
		HWF_NOSPLIT = 64,
		HWF_TRANSLUCENT = 128,
		HWF_NOSLICE = 256
	};

	enum
	{
		RWF_BLANK = 0,
		RWF_TEXTURED = 1,	// actually not being used anymore because with buffers it's even less efficient not writing the texture coordinates - but leave it here
		RWF_NOSPLIT = 4,
		RWF_NORENDER = 8,
		RWF_BATCH = 16,
	};

	enum
	{
		LOLFT,
		UPLFT,
		UPRGT,
		LORGT,
	};

	friend struct HWDrawList;
	friend class HWPortal;

	vertex_t* vertexes[2];				// required for polygon splitting
	FGameTexture* texture;
	TArray<lightlist_t>* lightlist;
	LightmapSurface* lightmap;

	HWSeg glseg;
	float ztop[2], zbottom[2];
	texcoord tcs[4];
	texcoord lightuv[4];
	float lindex;
	float alpha;

	FColormap Colormap;
	ERenderStyle RenderStyle;

	float ViewDistance;

	short lightlevel;
	uint16_t flags;
	uint8_t type;
	short rellight;

	float topglowcolor[4];
	float bottomglowcolor[4];

	int dynlightindex;

	union
	{
		// it's either one of them but never more!
		FSectorPortal* secportal;	// sector portal (formerly skybox)
		HWSkyInfo* sky;			// for normal sky
		HWHorizonInfo* horizon;	// for horizon information
		FSectorPortalGroup* portal;			// stacked sector portals
		secplane_t* planemirror;	// for plane mirrors
		FLinePortalSpan* lineportal;	// line-to-line portals
		struct
		{
			int portaltype;				// for the mesh builder. Real portals can only be assigned when being rendered.
			int portalplane;
		};
	};

	sector_t* skysector = nullptr;
	int skyplane = -1, skyportalplane = -1;

	int HeightTop = -1, HeightBottom = -1, HeightAnchor = -1;
	float HeightVScale = 0.f;
	bool HeightArmed = false, HeightValid = false;
	bool HeightKnown = false;



	// these are not the same as ytop and ybottom!!!
	float zceil[2];
	float zfloor[2];

	unsigned int vertindex;
	unsigned int vertcount;

public:
	seg_t* seg;			// this gives the easiest access to all other structs involved
	subsector_t* sub;		// For polyobjects
	sector_t* frontsector, * backsector;
	//private:

	void PutWall(HWWallDispatcher* di, bool translucent);
	void PutPortal(HWWallDispatcher* di, int ptype, int plane);
	void CheckTexturePosition(FTexCoordInfo* tci);

	void Put3DWall(HWWallDispatcher* di, lightlist_t* lightlist, bool translucent);
	bool SplitWallComplex(HWWallDispatcher* di, sector_t* frontsector, bool translucent, float& maplightbottomleft, float& maplightbottomright);
	void SplitWall(HWWallDispatcher* di, sector_t* frontsector, bool translucent);

	void SetupLights(HWDrawInfo* di, FDynLightData& lightdata);

	void MakeVertices(bool nosplit);
	void PrepareWall(HWWallDispatcher* di, seg_t* seg, sector_t* frontsector, sector_t* backsector, bool isculled);
	bool CanCacheVertices(bool allowGenerated = false) const;
	bool CanGpuDriveWall(bool allowGenerated = false) const;
	bool CanDrawPersistentWall(unsigned* rejected = nullptr) const;
	bool DrawPreparedWall(HWWallDispatcher* di, FRenderState& state, HWPreparedWallRun& run, uint64_t ownerVersion = 0);

	void SkyPlane(HWWallDispatcher* di, sector_t* sector, int plane, bool allowmirror);
	void SkyLine(HWWallDispatcher* di, sector_t* sec, line_t* line);
	void ReplaySky(HWWallDispatcher* di);
	void SkyNormal(HWWallDispatcher* di, sector_t* fs, vertex_t* v1, vertex_t* v2);
	void SkyTop(HWWallDispatcher* di, seg_t* seg, sector_t* fs, sector_t* bs, vertex_t* v1, vertex_t* v2);
	void SkyBottom(HWWallDispatcher* di, seg_t* seg, sector_t* fs, sector_t* bs, vertex_t* v1, vertex_t* v2);

	bool DoHorizon(HWWallDispatcher* di, seg_t* seg, sector_t* fs, vertex_t* v1, vertex_t* v2);

	bool SetWallCoordinates(seg_t* seg, FTexCoordInfo* tci, float ceilingrefheight,
		float topleft, float topright, float bottomleft, float bottomright, float t_ofs, float skew);

	void DoTexture(HWWallDispatcher* di, int type, seg_t* seg, int peg,
		float ceilingrefheight, float floorrefheight,
		float CeilingHeightstart, float CeilingHeightend,
		float FloorHeightstart, float FloorHeightend,
		float v_offset, float skew);

	void DoMidTexture(HWWallDispatcher* di, seg_t* seg, bool drawfogboundary,
		sector_t* front, sector_t* back,
		sector_t* realfront, sector_t* realback,
		float fch1, float fch2, float ffh1, float ffh2,
		float bch1, float bch2, float bfh1, float bfh2, float zalign, float skew);

	void GetPlanePos(F3DFloor::planeref* planeref, float& left, float& right);

	void BuildFFBlock(HWWallDispatcher* di, seg_t* seg, F3DFloor* rover, int roverIndex,
		float ff_topleft, float ff_topright,
		float ff_bottomleft, float ff_bottomright);
	void InverseFloors(HWWallDispatcher* di, seg_t* seg, sector_t* frontsector,
		float topleft, float topright,
		float bottomleft, float bottomright);
	void ClipFFloors(HWWallDispatcher* di, seg_t* seg, F3DFloor* ffloor, int ffloorIndex, sector_t* frontsector,
		float topleft, float topright,
		float bottomleft, float bottomright);
	void DoFFloorBlocks(HWWallDispatcher* di, seg_t* seg, sector_t* frontsector, sector_t* backsector,
		float fch1, float fch2, float ffh1, float ffh2,
		float bch1, float bch2, float bfh1, float bfh2);

	void ProcessDecal(HWDrawInfo* di, DBaseDecal* decal, const FVector3& normal);
	void ProcessDecals(HWDrawInfo* di);

	int CreateVertices(FFlatVertex*& ptr, bool nosplit);
	void SplitLeftEdge(FFlatVertex*& ptr);
	void SplitRightEdge(FFlatVertex*& ptr);
	void SplitUpperEdge(FFlatVertex*& ptr);
	void SplitLowerEdge(FFlatVertex*& ptr);

	void CountLeftEdge(unsigned& ptr);
	void CountRightEdge(unsigned& ptr);

	int CountVertices();

	void RenderWall(FRenderState& state, int textured, uint32_t gpuSlot = UINT32_MAX);
	void RenderFogBoundary(HWWallDispatcher* di, FRenderState& state);
	void RenderMirrorSurface(HWWallDispatcher* di, FRenderState& state);
	void RenderTexturedWall(HWWallDispatcher* di, FRenderState& state, int rflags, uint32_t gpuSlot = UINT32_MAX);
	void RenderTranslucentWall(HWWallDispatcher* di, FRenderState& state);
	void DrawDecalsForMirror(HWDrawInfo* di, FRenderState& state, TArray<HWDecal*>& decals);

public:
	void Process(HWWallDispatcher* di, seg_t* seg, sector_t* frontsector, sector_t* backsector, bool isculled);
	void ProcessLowerMiniseg(HWWallDispatcher* di, seg_t* seg, sector_t* frontsector, sector_t* backsector);

	float PointOnSide(float x, float y)
	{
		return -((y - glseg.y1) * (glseg.x2 - glseg.x1) - (x - glseg.x1) * (glseg.y2 - glseg.y1));
	}

	void DrawWall(HWWallDispatcher* di, FRenderState& state, bool translucent, uint32_t gpuSlot = UINT32_MAX);

};

//==========================================================================
//
// One flat plane in the draw list
//
//==========================================================================

class HWFlat
{
public:
	int RetainedSurface = -1;
	sector_t * sector;
	FSection *section;
	FGameTexture *texture;
	TextureManipulation* TextureFx;

	float z; // the z position of the flat (only valid for non-sloped planes)
	FColormap Colormap;	// light and fog
	PalEntry FlatColor;
	PalEntry AddColor;
	ERenderStyle renderstyle;

	float alpha;
	HWSectorPlane plane;
	int lightlevel;
	bool stack;
	bool ceiling;
	uint8_t renderflags;
	uint8_t hacktype;
	int iboindex;
	//int vboheight;

	int dynlightindex;

	void CreateSkyboxVertices(FFlatVertex *buffer);
	void SetupLights(HWDrawInfo *di, FDynLightData &lightdata, int portalgroup);

	void PutFlat(HWDrawInfo *di, bool fog = false);
	void Process(HWDrawInfo *di, sector_t * model, int whichplane, bool notexture);
	void SetFrom3DFloor(F3DFloor *rover, bool top, bool underside);
	void ProcessSector(HWDrawInfo *di, sector_t * frontsector, int which = 7 /*SSRF_RENDERALL*/);	// cannot use constant due to circular dependencies.

	void DrawSubsectors(HWDrawInfo *di, FRenderState &state);
	void DrawFlat(HWDrawInfo *di, FRenderState &state, bool translucent);

	void DrawOtherPlanes(HWDrawInfo *di, FRenderState &state);
	void DrawFloodPlanes(HWDrawInfo *di, FRenderState &state);
};

//==========================================================================
//
// One sprite in the draw list
//
//==========================================================================


class HWSprite
{
public:
	int lightlevel;
	uint8_t foglevel;
	uint8_t hw_styleflags;
	bool fullbright;
	bool polyoffset;
	PalEntry ThingColor;	// thing's own color
	FColormap Colormap;
	FSpriteModelFrame * modelframe;
	int modelframeflags;
	FRenderStyle RenderStyle;
	int OverrideShader;

	FTranslationID translation;
	int index;
	float depth;
	int vertexindex;

	float topclip;
	float bottomclip;

	float x,y,z;	// needed for sorting!

	float ul,ur;
	float vt,vb;
	float x1,y1,z1;
	float x2,y2,z2;
	float offx, offy;
	float trans;
	int dynlightindex;
	int32_t GpuParticleRange = -1;

	FGameTexture *texture;
	AActor * actor;
	const struct FSpriteRecord* record = nullptr;
	particle_t * particle;
	TArray<lightlist_t> *lightlist;
	DRotator Angles;

	void SplitSprite(HWDrawInfo *di, sector_t * frontsector, bool translucent);
	void PerformSpriteClipAdjustment(AActor *thing, const DVector2 &thingpos, float spriteheight);
	void PerformSpriteClipAdjustment(const struct FSpriteClipSource& thing, const DVector2 &thingpos, float spriteheight);
	bool CalculateVertices(HWDrawInfo *di, FVector3 *v, DVector3 *vp);

public:

	void CreateVertices(HWDrawInfo *di, bool fan = false);
	void ProcessRecord(HWDrawInfo *di, const struct FSpriteRecord& r, sector_t * sector, area_t in_area, int thruportal);
	void PutSprite(HWDrawInfo *di, bool translucent);
	void Process(HWDrawInfo *di, AActor* thing,sector_t * sector, area_t in_area, int thruportal = false, bool isSpriteShadow = false);
	void ProcessParticle(HWDrawInfo *di, particle_t *particle, sector_t *sector, class DVisualThinker *spr);//, int shade, int fakeside)
	void AdjustVisualThinker(HWDrawInfo *di, DVisualThinker *spr, sector_t *sector);

	// [Karto] runStart: the first sprite of a run of equal draw keys batches its quad and leaves its state set for the
	// repeats; FinishAdditiveRun or FinishSolidRun restores what DrawSprite's end would have.
	void DrawSprite(HWDrawInfo *di, FRenderState &state, bool translucent, bool runStart = false);
	void DrawRunRepeat(HWDrawInfo *di, FRenderState &state);
	static void FinishAdditiveRun(FRenderState &state);
	static void FinishSolidRun(FRenderState &state);
};

// [Karto] Every input DrawSprite's state setup reads for a batched face sprite; equal keys set identical state.
struct HWSpriteRunKey
{
	FGameTexture *texture;
	const void *cls;
	FColormap Colormap;
	int lightlevel;
	int OverrideShader;
	int translation;
	uint32_t style;
	uint32_t thingColor, sectorColor, sectorAdd;
	float trans;
	uint8_t foglevel, hw_styleflags;
	bool fullbright;
	bool hasSector;
	// [Karto] SetMaterial expands face sprites drawn from an actor but not record sprites (DrawSprite's spritetype).
	bool fromRecord;
	// Per-sprite dynamic light (lit level, not fullbright): never repeated.
	bool dynlight;

	void Set(HWDrawInfo *di, const HWSprite &s);
	bool operator==(const HWSpriteRunKey &o) const;
};




struct DecalVertex
{
	float x, y, z;
	float u, v;
};

struct HWDecal
{
	FGameTexture *texture;
	TArray<lightlist_t> *lightlist;
	DBaseDecal *decal;
	DecalVertex dv[4];
	float zcenter;
	unsigned int vertindex;

	FRenderStyle renderstyle;
	int lightlevel;
	int rellight;
	float alpha;
	FColormap Colormap;
	int dynlightindex;
	sector_t *frontsector;
	FVector3 Normal;

	void DrawDecal(HWDrawInfo *di, FRenderState &state);

};


inline float Dist2(float x1,float y1,float x2,float y2)
{
	return sqrtf((x1-x2)*(x1-x2)+(y1-y2)*(y1-y2));
}

bool hw_SetPlaneTextureRotation(const HWSectorPlane * secplane, FGameTexture * gltexture, VSMatrix &mat);
void hw_GetDynModelLight(AActor *self, FDynLightData &modellightdata);
LightProbe* FindLightProbe(FLevelLocals* level, float x, float y, float z);

extern const float LARGE_VALUE;

struct FDynLightData;
struct FDynamicLight;
bool GetLight(FDynLightData& dld, int group, Plane& p, FDynamicLight* light, bool checkside);
bool LightReachesPlane(Plane& p, const DVector3& pos, float radius, bool checkside);
void AddLightToList(FDynLightData &dld, int group, FDynamicLight* light, bool forceAttenuate);
void SetSplitPlanes(FRenderState& state, const secplane_t& top, const secplane_t& bottom);
