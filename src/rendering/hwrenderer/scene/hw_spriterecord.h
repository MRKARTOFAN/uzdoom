#pragma once

#include "actor.h"

struct FSpriteRecord
{
	AActor* Live;			// identity only (camera, light target): frames never dereference it
	sector_t* Sector;		// the published sector copy when there is one, else the live sector
	FSection* section;
	int PortalGroup;
	DVector3 Pos, Prev, WorldOffset;
	DVector2 Scale, PrevScale, SpriteOffset;
	double Alpha, PrevAlpha;
	double Floorclip, Height, floorz, ceilingz, radius, RenderRadius;
	double FloatBobStrength, FloatBobFactor;
	DRotator Angles;
	DAngle SpriteAngle, SpriteRotation;
	PClassActor* Class;
	FRenderStyle RenderStyle;
	FTranslationID Translation;
	uint32_t fillcolor, RenderRequired, RenderHidden, SpawnOrder;
	ActorRenderFlags renderflags;
	ActorRenderFlags2 renderflags2;
	ActorFlags flags;
	ActorFlags2 flags2;
	ActorFlags3 flags3;
	ActorFlags5 flags5;
	ActorFlags7 flags7;
	ActorFlags8 flags8;
	ActorFlags9 flags9;
	int sprite, waterlevel, MapSection;
	int validcount;			// the sprite pass's per-frame dedup across sector lists, as AActor::validcount
	int16_t LightLevel;
	uint8_t frame, FloatBobPhase;
	bool Moving, IsInventory, ClientSide;
	int32_t Instance;		// Index of this sprite's GPU instance this tic, or -1
};

struct FSpriteFrameEntry
{
	FGameTexture* Texture[16];
	float UL[16], UR[16], VT[16], VB[16];
	FloatRect Rect[16];
	uint16_t Flip;
	bool SameFirst;			// Texture[0] == Texture[1]: GetSpriteFrame's 8-rotation angle formula
	bool Usable;			// every rotation a valid, non-canvas texture
	bool Translucent;		// some rotation's texture has translucent pixels (drawn blended, not solid)
	uint16_t TranslucentRotations;
	uint16_t FullbrightDisabled;	// rotations whose texture disables RF_FULLBRIGHT
};

enum ESpriteInstanceFlags : uint32_t
{
	SIF_DontInterpolate = 1, SIF_SpriteAngle = 2, SIF_XFlip = 4, SIF_SpriteFlip = 8, SIF_YFlip = 16, SIF_FloatBob = 32,
	SIF_ClientSide = 64, SIF_ClipThing = 128, SIF_Corpse = 256, SIF_Monster = 512, SIF_NoGravity = 1024, SIF_Moving = 2048,
	SIF_Roll = 4096, SIF_RollCenter = 8192, SIF_RollScale = 16384,
	SIF_XYBillboard = 32768,
	SIF_NoDynLight = 65536,
	SIF_ShallowWater = 131072,	// waterlevel 1-2: no anamorphosis
};

struct FSpriteInstance
{
	float Prev[3], Pos[3], WorldOffset[3];
	float Scale[2];
	float Floorclip, Height;
	float BobPhase, BobFactor, BobStrength;
	float Angle;			// degrees: SpriteAngle with SIF_SpriteAngle, else yaw + sprite rotation
	float ClipBottom;		// floor at the actor minus floorclip, as sprite clipping takes it
	float Ceiling[4];		// ceiling plane: normal x, normal y, negiC, D (evaluated at the interpolated position)
	uint32_t Frame;			// SpriteFrames index
	uint32_t Flags;
	int32_t Translation;	// FTranslationID index: selects the bindless texture variant
	int32_t Record;			// index of the record (appearance inputs, CPU fallback)
	uint32_t GpuFrame;		// entry of the GPU frame table: (Frame, Translation) with bindless IDs per rotation
	uint32_t Appearance;	// appearance key: the row the frame builds for it
	float DynLight[3];
	float Roll;				// degrees, with SIF_Roll
	float Offset[2];
	float LightZ;
	float Radius;
	float Floor[4];			// floor plane, as Ceiling: anamorphosis (gl_spriteclip -1) reads it at the view
};
static_assert(sizeof(FSpriteInstance) == 40 * 4, "GPU sprite instance layout (GPU_SPRITE_INSTANCES in vk_shader.cpp)");

struct FSpriteAppearanceKey
{
	sector_t* Sector;
	int LightLevel;
	bool Fullbright;
	uint8_t Blend;
	float Alpha;
	bool SoftTexture;
	bool Shaded;
	uint32_t FillColor;
};

// [Karto] GPU frame table: per (frame, translation), a header word (flip bits, SameFirst << 16, Failed << 17), 3
// pad words, then 16 rotations of { 4 bindless IDs, UL, UR, VT, VB, rect left/top/width/height } = 196 words.
enum { GpuSpriteFrameWords = 196 };

bool WorldModeGpuSprites(FLevelLocals* level, const FSpriteInstance*& instances, uint32_t& count, const FSpriteAppearanceKey*& keys,
	uint32_t& keyCount);
void WorldModeFillSpriteFrameTable(class FRenderState& state, const uint32_t*& words, uint32_t& wordCount, uint64_t& version);
uint32_t WorldModeGpuSpriteSerial();
struct FGpuLightPack;
bool WorldModeGpuLightPack(FLevelLocals* level, FGpuLightPack& out);

const FSpriteInstance* WorldModeSpriteInstance(int32_t index);
const FSpriteRecord* WorldModeSpriteRecord(int32_t index);
int WorldModeGpuSpriteBlend(int32_t instance);
const FSpriteFrameEntry* WorldModeSpriteFrameEntry(uint32_t frame);

// [Karto] One entry of a sector's per-publish thing split: GPU entries first (Blend 0-2), then CPU entries (Blend 0xff).
struct FThingSplitEntry
{
	uint32_t Instance, GpuFrame, MapSection, Listed;
	uint8_t Blend;
};

bool WorldModeThingSplit(FLevelLocals* level, const sector_t* sector, const FThingSplitEntry*& entries, uint32_t& gpuCount,
	uint32_t& count);
bool WorldModeOccupiedSectors(FLevelLocals* level, const uint32_t*& sectors, uint32_t& count, const uint32_t*& subStart,
	const uint32_t*& subs);
bool WorldModeGpuFrameFailed(uint32_t gpuFrame);
uint32_t WorldModeGpuInstanceCount();

struct FSnapshotThing
{
	AActor* Copy;
	FSpriteRecord* Record;
};
