/*
** hw_renderstate.h
**
**
**
**---------------------------------------------------------------------------
**
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#pragma once

#include "vectors.h"
#include "matrix.h"
#include "hw_material.h"
#include "texmanip.h"
#include "version.h"
#include "i_interface.h"

struct FColormap;
class IVertexBuffer;
class IIndexBuffer;
class PClass;

enum EClearTarget
{
	CT_Depth = 1,
	CT_Stencil = 2,
	CT_Color = 4
};

enum ERenderEffect
{
	EFF_NONE = -1,
	EFF_FOGBOUNDARY,
	EFF_SPHEREMAP,
	EFF_BURN,
	EFF_STENCIL,
	EFF_DITHERTRANS,
	MAX_EFFECTS
};

enum EAlphaFunc
{
	Alpha_GEqual = 0,
	Alpha_Greater = 1
};

enum EDrawType
{
	DT_Points = 0,
	DT_Lines = 1,
	DT_Triangles = 2,
	DT_TriangleFan = 3,
	DT_TriangleStrip = 4
};

enum EDepthFunc
{
	DF_Less,
	DF_LEqual,
	DF_Always
};

enum EStencilFlags
{
	SF_AllOn = 0,
	SF_ColorMaskOff = 1,
	SF_DepthMaskOff = 2,
};

enum EStencilOp
{
	SOP_Keep = 0,
	SOP_Increment = 1,
	SOP_Decrement = 2
};

enum ECull
{
	Cull_None,
	Cull_CCW,
	Cull_CW
};



struct FStateVec4
{
	float vec[4];

	void Set(float r, float g, float b, float a)
	{
		vec[0] = r;
		vec[1] = g;
		vec[2] = b;
		vec[3] = a;
	}
};

struct FMaterialState
{
	FMaterial *mMaterial = nullptr;
	int mClampMode;
	int mTranslation;
	int mOverrideShader;
	bool mChanged;

	void Reset()
	{
		mMaterial = nullptr;
		mTranslation = 0;
		mClampMode = CLAMP_NONE;
		mOverrideShader = -1;
		mChanged = false;
	}
};

struct FDepthBiasState
{
	float mFactor;
	float mUnits;
	bool mChanged;

	void Reset()
	{
		mFactor = 0;
		mUnits = 0;
		mChanged = false;
	}
};

enum EPassType
{
	NORMAL_PASS,
	GBUFFER_PASS,
	MAX_PASS_TYPES
};

struct FVector4PalEntry
{
	float r, g, b, a;

	bool operator==(const FVector4PalEntry &other) const
	{
		return r == other.r && g == other.g && b == other.b && a == other.a;
	}

	bool operator!=(const FVector4PalEntry &other) const
	{
		return r != other.r || g != other.g || b != other.b || a != other.a;
	}

	FVector4PalEntry &operator=(PalEntry newvalue)
	{
		const float normScale = 1.0f / 255.0f;
		r = newvalue.r * normScale;
		g = newvalue.g * normScale;
		b = newvalue.b * normScale;
		a = newvalue.a * normScale;
		return *this;
	}

	FVector4PalEntry& SetIA(PalEntry newvalue)
	{
		const float normScale = 1.0f / 255.0f;
		r = newvalue.r * normScale;
		g = newvalue.g * normScale;
		b = newvalue.b * normScale;
		a = newvalue.a;
		return *this;
	}

	FVector4PalEntry& SetFlt(float v1, float v2, float v3, float v4)
	{
		r = v1;
		g = v2;
		b = v3;
		a = v4;
		return *this;
	}

};

struct StreamData
{
	FVector4PalEntry uObjectColor;
	FVector4PalEntry uObjectColor2;
	FVector4 uDynLightColor;
	FVector4PalEntry uAddColor;
	FVector4PalEntry uTextureAddColor;
	FVector4PalEntry uTextureModulateColor;
	FVector4PalEntry uTextureBlendColor;
	FVector4PalEntry uFogColor;
	float uDesaturationFactor;
	float uInterpolationFactor;
	float timer;
	int useVertexData;
	FVector4 uVertexColor;
	FVector4 uVertexNormal;

	FVector4 uGlowTopPlane;
	FVector4 uGlowTopColor;
	FVector4 uGlowBottomPlane;
	FVector4 uGlowBottomColor;

	FVector4 uGradientTopPlane;
	FVector4 uGradientBottomPlane;

	FVector4 uSplitTopPlane;
	FVector4 uSplitBottomPlane;

	FVector4 uDetailParms;
	FVector4 uNpotEmulation;
	FVector4 padding1, padding2, padding3;
};

enum EShaderFeature
{
	SF_Desaturation = 1,
	SF_TextureManipulation = 2,
	SF_Doom64Colors = 4,
	SF_Glow = 8,
	SF_Lightmap = 16,
	SF_DynamicLights = 32,
	SF_All = 63
};
extern int ShaderFeaturesSeen;

inline void NoteShaderFeatures(const StreamData &s, int lightIndex)
{
	int f = 0;
	if (s.uDesaturationFactor != 0.0f) f |= SF_Desaturation;
	if (s.uTextureAddColor.a != 0.0f) f |= SF_TextureManipulation;
	if (s.uAddColor.r != 0.0f || s.uAddColor.g != 0.0f || s.uAddColor.b != 0.0f || s.uObjectColor2.a != 0.0f) f |= SF_Doom64Colors;
	if (s.uGlowTopColor.W > 0.0f || s.uGlowBottomColor.W > 0.0f) f |= SF_Glow;
	if (lightIndex >= 0) f |= SF_DynamicLights;
	ShaderFeaturesSeen |= f;
}

struct HWWorldLightingState
{
	FVector4 VertexColor;
	FVector4PalEntry FogColor;
	FVector4 LightParameters;
	PalEntry PackedFogColor;
	float Desaturation;
	uint8_t FogEnabled;

	bool operator==(const HWWorldLightingState& other) const
	{
		return VertexColor == other.VertexColor && FogColor == other.FogColor &&
			PackedFogColor == other.PackedFogColor && Desaturation == other.Desaturation &&
			FogEnabled == other.FogEnabled && LightParameters.Z == other.LightParameters.Z &&
			LightParameters.W == other.LightParameters.W && (!FogEnabled ||
			(LightParameters.X == other.LightParameters.X && LightParameters.Y == other.LightParameters.Y));
	}
};

// [Karto] Equal keys must produce identical fog/light state.
struct FSharedAppearanceKey
{
	int LightMode;
	int LightLevel;
	int RelativeLight;
	unsigned LightColor;
	unsigned FadeColor;
	int Desaturation;
	int BlendFactor;
	int FogDensity;
	float Alpha;
	int Additive;
	int Fullbright;
	int SoftwareLighting;
	int FogMode;
	int CullType;
	int LevelFlags;
	float CullDist;
	unsigned CullColor;
	unsigned OutsideFog;
	int BrightFog;
	int InSkybox;
	const void* Level;
	bool operator==(const FSharedAppearanceKey& other) const { return memcmp(this, &other, sizeof(*this)) == 0; }
};

struct FGpuStaticMaterial
{
	uint32_t TextureIDs[4] = {};
	float TextureMatrix[16] = {};
	float LightDistFactor[2] = {};
	float Specular[2] = {};
	float AlphaThreshold = 0.f;
	float ShaderSpeed = 0.f;
	uint32_t TextureMode = 0;
	uint32_t FogEnabled = 0;
	uint32_t Reserved = 0;
	bool Valid = false;
};

struct FWorldCaptureDraw
{
	int Type = 0;
	int Index = 0;
	int Count = 0;
	bool Indexed = false;
	bool AlphaTest = false;
	const void* Row = nullptr;
	size_t RowBytes = 0;
	int ScaleFlags = 0, ClampMode = 0, Translation = 0;
	size_t TextureIDOffset = 0;	// where the row keeps its four texture IDs
};

struct FWorldCaptureSink
{
	enum ERefusal { RefuseMaterial, RefuseState, RefusePrimitive, RefuseVertexSource, RefuseCount };
	virtual ~FWorldCaptureSink() = default;
	virtual void Capture(const FWorldCaptureDraw& draw) = 0;
	virtual void Refuse(ERefusal reason) = 0;
};

// [Karto] Opaque clusters precede alpha-tested ones.
struct FWorldModeUpload
{
	const void* Vertices = nullptr;
	size_t VertexCount = 0;
	const float* SurfaceIDs = nullptr;
	const uint32_t* Indices = nullptr;
	size_t IndexCount = 0;
	const void* Rows = nullptr;
	size_t RowCount = 0;
	size_t RowBytes = 0;
	const uint32_t* ClusterFirstIndex = nullptr;
	const uint32_t* ClusterIndexCount = nullptr;
	uint32_t ClusterCount = 0;
	uint32_t OpaqueClusters = 0;
	const uint32_t* SurfaceRows = nullptr;
	size_t SurfaceCount = 0;
	uint32_t PoolClusterCapacity = 0;
	// [Karto] Per surface: a parameter record index or UINT32_MAX. Record 0 holds the frame's interpolation fraction.
	const uint32_t* SurfaceParams = nullptr;
	const float* Params = nullptr;
	size_t ParamCount = 0;
	// [Karto] A surface reference with the top bit set names a sector plane's texture matrix instead.
	const float* PlaneMatrices = nullptr;
	size_t PlaneCount = 0;
	const float* PlaneHeights = nullptr;
	// [Karto] Per vertex: plane slots of its height and texture anchor, V scale and anchor z at production; UINT32_MAX =
	// produced height.
	const uint32_t* VertexRefs = nullptr;
};

struct FWorldModeLightPatch
{
	static constexpr int MaxRanges = 5;
	struct { uint32_t Offset, Bytes; } Range[MaxRanges] = {};
	uint8_t Data[64] = {};
	int Count = 0;
};

constexpr uint32_t WorldModeLightCapacity = 1u << 20;

struct FWorldModeWrite
{
	enum ETarget { Vertices, SurfaceIDs, Indices, Rows, SurfaceParams, Params, PlaneMatrices, PlaneHeights, VertexRefs, SurfaceLights,
		WorldLights, TargetCount };
	int Target = 0;
	size_t Offset = 0;
	size_t Bytes = 0;
	const void* Data = nullptr;
};

struct FGpuParticleParams
{
	float ViewVector[2], TimeFrac, PixelStretch;
	float Yaw, Pitch, ViewPos[2];
	uint32_t Flags;			// 1 paused, 2 level frozen, 4 XY billboard, 8 face camera, 16 hw_force_cambbpref
	int32_t Style;			// gl_particles_style
	uint32_t EntryCount, Pad;	// Pad: 0 unlit, 1 lit with the entries' lights (5 words), 2 lit by the GPU light pass
	uint32_t RecordCount, Spare;	// Pad 0 and 2: range records (FGpuParticleIndices)
};

// [Karto] Pad 1 entries carry their own particle indices; otherwise a record's first index, top bit set, reads the tic's
// particle programs (re-uploaded only when ProgramVersion changes).
struct FGpuParticleIndices
{
	const uint32_t* Program = nullptr;
	uint32_t ProgramCount = 0;
	uint64_t ProgramVersion = 0;
	const uint16_t* Frame = nullptr;
	uint32_t FrameCount = 0;
};

struct FGpuLightPack
{
	const uint32_t* Words = nullptr;
	uint32_t Count = 0, Serial = 0;
	bool Shadows = false;	// the pack asks for the shadow tree's ray test
};

class FRenderState
{
protected:
	uint8_t mFogEnabled;
	uint8_t mTextureEnabled:1;
	uint8_t mGlowEnabled : 1;
	uint8_t mGradientEnabled : 1;
	uint8_t mModelMatrixEnabled : 1;
	uint8_t mTextureMatrixEnabled : 1;
	uint8_t mSplitEnabled : 1;
	uint8_t mBrightmapEnabled : 1;

	int mLightIndex;
	int mBoneIndexBase;
	int mSpecialEffect;
	int mTextureMode;
	int mTextureClamp;
	int mTextureModeFlags;
	int mSoftLight;
	float mLightParms[4];

	float mAlphaThreshold;
	float mClipSplit[2];


	int mColorMapSpecial;
	float mColorMapFlash;

	StreamData mStreamData = {};
	PalEntry mFogColor;
	const HWWorldLightingState* mDeferredWorldLighting = nullptr;
	// [Karto] Shared-appearance row index; 0 means the legacy per-surface stream.
	unsigned mAppearanceIndex = 0;

	FRenderStyle mRenderStyle;

	FMaterialState mMaterial;
	FDepthBiasState mBias;

	IVertexBuffer *mVertexBuffer;
	IVertexBuffer* mSecondaryVertexBuffer = nullptr;
	int mVertexOffsets[2];	// one per binding point
	IIndexBuffer *mIndexBuffer;

	EPassType mPassType = NORMAL_PASS;

public:

	uint64_t firstFrame = 0;
	VSMatrix mModelMatrix;
	VSMatrix mTextureMatrix;

public:

	HWWorldLightingState CaptureWorldLighting() const
	{
		if (mDeferredWorldLighting)
		{
			auto lighting = *mDeferredWorldLighting;
			if (!lighting.FogEnabled)
			{
				lighting.LightParameters.X = mLightParms[0];
				lighting.LightParameters.Y = mLightParms[1];
			}
			return lighting;
		}
		return { mStreamData.uVertexColor, mStreamData.uFogColor,
			{ mLightParms[0], mLightParms[1], mLightParms[2], mLightParms[3] },
			mFogColor, mStreamData.uDesaturationFactor, mFogEnabled };
	}

	void ApplyWorldLighting(const HWWorldLightingState& lighting)
	{
		ResolveWorldLighting();
		mStreamData.uVertexColor = lighting.VertexColor;
		mStreamData.uFogColor = lighting.FogColor;
		mStreamData.uDesaturationFactor = lighting.Desaturation;
		mFogColor = lighting.PackedFogColor;
		mFogEnabled = lighting.FogEnabled;
		// [Karto] Disabled fog leaves distance/factor untouched; replay must preserve that.
		if (lighting.FogEnabled)
		{
			mLightParms[0] = lighting.LightParameters.X;
			mLightParms[1] = lighting.LightParameters.Y;
		}
		mLightParms[2] = lighting.LightParameters.Z;
		mLightParms[3] = lighting.LightParameters.W;
	}

	void DeferWorldLighting(const HWWorldLightingState& lighting)
	{
		if (!lighting.FogEnabled && mDeferredWorldLighting && mDeferredWorldLighting->FogEnabled)
		{
			mLightParms[0] = mDeferredWorldLighting->LightParameters.X;
			mLightParms[1] = mDeferredWorldLighting->LightParameters.Y;
		}
		mDeferredWorldLighting = &lighting;
	}

	void ResolveWorldLighting()
	{
		if (!mDeferredWorldLighting) return;
		const auto* lighting = mDeferredWorldLighting;
		mDeferredWorldLighting = nullptr;
		ApplyWorldLighting(*lighting);
	}

	void CopyWorldLighting(StreamData& stream) const
	{
		if (!mDeferredWorldLighting) return;
		stream.uVertexColor = mDeferredWorldLighting->VertexColor;
		stream.uFogColor = mDeferredWorldLighting->FogColor;
		stream.uDesaturationFactor = mDeferredWorldLighting->Desaturation;
	}

	void Reset()
	{
		mDeferredWorldLighting = nullptr;
		mTextureEnabled = true;
		mBrightmapEnabled = mGradientEnabled = mFogEnabled = mGlowEnabled = false;
		mFogColor = 0xffffffff;
		mStreamData.uFogColor = mFogColor;
		mTextureMode = -1;
		mTextureClamp = 0;
		mTextureModeFlags = 0;
		mStreamData.uDesaturationFactor = 0.0f;
		mAlphaThreshold = 0.5f;
		mModelMatrixEnabled = false;
		mTextureMatrixEnabled = false;
		mSplitEnabled = false;
		mStreamData.uAddColor = 0;
		mStreamData.uObjectColor = 0xffffffff;
		mStreamData.uObjectColor2 = 0;
		mStreamData.uTextureBlendColor = 0;
		mStreamData.uTextureAddColor = 0;
		mStreamData.uTextureModulateColor = 0;
		mSoftLight = 0;
		mLightParms[0] = mLightParms[1] = mLightParms[2] = 0.0f;
		mLightParms[3] = -1.f;
		mSpecialEffect = EFF_NONE;
		mLightIndex = -1;
		mBoneIndexBase = -1;
		mStreamData.uInterpolationFactor = 0;
		mRenderStyle = DefaultRenderStyle();
		mMaterial.Reset();
		mBias.Reset();
		mPassType = NORMAL_PASS;

		mColorMapSpecial = 0;
		mColorMapFlash = 1;

		mVertexBuffer = nullptr;
		mSecondaryVertexBuffer = nullptr;
		mVertexOffsets[0] = mVertexOffsets[1] = 0;
		mIndexBuffer = nullptr;

		mStreamData.uVertexColor = { 1.0f, 1.0f, 1.0f, 1.0f };
		mStreamData.uGlowTopColor = { 0.0f, 0.0f, 0.0f, 0.0f };
		mStreamData.uGlowBottomColor = { 0.0f, 0.0f, 0.0f, 0.0f };
		mStreamData.uGlowTopPlane = { 0.0f, 0.0f, 0.0f, 0.0f };
		mStreamData.uGlowBottomPlane = { 0.0f, 0.0f, 0.0f, 0.0f };
		mStreamData.uGradientTopPlane = { 0.0f, 0.0f, 0.0f, 0.0f };
		mStreamData.uGradientBottomPlane = { 0.0f, 0.0f, 0.0f, 0.0f };
		mStreamData.uSplitTopPlane = { 0.0f, 0.0f, 0.0f, 0.0f };
		mStreamData.uSplitBottomPlane = { 0.0f, 0.0f, 0.0f, 0.0f };
		mStreamData.uDynLightColor = { 0.0f, 0.0f, 0.0f, 1.0f };
		mStreamData.uDetailParms = { 0.0f, 0.0f, 0.0f, 0.0f };
#ifdef NPOT_EMULATION
		mStreamData.uNpotEmulation = { 0,0,0,0 };
#endif
		mModelMatrix.loadIdentity();
		mTextureMatrix.loadIdentity();
		ClearClipSplit();
	}

	void SetNormal(FVector3 norm)
	{
		mStreamData.uVertexNormal = { norm.X, norm.Y, norm.Z, 0.f };
	}

	void SetNormal(float x, float y, float z)
	{
		mStreamData.uVertexNormal = { x, y, z, 0.f };
	}

	void SetAppearanceIndex(unsigned index)
	{
		mAppearanceIndex = index;
	}

	virtual bool SelectSharedAppearance(const FSharedAppearanceKey& key) { return false; }
	virtual void StoreSharedAppearance(const FSharedAppearanceKey& key) {}
	virtual void UploadCoverage(const void* data, size_t bytes) {}
	virtual void UploadStaticBake(const void* verts, size_t vertBytes, const void* indices, size_t indexBytes, const void* table, size_t tableBytes) {}
	virtual void UploadPrimitiveGrid(const void* offsets, size_t offsetBytes, const void* refs, size_t refBytes) {}
	virtual void UploadPrimitiveBounds(const void* bounds, size_t bytes) {}
	virtual void UploadGpuCullAdmission(const void* rows, size_t bytes) {}
	virtual void SetGpuCullPlanes(const float* planes24, uint32_t primCount, uint32_t expectedVisible, bool verify, bool forDraw, uint32_t commandCount = 0) {}
	virtual void UploadPrimitiveMaterials(const void* rows, size_t bytes) {}

	void SetColor(float r, float g, float b, float a = 1.f, int desat = 0)
	{
		ResolveWorldLighting();
		mStreamData.uVertexColor = { r, g, b, a };
		mStreamData.uDesaturationFactor = desat * (1.0f / 255.0f);
	}

	void SetColor(PalEntry pe, int desat = 0)
	{
		ResolveWorldLighting();
		const float scale = 1.0f / 255.0f;
		mStreamData.uVertexColor = { pe.r * scale, pe.g * scale, pe.b * scale, pe.a * scale };
		mStreamData.uDesaturationFactor = desat * (1.0f / 255.0f);
	}

	void SetColorAlpha(PalEntry pe, float alpha = 1.f, int desat = 0)
	{
		ResolveWorldLighting();
		const float scale = 1.0f / 255.0f;
		mStreamData.uVertexColor = { pe.r * scale, pe.g * scale, pe.b * scale, alpha };
		mStreamData.uDesaturationFactor = desat * (1.0f / 255.0f);
	}

	void ResetColor()
	{
		ResolveWorldLighting();
		mStreamData.uVertexColor = { 1.0f, 1.0f, 1.0f, 1.0f };
		mStreamData.uDesaturationFactor = 0.0f;
	}

	void SetTextureClamp(bool on)
	{
		if (on) mTextureClamp = TM_CLAMPY;
		else mTextureClamp = 0;
	}

	void SetTextureMode(int mode)
	{
		mTextureMode = mode;
	}

	void SetTextureMode(FRenderStyle style)
	{
		if (style.Flags & STYLEF_RedIsAlpha)
		{
			SetTextureMode(TM_ALPHATEXTURE);
		}
		else if (style.Flags & STYLEF_ColorIsFixed)
		{
			SetTextureMode(TM_STENCIL);
		}
		else if (style.Flags & STYLEF_InvertSource)
		{
			SetTextureMode(TM_INVERSE);
		}
	}

	int GetTextureMode()
	{
		return mTextureMode;
	}

	int GetTextureModeAndFlags(int tempTM)
	{
		int f = mTextureModeFlags;
		if (!mBrightmapEnabled) f &= ~(TEXF_Brightmap | TEXF_Glowmap);
		if (mTextureClamp) f |= TEXF_ClampY;
		return (mTextureMode == TM_NORMAL && tempTM == TM_OPAQUE ? TM_OPAQUE : mTextureMode) | f;
	}

	void EnableTexture(bool on)
	{
		mTextureEnabled = on;
	}

	void EnableFog(uint8_t on)
	{
		ResolveWorldLighting();
		mFogEnabled = on;
	}

	void SetEffect(int eff)
	{
		mSpecialEffect = eff;
	}

	void EnableGlow(bool on)
	{
		if (mGlowEnabled && !on)
		{
			mStreamData.uGlowTopColor = { 0.0f, 0.0f, 0.0f, 0.0f };
			mStreamData.uGlowBottomColor = { 0.0f, 0.0f, 0.0f, 0.0f };
		}
		mGlowEnabled = on;
	}

	void EnableGradient(bool on)
	{
		mGradientEnabled = on;
	}

	void EnableBrightmap(bool on)
	{
		mBrightmapEnabled = on;
	}

	void EnableSplit(bool on)
	{
		if (mSplitEnabled && !on)
		{
			mStreamData.uSplitTopPlane = { 0.0f, 0.0f, 0.0f, 0.0f };
			mStreamData.uSplitBottomPlane = { 0.0f, 0.0f, 0.0f, 0.0f };
		}
		mSplitEnabled = on;
	}

	void EnableModelMatrix(bool on)
	{
		mModelMatrixEnabled = on;
	}

	void EnableTextureMatrix(bool on)
	{
		mTextureMatrixEnabled = on;
	}

	void SetGlowParams(float *t, float *b)
	{
		mStreamData.uGlowTopColor = { t[0], t[1], t[2], t[3] };
		mStreamData.uGlowBottomColor = { b[0], b[1], b[2], b[3] };
	}

	void SetSoftLightLevel(int llevel, int blendfactor = 0)
	{
		ResolveWorldLighting();
		if (blendfactor == 0) mLightParms[3] = llevel / 255.f;
		else mLightParms[3] = -1.f;
	}

	void SetNoSoftLightLevel()
	{
		ResolveWorldLighting();
		 mLightParms[3] = -1.f;
	}

	void SetGlowPlanes(const FVector4 &tp, const FVector4& bp)
	{
		mStreamData.uGlowTopPlane = tp;
		mStreamData.uGlowBottomPlane = bp;
	}

	void SetGradientPlanes(const FVector4& tp, const FVector4& bp)
	{
		mStreamData.uGradientTopPlane = tp;
		mStreamData.uGradientBottomPlane = bp;
	}

	void SetSplitPlanes(const FVector4& tp, const FVector4& bp)
	{
		mStreamData.uSplitTopPlane = tp;
		mStreamData.uSplitBottomPlane = bp;
	}

	void SetDetailParms(float xscale, float yscale, float bias)
	{
		mStreamData.uDetailParms = { xscale, yscale, bias, 0 };
	}

	void SetDynLight(float r, float g, float b)
	{
		mStreamData.uDynLightColor.X = r;
		mStreamData.uDynLightColor.Y = g;
		mStreamData.uDynLightColor.Z = b;
	}

	void SetScreenFade(float f)
	{
		// This component is otherwise unused.
		mStreamData.uDynLightColor.W = f;
	}

	void SetObjectColor(PalEntry pe)
	{
		mStreamData.uObjectColor = pe;
	}

	void SetObjectColor2(PalEntry pe)
	{
		mStreamData.uObjectColor2 = pe;
	}

	void SetAddColor(PalEntry pe)
	{
		mStreamData.uAddColor = pe;
	}

	void SetNpotEmulation(float factor, float offset)
	{
#ifdef NPOT_EMULATION
		mStreamData.uNpotEmulation = { offset, factor, 0, 0 };
#endif
	}

	void ApplyTextureManipulation(TextureManipulation* texfx)
	{
		if (!texfx || texfx->AddColor.a == 0)
		{
			mStreamData.uTextureAddColor.a = 0;	// we only need to set the flags to 0
		}
		else
		{
			// set up the whole thing
			mStreamData.uTextureAddColor.SetIA(texfx->AddColor);
			auto pe = texfx->ModulateColor;
			mStreamData.uTextureModulateColor.SetFlt(pe.r * pe.a / 255.f, pe.g * pe.a / 255.f, pe.b * pe.a / 255.f, texfx->DesaturationFactor);
			mStreamData.uTextureBlendColor = texfx->BlendColor;
		}
	}

	bool CanBeginPreparedWallRun() const
	{
		// [Karto] A run may omit resets only when their exact post-state is already present.
		static const FVector4PalEntry white = { 1.f, 1.f, 1.f, 1.f };
		static const FVector4PalEntry zero = { 0.f, 0.f, 0.f, 0.f };
		static const float disabled = 0.f;
		if (!mTextureEnabled || mSpecialEffect != EFF_NONE || mGlowEnabled || mGradientEnabled || mSplitEnabled || mTextureClamp ||
			memcmp(&mStreamData.uObjectColor, &white, sizeof(white)) ||
			memcmp(&mStreamData.uObjectColor2, &zero, sizeof(zero)) ||
			memcmp(&mStreamData.uAddColor, &zero, sizeof(zero)) ||
			memcmp(&mStreamData.uTextureAddColor.a, &disabled, sizeof(disabled))) return false;
#ifdef NPOT_EMULATION
		static const FVector4 npot = { 0.f, 0.f, 0.f, 0.f };
		if (memcmp(&mStreamData.uNpotEmulation, &npot, sizeof(npot))) return false;
#endif
		return true;
	}

	bool HasPreparedWallMaterial() const
	{
		return mMaterial.mMaterial && mMaterial.mOverrideShader <= 0 &&
			mMaterial.mMaterial->GetShaderIndex() == 0 && !mMaterial.mMaterial->Source()->isHardwareCanvas();
	}
	void SetTextureColors(float* modColor, float* addColor, float* blendColor)
	{
		mStreamData.uTextureAddColor.SetFlt(addColor[0], addColor[1], addColor[2], addColor[3]);
		mStreamData.uTextureModulateColor.SetFlt(modColor[0], modColor[1], modColor[2], modColor[3]);
		mStreamData.uTextureBlendColor.SetFlt(blendColor[0], blendColor[1], blendColor[2], blendColor[3]);
	}

	void SetFog(PalEntry c, float d)
	{
		ResolveWorldLighting();
		const float LOG2E = 1.442692f;	// = 1/log(2)
		mFogColor = c;
		mStreamData.uFogColor = mFogColor;
		if (d >= 0.0f) mLightParms[2] = d * (-LOG2E / 64000.f);
	}

	void SetLightParms(float f, float d)
	{
		ResolveWorldLighting();
		mLightParms[1] = f;
		mLightParms[0] = d;
	}

	PalEntry GetFogColor() const
	{
		return mDeferredWorldLighting ? mDeferredWorldLighting->PackedFogColor : mFogColor;
	}

	void AlphaFunc(int func, float thresh)
	{
		if (func == Alpha_Greater) mAlphaThreshold = thresh;
		else mAlphaThreshold = thresh - 0.001f;
	}

	void SetLightIndex(int index)
	{
		mLightIndex = index;
	}

	void SetBoneIndexBase(int index)
	{
		mBoneIndexBase = index;
	}

	void SetRenderStyle(FRenderStyle rs)
	{
		mRenderStyle = rs;
	}

	void SetRenderStyle(ERenderStyle rs)
	{
		mRenderStyle = rs;
	}

	auto GetDepthBias()
	{
		return mBias;
	}

	void SetDepthBias(float a, float b)
	{
		mBias.mChanged |= mBias.mFactor != a || mBias.mUnits != b;
		mBias.mFactor = a;
		mBias.mUnits = b;
	}

	void SetDepthBias(FDepthBiasState& bias)
	{
		SetDepthBias(bias.mFactor, bias.mUnits);
	}

	void ClearDepthBias()
	{
		mBias.mChanged |= mBias.mFactor != 0 || mBias.mUnits != 0;
		mBias.mFactor = 0;
		mBias.mUnits = 0;
	}

private:
	void SetMaterial(FMaterial *mat, int clampmode, int translation, int overrideshader, PClass * cls)
	{
		mAppearanceIndex = 0;
		mMaterial.mMaterial = mat;
		mMaterial.mClampMode = clampmode;
		mMaterial.mTranslation = translation;
		mMaterial.mOverrideShader = overrideshader;
		mMaterial.mChanged = true;
		mTextureModeFlags = mat->GetLayerFlags();
		auto scale = mat->GetDetailScale();
		mStreamData.uDetailParms = { scale.X, scale.Y, 2, 0 };
	}

public:
	void SetMaterial(FGameTexture* tex, EUpscaleFlags upscalemask, int scaleflags, int clampmode, int translation, int overrideshader, PClass *cls)
	{
		tex->setSeen();
		if (!sysCallbacks.PreBindTexture || !sysCallbacks.PreBindTexture(this, tex, upscalemask, scaleflags, clampmode, translation, overrideshader))
		{
			if (shouldUpscale(tex, upscalemask)) scaleflags |= CTF_Upscale;
		}
		auto mat = FMaterial::ValidateTexture(tex, scaleflags);
		assert(mat);
		SetMaterial(mat, clampmode, translation, overrideshader, cls);
	}

	void SetMaterial(FGameTexture* tex, EUpscaleFlags upscalemask, int scaleflags, int clampmode, FTranslationID translation, int overrideshader, PClass *cls)
	{
		SetMaterial(tex, upscalemask, scaleflags, clampmode, translation.index(), overrideshader, cls);
	}


	void SetClipSplit(float bottom, float top)
	{
		mClipSplit[0] = bottom;
		mClipSplit[1] = top;
	}

	void SetClipSplit(float *vals)
	{
		memcpy(mClipSplit, vals, 2 * sizeof(float));
	}

	void GetClipSplit(float *out)
	{
		memcpy(out, mClipSplit, 2 * sizeof(float));
	}

	void ClearClipSplit()
	{
		mClipSplit[0] = -1000000.f;
		mClipSplit[1] = 1000000.f;
	}

	void SetVertexBuffer(IVertexBuffer *vb, int offset0, int offset1, IVertexBuffer* secondary = nullptr)
	{
		assert(vb);
		mVertexBuffer = vb;
		mSecondaryVertexBuffer = secondary;
		mVertexOffsets[0] = offset0;
		mVertexOffsets[1] = offset1;
	}

	void SetIndexBuffer(IIndexBuffer *ib)
	{
		mIndexBuffer = ib;
	}

	template <class T> void SetVertexBuffer(T *buffer)
	{
		auto ptrs = buffer->GetBufferObjects();
		SetVertexBuffer(ptrs.first, 0, 0);
		SetIndexBuffer(ptrs.second);
	}

	void SetInterpolationFactor(float fac)
	{
		mStreamData.uInterpolationFactor = fac;
	}

	float GetInterpolationFactor()
	{
		return mStreamData.uInterpolationFactor;
	}

	void EnableDrawBufferAttachments(bool on) // Used by fog boundary drawer
	{
		EnableDrawBuffers(on ? GetPassDrawBufferCount() : 1);
	}

	int GetPassDrawBufferCount()
	{
		return mPassType == GBUFFER_PASS ? 3 : 1;
	}

	void SetPassType(EPassType passType)
	{
		mPassType = passType;
	}

	EPassType GetPassType()
	{
		return mPassType;
	}

	void SetSpecialColormap(int cm, float flash)
	{
		mColorMapSpecial = cm;
		mColorMapFlash = flash;
	}

	// API-dependent render interface

	// Draw commands
	virtual void ClearScreen() = 0;
	virtual void Draw(int dt, int index, int count, bool apply = true) = 0;
	virtual void DrawOpaqueQuad(int index) { Draw(DT_TriangleFan, index, 4); }
	virtual void DrawBatchedQuad(int index) { Draw(DT_TriangleFan, index, 4); }
	// [Karto] Caller guarantees no state was set since its last DrawBatchedQuad.
	virtual void DrawBatchedQuadSameState(int index) { DrawBatchedQuad(index); }
	struct FIndirectFan
	{
		unsigned VertexIndex;
		unsigned VertexCount;
		int LightIndex;
	};
	virtual void DrawIndirectFans(const FIndirectFan* fans, int count)
	{
		for (int i = 0; i < count; ++i)
		{
			SetLightIndex(fans[i].LightIndex);
			Draw(DT_TriangleFan, fans[i].VertexIndex, fans[i].VertexCount);
		}
	}
	virtual void DrawRetainedQuad(int index) { Draw(DT_TriangleFan, index, 4); }
	virtual void PrepareRetainedWallRun(bool changed, bool owned = false) {}
	virtual bool SetRetainedWorldOwnership(bool enabled) { return false; }
	virtual void DrawOwnedRetainedQuad(int index, uint64_t version) { DrawRetainedQuad(index); }
	virtual void EndRetainedWallRun() {}
	virtual void DrawRetainedTriangles(int index, int count, unsigned surface) { DrawIndexed(DT_Triangles, index, count); }
	virtual bool DrawBakedPrimitive(uint32_t iboOffset, uint32_t indexCount) { return false; }
	virtual bool BeginWorldCapture(FWorldCaptureSink* sink) { return false; }
	virtual void EndWorldCapture() {}
	virtual bool UploadWorldMode(const FWorldModeUpload& data) { return false; }
	virtual void ReleaseWorldMode() {}
	virtual bool SetWorldModeVisibility(const uint8_t* visible, uint32_t count) { return false; }
	virtual bool SetWorldModePoolClusters(const uint32_t* firstIndex, const uint32_t* indexCount, uint32_t opaque, uint32_t total) { return false; }
	virtual bool WriteWorldMode(const FWorldModeWrite* writes, size_t count) { return false; }
	virtual bool GetWorldModeTextureIDs(const FMaterialState& state, uint32_t ids[4]) { return false; }
	bool GetCurrentMaterialGpuIDs(uint32_t ids[4]) { return GetWorldModeTextureIDs(mMaterial, ids); }
	virtual uint32_t AddSpriteRow() { return ~0u; }
	virtual uint64_t GetGpuTextureEpoch() { return 0; }
	virtual bool SupportsSpriteInstances() { return false; }
	virtual bool ComputeSpriteLights(const uint32_t* instances, uint32_t instanceCount, uint32_t instanceWords, uint64_t instanceSerial,
		const FGpuLightPack& pack) { return false; }
	virtual bool DrawSpriteInstances(const uint32_t* instances, uint32_t instanceCount, uint32_t instanceWords, uint64_t instanceSerial,
		const uint32_t* visible, uint32_t count, const uint32_t* frames, uint32_t frameWords, uint64_t frameVersion, const float* params,
		uint32_t paramVec4s) { return false; }
	// [Karto] The compute runs before this frame's draws; false leaves target unusable for this frame.
	virtual bool SupportsParticleExpansion() { return false; }
	virtual bool ExpandParticles(IVertexBuffer* target, const float* particles, uint32_t particleCount, uint64_t serial,
		const uint32_t* entries, const float* ranges, uint32_t rangeCount, const FGpuParticleParams& params, const FGpuParticleIndices& indices,
		const FGpuLightPack* lights) { return false; }
	virtual void DrawWorldMode(bool alphaTest) {}
	virtual bool DrawWorldModeVisibility() { return false; }
	virtual bool GetWorldModeVisibility(const uint32_t*& stamps, size_t& count, uint32_t& stamp) { return false; }
	virtual uint32_t PeekWorldModeStamp() { return 0; }
	virtual bool GetWorldModeLightPatch(FWorldModeLightPatch& patch) { return false; }
	// [Karto] Texture teardown invalidates baked bindless IDs; the bake must then be redone.
	virtual bool IsWorldModeStale() { return false; }
	virtual bool BeginGpuDrivenWalls() { return false; }
	virtual void CaptureGpuStaticMaterial(FGpuStaticMaterial& out) {}
	virtual bool DrawGpuDrivenWall(uint32_t slot, uint32_t bakeRow = UINT32_MAX) { return false; }
	virtual bool HasGpuResidentAppearance(uint32_t id, uint64_t revision) const { return false; }
	virtual void SetGpuResidentCapture(uint32_t id, uint64_t revision) {}
	virtual bool DrawGpuResidentWall(uint32_t slot, uint32_t id, uint64_t revision) { return false; }
	virtual void EndGpuResidentBatch() {}
	virtual uint64_t GpuResidentDrawCount() const { return 0; }
	virtual void EndGpuDrivenWalls() {}
	virtual bool DrawGpuDrivenGeometry(uint32_t drawCount) { return false; }
	virtual uint32_t GpuDrivenGeometryCount() const { return 0; }
	virtual void DrawIndexed(int dt, int index, int count, bool apply = true) = 0;
	virtual void DrawModelIndexed(int index, int count) { DrawIndexed(DT_Triangles, index, count); }
	virtual void BeginGPUProfile(const char* name) {}
	virtual void EndGPUProfile() {}

	// Immediate render state change commands. These only change infrequently and should not clutter the render state.
	virtual bool SetDepthClamp(bool on) = 0;					// Deactivated only by skyboxes.
	virtual void SetDepthMask(bool on) = 0;						// Used by decals and indirectly by portal setup.
	virtual void SetDepthFunc(int func) = 0;					// Used by models, portals and mirror surfaces.
	virtual void SetDepthRange(float min, float max) = 0;		// Used by portal setup.
	virtual void SetColorMask(bool r, bool g, bool b, bool a) = 0;	// Used by portals.
	virtual void SetStencil(int offs, int op, int flags=-1) = 0;	// Used by portal setup and render hacks.
	virtual void SetCulling(int mode) = 0;						// Used by model drawer only.
	virtual void EnableClipDistance(int num, bool state) = 0;	// Use by sprite sorter for vertical splits.
	virtual void Clear(int targets) = 0;						// not used during normal rendering
	virtual void EnableStencil(bool on) = 0;					// always on for 3D, always off for 2D
	virtual void SetScissor(int x, int y, int w, int h) = 0;	// constant for 3D, changes for 2D
	virtual void SetViewport(int x, int y, int w, int h) = 0;	// constant for all 3D and all 2D
	virtual void EnableDepthTest(bool on) = 0;					// used by 2D, portals and render hacks.
	virtual void EnableMultisampling(bool on) = 0;				// only active for 2D
	virtual void EnableLineSmooth(bool on) = 0;					// constant setting for each 2D drawer operation
	virtual void EnableDrawBuffers(int count, bool apply = false) = 0;	// Used by SSAO and EnableDrawBufferAttachments

	void SetColorMask(bool on)
	{
		SetColorMask(on, on, on, on);
	}

};
